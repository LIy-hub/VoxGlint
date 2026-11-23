# VoxGlint 使用与渲染开发指南

[项目首页](../README.md) · [贡献指南](../CONTRIBUTING.md) · [v28 桥接协议](../protocol/README.md)

本文对应 VoxGlint v0.28.2。PowerShell 示例从仓库根目录执行，`$ue` 表示 Unreal Engine 5.8 的安装根目录。

[安装并启动](#安装并启动-windows-版) · [构建与开发启动](#构建与开发启动) · [操作与摄影](#在-ue-里操作) · [验证](#验证)

## 窗口与桥接

单窗口模式下，启动加载显示在 Minecraft 原生窗口中。到主菜单以后，UE 绘制出第一张界面帧才接管显示和输入，Minecraft 的 GLFW 窗口在本次运行中保持隐藏。Minecraft 保留游戏逻辑和交互判定，UE 绘制地形、动态对象和第一人称模型，MC 的 HUD、物品栏、暂停菜单和聊天界面透明叠加在其上。

UE 首次连接时采用 Minecraft 窗口的位置和客户区大小。视频设置里的「全屏」控制 UE 无边框全屏，进入控制模式后也可以按 F11 切换。退出全屏会恢复窗口的位置和大小。窗口模式下可移动、最小化、最大化和还原，也可拖动边缘分别调整宽和高。尺寸稳定约 100 毫秒后，MC 离屏目标跟随 UE 客户区的实际像素尺寸。

单窗口交接后，同一块显卡上的 Minecraft OpenGL 或 Vulkan 可以与 UE D3D12 通过三张共享纹理传输 HUD。MC 在 GPU 上复制离屏界面，UE 采样最近完成的纹理并合成。共享路径在 Performance、Balanced 档的默认上限为 120 FPS，其他设置为 165 FPS。可用 `Start-Bridge.ps1 -BridgeHudMaxFps 144` 或自动启动 JVM 参数 `-Dminecraftue.hudMaxFps=144` 指定共享路径上限。交接前及显卡、驱动或后端不支持互操作时，使用系统内存路径，隐藏的 MC 客户端上限为 120 FPS。两条路径同时受游戏帧率设置和 UE 当前限帧约束。用 `-BridgeCpuHud` 可强制 CPU 路径作对照。`-BridgeTrace` 记录共享路径的 MC 绘制、发布、UE 采用及槽位不足频率。

Minecraft 发送相机、环境、方块纹理图集，以及有效渲染距离内已加载的区段网格和光源。Fabric 在 Minecraft 区段编译时记录材质语义、面朝向、顶点颜色和方块光/天空光。挖放及光照变化更新受影响的 16×16×16 区段。UE 绘制 Minecraft 同步的 3D 方块选中线框、挖掘裂纹和粒子。

两端使用 v28 协议。升级时请同时替换 Fabric JAR 和 Windows 渲染器。每个会话只建立一条双向本机 TCP 连接，开发启动用 `127.0.0.1:39182`，Mod 自动启动使用系统分配的回环端口。键盘、窗口、HUD 握手、图集、区段引用与归还、世界事件、天气和动态网格沿用可靠连接。区段网格、完整区段光源与 MC 权威相机使用 Windows 共享内存。区段保留 v26 的几何、水面、岸线和发光数据布局。

玩家附近的挖放和光照变化在两端获得独立调度预算。重复更新按区段合并，远处区段继续流入。第一人称游玩态沿用 UE 本地转头、MC 灵敏度与 GCD 步进，通过 `overrideEpoch` 和 `teleportEpoch` 处理服务端强制视角及传送。第三人称、平滑相机、骑乘和游戏菜单使用 MC 权威相机。

在 MC 视频设置里调整渲染距离、最大帧率、垂直同步和空闲降帧模式，可即时同步到 UE。「无限制」取消 UE 的常规帧率上限。最小化时上限为 10 FPS，选择「离开时降低帧率」后，无操作超过 1 分钟和 10 分钟时上限分别降到 30 和 10 FPS。视距调高后，等待 MC 加载区块，已加载的区块从近到远传给 UE。视距调低时，超出范围的区块逐帧卸载，保留仍在范围内的网格。视距支持原版的 2–32 区块范围，实际范围取 MC 的有效视距。

## 水体调试

### P1 逐面水体

开发版控制台中，`mc.Water.P1 0` 使用 P0 材质响应，`mc.Water.P1 1` 使用逐面水体。几何沿用 Minecraft 提交的轮廓。

| `mc.Water.Debug` | 视图 |
| --- | --- |
| 0 | 正常渲染 |
| 1 | 自由顶面绿、贴玻璃紫、开放跌水蓝、普通裸露侧面橙、底面黄 |
| 2 | 水平流向箭头与视觉流强 |
| 3 | 岸距。紫色表示地表网格在当前面无有效匹配 |
| 4 | 水面高度，绿色通道显示地表网格匹配 |
| 5 | 卷气和入水冲击 |
| 6 | 岸湿和薄膜 |
| 7 | 可发生雨纹的位置 |

用 `python ue-renderer/Scripts/validate_water_v26.py <输出目录>` 生成两个独立的世界流样本。分别运行 `./ue-renderer/Scripts/Run-SkyLakeBench.ps1 -EngineRoot $ue -HighOnly -WaterP1 0` 和同一命令的 `-WaterP1 1` 在同一项目生成四种天气的对照和整帧计时 CSV。加 `-GpuEvents` 可同时捕获每种天气的 UE GPU 事件，`-OutputDirectory` 为每轮指定一个新目录。

### P2 水下光学

P2 在色调映射和 Minecraft HUD 合成前，按实际水面、眼位和遮挡逐像素计算水程。下表面仍由 Single Layer Water 绘制，水侧合成处理 RGB 消光和入散射。Balanced 及以上会在相机附近计算折射太阳光、体积散射和焦散。气泡柱来自 Fabric 的实际方块状态。

开发版对照开关是 `mc.Water.P2`、`mc.Water.P2.Interface`、`mc.Water.P2.Volume`、`mc.Water.P2.Caustics`、`mc.Water.P2.Distortion`、`mc.Water.P2.Film`、`mc.Water.P2.Plankton` 和 `mc.Water.P2.Bubbles`。设为 0 关闭相应效果，设为 1 启用。`mc.Water.UnderwaterInterface 0` 可单独关闭下表面折射。

| `mc.Water.P2.Debug` | 视图 |
| --- | --- |
| 0 | 正常渲染 |
| 1 | 红通道为湿侧，绿通道表示近处几何判定，蓝通道表示 Minecraft 介质回退 |
| 2 | RGB 依次编码入水距离、出水或遮挡距离、水中光程 |
| 3 | RGB 依次显示水前不透明深度、水面深度差和独立透明层覆盖 |
| 4 | RGB 透射率 |
| 5 | 平面斯涅尔临界角。红色为 Fresnel 反射分量，绿色为透射分量 |
| 6 | 体积入散射 |

用 `./ue-renderer/Scripts/Run-WaterP2Fixture.ps1 -EngineRoot $ue -Output <新目录> -Fixture flat -FixedExposure -Probe` 执行合成回放。可用 `-RenderPercent 62` 切换内部比例，`-RayTracing` 测试硬件光追，`-PerfRounds 3 -GpuEvents` 输出计时。

### P3 水面交互与几何波浪

High 用 256² 网格模拟相机周围 32 × 32 米的浅水波纹，Ultra 用 512² 网格覆盖 48 × 48 米。玩家、活动实体、船体、瀑布和露天暴雨共享同一个模拟。波纹在岸壁反射，在网格边缘 2 米内淡出。船体由动态网格的外形和表面运动识别。

开阔深水的水平静水顶面使用 FFT 和局部波高产生最大 3–8 厘米的垂直起伏。岸距 1.5 格以内的顶点保持原高。瀑布落点使用 Niagara 体积雾和飞溅水珠，冰邻接标记控制冷水光学预设和波浪阻尼。

开发版可用 `mc.Water.P3 0` 关闭整组效果，或用 `mc.Water.P3.WPO 0` 单独锁定几何。 `mc.Water.Debug 8` 查看波高，`9` 查看尾迹白沫，`10` 查看几何位移。运行 `./ue-renderer/Scripts/Run-WaterP3Fixture.ps1 -EngineRoot $ue -Output <新目录>` 回放入水、船尾迹和瀑布，或附加 `-Quality Ultra -Scenario storm` 检查暴雨、屋檐和闪电。

## VoxGlint 设置

在 Minecraft 中打开「选项 → 视频设置 → VoxGlint 渲染设置…」。若同时安装了 Sodium（钠），视频设置页右上角会自动出现「VoxGlint 渲染设置…」入口，点击可无缝进入并在返回时回到 Sodium 界面。

新版设置界面采用左侧 7 大分类 + 顶栏搜索与实时状态（硬件光追路径、UE FPS、内部渲染分辨率）+ 右侧详情/性能开销面板布局，并在调整曝光、洞穴保底亮度、调色、泛光、云量、大气雾、水体清澈度等视觉滑块时自动切入**透明实时预览**，支持右键单项恢复默认。UE 渲染器按 100 ms 轻量化时间戳检测配置变更，拖动滑块即可实时看到变化。

1. **概览与显示**：总画质预设（性能 / 均衡 / 高 / 极高，自定义后自动标示）、硬件光追开关、内置硬件优化模板（含 RTX 3060 实测档）、抗锯齿与超分质量档、动态分辨率与目标帧率、画面锐化，以及与 MC 实时同步的视距、最大帧率、垂直同步、空闲降帧与无边框全屏。
2. **光照与阴影**：Lumen 全局光照与反射档位、半透明前层反射、阴影质量、VSM / 硬件光追阴影、近景接触阴影、日影连续更新、方块动态光源质量与 MegaLights 开关、方块光照与自发光亮度倍率，以及默认关闭的**染色玻璃透光（实验性）**开关。
3. **天空与天气**：体积云质量、云层结构（单层 / 双层 / 三层含卷云）、云层地面阴影、晴天云量、体积雾档位、大气雾浓度、雨后地面打湿与积水反光模式、雨雪粒子密度。
4. **水体**：水体综合预设、P1 逐面水体、P2 水下物理光学、P3 浅水涟漪与瀑布水雾、FFT 水面几何波浪、水下太阳光柱、水底焦散、水下浮游微粒与气泡柱、正向「水体清澈度」、水下光雾强度、出水镜头水膜与资源包水法线混合。
5. **色彩与后处理**：曝光模式、画面亮度（-2.0 至 +2.0 EV）、明暗平衡（局部曝光）、夜晚与洞穴保底亮度、色彩风格预设（自然 / 暖阳电影 / 清透鲜艳 / 冷冽高对比）与饱和度/对比度/色温、泛光（Bloom）强度、镜头暗角与动态模糊。
6. **材质与表面**：自动贴图凹凸预设与细调（强度、弱色差阈值、最大倾角，退出自动重载资源包）、无缝连接玻璃开关（自动刷新区块网格）、多级纹理与各向异性过滤同步。
7. **摄影与高级**：路径追踪摄影（立即拍摄、打开照片目录、1080p–4K 分辨率、64–1024 采样、可选仅 PNG 或 PNG+EXR、AI 降噪）、实时热生效的右上角 UE 性能浮窗、HUD 刷新上限、配置档案导入/导出，以及可折叠的 **专家 / 引擎微调**（保留 TSR 历史缓冲、SMRT 射线数、反射反弹/粗糙度/屏幕追踪、BLAS Region 粒度、地形与动态对象光追范围预算、精确点光源预算和体积雾格宽）。

切换总画质预设会重置性能画质与专家覆盖项，曝光、洞穴亮度、调色、泛光、水体清澈度、云量、自动法线、连接玻璃与拍照等个人偏好继续生效。VoxGlint 继续读写实例目录的 `minecraftue-quality.txt`、`minecraftue-graphics.txt` 和 `minecraftue-settings.properties`。

在「概览与显示」可热切换 DLSS、FSR 和 XeSS 超分，当前设备不支持所选算法时回退到 TSR。帧生成倍数按当前设备能力列出，默认请求 2×。切换帧生成后端需重启渲染器。

资源包重载后会同步新的图集、区段网格和动态模型。地形半透明排序使用网格编译时的视点。背包和快捷栏的图标由 Minecraft 界面绘制。

## 画质档位

自动启动可加 JVM 参数 `-Dminecraftue.quality=Balanced`。开发启动使用 `./Start-Bridge.ps1 -EngineRoot $ue -BridgeQuality Balanced`，直接运行 UE 可用 `-BridgeQuality=Balanced`。接受四个档名或 `0`–`3`。默认 `Baseline` 沿用 v0.22 的自动主分辨率及现有画质配置，其他档位以临时质量分组运行，正常退出仍保存用户原有的 UE 基线。窗口、视距、最大帧率和垂直同步由 Minecraft 的视频设置控制。

| 档位 | TSR 输入 / 历史倍率 | 光照 | 阴影 / 体积效果 |
| --- | --- | --- | --- |
| 0 · Performance | 62% / 100% | SSR、MC 顶点补光，48 盏延迟点光 | 阴影 1 档，高度雾 |
| 1 · Balanced | 70% / 100% | 硬件 Hit Lighting，最多 512 盏 MegaLights | 阴影 2 档，降低云雾采样 |
| 2 · High | 75% / 100% | 硬件 Hit Lighting，最多 1024 盏 MegaLights | 阴影 3 档，MegaLights 4 样本，Lumen 双线性探针插值 |
| 3 · Ultra | 90% / 100% | 硬件 Hit Lighting，最多 2048 盏 MegaLights | 阴影 3 档，MegaLights 4 样本，更密的云采样 |

High 在每个方向以一半分辨率追踪反射，云投影和云对天空光的遮蔽使用 256 × 256 纹理。High 保留前层半透明反射、完整的体积云和 8 条方向光软阴影射线。Ultra 恢复全分辨率反射和 512 × 512 云影，并连续更新太阳阴影方向。

设置里内置「RTX 3060 均衡高画质」，采用 70% 输入、一次反射反弹和 2 × 2 区块的光追地形区域。区域大小随渲染器重启生效。早期对照中的 `RTX-3060-High-75` 将输入改为 75%，其余这两项相同。

硬件档需要 DX12 SM6 和受支持的光追设备。条件不足时使用 SSR 和顶点补光。菜单画质优先于启动档。`Baseline` 恢复项目基线。实例目录的 `minecraftue-quality.txt.effective` 会在切档时写出实际 CVar、硬件光照和 MegaLights 的状态。直接启动 UE 时可传 `-BridgeQualityFile="配置文件的绝对路径"` 连接画质配置文件。

## 动态纹理

`AtlasPatch` 同步 Minecraft 已绘制的图集动画，例如水流、岩浆、火焰、下界传送门和资源包定义的方块或物品动画。Fabric 异步读回变化区域，保留原版帧顺序、插值、透明度和 sprite 边缘填充。UE 原位更新地形 Albedo 图集的各级 mip，以及动态模型已引用的图集基础层。动画复用现有网格和材质。法线和 LabPBR 材质图随资源包重载更新。地图画面等 `DynamicTexture` 内容沿用纹理行同步，同尺寸更新复用 UE 贴图。

开启 `-BridgeTrace` 后，UE 日志中的 `AtlasPatch` 记录当前场景的补丁包数、更新区域数、方块图集 mip 掩码和传输字节数。可在原地观察火焰或传送门，再切换资源包，检查更新后的图集继续播放。

## 粒子

Minecraft 粒子引擎负责生成、更新和剔除粒子。Fabric 以最高 60 Hz 导出它实际提交的精灵和模型网格。原版的方块碎屑、烟雾、火焰、药水、烟花、振动粒子、物品拾取和远古守卫者幻影都走通用采集。使用 Fabric 粒子工厂注册的模组粒子可以沿用自己的位置、大小、转动、逐帧精灵、颜色和透明度。自定义粒子组可沿用 Minecraft 的精灵、模型和 `submitCustomGeometry` 提交接口。粒子图集和单独贴图随当前资源包读取，重载时重新建立引用。

UE 依据提交时的贴图和混合状态绘制，并使用场景深度遮挡。支持透明度遮罩、普通 Alpha、预乘 Alpha、加法混合和关闭深度检测。自定义网格支持三角形、四边形、三角带和三角扇。模组专用 GLSL、多贴图效果、点线图元及绕过 Minecraft 提交接口的 GPU 绘制需要对应的 UE 实现。开启 `-BridgeTrace` 可在 UE 日志中查看 `ParticleBatch`。

## 动态网格

本地玩家的身体和装备、第一人称手臂和主副手物品、方块实体及生物共用 `DynamicMesh` 和 `InstanceBatch`。桥接从 Minecraft 原版模型提交阶段采集已烘焙的顶点、颜色和光照。本地玩家的身体和第一人称手部分别以最高 60 Hz 更新姿态。身体的脚下坐标随相机逐帧同步，第三人称正面和背面沿用原版相机方向。第一人称手部沿用原版的换手、挥动与使用姿势，按 F1 或切到第三人称时隐藏。其他世界对象最多以 30 Hz 采样，优先覆盖 64 格内的近处对象，并受 MC 视距和每批顶点预算限制。

批次可引用同一份网格定义。掉落物和已跟踪的近处对象有稳定的部件 ID，UE 在世界快照之间插值根位置和旋转，肢体形变继续采用原版快照。连续快照通常带来约一个采样间隔的显示延迟，超过四格位移直接采用新姿态。仅变换变化时复用原组件和网格。硬件档优先为本地玩家分配光追部件，再选择约 32 格内的遮罩世界对象，默认总预算为 96 个部件，世界对象保留至 36 格。实体卸载、消失或移出范围时由下一批清除，切维度或断线时清空动态场景。

玩家皮肤使用写入深度的透明度遮罩，皮肤外层的部分透明像素按遮罩阈值处理。第一人称画面显示相机层的手和物品，身体和手持装备的投影由原版世界模型及主副手挂点决定。动态网格在软件路径同步方块光和天空光补光，透明混合物体关闭投影。贴图随资源代次刷新，支持方块和物品图集、单独模型贴图及可读取的动态纹理。附魔光效、专用多纹理着色器和精确的透明面排序仍待接入。

地形沿用 Minecraft 当前图集的多级纹理（mip）和贴图边缘填充。默认地形模式在远处使用三线性缩小，近处保留像素边界。可在 Minecraft 的视频设置中调整多级纹理与纹理过滤，退出设置界面后 Minecraft 重载图集，UE 随后同步。沿长路斜看时可用各向异性过滤保留砖缝，UE 支持 2×、4× 或 8×。对照时可保持 MC 的各向异性设置，以 `-BridgeTerrainNoAF` 启动 UE，在同一图集上关闭各向异性。用 `-BridgeTerrainMip0` 可查看仅采样基础层的效果。

渲染桥接世界时，F3 显示 UE 的平均帧率及帧时间、MC 客户端帧率、UE 视距及已接收的区块列数、已加载及待上传的区段数，以及 UE 当前的限帧与垂直同步状态。帧时间图继续采集 MC 客户端数据。UE 窗口右上角默认显示半透明调试浮窗。可用 `-BridgeHideDebugPanel` 启动时隐藏，或在已释放控制时按 F8 切换。浮窗与 F3 同时显示时，F3 的右栏会下移。

## 地形材质、天气和光照

UE 以 4 × 4 区块列为一个 Region 组件，按区段更新各个槽位。以 32 视距的 65 × 65 列为例，满覆盖需要 289 个地形组件。UE 后台线程池解码区段并用面的几何和图集 UV 重建 TBN，渲染线程复用 Region 的顶点页与索引缓冲。顶点流共 32 字节，含本地位置、UNORM16 图集 UV、独立的方块光与天空光 UV、打包切线和顶点颜色。8192 像素图集的最大 UV 量化误差约为 1/16 像素。

地形六桶依次是 `Solid`、`Cutout`、`CutoutNoShadow`、`Foliage`、`Water` 和 `Translucent`。栅栏、门和梯子参与遮罩阴影投射，草丛、花、红石和铁轨关闭阴影。树叶使用双面次表面着色，水面用 Single Layer Water 单独绘制，普通半透明继续按 Region 和区段排序。硬件光追按 Region 提交 BLAS，同帧多个区段的变更合并后只重建一次。Region 保留至多 64 MiB 的全局区段对照数据，逐字节核对变动的顶点流。仅光色变化时上传发生变化的流，复用 BLAS 和有效阴影页。位置、拓扑和遮罩覆盖变化各走对应的更新路径。编辑器游戏模式可用 `-BridgeRegionTrace` 检查 `shadowNotify`、`blas` 和 CPU 提交耗时。[UE 水体着色模型](https://dev.epicgames.com/documentation/unreal-engine/single-layer-water-shading-model-in-unreal-engine)

DX12 SM6 默认启用 Lumen GI 与反射、VSM、MegaLights、TSR、自动曝光和 Bloom。支持硬件光追时，Lumen 在射线命中点计算 GI 和反射，适配流式 Region 网格。软件路径使用屏幕空间反射、直接阴影，并按 MC 的天空光和方块光补光，避免未遮挡的天空光铺满洞穴。可用 `-BridgeNoHardwareRayTracing` 启动软件路径作对照。流式 Region 尚无距离场和 Surface Cache 卡片，因此软件路径的屏幕外地形 GI 受限。

Minecraft 的日照角驱动太阳与月亮。主世界使用实时捕获的天空光、Sky Atmosphere、体积云和高度雾。降水改变云层、雾和阳光，潜入水或岩浆时切换介质雾。水面用切线法线驱动反射，Single Layer Water 按深度散射和吸收，太阳方向联动水底的动态焦散。普通透明表面使用受光材质和折射。

体积云使用 Epic 自带的 `m_SimpleVolumetricCloud_Inst` 预设。在同一次体积采样中，低、中、高三层分别采样这套预设的云图、垂直剖面和三维噪声。`mcue.Clouds.Coverage` 控制晴天低云覆盖率（0–1，默认 0.5）。`mcue.Clouds.MidLayer` 控制中层浓度（0–2，默认 1），在 Balanced 及以上启用。High 和 Ultra 还会绘制高层薄云，可用 `mcue.Clouds.Cirrus` 调整（0–4，默认 1）。

开发版可以输入 `mcue.Clouds.PreviewNative 1` 对照 Epic 的原始单层预设，输入 `mcue.Clouds.PreviewNative 0` 返回默认的多层预设。

在 Minecraft 的资源包界面导入并启用 LabPBR 包，三张图集会随资源重载一起刷新。UE 保留同一套 UV、mip 和 sprite padding。LabPBR `_n` 提供切线法线和 AO，`_s` 提供表面粗糙度、导体和每像素发光。缺失的材质图使用内置 [原版材质参数表](../fabric-bridge/src/main/resources/assets/minecraft_ue_bridge/terrain_materials.json)。标准导体用原 Albedo 表现反射色。 [LabPBR 规范](https://shaderlabs.org/wiki/LabPBR_Material_Standard)

首次加载一套资源时，Fabric 在后台准备完整的 Albedo、Normal 和 PBR 图集与各级 mip，并将结果写入**当前实例游戏目录**下的 `minecraftue-atlas-cache`。下次同一套资源命中已校验的缓存，可以跳过 LabPBR 解码并让 UE 直接读盘。同一 UE 进程切换维度时，还可复用已经上传的 GPU 纹理。修改资源包内容、图集布局或生成设置会产生新指纹。动画继续原位发送当前帧区域的 `AtlasPatch`，缓存只保存基础图集。若文件丢失、损坏、共享目录不可写或 UE 无法采用该缓存，桥接可走可靠连接的分批图集行传输。正常自动启动已指定实例缓存根，手工启动可传 `-BridgeAtlasCacheRoot="绝对路径"`。

缺少法线图时默认使用平坦法线。在「贴图凹凸」中开启「自动法线」后，图集准备阶段按颜色贴图的亮度梯度生成法线。提高强度会增强凹凸，提高弱色差阈值会滤掉更多细小色差，最大倾角限制最强的凹凸。强度为 0% 时停用自动生成。圆石、砖块和木板的凹凸更明显。自动生成会跳过动画、含透明像素的贴图、草顶和树叶。开发启动时也可用如下开关。

```powershell
.\Start-Bridge.ps1 -EngineRoot $ue -BridgeGradientNormals
```

萤石和海晶灯的整块材质都有底光。萤石的晶粒更亮，深褐色暗缝微亮。海晶灯的深青色外圈保持低亮度，内圈明暗纹路跟随 Minecraft 当前动画帧。岩浆块的冷岩壳、灯笼的金属提环和火把的木柄只接受场景照明。

钻石、绿宝石、青金石、铁、铜、金、煤和红石及各自的深板岩变种，只在矿物颗粒上叠加微弱的原色自发光。下界石英矿、下界金矿、镶金黑石与远古残骸也有各自的纹理选择。红石矿激活时恢复原有的强发光和环境照明。普通矿石的视觉微光保留 Minecraft 自身的亮度、刷怪条件和区段光源数。常规命名的模组矿石纹理也能从围岩推断颗粒，资源包可以通过 LabPBR 显式指定亮度。

Minecraft 26.2 的原版注册表中，109 种方块至少有一个发光状态，合计 1,626 个发光状态。其中 103 种使用烘焙模型。逐状态核对后，这 103 种方块的 1,568 个发光状态都至少有一个可见模型面能匹配到发光像素。岩浆流体使用实际流面和 UV。末影箱与潮涌核心在方块实体材质中按像素选择发光部位，这两者的直接光仍按方块近似。隐形光源保留其空间位置。末地传送门和末地折跃门的星点由视角投影产生。运行 `./fabric-bridge/gradlew.bat -p ./fabric-bridge verifySectionLighting` 会在 `fabric-bridge/build/tmp/verifySectionLighting/texture-emission/vanilla-texture-coverage.csv` 列出每种方块的渲染路径与匹配纹理。

模组和资源包可为普通模型提供 LabPBR `_s` 自发光通道，或给发光的模型元素标注 `light_emission`。解析模型时优先使用这些声明。只有方块报告发光且模型缺少具体部位时，才会从可见贴图颜色估算分布。纯颜色贴图无法可靠区分浅色金属和灯芯，作者遮罩可以指定每个像素的亮度。显式全黑遮罩会关闭相应的发光。

Fabric 按模型的真实 UV、线性颜色和覆盖面积生成光源。表面遮罩使用材质分辨率，直接光用每面最多四片、每方块最多 32 片近似。动画光片跟随 Minecraft 上传的颜色帧，UE 在局部灯光更新时对每片采样 8×8 个位置。边界和颜色由纹理决定，原版火把的直接光集中在火头。

UE 在相机 80 格内复用灯组件。MegaLights 基线预算为 1024，软件路径默认保留 48 盏，其中前 8 盏投射阴影。更远的表面照常按材质自发光。`-BridgeTrace` 的 `SceneLighting` 可查看区段光源数、活跃组件数与组件池大小。

### 染色玻璃的透光材质

染色玻璃逐灯透光目前作为**实验性选项默认关闭**，可在「VoxGlint 渲染设置 → 光照与阴影 → 染色玻璃透光（实验性）」中实时开启（对应配置键 `glassLighting=1` 与控制台变量 `mcue.Glass.Lighting 1`）。VoxGlint 内置原版 16 色染色玻璃和玻璃板的线性 RGB 透过率，每种颜色的两类方块共用 1 格参考厚度。玻璃板按方块状态的实际形状计算穿行长度，连接、拆放和区段变化会更新形状。已加载区段中的滤光体也会影响画面外经过它们的光线。

模组与客户端资源包可在 `assets/<namespace>/voxglint/light_filters.json` 声明或覆盖同命名空间的方块。对象键是方块 ID 的路径部分，例如下面的 `assets/minecraft/voxglint/light_filters.json`。

```json
{
  "green_stained_glass": {
    "transmittance": [0.055, 0.65, 0.11],
    "reference_thickness": 1.0
  },
  "green_stained_glass_pane": {
    "transmittance": [0.10, 0.80, 0.16],
    "reference_thickness": 0.125
  }
}
```

`transmittance` 是参考厚度下透过的线性光能比例，依次填 R、G、B 三个 `[0,1]` 数值。`reference_thickness` 为正数，单位是 Minecraft 方块。这份示例的玻璃板以 2/16 格为参考厚度。重新加载资源（F3+T）后应用配置。资源包按方块 ID 从低到高优先级逐项覆盖，值为 `null` 会撤销低优先级定义。第三方包可用 `assets/<modid>/...` 为已安装模组的玻璃提供参数。光学形状来自注册方块状态的 `VoxelShape`，可随邻接和方块状态变化。

点光、矩形面光和直射太阳按各自到受光点的射线计算。命中的每段按真实长度逐通道乘以 `Tref^(d/dref)`，再叠加各灯贡献。矩形面光取 2×2 个等面积子光源，因此小于采样间距的遮挡轮廓会有近似误差。穿过任意薄板或多种材质的路径均计算实际盒体交段。沿同一平面的整块玻璃可用二维格网精确加速。

默认按受滤局部光源需要的采样数量选择不透明阴影后端。有硬件光追时，最多 12 个颜色分量／面光采样使用光追阴影。超过预算时使用 UE 原生虚拟阴影贴图，采样数回落到 9 后再切回。太阳独立使用当前画质的方向阴影。此切换保留参与滤光的所有活跃灯和全部 RGB 通道。按需给 Minecraft 加 JVM 参数 `-Dminecraftue.glassRayShadowBudget=-1` 可对所有受滤局部灯保留硬件阴影，`0` 可强制它们使用虚拟阴影贴图，正整数指定预算。手动启动渲染器时使用 `-BridgeGlassLocalRayShadowBudget=N`。

## 安装并启动 Windows 版

准备 Minecraft Java 26.2、Fabric Loader 0.19.5、Java 25，以及能运行 DirectX 12 / Shader Model 6 的 Windows x64 环境。下载相同版本的 Fabric JAR 和 Windows 渲染器 ZIP。渲染器包自带 UE 运行时，直接安装到 Minecraft 实例即可。[v0.28.2 发布页](https://github.com/LIy-hub/VoxGlint/releases/tag/v0.28.2) · [Windows 运行时条款](windows-runtime-terms.md)

打开启动器设置的实例**游戏目录**。已经启动过的实例，可用 `options.txt` 所在目录定位。HMCL 等启动器启用版本隔离时，指对应版本自己的游戏目录。将 JAR 放到该目录的 `mods` 下，将 Windows ZIP 直接解压到游戏目录。ZIP 最外层已经有 `minecraftue-renderer`，解压后应与 `mods` 同级。开发者也可以将本地构建目录以同样的结构放入实例。

```text
游戏目录/
├── options.txt
├── mods/voxglint-0.28.2.jar
└── minecraftue-renderer/
    ├── WINDOWS-RUNTIME-TERMS.md
    ├── Engine/
    └── MinecraftUEViewer/
        ├── Binaries/Win64/MinecraftUEViewer-Win64-Shipping.exe
        └── Content/Paks/
```

准备好 Microsoft Visual C++ v14 x64 Redistributable。微软要求运行库架构与应用一致，版本至少与编译应用所用的 MSVC 工具链一样新。新系统可直接安装最新的 x64 版本。[官方下载](https://aka.ms/vc14/vc_redist.x64.exe) · [Microsoft 运行库说明](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-170#latest-supported-redistributable-version)

像平时一样从 Minecraft 启动器启动该实例。Mod 会拉起 Win64 Shipping 渲染器，等 UE 准备好并显示 Minecraft 界面后，自动隐藏原生窗口。新实例首次启动时，先在原生窗口完成 Minecraft 的辅助功能引导。启动前缺少渲染器，或进程在交接前启动失败时，可继续用原生窗口。交接后断线若超过 20 秒，MC 会保存并退出。退出 Minecraft 时，Mod 启动的渲染进程也会退出。启动和连接信息写入游戏目录的 `logs/latest.log`。

升级时先退出游戏，移出 `mods` 中的旧 JAR（文件名可能以 `minecraft-ue-bridge-` 或 `voxglint-` 开头），再把旧的 `minecraftue-renderer` 目录移出实例，按上面的目录结构安装新版本。若游戏启动后仍留在原生窗口，可从 `logs/latest.log` 查看 `Minecraft UE Bridge` 的查找路径与启动记录。

可用 JVM 参数 `-Dminecraftue.renderer.directory="渲染器目录的绝对路径"` 指定渲染器目录，或用 `-Dminecraftue.renderer.autostart=false` 停用自动启动。`--enable-native-access=ALL-UNNAMED` 可消除 Java 25 的本地访问警告。

## 构建与开发启动

Win64 渲染器需要 Unreal Engine 5.8、MSVC C++ 工具链和 Windows SDK，开发验证使用 UE 5.8.4。渲染器及编辑器目标会引用 UE 的 Renderer 和 Niagara 内部头文件，请使用含这些头文件的安装。Fabric 模组用 JDK 25，通过 Gradle Wrapper 获取 Gradle 和开发依赖。[UE 硬件与软件要求](https://dev.epicgames.com/documentation/unreal-engine/hardware-and-software-specifications-for-unreal-engine)

### 打包渲染器和模组

```powershell
$ue = Read-Host 'Unreal Engine 5.8 安装目录'
.\Package-Renderer.ps1 -EngineRoot $ue
.\fabric-bridge\gradlew.bat -p .\fabric-bridge build
```

渲染器脚本会从本机 UE 准备构建所需资源，再生成 Win64 Shipping 包。默认输出到 `dist/minecraftue-renderer`。若该目录已存在，传 `-OutputDirectory` 指定一个新目录。Mod JAR 输出到 `fabric-bridge/build/libs`。首次执行 Gradle Wrapper 需要联网，依赖齐全后可加 `--offline`。

默认生成带月相的简化月面。要用可选的 NASA CGI Moon Kit 月面纹理，在装有 NumPy 和 Pillow 的 Python 环境运行 `python ./tools/lunar_moon/prepare_lunar_data.py '<TIFF 文件目录>' '<生成结果目录>'`，再给 `Package-Renderer.ps1` 或 `Prepare-RendererAssets.ps1` 传入 `-LunarInputDirectory`，其值为脚本的输出目录。[月面数据来源与署名](../THIRD_PARTY_NOTICES.md#月面数据)

### 可选超分与帧生成插件

基础构建使用 Unreal Engine 的 TSR。要启用厂商超分和帧生成，从各厂商自行获取兼容 UE 5.8 Win64 的官方插件。将下面的插件目录放到 `ue-renderer/Plugins/Upscalers/` 后重新构建。每个插件以对应的 `.uplugin` 识别。

| 厂商包 | 插件目录 |
| --- | --- |
| [NVIDIA DLSS 4.5](https://developer.nvidia.com/rtx/dlss) | `DLSS`、`StreamlineNGXCommon`、`StreamlineCore`、`StreamlineDLSSG` 和 `StreamlineReflex` |
| [AMD FSR 4.1.2](https://gpuopen.com/learn/ue-fsr/) | `FSR-580` 内的 `FSR` |
| [Intel XeSS 3.2.1](https://github.com/GameTechDev/XeSSUnrealPlugin/releases/tag/v3.2.1) | `XeSS` |

超分插件在 Win64 DX12 SM6 下启用。DLSS 需要兼容的 NVIDIA RTX 显卡和驱动。AMD 插件会按设备选择 FSR 4 或 FSR 3，XeSS 会选择 Intel 或跨厂硬件路径。[第三方声明](../THIRD_PARTY_NOTICES.md)

### 编辑器联调

`Start-Bridge.ps1` 会准备本机渲染器资源，并由 Gradle 下载 Fabric 客户端开发所需的依赖。依赖和游戏资源已在本机缓存后可加 `-Offline`。需要单独生成云层、Niagara 和月面等资源时，运行 `./Prepare-RendererAssets.ps1 -EngineRoot $ue`。

```powershell
$ue = Read-Host 'Unreal Engine 5.8 安装目录'
.\Start-Bridge.ps1 -EngineRoot $ue -Build
```

脚本先启动 UE 编辑器的游戏模式，再启动 Fabric 开发实例，存档位于 `fabric-bridge/run/saves`。开发版可运行 `./ue-renderer/Scripts/Run-V27ShmFixture.ps1 -EngineRoot $ue -OutputDirectory <输出目录>` 验证单连接、共享块归还、近处区段优先以及跨世界清理。附加 `-Captures` 可回放 v26 捕获文件。回放脚本使用 Python，并启动独立的后台 UE 进程，在输出目录保存日志。

## 分别启动两端

可在 PowerShell 中单独启动 UE。首次启动或修改 C++ 后先编译和准备资源。

```powershell
$ue = Read-Host 'Unreal Engine 5.8 安装目录'
$project = (Resolve-Path -LiteralPath '.\ue-renderer\MinecraftUEViewer.uproject').Path

& "$ue\Engine\Build\BatchFiles\Build.bat" MinecraftUEViewerEditor Win64 Development "-Project=$project" -WaitMutex -NoHotReload -NoHotReloadFromIDE
if ($LASTEXITCODE -ne 0) { throw 'UE 编译失败' }
.\Prepare-RendererAssets.ps1 -EngineRoot $ue -SkipEditorBuild

& "$ue\Engine\Binaries\Win64\UnrealEditor.exe" $project -game -windowed -ResX=920 -ResY=650 -BridgeTrace -NoSplash
```

`-game` 直接打开游戏窗口。连接前窗口是黑色的。首次启动时 UE 可能需要编译着色器。

另开一个 PowerShell，在仓库根目录执行下面的命令。这种启动方式保留 Minecraft 窗口，便于和 UE 画面对照。改用 `build` 可以只生成正式实例需要的 JAR。

```powershell
.\fabric-bridge\gradlew.bat -p .\fabric-bridge -PexternalRenderer runClient
```

## 在 UE 里操作

UE 获得焦点后，可直接操作菜单。游戏中键入按键、点击画面或滚动会进入控制模式，首个事件也会转发给 Minecraft。光标在游戏中锁在 UE 窗口里，打开菜单时恢复可见。

| 输入 | 操作 |
| --- | --- |
| 字母、数字、小键盘、功能键、标点和修饰键 | 沿用 Minecraft 原版绑定，支持按住、松开和组合键 |
| `E`、`T`、`/`、`Esc` | 按默认绑定打开物品栏、聊天、命令栏和暂停菜单 |
| 鼠标相对移动 | 按 Minecraft 的灵敏度和反转设置转头 |
| 左键、右键、中键和两个侧键 | 按 Minecraft 的绑定处理按下、长按和松开，默认包括挖掘、使用和选取方块 |
| 滚轮 | 在游戏内切换快捷栏，在菜单中滚动。保留横向滚轮和高精度滚轮的实际增量 |
| 菜单中的鼠标 | 原版悬停、点击、双击和拖动，支持 Shift 等修饰键 |
| Windows 输入法 | 在聊天、搜索框等可编辑界面选词，确认的文字进入 Minecraft |
| 文件拖放 | 在资源包等界面拖入文件，由 Minecraft 执行原版确认流程 |
| 主触点 | 转为 Minecraft 的左键按下、移动和松开 |
| 切出 UE | 释放鼠标并清除按键状态 |

连接期间，处于游戏画面的 Minecraft 单人世界在失焦时继续运行。在 UE 中可以点击主菜单、选择存档、操作物品栏与暂停菜单，也可以用 `Tab`、方向键、`Enter`、`Esc` 操作支持键盘导航的界面，或在聊天和命令栏输字。关掉菜单后恢复游戏鼠标控制。

MC 的准星采用原版图案和显示条件，叠加时使用普通透明混合。单人场景的菜单保留原版半透明遮罩，UE 场景在菜单后方保持清晰。

桥接处理 Windows 交给 UE 窗口的键盘事件，包括 F8、F12 和组合键。`Alt+Tab` 和 `Alt+Space` 等系统快捷键由 Windows 处理。游戏中可按 `Esc` 打开菜单再拖动窗口边缘。需要原先的手动解锁组合键时，以 `-BridgeReleaseShortcut` 启动即可启用 `Ctrl+Shift+F12`。

在 UE 开发版窗口中按住 `Ctrl` 再按 `Esc` 下方的反引号 / `~` 键，可开关 UE 控制台。控制台打开时，Minecraft 暂停接收桥接输入。关闭后，重新用键盘或鼠标操作即可继续游戏。

游戏画面加载后，在 UE 窗口按 `Ctrl+Home` 可定格当前机位，用硬件路径追踪渲染一张无 HUD 的图片。截图保留当前构图，默认长边 2560 像素，每像素采样 256 次并启用降噪。同一帧的 sRGB PNG 和场景线性 EXR 使用同一个文件名，保存在 UE 的 `Saved/Screenshots/<平台>/PathTracing` 下（发行版为 `Windows`，开发版为 `WindowsEditor`）。渲染完成后即可恢复操作。

开发版也可以在 UE 控制台运行 `mcue.Photo.Capture`，执行同样的双格式路径追踪截图。UE 自带的 `Shot` 可以快速保存当前画面。

## UE 性能采集

```powershell
.\Start-Bridge.ps1 -EngineRoot $ue -Build -BridgeQuality Ultra -PerformanceCapture OnReady
```

`OnReady` 在地形、画质和分辨率稳定约 5 秒后自动采集 240 帧。用 `OnRequest` 可以先进入掉帧场景并固定镜头，然后在另一个 PowerShell 窗口运行启动时打印的 `New-Item` 命令。每次请求都会生成独立的 `capture-*` 目录，可以在同一场景调整一项设置后再采一轮。实例里已有的画质设置仍会生效。

- `frames.csv` 记录整帧、Game／Render／RHI 线程、GPU 总时间和 UE 的 GPU 通道计时。
- 用 Unreal Insights 打开 `timing.utrace`，在 Timing Insights 中查看 CPU 线程与 GPU 队列。搜索 `Minecraft` 可定位区块解码、提交、动态网格、点光更新和 HUD 等桥接事件。
- 对照 `settings.txt` 里的实际输出和内部尺寸、机位、硬件 GI／反射、MegaLights 和异步计算设置。`status.txt` 为 `complete` 时可汇总。采集中途请保持窗口尺寸和图形设置稳定。

```powershell
python .\ue-renderer\Scripts\summarize_bridge_performance.py '<capture-* 目录>' --engine-root $ue
```

带 `--engine-root` 时，汇总脚本还会用 Unreal Insights 导出 GPU 异步队列与桥接 CPU 事件，可查到 CSV 里缺少的异步通道。可依次传入多组采集目录，比较整帧耗时相对第一组的变化。GPU 通道可能嵌套，图形和异步计算队列也可能并行。判断某个开关能省多少帧时间时，应对比同机位、同分辨率的整帧数据。

## 验证

v0.28.2 的公开源码已通过 Fabric 的 `build` 及其七组现有合约检查，UE 5.8.4 的 Win64 编辑器目标也在无厂商插件的配置下通过编译。以下步骤用于验证本机联调。

1. **初始画面** · 确认启动加载只显示 Minecraft 窗口，主菜单出现后由同位置的 UE 窗口接手。确认任务栏和 Alt+Tab 中只剩 UE，在其中打开单人存档，检查原版热栏、准星及陆续补齐的地形。
2. **走动、跳跃** · 捕获控制后按 `W`、`A` 和空格。跨过区块边界后，UE 应补进新的一排区块并移除远端的一排。
3. **转头和比例** · 在 UE 中向左右转，再上下看。调高视距，让区段持续加载，同时转头、走动和挖放，观察镜头、准星及方块反馈。改变窗口比例，检查准星仍居于场景中心、热栏位于画面下缘。
4. **模型和资源包** · 在 UE 中检查半砖、楼梯、玻璃和一个走区段编译路径的模组方块。重载替换其纹理的资源包，检查模型保持原样并显示新纹理。
5. **跨界挖放** · 在 `X=15` 和 `X=16`（同一高度、同一 Z）验证楼梯的放置和挖掘。检查裂纹贴合模型、松手清除、挖穿后地形粒子出现并消失，再用 UE 区段上传日志确认更新范围。
6. **界面检查** · 按 `E` 打开物品栏，在 UE 内移动物品。按 `Esc` 关闭物品栏，再按 `Esc` 打开暂停菜单。点击「返回游戏」，检查光标恢复锁定。按 `T` 后用 UE 窗口输入文字，按 `F1` 检查热栏和准星一同隐藏或恢复。
7. **释放检查** · 在 UE 里按住 `W`，保持按键并用 `Alt+Tab` 切到另一个窗口。角色应在原版运动惯性结束后停下。切回游戏后，首次按下 `Esc` 应直接打开暂停菜单。
8. **日志** · 用 `./Start-Bridge.ps1 -EngineRoot $ue -BridgeTrace` 启用采样。现有 `Live` 行记录地形积压、UE 帧时间和 UI 上传状态。`CameraTiming` 行约每秒汇总一次，用 `scene` 对齐世界，按下列字段对比相同帧率和视距下的稳态转头与区段加载。

   - `frames` 和 `reused` 分别是有有效相机的 UE 帧数和复用上次姿态的帧数。
   - `lookFrames` 和 `lookReused` 对应最近 100 毫秒内收到有效相对鼠标输入的帧。`coalesced` 统计当帧采用前被更新姿态覆盖的相机包。
   - `poseP95Ms` 和 `poseMaxMs` 记录 UE 采用相邻新姿态的间隔。样本数为 `poseSamples`。
   - `lookP50Ms`、`lookP95Ms` 和 `lookMaxMs` 从 UE 收到一批鼠标位移的首个输入事件起算，到 UE 采用包含对应回执序号的 Minecraft 姿态为止。`lookSamples` 是匹配样本数，合并后的旧输入可能没有单独的回执样本。该计时截止于 UE 应用相机姿态，屏幕实际显示还要经过渲染和呈现。
9. **远景纹理** · 沿长石砖路缓慢横向转头，比较默认模式与各向异性模式下远处砖缝和条带的变化。靠近树叶、草丛和彩色交界处，检查镂空和相邻贴图边缘。`Minecraft terrain filtering` 日志记录请求模式、实际模式、实际各向异性级别和已加载的 mip 层数。

10. **天空、水面和光源** · 同一画面放置栅栏、矮草、树叶、水和彩色玻璃。执行 `/time set noon`、`/time set midnight` 和 `/weather rain` 比较日月、云与影子。潜入水中确认水下雾。放置和拆除火把或灯笼，观察直接照明和 `SceneLighting` 的活跃计数。
11. **材质重载** · 在资源包界面启用 LabPBR 包。确认 `VoxelAtlas`、`NormalAtlas`、`PbrAtlas` 在同一个 `scene` 到齐后开始加载区段。观察岩石凹凸、金属反射和像素发光。在 `fabric-bridge` 下运行 `./gradlew.bat --offline verifyTerrainAtlas` 可复查打包与通道字节。
12. **动态对象** · 空手与剑之间切换，挥动并举起副手盾牌。用 F1 和 F5 检查第一人称网格清除与恢复。观察箱子、告示牌、两组同类掉落物和生物，拾取掉落物或移除实体后检查残留。日志的 `DynamicMeshes` 行给出世界和手部的批次数、实例数、网格定义数与纹理数。

13. **1440p 离屏对照** · 编译编辑器目标后运行 `./ue-renderer/Scripts/Run-V23RendererFixture.ps1 -EngineRoot $ue -OutputDirectory ./ue-renderer/Saved/V23QualityBench`。固定合成场景检查实际主分辨率、四档热切、区段投影失效通知、点灯复用及刚体变换。`-Mode Baseline`、`-Mode History100`、`-Mode Fixed70History100` 分别重跑相同场景下的 TSR 对照。`-BridgeQualityTrace` 在场景稳定后输出 240 帧的帧时间和 CPU/GPU 平均耗时。

### 生成 Prism Foundry 展示存档

要在同一场景对照水面、材质和光照，可用项目的地图生成脚本。准备安装了 Pillow 的 Python、一个新建的 Minecraft 26.2 原版存档和本机客户端 JAR。`--output` 指向一个尚不存在的目录。

```powershell
python .\tools\prism_foundry\generate_world.py --template-save '<原版存档目录>' --minecraft-jar '<26.2 客户端 JAR 路径>' --output '<新存档目录>'
```

脚本会在新目录生成展示世界。将此目录放入要测试的实例的 `saves` 下即可打开。[地图生成脚本](../tools/prism_foundry/)

VoxGlint 自有代码采用 Apache-2.0。[Apache-2.0 许可证](../LICENSE) · [版权声明](../NOTICE) · [第三方声明](../THIRD_PARTY_NOTICES.md)
