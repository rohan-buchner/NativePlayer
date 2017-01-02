/*!
 * url_player_controller.cc (https://github.com/SamsungDForum/NativePlayer)
 * Copyright 2016, Samsung Electronics Co., Ltd
 * Licensed under the MIT license
 *
 * @author Tomasz Borkowski
 * @author Jacob Tarasiewicz
 */

#include "player/es_dash_player/stream_manager.h"

#include <stdlib.h>
#include <functional>
#include <memory>

#include "ppapi/utility/threading/lock.h"

#include "common.h"
#include "demuxer/elementary_stream_packet.h"
#include "demuxer/stream_demuxer.h"

#include "player/es_dash_player/es_dash_player_controller.h"
#include "player/es_dash_player/stream_listener.h"

#include "async_data_provider.h"
#include "media_segment.h"

using pp::AutoLock;
using Samsung::NaClPlayer::AudioElementaryStream;
using Samsung::NaClPlayer::DRMType;
using Samsung::NaClPlayer::DRMType_Unknown;
using Samsung::NaClPlayer::DRMType_Playready;
using Samsung::NaClPlayer::ElementaryStream;
using Samsung::NaClPlayer::ElementaryStreamListener;
using Samsung::NaClPlayer::ErrorCodes;
using Samsung::NaClPlayer::ESDataSource;
using Samsung::NaClPlayer::ESPacket;
using Samsung::NaClPlayer::MediaPlayer;
using Samsung::NaClPlayer::TimeTicks;
using Samsung::NaClPlayer::VideoElementaryStream;

using std::placeholders::_1;
using std::placeholders::_2;
using std::make_shared;
using std::shared_ptr;
using std::unique_ptr;
using std::vector;

const TimeTicks kNextSegmentTimeThreshold = 7.0f;    // in seconds

class StreamManager::Impl :
    public std::enable_shared_from_this<StreamManager::Impl> {
 public:
  explicit Impl(pp::InstanceHandle instance, StreamType type);
  ~Impl();
  bool Initialize(
       std::unique_ptr<MediaSegmentSequence> segment_sequence,
       std::shared_ptr<Samsung::NaClPlayer::ESDataSource> es_data_source,
       std::function<void(StreamType)> stream_configured_callback,
       std::function<void(StreamDemuxer::Message,
                          unique_ptr<ElementaryStreamPacket>)>
                              es_packet_callback,
       StreamListener* stream_listener,
       Samsung::NaClPlayer::DRMType drm_type,
       std::shared_ptr<ElementaryStreamListener> listener);

  void SetMediaSegmentSequence(
       std::unique_ptr<MediaSegmentSequence> segment_sequence);

  bool UpdateBuffer(Samsung::NaClPlayer::TimeTicks playback_time);

  bool IsInitialized() { return initialized_; }

  bool IsSeeking() const { return seeking_; }

  void OnNeedData(int32_t bytes_max);
  void OnEnoughData();
  void OnSeekData(Samsung::NaClPlayer::TimeTicks new_position);

  void PrepareForSeek(Samsung::NaClPlayer::TimeTicks new_position);

  void SetSegmentToTime(Samsung::NaClPlayer::TimeTicks time,
      Samsung::NaClPlayer::TimeTicks* timestamp,
      Samsung::NaClPlayer::TimeTicks* duration);

  bool AppendPacket(std::unique_ptr<ElementaryStreamPacket>);

  bool SetConfig(const AudioConfig& audio_config);
  bool SetConfig(const VideoConfig& video_config);

  Samsung::NaClPlayer::TimeTicks GetClosestKeyframeTime(
      Samsung::NaClPlayer::TimeTicks);

 private:
  bool InitParser();
  bool ParseInitSegment();
  void GotSegment(std::unique_ptr<MediaSegment> segment);

  void OnAudioConfig(const AudioConfig& audio_config);
  void OnVideoConfig(const VideoConfig& video_config);
  void OnDRMInitData(const std::string& type,
                     const std::vector<uint8_t>& init_data);

  pp::InstanceHandle instance_handle_;
  StreamType stream_type_;

  std::unique_ptr<StreamDemuxer> demuxer_;
  std::unique_ptr<AsyncDataProvider> data_provider_;

  pp::CompletionCallbackFactory<Impl> callback_factory_;

  std::shared_ptr<Samsung::NaClPlayer::ElementaryStream> elementary_stream_;
  std::function<void(StreamType)> stream_configured_callback_;
  std::function<void(StreamDemuxer::Message,
                     unique_ptr<ElementaryStreamPacket>)>
                         es_packet_callback_;

  StreamListener* stream_listener_;

  bool drm_initialized_;
  bool exited_;
  bool init_seek_;
  bool initialized_;
  bool seeking_;
  bool changing_representation_;
  bool segment_pending_;

  AudioConfig audio_config_;
  VideoConfig video_config_;
  Samsung::NaClPlayer::DRMType drm_type_;

  Samsung::NaClPlayer::TimeTicks buffered_segments_time_;
  Samsung::NaClPlayer::TimeTicks need_time_;
};  // class StreamManager::Impl

