/*******************************************************************************
 * texturepack_extension - core
 * Copyright (C) 2026 eternalfuture-e38299
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * Author: eternalfuture-e38299
 * GitHub: https://github.com/eternalfuture-e38299
 * Created: 2026/6/7
 *******************************************************************************/

#include "tefkernel-cpp-wrapper/tefkernel/module/module_core.h"
#include "core.hpp"
#include "AndroidPcmPlayer.hpp"
#include "SoundPack.hpp"
#include "TextureManager.hpp"
#include "XnbSound.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Log.hpp"
#include "TexturePack.hpp"
#include "lib/json.hpp"

#include "tefkernel-cpp-wrapper/patchlib/type.hpp"
#include "tefkernel-cpp-wrapper/patchlib/method.hpp"
#include "tefkernel-cpp-wrapper/patchlib/field.hpp"
#include "tefkernel-cpp-wrapper/patchlib/struct/string.hpp"
#include "tefkernel-cpp-wrapper/patchlib/struct/array.hpp"
#include "tefkernel-cpp-wrapper/patchlib/property.hpp"
#include "tefkernel-cpp-wrapper/tefkernel/terraria/texture2d.h"


static constexpr module_info_t g_module_info = {
    .pkg_id = "eternal.future.texturepackextension", // 唯一包名
    .name = "TexturePackExtension", // 插件名称
    .author = "eternalfuture-e38299", // 作者
    .version = "1.15.1", // 版本
    .version_code = 39, // 版本代码
    .api_version = 1, // API版本
    .plugin_dependencies_sizes = 0, // 依赖插件数组大小（如需依赖请修改）
    .plugin_dependencies = nullptr, // 依赖插件列表
};

/**
 * @brief 读取 TEFManager BasePackManager 格式的配置。
 *
 * TexturePackManager 和 AudioManager 使用相同的 config.json 架构，只是资源 ZIP
 * 分别存放在 texture_packs / audio_packs。声音 XNB 包由 AudioManager 管理，但
 * 运行时仍由本模块加载，因此需要兼容两个目录。
 */
static bool load_json(const std::filesystem::path &path,
                      const std::string &packSubDirectory,
                      const bool enabledOnly,
                      std::vector<PackEntry> &output) {
    try {
        std::ifstream file(path);
        if (!file.is_open()) {
            LOGW("Warning: Cannot open %s\n", path.c_str());
            return false;
        }

        nlohmann::json j;
        file >> j;

        if (!j.is_array()) {
            LOGE("Error: JSON root must be an array\n");
            return false;
        }

        // 材质索引仅载入 enable=true；声音索引额外支持默认关闭的标准 XNB 包。
        // TEFManager 新装资源包默认写为 enable=false，若仍用同一开关过滤声音，
        // 纯音效包会在首次启动时完全没有机会被索引。
        std::vector<PackEntry> temp;

        for (const auto &item: j) {
            if (enabledOnly && !item.value("enable", false)) {
                continue;
            }
            PackEntry entry;
            entry.file = path.parent_path() / packSubDirectory / item.value("file", "");
            entry.priority = item.value("priority", 0);
            entry.type = static_cast<PackType>(item.value("type", 0));
            temp.push_back(entry);
        }

        // 按优先级排序
        std::ranges::sort(temp,
                          [](const PackEntry &a, const PackEntry &b) {
                              return a.priority < b.priority;
                          });

        output.insert(output.end(),
                      std::make_move_iterator(temp.begin()),
                      std::make_move_iterator(temp.end()));
        return true;
    } catch (const nlohmann::json::parse_error &e) {
        LOGE("JSON parse error in %s: %s\n", path.c_str(), e.what());
        return false;
    } catch (const std::exception &e) {
        LOGE("Error reading %s: %s\n", path.c_str(), e.what());
        return false;
    }
}

static bool has_zip_extension(const std::filesystem::path &path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return extension == ".zip";
}

/**
 * AudioManager 的实际资源包可能位于当前模块配置之外。仅扫描 TEFManager 自身的
 * 应用 files 根目录，深度、归档数量与单文件大小均受限，不跟随符号链接；候选仍须通过
 * SoundPack 的 Content/Sounds 目录中 XNB 文件与路径安全校验才会被真正提取。
 */
static void discover_sound_archives(const std::filesystem::path &privateRoot,
                                    std::vector<PackEntry> &entries) {
    std::unordered_set<std::string> known;
    for (const auto &entry : entries) {
        known.insert(std::filesystem::path(entry.file).lexically_normal().generic_string());
    }

    constexpr size_t MAX_DISCOVERED_ARCHIVES = 64;
    constexpr std::uintmax_t MAX_DISCOVERED_ARCHIVE_BYTES = 256U * 1024U * 1024U;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        privateRoot, std::filesystem::directory_options::skip_permission_denied, error);
    if (error) {
        LOGW("Unable to scan module private root for sound packs: %s", error.message().c_str());
        return;
    }
    const std::filesystem::recursive_directory_iterator end;
    for (; iterator != end; iterator.increment(error)) {
        if (error) {
            LOGW("Skipping unreadable sound-pack candidate: %s", error.message().c_str());
            error.clear();
            continue;
        }
        if (iterator.depth() > 6) {
            iterator.disable_recursion_pending();
            continue;
        }
        if (!iterator->is_regular_file(error) || error || !has_zip_extension(iterator->path())) {
            error.clear();
            continue;
        }
        const auto bytes = iterator->file_size(error);
        if (error || bytes == 0 || bytes > MAX_DISCOVERED_ARCHIVE_BYTES) {
            error.clear();
            continue;
        }
        if (entries.size() >= MAX_DISCOVERED_ARCHIVES) {
            LOGW("Reached automatic sound archive limit (%zu)", MAX_DISCOVERED_ARCHIVES);
            break;
        }
        const auto normalized = iterator->path().lexically_normal().generic_string();
        if (!known.insert(normalized).second) {
            continue;
        }
        PackEntry entry;
        entry.file = normalized;
        entry.type = PackType::Terraria;
        // 明确配置的候选保持其排序；自动发现项后置以作为遗漏配置的回退来源。
        entry.priority = std::numeric_limits<int>::max();
        entries.push_back(std::move(entry));
        LOGI("Discovered possible sound archive: %s", normalized.c_str());
    }
}

// 纹理批次字段（决定绘制顺序，UI 面板需非共享头插）。
static TEFKernel::PatchLib::Field FSharedBatching;
static TEFKernel::PatchLib::Field FNonSharedHeadInsert;

// 用于直接打开模块私有目录中的解压 XNB，避免 TitleContainer 仅能读取 APK 资源的问题。
static TEFKernel::PatchLib::Method FileStreamConstructor;
static TEFKernel::PatchLib::Method SoundEffectConstructor;
static TEFKernel::PatchLib::Method ContentManagerConstructor;
static TEFKernel::PatchLib::Method ContentManagerServiceProviderGetter;
static TEFKernel::PatchLib::Method ContentManagerLoadSoundEffect;
static TEFKernel::PatchLib::Method SoundEffectFromStream;
static TEFKernel::PatchLib::Method SoundEffectNameGetter;
static TEFKernel::PatchLib::Method PlayerPlayHurtSound;
static TEFKernel::PatchLib::Field PlayerVoiceVariantField;
static TEFKernel::PatchLib::Field PlayerVoicePitchOffsetField;
static TEFKernel::PatchLib::Field SoundEffectClipField;
static thread_local bool g_loading_override_sound = false;
static std::mutex g_override_manager_mutex;
static std::unordered_map<std::string, patch_handle_t> g_override_content_managers;
// 唯一别名资源键 -> 模块私有 XNB；别名不与原版 ContentManager 缓存键重合。
static std::unordered_map<std::string, std::filesystem::path> g_unique_sound_aliases;

