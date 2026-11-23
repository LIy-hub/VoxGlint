# 第三方组件和素材

根目录的 [Apache License 2.0](LICENSE) 适用于 VoxGlint 的自有代码和素材。下列第三方组件和素材适用各自的权利声明和许可。

## Gradle Wrapper 和 Fabric

- `fabric-bridge/gradlew`、`gradlew.bat` 和 `gradle/wrapper/gradle-wrapper.jar` 来自 Gradle，适用 Apache License 2.0。脚本保留上游版权声明，JAR 内含 `META-INF/LICENSE`。Wrapper JAR 的 SHA-256 与 Gradle 9.3.0 和 9.3.1 公布值一致，构建使用 `gradle-wrapper.properties` 指定并校验的 Gradle 9.6.0。[Gradle 上游校验值](https://gradle.org/release-checksums/)
- Fabric Loom 和 Fabric Loader 从上游 Maven 仓库解析，适用各自的 Apache License 2.0。Fabric Loader 由玩家单独安装。Sodium 是可选的运行时兼容对象，由玩家另行安装。[Fabric Loom](https://github.com/FabricMC/fabric-loom) · [Fabric Loader](https://github.com/FabricMC/fabric-loader) · [Sodium](https://github.com/CaffeineMC/sodium)

## Minecraft

VoxGlint 是独立制作的 Minecraft: Java Edition 模组。Minecraft 及其代码、纹理和其他游戏素材的权利归各自权利人。构建从上游解析用于编译的客户端依赖，运行时从玩家已经安装的游戏和资源包读取画面数据，源仓库和模组 JAR 仅分发项目自身实现。仓库中的实机截图用于展示 VoxGlint，画面所含的 Minecraft 和第三方素材继续适用原有权利和使用条款。VoxGlint 与 Mojang Studios、Microsoft 无隶属关系，未获其背书。[Minecraft EULA](https://www.minecraft.net/en-us/eula) · [Minecraft 使用指南](https://www.minecraft.net/en-us/usage-guidelines)

## Unreal Engine

VoxGlint uses Unreal® Engine. Unreal® is a trademark or registered trademark of Epic Games, Inc. in the United States of America and elsewhere.

Unreal® Engine, Copyright 1998 – 2026, Epic Games, Inc. All rights reserved.

源项目以单独安装的 UE 5.8.4 为依赖。`Prepare-RendererAssets.ps1` 从本机引擎的体积云预设和 Niagara 模板生成本地 .uasset，相关 Epic 内容依其原许可使用。Win64 Shipping 包中的 UE 运行时、烘焙的 Epic 内容和随引擎分发的第三方运行库适用各自条款，随包原有版权及许可声明保持效力。[Unreal Engine EULA](https://www.unrealengine.com/eula/unreal)

发行包中的 Unreal Engine 授权技术仅限作为 VoxGlint 二进制发行版的一部分使用。法律允许的最大范围内，任何与该授权技术相关的明示、默示或法定陈述、保证及条件均予以排除，VoxGlint 贡献者及 Epic 不承担与该授权技术相关的责任。

## 可选显卡组件

DLSS、Streamline、FSR 和 XeSS 的适配代码保留在本仓库。使用各自适用于 UE 5.8 和 Win64 的官方插件，可本地构建相应的超分和帧生成功能。厂商插件包及 SDK 按各上游包所附的许可使用。[NVIDIA DLSS](https://developer.nvidia.com/rtx/dlss) · [AMD FSR for Unreal Engine](https://gpuopen.com/learn/ue-fsr/) · [Intel XeSS Unreal Engine Plugin](https://github.com/GameTechDev/XeSSUnrealPlugin/releases/tag/v3.2.1)

## 月面数据

可选的真实月面由 NASA 的 CGI Moon Kit 生成。资料署名为 NASA's Scientific Visualization Studio，制图 Ernie Wright，科学贡献 Noah Petro。底层数据来自 LRO 的 LOLA 和 LROC 仪器团队，彩色拼接数据由 Arizona State University 收集。绘制月面的脚本随源仓库提供，原始 TIFF 和生成的纹理留在本机生成目录。包含这些纹理的二进制包也保留本段署名。[NASA CGI Moon Kit](https://svs.gsfc.nasa.gov/4720/) · [NASA 媒体使用指南](https://www.nasa.gov/nasa-brand-center/images-and-media/)
