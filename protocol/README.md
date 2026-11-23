# VoxGlint 桥接协议 v28

一个 Fabric 会话与 UE 建立一条双向 TCP 连接。开发模式默认监听 `127.0.0.1:39182`，自动启动由 UE 监听系统分配的回环端口并向 Fabric 原子写回 `pid version port` 启动记录。TCP 负责握手、输入、窗口、HUD 引用、图集、动态对象、环境和有序世界操作。网格与区段光源正文，以及 MC 权威相机，位于 Fabric 创建的 Windows 会话共享内存。

TCP 消息由大端 `u32` 长度、`u8` 类型及消息体组成。长度包含类型字节，上限为 1 MiB。除特别注明的共享内存布局，消息整数和 IEEE 754 浮点数使用大端。

## Fabric → UE

| 类型 | 消息体 |
| --- | --- |
| 1 · Reset | `u32 0x4D435545`、`u32 28`、`u64 sessionNonce`。Fabric 随后发送 ControlShm。UE 映射两块共享内存并校验会话后，回送相同的 Reset。版本不匹配时 UE 回报自己的版本并关闭连接。 |
| SectionMesh 正文 | `i32 sectionX, sectionY, sectionZ`、六个 `u32 vertexCount`，后接按桶顺序排列的四边形。每面由 `u8 face` 和四个顶点组成，共 101 字节。每个顶点为区段本地 `f32 x, y, z`、图集 `f32 u, v`、`u8 r, g, b, a`、`u8 packedLight`。几何之后追加水面及岸线记录。整条消息原子替换该区段，六桶全空并且两种记录计数为零时移除网格。 |
| 3 · DropChunk | 一个或多个 `i32 chunkX, chunkZ` 坐标对。移除对应整列区段的网格、光源和滤光体。 |
| Camera 快照 | 权威相机槽中的前 89 字节。`u32 worldSerial`、`f64 x, y, z`、`f32 forwardX, forwardY, forwardZ`、`f32 upX, upY, upZ`、`f32 verticalFovDegrees, aspectRatio`、`u32 appliedLookSerial`、`u8 playerView`、`f64 playerX, playerY, playerZ`，共 89 字节。随后为 `u32 appliedLookCapture`、`u8 playerMovementFlags`（偏移 93）及 2 字节填零。相机确认 MC 已消费的最近一次相对鼠标移动序号及所属捕获，新世界清为 0。`playerView` 为 0 时隐藏本地玩家与手部，脚下坐标为零。为 1 时显示第一人称手部并让世界模型投影。为 2 时直接绘制世界模型。脚下坐标与相机取自同一帧。`playerMovementFlags` 位 0 表示移动状态已知，位 1 表示 Minecraft 报告该玩家双脚接地，且当前没有飞行、滑翔、旁观、乘骑或入水。旧帧将该字节写作 0，即移动状态未知。 |
| 6 · AtlasRows | `u16 mipIndex, mipCount, width, height, firstRow, rowCount`、连续的 BGRA8 Albedo 行。RGB 使用 sRGB，A 为覆盖率。最长边 8192 像素，每条消息的像素数据最多 256 KiB。宽高指当前 mip 层。 |
| 9 · InputAvailability | `u8 mode`。0 表示输入不可用，1 表示游戏内键盘和相对鼠标，2 表示界面键盘和绝对鼠标，3 表示界面内启用系统输入法，4 表示仅键盘，由 Minecraft 按当前遮罩或界面决定哪些键生效。 |
| 10 · UI | `u128 mappingGuid`、`u16 width, height`、`u8 slot`、`u8 opaque`、`u32 readbackMicros`。值 `opaque=1` 表示画面覆盖视口，`0` 表示透明界面层。像素从 Windows 共享内存读取。 |
| 11 · World | `u32 worldSerial`、`i32 minY, height`。进入或重载世界时更换地形、图集、动态网格、光源、环境与相机的代次。`height=0` 表示退出桥接世界。 |
| 12 · Window | `i32 clientX, clientY`、`u16 width, height`、`u8 singleWindow`。报告 MC 的初始客户区几何和是否进入单窗口模式。 |
| 13 · Close | 无消息体。单窗口正常退出时通知另一端。 |
| 14 · WindowMode | `u8 fullscreen`。单窗口下在 Window 之后同步初始模式及后续变更。0 表示窗口模式，1 表示 UE 无边框全屏。 |
| 15 · SceneReady | `u32 worldSerial`。Fabric 已发送玩家所在区块的初始区段。UE 完成待处理区段上传并收到相机后回报就绪。 |
| 16 · VideoSettings | `u8 viewDistance`、`u16 fpsLimit`、`u8 flags`、`u8 terrainFiltering`、`u8 maxAnisotropy`。有效视距为 2–32 区块。限帧为 10–250，0 表示无限制。标志的位 0 为垂直同步，位 1 为离开时降帧。地形过滤 0、1、2 分别对应 Minecraft 的 NONE、RGSS 和 ANISOTROPIC。各向异性级别为 1–8，前两种模式固定为 1。初次连接及设置变化时发送。 |
| 17 · CrackAtlas | `u32 worldSerial` 后接 10 张 16 × 16 的 BGRA8 裂纹纹理。按阶段 0–9 排列，每张图从顶行开始。每个世界或资源代次传一次，先于 CrackUpdate。 |
| 18 · CrackUpdate | `u32 worldSerial`、`i32 sourceId, blockX, blockY, blockZ`、`u8 stage`。阶段 255 清除同一来源在该坐标的裂纹，此时消息结束。阶段 0–9 还包含 `u32 vertexCount` 和 `vertexCount` 个 `f32 localX, localY, localZ, u, v`。每四个顶点一面，最多 8192 个顶点。位置相对方块，纹理坐标允许重复。更新替换该坐标的裂纹。 |
| 21 · ControlShm | `u128 controlGuid, terrainGuid`。命名格式为大写 `{GUID}`。两块映射与本次 TCP 会话绑定。 |
| 23 · GpuUi | `u128 mappingGuid`、`u16 width, height`、`u32 adapterLuidLow`、`i32 adapterLuidHigh`、`u32 adapterNodeMask`、`u8 backend`。单窗口下请求同一 GPU 上的 HUD 纹理。0 表示 OpenGL，1 表示 Vulkan。每次尺寸变化使用新的映射。 |
| 24 · EnvironmentState | 52 字节。`u32 worldSerial`、`u8 dimension, medium, moonPhase, flags`、`f32 sunAngle, globalRain, thunder, localRain, localSnow, skyFlash`、`u32 skyRgb`（`0x00RRGGBB`）、`f32 fogR, fogG, fogB`、`f32 gameTimeSec`。`dimension` 的 0–3 分别为主世界型、下界、末地型、其他；`medium` 的 0–2 分别为空气或其他介质、水下、岩浆内；`moonPhase` 为 0–7。`sunAngle` 为弧度 `[0, 2π)`，0 为正午天顶。`globalRain` 与 `thunder` 为世界级降水与雷暴强度（进入沙漠不归零）；`localRain` 与 `localSnow` 按相机群系降水类型给出；`skyFlash` 为雷暴天空闪光强度；`gameTimeSec` 为连续游戏时间秒数（暂停时冻结）。 |
| SectionLights 正文 | `u32 worldSerial`、`i32 sectionX, sectionY, sectionZ`、`u16 count`，后接 `count` 条 `u16 localPos`、`u8 r, g, b, level, flags`。`localPos = x \| (y << 4) \| (z << 8)`，各轴取区段内的 0–15。每区段最多 4096 条，每位置一条。flags 的第 0–5 位依次表示下、上、北、南、西、东六个暴露面，沿暴露面外移时须设置第 6 位，低六位至少设置一位。支持遮盖光源的 UE 允许在仅一个暴露面时设置第 7 位，表示该面外侧 0.20 格内有非透明方块的碰撞形状，UE 改用贴面的矩形光源。flags 为 0 的光源沿用格内位置。RGB 为 sRGB，优先汇总方块模型面的贴图自发光，缺少有效颜色时使用预设。`level` 为发光方块状态的 1–15 级光照，也是按每格衰减一级估算的名义半径。`count=0` 清空该区段的光源。 |
| 27 · NormalAtlasRows | 头部及 BGRA8 行布局同 `AtlasRows`。RGB 为线性 UNORM 切线空间 XYZ，按 `[-1,1]` 编码到 `[0,255]`，Y 向下；A 通道编码线性表面孔隙度 Porosity（`0..255`，来自 LabPBR `_s` 蓝通道 `0..64` 或原版材质表默认值），用于雨天吸水变暗与湿润响应。 |
| 28 · PbrAtlasRows | 头部及 BGRA8 行布局同 `AtlasRows`。解码后的线性 RGBA 分别存线性粗糙度、金属度、发光、材质 AO。 |
| 29 · DynamicTextureRows | `u32 worldSerial, textureId`、`u16 width, height, firstRow, rowCount`、BGRA8 行（RGB 为 sRGB）。从第 0 行连续上传，末行到达后替换该 ID 的纹理。纹理 ID 2–513 按场景分配，0 为白色，1 为已传输的方块 Albedo 图集。 |
| 30 · DynamicMesh | `u32 worldSerial`、`u8 space`、`u32 meshId, textureId`、`u8 blend, topology`、`u32 vertexCount`，每顶点 28 字节。网格由一个材质的一个图元流组成。网格 ID 非零，在场景与空间内唯一。详见下文。 |
| 31 · InstanceBatch | `u32 worldSerial`、`u8 space`、`f32 handVerticalFovDegrees`、`u16 count`。每实例为 `u32 meshId, rigidPart`、`f64 x, y, z`、`f32 qx, qy, qz, qw, scale`，共 52 字节。`rigidPart=0` 表示普通快照，低 31 位保存当前场景内的稳定对象和部件标识。稳定部件在快照间插值根位置和旋转，第 31 位为 1 的部件同时提交最新烘焙形变。只有 `space=0` 支持非零部件 ID。位置单位为格，旋转为单位四元数，缩放为正的统一倍率。批次替换所选空间的完整快照，数量为 0 时清除。 |
| 32 · AtlasPatch | `u32 worldSerial, textureId`、`u16 regionCount`。每个区域为 `u16 mipIndex, x, y, width, height` 后接紧密排列的 BGRA8 像素。宽高与坐标按当前 mip 的像素计，原点在左上。`textureId=1` 更新方块 Albedo 图集，2–513 可更新已上传的动态图集第 0 层。每条消息含 1–256 个区域，消息体最多 256 KiB。 |
| 33 · AtlasReference | `u32 worldSerial`、32 字节资源指纹、20 字节像素校验和。申请读取实例缓存或复用 UE 中已上传的同一套图集。 |
| 34 · AtlasUse | `u32 worldSerial`。收到匹配场景的就绪回执后，提交此次申请，随后的区段使用该图集。 |
| 35 · ChunkPrecipitation | 524 字节。`u32 worldSerial`、`i32 chunkX, chunkZ` 后接 `256` 个 `u16`（按 `lz * 16 + lx` 排列）。每格低 11 位编码 `MOTION_BLOCKING` 顶面相对 `minY` 高度（`0..2047`），位 12–13 编码该列在顶面位置的 Minecraft 降水类型（`0=NONE, 1=RAIN, 2=SNOW`），位 14 为有效位。位 11 和 15 分别为可选群系氛围档的低位和高位（`0=普通, 1=干燥, 2=寒冷, 3=潮湿`）。区块加载、方块变更、群系更新时增量同步，随 `DropChunk` 清理。 |
| 36 · LightningStrike | 40 字节。`u32 worldSerial, entityId`、`f64 x, y, z`、`u32 seed`、`f32 remainingSeconds`。同步 Minecraft 客户端收到的 `LightningBolt` 实体，坐标为原版世界坐标。`seed` 由客户端闪电种子折叠为 32 位，`remainingSeconds` 根据实体剩余生命和闪光次数估算。实体种子变化时重发，走高优先级控制流。 |
| 37 · ChunkWaterGrid | 每列代表性最高水面及其深度、暴露和障碍标志。见下文。 |
| 38 · TerrainShm | `u32 worldSerial`、`u64 token`、`u32 offset, meshBytes, lightBytes`、`u8 priority`，共 25 字节。偏移相对地形映射首字节。 `priority=1` 表示玩家附近的编辑或光照变更。共享内存在该偏移依次存放 SectionMesh 和 SectionLights 正文。 |
| 39 · ChunkWaterBubbles | P2 可选。更新指定区块的气泡柱，计数为 0 时清除。格式及握手见下文。 |
| 40 · SectionGlass | 可选。原子替换指定区段的滤光材质和形状盒。分片格式及握手见下文。 |

