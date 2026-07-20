#ifndef SSERVER_MODULES_ENCODING_VIDEO_X264_X264VIDEOENCODERBACKEND_H
#define SSERVER_MODULES_ENCODING_VIDEO_X264_X264VIDEOENCODERBACKEND_H

#include <memory>
#include <string>

#include "modules/encoding/video/VideoEncoderBackend.h"
#include "x264.h"

namespace sserver {
namespace modules {
namespace encoding {

std::unique_ptr<VideoEncoderBackend> CreateX264VideoEncoderBackend();

class X264VideoEncoderBackend final : public VideoEncoderBackend {
public:
    X264VideoEncoderBackend();
    ~X264VideoEncoderBackend() override;

    bool Initialize(int width, int height, int fps, const config::CodecConfig &config, std::string *error_message) override;
    bool EncodeYuyv422Frame(
            const std::uint8_t *input,
            std::size_t input_length,
            std::vector<std::uint8_t> *output,
            bool *is_keyframe,
            std::string *error_message) override;
    void Shutdown() override;
    EncodeBackend backend() const override;
    const std::string &backend_name() const override;

private:
    std::string backend_name_;
    x264_param_t *param_;
    x264_t *handle_;
    x264_picture_t *picture_;
    x264_nal_t *nal_;
    int pts_;
};

}  // namespace encoding
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_ENCODING_VIDEO_X264_X264VIDEOENCODERBACKEND_H
