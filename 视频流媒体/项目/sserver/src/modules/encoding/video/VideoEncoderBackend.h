#ifndef SSERVER_MODULES_ENCODING_VIDEO_VIDEOENCODERBACKEND_H
#define SSERVER_MODULES_ENCODING_VIDEO_VIDEOENCODERBACKEND_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "config/AppConfig.h"
#include "modules/encoding/video/VideoEncoder.h"

namespace sserver {
namespace modules {
namespace encoding {

class VideoEncoderBackend {
public:
    virtual ~VideoEncoderBackend() = default;

    virtual bool Initialize(int width, int height, int fps, const config::CodecConfig &config, std::string *error_message) = 0;
    virtual bool EncodeYuyv422Frame(
            const std::uint8_t *input,
            std::size_t input_length,
            std::vector<std::uint8_t> *output,
            bool *is_keyframe,
            std::string *error_message) = 0;
    virtual void Shutdown() = 0;
    virtual EncodeBackend backend() const = 0;
    virtual const std::string &backend_name() const = 0;
};

}  // namespace encoding
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_ENCODING_VIDEO_VIDEOENCODERBACKEND_H