// v1.6.1 仅诊断：保存由独立 ContentManager 返回的 SoundEffect 身份，
// 再追踪它是否被实际创建实例和播放。该表不修改游戏对象或播放参数。
static std::mutex g_replacement_sound_mutex;
static std::unordered_map<patch_handle_t, std::string> g_replacement_sound_keys;
static std::unordered_map<patch_handle_t, std::string> g_replacement_sound_instance_keys;
static std::unordered_set<patch_handle_t> g_untracked_sound_names_logged;
static thread_local std::string g_legacy_player_hit_pending_key;
struct LegacySoundContext {
    int type = -1;
    int style = -1;
    bool active = false;
};
static thread_local LegacySoundContext g_legacy_sound_context;
// 与已索引的 XNB 对应的标准 PCM WAV。仅在 SoundEffect.FromStream(1) 经运行时反射确认可用时使用。
static std::unordered_map<std::string, std::filesystem::path> g_sound_wav_indexes;

// App 日志不包含模块原生日志，因此将可验证状态写到模块私有目录。
static std::filesystem::path g_diagnostic_path;
static std::mutex g_diagnostic_mutex;

static void write_diagnostic(const std::string &line, const bool truncate = false) {
    if (g_diagnostic_path.empty()) {
        return;
    }
    std::lock_guard lock(g_diagnostic_mutex);
    std::ofstream output(g_diagnostic_path, truncate ? std::ios::trunc : std::ios::app);
    if (output.is_open()) {
        output << line << '\n';
    }
}

static std::string get_replacement_sound_key(const patch_handle_t sound) {
    std::lock_guard lock(g_replacement_sound_mutex);
    const auto found = g_replacement_sound_keys.find(sound);
    return found == g_replacement_sound_keys.end() ? std::string{} : found->second;
}

static std::string get_replacement_sound_instance_key(const patch_handle_t instance) {
    std::lock_guard lock(g_replacement_sound_mutex);
    const auto found = g_replacement_sound_instance_keys.find(instance);
    return found == g_replacement_sound_instance_keys.end() ? std::string{} : found->second;
}

// Android ContentManager 只以 Unity Resource 构造 AudioClip，忽略模块私有 XNB 根目录。
// 因此不再伪造 LoadSoundEffect 的返回值；只在原方法返回后把实际 SoundEffect 与
// 已注册的私有 PCM 映射关联，最终在其真实播放实例处替换输出。
static void LoadSoundEffect_Tracking_Postfix(patch_handle_t instance, void **args,
                                             void *result, const patch_method_signature_t *sig_info) {
    if (result == nullptr || args == nullptr || args[0] == nullptr) {
        return;
    }
    const auto assetHandle = *static_cast<patch_handle_t *>(args[0]);
    if (!patchlib_is_valid(assetHandle)) {
        return;
    }
    const auto key = SoundPack::NormalizeAssetKey(
        TEFKernel::PatchLib::Struct::String(assetHandle, false).ToString());
    if (key.empty() || !AndroidPcmPlayer::Instance().Has(key)) {
        return;
    }
    const auto sound = *static_cast<patch_handle_t *>(result);
    if (!patchlib_is_valid(sound)) {
        write_diagnostic("NativePCM track=invalid-sound key=" + key);
        return;
    }
    {
        std::lock_guard lock(g_replacement_sound_mutex);
        g_replacement_sound_keys[sound] = key;
    }
    write_diagnostic("NativePCM track=success key=" + key);
}

// Player_Killed 等声音可直接调用 SoundEffect.Play()，不会创建 SoundEffectInstance。
// 只有资源追踪与原生 PCM 播放均成功时才跳过原调用；失败时继续原版声音。
static bool ReplacementSoundEffect_Play_Prefix(patch_handle_t instance, void **args,
                                               const patch_method_signature_t *sig_info, void *result) {
    const auto key = get_replacement_sound_key(instance);
    if (key.empty()) {
        return false;
    }
    std::string error;
    if (!AndroidPcmPlayer::Instance().Play(key, error)) {
        write_diagnostic("NativePCM direct-play=failed key=" + key +
                         " error=" + error + " original=preserved");
        return false;
    }
    // MonoGame/XNA SoundEffect.Play() 返回 bool。PatchLib 说明 result 只在跳过时生效；
    // result 为空时依然安全，因为本次原生播放已经成功启动。
    if (result != nullptr) {
        *static_cast<bool *>(result) = true;
    }
    write_diagnostic("NativePCM direct-play=success key=" + key + " original=suppressed");
    return true;
}

// SoundEffect.CreateInstance() 将返回的实例与其替换源建立只读关联。
static void ReplacementSoundEffect_CreateInstance_Postfix(patch_handle_t instance, void **args,
                                                          void *result, const patch_method_signature_t *sig_info) {
    if (result == nullptr) {
        return;
    }
    const auto key = get_replacement_sound_key(instance);
    if (key.empty()) {
        if (g_legacy_sound_context.active) {
            write_diagnostic("LegacySoundPlayer untracked CreateInstance type=" +
                             std::to_string(g_legacy_sound_context.type) +
                             " style=" + std::to_string(g_legacy_sound_context.style));
        }
        return;
    }
    const auto soundInstance = *static_cast<patch_handle_t *>(result);
    if (!patchlib_is_valid(soundInstance)) {
        write_diagnostic("Playback CreateInstance result-invalid key=" + key);
        return;
    }
    {
        std::lock_guard lock(g_replacement_sound_mutex);
        g_replacement_sound_instance_keys[soundInstance] = key;
    }
    write_diagnostic("Playback CreateInstance key=" + key);
}

// Player.PlayHurtSound 是当前移动版真实的角色受击入口。voiceVariant 和
// voicePitchOffset 均由角色创建界面/梳妆台写入 Player 并随角色保存。模块仅读取
// 这两个字段：不保存、不改写，也不维护自己的虚拟音调偏移。
// 仅当上传包的 PCM 成功开始输出时跳过原方法；字段或后端失败时保留游戏原声。
static bool Player_PlayHurtSound_Prefix(patch_handle_t instance, void **args,
                                        const patch_method_signature_t *sig_info, void *result) {
    // 真机 v1.13.1 日志确认三个 UI 项的实际值为 1、2、3，而非 0、1、2。
    // 保持已验证的 1->Female_Hit_1、2->Female_Hit_2，并使第 3 项固定使用余下的
    // Female_Hit_0；资源内容完全由玩家上传的 ZIP 决定。值 0 同样映射到 Female_Hit_0，
    // 以兼容旧角色或默认值。
    static constexpr std::array<const char *, 4> kPlayerHurtSoundKeys = {
        "Content/Sounds/Female_Hit_0",
        "Content/Sounds/Female_Hit_1",
        "Content/Sounds/Female_Hit_2",
        "Content/Sounds/Female_Hit_0",
    };
    if (instance == PATCH_NULL || !PlayerVoiceVariantField.IsValid() ||
        !PlayerVoicePitchOffsetField.IsValid()) {
        write_diagnostic("PlayerHurtSound voice-settings=unavailable original=preserved");
        return false;
    }

    const int voiceVariant = PlayerVoiceVariantField.GetValue<int>(instance);
    float voicePitchOffset = PlayerVoicePitchOffsetField.GetValue<float>(instance);
    if (!std::isfinite(voicePitchOffset)) {
        write_diagnostic("PlayerHurtSound voicePitchOffset=non-finite original=preserved");
        return false;
    }
    // 与 XNA/FNA SoundStyle.Pitch 语义一致：[-1, 1] 对应上下一个八度。
    voicePitchOffset = std::clamp(voicePitchOffset, -1.0F, 1.0F);
    if (voiceVariant < 0 || voiceVariant >= static_cast<int>(kPlayerHurtSoundKeys.size())) {
        write_diagnostic("PlayerHurtSound voiceVariant=" + std::to_string(voiceVariant) +
                         " unsupported original=preserved");
        return false;
    }

    const std::string key = kPlayerHurtSoundKeys[static_cast<size_t>(voiceVariant)];
    std::string error;
    if (AndroidPcmPlayer::Instance().Play(key, voicePitchOffset, error)) {
        write_diagnostic("PlayerHurtSound voiceVariant=" + std::to_string(voiceVariant) +
                         " voicePitchOffset=" + std::to_string(voicePitchOffset) +
                         " play=success key=" + key + " original=suppressed");
        return true;
    }
    write_diagnostic("PlayerHurtSound voiceVariant=" + std::to_string(voiceVariant) +
                     " voicePitchOffset=" + std::to_string(voicePitchOffset) +
                     " play=failed key=" + key + " error=" + error + " original=preserved");
    return false;
}

