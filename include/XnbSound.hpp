#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

/**
 * Terraria/XNA SoundEffectReader 解析后的有效载荷。
 * format 为原始 WAVEFORMATEX 字节，waveform 为未压缩 PCM 数据。
 */
struct XnbSoundEffectData {
    std::vector<std::uint8_t> format;
    std::vector<std::uint8_t> waveform;
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
