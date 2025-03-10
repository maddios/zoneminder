//
// ZoneMinder Ffmpeg Camera Class Implementation
// Copyright (C) 2001-2008 Philip Coombes
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//

#include "zm_ffmpeg_camera.h"

#include "zm_ffmpeg_input.h"
#include "zm_monitor.h"
#include "zm_packet.h"
#include "zm_signal.h"
#include "zm_utils.h"

extern "C" {
#include <libavutil/time.h>
}

TimePoint start_read_time;

#if HAVE_LIBAVUTIL_HWCONTEXT_H
#if LIBAVCODEC_VERSION_CHECK(57, 89, 0, 89, 0)
static enum AVPixelFormat hw_pix_fmt;
static enum AVPixelFormat get_hw_format(
    AVCodecContext *ctx,
    const enum AVPixelFormat *pix_fmts
) {
  const enum AVPixelFormat *p;

  for ( p = pix_fmts; *p != -1; p++ ) {
    if ( *p == hw_pix_fmt )
      return *p;
  }

  Error("Failed to get HW surface format for %s.",
      av_get_pix_fmt_name(hw_pix_fmt));
  for ( p = pix_fmts; *p != -1; p++ )
    Error("Available HW surface format was %s.",
        av_get_pix_fmt_name(*p));

  return AV_PIX_FMT_NONE;
}
#if !LIBAVUTIL_VERSION_CHECK(56, 22, 0, 14, 0)
static enum AVPixelFormat find_fmt_by_hw_type(const enum AVHWDeviceType type) {
  switch (type) {
    case AV_HWDEVICE_TYPE_VAAPI:
      return AV_PIX_FMT_VAAPI;
    case AV_HWDEVICE_TYPE_DXVA2:
      return AV_PIX_FMT_DXVA2_VLD;
    case AV_HWDEVICE_TYPE_D3D11VA:
      return AV_PIX_FMT_D3D11;
    case AV_HWDEVICE_TYPE_VDPAU:
      return AV_PIX_FMT_VDPAU;
    case AV_HWDEVICE_TYPE_CUDA:
      return AV_PIX_FMT_CUDA;
#ifdef AV_HWDEVICE_TYPE_MMAL
    case AV_HWDEVICE_TYPE_MMAL:
      return AV_PIX_FMT_MMAL;
#endif
    case AV_HWDEVICE_TYPE_VIDEOTOOLBOX:
      return AV_PIX_FMT_VIDEOTOOLBOX;
    default:
      return AV_PIX_FMT_NONE;
  }
}
#endif
#endif
#endif

FfmpegCamera::FfmpegCamera(
    const Monitor *monitor,
    const std::string &p_path,
    const std::string &p_second_path,
    const std::string &p_method,
    const std::string &p_options,
    int p_width,
    int p_height,
    int p_colours,
    int p_brightness,
    int p_contrast,
    int p_hue,
    int p_colour,
    bool p_capture,
    bool p_record_audio,
    const std::string &p_hwaccel_name,
    const std::string &p_hwaccel_device) :
  Camera(
      monitor,
      FFMPEG_SRC,
      p_width,
      p_height,
      p_colours,
      ZM_SUBPIX_ORDER_DEFAULT_FOR_COLOUR(p_colours),
      p_brightness,
      p_contrast,
      p_hue,
      p_colour,
      p_capture,
      p_record_audio
      ),
  mPath(p_path),
  mSecondPath(p_second_path),
  mMethod(p_method),
  mOptions(p_options),
  hwaccel_name(p_hwaccel_name),
  hwaccel_device(p_hwaccel_device)
{
  if ( capture ) {
    FFMPEGInit();
  }

  frameCount = 0;
  mCanCapture = false;
  error_count = 0;
  use_hwaccel = true;
#if HAVE_LIBAVUTIL_HWCONTEXT_H
  hw_device_ctx = nullptr;
#if LIBAVCODEC_VERSION_CHECK(57, 89, 0, 89, 0)
  hw_pix_fmt = AV_PIX_FMT_NONE;
#endif
#endif

  mConvertContext = nullptr;
  /* Has to be located inside the constructor so other components such as zma
   * will receive correct colours and subpixel order */
  if ( colours == ZM_COLOUR_RGB32 ) {
    subpixelorder = ZM_SUBPIX_ORDER_RGBA;
    imagePixFormat = AV_PIX_FMT_RGBA;
  } else if ( colours == ZM_COLOUR_RGB24 ) {
    subpixelorder = ZM_SUBPIX_ORDER_RGB;
    imagePixFormat = AV_PIX_FMT_RGB24;
  } else if ( colours == ZM_COLOUR_GRAY8 ) {
    subpixelorder = ZM_SUBPIX_ORDER_NONE;
    imagePixFormat = AV_PIX_FMT_GRAY8;
  } else {
    Panic("Unexpected colours: %d", colours);
  }

}  // FfmpegCamera::FfmpegCamera