// 仅保留 LegacySoundPlayer 上下文诊断代码，实际替换已上移到 Player.PlayHurtSound，
// 避免将非玩家的 legacy type=1 调用错误替换。
static bool LegacySoundPlayer_PlaySound_Prefix(patch_handle_t instance, void **args,
                                               const patch_method_signature_t *sig_info, void *result) {
    if (args == nullptr || args[0] == nullptr || args[3] == nullptr) {
        return false;
    }
    const auto type = *static_cast<int *>(args[0]);
    const auto style = *static_cast<int *>(args[3]);
    g_legacy_sound_context = {.type = type, .style = style, .active = true};
    return false;
}

static void LegacySoundPlayer_PlaySound_Postfix(patch_handle_t instance, void **args,
                                                void *result, const patch_method_signature_t *sig_info) {
    const auto key = std::exchange(g_legacy_player_hit_pending_key, {});
    if (!key.empty()) {
        if (result == nullptr) {
            write_diagnostic("LegacyPlayerHit result=missing key=" + key);
        } else {
            const auto soundInstance = *static_cast<patch_handle_t *>(result);
            if (!patchlib_is_valid(soundInstance)) {
                write_diagnostic("LegacyPlayerHit result=invalid key=" + key);
            } else {
                std::lock_guard lock(g_replacement_sound_mutex);
                g_replacement_sound_instance_keys[soundInstance] = key;
                write_diagnostic("LegacyPlayerHit result=mapped key=" + key);
            }
        }
    }
    g_legacy_sound_context = {};
}

// 绝大多数 SoundEffect 都经 SoundEffectInstance.Play() 输出。只有已经注册且 OpenSL
// 成功启动的私有 PCM 才会跳过原版实例；任一失败均继续播放原版，避免将声音变为静音。
static bool ReplacementSoundEffectInstance_Play_Prefix(patch_handle_t instance, void **args,
                                                       const patch_method_signature_t *sig_info, void *result) {
    auto key = get_replacement_sound_instance_key(instance);
    bool legacyPlayerHit = false;
    if (key.empty() && !g_legacy_player_hit_pending_key.empty()) {
        key = std::exchange(g_legacy_player_hit_pending_key, {});
        legacyPlayerHit = true;
        {
            std::lock_guard lock(g_replacement_sound_mutex);
            g_replacement_sound_instance_keys[instance] = key;
        }
    }
    if (key.empty()) {
        return false;
    }
    std::string error;
    if (AndroidPcmPlayer::Instance().Play(key, error)) {
        write_diagnostic(std::string(legacyPlayerHit ? "LegacyPlayerHit play=success key=" : "NativePCM play=success key=") +
                         key + " original=suppressed");
        return true;
    }
    write_diagnostic(std::string(legacyPlayerHit ? "LegacyPlayerHit play=failed key=" : "NativePCM play=failed key=") +
                     key + " error=" + error + " original=preserved");
    return false;
}

static std::string make_unique_sound_alias_key(const std::filesystem::path &xnbPath) {
    // xnbPath = <private>/sound_cache/pack_N/Content/Sounds/<relative>.xnb
    auto contentDirectory = xnbPath.parent_path();
    while (!contentDirectory.empty() && contentDirectory.filename() != "Content") {
        contentDirectory = contentDirectory.parent_path();
    }
    if (contentDirectory.empty()) {
        return {};
    }
    const auto packDirectory = contentDirectory.parent_path();
    const auto soundDirectory = contentDirectory / "Sounds";
    std::error_code error;
    auto relative = std::filesystem::relative(xnbPath, soundDirectory, error);
    if (error || relative.empty()) {
        return {};
    }
    relative.replace_extension();
    return "Content/Sounds/__tef_override__/" + packDirectory.filename().generic_string() + "/" +
           relative.generic_string();
}

static void write_audio_clip_probe(const TEFKernel::PatchLib::Type &audioClipType) {
    // 所有查询均为固定名称/参数数，不枚举字段或方法，也不创建或修改任何 Unity 对象。
    const auto create3 = audioClipType.GetMethod("Create", 3);
    const auto create4 = audioClipType.GetMethod("Create", 4);
    const auto create5 = audioClipType.GetMethod("Create", 5);
    const auto setData2 = audioClipType.GetMethod("SetData", 2);
    write_diagnostic("Unity.AudioClip.probe type=" + std::to_string(audioClipType.IsValid()) +
                     " Create3=" + std::to_string(create3.IsValid()) +
                     " Create4=" + std::to_string(create4.IsValid()) +
                     " Create5=" + std::to_string(create5.IsValid()) +
                     " SetData2=" + std::to_string(setData2.IsValid()));
}

static void write_sound_effect_ctor2_signature(const TEFKernel::PatchLib::Type &soundEffectType) {
    const auto constructor = soundEffectType.GetMethod(".ctor", 2);
    if (!constructor.IsValid()) {
        write_diagnostic("SoundEffect.ctor2.signature=unavailable");
        return;
    }
    const auto signature = constructor.GetSignature();
    if (!signature) {
        write_diagnostic("SoundEffect.ctor2.signature=read-failed");
        return;
    }

    const auto argumentTypes = signature->GetArgTypes();
    const auto argumentNames = signature->GetArgNames();
    std::string line = "SoundEffect.ctor2.signature params=" + std::to_string(argumentTypes.size());
    for (size_t index = 0; index < argumentTypes.size(); ++index) {
        line += " type" + std::to_string(index) + "=" +
                std::to_string(static_cast<int>(argumentTypes[index]));
    }
    for (size_t index = 0; index < argumentNames.size(); ++index) {
        line += " name" + std::to_string(index) + "=" + argumentNames[index];
    }
    write_diagnostic(line);
}

