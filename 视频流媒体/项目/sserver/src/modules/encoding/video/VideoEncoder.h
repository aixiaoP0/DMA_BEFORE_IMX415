#ifndef SSERVER_MODULES_ENCODING_VIDEO_VIDEOENCODER_H
#define SSERVER_MODULES_ENCODING_VIDEO_VIDEOENCODER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "config/AppConfig.h"

namespace sserver {
namespace modules {
namespace encoding {

class VideoEncoderBackend;

enum class EncodeBackend {
    kAuto,
    kX264,
};

struct VideoEncoderBackendSelection {
    EncodeBackend backend = EncodeBackend::kAuto;
    std::string backend_name = "auto";
};

inline bool ResolveVideoEncoderBackendSelection(
        const config::CodecConfig &config,
        VideoEncoderBackendSelection *selection,
        std::string *error_message) {
    if (selection == nullptr) {
        if (error_message != nullptr) {
            *error_message = "video encoder backend selection output is null";
        }
        return false;
    }

    if (config.backend.empty()) {
        if (error_message != nullptr) {
            *error_message = "codec.backend must not be empty";
        }
        return false;
    }

    if (config.backend == "x264") {
        selection->backend = EncodeBackend::kX264;
        selection->backend_name = "x264";
        return true;
    }

    if (error_message != nullptr) {
        if (config.backend == "libx264") {
            *error_message = "codec.backend=libx264 is no longer supported; use codec.backend=x264 for software encoding";
        } else {
            *error_message = "codec.backend must be 'x264'";
        }
    }
    return false;
}

class VideoEncoderBackendFactory {
public:
    static std::unique_ptr<VideoEncoderBackend> Create(
            const VideoEncoderBackendSelection &selection,
            std::string *error_message);
};

class VideoEncoder {
public:
    VideoEncoder();
    ~VideoEncoder();

    bool Initialize(int width, int height, int fps, const config::CodecConfig &config);
    bool Initialize(int width, int height, int fps, const config::CodecConfig &config, std::string *error_message);
    bool EncodeYuyv422Frame(
            const std::uint8_t *input,
            std::size_t input_length,
            std::vector<std::uint8_t> *output,
            bool *is_keyframe);
    bool EncodeYuyv422Frame(
            const std::uint8_t *input,
            std::size_t input_length,
            std::vector<std::uint8_t> *output,
            bool *is_keyframe,
            std::string *error_message);
    void Shutdown();

    EncodeBackend backend() const;
    const std::string &backend_name() const;

private:
    struct VideoEncoderImpl;
    std::unique_ptr<VideoEncoderImpl> impl_;
};

using IVideoEncoder = VideoEncoder;

class CodecFactory {
public:
    static std::unique_ptr<IVideoEncoder> Create(const config::CodecConfig &config);
};

}  // namespace encoding
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_ENCODING_VIDEO_VIDEOENCODER_H
