/*******************************************************************************
 * texturepack_extension - TexturePack
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

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

enum class PackType {
    Terraria = 0,   // 标准 ZIP 格式，Content/ 目录
    TLPro,          // TLPro 格式，Modified/ 目录 + JSON 配置
    TEFManager      // TEFManager 格式（预留）
};

struct PackEntry {
    std::string file;
    int priority;
    PackType type;

    PackEntry() : priority(0), type(PackType::Terraria) {}
};

/**
 * @brief 纹理数据，包含解码后的像素数据和元信息
 */
struct TextureData {
    std::vector<uint8_t> data;  // RGBA 像素数据
    int width = 0;
    int height = 0;
    int channels = 4;  // 默认 RGBA

    TextureData() = default;

    TextureData(std::vector<uint8_t>&& pixels, const int w, const int h, const int c = 4)
        : data(std::move(pixels)), width(w), height(h), channels(c) {}

    TextureData(const uint8_t* pixels, const size_t size, const int w, const int h, const int c = 4)
        : data(pixels, pixels + size), width(w), height(h), channels(c) {}

    /**
     * @brief 检查数据是否有效
     */
    [[nodiscard]] bool IsValid() const {
        return !data.empty() && width > 0 && height > 0;
    }

    /**
     * @brief 获取总像素数
     */
    [[nodiscard]] size_t GetPixelCount() const {
        return static_cast<size_t>(width) * height;
    }

    /**
     * @brief 获取数据大小（字节）
     */
    [[nodiscard]] size_t GetSize() const {
        return data.size();
    }

    /**
     * @brief 清空数据
     */
    void Clear() {
        data.clear();
        width = 0;
        height = 0;
        channels = 4;
    }
};

/**
 * @brief 纹理包类 - 支持延迟加载
 */
class TexturePack {
public:
    explicit TexturePack(PackEntry entry);

    /**
     * @brief 构建索引（不加载纹理数据）
     * @return true 成功, false 失败
     */
    bool BuildIndex();

    /**
     * @brief 通过 entry_name 获取纹理数据
     * @param name entry_name
     * @return 纹理数据（解压后的原始字节），不存在返回空
     */
    TextureData GetTexture(const std::string& name);

    /**
     * @brief 通过 entry_name 获取材质包的原始文件字节（不解码）
     * @param name entry_name
     * @return 原始文件字节，不存在返回空
     */
    std::vector<uint8_t> GetRawFile(const std::string& name);

    /**
     * @brief 获取所有索引的 entry_name 列表
     */
    [[nodiscard]] std::vector<std::string> GetEntryNames() const;

    /**
     * @brief 清空索引
     */
    void Clear();

private:
    PackEntry _entry;
    std::unordered_map<std::string, std::string> _index; // entry_name -> zip内文件路径

    // 内部方法
    bool BuildZipIndex();
    bool BuildTLProIndex();
    std::vector<uint8_t> ExtractFile(const std::string& path) const;

    // 工具方法
    static bool ParseTLConfig(const std::vector<uint8_t>& jsonData, std::string& entry_name);
};