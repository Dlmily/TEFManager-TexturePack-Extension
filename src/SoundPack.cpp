#include "SoundPack.hpp"

#include "Log.hpp"
#include "lib/miniz.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <vector>

namespace {
constexpr std::string_view kSoundPrefix = "Content/Sounds/";
constexpr std::size_t kMaxExtractedSoundBytes = 32U * 1024U * 1024U;

std::string canonical_slashes(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    while (value.find("//") != std::string::npos) {
        value.erase(value.find("//"), 1);
    }
    return value;
}

bool equals_ascii_insensitive(const std::string_view lhs, const std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(lhs[index])) !=
            std::tolower(static_cast<unsigned char>(rhs[index]))) {
            return false;
        }
    }
    return true;
}

bool is_safe_sound_entry(const std::string &entry_name) {
    if (entry_name.empty() || entry_name.front() == '/' || entry_name.find("\\") != std::string::npos ||
        entry_name.find("..") != std::string::npos || entry_name.find(':') != std::string::npos) {
        return false;
    }
    const auto normalized = canonical_slashes(entry_name);
    if (normalized.rfind(kSoundPrefix, 0) != 0 || normalized.size() <= kSoundPrefix.size()) {
        return false;
    }
    const auto dot = normalized.find_last_of('.');
    return dot != std::string::npos && equals_ascii_insensitive(std::string_view(normalized).substr(dot), ".xnb");
}

void add_key_aliases(std::unordered_map<std::string, std::filesystem::path> &index,
                     const std::string &key, const std::filesystem::path &path) {
    if (key.empty()) {
        return;
    }
    index[key] = path;
    const auto slash = key.find_last_of('/');
    if (slash == std::string::npos || slash + 1 >= key.size()) {
        return;
    }
    const auto base = key.substr(slash + 1);
    if (!base.empty() && base.front() == '@') {
        index[key.substr(0, slash + 1) + base.substr(1)] = path;
    } else {
        index[key.substr(0, slash + 1) + "@" + base] = path;
    }
}
} // namespace

SoundPack::SoundPack(PackEntry entry) : entry_(std::move(entry)) {
}

std::string SoundPack::NormalizeAssetKey(const std::string &asset_name) {
    auto normalized = canonical_slashes(asset_name);
    const auto sound_position = normalized.find("Content/Sounds/");
    if (sound_position == std::string::npos) {
        return {};
    }
    normalized = normalized.substr(sound_position);
    const auto extension = normalized.find_last_of('.');
    if (extension != std::string::npos) {
        normalized.erase(extension);
    }
    const auto final_slash = normalized.find_last_of('/');
    if (final_slash != std::string::npos && final_slash + 1 < normalized.size() &&
        normalized[final_slash + 1] == '@') {
        normalized.erase(final_slash + 1, 1);
    }
    return normalized;
}

bool SoundPack::BuildAndExtract(const std::filesystem::path &destination_root,
                                std::unordered_map<std::string, std::filesystem::path> &sound_index) const {
    if (entry_.type != PackType::Terraria) {
        LOGW("Sound pack uses unsupported type: %d", static_cast<int>(entry_.type));
        return false;
    }

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, entry_.file.c_str(), 0)) {
        LOGW("Cannot open sound ZIP: %s", entry_.file.c_str());
        return false;
    }
    std::error_code filesystem_error;
    std::filesystem::create_directories(destination_root, filesystem_error);
    if (filesystem_error) {
        mz_zip_reader_end(&zip);
        LOGE("Cannot create sound cache: %s", destination_root.c_str());
        return false;
    }

    bool extracted_any = false;
    const auto file_count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint index = 0; index < file_count; ++index) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, index, &stat) || mz_zip_reader_is_file_a_directory(&zip, index)) {
            continue;
        }
        const std::string archive_name(stat.m_filename);
        if (!is_safe_sound_entry(archive_name) || stat.m_uncomp_size == 0 ||
            stat.m_uncomp_size > kMaxExtractedSoundBytes) {
            continue;
        }
        const auto output_path = destination_root / std::filesystem::path(canonical_slashes(archive_name));
        std::filesystem::create_directories(output_path.parent_path(), filesystem_error);
        if (filesystem_error || !mz_zip_reader_extract_to_file(&zip, index, output_path.c_str(), 0)) {
            filesystem_error.clear();
            LOGW("Failed to extract sound entry: %s", archive_name.c_str());
            continue;
        }
        const auto canonical_key = NormalizeAssetKey(archive_name);
        add_key_aliases(sound_index, canonical_key, output_path);
        extracted_any = true;
    }
    mz_zip_reader_end(&zip);
    return extracted_any;
}
