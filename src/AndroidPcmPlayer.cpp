#include "AndroidPcmPlayer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

#if defined(__ANDROID__)
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#endif

namespace {
constexpr std::size_t kMaxNormalizedPcmBytes = 64U * 1024U * 1024U;
constexpr std::array<int, 9> kOpenSlSampleRates = {
    8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000,
};

int nearest_open_sl_sample_rate(const int rate) {
    auto closest = kOpenSlSampleRates.front();
    auto distance = std::abs(rate - closest);
    for (const auto candidate : kOpenSlSampleRates) {
        const auto candidate_distance = std::abs(rate - candidate);
        if (candidate_distance < distance) {
            closest = candidate;
            distance = candidate_distance;
        }
    }
    return closest;
}

std::int16_t decode_pcm_sample(const XnbSoundEffectData &sound, const std::size_t frame,
                               const std::size_t channel, const std::size_t bytes_per_sample) {
    const std::size_t offset = frame * sound.block_align + channel * bytes_per_sample;
    const auto *bytes = sound.waveform.data() + offset;
    switch (sound.bits_per_sample) {
        case 8:
            return static_cast<std::int16_t>((static_cast<int>(bytes[0]) - 128) * 256);
        case 16: {
            const auto value = static_cast<std::int16_t>(static_cast<std::uint16_t>(bytes[0]) |
                                                         (static_cast<std::uint16_t>(bytes[1]) << 8U));
            return value;
        }
        case 24: {
            std::int32_t value = static_cast<std::int32_t>(bytes[0]) |
                                 (static_cast<std::int32_t>(bytes[1]) << 8U) |
                                 (static_cast<std::int32_t>(bytes[2]) << 16U);
            if ((value & 0x00800000) != 0) {
                value |= static_cast<std::int32_t>(0xFF000000U);
            }
            return static_cast<std::int16_t>(value >> 8);
        }
        case 32: {
            const auto value = static_cast<std::int32_t>(static_cast<std::uint32_t>(bytes[0]) |
                                                         (static_cast<std::uint32_t>(bytes[1]) << 8U) |
                                                         (static_cast<std::uint32_t>(bytes[2]) << 16U) |
                                                         (static_cast<std::uint32_t>(bytes[3]) << 24U));
            return static_cast<std::int16_t>(value >> 16);
        }
        default:
            return 0;
    }
}

bool normalize_pcm_for_open_sl(const XnbSoundEffectData &sound, std::vector<std::uint8_t> &pcm,
                               int &output_rate, std::string &error) {
    // WAVE_FORMAT_PCM。压缩、浮点、ADPCM 等数据不能直接放进 Android simple buffer queue。
    if (sound.format_tag != 1) {
        error = "only uncompressed WAVE_FORMAT_PCM XNB audio is supported";
        return false;
    }
    if (sound.channel_count != 1 && sound.channel_count != 2) {
        error = "only mono or stereo PCM XNB audio is supported";
        return false;
    }
    if (sound.bits_per_sample != 8 && sound.bits_per_sample != 16 &&
        sound.bits_per_sample != 24 && sound.bits_per_sample != 32) {
        error = "only 8/16/24/32-bit PCM XNB audio is supported";
        return false;
    }
    const auto bytes_per_sample = static_cast<std::size_t>(sound.bits_per_sample / 8);
    const auto expected_block_align = static_cast<std::size_t>(sound.channel_count) * bytes_per_sample;
    if (sound.block_align != expected_block_align || sound.waveform.size() % sound.block_align != 0U) {
        error = "PCM block alignment does not match WAVEFORMATEX";
        return false;
    }
    const auto input_frames = sound.waveform.size() / sound.block_align;
    if (input_frames == 0 || input_frames > (std::numeric_limits<std::size_t>::max() / 4U)) {
        error = "PCM frame count is invalid";
        return false;
    }
    if (sound.sample_rate > 192000U) {
        error = "PCM sample rate exceeds supported conversion limit";
        return false;
    }

    output_rate = nearest_open_sl_sample_rate(std::clamp(static_cast<int>(sound.sample_rate), 8000, 48000));
    const auto output_frames = std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(
        static_cast<long double>(input_frames) * output_rate / sound.sample_rate)));
    if (output_frames > (std::numeric_limits<std::size_t>::max() / (sound.channel_count * 2U)) ||
        output_frames * sound.channel_count * 2U > kMaxNormalizedPcmBytes) {
        error = "resampled PCM is too large";
        return false;
    }
    pcm.resize(output_frames * sound.channel_count * 2U);
    for (std::size_t output_frame = 0; output_frame < output_frames; ++output_frame) {
        const auto source_position = static_cast<long double>(output_frame) * sound.sample_rate / output_rate;
        const auto source_frame_a = std::min<std::size_t>(static_cast<std::size_t>(source_position), input_frames - 1U);
        const auto source_frame_b = std::min(source_frame_a + 1U, input_frames - 1U);
        const auto fraction = source_position - static_cast<long double>(source_frame_a);
        for (std::size_t channel = 0; channel < sound.channel_count; ++channel) {
            const auto a = decode_pcm_sample(sound, source_frame_a, channel, bytes_per_sample);
            const auto b = decode_pcm_sample(sound, source_frame_b, channel, bytes_per_sample);
            const auto mixed = static_cast<std::int32_t>(std::lround(
                static_cast<long double>(a) + (static_cast<long double>(b) - a) * fraction));
            const auto sample = static_cast<std::int16_t>(std::clamp(
                mixed, static_cast<std::int32_t>(std::numeric_limits<std::int16_t>::min()),
                static_cast<std::int32_t>(std::numeric_limits<std::int16_t>::max())));
            const auto offset = (output_frame * sound.channel_count + channel) * 2U;
            pcm[offset] = static_cast<std::uint8_t>(sample & 0xFF);
            pcm[offset + 1U] = static_cast<std::uint8_t>((static_cast<std::uint16_t>(sample) >> 8U) & 0xFFU);
        }
    }
    return true;
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
    std::vector<std::uint8_t> normalized_pcm;
    int normalized_rate = 0;
    if (!normalize_pcm_for_open_sl(sound, normalized_pcm, normalized_rate, error)) {
        return false;
    }
    std::lock_guard lock(mutex_);
    // 统一输出为 OpenSL 兼容的 16-bit PCM；声道和实际数据均来自上传 XNB 的 WAVEFORMATEX。
    sounds_[key] = RegisteredSound{.pcm = std::move(normalized_pcm),
                                  .sample_rate = normalized_rate,
                                  .channel_count = static_cast<int>(sound.channel_count),
                                  .bits_per_sample = 16};
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
    const auto requested_rate = static_cast<int>(std::lround(
        static_cast<float>(sound->second.sample_rate) * std::exp2(pitch_offset)));
    const auto playback_rate = nearest_open_sl_sample_rate(std::clamp(requested_rate, 8000, 48000));
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
                                 sound->second.channel_count == 1
                                     ? SL_SPEAKER_FRONT_CENTER
                                     : (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT),
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