### 天气数据

- `EnvironmentState` 的降水、雷暴强度和天空闪光取自 Minecraft 客户端世界状态。`flags` 位 0 为天气判定已知，置位时位 1–3 分别给出原版 `Level.canHaveWeather()`、`Level.isRaining()` 和 `Level.isThundering()` 的结果。旧包这些位为零，表示判定未知。原版天气能力取决于天空光、维度顶盖和末地维度标识。保留 `globalRain` 和 `thunder` 的视觉过渡强度，原版玩法判定使用整刻状态，其中 `isRaining()` 阈值为 `>0.2`，`isThundering()` 为 `>0.9`。
- 逐列降水类型表示该位置允许的降水，由 `Biome.getPrecipitationAt` 按群系、顶面高度和海平面决定。该列的即时降水同时取决于维度天气能力与 `globalRain`。相机附近的 `localRain` 和 `localSnow` 采样相机方块位置，在禁用天气的维度为零。
- 新写入端把无降水列标为干燥，把降雪列标为寒冷，把降雨的沼泽、红树林、丛林系列及匹配 `minecraft:is_jungle` 或 `minecraft:has_structure/swamp_hut` 标签的群系标为潮湿，其余列为普通。氛围档仅供表现层选用。旧包的相关位为零，按普通读取，既有的高度、降水类型和有效位兼容。
- 区块边界用原版 `BiomeManager` 选择群系。若选中的源区块尚未加载，该格有效位为零。源区块载入、更新群系或卸载时，已发送的相邻列会重新采样。顶面高度提供逐列降水遮挡。
- 在 `fabric-bridge` 中运行 `gradlew.bat verifyWeatherTransport verifyNativeWeatherWorld` 可验证原版群系和真实落雷，并在相应任务的 `build/tmp` 目录中生成 35、36 消息样本。

