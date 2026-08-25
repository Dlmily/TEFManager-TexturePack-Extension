# v1.15.0：自定义 XNB 音频数据支持

## 支持范围

XNA `SoundEffectReader` 在 XNB 中存储一个长度前缀的 `WAVEFORMATEX` 格式块和一个长度前缀的波形块。模块从实际格式块读取数据，而不是假定所有上传音频都是 48 kHz 单声道 16-bit。

| 上传 XNB 的 WAVEFORMATEX 参数 | v1.15.0 行为 |
|---|---|
| 未压缩 `WAVE_FORMAT_PCM` | 支持。 |
| 声道数 1 或 2 | 支持，按原声道数保留。 |
| 位深 8、16、24、32-bit integer PCM | 支持；播放前规范化为 16-bit signed PCM。 |
| 采样率 | 支持自定义值，最高 192 kHz；规范化到最接近的 Android OpenSL ES buffer queue 标准播放率。 |
| 压缩 XNB | 拒绝；不会把压缩有效载荷误作 PCM。 |
| ADPCM、浮点、未知/压缩 WAVEFORMATEX | 拒绝并记录诊断；游戏保留原版声音。 |
| 多于两个声道 | 拒绝并记录诊断；游戏保留原版声音。 |

## 转换过程

模块首先严格检查 XNB 边界和 `WAVEFORMATEX` 字段。对于合法 PCM，按照声明的原始位深将每一帧转换为 16-bit signed PCM；8-bit 按 unsigned PCM 解码，24/32-bit 按 little-endian signed integer 解码。对于非 Android 标准采样率，使用线性插值重采样到 8000、11025、12000、16000、22050、24000、32000、44100 或 48000 Hz 中最接近的一项。

`voicePitchOffset` 仍然来自梳妆台保存的 Player 字段。模块先完成 XNB 的一次格式规范化，再以 `2^voicePitchOffset` 计算播放目标并映射到 OpenSL ES 的合法离散播放率。上传的 XNB 文件和角色字段均不会被修改。

## 诊断

模块加载成功时，`audio_diagnostic_v1.15.0.txt` 会记录：

```text
NativePCM register=success key=Content/Sounds/Female_Hit_0 formatTag=1 channels=2 sourceRate=44100 bits=24 blockAlign=6
```

不支持时会记录 `NativePCM register=failed` 和具体原因，例如 `only mono or stereo PCM XNB audio is supported`。

## 回归测试

`tests/CustomXnbAudioFormatTest.cpp` 实际生成并验证以下自定义 XNB：

| 格式 | 覆盖目标 |
|---|---|
| 8-bit、22,050 Hz、立体声 | unsigned PCM、立体声和非 48 kHz 重采样。 |
| 24-bit、44,100 Hz、单声道 | 24-bit sign extension 和 44.1 kHz 输入。 |
| 32-bit、32,000 Hz、立体声 | 32-bit integer down-conversion、立体声和自定义采样率。 |

## References

[1] [MonoGame SoundEffectReader source](https://github.com/dptug/FEZ/blob/master/MonoGame.Framework.Windows/Microsoft/Xna/Framework/Content/SoundEffectReader.cs)

[2] [Microsoft Learn — WAVEFORMATEX](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/ns-mmeapi-waveformatex)

[3] [Android NDK — OpenSL ES for Android](https://developer.android.com/ndk/guides/audio/opensl/opensl-for-android)

[4] [MonoGame SoundEffect API](https://docs.monogame.net/api/Microsoft.Xna.Framework.Audio.SoundEffect.html)
