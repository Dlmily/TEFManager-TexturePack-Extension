#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

/**
 * Terraria/XNA SoundEffectReader 解析后的有效载荷。
 * format 为原始 WAVEFORMATEX 字节；其格式字段会被保留。waveform 为 SoundEffectReader 有效载荷。
 */
struct XnbSoundEffectData {
    // WAVEFORMATEX 的原始字节以及已解码字段。解析器不将这些参数写死为某一特定音效。
    std::vector<std::uint8_t> format;
    std::vector<std::uint8_t> waveform;
    std::uint16_t format_tag = 0;
    std::uint16_t channel_count = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t average_bytes_per_second = 0;
    std::uint16_t block_align = 0;
    std::uint16_t bits_per_sample = 0;
    std::int32_t loop_start = 0;
    std::int32_t loop_length = 0;
    std::int32_t duration_ms = 0;
};

/**
 * 读取未压缩 XNB 中的 XNA SoundEffectReader 有效载荷。
 * 当前实现明确拒绝 LZX 压缩 XNB，避免将压缩字节误当作 PCM 播放。
 */
bool ParseXnbSoundEffect(const std::filesystem::path &source,
                         XnbSoundEffectData &output,
                         std::string &error);