### 动态模型与实例

- `space=0` 使用 Minecraft 世界坐标。`space=1` 使用第一人称相机坐标，X 向右、Y 向上、负 Z 向前，FOV 为原版手部垂直视角。`space=2` 用于本地玩家世界模型，轴向与 Minecraft 世界一致，原点由 `Camera` 中的脚下坐标指定。空间 0 和 2 的手部 FOV 字段为 0。
- `blend=0` 使用透明度遮罩，`blend=1` 使用透明混合。图元类型 3 为三角形，4 为四边形，5 为三角带，6 为三角扇。每顶点为本地 `f32 x, y, z, u, v`、`u8 r, g, b, a`、`i8 normalX, normalY, normalZ`（单位法线乘 127，取整）和 `u8 packedLight`（高四位天空光，低四位方块光）。
- 单个网格最多 8192 个顶点。每批最多 512 个实例，按所有实例累计，世界空间最多 32768 个顶点，第一人称手部和本地玩家各最多 8192 个顶点。单张动态纹理最多 4,194,304 像素、最长边 8192，场景总计 16,777,216 像素。每条纹理消息的像素数据最多 256 KiB。
- 同个场景内，发送端先发完纹理行才引用纹理。同一空间先发新增网格，再发引用它们的 `InstanceBatch`。快照可以引用上一快照保留的网格，同一个 ID 可以放置多次。UE 收齐批次后释放该空间未被引用的网格。各动态空间及纹理各自只暂存一个发送批次，与界面和地形按消息轮询。收到新的 `World` 或断线后清空纹理、网格和实例，旧场景的动态消息被丢弃。