static void write_sound_effect_creation_probe(const TEFKernel::PatchLib::Type &soundEffectType) {
    // 不遍历运行时返回的方法句柄。每一项均采用此前已经安全使用过的
    // GetMethod(name, parameterCount) 精确查询，并且绝不调用候选方法。
    std::string line = "SoundEffect.creation-probe";
    for (int count = 0; count <= 8; ++count) {
        line += " ctor" + std::to_string(count) + "=" +
                std::to_string(soundEffectType.GetMethod(".ctor", count).IsValid());
    }
    line += " FromStream0=" + std::to_string(soundEffectType.GetMethod("FromStream", 0).IsValid());
    line += " FromStream1=" + std::to_string(soundEffectType.GetMethod("FromStream", 1).IsValid());
    line += " FromStream2=" + std::to_string(soundEffectType.GetMethod("FromStream", 2).IsValid());
    line += " FromFile1=" + std::to_string(soundEffectType.GetMethod("FromFile", 1).IsValid());
    line += " FromFile2=" + std::to_string(soundEffectType.GetMethod("FromFile", 2).IsValid());
    line += " FromBytes1=" + std::to_string(soundEffectType.GetMethod("FromBytes", 1).IsValid());
    line += " FromBytes2=" + std::to_string(soundEffectType.GetMethod("FromBytes", 2).IsValid());
    line += " FromBytes3=" + std::to_string(soundEffectType.GetMethod("FromBytes", 3).IsValid());
    line += " FromBytes4=" + std::to_string(soundEffectType.GetMethod("FromBytes", 4).IsValid());
    line += " FromBytes5=" + std::to_string(soundEffectType.GetMethod("FromBytes", 5).IsValid());
    write_diagnostic(line);
}

static void write_little_endian_u32(std::ofstream &output, const std::uint32_t value) {
    const std::uint8_t bytes[] = {
        static_cast<std::uint8_t>(value & 0xffU),
        static_cast<std::uint8_t>((value >> 8U) & 0xffU),
        static_cast<std::uint8_t>((value >> 16U) & 0xffU),
        static_cast<std::uint8_t>((value >> 24U) & 0xffU),
    };
    output.write(reinterpret_cast<const char *>(bytes), sizeof(bytes));
}

// 将 Terraria SoundEffect XNB 的已解析 PCM 区段写为标准 RIFF/WAV。
// 对 PCM 的 18-byte WAVEFORMATEX 仅写标准 16-byte fmt 块，兼容 FromStream 的 WAV reader。
static bool export_xnb_sound_to_wav(const std::filesystem::path &source,
                                    const std::filesystem::path &destination,
                                    std::string &error) {
    XnbSoundEffectData sound;
    if (!ParseXnbSoundEffect(source, sound, error) || sound.format.size() < 16 || sound.waveform.empty()) {
        if (error.empty()) {
            error = "XNB sound payload is incomplete";
        }
        return false;
    }

    const std::uint16_t formatTag = static_cast<std::uint16_t>(sound.format[0]) |
                                    (static_cast<std::uint16_t>(sound.format[1]) << 8U);
    const std::size_t formatSize = formatTag == 1 ? 16U : sound.format.size();
    if (formatSize > sound.format.size() || sound.waveform.size() > UINT32_MAX || formatSize > UINT32_MAX) {
        error = "WAV export size is invalid";
        return false;
    }

    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        error = "cannot create WAV cache file";
        return false;
    }
    const auto fmtSize = static_cast<std::uint32_t>(formatSize);
    const auto dataSize = static_cast<std::uint32_t>(sound.waveform.size());
    output.write("RIFF", 4);
    write_little_endian_u32(output, 4U + 8U + fmtSize + 8U + dataSize);
    output.write("WAVEfmt ", 8);
    write_little_endian_u32(output, fmtSize);
    output.write(reinterpret_cast<const char *>(sound.format.data()), static_cast<std::streamsize>(formatSize));
    output.write("data", 4);
    write_little_endian_u32(output, dataSize);
    output.write(reinterpret_cast<const char *>(sound.waveform.data()), static_cast<std::streamsize>(sound.waveform.size()));
    if (!output.good()) {
        error = "failed while writing WAV cache file";
        return false;
    }
    return true;
}

static bool isUiTexture(const std::string& assetName) {
    return assetName.find("/UI/") != std::string::npos ||
                             assetName.find("Inventory_Back") != std::string::npos ||
                             assetName.find("PanelBackground") != std::string::npos ||
                             assetName.find("/CharCreation/") != std::string::npos ||
                             assetName.find("/WorldCreation/") != std::string::npos;
}

static void fixUi(const std::string& assetName, patch_handle_t texture) {
    // 批次字段处理（规律明确，无需参考原版对象）：
    // UI 面板类纹理需 SharedBatching=0 + NonSharedHeadInsert=1（非共享分支头插，先画背景，
    // 避免尾插盖住上层文字/图标）。物品/方块/弹幕等世界内容 SharedBatching=1。

    if (isUiTexture(assetName)) {
        FSharedBatching.SetValue<bool>(texture, false);
        FNonSharedHeadInsert.SetValue<bool>(texture, true);
        LOGD("LoadTexture2D: UI texture, SharedBatching=0 NonShared=1 for %s", assetName.c_str());
    } else {
        FSharedBatching.SetValue<bool>(texture, true);
        LOGD("LoadTexture2D: world texture, keep SharedBatching=1 for %s", assetName.c_str());
    }
}

// Prefix：若 assetName 在材质包索引中，直接创建材质包纹理并作为返回值
// （返回 true 跳过原方法，避免先加载原版再加载材质包造成双重加载）
static bool LoadTexture2D_Prefix(patch_handle_t instance, void **args,
                                 const patch_method_signature_t *sig_info, void *result) {
    if (!result) {
        return false;
    }

    const auto assetName = TEFKernel::PatchLib::Struct::String(*static_cast<patch_handle_t *>(args[0]), false).
            ToString();

    // 不在材质包索引中则使用原版纹理（走原方法）
    if (!g_texture_indexes.contains(assetName)) {
        return false;
    }

    // 获取材质包纹理数据
    TextureData textureData = TextureManager::Instance().Get(assetName);
    if (!textureData.IsValid()) {
        LOGW("LoadTexture2D: invalid texture data: %s", assetName.c_str());
        return false;
    }

    // 创建材质包的新纹理（UnityEngine 像素）
    const auto texture = terraria_texture2d_create(
        textureData.width,
        textureData.height,
        TEXTURE_FORMAT_RGBA32,
        textureData.data.data(),
        textureData.data.size()
    );
    if (!texture) {
        LOGE("LoadTexture2D: failed to create texture: %s", assetName.c_str());
        return false;
    }

    fixUi(assetName, texture);

    // 直接作为返回值（跳过原方法，只加载材质包一份）
    *static_cast<patch_handle_t *>(result) = texture;
    LOGD("LoadTexture2D: replaced texture object for %s", assetName.c_str());
    return true;
}

/**
 * @brief 重定向标准 Terraria 的 Content/Sounds 资源请求。
 *
 * TitleContainer.OpenStream 是 ContentManager 读取 XNB 的底层入口。命中替换项时，
 * 直接构造 System.IO.FileStream 并跳过原调用；其他资源继续执行原有加载流程，因此
 * 不会影响纹理、语言或游戏文件。
 */
/**
 * @brief 以只读 FileStream 打开模块私有缓存中的音效 XNB。
 */
static patch_handle_t OpenReplacementSoundStream(const std::filesystem::path &replacementPath) {
    if (!FileStreamConstructor.IsValid()) {
        return PATCH_NULL;
    }

    TEFKernel::PatchLib::Struct::String replacementName(replacementPath.generic_string());
    auto replacementHandle = replacementName.GetHandle();

    // System.IO.FileMode.Open = 3, FileAccess.Read = 1, FileShare.Read = 1。
    int fileMode = 3;
    int fileAccess = 1;
    int fileShare = 1;
    void *constructorArgs[] = {&replacementHandle, &fileMode, &fileAccess, &fileShare};

    patch_handle_t stream = PATCH_NULL;
    const bool opened = FileStreamConstructor.InvokeConstructorRaw(&stream, constructorArgs);
    if (!opened || !patchlib_is_valid(stream)) {
        LOGE("Failed to open replacement sound stream: %s", replacementPath.c_str());
        return PATCH_NULL;
    }
    return stream;
}

