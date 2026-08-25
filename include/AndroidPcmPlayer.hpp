#pragma once

#include "XnbSound.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class AndroidPcmPlayer {
public:
    static AndroidPcmPlayer &Instance();

    bool Initialize(std::string &error);
    bool Register(const std::string &key, const XnbSoundEffectData &sound, std::string &error);
    bool Has(const std::string &key) const;
    // pitch_offset 使用 Terraria/XNA SoundStyle.Pitch 语义：0 为原始音调，±1 为一个八度。
    bool Play(const std::string &key, float pitch_offset, std::string &error);
    bool Play(const std::string &key, std::string &error) { return Play(key, 0.0F, error); }
    void Shutdown();

    // OpenSL ES 缓冲队列回调的上下文类型；定义保持在实现文件。
    struct ActivePlayback;

private:
    struct RegisteredSound {
        std::vector<std::uint8_t> pcm;
        int sample_rate = 0;
        int channel_count = 0;
        int bits_per_sample = 0;
    };
    AndroidPcmPlayer() = default;
    ~AndroidPcmPlayer();
    AndroidPcmPlayer(const AndroidPcmPlayer &) = delete;
    AndroidPcmPlayer &operator=(const AndroidPcmPlayer &) = delete;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, RegisteredSound> sounds_;
    std::vector<std::unique_ptr<ActivePlayback>> active_playbacks_;
    const void *engine_object_ = nullptr;
    const void *engine_interface_ = nullptr;
    const void *output_mix_object_ = nullptr;
    bool initialized_ = false;
};
