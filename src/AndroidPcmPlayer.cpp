#include "AndroidPcmPlayer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

#if defined(__ANDROID__)
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#endif

namespace {
std::uint16_t read_u16(const std::vector<std::uint8_t> &value, const std::size_t offset) {
    return static_cast<std::uint16_t>(value[offset]) |
           (static_cast<std::uint16_t>(value[offset + 1]) << 8U);
}

std::uint32_t read_u32(const std::vector<std::uint8_t> &value, const std::size_t offset) {
    return static_cast<std::uint32_t>(value[offset]) |
           (static_cast<std::uint32_t>(value[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(value[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(value[offset + 3]) << 24U);
}

#if defined(__ANDROID__)
SLObjectItf as_object(const void *value) {
    return reinterpret_cast<SLObjectItf>(const_cast<void *>(value));
}
SLEngineItf as_engine(const void *value) {
    return reinterpret_cast<SLEngineItf>(const_cast<void *>(value));
}
#endif
} // namespace

struct AndroidPcmPlayer::ActivePlayback {
#if defined(__ANDROID__)
    SLObjectItf player_object = nullptr;
    SLAndroidSimpleBufferQueueItf queue = nullptr;
#endif
    bool finished = false;
};

#if defined(__ANDROID__)
void SLAPIENTRY playback_finished(SLAndroidSimpleBufferQueueItf, void *context) {
    auto *playback = static_cast<AndroidPcmPlayer::ActivePlayback *>(context);
    playback->finished = true;
}
#endif

AndroidPcmPlayer &AndroidPcmPlayer::Instance() {
    static AndroidPcmPlayer instance;
    return instance;
}

AndroidPcmPlayer::~AndroidPcmPlayer() {
    Shutdown();
}

bool AndroidPcmPlayer::Initialize(std::string &error) {
    std::lock_guard lock(mutex_);
    error.clear();
    if (initialized_) {
        return true;
    }
#if !defined(__ANDROID__)
    error = "OpenSL ES is only available on Android";
    return false;
#else
    SLObjectItf engine = nullptr;
    if (slCreateEngine(&engine, 0, nullptr, 0, nullptr, nullptr) != SL_RESULT_SUCCESS ||
        (*engine)->Realize(engine, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) {
        if (engine != nullptr) {
            (*engine)->Destroy(engine);
        }
        error = "failed to initialize OpenSL ES engine";
        return false;
    }
    SLEngineItf engine_interface = nullptr;
    if ((*engine)->GetInterface(engine, SL_IID_ENGINE, &engine_interface) != SL_RESULT_SUCCESS) {
        (*engine)->Destroy(engine);
        error = "failed to get OpenSL ES engine interface";
        return false;
    }
    SLObjectItf mix = nullptr;
    if ((*engine_interface)->CreateOutputMix(engine_interface, &mix, 0, nullptr, nullptr) != SL_RESULT_SUCCESS ||
        (*mix)->Realize(mix, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) {
        if (mix != nullptr) {
            (*mix)->Destroy(mix);
        }
        (*engine)->Destroy(engine);
        error = "failed to create OpenSL ES output mix";
        return false;
    }
    engine_object_ = reinterpret_cast<const void *>(engine);
    engine_interface_ = reinterpret_cast<const void *>(engine_interface);
    output_mix_object_ = reinterpret_cast<const void *>(mix);
    initialized_ = true;
    return true;
#endif
}

bool AndroidPcmPlayer::Register(const std::string &key, const XnbSoundEffectData &sound, std::string &error) {
    error.clear();
    if (key.empty() || sound.format.size() < 16 || sound.waveform.empty()) {
        error = "SoundEffect data is incomplete";
        return false;
    }
    const auto format_tag = read_u16(sound.format, 0);
    const auto channels = read_u16(sound.format, 2);
    const auto sample_rate = read_u32(sound.format, 4);
    const auto bits = read_u16(sound.format, 14);
    if (format_tag != 1 || channels != 1 || sample_rate != 48000 || bits != 16 ||
        (sound.waveform.size() % 2U) != 0U) {
        error = "only 16-bit PCM, 48 kHz, mono SoundEffect XNB is supported";
        return false;
    }
    std::lock_guard lock(mutex_);
    sounds_[key] = RegisteredSound{.pcm = sound.waveform,
                                  .sample_rate = static_cast<int>(sample_rate),
                                  .channel_count = static_cast<int>(channels),
                                  .bits_per_sample = static_cast<int>(bits)};
    return true;
}

bool AndroidPcmPlayer::Has(const std::string &key) const {
    std::lock_guard lock(mutex_);
    return sounds_.contains(key);
}

bool AndroidPcmPlayer::Play(const std::string &key, float pitch_offset, std::string &error) {
    std::lock_guard lock(mutex_);
    error.clear();
    const auto sound = sounds_.find(key);
    if (sound == sounds_.end()) {
        error = "PCM resource is not registered";
        return false;
    }
    if (!std::isfinite(pitch_offset)) {
        error = "pitch offset is not finite";
        return false;
    }
    pitch_offset = std::clamp(pitch_offset, -1.0F, 1.0F);
    const auto playback_rate = static_cast<int>(std::lround(
        static_cast<float>(sound->second.sample_rate) * std::exp2(pitch_offset)));
    if (playback_rate < 8000 || playback_rate > 192000) {
        error = "pitch-derived sample rate is unsupported";
        return false;
    }
#if !defined(__ANDROID__)
    error = "OpenSL ES is only available on Android";
    return false;
#else
    if (!initialized_ || engine_interface_ == nullptr || output_mix_object_ == nullptr) {
        error = "OpenSL ES player is not initialized";
        return false;
    }

    for (auto iterator = active_playbacks_.begin(); iterator != active_playbacks_.end();) {
        if ((*iterator)->finished) {
            if ((*iterator)->player_object != nullptr) {
                (*(*iterator)->player_object)->Destroy((*iterator)->player_object);
            }
            iterator = active_playbacks_.erase(iterator);
        } else {
            ++iterator;
        }
    }

    auto playback = std::make_unique<ActivePlayback>();
    SLDataLocator_AndroidSimpleBufferQueue queue_locator{SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 1};
    SLDataFormat_PCM pcm_format{SL_DATAFORMAT_PCM,
                                 static_cast<SLuint32>(sound->second.channel_count),
                                 static_cast<SLuint32>(playback_rate * 1000),
                                 static_cast<SLuint32>(sound->second.bits_per_sample),
                                 static_cast<SLuint32>(sound->second.bits_per_sample),
                                 SL_SPEAKER_FRONT_CENTER,
                                 SL_BYTEORDER_LITTLEENDIAN};
    SLDataSource source{&queue_locator, &pcm_format};
    SLDataLocator_OutputMix output_locator{SL_DATALOCATOR_OUTPUTMIX, as_object(output_mix_object_)};
    SLDataSink sink{&output_locator, nullptr};
    const SLInterfaceID interfaces[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
    const SLboolean required[] = {SL_BOOLEAN_TRUE};
    auto engine = as_engine(engine_interface_);
    if ((*engine)->CreateAudioPlayer(engine, &playback->player_object, &source, &sink, 1, interfaces, required) != SL_RESULT_SUCCESS ||
        (*playback->player_object)->Realize(playback->player_object, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) {
        if (playback->player_object != nullptr) {
            (*playback->player_object)->Destroy(playback->player_object);
        }
        error = "failed to create OpenSL ES PCM player";
        return false;
    }
    SLPlayItf play_interface = nullptr;
    if ((*playback->player_object)->GetInterface(playback->player_object, SL_IID_PLAY, &play_interface) != SL_RESULT_SUCCESS ||
        (*playback->player_object)->GetInterface(playback->player_object, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &playback->queue) != SL_RESULT_SUCCESS ||
        (*playback->queue)->RegisterCallback(playback->queue, playback_finished, playback.get()) != SL_RESULT_SUCCESS ||
        (*playback->queue)->Enqueue(playback->queue, sound->second.pcm.data(),
                                    static_cast<SLuint32>(sound->second.pcm.size())) != SL_RESULT_SUCCESS ||
        (*play_interface)->SetPlayState(play_interface, SL_PLAYSTATE_PLAYING) != SL_RESULT_SUCCESS) {
        (*playback->player_object)->Destroy(playback->player_object);
        error = "failed to enqueue OpenSL ES PCM buffer";
        return false;
    }
    active_playbacks_.push_back(std::move(playback));
    return true;
#endif
}

void AndroidPcmPlayer::Shutdown() {
    std::lock_guard lock(mutex_);
#if defined(__ANDROID__)
    for (auto &playback : active_playbacks_) {
        if (playback->player_object != nullptr) {
            (*playback->player_object)->Destroy(playback->player_object);
        }
    }
    if (output_mix_object_ != nullptr) {
        (*as_object(output_mix_object_))->Destroy(as_object(output_mix_object_));
    }
    if (engine_object_ != nullptr) {
        (*as_object(engine_object_))->Destroy(as_object(engine_object_));
    }
#endif
    active_playbacks_.clear();
    sounds_.clear();
    output_mix_object_ = nullptr;
    engine_interface_ = nullptr;
    engine_object_ = nullptr;
    initialized_ = false;
}