FfmpegCamera::~FfmpegCamera() {
  Close();

  FFMPEGDeInit();
}

int FfmpegCamera::PrimeCapture() {
  start_read_time = std::chrono::steady_clock::now();
  if ( mCanCapture ) {
    Debug(1, "Priming capture from %s, Closing", mPath.c_str());
    Close();
  }
  mVideoStreamId = -1;
  mAudioStreamId = -1;
  Debug(1, "Priming capture from %s", mPath.c_str());

  return OpenFfmpeg();
}

int FfmpegCamera::PreCapture() {
  return 0;
}

int FfmpegCamera::Capture(std::shared_ptr<ZMPacket> &zm_packet) {
  if (!mCanCapture) return -1;

  start_read_time = std::chrono::steady_clock::now();
  int ret;
  AVFormatContext *formatContextPtr;

  if ( mSecondFormatContext and
      (
        av_rescale_q(mLastAudioPTS, mAudioStream->time_base, AV_TIME_BASE_Q)
        <
        av_rescale_q(mLastVideoPTS, mVideoStream->time_base, AV_TIME_BASE_Q)
      ) ) {
    // if audio stream is behind video stream, then read from audio, otherwise video
    formatContextPtr = mSecondFormatContext;
    Debug(4, "Using audio input because audio PTS %" PRId64 " < video PTS %" PRId64,
        av_rescale_q(mLastAudioPTS, mAudioStream->time_base, AV_TIME_BASE_Q),
        av_rescale_q(mLastVideoPTS, mVideoStream->time_base, AV_TIME_BASE_Q)
        );
  } else {
    formatContextPtr = mFormatContext;
    Debug(4, "Using video input because %" PRId64 " >= %" PRId64,
        (mAudioStream?av_rescale_q(mLastAudioPTS, mAudioStream->time_base, AV_TIME_BASE_Q):0),
        av_rescale_q(mLastVideoPTS, mVideoStream->time_base, AV_TIME_BASE_Q)
        );
  }

  if ((ret = av_read_frame(formatContextPtr, &packet)) < 0) {
    if (
        // Check if EOF.
        (ret == AVERROR_EOF || (formatContextPtr->pb && formatContextPtr->pb->eof_reached)) ||
        // Check for Connection failure.
        (ret == -110)
       ) {
      Info("Unable to read packet from stream %d: error %d \"%s\".",
          packet.stream_index, ret, av_make_error_string(ret).c_str());
    } else {
      Error("Unable to read packet from stream %d: error %d \"%s\".",
          packet.stream_index, ret, av_make_error_string(ret).c_str());
    }
    return -1;
  }

  AVStream *stream = formatContextPtr->streams[packet.stream_index];
  ZM_DUMP_STREAM_PACKET(stream, packet, "ffmpeg_camera in");

  zm_packet->codec_type = stream->codecpar->codec_type;

  bytes += packet.size;
  zm_packet->set_packet(&packet);
  zm_packet->stream = stream;
  zm_packet->pts = av_rescale_q(packet.pts, stream->time_base, AV_TIME_BASE_Q);
  if (packet.pts != AV_NOPTS_VALUE) {
    if (stream == mVideoStream) {
      if (mFirstVideoPTS == AV_NOPTS_VALUE)
        mFirstVideoPTS = packet.pts;

      mLastVideoPTS = packet.pts - mFirstVideoPTS;
    } else {
      if (mFirstAudioPTS == AV_NOPTS_VALUE)
        mFirstAudioPTS = packet.pts;

      mLastAudioPTS = packet.pts - mFirstAudioPTS;
    }
  }
  zm_av_packet_unref(&packet);

  return 1;
} // FfmpegCamera::Capture

int FfmpegCamera::PostCapture() {
  // Nothing to do here
  return 0;
}

