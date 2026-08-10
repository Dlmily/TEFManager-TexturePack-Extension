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
#include "TextureManager.hpp"

#include <fstream>
#include <vector>

#include "Log.hpp"
#include "TexturePack.hpp"
#include "lib/json.hpp"

#include "tefkernel-cpp-wrapper/patchlib/type.hpp"
#include "tefkernel-cpp-wrapper/patchlib/method.hpp"
#include "tefkernel-cpp-wrapper/patchlib/field.hpp"
#include "tefkernel-cpp-wrapper/patchlib/struct/string.hpp"
#include "tefkernel-cpp-wrapper/tefkernel/terraria/texture2d.h"
#include "tefkernel-cpp-wrapper/tefkernel/terraria/asset.h"


static constexpr module_info_t g_module_info = {
    .pkg_id = "eternal.future.texturepackextension",           // 唯一包名
    .name = "TexturePackExtension",                          // 插件名称
    .author = "eternalfuture-e38299",                        // 作者
    .version = "1.0.0",                           // 版本
    .version_code = 1,                            // 版本代码
    .api_version = 1,                             // API版本
    .plugin_dependencies_sizes = 0,               // 依赖插件数组大小（如需依赖请修改）
    .plugin_dependencies = nullptr,                 // 依赖插件列表
};

static bool load_json(const std::filesystem::path &path, std::vector<PackEntry> &output) {
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

        // 临时存储所有 enable=true 的条目
        std::vector<PackEntry> temp;

        for (const auto& item : j) {
            if (item.value("enable", false)) {
                PackEntry entry;
                entry.file = path.parent_path() / "texture_packs" / item.value("file", "");
                entry.priority = item.value("priority", 0);
                entry.type = static_cast<PackType>(item.value("type", 0));
                temp.push_back(entry);
            }
        }

        // 按优先级排序
        std::ranges::sort(temp,
                          [](const PackEntry& a, const PackEntry& b) {
                              return a.priority < b.priority;
                          });

        output = std::move(temp);  // 移动赋值，高效
        return true;

    } catch (const nlohmann::json::parse_error& e) {
        LOGE("JSON parse error in %s: %s\n", path.c_str(), e.what());
        return false;
    } catch (const std::exception& e) {
        LOGE("Error reading %s: %s\n", path.c_str(), e.what());
        return false;
    }
}

// 当前正在由 LoadTexture2D 加载的 assetName（供 LoadImage hook 关联）
static std::string g_currentAsset;

// 纹理批次相关字段（复制到新纹理，保证 MaterialBuffer 状态一致，避免崩溃）
TEFKernel::PatchLib::Field FTextureOffsetScale;
TEFKernel::PatchLib::Field FSharedBatching;
TEFKernel::PatchLib::Field FNonSharedHeadInsert;
TEFKernel::PatchLib::Field FPackedEntry;
TEFKernel::PatchLib::Field FTextureAtlas;
TEFKernel::PatchLib::Field FBatchTextureIndex;
TEFKernel::PatchLib::Field FUnityTexture;


// Prefix：记录当前加载的 assetName
static bool LoadTexture2D_Prefix(patch_handle_t instance, void **args,
                                 const patch_method_signature_t *sig_info, void *result) {
    const auto assetName = TEFKernel::PatchLib::Struct::String(*static_cast<patch_handle_t *>(args[0]), false).ToString();
    g_currentAsset = assetName;
    LOGD("LoadTexture2D: loading asset='%s'", assetName.c_str());
    return false;
}

