#ifndef SSERVER_COMMON_NET_H264ANNEXB_H
#define SSERVER_COMMON_NET_H264ANNEXB_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sserver {
namespace common {
namespace net {

struct H264NaluView {
    const std::uint8_t *data = nullptr;
    std::size_t size = 0;

    std::uint8_t nal_type() const {
        if (data == nullptr || size == 0) {
            return 0;
        }
        return static_cast<std::uint8_t>(data[0] & 0x1F);
    }
};

inline std::size_t FindAnnexBStartCode(
        const std::vector<std::uint8_t> &data,
        std::size_t offset,
        std::size_t *prefix_size) {
    if (prefix_size != nullptr) {
        *prefix_size = 0;
    }

    for (std::size_t index = offset; index + 3 <= data.size(); ++index) {
        if (data[index] != 0 || data[index + 1] != 0) {
            continue;
        }
        if (data[index + 2] == 1) {
            if (prefix_size != nullptr) {
                *prefix_size = 3;
            }
            return index;
        }
        if (index + 4 <= data.size() && data[index + 2] == 0 && data[index + 3] == 1) {
            if (prefix_size != nullptr) {
                *prefix_size = 4;
            }
            return index;
        }
    }

    return data.size();
}

inline void SplitAnnexBNalus(const std::vector<std::uint8_t> &data, std::vector<H264NaluView> *output) {
    if (output == nullptr) {
        return;
    }
    output->clear();
    std::size_t start_code_size = 0;
    std::size_t start = FindAnnexBStartCode(data, 0, &start_code_size);
    while (start < data.size()) {
        const std::size_t payload_start = start + start_code_size;
        std::size_t next_start_code_size = 0;
        const std::size_t next = FindAnnexBStartCode(data, payload_start, &next_start_code_size);
        std::size_t payload_end = next;
        while (payload_end > payload_start && data[payload_end - 1] == 0) {
            --payload_end;
        }
        if (payload_end > payload_start) {
            H264NaluView view;
            view.data = data.data() + payload_start;
            view.size = payload_end - payload_start;
            output->push_back(view);
        }
        start = next;
        start_code_size = next_start_code_size;
    }
}

inline std::vector<H264NaluView> SplitAnnexBNalus(const std::vector<std::uint8_t> &data) {
    std::vector<H264NaluView> nalus;
    SplitAnnexBNalus(data, &nalus);
    return nalus;
}

inline void AppendAnnexBStartCode(std::vector<std::uint8_t> *output) {
    if (output == nullptr) {
        return;
    }

    output->push_back(0);
    output->push_back(0);
    output->push_back(0);
    output->push_back(1);
}

inline void AppendAnnexBNalu(
        const std::uint8_t *data,
        std::size_t size,
        std::vector<std::uint8_t> *output) {
    if (data == nullptr || size == 0 || output == nullptr) {
        return;
    }

    AppendAnnexBStartCode(output);
    output->insert(output->end(), data, data + size);
}

}  // namespace net
}  // namespace common
}  // namespace sserver

#endif  // SSERVER_COMMON_NET_H264ANNEXB_H