// 最低层强制覆盖：Android 的 ContentManager 会在内部把 RootDirectory 拼成绝对路径。
// 构造函数没有可写返回值，因此改写其 string 参数并继续执行原构造函数，确保
// ContentReader 得到的就是目标覆盖文件的流，而不是依赖上层返回对象。
static bool FileStream_SoundReplacement_Prefix(patch_handle_t instance, void **args,
                                               const patch_method_signature_t *sig_info, void *result) {
    if (args == nullptr || args[0] == nullptr) {
        return false;
    }
    const auto originalHandle = *static_cast<patch_handle_t *>(args[0]);
    if (!patchlib_is_valid(originalHandle)) {
        return false;
    }
    const auto requestedPath = TEFKernel::PatchLib::Struct::String(originalHandle, false).ToString();
    const auto soundKey = SoundPack::NormalizeAssetKey(requestedPath);
    const auto soundIt = g_sound_indexes.find(soundKey);
    if (soundKey.empty() || soundIt == g_sound_indexes.end()) {
        const auto uniqueIt = g_unique_sound_aliases.find(soundKey);
        if (uniqueIt != g_unique_sound_aliases.end()) {
            write_diagnostic("FileStream alias-read key=" + soundKey + " path=" + requestedPath);
        }
        return false;
    }
    TEFKernel::PatchLib::Struct::String replacementName(soundIt->second.generic_string());
    const auto replacementHandle = replacementName.GetHandle();
    *static_cast<patch_handle_t *>(args[0]) = replacementHandle;
    write_diagnostic("FileStream replacement=rewritten key=" + soundKey + " source=" + requestedPath +
                     " target=" + soundIt->second.string());
    return false;
}

/**
 * @brief 回退拦截：重定向 TitleContainer.OpenStream 的匹配声音请求。
 */
static bool OpenStream_Prefix(patch_handle_t instance, void **args,
                              const patch_method_signature_t *sig_info, void *result) {
    if (result == nullptr || args == nullptr || args[0] == nullptr) {
        return false;
    }

    const auto originalHandle = *static_cast<patch_handle_t *>(args[0]);
    if (!patchlib_is_valid(originalHandle)) {
        return false;
    }

    const auto requestedPath = TEFKernel::PatchLib::Struct::String(originalHandle, false).ToString();
    const auto soundKey = SoundPack::NormalizeAssetKey(requestedPath);
    if (soundKey.empty()) {
        return false;
    }
    const auto soundIt = g_sound_indexes.find(soundKey);
    if (soundIt == g_sound_indexes.end()) {
        write_diagnostic("sound-request miss key=" + soundKey + " path=" + requestedPath);
        return false;
    }

    const auto stream = OpenReplacementSoundStream(soundIt->second);
    if (!patchlib_is_valid(stream)) {
        write_diagnostic("sound-request stream-open-failed key=" + soundKey + " target=" + soundIt->second.string());
        return false;
    }

    *static_cast<patch_handle_t *>(result) = stream;
    write_diagnostic("sound-request replaced key=" + soundKey + " target=" + soundIt->second.string());
    LOGD("OpenStream: replaced %s", requestedPath.c_str());
    return true;
}

/**
 * @brief tModLoader 风格的 XNB 覆盖：以相同 IServiceProvider 创建一个独立 ContentManager，
 * 将其 RootDirectory 指向模块私有的 override 根目录，再让游戏原生 LoadSoundEffect
 * 走自己的 XNB reader。这样不会手工构造 SoundEffect，也不会绕过平台音频实现。
 */
static bool LoadSoundEffect_Replacement_Prefix(patch_handle_t instance, void **args,
                                               const patch_method_signature_t *sig_info, void *result) {
    if (g_loading_override_sound || result == nullptr || args == nullptr || args[0] == nullptr ||
        !ContentManagerConstructor.IsValid() || !ContentManagerServiceProviderGetter.IsValid() ||
        !ContentManagerLoadSoundEffect.IsValid()) {
        return false;
    }
    auto assetHandle = *static_cast<patch_handle_t *>(args[0]);
    if (!patchlib_is_valid(assetHandle)) {
        return false;
    }

    const auto requestedPath = TEFKernel::PatchLib::Struct::String(assetHandle, false).ToString();
    const auto soundKey = SoundPack::NormalizeAssetKey(requestedPath);
    const auto soundIt = g_sound_indexes.find(soundKey);
    if (soundKey.empty() || soundIt == g_sound_indexes.end()) {
        return false;
    }

    // 首选独立 WAV：此 Android 运行时已证明 ContentManager/XNB 路径可返回全局缓存，
    // 所以在运行时存在公开 FromStream(Stream) 时，直接由游戏自身的 WAV reader 创建新对象。
    const auto wavIt = g_sound_wav_indexes.find(soundKey);
    if (SoundEffectFromStream.IsValid() && wavIt != g_sound_wav_indexes.end()) {
        auto wavStream = OpenReplacementSoundStream(wavIt->second);
        patch_handle_t wavReplacement = PATCH_NULL;
        void *wavArgs[] = {&wavStream};
        const bool wavLoaded = patchlib_is_valid(wavStream) &&
                               SoundEffectFromStream.InvokeArgs(PATCH_NULL, &wavReplacement, wavArgs);
        if (wavLoaded && patchlib_is_valid(wavReplacement)) {
            *static_cast<patch_handle_t *>(result) = wavReplacement;
            {
                std::lock_guard lock(g_replacement_sound_mutex);
                g_replacement_sound_keys[wavReplacement] = soundKey;
            }
            write_diagnostic("LoadSoundEffect wav-stream=success key=" + soundKey +
                             " target=" + wavIt->second.string());
            return true;
        }
        write_diagnostic("LoadSoundEffect wav-stream=failed key=" + soundKey +
                         " target=" + wavIt->second.string());
    }

    // target = <private>/sound_cache/pack_N/Content/Sounds/<name>.xnb
    auto contentDirectory = soundIt->second.parent_path();
    while (!contentDirectory.empty() && contentDirectory.filename() != "Content") {
        contentDirectory = contentDirectory.parent_path();
    }
    if (contentDirectory.empty()) {
        write_diagnostic("LoadSoundEffect content-manager=missing-Content-root key=" + soundKey);
        return false;
    }
    const auto rootDirectory = contentDirectory.parent_path().generic_string();

    patch_handle_t overrideManager = PATCH_NULL;
    {
        std::lock_guard lock(g_override_manager_mutex);
        const auto existing = g_override_content_managers.find(rootDirectory);
        if (existing != g_override_content_managers.end()) {
            overrideManager = existing->second;
        } else {
            patch_handle_t serviceProvider = PATCH_NULL;
            if (!ContentManagerServiceProviderGetter.InvokeArgs(instance, &serviceProvider, nullptr) ||
                !patchlib_is_valid(serviceProvider)) {
                write_diagnostic("LoadSoundEffect content-manager=service-provider-unavailable key=" + soundKey);
                return false;
            }
            TEFKernel::PatchLib::Struct::String rootPath(rootDirectory);
            auto rootPathHandle = rootPath.GetHandle();
            void *constructorArgs[] = {&serviceProvider, &rootPathHandle};
            if (!ContentManagerConstructor.InvokeConstructorRaw(&overrideManager, constructorArgs) ||
                !patchlib_is_valid(overrideManager)) {
                write_diagnostic("LoadSoundEffect content-manager=constructor-failed root=" + rootDirectory);
                return false;
            }
            g_override_content_managers.emplace(rootDirectory, overrideManager);
            write_diagnostic("LoadSoundEffect content-manager=created root=" + rootDirectory);
        }
    }

    // 原资源键会命中 Android 的进程级 ContentManager 缓存；必须改为每个解压包独有的
    // 实体 XNB 键，才能令原生 reader 创建新的 Unity AudioClip 与 SoundEffect。
    const auto overrideAssetKey = make_unique_sound_alias_key(soundIt->second);
    const auto uniqueIt = g_unique_sound_aliases.find(overrideAssetKey);
    if (overrideAssetKey.empty() || uniqueIt == g_unique_sound_aliases.end() ||
        !std::filesystem::exists(uniqueIt->second)) {
        write_diagnostic("LoadSoundEffect unique-key=missing key=" + soundKey +
                         " override=" + overrideAssetKey);
        return false;
    }
    TEFKernel::PatchLib::Struct::String overrideAssetName(overrideAssetKey);
    auto overrideAssetHandle = overrideAssetName.GetHandle();

    // 原方法会再次触发本前置钩子；线程内重入标记确保第二次直接走游戏原始读取链。
    void *loadArgs[] = {&overrideAssetHandle};
    patch_handle_t replacement = PATCH_NULL;
    g_loading_override_sound = true;
    const bool loaded = ContentManagerLoadSoundEffect.InvokeArgs(overrideManager, &replacement, loadArgs);
    g_loading_override_sound = false;
    if (!loaded || !patchlib_is_valid(replacement)) {
        write_diagnostic("LoadSoundEffect content-manager=load-failed key=" + soundKey + " root=" + rootDirectory);
        return false;
    }

    *static_cast<patch_handle_t *>(result) = replacement;
    {
        std::lock_guard lock(g_replacement_sound_mutex);
        g_replacement_sound_keys[replacement] = soundKey;
    }
    if (SoundEffectClipField.IsValid()) {
        patch_handle_t clip = PATCH_NULL;
        SoundEffectClipField.GetValue(replacement, &clip);
        write_diagnostic("LoadSoundEffect clip-field key=" + soundKey +
                         " valid=" + std::to_string(patchlib_is_valid(clip)));
    }
    write_diagnostic("LoadSoundEffect content-manager=success key=" + soundKey +
                     " override=" + overrideAssetKey + " root=" + rootDirectory);
    return true;
}

