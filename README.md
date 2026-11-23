<img src="branding/voxglint.svg" alt="VoxGlint 蓝绿标志" width="80" height="80">

# VoxGlint

**用 Unreal Engine 实时渲染 Minecraft Java 版。**

VoxGlint 由一个 Fabric 客户端模组和一个 Unreal Engine 渲染器组成。Minecraft 照常运行世界、处理交互和生成原版界面。进入主菜单后，UE 接管窗口与输入，在同一窗口绘制地形、生物和第一人称物品，并叠加 Minecraft 的热栏、菜单和聊天框。

VoxGlint is a real-time Unreal Engine renderer for Minecraft Java Edition. [Contribute](CONTRIBUTING.md)

> VoxGlint 是独立社区项目，非 Minecraft 官方产品，与 Mojang 或 Microsoft 无隶属关系，也未获其认可。

![VoxGlint 渲染的日落水景，木船浮在浅水上](docs/media/water-sunset.jpg)

## 当前版本

**v0.28.2 · Windows x64 · Minecraft Java 26.2 · 桥接协议 v28**

- 渲染器对应 Windows x64，显卡与驱动需支持 DirectX 12 / Shader Model 6。Minecraft 实例使用 Fabric Loader 0.19.5 和 Java 25。
- Fabric 模组 JAR 和 Windows 渲染器 ZIP 分开下载，安装时请使用同一版本。Windows 包自带渲染运行时，无需安装 Unreal Engine。[v0.28.2 发布页](https://github.com/LIy-hub/VoxGlint/releases/tag/v0.28.2) · [Windows 运行时条款](docs/windows-runtime-terms.md)

## 画面与控制

- UE 绘制随游戏变化的日夜、天气和体积云。水体有岸边涟漪、船尾迹和瀑布飞溅，水下光按穿过水体的距离衰减和散射。
- 资源包可以提供 LabPBR 材质。VoxGlint 同步动态纹理和模型，并用纹理决定哪些区域发光。染色玻璃透光可在设置中开启，目前默认关闭。
- 画质与色彩在 Minecraft 的「视频设置 → VoxGlint 渲染设置…」中调整。设备支持时可使用硬件光追和路径追踪摄影。

## 安装 Windows 版

1. 打开启动器中 Minecraft Java 26.2 的 Fabric 实例，找到它的游戏目录。已有 `options.txt` 的实例，以该文件所在目录为准。开启版本隔离时，使用该版本单独的游戏目录。
2. 把 `voxglint-0.28.2.jar` 放到这个目录的 `mods` 下。将 Windows ZIP 解压到同一个游戏目录，解压完成后 `minecraftue-renderer` 与 `mods` 同级。[Fabric 模组 JAR](https://github.com/LIy-hub/VoxGlint/releases/download/v0.28.2/voxglint-0.28.2.jar) · [Windows x64 渲染器 ZIP](https://github.com/LIy-hub/VoxGlint/releases/download/v0.28.2/VoxGlint-Renderer-v0.28.2-Windows-x64.zip)
3. 如果电脑缺少 Microsoft Visual C++ v14 x64 运行库，安装 Microsoft 提供的 x64 Redistributable。[官方下载](https://aka.ms/vc14/vc_redist.x64.exe)
4. 从启动器正常启动该 Fabric 实例，模组会自动启动渲染器。[详细目录结构与启动排查](docs/renderer-guide.md#安装并启动-windows-版)

开发者可用 Unreal Engine 5.8.4、JDK 25、MSVC 和 Windows SDK 从源码构建。[构建和编辑器联调](docs/renderer-guide.md#构建与开发启动)

## 一起开发

我很期待和大家一起把 VoxGlint 做下去。很多渲染问题只有换一台机器、一个资源包或一个真实存档才会碰到。如果你愿意带一个复现、一组对照图，或者一段想改的代码来，我们就可以从那里一起做。

—— 项目所有者与维护者 [LIy-hub](https://github.com/LIy-hub)

- **游玩和测试**。首轮征集不同显卡上的 F11 全屏切换和窗口缩放反馈，重点看准星、热栏和菜单点击的位置。遇到切维度、资源重载等异常，也欢迎附版本、复现步骤和日志。[参与首轮实测（#1）](https://github.com/LIy-hub/VoxGlint/issues/1) · [提交 Issue](https://github.com/LIy-hub/VoxGlint/issues/new)
- **Fabric 和资源包**。欢迎补充方块模型、粒子、LabPBR 的兼容样例，或改进 Sodium 设置入口和现有合约测试。[Fabric 桥接](fabric-bridge/)
- **UE 和着色器**。欢迎用现有回放检查水体、玻璃和天气，或在同机位采集帧时间，帮助定位区段上传和渲染的开销。[UE 源码](ue-renderer/Source/) · [回放脚本](ue-renderer/Scripts/)

中文和英文的 Issue、PR 都欢迎。[查看 Issues](https://github.com/LIy-hub/VoxGlint/issues) · [查看 Pull Requests](https://github.com/LIy-hub/VoxGlint/pulls) · [贡献指南](CONTRIBUTING.md) · [桥接协议](protocol/README.md)

## 许可

VoxGlint 自有代码采用 Apache-2.0 许可证。截图中的 Minecraft 游戏内容和商标归各自权利人所有。[Apache-2.0 许可证](LICENSE) · [版权声明](NOTICE) · [第三方声明](THIRD_PARTY_NOTICES.md)