### 粒子

v28 移除消息 19。`DynamicMesh` 和 `InstanceBatch` 的 `space=3` 携带 Minecraft `ParticleEngine` 的实际渲染提交。顶点相对采样时的相机位置，轴向与 Minecraft 世界一致，每轴范围为 ±4096 格。实例坐标是采样时的相机世界位置，旋转为单位旋转，`scale=1`、`rigidPart=0`、`handVerticalFovDegrees=0`。每条网格仍最多 8192 顶点，整个快照最多 65536 顶点。新的完整批次替换上一帧，空批次清除粒子。

粒子空间的 `blend` 低三位支持 0 遮罩、1 普通 Alpha、2 源 Alpha 加法、3 预乘 Alpha、4 加法。第 3 位为 1 时关闭深度检测，其余位为 0。顶点布局和纹理命名空间与其他动态空间共用。粒子贴图通过 `DynamicTextureRows` 发送，粒子使用的方块图集由纹理 ID 1 引用。

### 图集动画

Fabric 读回 Minecraft 已绘制的动画 sprite 区域，包含对应 mip 和边缘填充。方块图集在 UE 确认场景就绪后发送 `AtlasPatch`。动态图集的补丁与纹理行共用同一队列，基础纹理发完后才发送补丁。每个区域携带完整像素，等待 GPU 或发送队列期间，同一区域发生的多次更新会合并。大区域按行分批。UE 丢弃旧场景的补丁，在当前纹理上原位更新各矩形。

### 地形语义桶

| 桶 | 用途 |
| --- | --- |
| 0 · Solid | 普通不透明面 |
| 1 · Cutout | 带结构的遮罩模型，例如栅栏、门、活板门和梯子。参与阴影投射 |
| 2 · CutoutNoShadow | 红石线、轨道和小植被。关闭阴影投射 |
| 3 · Foliage | 树叶。使用双面植被次表面着色，参与阴影投射 |
| 4 · Water | 水体和含水方块的流体面。使用 UE 的 Single Layer Water 材质 |
| 5 · Translucent | 彩色玻璃、冰和史莱姆块等普通半透明面。保留 MC 层内的排序 |

`face` 的低三位（`0x07`）中 0–5 依次为下、上、北、南、西、东，6 表示用四边形的两组三角形重建法线；当 UE 公布能力位 3 时，`CutoutNoShadow` 桶中的草、花、甘蔗、作物、藤蔓等草本植物面可设置最高位 `0x80`，由 UE 路由到独立植被次表面着色层并做朝上法线混合（不投射 VSM 阴影），其余位保留为 0。UE 用位置与图集 UV 重建正交切线及副切线方向，处理旋转和镜像 UV。`packedLight` 高四位为天空光，低四位为方块光。UE 顶点工厂将它们暴露到第二套纹理坐标，顺序为 `(blockLight, skyLight)`，各自除以 15。UE 解码时将与实心面完全重叠的遮罩覆盖层移入无阴影桶，并沿法线偏移 0.1 厘米。

### 场景同步

`World`、图集控制与行传输、区段描述符、`SectionGlass`、`DropChunk` 与 `SceneReady` 在同一可靠队列中保持顺序。资源重载期间，Fabric 在方块图集装箱及 mip 准备结束后，于专用线程生成或校验完整的 Albedo、Normal、PBR。三张图集及各 mip 存于该实例游戏目录下的 `minecraftue-atlas-cache`。资源指纹覆盖装箱位置、边缘填充、动画首帧、Albedo mip 像素、用于生成静态发光遮罩的动画帧、LabPBR 有效资源字节、默认材质、生成参数与版本。有效缓存允许跳过 LabPBR 解码。

### v28 染色玻璃扩展

控制映射的能力位 6（`1u << 6`）启用 `SectionGlass`。Fabric 仅在完成握手并读到该位时发送扩展消息。已有 v28 的握手、地形映射和区段光源布局沿用原格式。

每个分片的 24 字节头依次为 `u32 worldSerial`、`i32 sectionX, sectionY, sectionZ`、`u16 partIndex, partCount, materialCount, boxCount`。分片以 0 为起点，按序发送，整组 1–256 片。每片材质和形状盒各至多 4096 条。头之后先写材质表，再写形状盒。材质记录为 `f32 Tr, Tg, Tb, dref`，前三个通道为有限的线性 RGB 透过率，取值 `[0,1]`。参考厚度 `dref` 为有限正数，单位为格。

每个形状盒为 `u16 localPos, materialIndex` 和 `f32 minX, minY, minZ, maxX, maxY, maxZ`。`localPos = x | (y << 4) | (z << 8)`，每轴取区段内的 0–15。`materialIndex` 从零开始索引本分片的材质表。形状盒坐标相对于其所属的 Minecraft 方块。每轴端点须有限且 `min < max`，可延伸到所属方块边界之外。同一方块的形状用内部互不相交的盒组成。