/*
// Postfix：修复Ui显示问题
static void LoadTexture2D_Postfix(patch_handle_t instance, void **args,
                                  void *result, const patch_method_signature_t *sig_info) {
    const auto texture = *static_cast<patch_handle_t*>(instance);
    if (!texture) return;


    const auto assetName = TEFKernel::PatchLib::Struct::String(*static_cast<patch_handle_t *>(args[0]), false).
            ToString();

// 08-18 20:35:24.302  8598  8630 I texturepack_extension: load Content/Images/UI/PageIcons/MapSelected

    if (assetName.find("PanelBackground") != std::string::npos) {
        // 修复物品的 UI 显示问题
        FSharedBatching.SetValue<bool>(texture, false);
        FNonSharedHeadInsert.SetValue<bool>(texture, true);
        LOGD("LoadTexture2D: Item texture set to UI mode for %s", assetName.c_str());
    }
}
*/

/**
 * @brief 初始化模块
 * @param entry 模块条目指针
 * @return true-成功, false-失败
 */
static bool init_module(module_entry_t *entry) {
    std::vector<PackEntry> texture_pack_entries{};
    std::vector<PackEntry> sound_pack_entries{};
    const std::filesystem::path privateDirectory(entry->private_dir);
    g_diagnostic_path = privateDirectory / "audio_diagnostic_v1.15.1.txt";
    write_diagnostic("version=1.15.1 init_module=entered", true);
    write_diagnostic("private_dir=" + privateDirectory.string());
    const auto audioExtensionDirectory = privateDirectory.parent_path() / "eternal.future.audiopackextension";

    // 材质替换仍严格遵从 UI 开关；声音则同时枚举已启用和默认关闭的标准 Terraria
    // XNB 包，避免新安装音效包因 BasePackManager 的默认 enable=false 而完全不生效。
    load_json(privateDirectory / "config.json", "texture_packs", true, texture_pack_entries);
    load_json(audioExtensionDirectory / "config.json", "audio_packs", true, texture_pack_entries);
    load_json(privateDirectory / "config.json", "texture_packs", false, sound_pack_entries);
    load_json(audioExtensionDirectory / "config.json", "audio_packs", false, sound_pack_entries);

    const auto sortByPriority = [](std::vector<PackEntry> &entries) {
        std::ranges::sort(entries, [](const PackEntry &a, const PackEntry &b) {
            return a.priority < b.priority;
        });
    };
    sortByPriority(texture_pack_entries);
    sortByPriority(sound_pack_entries);
    const auto configuredSoundCandidates = sound_pack_entries.size();
    // <files>/module/private/<module-id> -> <files>
    const auto applicationFilesDirectory = privateDirectory.parent_path().parent_path().parent_path();
    discover_sound_archives(applicationFilesDirectory, sound_pack_entries);
    sortByPriority(sound_pack_entries);
    write_diagnostic("sound-pack-discovery configured=" + std::to_string(configuredSoundCandidates) +
                     " total=" + std::to_string(sound_pack_entries.size()));
    LOGI("Loaded %zu enabled texture packs and %zu sound-pack candidates",
         texture_pack_entries.size(), sound_pack_entries.size());
    write_diagnostic("texture_pack_entries=" + std::to_string(texture_pack_entries.size()) +
                     " sound_pack_candidates=" + std::to_string(sound_pack_entries.size()));

    // 热重载时清除旧索引与缓存，避免禁用包残留在后续游戏会话中。
    TextureManager::Instance().Clear();
    packs.clear();
    g_texture_indexes.clear();
    g_sound_indexes.clear();
    g_unique_sound_aliases.clear();
    AndroidPcmPlayer::Instance().Shutdown();
    {
        std::lock_guard lock(g_override_manager_mutex);
        g_override_content_managers.clear();
    }
    {
        std::lock_guard lock(g_replacement_sound_mutex);
        g_replacement_sound_keys.clear();
        g_replacement_sound_instance_keys.clear();
        g_sound_wav_indexes.clear();
    }

    const auto soundCacheDirectory = privateDirectory / "sound_cache";
    std::error_code cacheError;
    std::filesystem::remove_all(soundCacheDirectory, cacheError);
    if (cacheError) {
        LOGW("Failed to clear sound cache %s: %s", soundCacheDirectory.c_str(), cacheError.message().c_str());
    }
    std::filesystem::create_directories(soundCacheDirectory, cacheError);
    if (cacheError) {
        LOGE("Failed to create sound cache %s: %s", soundCacheDirectory.c_str(), cacheError.message().c_str());
        return false;
    }

    // 预先分配容量，避免 push_back 扩容导致 g_texture_indexes 中已存储的指针失效（悬空）。
    packs.reserve(texture_pack_entries.size());
    g_texture_indexes.reserve(texture_pack_entries.size());

    for (const auto &_pack : texture_pack_entries) {
        TexturePack pack(_pack);
        pack.BuildIndex();
        packs.push_back(std::move(pack));

        for (auto entryNames = packs.back().GetEntryNames(); const auto &entryName: entryNames) {
            // 存储 packs 中最后一个元素的地址（即刚 push_back 的元素）。
            g_texture_indexes[entryName] = &packs.back();
        }
    }

    for (size_t index = 0; index < sound_pack_entries.size(); ++index) {
        const auto &soundEntry = sound_pack_entries[index];
        const std::filesystem::path packPath(soundEntry.file);
        std::error_code packError;
        const bool exists = std::filesystem::exists(packPath, packError);
        const bool regular = exists && std::filesystem::is_regular_file(packPath, packError);
        const auto bytes = regular ? std::filesystem::file_size(packPath, packError) : 0U;
        write_diagnostic("sound-candidate index=" + std::to_string(index) +
                         " type=" + std::to_string(static_cast<int>(soundEntry.type)) +
                         " exists=" + std::to_string(exists) +
                         " regular=" + std::to_string(regular) +
                         " bytes=" + std::to_string(bytes) +
                         " path=" + packPath.string());
        const auto before = g_sound_indexes.size();
        const bool extracted = SoundPack(soundEntry).BuildAndExtract(
            soundCacheDirectory / ("pack_" + std::to_string(index)), g_sound_indexes);
        write_diagnostic("sound-extract index=" + std::to_string(index) +
                         " success=" + std::to_string(extracted) +
                         " added=" + std::to_string(g_sound_indexes.size() - before) +
                         " total=" + std::to_string(g_sound_indexes.size()));
    }

    if (g_sound_indexes.empty()) {
        // 不再安装任何内置音效。用户必须通过已上传的 Terraria 格式 ZIP 提供 XNB；
        // 索引为空时所有原版声音保持原样，避免模块附带资源覆盖用户的选择。
        write_diagnostic("sound-source=user-upload-only indexed=0 original-audio=preserved");
    }

    for (const auto &[soundKey, xnbPath] : g_sound_indexes) {
        const auto uniqueKey = make_unique_sound_alias_key(xnbPath);
        auto aliasPath = xnbPath.parent_path();
        while (!aliasPath.empty() && aliasPath.filename() != "Sounds") {
            aliasPath = aliasPath.parent_path();
        }
        if (!uniqueKey.empty() && !aliasPath.empty()) {
            auto contentDirectory = aliasPath.parent_path();
            const auto packDirectory = contentDirectory.parent_path();
            std::error_code relativeError;
            const auto relative = std::filesystem::relative(xnbPath, aliasPath, relativeError);
            const auto uniquePath = aliasPath / "__tef_override__" / packDirectory.filename() / relative;
            if (!relativeError && std::filesystem::exists(uniquePath)) {
                g_unique_sound_aliases[uniqueKey] = uniquePath;
            } else {
                write_diagnostic("unique-alias=missing key=" + soundKey + " expected=" + uniquePath.string());
            }
        }

        auto wavPath = xnbPath;
        wavPath.replace_extension(".wav");
        std::string wavError;
        if (export_xnb_sound_to_wav(xnbPath, wavPath, wavError)) {
            g_sound_wav_indexes[soundKey] = std::move(wavPath);
        } else {
            write_diagnostic("wav-cache=failed key=" + soundKey + " error=" + wavError);
        }
    }
    std::string pcmInitError;
    size_t nativePcmEntries = 0;
    if (!g_sound_indexes.empty() && AndroidPcmPlayer::Instance().Initialize(pcmInitError)) {
        for (const auto &[soundKey, xnbPath] : g_sound_indexes) {
            XnbSoundEffectData sound;
            std::string pcmError;
            if (!ParseXnbSoundEffect(xnbPath, sound, pcmError) ||
                !AndroidPcmPlayer::Instance().Register(soundKey, sound, pcmError)) {
                write_diagnostic("NativePCM register=failed key=" + soundKey + " error=" + pcmError);
                continue;
            }
            write_diagnostic("NativePCM register=success key=" + soundKey +
                             " formatTag=" + std::to_string(sound.format_tag) +
                             " channels=" + std::to_string(sound.channel_count) +
                             " sourceRate=" + std::to_string(sound.sample_rate) +
                             " bits=" + std::to_string(sound.bits_per_sample) +
                             " blockAlign=" + std::to_string(sound.block_align));
            ++nativePcmEntries;
        }
        write_diagnostic("NativePCM init=success registered=" + std::to_string(nativePcmEntries));
    } else if (!g_sound_indexes.empty()) {
        write_diagnostic("NativePCM init=failed error=" + pcmInitError);
    }
    write_diagnostic("sound_index_entries=" + std::to_string(g_sound_indexes.size()) +
                     " unique_alias_entries=" + std::to_string(g_unique_sound_aliases.size()) +
                     " wav_index_entries=" + std::to_string(g_sound_wav_indexes.size()));
    TextureManager::Instance().LoadAllAsync();

    const TEFKernel::PatchLib::Type ContentManager("Microsoft.Xna.Framework.Content", "ContentManager");
    const auto LoadTexture2D = ContentManager.GetMethod("LoadTexture2D", 1);
    const auto LoadSoundEffect = ContentManager.GetMethod("LoadSoundEffect", 1);
    ContentManagerConstructor = ContentManager.GetMethod(".ctor", 2);
    ContentManagerServiceProviderGetter = ContentManager.GetProperty("ServiceProvider").GetGetMethod();
    ContentManagerLoadSoundEffect = LoadSoundEffect;

    const TEFKernel::PatchLib::Type Texture2d("Microsoft.Xna.Framework.Graphics", "Texture2D");
    FSharedBatching = Texture2d.GetField("SharedBatching");
    FNonSharedHeadInsert = Texture2d.GetField("NonSharedHeadInsert");

    if (LoadTexture2D.IsValid()) {
        patchlib_install_prepost_hook(LoadTexture2D.GetHandle(), LoadTexture2D_Prefix, nullptr);
    } else {
        LOGE("Failed to locate ContentManager.LoadTexture2D");
        return false;
    }

    if (!g_sound_indexes.empty()) {
        const auto ContentOpenStream = ContentManager.GetMethod("OpenStream", 1);
        const TEFKernel::PatchLib::Type TitleContainer("Microsoft.Xna.Framework", "TitleContainer");
        const auto TitleContainerOpenStream = TitleContainer.GetMethod("OpenStream", 1);
        const TEFKernel::PatchLib::Type FileStream("System.IO", "FileStream");
        const TEFKernel::PatchLib::Type SoundEffect("Microsoft.Xna.Framework.Audio", "SoundEffect");
        const TEFKernel::PatchLib::Type SoundEffectInstance("Microsoft.Xna.Framework.Audio", "SoundEffectInstance");
        const TEFKernel::PatchLib::Type Player("Terraria", "Player");
        const TEFKernel::PatchLib::Type LegacySoundPlayer("Terraria.Audio", "LegacySoundPlayer");
        const TEFKernel::PatchLib::Type UnityAudioClip("UnityEngine", "AudioClip");
        FileStreamConstructor = FileStream.GetMethod(".ctor", 4);
        SoundEffectConstructor = SoundEffect.GetMethod(".ctor", 6);
        SoundEffectFromStream = SoundEffect.GetMethod("FromStream", 1);
        SoundEffectNameGetter = SoundEffect.GetMethod("get_Name", 0);
        PlayerPlayHurtSound = Player.GetMethod("PlayHurtSound", 0);
        PlayerVoiceVariantField = Player.GetField("voiceVariant");
        PlayerVoicePitchOffsetField = Player.GetField("voicePitchOffset");
        SoundEffectClipField = SoundEffect.GetField("clip");
        write_sound_effect_creation_probe(SoundEffect);
        write_audio_clip_probe(UnityAudioClip);
        write_sound_effect_ctor2_signature(SoundEffect);
        const auto SoundEffectCreateInstance = SoundEffect.GetMethod("CreateInstance", 0);
        const auto SoundEffectPlay = SoundEffect.GetMethod("Play", 0);
        const auto SoundEffectPlaySignature = SoundEffectPlay.GetSignature();
        const auto SoundEffectInstancePlay = SoundEffectInstance.GetMethod("Play", 0);
        const auto LegacySoundPlayerPlaySound = LegacySoundPlayer.GetMethod("PlaySound", 6);

        write_diagnostic("ContentManager.OpenStream.valid=" + std::to_string(ContentOpenStream.IsValid()) +
                         " ContentManager.LoadSoundEffect.valid=" + std::to_string(LoadSoundEffect.IsValid()) +
                         " TitleContainer.OpenStream.valid=" + std::to_string(TitleContainerOpenStream.IsValid()) +
                         " FileStream.ctor4.valid=" + std::to_string(FileStreamConstructor.IsValid()) +
                         " SoundEffect.ctor6.valid=" + std::to_string(SoundEffectConstructor.IsValid()) +
                         " SoundEffect.FromStream1.valid=" + std::to_string(SoundEffectFromStream.IsValid()) +
                         " SoundEffect.Name.get.valid=" + std::to_string(SoundEffectNameGetter.IsValid()) +
                         " SoundEffect.clip.field.valid=" + std::to_string(SoundEffectClipField.IsValid()) +
                         " ContentManager.ctor2.valid=" + std::to_string(ContentManagerConstructor.IsValid()) +
                         " ContentManager.ServiceProvider.get.valid=" + std::to_string(ContentManagerServiceProviderGetter.IsValid()) +
                         " SoundEffect.CreateInstance0.valid=" + std::to_string(SoundEffectCreateInstance.IsValid()) +
                         " SoundEffect.Play0.valid=" + std::to_string(SoundEffectPlay.IsValid()) +
                         " SoundEffect.Play0.returnType=" + std::to_string(SoundEffectPlaySignature ? SoundEffectPlaySignature->getReturnType() : PATCH_VOID) +
                         " SoundEffectInstance.Play0.valid=" + std::to_string(SoundEffectInstancePlay.IsValid()) +
                         " LegacySoundPlayer.PlaySound6.valid=" + std::to_string(LegacySoundPlayerPlaySound.IsValid()) +
                         " Player.PlayHurtSound0.valid=" + std::to_string(PlayerPlayHurtSound.IsValid()) +
                         " Player.voiceVariant.field.valid=" + std::to_string(PlayerVoiceVariantField.IsValid()) +
                         " Player.voiceVariant.field.size=" + std::to_string(PlayerVoiceVariantField.IsValid() ? PlayerVoiceVariantField.GetSize() : 0) +
                         " Player.voicePitchOffset.field.valid=" + std::to_string(PlayerVoicePitchOffsetField.IsValid()) +
                         " Player.voicePitchOffset.field.size=" + std::to_string(PlayerVoicePitchOffsetField.IsValid() ? PlayerVoicePitchOffsetField.GetSize() : 0));
        // Terraria 的 XNB 内容读取发生在 ContentManager.OpenStream；该流再由
        // SoundEffectReader 解码。TitleContainer 仅作为底层回退，因为不同运行时
        // 可能在 ContentManager 的覆盖实现中直接返回流而不再调用 TitleContainer。
        bool installedSoundHook = false;
        if (LoadSoundEffect.IsValid()) {
            const auto trackingHook = patchlib_install_prepost_hook(
                LoadSoundEffect.GetHandle(), nullptr, LoadSoundEffect_Tracking_Postfix);
            installedSoundHook = trackingHook != PATCH_HOOK_INVALID_ID;
            write_diagnostic("LoadSoundEffect native-pcm-tracking=" + std::to_string(installedSoundHook));
        } else {
            write_diagnostic("LoadSoundEffect native-pcm-tracking=unavailable");
        }
        // v1.9.0 已由设备日志证明 Unity ContentManager 不会进入 FileStream/TitleContainer
        // 读取模块私有 XNB；不再安装无效的流重定向钩子。
        (void) ContentOpenStream;
        (void) TitleContainerOpenStream;
        const bool createInstanceProbeInstalled = SoundEffectCreateInstance.IsValid() &&
                                                  patchlib_install_prepost_hook(SoundEffectCreateInstance.GetHandle(), nullptr,
                                                                               ReplacementSoundEffect_CreateInstance_Postfix) != PATCH_HOOK_INVALID_ID;
        const bool soundEffectPlayProbeInstalled = SoundEffectPlay.IsValid() &&
                                                   patchlib_install_prepost_hook(SoundEffectPlay.GetHandle(),
                                                                                ReplacementSoundEffect_Play_Prefix, nullptr) != PATCH_HOOK_INVALID_ID;
        const bool soundEffectInstancePlayProbeInstalled = SoundEffectInstancePlay.IsValid() &&
                                                           patchlib_install_prepost_hook(SoundEffectInstancePlay.GetHandle(),
                                                                                        ReplacementSoundEffectInstance_Play_Prefix, nullptr) != PATCH_HOOK_INVALID_ID;
        const bool fileStreamReplacementInstalled = FileStreamConstructor.IsValid() &&
                                                    patchlib_install_prepost_hook(FileStreamConstructor.GetHandle(),
                                                                                 FileStream_SoundReplacement_Prefix, nullptr) != PATCH_HOOK_INVALID_ID;
        const bool playerHurtSoundHookInstalled = PlayerPlayHurtSound.IsValid() &&
                                                   patchlib_install_prepost_hook(PlayerPlayHurtSound.GetHandle(),
                                                                                Player_PlayHurtSound_Prefix, nullptr) != PATCH_HOOK_INVALID_ID;
        const bool legacyPlaySoundProbeInstalled = false;
        write_diagnostic("Playback probes CreateInstance=" + std::to_string(createInstanceProbeInstalled) +
                         " SoundEffect.Play=" + std::to_string(soundEffectPlayProbeInstalled) +
                         " SoundEffectInstance.Play=" + std::to_string(soundEffectInstancePlayProbeInstalled) +
                         " FileStream.replacement=" + std::to_string(fileStreamReplacementInstalled) +
                         " LegacySoundPlayer.PlaySound=" + std::to_string(legacyPlaySoundProbeInstalled) +
                         " Player.PlayHurtSound=" + std::to_string(playerHurtSoundHookInstalled));

        if (!installedSoundHook) {
            write_diagnostic("init_module=failed reason=no-compatible-XNB-stream-method");
            LOGE("No compatible XNB sound stream method was found");
            return false;
        }
        write_diagnostic("init_module=sound-hooks-installed");
    } else {
        write_diagnostic("init_module=no-sound-assets-indexed");
    }

    write_diagnostic("init_module=success");
    return true;
}

/**
 * @brief 清理并关闭模块
 * @param entry 模块条目指针
 * @return true-成功, false-失败
 */
static bool cleanup_module(module_entry_t *entry) {
    return true;
}

/**
 * @brief 热重载操作
 * @param entry 模块条目指针
 */
static void hot_reload(module_entry_t *entry) {
}

static const module_info_t *get_info() {
    return &g_module_info;
}

static constexpr module_ops_t g_module_ops = {
    .init_module = init_module,
    .cleanup_module = cleanup_module,
    .hot_reload = hot_reload,
    .get_info = get_info,
};

API_EXPORT const module_ops_t * API_CALL module_create(void) {
    return &g_module_ops;
}
