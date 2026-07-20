#include "modules/decoding/VideoDecoderBackend.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/pixfmt.h>
}

#include <string>

namespace sclient {

namespace {

DecodedPixelFormat ResolveDecodedPixelFormat(AVPixelFormat pixel_format) {
    switch (pixel_format) {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P:
            return DecodedPixelFormat::kYuv420p;
        case AV_PIX_FMT_NV12:
            return DecodedPixelFormat::kNv12;
        default:
            return DecodedPixelFormat::kUnknown;
    }
}

std::string AvErrorString(int error_code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(error_code, buffer, sizeof(buffer));
    return std::string(buffer);
}

void CopyDecodedFrameView(const AVFrame &source, DecodedFrame *decoded_frame) {
    if (decoded_frame == nullptr) {
        return;
    }

    decoded_frame->width = source.width;
    decoded_frame->height = source.height;
    decoded_frame->pixel_format = ResolveDecodedPixelFormat(static_cast<AVPixelFormat>(source.format));
    for (std::size_t index = 0; index < decoded_frame->data.size(); ++index) {
        decoded_frame->data[index] = source.data[index];
        decoded_frame->linesize[index] = source.linesize[index];
    }
}

std::shared_ptr<void> MakeAvFrameOwner(AVFrame *frame) {
    return std::shared_ptr<void>(frame, [](void *pointer) {
        AVFrame *owned_frame = reinterpret_cast<AVFrame *>(pointer);
        av_frame_free(&owned_frame);
    });
}

class SoftwareVideoDecoderBackend final : public VideoDecoderBackend {
public:
    ~SoftwareVideoDecoderBackend() override {
        Shutdown();
    }

    bool Initialize(std::string *error_message) override {
        Shutdown();

        codec_ = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (codec_ == nullptr) {
            if (error_message != nullptr) {
                *error_message = "failed to find H.264 decoder";
            }
            return false;
        }

        codec_context_ = avcodec_alloc_context3(codec_);
        if (codec_context_ == nullptr) {
            if (error_message != nullptr) {
                *error_message = "failed to allocate decoder context";
            }
            return false;
        }

        if (avcodec_open2(codec_context_, codec_, nullptr) < 0) {
            if (error_message != nullptr) {
                *error_message = "failed to open H.264 decoder";
            }
            Shutdown();
            return false;
        }

        decoded_frame_ = av_frame_alloc();
        if (decoded_frame_ == nullptr) {
            if (error_message != nullptr) {
                *error_message = "failed to allocate decode frame";
            }
            Shutdown();
            return false;
        }

        return true;
    }

    bool Decode(
            const std::uint8_t *data,
            std::size_t size,
            DecodedFrame *decoded_frame,
            std::string *error_message) override {
        if (codec_context_ == nullptr ||
            decoded_frame_ == nullptr ||
            decoded_frame == nullptr ||
            data == nullptr ||
            size == 0) {
            if (error_message != nullptr) {
                *error_message = "decoder is not initialized";
            }
            return false;
        }

        *decoded_frame = DecodedFrame();
        av_frame_unref(decoded_frame_);

        AVPacket packet{};
        packet.data = const_cast<std::uint8_t *>(data);
        packet.size = static_cast<int>(size);

        const int send_result = avcodec_send_packet(codec_context_, &packet);
        if (send_result < 0) {
            if (error_message != nullptr) {
                *error_message = "avcodec_send_packet failed: " + AvErrorString(send_result);
            }
            return false;
        }

        bool produced_frame = false;
        AVFrame *owned_frame = nullptr;
        while (true) {
            const int receive_result = avcodec_receive_frame(codec_context_, decoded_frame_);
            if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) {
                break;
            }
            if (receive_result < 0) {
                if (error_message != nullptr) {
                    *error_message = "avcodec_receive_frame failed: " + AvErrorString(receive_result);
                }
                return false;
            }

            if (decoded_frame_->width <= 0 || decoded_frame_->height <= 0) {
                av_frame_unref(decoded_frame_);
                continue;
            }

            AVFrame *cloned_frame = av_frame_clone(decoded_frame_);
            av_frame_unref(decoded_frame_);
            if (cloned_frame == nullptr) {
                if (owned_frame != nullptr) {
                    av_frame_free(&owned_frame);
                }
                if (error_message != nullptr) {
                    *error_message = "failed to clone decoded frame";
                }
                return false;
            }

            if (owned_frame != nullptr) {
                av_frame_free(&owned_frame);
            }
            owned_frame = cloned_frame;
            produced_frame = true;
        }

        if (produced_frame) {
            decoded_frame->owner = MakeAvFrameOwner(owned_frame);
            CopyDecodedFrameView(*owned_frame, decoded_frame);
        }
        if (!produced_frame && error_message != nullptr) {
            *error_message = "decoder did not output a frame";
        }
        return produced_frame;
    }

    void Shutdown() override {
        if (decoded_frame_ != nullptr) {
            av_frame_free(&decoded_frame_);
        }
        if (codec_context_ != nullptr) {
            avcodec_free_context(&codec_context_);
        }

        codec_ = nullptr;
    }

    const std::string &backend_name() const override {
        return backend_name_;
    }

private:
    const AVCodec *codec_ = nullptr;
    AVCodecContext *codec_context_ = nullptr;
    AVFrame *decoded_frame_ = nullptr;
    const std::string backend_name_ = "software";
};

}  // namespace

std::unique_ptr<VideoDecoderBackend> CreateSoftwareVideoDecoderBackend() {
    return std::unique_ptr<VideoDecoderBackend>(new SoftwareVideoDecoderBackend());
}

}  // namespace sclient
