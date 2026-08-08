/*******************************************************************************
 * texturepack_extension - TextureManager
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
 * Created: 2026/7/28
 *******************************************************************************/

#include "TextureManager.hpp"
#include "core.hpp"
#include "Log.hpp"

#include <ranges>

TextureManager & TextureManager::Instance()  {
    static TextureManager instance;
    return instance;
}

void TextureManager::LoadAllAsync() {
    std::lock_guard lock(_indexMutex);

    for (const auto &name: g_texture_indexes | std::views::keys) {
        // 检查是否已加载
        {
            std::lock_guard lock2(_loadedMutex);
            if (_loaded.contains(name)) {
                continue;
            }
        }

        // 检查是否已在加载队列
        {
            std::lock_guard lock2(_loadingMutex);
            if (_loading.contains(name)) {
                continue;
            }

            // 启动异步任务
            _cancelFlags[name] = false;
            _loading[name] = std::async(std::launch::async, [this, name]() -> TextureData {
                // 检查是否被取消
                if (_cancelFlags[name].load(std::memory_order_acquire)) {
                    return {};
                }

                // 通过索引查找对应的 TexturePack
                {
                    std::lock_guard lock1(_indexMutex);
                    const auto it = g_texture_indexes.find(name);
                    if (it == g_texture_indexes.end()) {
                        return {};
                    }

                    // 解码纹理
                    auto data = it->second->GetTexture(name);

                    // 如果成功解码，移到 loaded 缓存
                    if (data.IsValid()) {
                        std::lock_guard lock3(_loadedMutex);
                        _loaded[name] = std::move(data);
                    }

                    return data;
                }
            });
        }
    }

    LOGD("Started async loading for %zu textures", g_texture_indexes.size());
}

void TextureManager::LoadAsync(const std::vector<std::string> &names)  {
    for (const auto& name : names) {
        LoadAsync(name);
    }
}

void TextureManager::LoadAsync(const std::string &name) {
    // 检查是否已加载
    {
        std::lock_guard lock(_loadedMutex);
        if (_loaded.contains(name)) {
            return;
        }
    }

    // 检查是否已在加载队列
    {
        std::lock_guard lock(_loadingMutex);
        if (_loading.contains(name)) {
            return;
        }

        // 启动异步任务
        _cancelFlags[name] = false;
        _loading[name] = std::async(std::launch::async, [this, name]() -> TextureData {
            // 检查是否被取消
            if (_cancelFlags[name].load(std::memory_order_acquire)) {
                return {};
            }

            // 通过索引查找对应的 TexturePack
            {
                std::lock_guard lock1(_indexMutex);
                const auto it = g_texture_indexes.find(name);
                if (it == g_texture_indexes.end()) {
                    return {};
                }

                // 解码纹理
                auto data = it->second->GetTexture(name);

                // 如果成功解码，移到 loaded 缓存
                if (data.IsValid()) {
                    std::lock_guard lock2(_loadedMutex);
                    _loaded[name] = std::move(data);
                }

                return data;
            }
        });
    }
}

TextureData TextureManager::Get(const std::string &name) {
    // 先检查是否已加载完成（快速路径）
    {
        std::lock_guard lock(_loadedMutex);
        if (const auto it = _loaded.find(name); it != _loaded.end()) {
            LOGD("Texture already loaded: %s", name.c_str());
            return it->second;
        }
    }

    //  检查是否正在异步加载
    {
        std::lock_guard lock(_loadingMutex);
        if (const auto it = _loading.find(name); it != _loading.end()) {
            LOGD("Texture is loading asynchronously, cancelling and switching to sync: %s", name.c_str());

            // 取消异步任务
            _cancelFlags[name].store(true, std::memory_order_release);

            // 等待异步任务结束（确保资源释放）
            if (it->second.valid()) {
                try {
                    it->second.wait();  // 等待任务结束，但不获取结果
                } catch (...) {
                    // 忽略异常
                }
            }

            // 从加载队列移除
            _loading.erase(it);
            _cancelFlags.erase(name);
        }
    }

    // 同步加载（回退方案）
    LOGD("Synchronously loading texture: %s", name.c_str());
    {
        std::lock_guard lock(_indexMutex);
        const auto indexIt = g_texture_indexes.find(name);
        if (indexIt == g_texture_indexes.end()) {
            LOGW("Texture not found in index: %s", name.c_str());
            return {};
        }

        auto data = indexIt->second->GetTexture(name);
        if (data.IsValid()) {
            std::lock_guard lock2(_loadedMutex);
            _loaded[name] = data;
        }
        return data;
    }
}

TextureData TextureManager::GetWithTimeout(const std::string &name, const int timeoutMs) {
    // 检查是否已加载
    {
        std::lock_guard lock(_loadedMutex);
        if (const auto it = _loaded.find(name); it != _loaded.end()) {
            return it->second;
        }
    }

    // 检查异步加载状态
    {
        std::lock_guard lock(_loadingMutex);
        if (const auto it = _loading.find(name); it != _loading.end()) {
            // 等待一小段时间，看看能否完成
            if (const auto status = it->second.wait_for(std::chrono::milliseconds(timeoutMs)); status == std::future_status::ready) {
                // 异步已完成，取结果
                auto data = it->second.get();
                _loading.erase(it);
                _cancelFlags.erase(name);

                if (data.IsValid()) {
                    std::lock_guard lock2(_loadedMutex);
                    _loaded[name] = data;
                }
                return data;
            }

            // 超时，取消异步任务
            LOGD("Async loading timeout for %s, switching to sync", name.c_str());
            _cancelFlags[name].store(true, std::memory_order_release);
            if (it->second.valid()) {
                try {
                    it->second.wait();
                } catch (...) {}
            }
            _loading.erase(it);
            _cancelFlags.erase(name);
        }
    }

    // 同步加载
    {
        std::lock_guard lock(_indexMutex);
        const auto indexIt = g_texture_indexes.find(name);
        if (indexIt == g_texture_indexes.end()) {
            return {};
        }

        auto data = indexIt->second->GetTexture(name);
        if (data.IsValid()) {
            std::lock_guard lock2(_loadedMutex);
            _loaded[name] = data;
        }
        return data;
    }
}

