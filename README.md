# TEFManager TexturePack Extension v1.15.1

这是一个**独立、完整、可编译**的 Terraria Android 模块工程。它以公开的 TEFManager-TexturePack-Extension 材质替换工程为基础，加入玩家上传 ZIP 的标准 Terraria XNB 音效扫描、SoundEffect XNB PCM 解析、Android OpenSL ES 多实例播放，以及三项角色受击声音固定映射。工程不包含任何内置音效资源。

## 工程内容

| 目录或文件 | 内容 |
|---|---|
| `src/core.cpp` | 模块入口、材质替换、声音 Hook、诊断输出、`Player.PlayHurtSound` 的固定映射。 |
| `src/SoundPack.cpp` | ZIP 中 `Content/Sounds/*.xnb` 的安全扫描、解压和 `@` 前缀别名兼容。 |
| `src/XnbSound.cpp` | 未压缩 Terraria/XNA `SoundEffectReader` XNB 解析器。 |
| `src/AndroidPcmPlayer.cpp` | Android OpenSL ES 多实例播放后端；将上传的合法自定义 PCM 规范化为兼容的 16-bit PCM。 |
| `CMakeLists.txt` | Linux 与 Android `arm64-v8a` / `armeabi-v7a` 构建定义。 |

## 三项角色受击固定映射

真机 Android IL2CPP 诊断确认三项 UI 声音设置使用 `voiceVariant=1`、`2`、`3`。模块不使用全局轮换计数器；相同设置每次受击都固定使用玩家上传包中对应的 PCM。模块同时读取梳妆台保存的 `voicePitchOffset`，以 XNA `Pitch` 语义通过采样率倍率 `2^voicePitchOffset` 输出声音；不会写入或维护模块自己的音调值。

| `voiceVariant` | 固定资源键 |
|---:|---|
| `1` | `Content/Sounds/Female_Hit_1` |
| `2` | `Content/Sounds/Female_Hit_2` |
| `3` | `Content/Sounds/Female_Hit_0` |
| `0` | 回退至 `Content/Sounds/Female_Hit_0` |

`Player_Killed` 先由 `ContentManager.LoadSoundEffect` 关联到已上传 PCM；若运行时直接调用 `SoundEffect.Play()`，模块会立即播放该 PCM 并在成功时抑制原版。若它创建 `SoundEffectInstance`，既有实例播放替换链仍会生效。

## 音效包格式

标准音效包使用 ZIP，并将 XNB 放在 `Content/Sounds/`。同一个资源名可带或不带 `@` 前缀，模块会建立兼容别名。

```text
my-sound-pack.zip
└── Content
    └── Sounds
        ├── @Female_Hit_0.xnb
        ├── @Female_Hit_1.xnb
        ├── @Female_Hit_2.xnb
        └── @Player_Killed.xnb
```

当前 Android PCM 后端支持 **未压缩 XNB、WAVE_FORMAT_PCM、单声道或立体声，以及 8/16/24/32-bit 整数 PCM**。XNB 可声明自定义采样率；模块会先按 XNB 的真实 `WAVEFORMATEX` 参数读取数据，转换为 OpenSL ES 兼容的 16-bit PCM，并重采样到最接近的 Android 标准播放率。压缩 XNB、ADPCM、浮点 PCM 和多于两个声道的数据会被明确记录为不受支持并保留游戏原声，而不会被错误播放。若未从玩家上传 ZIP 成功索引音效，模块不安装任何内置替换，游戏会保持原版声音。

## 依赖

构建需要：

| 工具 | 版本或用途 |
|---|---|
| CMake | 3.22 或更高版本 |
| C++ 编译器 | 支持 C++20 |
| Android NDK | r26 或兼容版本；Android 构建需要 OpenSL ES。 |
| TEFKernel 头文件 | 已包含于 `include/tefkernel-cpp-wrapper/`。 |

## 构建

### Linux 编译检查

```bash
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Debug
cmake --build build-linux -j2
```

### Android arm64-v8a

```bash
cmake -S . -B build-android-arm64 \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-android-arm64 -j2
```

### Android armeabi-v7a

```bash
cmake -S . -B build-android-arm \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=armeabi-v7a -DANDROID_PLATFORM=android-24 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-android-arm -j2
```

生成库分别为：

```text
build-android-arm64/libmodule.android.arm64.so
build-android-arm/libmodule.android.arm.so
```

## 验证方式

上传一个符合上述目录结构的标准 Terraria 音效 ZIP 后，触发受击并读取模块私有目录中的 `audio_diagnostic_v1.15.1.txt`。成功时应出现：

```text
Player.voicePitchOffset.field.valid=1 Player.voicePitchOffset.field.size=4
PlayerHurtSound voiceVariant=1 voicePitchOffset=<已保存值> play=success key=Content/Sounds/Female_Hit_1 original=suppressed
```

在梳妆台调整角色受击音调后，下一次受击日志中的 `voicePitchOffset` 应变为对应保存值；模块仅以该值改变 OpenSL ES 播放采样率，并不会改写 XNB 文件或 Player 字段。

## 许可证

工程保留原项目的 GNU AGPLv3-or-later 许可证声明。用户自行上传的音效资源应仅在拥有相应使用权的条件下使用和分发。