StreamManager::Impl::Impl(pp::InstanceHandle instance, StreamType type)
    : instance_handle_(instance),
      stream_type_(type),
      data_provider_(),
      callback_factory_(this),
      stream_listener_(nullptr),
      drm_initialized_(false),
      exited_(false),
      init_seek_(false),
      initialized_(false),
      seeking_(false),
      changing_representation_(false),
      segment_pending_(false),
      drm_type_(Samsung::NaClPlayer::DRMType_Unknown),
      buffered_segments_time_(0.),
      need_time_(0.) {}

StreamManager::Impl::~Impl() {
  LOG_DEBUG("");
  exited_ = true;
}

void StreamManager::Impl::OnNeedData(int32_t bytes_max) {
  LOG_DEBUG("Type: %s size: %d",
            stream_type_ == StreamType::Video ? "VIDEO" : "AUDIO", bytes_max);
}

void StreamManager::Impl::OnEnoughData() {
  LOG_DEBUG("Type: %s", stream_type_ == StreamType::Video ? "VIDEO" : "AUDIO");
}

void StreamManager::Impl::OnSeekData(TimeTicks new_position) {
  LOG_INFO("Type: %s, new_position: %f",
      stream_type_ == StreamType::Video ? "VIDEO" : "AUDIO", new_position);
  if (!init_seek_) {
    init_seek_ = true;
    return;
  }
  if (InitParser()) {
    ParseInitSegment();
    stream_listener_->OnSeekData(stream_type_, new_position);
  }
}

void StreamManager::Impl::PrepareForSeek(
    Samsung::NaClPlayer::TimeTicks new_position) {
  buffered_segments_time_ = 0.0;
  seeking_ = true;
  drm_initialized_ = false;
  demuxer_.reset();
}

void StreamManager::Impl::SetSegmentToTime(Samsung::NaClPlayer::TimeTicks time,
      Samsung::NaClPlayer::TimeTicks* timestamp,
      Samsung::NaClPlayer::TimeTicks* duration) {
  need_time_ = time;
  if (need_time_ < 0.0) need_time_ = 0.0;
  data_provider_->SetNextSegmentToTime(time);
  if (timestamp)
    *timestamp = data_provider_->CurrentSegmentTimestamp();
  if (duration)
    *duration =  data_provider_->CurrentSegmentDuration();
}

bool StreamManager::Impl::AppendPacket(
    std::unique_ptr<ElementaryStreamPacket> packet) {
  int32_t ret = ErrorCodes::Success;
  const char* fname = "";
  if (!packet->IsEncrypted()) {
    fname = "AppendPacket";
    ret = elementary_stream_->AppendPacket(packet->GetESPacket());
  } else {
    fname = "AppendEncryptedPacket";
    ret = elementary_stream_->AppendEncryptedPacket(
        packet->GetESPacket(), packet->GetEncryptionInfo());
  }
  LOG_DEBUG("stream: %s , %p, %s ret: %d, packet pts: %f",
            stream_type_ == StreamType::Video ? "VIDEO" : "AUDIO", this,
            fname, ret, packet->GetPts());
  if (ret != ErrorCodes::Success) {
    LOG_ERROR("Failed to AppendPacket! Error code: %d", ret);
    return false;
  }
  return true;
}