// Postfix：创建材质包纹理，替换原版纹理的 _unityTexture，同时复制全部批次字段
static void LoadTexture2D_Postfix(patch_handle_t instance, void **args,
                                  void *result, const patch_method_signature_t *sig_info) {
    g_currentAsset.clear();
    if (!result) {
        return;
    }

    // 获取资源名称
    const auto assetName = TEFKernel::PatchLib::Struct::String(*static_cast<patch_handle_t *>(args[0]), false).ToString();

    // 不在材质包索引中则使用原版纹理
    if (!g_texture_indexes.contains(assetName)) {
        return;
    }

    // 获取游戏原版纹理对象（原方法已执行，批次状态完整）
    patch_handle_t originalTexture = *static_cast<patch_handle_t *>(result);
    if (!originalTexture) {
        LOGW("LoadTexture2D: original texture is null: %s", assetName.c_str());
        return;
    }

    // 获取材质包纹理数据
    TextureData textureData = TextureManager::Instance().Get(assetName);
    if (!textureData.IsValid()) {
        LOGW("LoadTexture2D: invalid texture data: %s", assetName.c_str());
        return;
    }

    // 创建材质包的新纹理（UnityEngine 像素）
    auto texture = terraria_texture2d_create(
        textureData.width,
        textureData.height,
        TEXTURE_FORMAT_RGBA32,
        textureData.data.data(),
        textureData.data.size()
    );
    if (!texture) {
        LOGE("LoadTexture2D: failed to create texture: %s", assetName.c_str());
        return;
    }

    // 诊断：打印原版纹理的关键批次字段值（确认 UI 面板应设的值）
    void *origPackedEntry = nullptr; FPackedEntry.GetValue(originalTexture, &origPackedEntry);
    void *origAtlas = nullptr; FTextureAtlas.GetValue(originalTexture, &origAtlas);
    LOGD("LoadTexture2D: orig SharedBatching=%d NonSharedHeadInsert=%d BatchIndex=%d PackedEntry=%p Atlas=%p for %s",
         FSharedBatching.GetValue<bool>(originalTexture) ? 1 : 0,
         FNonSharedHeadInsert.GetValue<bool>(originalTexture) ? 1 : 0,
         FBatchTextureIndex.GetValue<int>(originalTexture),
         origPackedEntry, origAtlas,
         assetName.c_str());

    // 诊断：打印新纹理尺寸与内部 _unityTexture 是否有效
    void *diagUnity = nullptr;
    FUnityTexture.GetValue(texture, &diagUnity);
    LOGD("LoadTexture2D: new tex w=%d h=%d unityTexture=%p for %s",
         terraria_texture2d_get_width(texture),
         terraria_texture2d_get_height(texture),
         diagUnity, assetName.c_str());

    // 保留游戏原版纹理对象（批次/图集状态完整），仅替换其内部 UnityEngine 像素纹理
    // 这样批处理系统/绘制顺序不被破坏，避免“面板盖住上层”
    // 注意：直接替换 _unityTexture 会导致渲染崩溃，因此改为返回新纹理对象
    // 尺寸检查（记录日志）
    const int origW = terraria_texture2d_get_width(originalTexture);
    const int origH = terraria_texture2d_get_height(originalTexture);
    if (origW != textureData.width || origH != textureData.height) {
        LOGW("LoadTexture2D: size mismatch orig=%dx%d pack=%dx%d, skip: %s",
             origW, origH, textureData.width, textureData.height, assetName.c_str());
        return;
    }

    // 批次字段处理：
    // UI 面板类纹理需要 SharedBatching=0 + NonSharedHeadInsert=1（非共享分支头插，先画背景，
    // 避免尾插盖住上层文字/图标）。物品/方块/弹幕等世界内容保持原版 SharedBatching=1。
    // 用路径前缀判断：Content/Images/UI/ 及背包面板（Inventory_Back）等属 UI 面板。
    const bool isUiTexture = assetName.find("/UI/") != std::string::npos ||
                             assetName.find("Inventory_Back") != std::string::npos ||
                             assetName.find("PanelBackground") != std::string::npos ||
                             assetName.find("/CharCreation/") != std::string::npos ||
                             assetName.find("/WorldCreation/") != std::string::npos;

    if (isUiTexture) {
        // UI 面板：非共享 + 头插，先画背景
        if (FSharedBatching.IsValid()) FSharedBatching.SetValue<bool>(texture, false);
        if (FNonSharedHeadInsert.IsValid()) FNonSharedHeadInsert.SetValue<bool>(texture, true);
        LOGD("LoadTexture2D: UI texture, SharedBatching=0 NonShared=1 for %s", assetName.c_str());
    } else {
        // 世界内容：保持共享批
        if (FSharedBatching.IsValid()) FSharedBatching.SetValue<bool>(texture, true);
        LOGD("LoadTexture2D: world texture, keep SharedBatching=1 for %s", assetName.c_str());
    }

    // 返回新纹理对象
    *static_cast<patch_handle_t *>(result) = texture;
    LOGD("LoadTexture2D: replaced texture object for %s", assetName.c_str());
}

