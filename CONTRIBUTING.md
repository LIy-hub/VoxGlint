# 参与 VoxGlint 开发

欢迎用中文或英文提交问题、测试结果和代码。实机上遇到的一个可复现问题，就能帮我们找到应该先改的地方。

## 报告问题

先看看同类问题是否已有讨论。可以补充新的复现条件或结果，也可以为独立问题开一条 Issue。[查看 Issues](https://github.com/LIy-hub/VoxGlint/issues) · [提交 Issue](https://github.com/LIy-hub/VoxGlint/issues/new)

- 写清楚 VoxGlint、Minecraft、Fabric Loader 和显卡驱动版本，以及显卡型号、资源包或相关模组、画质档和渲染后端。自行构建时请给出提交号和 UE 版本。
- 从启动或进入存档开始给出最短复现步骤、预期画面和实际结果，并注明是否每次出现。窗口交接、玻璃透光和资源重载分别在什么时候出问题会直接影响定位。
- 对照图请尽量固定机位、分辨率、时间、天气和曝光。帧率问题可附同设置下的帧时间对照。日志优先使用游戏目录的 `logs/latest.log` 和相关 UE 日志，分享前删去用户名、服务器地址和私人路径。

## 找到对应代码

| 范围 | 从这里入手 |
| --- | --- |
| Minecraft 模型、资源包、HUD 和设置 | [Fabric 客户端源码](fabric-bridge/src/main/java/dev/minecraftue/bridge/) · [合约测试](fabric-bridge/src/atlasContract/java/dev/minecraftue/bridge/) |
| UE 场景、输入、光照、水体和天气 | [UE C++](ue-renderer/Source/MinecraftUEViewer/) · [着色器](ue-renderer/Shaders/) · [回放和采集脚本](ue-renderer/Scripts/) |
| TCP 消息和共享内存格式 | [v28 桥接协议](protocol/README.md) |

## 构建和验证

用 JDK 25 运行 Fabric 的合约测试。Gradle Wrapper 在第一次运行时需要联网，依赖下载完成后可用 `--offline`。从仓库根目录执行。

```powershell
.\fabric-bridge\gradlew.bat -p .\fabric-bridge check
```

`check` 覆盖图集、区段光照与玻璃、HUD、共享内存、设置及天气传输。也可单独运行 `verifyTerrainAtlas`、`verifySettings`、`verifySectionGlass` 或其他对应任务。UE 改动用 Windows x64 和 UE 5.8 构建，并按改动跑一条相关回放或做一次游戏内测试。[安装与开发启动](docs/renderer-guide.md#构建与开发启动) · [游戏内验证步骤](docs/renderer-guide.md#验证)

对画质或性能改动，提交同一场景下的前后截图或帧时间数据，注明输出和内部分辨率。改桥接消息或共享内存布局时，一并检查 Java、C++ 和协议文档，确认两端能识别同一套格式。[性能采集](docs/renderer-guide.md#ue-性能采集)

## 提交 Pull Request

从 GitHub fork 仓库，在自己的分支上提交。说明改动要解决的问题、关联的 Issue、验证方式和实际结果。渲染器行为、跨进程协议或广泛的模组兼容改动，可以先开 Issue 讨论设计和验收场景。[Pull Requests](https://github.com/LIy-hub/VoxGlint/pulls)

项目自有代码采用 Apache-2.0。提交新增资源时请注明来源和再分发许可。游戏本体、UE 引擎文件和厂商插件由开发者自行获取。[Apache-2.0 许可证](LICENSE) · [版权声明](NOTICE) · [第三方声明](THIRD_PARTY_NOTICES.md)