Fabric 将一个区段更新的所有分片排在同一地形队列项内，后接该区段的网格与光源描述符。不同地形更新的分片组按顺序通过队列。UE 收齐当前场景的一组分片后替换整个区段。单片零记录（`partIndex=0, partCount=1, materialCount=boxCount=0`）清空区段。普通空区段无须发送滤光消息，形状或材质未变化时复用上一组。拆放及邻接形状更新后重新捕获。整列丢弃、场景切换、资源重载及断线清理相关区段与未完成的分片。

直接照明对每条光源到受光点的路径分别计算。光线在一段材料中穿过长度 `d` 时，线性光照逐通道乘以 `Tref ^ (d / dref)`，叠层相乘，各灯过滤后的贡献相加。

Fabric 在 `World` 后发送 `AtlasReference`。UE 以实例目录中的 `minecraftue-atlas-cache` 为唯一路径根，仅打开由指纹生成的文件名。Fabric 自动启动时由 `MCUE_ATLAS_CACHE_ROOT` 指定，手工启动可用 `-BridgeAtlasCacheRoot`。匹配上一个图集的指纹和校验和时，UE 复用内存中的纹理。UE 读盘校验并暂存图集，回报就绪后 Fabric 发送 `AtlasUse`，随后的区段才开始发送。读取拒绝、8 秒无答复或已接受但 60 秒仍未准备好时，Fabric 按 Albedo、Normal、PBR 的顺序发送图集行。迟到的缓存读取会被图集行替代。共享目录不可写时，Fabric 使用私有临时文件准备图集行。每个场景的动态纹理 ID 和补丁仍随场景重置，首次动画会重新发送当前帧区域。

缓存文件名为 `<小写 SHA-256 指纹>.mcat`。68 字节大端文件头包含 `u32 0x4D434154`、`u32 1`、`u16 width, height, mipCount, reserved`（保留值须为 0）、32 字节 SHA-256 资源指纹及 20 字节 SHA-1 像素校验和。其后为三张无间隔的 BGRA8 图集，按 Albedo、Normal、PBR 及 mip 从高分辨率到低分辨率顺序存放。Fabric 在同目录完成写入和校验后原子替换目标文件。

区段每次更新通过同一描述符引用网格和完整光源快照。挖放、光照更新、区段重载和视距变化沿用区段更新流程。隐形的 `minecraft:light` 也有光源记录。区块列卸载或切换世界时清空相应记录。

控制映射能力位 4 启用材质光源。仅为支持该位的 UE，Fabric 可将单条 SectionLights 记录的 `level` 第 7 位置一，低四位仍为 1–15。紧随该记录的 `flags` 写入 `u8 patchCount`（0–32），然后写入对应数量的 23 字节记录。零片表示材质明确关闭直接光。未置位的记录仍为七字节。

每片依次为相对方块原点的 `i16 x,y,z`（单位 1/1024 格）、`i8 normal[3]`、`i8 widthAxis[3]`（各轴除以 127）、`u16 width,height`（单位 1/1024 格）、`u8 r,g,b`（归一化色相的 sRGB 编码）、`f32 power`（面积加权的线性 RGB 最大分量，含紧凑灯具的强度标定）。尺寸均为正时为单面矩形光。宽为零时为火头点光，高字段保存光源半径。仅材质发光的方块使用名义等级 15。网格和扩展光源正文合计仍受原地形映射上限约束，扩展记录无法放下时写入普通光源记录。

控制映射能力位 5 启用按纹理和状态发光。地形 `face` 的位 3–5 表示遮罩模式（0 为 PBR.B、1 为关闭、2 为当前颜色乘静态遮罩、3 为当前颜色自动估计、4 为海晶灯内圈）。位 6 表示顶点光照低四位携带模型自身的发光等级。动态世界模型的 blend 低位保留原透明标志，高两位 1、2、3 分别选择末影箱、已激活和未激活的潮涌核心材质。

能力位 7 启用额外地形模式 5，表示围岩中的矿物微光。Fabric 为零级矿石提供颗粒遮罩和该材质模式，`SectionLights` 保持空。在模式 4 下，海晶灯全表面带低强度底光，PBR.B 编码活动纹路的参考白，采样与暗外圈过渡时保持连续。

材质光源记录的 `level` 第 6 位表示该记录含动画采样数据。此时 `patchCount` 后增加 `u32` 位图，其第 i 位为一时，在第 i 片的 23 字节后继续读 24 字节：四个 `u32` 图集 UV（高 16 位 U，低 16 位 V，均为 UNorm16）、`u8 mode`、三个 sRGB 染色字节以及 `f32` 有效面积。UE 使用与 `AtlasPatch` 同步的颜色数据计算当前线性光能。空间不足时，先保留原始光片，再按原协议的区段容量退回普通记录。

Fabric 每 tick 至多采样一次 `EnvironmentState`，有变化时发布。主连接优先传送最新环境，UE 暂存先于同代次 `World` 到达的环境。主世界日月方向、天空、大气、云与雾以这些数据为输入，水下和岩浆内还切换雾与相机色调。

### 会话共享内存

