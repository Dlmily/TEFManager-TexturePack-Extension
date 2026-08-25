#include "SoundPack.hpp"
#include "XnbSound.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "usage: SoundPackUploadTest <sound-pack.zip> <output-directory>\n";
        return 2;
    }
    PackEntry entry;
    entry.file = argv[1];
    entry.type = PackType::Terraria;
    std::unordered_map<std::string, std::filesystem::path> index;
    const std::filesystem::path output(argv[2]);
    std::error_code error;
    std::filesystem::remove_all(output, error);

    if (!SoundPack(entry).BuildAndExtract(output, index)) {
        std::cerr << "sound pack extraction failed\n";
        return 1;
    }
    const std::string key = "Content/Sounds/Female_Hit_0";
    const auto found = index.find(key);
    if (found == index.end() || !std::filesystem::exists(found->second) ||
        !index.contains("Content/Sounds/@Female_Hit_0")) {
        std::cerr << "standard and @ aliases were not indexed\n";
        return 1;
    }
    XnbSoundEffectData sound;
    std::string parse_error;
    if (!ParseXnbSoundEffect(found->second, sound, parse_error) || sound.waveform.empty()) {
        std::cerr << "extracted XNB cannot be parsed: " << parse_error << '\n';
        return 1;
    }
    std::cout << "uploaded sound pack test passed: " << index.size()
              << " aliases, " << sound.waveform.size() << " PCM bytes\n";
    return 0;
}