// Prefix：替换 Unity 解码时的 PNG 字节数据（ImageConversion.LoadImage）
static bool ImageLoadImage_Prefix(patch_handle_t instance, void **args,
                                  const patch_method_signature_t *sig_info, void *result) {
    if (g_currentAsset.empty() || !g_texture_indexes.contains(g_currentAsset)) {
        return false;
    }
    LOGD("ImageLoadImage: called for currentAsset='%s'", g_currentAsset.c_str());
    return false;
}

/**
 * @brief 初始化模块
 * @param entry 模块条目指针
 * @return true-成功, false-失败
 */
static bool init_module(module_entry_t *entry)
{
    std::vector<PackEntry> pack_entries{};

    load_json(std::filesystem::path(entry->private_dir) / "config.json", pack_entries);
    for (const auto& _pack : pack_entries) {
        TexturePack pack(_pack);
        pack.BuildIndex();
        packs.push_back(pack);  // 拷贝到packs中

        for (auto entries_names = pack.GetEntryNames(); const auto& entry_name : entries_names) {
            // 存储packs中最后一个元素的地址（即刚push_back的元素）
            g_texture_indexes[entry_name] = &packs.back();
        }
    }

    TextureManager::Instance().LoadAllAsync();

    TEFKernel::PatchLib::Type ContentManager("Microsoft.Xna.Framework.Content", "ContentManager");
    auto LoadTexture2D = ContentManager.GetMethod("LoadTexture2D", 1);

    TEFKernel::PatchLib::Type Texture2d("Microsoft.Xna.Framework.Graphics", "Texture2D");
    FTextureOffsetScale = Texture2d.GetField("TextureOffsetScale");
    FSharedBatching = Texture2d.GetField("SharedBatching");
    FNonSharedHeadInsert = Texture2d.GetField("NonSharedHeadInsert");
    FPackedEntry = Texture2d.GetField("PackedEntry");
    FTextureAtlas = Texture2d.GetField("_textureAtlas");
    FBatchTextureIndex = Texture2d.GetField("BatchTextureIndex");
    FUnityTexture = Texture2d.GetField("_unityTexture");

    // hook UnityEngine.ImageConversion.LoadImage(Texture2D, byte[], bool)
    TEFKernel::PatchLib::Type ImageConversionType("UnityEngine", "ImageConversion");
    auto LoadImageMethod = ImageConversionType.GetMethod("LoadImage", 3);

    // ReSharper disable once CppNoDiscardExpression
    patchlib_install_prepost_hook(LoadTexture2D.GetHandle(), LoadTexture2D_Prefix, LoadTexture2D_Postfix);
    if (LoadImageMethod.IsValid()) {
        patchlib_install_prepost_hook(LoadImageMethod.GetHandle(), ImageLoadImage_Prefix, nullptr);
        LOGI("Hooked ImageConversion.LoadImage");
    } else {
        LOGE("Failed to get ImageConversion.LoadImage");
    }

    return true;
}

/**
 * @brief 清理并关闭模块
 * @param entry 模块条目指针
 * @return true-成功, false-失败
 */
static bool cleanup_module(module_entry_t *entry)
{

    return true;
}

/**
 * @brief 热重载操作
 * @param entry 模块条目指针
 */
static void hot_reload(module_entry_t *entry) {  }

static const module_info_t *get_info()
{
    return &g_module_info;
}

static constexpr module_ops_t g_module_ops = {
    .init_module = init_module,
    .cleanup_module = cleanup_module,
    .hot_reload = hot_reload,
    .get_info = get_info,
};

API_EXPORT const module_ops_t * API_CALL module_create(void)
{
    return &g_module_ops;
}