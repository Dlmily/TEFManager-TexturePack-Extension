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

#include "TexturePack.hpp"
#include "Log.hpp"
#include "lib/miniz.h"
#include "lib/json.hpp"
#include "lib/stb_image.h"

using json = nlohmann::json;

namespace {
    constexpr auto CONTENT_PREFIX = "Content/";
    constexpr auto MODIFIED_PREFIX = "Modified/";
    constexpr auto JSON_EXT = ".json";

    bool is_json_file(const std::string &filename) {
        return filename.size() >= 5 &&
               filename.compare(filename.size() - 5, 5, JSON_EXT) == 0;
    }

    std::string extract_id(const std::string &path) {
        const size_t last_slash = path.find_last_of('/');
        const size_t last_dot = path.find_last_of('.');

        if (last_slash == std::string::npos || last_dot == std::string::npos) {
            return {};
        }

        return path.substr(last_slash + 1, last_dot - last_slash - 1);
    }
}

TexturePack::TexturePack(PackEntry entry) : _entry(std::move(entry)) {
}

bool TexturePack::BuildIndex() {
    switch (_entry.type) {
        case PackType::Terraria:
            return BuildZipIndex();
        case PackType::TLPro:
            return BuildTLProIndex();
        case PackType::TEFManager:
            LOGW("TEFManager format not yet implemented");
            return false;
        default:
            LOGE("Unknown pack type: %d", static_cast<int>(_entry.type));
            return false;
    }
}

TextureData TexturePack::GetTexture(const std::string &name) {
    const auto it = _index.find(name);
    if (it == _index.end()) {
        LOGW("Texture not found: %s", name.c_str());
        return {};
    }

    // 提取原始文件数据
    const std::vector<uint8_t> fileData = ExtractFile(it->second);
    if (fileData.empty()) {
        LOGE("Failed to extract file: %s", it->second.c_str());
        return {};
    }

    //当参数为 true 时，加载后的图像数据原点将在左下角，
    //与 OpenGL 纹理坐标系统一致[citation:2][citation:5][citation:11]
    stbi_set_flip_vertically_on_load(true);

    // 解码图像
    int width, height, channels;
    unsigned char *imageData = stbi_load_from_memory(
        fileData.data(),
        static_cast<int>(fileData.size()),
        &width, &height, &channels,
        4 // 强制转换为 RGBA
    );

    if (!imageData) {
        LOGE("Failed to decode image: %s", it->second.c_str());
        return {};
    }

    // 将解码后的数据直接复制到 vector
    // 因为已经设置了翻转，所以数据可以直接使用
    const size_t dataSize = width * height * 4;
    TextureData result(
        std::vector(imageData, imageData + dataSize),
        width, height, 4);

    // 释放 stb_image 资源
    stbi_image_free(imageData);

    return result;
}

std::vector<std::string> TexturePack::GetEntryNames() const {
    std::vector<std::string> names;
    names.reserve(_index.size());
    for (const auto &name: _index | std::views::keys) {
        names.push_back(name);
    }
    return names;
}

void TexturePack::Clear() {
    _index.clear();
}

bool TexturePack::BuildZipIndex() {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, _entry.file.c_str(), 0)) {
        LOGE("Failed to open ZIP: %s", _entry.file.c_str());
        return false;
    }

    const auto file_count = mz_zip_reader_get_num_files(&zip);
    size_t total = 0;

    for (int i = 0; i < file_count; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            continue;
        }

        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            continue;
        }

        std::string filename(stat.m_filename);

        // 只索引 Content/ 下的文件
        if (filename.find(CONTENT_PREFIX) != 0) {
            continue;
        }

        // 跳过 JSON 等非图像文件（本地化/配置）
        if (is_json_file(filename)) {
            continue;
        }

        // entry_name = 去掉后缀
        std::string entry_name = filename;
        if (const size_t dot_pos = entry_name.find_last_of('.'); dot_pos != std::string::npos) {
            entry_name = entry_name.substr(0, dot_pos); // 删除最后一个 . 及其之后的内容
        }
        _index[entry_name] = filename;
        ++total;
    }

    mz_zip_reader_end(&zip);
    LOGI("Indexed %zu textures from %s", total, _entry.file.c_str());
    return true;
}

bool TexturePack::BuildTLProIndex() {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, _entry.file.c_str(), 0)) {
        LOGE("Failed to open ZIP: %s", _entry.file.c_str());
        return false;
    }

    auto file_count = mz_zip_reader_get_num_files(&zip);

    // 第一步：收集所有 JSON 配置
    std::unordered_map<std::string, std::string> json_map; // id -> json路径

    for (int i = 0; i < file_count; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            continue;
        }

        std::string filename(stat.m_filename);

        if (filename.find(MODIFIED_PREFIX) != 0) {
            continue;
        }

        if (is_json_file(filename)) {
            std::string id = extract_id(filename);
            if (!id.empty()) {
                json_map[id] = filename;
                LOGD("Found JSON: %s (id: %s)", filename.c_str(), id.c_str());
            }
        }
    }

    // 第二步：解析纹理文件，建立索引
    size_t total = 0;

    for (int i = 0; i < file_count; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            continue;
        }

        std::string filename(stat.m_filename);

        // 跳过目录、JSON 和非 Modified/ 文件
        if (filename.find(MODIFIED_PREFIX) != 0 || is_json_file(filename)) {
            continue;
        }

        std::string id = extract_id(filename);
        if (id.empty()) {
            continue;
        }

        auto it = json_map.find(id);
        if (it == json_map.end()) {
            LOGD("No JSON for: %s", filename.c_str());
            continue;
        }

        // 提取并解析 JSON
        auto json_data = ExtractFile(it->second);
        if (json_data.empty()) {
            LOGW("Failed to extract JSON: %s", it->second.c_str());
            continue;
        }

        std::string entry_name;
        if (!ParseTLConfig(json_data, entry_name)) {
            LOGW("Failed to parse JSON: %s", it->second.c_str());
            continue;
        }

        if (entry_name.empty()) {
            entry_name = id; // fallback
        }

        _index[entry_name] = filename;
        ++total;
    }

    mz_zip_reader_end(&zip);
    LOGI("Indexed %zu textures from %s", total, _entry.file.c_str());
    return true;
}

std::vector<uint8_t> TexturePack::ExtractFile(const std::string &path) const {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, _entry.file.c_str(), 0)) {
        LOGE("Failed to open ZIP: %s", _entry.file.c_str());
        return {};
    }

    size_t size;
    void *data = mz_zip_reader_extract_file_to_heap(&zip, path.c_str(), &size, 0);

    if (!data) {
        mz_zip_reader_end(&zip);
        LOGE("Failed to extract: %s", path.c_str());
        return {};
    }

    std::vector result(static_cast<uint8_t *>(data),
                       static_cast<uint8_t *>(data) + size);
    mz_free(data);
    mz_zip_reader_end(&zip);

    return result;
}

bool TexturePack::ParseTLConfig(const std::vector<uint8_t> &jsonData, std::string &entry_name) {
    try {
        const json config = json::parse(jsonData.begin(), jsonData.end());
        entry_name = config.value("entry_name", "");
        return true;
    } catch (const std::exception &e) {
        LOGW("JSON parse error: %s", e.what());
        return false;
    }
}
