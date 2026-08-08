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

#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>
#include <future>
#include <atomic>
#include <chrono>

#include "TexturePack.hpp"

/**
 * @brief 纹理管理器 - 负责纹理的异步预加载、同步加载和缓存管理
 */
class TextureManager final {
public:
    /**
     * @brief 获取单例实例
     */
    static TextureManager& Instance();

    // 禁用拷贝和移动
    TextureManager(const TextureManager&) = delete;
    TextureManager& operator=(const TextureManager&) = delete;
    TextureManager(TextureManager&&) = delete;
    TextureManager& operator=(TextureManager&&) = delete;

    /**
     * @brief 异步加载全部纹理（启动时调用）
     */
    void LoadAllAsync();

    /**
     * @brief 异步加载指定纹理列表
     * @param names 要加载的纹理名称列表
     */
    void LoadAsync(const std::vector<std::string>& names);

    /**
     * @brief 异步加载单个纹理
     * @param name 纹理名称
     */
    void LoadAsync(const std::string& name);

    /**
     * @brief 获取纹理（核心：检测异步加载状态，未完成则取消并同步加载）
     * @param name 纹理名称
     * @return 纹理数据
     */
    TextureData Get(const std::string& name);

    /**
     * @brief 获取纹理（带超时检测，避免无限等待）
     * @param name 纹理名称
     * @param timeoutMs 超时时间（毫秒）
     * @return 纹理数据
     */
    TextureData GetWithTimeout(const std::string& name, int timeoutMs = 100);

    /**
     * @brief 取消异步加载
     * @param name 纹理名称
     */
    void CancelAsync(const std::string& name);

    /**
     * @brief 卸载纹理（释放内存）
     * @param name 纹理名称
     */
    void Unload(const std::string& name);

    /**
     * @brief 卸载所有纹理
     */
    void UnloadAll();

    /**
     * @brief 清空所有缓存
     */
    void Clear();

    /**
     * @brief 等待所有异步任务完成
     */
    void WaitAll();

    /**
     * @brief 等待指定的异步任务完成
     * @param name 纹理名称
     */
    void WaitFor(const std::string& name);

    /**
     * @brief 获取异步加载状态
     * @param name 纹理名称
     * @return true 正在加载中
     */
    bool IsLoading(const std::string& name) const;

    /**
     * @brief 获取是否已加载完成
     * @param name 纹理名称
     * @return true 已加载
     */
    bool IsLoaded(const std::string& name) const;
    /**
     * @brief 获取加载进度
     * @return 0.0 ~ 1.0
     */
    float GetProgress() const;

    /**
     * @brief 获取缓存统计信息
     * @return 格式化的统计字符串
     */
    std::string GetStats() const;

    /**
     * @brief 获取已加载纹理数量
     */
    size_t GetLoadedCount() const;

    /**
     * @brief 获取正在加载纹理数量
     */
    size_t GetLoadingCount() const;

    /**
     * @brief 获取纹理总数
     */
    size_t GetTotalCount() const;

    /**
     * @brief 获取所有已加载的纹理名称
     */
    std::vector<std::string> GetLoadedNames() const;

    /**
     * @brief 获取所有正在加载的纹理名称
     */
    std::vector<std::string> GetLoadingNames() const;

private:
    // 私有构造函数（单例）
    TextureManager() = default;
    ~TextureManager() = default;

    // 已加载完成的纹理缓存
    std::unordered_map<std::string, TextureData> _loaded;
    mutable std::mutex _loadedMutex;

    // 正在异步加载的纹理
    std::unordered_map<std::string, std::future<TextureData>> _loading;
    mutable std::mutex _loadingMutex;

    // 取消标志
    std::unordered_map<std::string, std::atomic<bool>> _cancelFlags;

    // 索引访问互斥
    mutable std::mutex _indexMutex;
};