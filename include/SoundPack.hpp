#pragma once

#include "TexturePack.hpp"

#include <filesystem>
#include <string>
#include <unordered_map>

class SoundPack {
public:
    explicit SoundPack(PackEntry entry);

    /**
     * 安全提取 ZIP 中 Content/Sounds/ 的 .xnb 文件。
     * 同一资源键以后处理的包覆盖先前包，匹配 BasePackManager 的优先级语义。
     */
    bool BuildAndExtract(const std::filesystem::path &destination_root,
                         std::unordered_map<std::string, std::filesystem::path> &sound_index) const;

    /** 将路径、扩展名和 @ 前缀规范化为游戏运行时资源键。 */
    static std::string NormalizeAssetKey(const std::string &asset_name);

private:
    PackEntry entry_;
};