控制映射固定前缀为 640 B。头部 `u32 magic, version` 和 `u64 sessionNonce` 使用小端。偏移 16 为 UE 发布的 `u64 capabilities`，偏移 24 的 `u64 mappingBytes` 为 0 时沿用 640 B 布局，为 6912 时包含选框槽。其余头部字节保留为零。序号和所有原子字槽均按 8 B 对齐。能力位 1 支持 SectionLights 的遮盖标志。能力位 2 表示 UE 已映射选框槽。能力位 3 支持 `CutoutNoShadow` 中 `face` 最高位 `0x80` 植被标记。

| 数据 | 序号偏移 | 正文偏移与大小 |
| --- | ---: | --- |
| MC 状态 | 64 | 128，192 B 小端。包含世界序号、姿态标志、位置与眼高、鼠标比例、强制视角与传送代次、选框及手持物 |
| UE 本地转头 | 320 | 384，40 B 小端。含确认代次、时间戳、世界与捕获 ID、转头序号、yaw、pitch 和活动标志 |
| MC 权威相机 | 448 | 512，96 B。相机字段布局见上表 |
| MC 方块选框（可选） | 640 | 704，`u16 revision, edgeCount`、`u32 reserved=0`，后跟最多 256 条相对命中方块原点的 `f32 ax, ay, az, bx, by, bz`。字段为小端 |

Fabric 先发布轮廓，再更新 MC 状态正文偏移 150 的 `u16 selectionRevision`。UE 只绘制修订号对应的轮廓。零计数表示空轮廓，超过边数上限时使用该形状的包围盒。旧端或选框槽未映射时修订号为零，UE 使用整格选框。

每槽由单端写入。写方把序号置奇，更新按 8 B 原子访问的正文，再以 release 发布偶数序号。读方核对读取前后的偶数序号，最多尝试四次。在没有新有效样本时沿用上次快照。权威相机按 `worldSerial` 匹配可靠 World，转头确认同时匹配捕获 ID。菜单、平滑相机、骑乘或外部视角使用 MC 的位置、转向和投影。第一人称本地转头保留原有的视角仲裁。

地形映射共 56,393,792 B。头部 64 B，其中偏移 0、4、8、12 为小端 `u32 magic, version, capacity, headerBytes`，偏移 16 为同会话的小端 `u64 sessionNonce`。正文从偏移 64 开始按 16 KiB 页分配连续区间。普通区 16 MiB，最多 96 份。近处预留区 4 MiB，最多 32 份。大块区一次容纳一份合计最多 35,418,174 B 的更新，覆盖合法的最大网格和光源。普通区满时等待归还，近处小更新仍可使用预留区。超过普通区容量的更新使用大块区。

Fabric 完成写入后才将描述符放入可靠队列。UE 在实际派工时读取光源并将网格复制到本地解码任务，然后在 Input 中归还 token。相同坐标的旧描述符被替换、被 DropChunk 或 World 取消时，一旦确定无后续读取也会归还。照片冻结时尚未读取的描述符按原顺序保留，复播后继续消费。放弃整个连接时，两端关闭各自映射并清理在途状态，后续会话使用新映射。

### HUD 共享纹理

Fabric 创建 208 字节的小端共享内存 `{大写 GUID}`。UE 使用 `MCUE-HUD-<大写 GUID>` 前缀为三个槽创建 `-T0` 至 `-T2` D3D12 RGBA8 纹理及 `-F0` 至 `-F2` fence。头部偏移 0、4、8 分别是 `i32 status`（0 等待、1 可导入、−1 回退）、`u32 abandoned` 和 `u32 targetFps`。偏移 16 是 `u32 drawn`，24 和 32 分别为 `u64 published, noSlot`。Vulkan 后端中，偏移 40 存 UE 的 `u32 processId`，偏移 160–183 存三个 `u64 textureHandle`，偏移 184–207 存三个 `u64 fenceHandle`。Fabric 复制句柄后查询外部内存类型，专用导入同显卡的 D3D12 图像，并把 D3D12 围栏导入为时间线信号量。

三个槽从偏移 64 开始，各占 32 字节，槽内偏移 0、8、16 分别是 `u64 sequence`、`u64 releasedSequence` 和 `u32 opaque`。Fabric 完成复制后在该槽的 fence 上发出 `2 * sequence - 1`，然后发布槽序号。UE 取最新且 fence 已完成的帧，退还槽时在图形队列上发出 `2 * sequence`，确认完成后写入 `releasedSequence`。元数据通过 acquire/release 发布。Vulkan 复制使用 Minecraft 帧的图形命令编码器，源图为 GENERAL，目标图在 Vulkan 传输与 D3D12 共享之间切换布局和外部队列所有权。Fabric 在 Minecraft 提交当前帧后才发布槽序号。UE 下发的 `targetFps` 取当前有效限帧与 HUD 档位上限的较小值。Performance、Balanced 默认 120 FPS，Baseline、High、Ultra 默认 165 FPS。可用 `-BridgeHudMaxFps` 改写共享路径的档位上限。Fabric 还受 MC 设置中的限帧约束。导入失败、连续活跃采集时同步停滞或改尺寸时，Fabric 放弃映射并通过类型 10 发送界面。系统内存路径以 120 FPS 为上限，仍遵守两端当前限帧。

