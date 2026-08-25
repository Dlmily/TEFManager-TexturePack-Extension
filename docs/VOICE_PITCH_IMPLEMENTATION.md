# v1.14.0：梳妆台角色音调与上传音效包

## 设计目标

v1.14.0 删除了所有内置 Waifu XNB、内置资源生成器和内置回退。模块只从玩家上传的标准 Terraria ZIP 中提取 `Content/Sounds/*.xnb`。没有成功索引上传 XNB 时，模块保持游戏原版声音，不会附带或自动生成任何默认声音。

## 真实角色设置来源

Terraria 1.4.5 的角色创建界面和梳妆台均提供受击声音类型与音调调整。当前 Android IL2CPP 元数据已确认 `Terraria.Player` 包含：

| 字段 | 类型 | 用途 |
|---|---|---|
| `voiceVariant` | `int` | 当前受击声音类型；真机日志确认三个 UI 项依次读为 `1`、`2`、`3`。 |
| `voicePitchOffset` | `float` | 角色保存的受击声音音调偏移。 |

模块在 `Player.PlayHurtSound()` Hook 中仅读取这两个 Player 实例字段。它不写 Player 字段、不修改 `.plr` 文件、不持久化自己的音调设置，也不修改上传的 XNB 文件。

## 音调等价实现

tModLoader 的 `SoundStyle.Pitch` 文档说明：XNA/FNA 中 `0.0` 是原始音调，`-1.0` 为低一个八度，`1.0` 为高一个八度。对固定 PCM 波形，等价的播放速率倍率为：

```text
sample_rate = 48000 × 2 ^ voicePitchOffset
```

模块先拒绝非有限浮点数，再将读取值限制为 `[-1.0, 1.0]`，然后将计算得到的采样率传给 Android OpenSL ES `SLDataFormat_PCM`。原始 XNB 波形数据保持不变；每次受击都直接使用梳妆台保存的最新字段值。

## 三项固定映射

| 真机 `voiceVariant` | 上传 ZIP 需要提供的规范资源键 |
|---:|---|
| `1` | `Content/Sounds/Female_Hit_1` |
| `2` | `Content/Sounds/Female_Hit_2` |
| `3` | `Content/Sounds/Female_Hit_0` |

文件名可带或不带 `@` 前缀，例如 `Content/Sounds/@Female_Hit_1.xnb` 与 `Content/Sounds/Female_Hit_1.xnb` 都会建立兼容索引。三个值均固定映射，不使用受击次数轮换。

## 验证日志

安装后，在梳妆台调整音调并触发受击。`audio_diagnostic_v1.14.0.txt` 预期会包含：

```text
Player.voicePitchOffset.field.valid=1 Player.voicePitchOffset.field.size=4
PlayerHurtSound voiceVariant=2 voicePitchOffset=<当前保存值> play=success key=Content/Sounds/Female_Hit_2 original=suppressed
```

同一个声音设置连续受击时，`voiceVariant` 与 `key` 保持不变；调整梳妆台音调后，只有 `voicePitchOffset` 改变。

## References

[1] [Terraria Wiki — Character](https://terraria.wiki.gg/wiki/Character)

[2] [tModLoader API — SoundStyle.Pitch](https://docs.tmodloader.net/docs/stable/struct_sound_style.html)