void TextureManager::CancelAsync(const std::string &name) {
    std::lock_guard lock(_loadingMutex);
    if (const auto it = _loading.find(name); it != _loading.end()) {
        _cancelFlags[name].store(true, std::memory_order_release);
        if (it->second.valid()) {
            try {
                it->second.wait();
            } catch (...) {}
        }
        _loading.erase(it);
        _cancelFlags.erase(name);
        LOGD("Cancelled async loading: %s", name.c_str());
    }
}

void TextureManager::Unload(const std::string &name) {
    {
        std::lock_guard lock(_loadedMutex);
        _loaded.erase(name);
    }
    {
        std::lock_guard lock(_loadingMutex);
        if (const auto it = _loading.find(name); it != _loading.end()) {
            _cancelFlags[name] = true;
            if (it->second.valid()) {
                try {
                    it->second.wait();
                } catch (...) {}
            }
            _loading.erase(it);
        }
    }
    _cancelFlags.erase(name);
    LOGD("Unloaded texture: %s", name.c_str());
}

void TextureManager::UnloadAll() {
    Clear();
}

void TextureManager::Clear() {
    // 先取消所有异步任务
    {
        std::lock_guard lock(_loadingMutex);
        for (auto& [name, future] : _loading) {
            _cancelFlags[name] = true;
            if (future.valid()) {
                try {
                    future.wait();
                } catch (...) {}
            }
        }
        _loading.clear();
    }

    {
        std::lock_guard lock(_loadedMutex);
        _loaded.clear();
    }
    _cancelFlags.clear();

    LOGD("Cleared all texture caches");
}

void TextureManager::WaitAll() {
    std::lock_guard lock(_loadingMutex);
    for (auto &future: _loading | std::views::values) {
        if (future.valid()) {
            try {
                future.wait();
            } catch (...) {
                // 忽略异常
            }
        }
    }
    LOGD("All async tasks completed");
}

void TextureManager::WaitFor(const std::string &name)  {
    std::lock_guard lock(_loadingMutex);
    if (const auto it = _loading.find(name); it != _loading.end() && it->second.valid()) {
        try {
            it->second.wait();
        } catch (...) {}
    }
}

bool TextureManager::IsLoading(const std::string &name) const {
    std::lock_guard lock(_loadingMutex);
    return _loading.contains(name);
}

bool TextureManager::IsLoaded(const std::string &name) const {
    std::lock_guard lock(_loadedMutex);
    return _loaded.contains(name);
}

float TextureManager::GetProgress() const {
    size_t total = 0;
    {
        std::lock_guard lock(_indexMutex);
        total = g_texture_indexes.size();
    }

    if (total == 0) return 1.0f;

    std::lock_guard lock(_loadedMutex);
    return static_cast<float>(_loaded.size()) / static_cast<float>(total);
}

std::string TextureManager::GetStats() const {
    size_t loadedCount = 0;
    size_t loadingCount = 0;
    size_t totalCount = 0;

    {
        std::lock_guard lock(_indexMutex);
        totalCount = g_texture_indexes.size();
    }
    {
        std::lock_guard lock(_loadedMutex);
        loadedCount = _loaded.size();
    }
    {
        std::lock_guard lock(_loadingMutex);
        loadingCount = _loading.size();
    }

    char buf[256];
    snprintf(buf, sizeof(buf),
        "Textures: total=%zu, loaded=%zu, loading=%zu, progress=%.1f%%",
        totalCount, loadedCount, loadingCount,
        totalCount > 0 ? (100.0f * static_cast<float>(loadedCount) / static_cast<float>(totalCount)) : 100.0f);
    return buf;
}

size_t TextureManager::GetLoadedCount() const  {
    std::lock_guard lock(_loadedMutex);
    return _loaded.size();
}

size_t TextureManager::GetLoadingCount() const {
    std::lock_guard lock(_loadingMutex);
    return _loading.size();
}

size_t TextureManager::GetTotalCount() const {
    std::lock_guard lock(_indexMutex);
    return g_texture_indexes.size();
}

std::vector<std::string> TextureManager::GetLoadedNames() const {
    std::lock_guard lock(_loadedMutex);
    std::vector<std::string> names;
    names.reserve(_loaded.size());
    for (const auto &name: _loaded | std::views::keys) {
        names.push_back(name);
    }
    return names;
}

std::vector<std::string> TextureManager::GetLoadingNames() const {
    std::lock_guard lock(_loadingMutex);
    std::vector<std::string> names;
    names.reserve(_loading.size());
    for (const auto &name: _loading | std::views::keys) {
        names.push_back(name);
    }
    return names;
}