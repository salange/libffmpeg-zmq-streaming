#include <chrono>
#include <iostream>
#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <zmq.hpp>

using namespace std;
constexpr int KB = 1024;
constexpr int waitTime = 100;
constexpr double width = 600, height = 500;
constexpr int fps = 15;
constexpr int target_bitrate = 4000000;

static std::string av_strerror(int errnum) {
  std::vector<char> v(KB);
  av_strerror(errnum, v.data(), v.size());
  return std::string(v.begin(), v.end());
}

class AVReceiver {
  zmq::socket_t socket;
  zmq::context_t ctx;
  AVCodecContext *dec_ctx;
  AVCodecParserContext *parser;
  int successes = 0;
  size_t bytes_received = 0;

public:
  AVReceiver(const string &host, const unsigned int port) : ctx(1) {
    socket = zmq::socket_t(ctx, zmq::socket_type::sub);
    const auto connect_str = string("tcp://") + host + ":" + to_string(port);
    socket.set(zmq::sockopt::subscribe, "");
    socket.connect(connect_str);
    std::cout << "Connected socket to " << connect_str << std::endl;
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
      throw std::runtime_error("Could not find decoder");
    }
    dec_ctx = avcodec_alloc_context3(codec);
    if (!dec_ctx) {
      throw std::runtime_error("Could not allocate decoder context");
    }
    parser = av_parser_init(codec->id);
    if (!parser) {
      throw std::runtime_error("Could not init parser");
    }
    const AVRational dst_fps = {fps, 1};
    dec_ctx->codec_tag = 0;
    dec_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
    int res = avcodec_open2(dec_ctx, codec, nullptr);
    if (res < 0) {
      throw std::runtime_error("Could not open decoder context: " +
                               av_strerror(res));
    }
  }

  int decode(AVCodecContext *dec_ctx, AVFrame *frame, AVPacket *pkt) {
    int ret;

    ret = avcodec_send_packet(dec_ctx, pkt);
    if (ret < 0) {
      std::cerr << "Error sending packet for decoding: " << av_strerror(ret)
                << std::endl;
      return ret;
    } else {
      /* std::cout << "Sent packet of size " << pkt->size << " bytes" <<
       * std::endl; */
    }

    while (ret >= 0) {
      ret = avcodec_receive_frame(dec_ctx, frame);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        /* std::cerr << "Decoder shits bed: " << av_strerror(ret) << std::endl;
         */
        return ret;
      } else if (ret < 0) {
        std::cerr << "Error during decoding: " << av_strerror(ret) << std::endl;
        return ret;
      } else {
        // Ok to return? might be more than one frame ready? add callback for
        // this
        return 0;
      }
    }
    return 0;
  }

  void receive() {
    bool more = true;
    int num_pkts = 0;
    zmq::message_t incoming;
    std::vector<std::uint8_t> buffer;
    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    while (more) {
      socket.recv(incoming, zmq::recv_flags::none);
      bytes_received += incoming.size();
      ++num_pkts;
      /* std::cout << "Received packet of size " << incoming.size() <<
       * std::endl; */
      if (incoming.to_string().find("packet") != std::string::npos) {
        continue;
      } else {
        static int i = 0;
        buffer.resize(incoming.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(&buffer[0], incoming.data(), incoming.size());
        more = socket.get(zmq::sockopt::rcvmore) > 0;
        /* std::cout << "Attempt to parse " << incoming.size() << " bytes" */
        /*           << std::endl; */
        int in_len = incoming.size();
        while (in_len) {
          int result = av_parser_parse2(
              this->parser, this->dec_ctx, &pkt->data, &pkt->size,
              static_cast<std::uint8_t *>(&buffer[0]), incoming.size(),
              AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
          in_len -= result;
          if (result < 0) {
            std::cerr << "Parsing packet failed" << std::endl;
          } else {
            if (pkt->size > 0) {
              /* std::cout << "Decoding packet of size " << pkt->size <<
               * std::endl; */
              result = this->decode(dec_ctx, frame, pkt);
              if (result < 0) {
                std::cerr << "Could not decode: " << av_strerror(result)
                          << std::endl;
              } else {
                static int wat = 0;
                if (++wat < 10)
                  return;
                /* std::cout << "Decoded" << std::endl; */
                ++successes;
                std::vector<int> sizes = {frame->height, frame->width};
                std::vector<size_t> steps{
                    static_cast<size_t>(frame->linesize[0])};

                const auto height = frame->height;
                const auto width = frame->width;
                const auto actual_size = cv::Size(width, height);
                void *y = frame->data[0];
                void *u = frame->data[1];
                void *v = frame->data[2];

                cv::Mat y_mat(height, width, CV_8UC1, y, frame->linesize[0]);
                cv::Mat u_mat(height / 2, width / 2, CV_8UC1, u,
                              frame->linesize[1]);
                cv::Mat v_mat(height / 2, width / 2, CV_8UC1, v,
                              frame->linesize[2]);
                cv::Mat u_resized, v_resized;
                cv::resize(u_mat, u_resized, actual_size, 0, 0,
                           cv::INTER_NEAREST); // repeat u values 4 times
                cv::resize(v_mat, v_resized, actual_size, 0, 0,
                           cv::INTER_NEAREST); // repeat v values 4 times

                cv::Mat yuv;

                std::vector<cv::Mat> yuv_channels = {y_mat, u_resized,
                                                     v_resized};
                cv::merge(yuv_channels, yuv);

                cv::Mat image;
                cv::cvtColor(yuv, image, cv::COLOR_YUV2BGR);

                /* cv::Mat image(sizes, type, frame->data[0], &steps[0]); */
                /* cv::Mat image_converted; */
                /* cv::cvtColor(image, image_converted, cv::COLOR_YUV2BGR); */
                cv::imshow("decoded", image);
                cv::waitKey(waitTime);
              }
            }
          }
        }
      }
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    /* std::cout << "Received " << num_pkts << std::endl; */
  }

  ~AVReceiver() {
    socket.close();
    av_parser_close(parser);
    avcodec_free_context(&dec_ctx);
    std::cout << "Decoded " << successes << " frames." << std::endl;
    std::cout << "Received " << bytes_received / KB << " KB" << std::endl;
  }
};

class AVTransmitter {

  zmq::socket_t socket;
  zmq::context_t ctx;

  int num_pkts;
  size_t bytes_sent = 0;

public:
  AVTransmitter(const string &host, const unsigned int port) : ctx(1) {
    socket = zmq::socket_t(ctx, zmq::socket_type::pub);
    const auto bind_str = string("tcp://") + host + ":" + to_string(port);
    socket.bind(bind_str);
    std::cout << "Bound socket to " << bind_str << std::endl;
    num_pkts = 0;
  }

  static int custom_io_write(void *opaque, uint8_t *buffer,
                             int32_t buffer_size) {

    AVTransmitter *self = static_cast<AVTransmitter *>(opaque);
    return self->custom_io_write(buffer, buffer_size);
  }

  int custom_io_write(uint8_t *buffer, int32_t buffer_size) {
    bytes_sent += buffer_size;
    zmq::message_t msg(buffer_size);
    memcpy(msg.data(), buffer, buffer_size);
    /* std::cout << "Sent packet of size " << msg.size() << std::endl; */
    socket.send(zmq::message_t("packet"), zmq::send_flags::sndmore);
    socket.send(msg, zmq::send_flags::sndmore);
    num_pkts += 2;
    return 0;
  }
  ~AVTransmitter() {
    socket.close();
    std::cout << "Sent " << bytes_sent / KB << " KB" << std::endl;
  }

  void frame_ended() {

    socket.send(zmq::message_t());
    /* std::cout << "Sent " << ++num_pkts << std::endl; */
    num_pkts = 0;
  }

  AVIOContext *getContext(AVFormatContext *fctx) {
    constexpr int avio_buffer_size = 4 * KB;

    // TODO: free buffer
    unsigned char *avio_buffer =
        static_cast<unsigned char *>(av_malloc(avio_buffer_size));

    AVIOContext *custom_io =
        avio_alloc_context(avio_buffer, avio_buffer_size, 1, this, nullptr,
                           &AVTransmitter::custom_io_write, nullptr);

    if (!custom_io) {
      std::cout << "Could not open output IO context!" << std::endl;
      return nullptr;
    }
    return custom_io;
  }
};

int initialize_avformat_context(AVFormatContext *&fctx,
                                const char *format_name) {
  return avformat_alloc_output_context2(&fctx, nullptr, format_name, nullptr);
}

void set_codec_params(AVFormatContext *&fctx, AVCodecContext *&codec_ctx,
                      double width, double height, int fps) {
  const AVRational dst_fps = {fps, 1};

  codec_ctx->codec_tag = 0;
  codec_ctx->bit_rate = target_bitrate;
  codec_ctx->codec_id = AV_CODEC_ID_H264;
  codec_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
  codec_ctx->width = width;
  codec_ctx->height = height;
  codec_ctx->gop_size = 12;
  codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
  codec_ctx->framerate = dst_fps;
  codec_ctx->time_base = av_inv_q(dst_fps);
  if (fctx->oformat->flags & AVFMT_GLOBALHEADER) {
    codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }
}

int initialize_codec_stream(AVStream *&stream, AVCodecContext *&codec_ctx,
                            AVCodec *&codec) {
  int ret = avcodec_parameters_from_context(stream->codecpar, codec_ctx);
  if (ret < 0) {
    /* std::cout << "Could not initialize stream codec parameters!" <<
     * std::endl; */
    /* exit(1); */
    return ret;
  }

  AVDictionary *codec_options = nullptr;
  av_dict_set(&codec_options, "profile", "high", 0);
  av_dict_set(&codec_options, "preset", "ultrafast", 0);
  av_dict_set(&codec_options, "tune", "zerolatency", 0);

  // open video encoder
  ret = avcodec_open2(codec_ctx, codec, &codec_options);
  return ret;
}

SwsContext *initialize_sample_scaler(AVCodecContext *codec_ctx, double width,
                                     double height) {
  SwsContext *swsctx = sws_getContext(width, height, AV_PIX_FMT_BGR24, width,
                                      height, codec_ctx->pix_fmt, SWS_BICUBIC,
                                      nullptr, nullptr, nullptr);
  return swsctx;
}

AVFrame *allocate_frame_buffer(AVCodecContext *codec_ctx, double width,
                               double height) {
  AVFrame *frame = av_frame_alloc();

  std::uint8_t *framebuf = new uint8_t[av_image_get_buffer_size(
      codec_ctx->pix_fmt, width, height, 1)];
  av_image_fill_arrays(frame->data, frame->linesize, framebuf,
                       codec_ctx->pix_fmt, width, height, 1);
  frame->width = width;
  frame->height = height;
  frame->format = static_cast<int>(codec_ctx->pix_fmt);

  return frame;
}

int write_frame(AVCodecContext *codec_ctx, AVFormatContext *fmt_ctx,
                AVFrame *frame) {
  AVPacket pkt = {0};
  av_init_packet(&pkt);

  int ret = avcodec_send_frame(codec_ctx, frame);
  if (ret < 0) {
    std::cout << "Error sending frame to codec context!" << std::endl;
    return ret;
  }

  ret = avcodec_receive_packet(codec_ctx, &pkt);
  if (ret < 0) {
    std::cout << "Error receiving packet from codec context!" << std::endl;
    return ret;
  }

  av_interleaved_write_frame(fmt_ctx, &pkt);
  return ret;
}

static void generatePattern(cv::Mat &image, unsigned char i) {
  image.setTo(cv::Scalar(255, 255, 255));
  float perc_height = 1.0 * i / 255;
  float perc_width = 1.0 * i / 255;
  image.row(perc_height * image.rows).setTo(cv::Scalar(0, 0, 0));
  image.col(perc_width * image.cols).setTo(cv::Scalar(0, 0, 0));
}

void stream_video() {
  av_register_all();
  avformat_network_init();

  int ret;
  // extra 16 bytes which swscaler wants when operating on this buffer
  std::vector<uint8_t> imgbuf(height * width * 3 + 16);
  cv::Mat image(height, width, CV_8UC3, imgbuf.data(), width * 3);
  AVFormatContext *ofmt_ctx = nullptr;
  AVCodec *out_codec = nullptr;
  AVStream *out_stream = nullptr;
  AVCodecContext *out_codec_ctx = nullptr;
  AVTransmitter transmitter("*", 15001);
  AVReceiver receiver("localhost", 15001);

  int success = initialize_avformat_context(ofmt_ctx, "h264");
  if (success != 0) {
    std::cout << "Could not allocate output format context! "
              << av_strerror(success) << std::endl;
    exit(1);
  }

  ofmt_ctx->pb = transmitter.getContext(ofmt_ctx);

  out_codec = avcodec_find_encoder(AV_CODEC_ID_H264);
  out_stream = avformat_new_stream(ofmt_ctx, out_codec);
  out_codec_ctx = avcodec_alloc_context3(out_codec);

  set_codec_params(ofmt_ctx, out_codec_ctx, width, height, fps);
  success = initialize_codec_stream(out_stream, out_codec_ctx, out_codec);
  if (success != 0) {
    std::cout << "Could not initialize codec stream " << av_strerror(success)
              << std::endl;
    exit(1);
  }

  out_stream->codecpar->extradata = out_codec_ctx->extradata;
  out_stream->codecpar->extradata_size = out_codec_ctx->extradata_size;

  SwsContext *swsctx = initialize_sample_scaler(out_codec_ctx, width, height);
  if (!swsctx) {
    std::cout << "Could not initialize sample scaler!" << std::endl;
    exit(1);
  }

  AVFrame *frame = allocate_frame_buffer(out_codec_ctx, width, height);

  int cur_size;
  uint8_t *cur_ptr;

  ret = avformat_write_header(ofmt_ctx, nullptr);
  if (ret < 0) {
    std::cout << "Could not write header! " << av_strerror(ret) << std::endl;
    exit(1);
  }

  const int stride[] = {static_cast<int>(image.step[0])};

  int ms = 0;
  std::vector<std::string> filenames;
  cv::glob("/Users/rasmus/Downloads/images/*.jpeg", filenames);
  std::sort(filenames.begin(), filenames.end());

  const int n_frames = filenames.size();
  int jpeg_bytes = 0;
  for (int i = 0; i < n_frames; ++i) {

    /* generatePattern(image, i); */
    image = cv::imread(filenames[i]);
    vector<uchar> buf;
    cv::imencode(".jpg", image, buf);
    jpeg_bytes += buf.size();

    cv::imshow("encoded", image);
    cv::waitKey(waitTime);
    sws_scale(swsctx, &image.data, stride, 0, image.rows, frame->data,
              frame->linesize);
    frame->pts +=
        av_rescale_q(1, out_codec_ctx->time_base, out_stream->time_base);
    auto tic = std::chrono::system_clock::now();
    success = write_frame(out_codec_ctx, ofmt_ctx, frame);
    if (success != 0) {
      std::cout << "Could not write frame " << av_strerror(success)
                << std::endl;
      /* exit(1); */
    } else {
      auto toc = std::chrono::system_clock::now();
      ms += std::chrono::duration_cast<std::chrono::milliseconds>(toc - tic)
                .count();
      transmitter.frame_ended();
      receiver.receive();
    }
  }
  std::cout << "Avg encoding time " << ms * 1.0 / n_frames << std::endl;
  std::cout << "Sent " << n_frames << " frames." << std::endl;
  std::cout << "Cumulative jpeg size: " << jpeg_bytes / KB << " KB"
            << std::endl;

  av_write_trailer(ofmt_ctx);

  av_frame_free(&frame);
  avcodec_close(out_codec_ctx);
  avio_context_free(&ofmt_ctx->pb);
  avformat_free_context(ofmt_ctx);
}

int main() {
  /* av_log_set_level(AV_LOG_DEBUG); */

  stream_video();

  return 0;
}