int FfmpegCamera::OpenFfmpeg() {
  int ret;

  error_count = 0;

  // Handle options
  AVDictionary *opts = nullptr;
  ret = av_dict_parse_string(&opts, Options().c_str(), "=", ",", 0);
  if ( ret < 0 ) {
    Warning("Could not parse ffmpeg input options '%s'", Options().c_str());
  }

  // Set transport method as specified by method field, rtpUni is default
  std::string protocol = mPath.substr(0, 4);
  protocol = StringToUpper(protocol);
  if ( protocol == "RTSP" ) {
    const std::string method = Method();
    if ( method == "rtpMulti" ) {
      ret = av_dict_set(&opts, "rtsp_transport", "udp_multicast", 0);
    } else if ( method == "rtpRtsp" ) {
      ret = av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    } else if ( method == "rtpRtspHttp" ) {
      ret = av_dict_set(&opts, "rtsp_transport", "http", 0);
    } else if ( method == "rtpUni" ) {
      ret = av_dict_set(&opts, "rtsp_transport", "udp", 0);
    } else {
      Warning("Unknown method (%s)", method.c_str());
    }
    if ( ret < 0 ) {
      Warning("Could not set rtsp_transport method '%s'", method.c_str());
    }
  }  // end if RTSP

  Debug(1, "Calling avformat_open_input for %s", mPath.c_str());

  mFormatContext = avformat_alloc_context();
  // Speed up find_stream_info
  // FIXME can speed up initial analysis but need sensible parameters...
  // mFormatContext->probesize = 32;
  // mFormatContext->max_analyze_duration = 32;
  mFormatContext->interrupt_callback.callback = FfmpegInterruptCallback;
  mFormatContext->interrupt_callback.opaque = this;

  ret = avformat_open_input(&mFormatContext, mPath.c_str(), nullptr, &opts);
  if ( ret != 0 )
  {
    Error("Unable to open input %s due to: %s", mPath.c_str(),
        av_make_error_string(ret).c_str());

    if ( mFormatContext ) {
      avformat_close_input(&mFormatContext);
      mFormatContext = nullptr;
    }
    av_dict_free(&opts);

    return -1;
  }
  AVDictionaryEntry *e = nullptr;
  while ( (e = av_dict_get(opts, "", e, AV_DICT_IGNORE_SUFFIX)) != nullptr ) {
    Warning("Option %s not recognized by ffmpeg", e->key);
  }
  av_dict_free(&opts);

  Debug(1, "Finding stream info");
  ret = avformat_find_stream_info(mFormatContext, nullptr);

  if ( ret < 0 ) {
    Error("Unable to find stream info from %s due to: %s",
        mPath.c_str(), av_make_error_string(ret).c_str());
    return -1;
  }

  // Find first video stream present
  // The one we want Might not be the first
  mVideoStreamId = -1;
  mAudioStreamId = -1;
  for (unsigned int i=0; i < mFormatContext->nb_streams; i++) {
    AVStream *stream = mFormatContext->streams[i];
    if (is_video_stream(stream)) {
      if (mVideoStreamId == -1) {
        mVideoStreamId = i;
        mVideoStream = mFormatContext->streams[i];
        // if we break, then we won't find the audio stream
        continue;
      } else {
        Debug(2, "Have another video stream.");
      }
    } else if (is_audio_stream(stream)) {
      if (mAudioStreamId == -1) {
        mAudioStreamId = i;
        mAudioStream = mFormatContext->streams[i];
      } else {
        Debug(2, "Have another audio stream.");
      }
    }
  }  // end foreach stream

  if (mVideoStreamId == -1) {
    Error("Unable to locate video stream in %s", mPath.c_str());
    return -1;
  }

  Debug(3, "Found video stream at index %d, audio stream at index %d",
      mVideoStreamId, mAudioStreamId);

  AVCodec *mVideoCodec = nullptr;
  if (mVideoStream->codecpar->codec_id == AV_CODEC_ID_H264) {
    if ((mVideoCodec = avcodec_find_decoder_by_name("h264_mmal")) == nullptr) {
      Debug(1, "Failed to find decoder (h264_mmal)");
    } else {
      Debug(1, "Success finding decoder (h264_mmal)");
    }
  }

  if (!mVideoCodec) {
    mVideoCodec = avcodec_find_decoder(mVideoStream->codecpar->codec_id);
    if (!mVideoCodec) {
      // Try and get the codec from the codec context
      Error("Can't find codec for video stream from %s", mPath.c_str());
      return -1;
    }
  }

  mVideoCodecContext = avcodec_alloc_context3(mVideoCodec);
  avcodec_parameters_to_context(mVideoCodecContext, mFormatContext->streams[mVideoStreamId]->codecpar);

#ifdef CODEC_FLAG2_FAST
  mVideoCodecContext->flags2 |= CODEC_FLAG2_FAST | CODEC_FLAG_LOW_DELAY;
#endif

  zm_dump_stream_format(mFormatContext, mVideoStreamId, 0, 0);

  if (use_hwaccel && (hwaccel_name != "")) {
#if HAVE_LIBAVUTIL_HWCONTEXT_H
    // 3.2 doesn't seem to have all the bits in place, so let's require 3.4 and up
  #if LIBAVCODEC_VERSION_CHECK(57, 107, 0, 107, 0)
    // Print out available types
    enum AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
    while ((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE)
      Debug(1, "%s", av_hwdevice_get_type_name(type));

    const char *hw_name = hwaccel_name.c_str();
    type = av_hwdevice_find_type_by_name(hw_name);
    if (type == AV_HWDEVICE_TYPE_NONE) {
      Debug(1, "Device type %s is not supported.", hw_name);
    } else {
      Debug(1, "Found hwdevice %s", av_hwdevice_get_type_name(type));
    }

    #if LIBAVUTIL_VERSION_CHECK(56, 22, 0, 14, 0)
    // Get hw_pix_fmt
    for (int i = 0;; i++) {
      const AVCodecHWConfig *config = avcodec_get_hw_config(mVideoCodec, i);
      if (!config) {
        Debug(1, "Decoder %s does not support config %d.",
            mVideoCodec->name, i);
        break;
      }
      if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)
          && (config->device_type == type)
          ) {
        hw_pix_fmt = config->pix_fmt;
        Debug(1, "Decoder %s does support our type %s.",
            mVideoCodec->name, av_hwdevice_get_type_name(type));
        //break;
      } else {
        Debug(1, "Decoder %s hwConfig doesn't match our type: %s != %s, pix_fmt %s.",
            mVideoCodec->name,
            av_hwdevice_get_type_name(type),
            av_hwdevice_get_type_name(config->device_type),
            av_get_pix_fmt_name(config->pix_fmt)
            );
      }
    }  // end foreach hwconfig
    #else
    hw_pix_fmt = find_fmt_by_hw_type(type);
    #endif
    if (hw_pix_fmt != AV_PIX_FMT_NONE) {
      Debug(1, "Selected hw_pix_fmt %d %s",
          hw_pix_fmt, av_get_pix_fmt_name(hw_pix_fmt));

       mVideoCodecContext->hwaccel_flags |= AV_HWACCEL_FLAG_IGNORE_LEVEL;
        //if (!lavc_param->check_hw_profile)
       mVideoCodecContext->hwaccel_flags |= AV_HWACCEL_FLAG_ALLOW_PROFILE_MISMATCH;

      ret = av_hwdevice_ctx_create(&hw_device_ctx, type,
          (hwaccel_device != "" ? hwaccel_device.c_str() : nullptr), nullptr, 0);
      if ( ret < 0 and hwaccel_device != "" ) {
        ret = av_hwdevice_ctx_create(&hw_device_ctx, type, nullptr, nullptr, 0);
      }
      if (ret < 0) {
        Error("Failed to create hwaccel device. %s", av_make_error_string(ret).c_str());
        hw_pix_fmt = AV_PIX_FMT_NONE;
      } else {
        Debug(1, "Created hwdevice for %s", hwaccel_device.c_str());
        mVideoCodecContext->get_format = get_hw_format;
        mVideoCodecContext->hw_device_ctx = av_buffer_ref(hw_device_ctx);
      }
    } else {
      Debug(1, "Failed to find suitable hw_pix_fmt.");
    }
  #else
    Debug(1, "AVCodec not new enough for hwaccel");
  #endif
#else
    Warning("HWAccel support not compiled in.");
#endif
  }  // end if hwaccel_name

  // set codec to automatically determine how many threads suits best for the decoding job
  mVideoCodecContext->thread_count = 0;

  if (mVideoCodec->capabilities | AV_CODEC_CAP_FRAME_THREADS) {
    mVideoCodecContext->thread_type = FF_THREAD_FRAME;
  } else if (mVideoCodec->capabilities | AV_CODEC_CAP_SLICE_THREADS) {
    mVideoCodecContext->thread_type = FF_THREAD_SLICE;
  } else {
    mVideoCodecContext->thread_count = 1; //don't use multithreading
  }

  ret = avcodec_open2(mVideoCodecContext, mVideoCodec, &opts);

  e = nullptr;
  while ((e = av_dict_get(opts, "", e, AV_DICT_IGNORE_SUFFIX)) != nullptr) {
    Warning("Option %s not recognized by ffmpeg", e->key);
  }
  if (ret < 0) {
    Error("Unable to open codec for video stream from %s", mPath.c_str());
    av_dict_free(&opts);
    return -1;
  }
  zm_dump_codec(mVideoCodecContext);

  if (mAudioStreamId == -1 and !monitor->GetSecondPath().empty()) {
    Debug(1, "Trying secondary stream at %s", monitor->GetSecondPath().c_str());
    FFmpeg_Input *second_input = new FFmpeg_Input();
    if (second_input->Open(monitor->GetSecondPath().c_str()) > 0) {
      mSecondFormatContext = second_input->get_format_context();
      mAudioStreamId = second_input->get_audio_stream_id();
      mAudioStream = second_input->get_audio_stream();
    } else {
      Warning("Failed to open secondary input");
    }
  }  // end if have audio stream

  if ( mAudioStreamId >= 0 ) {
    AVCodec *mAudioCodec = nullptr;
    if (!(mAudioCodec = avcodec_find_decoder(mAudioStream->codecpar->codec_id))) {
      Debug(1, "Can't find codec for audio stream from %s", mPath.c_str());
    } else {
      mAudioCodecContext = avcodec_alloc_context3(mAudioCodec);
      avcodec_parameters_to_context(mAudioCodecContext, mAudioStream->codecpar);

      zm_dump_stream_format((mSecondFormatContext?mSecondFormatContext:mFormatContext), mAudioStreamId, 0, 0);
      // Open the codec
      if (avcodec_open2(mAudioCodecContext, mAudioCodec, nullptr) < 0) {
        Error("Unable to open codec for audio stream from %s", mPath.c_str());
        return -1;
      }  // end if opened
    }  // end if found decoder
  }  // end if mAudioStreamId

  if (
      ((unsigned int)mVideoCodecContext->width != width)
      ||
      ((unsigned int)mVideoCodecContext->height != height)
      ) {
    Warning("Monitor dimensions are %dx%d but camera is sending %dx%d",
        width, height, mVideoCodecContext->width, mVideoCodecContext->height);
  }

  mCanCapture = true;

  return 1;
} // int FfmpegCamera::OpenFfmpeg()