bool StreamManager::Impl::Initialize(
    unique_ptr<MediaSegmentSequence> segment_sequence,
    shared_ptr<ESDataSource> es_data_source,
    std::function<void(StreamType)> stream_configured_callback,
    std::function<void(StreamDemuxer::Message,
                       unique_ptr<ElementaryStreamPacket>)>
                           es_packet_callback,
    StreamListener* stream_listener,
    DRMType drm_type,
    std::shared_ptr<ElementaryStreamListener> listener) {
  LOG_DEBUG("");
  if (!stream_configured_callback) {
    LOG_ERROR(" callback is null!");
    return false;
  }
  stream_configured_callback_ = stream_configured_callback;
  es_packet_callback_ = es_packet_callback;
  stream_listener_ = stream_listener;
  drm_type_ = drm_type;
  auto callback = WeakBind(&StreamManager::Impl::GotSegment,
      shared_from_this(), _1);
  data_provider_ = MakeUnique<AsyncDataProvider>(
      instance_handle_, callback);
  data_provider_->SetMediaSegmentSequence(std::move(segment_sequence));

  int32_t result = ErrorCodes::BadArgument;
  // Add a stream to ESDataSource
  if (stream_type_ == StreamType::Video) {
    auto video_stream = make_shared<VideoElementaryStream>();
    result = es_data_source->AddStream(*video_stream, listener);
    elementary_stream_ =
        std::static_pointer_cast<ElementaryStream>(video_stream);
  } else if (stream_type_ == StreamType::Audio) {
    auto audio_stream = make_shared<AudioElementaryStream>();
    result = es_data_source->AddStream(*audio_stream, listener);
    elementary_stream_ =
        std::static_pointer_cast<ElementaryStream>(audio_stream);
  }

  if (result != ErrorCodes::Success) {
    LOG_ERROR("Failed to AddStream, type: %d, result: %d", stream_type_,
              result);
    return false;
  }

  if (!elementary_stream_) {
    return false;
  }

  // Initialize stream parser
  if (!InitParser()) {
    LOG_ERROR("Failed to initialize parser or config listeners");
    return false;
  }

  return ParseInitSegment();
}

bool StreamManager::Impl::InitParser() {
  StreamDemuxer::Type demuxer_type;
  switch (stream_type_) {
    case StreamType::Video:
      demuxer_type = StreamDemuxer::kVideo;
      break;
    case StreamType::Audio:
      demuxer_type = StreamDemuxer::kAudio;
      break;
    default:
      demuxer_type = StreamDemuxer::kUnknown;
  }

  demuxer_ = StreamDemuxer::Create(instance_handle_, demuxer_type);

  if (!demuxer_) {
    LOG_ERROR("Failed to construct a FFMpegStreamParser");
  }

  if (!demuxer_->Init(es_packet_callback_, pp::MessageLoop::GetCurrent()))
    return false;

  bool ok = demuxer_->SetAudioConfigListener(
                WeakBind(&StreamManager::Impl::OnAudioConfig,
                          shared_from_this(), _1));

  ok = ok &&
       demuxer_->SetVideoConfigListener(
           WeakBind(&StreamManager::Impl::OnVideoConfig,
                     shared_from_this(), _1));

  if (drm_type_ != DRMType_Unknown) {
    ok = ok &&
         demuxer_->SetDRMInitDataListener(
             WeakBind(&StreamManager::Impl::OnDRMInitData,
                       shared_from_this(), _1, _2));
  }
  return ok;
}

bool StreamManager::Impl::ParseInitSegment() {
  // Download initialization segment.
  vector<uint8_t> init_segment;
  if (!data_provider_->GetInitSegment(&init_segment)) {
    LOG_ERROR("Failed to download initialization segment!");
    return false;
  }

  if (init_segment.empty()) {
    LOG_ERROR("Initialization segment is empty!");
    return false;
  }

  demuxer_->Parse(init_segment);
  return true;
}

Samsung::NaClPlayer::TimeTicks StreamManager::Impl::GetClosestKeyframeTime(
    Samsung::NaClPlayer::TimeTicks time) {
  return data_provider_->GetClosestKeyframeTime(time);
}

bool StreamManager::Impl::UpdateBuffer(TimeTicks playback_time) {
  LOG_DEBUG("stream manager: %p, playback_time: %f, buffered time: %f", this,
            playback_time, buffered_segments_time_);

  if (!elementary_stream_) {
    LOG_DEBUG("elementary stream is not initialized!");
    return true;
  }

  // Check if we need to request next segment download.
  if (!segment_pending_) {
    auto next_segment_threshold = data_provider_->AverageSegmentDuration();
    if (next_segment_threshold <= kEps)
        next_segment_threshold = kNextSegmentTimeThreshold;
    if (buffered_segments_time_ - playback_time < next_segment_threshold) {
      LOG_DEBUG("Requesting next %s segment...",
                stream_type_ == StreamType::Video ? "VIDEO" : "AUDIO");
      bool has_more_segments = data_provider_->RequestNextDataSegment();
      if (has_more_segments) {
        segment_pending_ = true;
      } else {
        LOG_DEBUG("There are no more segments to load");
        return false;
      }
    }
  }

  return true;
}

