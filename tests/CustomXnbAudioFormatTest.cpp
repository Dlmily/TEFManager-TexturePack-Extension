#include "AndroidPcmPlayer.hpp"
#include "XnbSound.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
void append_u16(std::vector<std::uint8_t> &data, std::uint16_t value) {
    data.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    data.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}
void append_u32(std::vector<std::uint8_t> &data, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        data.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}
void write_custom_sound_xnb(const std::filesystem::path &output, const std::uint16_t channels,
                            const std::uint32_t sample_rate, const std::uint16_t bits) {
    const auto bytes_per_sample = static_cast<std::uint16_t>(bits / 8U);
    const auto block_align = static_cast<std::uint16_t>(channels * bytes_per_sample);
    constexpr std::uint32_t frame_count = 96;
    std::vector<std::uint8_t> waveform(frame_count * block_align);
    for (std::size_t i = 0; i < waveform.size(); ++i) {
        waveform[i] = static_cast<std::uint8_t>((i * 37U) & 0xFFU);
    }

    std::vector<std::uint8_t> body;
    const std::string reader = "Microsoft.Xna.Framework.Content.SoundEffectReader, Microsoft.Xna.Framework";
    body.push_back(1); // type reader count
    body.push_back(static_cast<std::uint8_t>(reader.size()));
    body.insert(body.end(), reader.begin(), reader.end());
    append_u32(body, 0); // reader version
    body.push_back(0);   // shared resource count
    body.push_back(1);   // primary reader index
    append_u32(body, 18);
    append_u16(body, 1); // WAVE_FORMAT_PCM
    append_u16(body, channels);
    append_u32(body, sample_rate);
    append_u32(body, sample_rate * block_align);
    append_u16(body, block_align);
    append_u16(body, bits);
    append_u16(body, 0); // cbSize
    append_u32(body, static_cast<std::uint32_t>(waveform.size()));
    body.insert(body.end(), waveform.begin(), waveform.end());
    append_u32(body, 0); // loop start
    append_u32(body, 0); // loop length
    append_u32(body, (frame_count * 1000U) / sample_rate);

    std::vector<std::uint8_t> xnb{'X', 'N', 'B', 'w', 5, 0};
    append_u32(xnb, static_cast<std::uint32_t>(10U + body.size()));
    xnb.insert(xnb.end(), body.begin(), body.end());
    std::ofstream stream(output, std::ios::binary);
    stream.write(reinterpret_cast<const char *>(xnb.data()), static_cast<std::streamsize>(xnb.size()));
}

bool test_format(const std::filesystem::path &directory, const std::string &name,
                 const std::uint16_t channels, const std::uint32_t rate, const std::uint16_t bits) {
    const auto file = directory / (name + ".xnb");
    write_custom_sound_xnb(file, channels, rate, bits);
    XnbSoundEffectData sound;
    std::string error;
    if (!ParseXnbSoundEffect(file, sound, error)) {
        std::cerr << name << " parse failed: " << error << '\n';
        return false;
    }
    if (sound.format_tag != 1 || sound.channel_count != channels || sound.sample_rate != rate ||
        sound.bits_per_sample != bits || sound.block_align != channels * (bits / 8U)) {
        std::cerr << name << " parsed metadata mismatch\n";
        return false;
    }
    if (!AndroidPcmPlayer::Instance().Register("Test/" + name, sound, error)) {
        std::cerr << name << " registration failed: " << error << '\n';
        return false;
    }
    return true;
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: CustomXnbAudioFormatTest <temporary-directory>\n";
        return 2;
    }
    const std::filesystem::path directory(argv[1]);
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::create_directories(directory);
    const bool passed = test_format(directory, "pcm8_stereo_22050", 2, 22050, 8) &&
                        test_format(directory, "pcm24_mono_44100", 1, 44100, 24) &&
                        test_format(directory, "pcm32_stereo_32000", 2, 32000, 32);
    AndroidPcmPlayer::Instance().Shutdown();
    std::filesystem::remove_all(directory, ignored);
    if (!passed) {
        return 1;
    }
    std::cout << "custom XNB PCM format tests passed\n";
    return 0;
}