TCP 连接以 Reset 和两块映射的握手开始。收到 Window 后，UE 回报当前视口尺寸，Fabric 随后开始发送界面。每张图集按 mip 索引从 0 递增，每层从第 0 行连续传到最后一行。每个维度等于基础层的对应维度右移 mip 索引，且必须大于 0。层数上限为 14，由 Minecraft 当前图集决定。三张图集具有相同的各层尺寸、sprite UV 和边缘填充。

UE 按世界序号匹配相机，姿态可先于匹配的 World 到达。较新的姿态可覆盖共享槽中尚未采纳的姿态。粒子快照按 Minecraft tick 发送，UE 在两个 tick 的位置之间插值并面向当前相机绘制。离开世界清除地形及效果，保留界面与控制连接。主连接断开时 UE 清空地形、效果和界面。

按 Minecraft 的有效视距跟踪客户端已经加载的区块列，协议上限为 `(2 × 32 + 1)²` 列。世界高度上限为 1024 格且必须是 16 的倍数。单区段最多 1,048,576 个顶点。区段坐标由方块坐标分别向下取整除以 16 得到。地形和相机在传输时保留 Minecraft 坐标。UE 用 `(X, Y, Z) = (mcZ, -mcX, mcY)` 转换方向，将位置乘以 100 厘米。相机位置是渲染时的眼位。

UE 在三种地形模式下均用屏幕导数限制近处 texel 的双线性过渡。NONE 按原图集做三线性缩小，RGSS 额外加入旋转四点采样，ANISOTROPIC 则使用地形专用的硬件采样器。硬件级别取不超过请求值的最大 1、2、4 或 8。树叶、草丛等 CUTOUT 层按混合后 alpha 的 0.5 阈值遮罩，半透明层保留 alpha ≥ 0.1 的像素。粒子固定点采样第 0 层，裂纹和界面使用各自的纹理。

对照时保持 Minecraft 的 ANISOTROPIC 模式和同一画面，用 UE 启动参数 `-BridgeTerrainNoAF` 只关闭硬件各向异性，保留同一套 mip 和 atlas 布局。UE 参数 `-BridgeTerrainMip0` 强制地形只用第 0 层点采样，可对照 mip 的作用。日志记录有效模式、级别和 mip 数。

共享内存由 Fabric 创建，名字为大写带花括号的 GUID。三个槽各 64 MiB，帧内容从槽起点开始，按从下到上的行排列，每像素为预乘透明度的 RGBA8。允许的单边上限为 8192，总像素上限为 4096 × 4096。每槽获得 UE 的确认后才能重写。UE 每次取已收到的最新帧，较早的槽也会确认释放。静止界面沿用上一帧。

### v27 气泡柱扩展

