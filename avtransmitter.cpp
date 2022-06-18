#include "avtransmitter.hpp"
#include <chrono>
#include <iomanip>
#include <iostream>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

AVTransmitter::AVTransmitter(const std::string &host, const unsigned int port,
                             unsigned int fps, unsigned int gop_size,
                             unsigned int target_bitrate)
    : fps_(fps), sdp_(""), gop_size_(gop_size),
      target_bitrate_(target_bitrate) {

  AVOutputFormat *format = av_guess_format("rtp", nullptr, nullptr);
  if (!format) {
    throw std::runtime_error("Could not guess output format.");
  }
  const auto url = std::string("rtp://") + host + ":" + std::to_string(port);
  int success =
      avutils::initialize_avformat_context(this->ofmt_ctx, format, url.c_str());
  /* this->ofmt_ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL; */
  // this->ofmt_ctx->flags = AVFMT_FLAG_NOBUFFER | AVFMT_FLAG_FLUSH_PACKETS;

  if (success != 0) {
    throw std::runtime_error("Could not allocate output format context! " +
                             avutils::av_strerror2(success));
  }

  this->out_codec = avcodec_find_encoder(AV_CODEC_ID_H264);
  if (!this->out_codec) {
    throw std::runtime_error("Could not find encoder");
  }
  this->out_stream = avformat_new_stream(this->ofmt_ctx, this->out_codec);
  if (!this->out_stream) {
    throw std::runtime_error("Could not find stream");
  }
  this->out_codec_ctx = avcodec_alloc_context3(this->out_codec);

  // Global header is optional. If present, PPS and SPS h264 info will get
  // written to AVFormatContext's extradata, and consequently show up in the SDP
  // file. Howerver the same info seems to be present inside the packet stream
  // when disabled, making it unnecessary.
  /* this->out_codec_ctx->flags |=AV_CODEC_FLAG_GLOBAL_HEADER; */
  if (!this->out_codec_ctx) {
    throw std::runtime_error("Could not allocate output codec context");
  }
}

void AVTransmitter::encode_frame(const cv::Mat &image) {
  static bool first_time = true;
  if (first_time) {
    first_time = false;
    height_ = image.rows;
    width_ = image.cols;
    avutils::set_codec_params(this->out_codec_ctx, width_, height_, fps_,
                              target_bitrate_, gop_size_);
    int success = avutils::initialize_codec_stream(this->out_stream,
                                                   out_codec_ctx, out_codec);
    this->out_stream->time_base.num = 1;
    this->out_stream->time_base.den = fps_;
    avio_open(&(this->ofmt_ctx->pb), this->ofmt_ctx->filename, AVIO_FLAG_WRITE);

    /* Write a file for VLC */
    constexpr int buflen = 1024;
    char buf[buflen] = {0};
    AVFormatContext *ac[] = {this->ofmt_ctx};
    av_sdp_create(ac, 1, buf, buflen);
    this->sdp_ = std::string(buf);

    if (success != 0) {
      throw std::invalid_argument("Could not initialize codec stream " +
                                  avutils::av_strerror2(success));
    }
    if (!swsctx) {
      swsctx = avutils::initialize_sample_scaler(this->out_codec_ctx, width_,
                                                 height_);
    }
    if (!swsctx) {
      throw std::runtime_error("Could not initialize sample scaler!");
    }
  }
  if (!frame_) {
    frame_ =
        avutils::allocate_frame_buffer(this->out_codec_ctx, width_, height_);
    int success = avformat_write_header(this->ofmt_ctx, nullptr);
    if (success != 0) {
      std::runtime_error("Could not write header! " +
                         avutils::av_strerror2(success));
    }
  }
  if (imgbuf.empty()) {
    imgbuf.resize(height_ * width_ * 3 + 16);
    this->canvas_ =
        cv::Mat(height_, width_, CV_8UC3, imgbuf.data(), width_ * 3);
  }
  image.copyTo(this->canvas_);
  const int stride[] = {static_cast<int>(image.step[0])};

  /* cv::imshow("encoded", image); */
  /* cv::waitKey(20); */
  sws_scale(this->swsctx, &canvas_.data, stride, 0, canvas_.rows, frame_->data,
            frame_->linesize);
  frame_->pts +=
      av_rescale_q(1, out_codec_ctx->time_base, this->out_stream->time_base);

  int success =
      avutils::write_frame(this->out_codec_ctx, this->ofmt_ctx, this->frame_);
  if (success != 0) {
    std::cerr << "Could not write frame: " << avutils::av_strerror2(success)
              << ". Maybe send more input. " << std::endl;
  } else {
    this->frame_ended();
  }
}

AVTransmitter::~AVTransmitter() {
  av_write_trailer(this->ofmt_ctx);
  av_frame_free(&frame_);
  avcodec_close(this->out_codec_ctx);
  avio_context_free(&(this->ofmt_ctx->pb));
  avformat_free_context(this->ofmt_ctx);
}

void AVTransmitter::frame_ended() {
  // Send an h264 AUD, which tells the receiving end that a frame has ended.
  // seeting the aud option to x264 does not seem to do this, so we do it
  // manually found this here https://stackoverflow.com/a/60469996/2397253, but
  // i dont know why they are sending 16 as the 6th byte, AUD is simply 0 0 0 1
  // 9
  // TODO: remove if h264 is not used
  AVPacket pkt2 = {0};
  // not sure if just using the last frame's pts or and dts like this is wise
  pkt2.dts = frame_->pts;
  pkt2.pts = frame_->pts;
  pkt2.data = static_cast<std::uint8_t *>(av_mallocz(5));
  pkt2.data[4] = 9;  // code for AUD
  pkt2.data[3] = 1;  // NAL starts with 0 0 0 1
  pkt2.size = 5;
  int success = av_write_frame(this->ofmt_ctx, &pkt2);
  if (success != 0) {
    std::cout << "Could not write AUD" << std::endl;
  }
}
std::string AVTransmitter::get_sdp() const { return this->sdp_; }