void StreamManager::Impl::SetMediaSegmentSequence(
    std::unique_ptr<MediaSegmentSequence> segment_sequence) {
  LOG_INFO("Setting new %s sequence to %f [s]",
            stream_type_ == StreamType::Video ? "VIDEO" : "AUDIO",
            buffered_segments_time_);
  // TODO(p.balut): This is needed only for adjusting a demuxer timestamp in
  //                GotSegment() and will be redundant after update to ffmpeg
  //                2.6+.
  changing_representation_ = true;
  // TODO(p.balut): Assuring we request next segment should be more reliable
  //                than just using timestamps (i.e. use segment indices).
  need_time_ = buffered_segments_time_ + kSegmentMargin;
  demuxer_.reset();
  drm_initialized_ = false;
  LOG_INFO("Parser reset");
  data_provider_->SetMediaSegmentSequence(std::move(segment_sequence),
      buffered_segments_time_ + kSegmentMargin);
  if (InitParser()) ParseInitSegment();

  LOG_DEBUG("SetMediaSegmentSequence changed segments in data provider");
}

void StreamManager::Impl::GotSegment(std::unique_ptr<MediaSegment> segment) {
  if (!segment->data_.empty()) {
    LOG_DEBUG("Got %s segment. duration: %f, data size: %d, timestamp: %f [s]",
        stream_type_ == StreamType::Video ? "VIDEO" : "AUDIO",
        segment->duration_, segment->data_.size(), segment->timestamp_);
  }
  segment_pending_ = false;
  if ((seeking_ || changing_representation_) &&
      segment->timestamp_ - kEps <= need_time_ &&
      need_time_ < segment->duration_ + segment->timestamp_) {
    LOG_INFO("This segment finishes a seek for this stream.");
    changing_representation_ = false;
    seeking_ = false;
    demuxer_->SetTimestamp(segment->timestamp_);
  } else if (seeking_) {
    LOG_INFO("This segment is out of bounds and will be dropped. Expected "
             "time == %f [s]", need_time_);
    return;
  }

  buffered_segments_time_ =
      static_cast<TimeTicks>(segment->duration_ + segment->timestamp_);
  demuxer_->Parse(segment->data_);
}

bool StreamManager::Impl::SetConfig(const AudioConfig& audio_config) {
  LOG_INFO("OnAudioConfig codec_type: %d!\n"
      "profile: %d, sample_format: %d,"
      " bits_per_channel: %d, channel_layout: %d, samples_per_second: %d",
      audio_config.codec_type, audio_config.codec_profile,
      audio_config.sample_format, audio_config.bits_per_channel,
      audio_config.channel_layout, audio_config.samples_per_second);

  if (audio_config_ == audio_config) {
    LOG_INFO("The same config as before");
    return true;
  }

  audio_config_ = audio_config;
  if (stream_type_ == StreamType::Audio) {
    auto audio_stream =
        std::static_pointer_cast<AudioElementaryStream>(elementary_stream_);
    audio_stream->SetAudioCodecType(audio_config.codec_type);
    audio_stream->SetAudioCodecProfile(audio_config.codec_profile);
    audio_stream->SetSampleFormat(audio_config.sample_format);
    audio_stream->SetChannelLayout(audio_config.channel_layout);
    audio_stream->SetBitsPerChannel(audio_config.bits_per_channel);
    audio_stream->SetSamplesPerSecond(audio_config.samples_per_second);
    audio_stream->SetCodecExtraData(audio_config.extra_data.size(),
                                    &audio_config.extra_data.front());

    int32_t ret = audio_stream->InitializeDone();
    LOG_DEBUG("audio - InitializeDone: %d", ret);

    if (ret == ErrorCodes::Success && !initialized_) {
      initialized_ = true;
      stream_configured_callback_(stream_type_);
      return true;
    }
  } else {
    LOG_ERROR("This is not an audio stream manager!");
  }
  return false;
}

bool StreamManager::Impl::SetConfig(const VideoConfig& video_config) {
  LOG_INFO("OnVideoConfig codec_type: %d!\n"
      "video configuration - codec: %d, profile: %d, frame: %d "
      "visible_rect: %d %d ",
      video_config.codec_type, video_config.codec_profile,
      video_config.frame_format, video_config.size.width,
      video_config.size.height);
  if (video_config_ == video_config) {
    LOG_INFO("The same config as before");
    return true;
  }

  video_config_ = video_config;
  if (stream_type_ == StreamType::Video) {
    auto video_stream =
        std::static_pointer_cast<VideoElementaryStream>(elementary_stream_);
    video_stream->SetVideoCodecType(video_config.codec_type);
    video_stream->SetVideoCodecProfile(video_config.codec_profile);
    video_stream->SetVideoFrameFormat(video_config.frame_format);
    video_stream->SetVideoFrameSize(video_config.size);
    video_stream->SetFrameRate(video_config.frame_rate);
    video_stream->SetCodecExtraData(video_config.extra_data.size(),
                                    &video_config.extra_data.front());

    int32_t ret = video_stream->InitializeDone();
    LOG_DEBUG("video - InitializeDone: %d", ret);

    if (ret == ErrorCodes::Success && !initialized_) {
      initialized_ = true;
      stream_configured_callback_(stream_type_);
      return true;
    }
  } else {
    LOG_ERROR("This is not a video stream manager!");
  }
  return false;
}