UE 映射并验证控制区和地形区以后，在控制区偏移 16 以 release 原子写发布 \`u64 capabilities\`，再回复 Reset。Fabric 在握手后以 acquire 读取，位 0 置 1 时才发送 \`ChunkWaterBubbles\`。旧渲染器保留的零值会让新 Fabric 跳过该消息。旧 Fabric 可以照常连接新 UE。

消息正文为大端 \`u32 scene, i32 chunkX, i32 chunkZ, u16 count, u16 reserved\`，后跟 count 个 6 B 记录。每条记录为 \`u8 localXZ, u8 dragDown, u16 bottom, u16 height\`。localXZ 的低四位为区块内 X，高四位为 Z。bottom 相对世界 minY。height 为连续气泡柱的格数。dragDown 为 0 表示上浮，1 表示下沉。同一列的记录按 bottom 递增且不得重叠。一个区块最多 4096 条，reserved 必须为 0。

每次消息替换该区块的全量气泡柱。区块离开水体跟踪范围时发送零计数更新。DropChunk、切换世界和断线清理相关记录及发射实例。该消息沿用可靠 TCP 地形队列，不改变 v27 的控制区大小、相机槽、多路队列或 v26 水体布局。

### v26 水体

`SectionMesh` 保留原来的 36 B 区段头和 101 B 四边形。在六桶几何之后追加 `u32 waterCount, u32 shoreCount`。随后依次是 Water 桶顺序的 16 B 逐面记录和 17 B 岸线段。水面记录数必须等于 Water 顶点数除以四。同一岸面最多携带两条线段，线段引用的是传输顺序中的零基桶内面号。区段总顶点上限仍为 1,048,576，区段网格正文上限为 35,389,484 B。

| 水面字节 | 内容 |
| --- | --- |
| 0 | 位 0–1 为自由顶面、开放侧面、透明接触或底面。位 2–3 为静水、水平流或下落。位 4 表示水源，位 5 表示原版 FALLING 状态。位 6–7 保留。 |
| 1–3 | 原版水量 `u8`、原版归一化水平分量 `i8 fx, fz`。分量单位为 1/127，UE 水平方向为 `(fz, -fx)`。 |
| 4–7 | `u8 strength, turbulence, exposure, edges`。前两项单位为 1/255。曝光位 0 是最大天空光，位 1 是下落柱触及区段采样边界。岸约束低四位为西、东、北、南。 |
| 8–11 | 西北、东北、西南、东南角水高，格内 `u8`，单位为 1/64 格。 |
| 12–15 | 连续下落水格数、入水冲击强度 `u8 / 255`、距崖唇格数、距落点格数。紧接下落柱的上游顶格也携带其下方柱长，最后两项为零。较早的 v26 发送端在该顶格报告零柱长。 |

岸线段格式为 `u8 bucket, u32 quad, i16 ax, ay, az, bx, by, bz`。端点以区段原点为基准，单位为 1/64 格。只有确有接触的陆地面携带记录。重新分桶后的面继续携带其原记录。对岸线只做材质着色，在单独的 GPU 逐面缓冲中求值。

`37 · ChunkWaterGrid` 为 `u32 worldSerial, i32 chunkX, i32 chunkZ, u32 cells[256]`，共 1036 B，按照 `lz*16+lx` 排列。每列依次为高度整数（位 0–10，相对 MinY）、高度小数（位 11–16，1/64 格）、连续水深（位 17–21）、光学预设（位 22–25）、列已知、自由水面存在、露天、邻冰、障碍、深度饱和（位 26–31）。光学预设 0 为原版色回退，1 为沼泽，2 为暖海，3 为海洋，4 为河流。全零网格撤销区块列数据。几何区块卸载和场景重置也会清理对应状态。

## UE → Fabric

类型 8 · Input 包含 `u32 captureId`、`f32 dx, dy`、`u16 viewportWidth, viewportHeight`、`u32 readyScene`、`u8 releasedUiSlots`、`u8 eventCount`、`u32 debugScene`、`f32 frameMs`、`u32 loadedSections, pendingSections`、`u16 receivedColumns`、`u8 viewDistance`、`u16 fpsLimit`、`u8 vsync`、`u16 debugPanelBottom`、`u32 lookSerial`、`u64 presentingWindow`、`u32 atlasReplyScene`、`u8 atlasReplyStatus`、`u8 terrainReleaseCount`，接着是按顺序排列的输入事件及 `terrainReleaseCount` 个 `u64 token`。长度含类型时，固定部分为 65 B，每条最多归还 32 个 token。图集状态依次是 0 无申请、1 拒绝、2 就绪、3 正在准备，只对匹配的场景生效。相对鼠标轴正方向为右、下。启用控制时，每条含非零相对鼠标移动的消息分配一个非零 `lookSerial`，等待 Camera 确认。`readyScene` 报告已完成玩家区块绘制准备的世界序号，未就绪为 0。槽掩码的位 0–2 分别确认对应的共享内存槽。

单窗口模式下，Fabric 等启动遮罩消失、主菜单可用后才发送 Window，随后收到视口尺寸时开始采集界面。UE 提交首张界面帧后显示自身窗口，并以 `presentingWindow` 回报本机 HWND，准备期间为 0。Minecraft 在收到这个回执后隐藏原生窗口。

`debugScene` 是统计和本帧控制输入归属的世界序号。Fabric 跳过旧世界的控制事件，继续应用视口和槽释放信息。`frameMs` 是 UE 引擎的平滑帧时间，0 表示尚无样本。`loadedSections` 是已有地形网格的区段数，`pendingSections` 是 UE 收到后等待上传的区段数，`receivedColumns` 是 UE 已接收且仍在范围内的区块列数。地形处于活动状态时约每 250 毫秒采样，未捕获输入时也发送。随后回报当前视距、实际限帧（含空闲降帧）及垂直同步状态。`debugPanelBottom` 是右上角浮窗加间距占视口高度的比例，按 `0..65535` 编码，0 表示隐藏。

每个事件为 `u8 action`、`u32 value`、`u8 flags`。动作 0、1、2 依次是键盘松开、按下、长按重复，值为 Windows 扫描码，由 Minecraft 当前 GLFW 的键表还原。动作 3 的值是提交的 Unicode 码点，标志字节为零。`flags` 的低 6 位按 GLFW 的 Shift、Ctrl、Alt、Super、Caps Lock 和 Num Lock 顺序编码。每帧最多 64 个事件。

动作 4 是界面鼠标移动。`value` 的高低半字以有符号 16 位整数编码横纵坐标，除以 8192 得到相对于界面矩形的归一化坐标。矩形外仍传相对坐标以支持拖动。动作 5–8 分别是界面按下、界面松开、游戏内按下、游戏内松开。`value` 为按钮索引 0–4，依次对应左键、右键、中键和两个侧键，`flags` 为修饰键。动作 9 的 `value` 以浮点数编码滚动量，`flags` 的位 0 表示横向，位 1 表示界面模式。

类型 20 · FileDrop 包含 `u32 worldSerial`、`u8 fileCount`，随后逐个发送 `u16 byteLength` 和 UTF-8 绝对路径。单条最多 32 项，每条路径最多 4096 字节，帧总长最多 32 KiB。只在匹配的场景和普通界面中交给 Minecraft 的文件拖放回调。

每次进入控制使用新的非零 `captureId`。保持控制时有输入即发送，静止时每约 100 毫秒发送当前状态。`captureId=0` 时，鼠标状态和事件数为零，仍可同步视口并确认界面槽。断线、Minecraft 重新获得焦点或超过 400 毫秒未收到输入时，Fabric 清除保持和待发事件，恢复通道后接收新的鼠标与键盘事件。单窗口正常关闭时，UE 可发送无消息体的类型 13 通知 MC 保存并退出。
