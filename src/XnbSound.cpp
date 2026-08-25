#include "XnbSound.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>

namespace {
constexpr std::size_t kXnbHeaderSize = 10;
constexpr std::size_t kMaxXnbBytes = 32U * 1024U * 1024U;

bool read_u16_at(const std::vector<std::uint8_t> &data, const std::size_t offset, std::uint16_t &value) {
    if (offset > data.size() || data.size() - offset < 2) {
        return false;
    }
    value = static_cast<std::uint16_t>(data[offset]) |
            (static_cast<std::uint16_t>(data[offset + 1]) << 8U);
    return true;
}

bool read_u32_at(const std::vector<std::uint8_t> &data, const std::size_t offset, std::uint32_t &value) {
    if (offset > data.size() || data.size() - offset < 4) {
        return false;
    }
    value = static_cast<std::uint32_t>(data[offset]) |
            (static_cast<std::uint32_t>(data[offset + 1]) << 8U) |
            (static_cast<std::uint32_t>(data[offset + 2]) << 16U) |
            (static_cast<std::uint32_t>(data[offset + 3]) << 24U);
    return true;
}

bool read_u32(const std::vector<std::uint8_t> &data, std::size_t &cursor, std::uint32_t &value) {
    if (cursor > data.size() || data.size() - cursor < 4) {
        return false;
    }
    value = static_cast<std::uint32_t>(data[cursor]) |
            (static_cast<std::uint32_t>(data[cursor + 1]) << 8U) |
            (static_cast<std::uint32_t>(data[cursor + 2]) << 16U) |
            (static_cast<std::uint32_t>(data[cursor + 3]) << 24U);
    cursor += 4;
    return true;
}

bool read_i32(const std::vector<std::uint8_t> &data, std::size_t &cursor, std::int32_t &value) {
    std::uint32_t unsigned_value = 0;
    if (!read_u32(data, cursor, unsigned_value)) {
        return false;
    }
    value = static_cast<std::int32_t>(unsigned_value);
    return true;
}

bool read_7bit_int(const std::vector<std::uint8_t> &data, std::size_t &cursor, std::uint32_t &value) {
    value = 0;
    for (unsigned shift = 0; shift < 35; shift += 7) {
        if (cursor >= data.size()) {
            return false;
        }
        const auto byte = data[cursor++];
        value |= static_cast<std::uint32_t>(byte & 0x7FU) << shift;
        if ((byte & 0x80U) == 0) {
            return true;
        }
    }
    return false;
}

bool skip_bytes(const std::vector<std::uint8_t> &data, std::size_t &cursor, std::size_t count) {
    if (cursor > data.size() || count > data.size() - cursor) {
        return false;
    }
    cursor += count;
    return true;
}
} // namespace

bool ParseXnbSoundEffect(const std::filesystem::path &source,
                         XnbSoundEffectData &output,
                         std::string &error) {
    output = {};
    error.clear();
    std::error_code filesystem_error;
    const auto byte_count = std::filesystem::file_size(source, filesystem_error);
    if (filesystem_error || byte_count < kXnbHeaderSize || byte_count > kMaxXnbBytes) {
        error = "XNB file size is invalid";
        return false;
    }

    std::ifstream input(source, std::ios::binary);
    if (!input.is_open()) {
        error = "cannot open XNB file";
        return false;
    }
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (data.size() != byte_count || data[0] != 'X' || data[1] != 'N' || data[2] != 'B') {
        error = "not an XNB file";
        return false;
    }
    const std::uint8_t flags = data[5];
    if ((flags & 0x80U) != 0) {
        error = "compressed XNB is not supported";
        return false;
    }

    std::size_t cursor = 6;
    std::uint32_t declared_size = 0;
    if (!read_u32(data, cursor, declared_size) || declared_size != data.size()) {
        error = "XNB declared size does not match file";
        return false;
    }

    std::uint32_t reader_count = 0;
    if (!read_7bit_int(data, cursor, reader_count) || reader_count == 0 || reader_count > 64) {
        error = "XNB type-reader table is invalid";
        return false;
    }
    bool saw_sound_reader = false;
    for (std::uint32_t index = 0; index < reader_count; ++index) {
        std::uint32_t name_size = 0;
        if (!read_7bit_int(data, cursor, name_size) || name_size > 4096 || !skip_bytes(data, cursor, name_size)) {
            error = "XNB type-reader name is invalid";
            return false;
        }
        const std::size_t name_offset = cursor - name_size;
        const std::string reader_name(reinterpret_cast<const char *>(data.data() + name_offset), name_size);
        saw_sound_reader = saw_sound_reader || reader_name.find("SoundEffectReader") != std::string::npos;
        std::uint32_t reader_version = 0;
        if (!read_u32(data, cursor, reader_version)) {
            error = "XNB type-reader version is missing";
            return false;
        }
    }
    if (!saw_sound_reader) {
        error = "XNB does not contain SoundEffectReader";
        return false;
    }

    std::uint32_t shared_resource_count = 0;
    std::uint32_t primary_reader_index = 0;
    if (!read_7bit_int(data, cursor, shared_resource_count) ||
        !read_7bit_int(data, cursor, primary_reader_index) || primary_reader_index == 0) {
        error = "XNB primary object header is invalid";
        return false;
    }

    std::uint32_t format_size = 0;
    if (!read_u32(data, cursor, format_size) || format_size < 16 || format_size > 256 ||
        !skip_bytes(data, cursor, format_size)) {
        error = "SoundEffect WAVEFORMATEX is invalid";
        return false;
    }
    const auto format_begin = cursor - format_size;
    output.format.assign(data.begin() + static_cast<std::ptrdiff_t>(format_begin),
                         data.begin() + static_cast<std::ptrdiff_t>(cursor));
    if (!read_u16_at(output.format, 0, output.format_tag) ||
        !read_u16_at(output.format, 2, output.channel_count) ||
        !read_u32_at(output.format, 4, output.sample_rate) ||
        !read_u32_at(output.format, 8, output.average_bytes_per_second) ||
        !read_u16_at(output.format, 12, output.block_align) ||
        !read_u16_at(output.format, 14, output.bits_per_sample) ||
        output.channel_count == 0 || output.sample_rate == 0 || output.block_align == 0) {
        output = {};
        error = "SoundEffect WAVEFORMATEX fields are invalid";
        return false;
    }

    std::uint32_t wave_size = 0;
    if (!read_u32(data, cursor, wave_size) || wave_size == 0 || wave_size > kMaxXnbBytes ||
        !skip_bytes(data, cursor, wave_size)) {
        output = {};
        error = "SoundEffect waveform is invalid";
        return false;
    }
    const auto wave_begin = cursor - wave_size;
    output.waveform.assign(data.begin() + static_cast<std::ptrdiff_t>(wave_begin),
                           data.begin() + static_cast<std::ptrdiff_t>(cursor));
    // 对 PCM，数据必须完整地由样本帧组成。非 PCM 的编码对齐语义属于格式自身，
    // 仍予以保留并由播放后端明确诊断，而不是把字节错误解释为 PCM。
    if (output.format_tag == 1 && (output.waveform.size() % output.block_align) != 0U) {
        output = {};
        error = "PCM waveform is not block-aligned";
        return false;
    }

    // loopStart、loopLength、duration 是 SoundEffectReader 的固定尾部。
    if (!read_i32(data, cursor, output.loop_start) ||
        !read_i32(data, cursor, output.loop_length) ||
        !read_i32(data, cursor, output.duration_ms)) {
        output = {};
        error = "SoundEffect loop metadata is missing";
        return false;
    }
    return true;
}