void StreamManager::Impl::OnAudioConfig(const AudioConfig& audio_config) {
  stream_listener_->OnStreamConfig(audio_config);
}

void StreamManager::Impl::OnVideoConfig(const VideoConfig& video_config) {
  stream_listener_->OnStreamConfig(video_config);
}

void StreamManager::Impl::OnDRMInitData(const std::string& type,
                                  const std::vector<uint8_t>& init_data) {
  LOG_DEBUG("stream type: %d, init data type: %s, init_data.size(): %d",
            stream_type_, type.c_str(), init_data.size());
  if (drm_initialized_) {
    LOG_INFO("DRM initialized already");
    return;
  }
  LOG_DEBUG("init_data hex str: [[%s]]",
            ToHexString(init_data.size(), init_data.data()).c_str());

  int32_t ret = elementary_stream_->SetDRMInitData(
      type, init_data.size(), static_cast<const void*>(init_data.data()));
  if (ret == ErrorCodes::Success) drm_initialized_ = true;
  LOG_DEBUG("SetDRMInitData returned: %d", ret);
}

// end of PIMPL implementation

StreamManager::StreamManager(pp::InstanceHandle instance, StreamType type)
  :pimpl_(MakeUnique<Impl>(instance, type)) {
}
StreamManager::~StreamManager() {
}
void StreamManager::OnNeedData(int32_t bytes_max) {
  pimpl_->OnNeedData(bytes_max);
}
void StreamManager::OnEnoughData() {
  pimpl_->OnEnoughData();
}
void StreamManager::OnSeekData(TimeTicks new_position) {
  pimpl_->OnSeekData(new_position);
}
bool StreamManager::IsInitialized() {
  return pimpl_->IsInitialized();
}
bool StreamManager::IsSeeking() const {
  return pimpl_->IsSeeking();
}
void StreamManager::PrepareForSeek(
    Samsung::NaClPlayer::TimeTicks new_position) {
  pimpl_->PrepareForSeek(new_position);
}
void StreamManager::SetSegmentToTime(Samsung::NaClPlayer::TimeTicks time,
    Samsung::NaClPlayer::TimeTicks* timestamp,
    Samsung::NaClPlayer::TimeTicks* duration) {
  pimpl_->SetSegmentToTime(time, timestamp, duration);
}
bool StreamManager::AppendPacket(
    std::unique_ptr<ElementaryStreamPacket> packet) {
  return pimpl_->AppendPacket(std::move(packet));
}
bool StreamManager::SetConfig(const AudioConfig& audio_config) {
  return pimpl_->SetConfig(audio_config);
}
bool StreamManager::SetConfig(const VideoConfig& video_config) {
  return pimpl_->SetConfig(video_config);
}
Samsung::NaClPlayer::TimeTicks StreamManager::GetClosestKeyframeTime(
    Samsung::NaClPlayer::TimeTicks time) {
  return pimpl_->GetClosestKeyframeTime(time);
}
bool StreamManager::Initialize(
    unique_ptr<MediaSegmentSequence> segment_sequence,
    shared_ptr<ESDataSource> es_data_source,
    std::function<void(StreamType)> stream_configured_callback,
    std::function<void(StreamDemuxer::Message,
                       unique_ptr<ElementaryStreamPacket>)>
                           es_packet_callback,
    StreamListener* stream_listener,
    DRMType drm_type) {
  return pimpl_->Initialize(std::move(segment_sequence), es_data_source,
                            stream_configured_callback, es_packet_callback,
                            stream_listener, drm_type, shared_from_this());
}
bool StreamManager::UpdateBuffer(TimeTicks playback_time) {
  return pimpl_->UpdateBuffer(playback_time);
}

void StreamManager::SetMediaSegmentSequence(
    std::unique_ptr<MediaSegmentSequence> segment_sequence) {
  pimpl_->SetMediaSegmentSequence(std::move(segment_sequence));
}


