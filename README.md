# TEFManager-TexturePack-Extension

> TEFManager 材质包扩展模块  
> 基于 TEFKernel 框架的 Terraria 安卓平台材质包加载系统  

---

## 📖 项目介绍

TEFManager 材质包扩展模块，为 Terraria 提供自定义纹理加载能力，支持标准格式和 TLPro 格式材质包，实现纹理的异步预加载和智能缓存管理。

### ✨ 核心特性

- **🎨 纹理替换** - 支持游戏内任意纹理的替换（UI、物品、NPC、弹幕等）
- **📦 多格式支持** - 兼容标准 Terraria 格式和 TLPro 格式材质包
- **⚡ 异步加载** - 启动时自动预加载，不影响游戏流畅度
- **🧠 智能缓存** - 已加载纹理自动缓存，避免重复解码
- **🔄 取消机制** - 支持取消正在进行的异步加载任务
- **🔊 音效包支持** - 支持自定义音效和音乐替换

---

## 📦 材质包制作指南

### 目录结构

#### 标准格式 (Terraria)

```text
材质包名称.zip
└── Content/
    ├── Images/
    │   ├── UI/
    │   │   ├── Inventory_Back.png
    │   │   └── PanelBackground.png
    │   ├── NPC/
    │   │   └── NPC_Head_1.png
    │   └── Tiles/
    │       └── Tile_1.png
    └── Map/
        └── MapBG.png
```

#### TLPro 格式

```text
材质包名称.zip
└── Modified/
    ├── Images/
    │   └── UI/
    │       ├── Inventory_Back.png
    │       └── Inventory_Back.json
    ├── Tiles/
    │   └── Tile_1.png
    │   └── Tile_1.json
    └── NPC/
        └── NPC_Head_1.png
        └── NPC_Head_1.json
```

---

## 🎨 纹理规范

### 图片格式要求

- **格式**：PNG（32bit RGBA）
- **尺寸**：建议为 2 的幂次方（如 64x64、128x128、256x256）
- **色彩**：支持透明通道（Alpha）

---

## 🤝 贡献指南

1. Fork 本仓库
2. 创建功能分支 (`git checkout -b feature/AmazingFeature`)
3. 提交更改 (`git commit -m 'Add some AmazingFeature'`)
4. 推送到分支 (`git push origin feature/AmazingFeature`)
5. 创建 Pull Request

---

## 📄 许可证
本项目采用 GNU Affero General Public License v3.0 许可证。n---

## 🔗 相关链接

- [TEFKernel](https://github.com/eternalfuture-e38299/TEFKernel) - 核心框架
- [问题反馈](https://github.com/Dlmily/TEFManager-TexturePack-Extension/issues)
