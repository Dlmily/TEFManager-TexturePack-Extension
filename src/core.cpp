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

TEFKernel::PatchLib::Field SharedBatching;
TEFKernel::PatchLib::Field NonSharedHeadInsert;


static bool LoadTexture2D_Hook(patch_handle_t instance, void **args,
                                  const patch_method_signature_t *sig_info, void *result) {
    // 获取资源名称
    const auto assetName = TEFKernel::PatchLib::Struct::String(*static_cast<patch_handle_t *>(args[0]), false).ToString();
        LOGD("Request_Texture2d_Hook: assetName='%s'", assetName.c_str());

        // 检查是否在索引中
        bool hasTexture = g_texture_indexes.contains(assetName);
        LOGD("Request_Texture2d_Hook: texture in index? %s", hasTexture ? "YES" : "NO");

        if (hasTexture) {
            LOGD("Request_Texture2d_Hook: Loading texture: %s", assetName.c_str());

            // 获取纹理数据
            TextureData textureData = TextureManager::Instance().Get(assetName);
            LOGD("Request_Texture2d_Hook: textureData.IsValid() = %s", textureData.IsValid() ? "true" : "false");

            if (textureData.IsValid()) {
                LOGD("Request_Texture2d_Hook: texture dimensions: %dx%d, size: %zu bytes",
                     textureData.width, textureData.height, textureData.data.size());

                // 创建纹理
                auto texture = terraria_texture2d_create(
                    textureData.width,
                    textureData.height,
                    TEXTURE_FORMAT_RGBA32,
                    textureData.data.data(),
                    textureData.data.size()
                );

                if (assetName.starts_with("Content/Images/Item_")) {
                    SharedBatching.SetValue<bool>(texture, false);
                    NonSharedHeadInsert.SetValue<bool>(texture, false);
                }

                if (texture) {
                    *static_cast<patch_handle_t *>(result) = texture;
                    return true;
                }
                LOGE("Request_Texture2d_Hook: Failed to create texture for: %s", assetName.c_str());
            } else {
                LOGE("Request_Texture2d_Hook: Invalid texture data for: %s", assetName.c_str());
                LOGE("Request_Texture2d_Hook: data.size()=%zu, width=%d, height=%d",
                     textureData.data.size(), textureData.width, textureData.height);
            }
        } else {
            LOGD("Request_Texture2d_Hook: texture not in index, using original: %s", assetName.c_str());
        }


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
    SharedBatching = Texture2d.GetField("SharedBatching");
    NonSharedHeadInsert = Texture2d.GetField("NonSharedHeadInsert");

    // ReSharper disable once CppNoDiscardExpression
    patchlib_install_prepost_hook(LoadTexture2D.GetHandle(), LoadTexture2D_Hook, nullptr);

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