int FfmpegCamera::Close() {
  mCanCapture = false;

  if ( mVideoCodecContext ) {
    avcodec_close(mVideoCodecContext);
    avcodec_free_context(&mVideoCodecContext);
    mVideoCodecContext = nullptr;  // Freed by av_close_input_file
  }
  if ( mAudioCodecContext ) {
    avcodec_close(mAudioCodecContext);
    avcodec_free_context(&mAudioCodecContext);
    mAudioCodecContext = nullptr;  // Freed by av_close_input_file
  }

#if HAVE_LIBAVUTIL_HWCONTEXT_H
  if ( hw_device_ctx ) {
    av_buffer_unref(&hw_device_ctx);
  }
#endif

  if ( mFormatContext ) {
    avformat_close_input(&mFormatContext);
    mFormatContext = nullptr;
  }

  return 0;
}  // end FfmpegCamera::Close

int FfmpegCamera::FfmpegInterruptCallback(void *ctx) {
  if (zm_terminate) {
    Debug(1, "Received terminate in cb");
    return zm_terminate;
  }

  TimePoint now = std::chrono::steady_clock::now();
  if (now - start_read_time > Seconds(10)) {
    Debug(1, "timeout in ffmpeg camera now %" PRIi64 " - %" PRIi64 " > 10 s",
          static_cast<int64>(std::chrono::duration_cast<Seconds>(now.time_since_epoch()).count()),
          static_cast<int64>(std::chrono::duration_cast<Seconds>(start_read_time.time_since_epoch()).count()));
    return 1;
  }
  return 0;
}
