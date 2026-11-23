package dev.minecraftue.bridge;

import com.mojang.blaze3d.textures.GpuTexture;
import com.mojang.blaze3d.vertex.PoseStack;
import com.mojang.logging.LogUtils;
import net.minecraft.client.Camera;
import net.minecraft.client.DeltaTracker;
import net.minecraft.client.InactivityFpsLimit;
import net.minecraft.client.Minecraft;
import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.client.renderer.block.BlockStateModelSet;
import net.minecraft.client.renderer.state.GameRenderState;
import net.minecraft.client.renderer.state.level.CameraRenderState;
import net.minecraft.client.renderer.texture.TextureAtlas;
import net.minecraft.core.BlockPos;
import net.minecraft.core.SectionPos;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.data.AtlasIds;
import net.minecraft.world.InteractionHand;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.chunk.LevelChunk;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.HitResult;
import net.minecraft.world.phys.Vec3;
import net.minecraft.world.phys.shapes.CollisionContext;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.joml.Vector3fc;
import org.slf4j.Logger;

import java.nio.ByteBuffer;
import java.util.function.Consumer;

public final class WorldBridge {
    public static final WorldBridge INSTANCE = new WorldBridge();
    private static final Logger LOGGER = LogUtils.getLogger();
    private final SceneMeshStream terrain = new SceneMeshStream();
    private final SceneEffectsStream effects = new SceneEffectsStream();
    private final DynamicSceneStream dynamic = new DynamicSceneStream();
    private final PrecipitationGridStream precipitation = new PrecipitationGridStream();
    private final WaterGridStream waterGrid = new WaterGridStream();
    private final LightningStream lightning = new LightningStream();
    private final ControlSharedMemory.StateSnapshot shmState = new ControlSharedMemory.StateSnapshot();
    private final ControlSharedMemory.LookSample ueLook = new ControlSharedMemory.LookSample();
    private final ByteBuffer cameraPose = ByteBuffer.allocate(ControlSharedMemory.CAMERA_SAMPLE_BYTES);
    private ClientLevel level;
    private LocalTransport transport;
    private LocalTransport.Connection connection;
    private ControlSharedMemory controlShm;
    private boolean selectionOutlineSupported;
    private BlockStateModelSet modelSource;
    private GpuTexture atlasSource;
    private MeshOptions meshOptions;
    private VideoSettings advertisedVideo;
    private int advertisedInput = -1;
    private int advertisedFullscreen = -1;
    private boolean advertisedWindow;
    private int sceneSerial;
    private boolean sceneBridged;
    private boolean sceneReady;
    private boolean terrainFailed;
    private boolean sampleEnvironment;
    private EnvironmentState advertisedEnvironment;
    private long tickCounter;
    private long lastTickTimeNs;
    private long overrideEpoch;
    private int teleportEpoch;
    private float mainHandEquip = 1.0f;
    private float offHandEquip = 1.0f;
    private boolean lastGameplayLook;
    private boolean hasEyeHeight;
    private float prevEyeHeight;
    private float currEyeHeight;
    private boolean hasEyePose;
    private double lastEyeX;
    private double lastEyeY;
    private double lastEyeZ;
    private float lastVerticalFov = 70.0f;
    private float lastAspectRatio = 16.0f / 9.0f;

    private WorldBridge() {}

    public void start() {
        BridgeSettings.read();
        if (transport == null) {
            transport = new LocalTransport();
        }
    }

    public void close() {
        terrain.close();
        dynamic.reset();
        if (transport != null) {
            transport.close();
        }
    }

    private void refreshConnection(Minecraft minecraft) {
        start();
        LocalTransport.Connection ready = transport.current();
        if (connection != ready) {
            connection = ready;
            level = null;
            advertisedInput = -1;
            advertisedFullscreen = -1;
            advertisedWindow = false;
            advertisedVideo = null;
            controlShm = ready == null ? null : ready.controlShm;
            selectionOutlineSupported = controlShm != null && controlShm.supports(ControlSharedMemory.CAP_SELECTION_OUTLINE);
            terrain.reset();
            effects.reset();
            dynamic.reset();
            precipitation.reset();
            waterGrid.reset();
            lightning.reset();
            terrainFailed = false;
            sampleEnvironment = false;
            advertisedEnvironment = null;
            hasEyePose = false;
            hasEyeHeight = false;
            lastGameplayLook = false;
        }
        if (ready != null && BridgePresentation.canCapture(minecraft)) {
            if (!advertisedWindow) {
                var window = minecraft.getWindow();
                advertisedWindow = ready.sendControl(BridgeWire.frame(BridgeWire.WINDOW, 13)
                        .putInt(window.getX()).putInt(window.getY()).putShort((short) window.getWidth())
                        .putShort((short) window.getHeight()).put((byte) (BridgePresentation.REQUESTED ? 1 : 0)).flip());
                if (!advertisedWindow) {
                    return;
                }
            }
            VideoSettings video = VideoSettings.read(minecraft);
            if (!video.equals(advertisedVideo) && ready.sendControl(
                    BridgeWire.frame(BridgeWire.VIDEO_SETTINGS, BridgeWire.VIDEO_SETTINGS_BYTES)
                            .put((byte) video.viewDistance()).putShort((short) video.fpsLimit())
                            .put((byte) video.flags()).put((byte) video.filtering()).put((byte) video.anisotropy()).flip())) {
                advertisedVideo = video;
            }
            if (BridgePresentation.REQUESTED) {
                int fullscreen = minecraft.getWindow().isFullscreen() ? 1 : 0;
                if (advertisedFullscreen != fullscreen && ready.sendControl(BridgeWire.frame(BridgeWire.WINDOW_MODE, 1)
                        .put((byte) fullscreen).flip())) {
                    advertisedFullscreen = fullscreen;
                }
            }
            int inputMode = RemoteControls.inputMode(minecraft);
            if (advertisedInput != inputMode && ready.sendControl(BridgeWire.frame(BridgeWire.INPUT_AVAILABILITY, 1)
                    .put((byte) inputMode).flip())) {
                advertisedInput = inputMode;
            }
        }
    }

    public void tick(Minecraft minecraft) {
        refreshConnection(minecraft);
        if (inputConnection() == null) {
            return;
        }
        ClientLevel currentLevel = minecraft.player != null ? minecraft.level : null;
        if (currentLevel != null && currentLevel.getHeight() > BridgeWire.MAX_HEIGHT) {
            currentLevel = null;
        }
        BlockStateModelSet currentModels = currentLevel == null ? null : minecraft.getModelManager().getBlockStateModelSet();
        var currentAtlas = currentLevel == null ? null : minecraft.getAtlasManager().getAtlasOrThrow(AtlasIds.BLOCKS);
        GpuTexture currentTexture = currentAtlas == null ? null : currentAtlas.getTexture();
        MeshOptions currentOptions = new MeshOptions(minecraft.options.ambientOcclusion().get(),
                minecraft.options.cutoutLeaves().get(), minecraft.options.biomeBlendRadius().get(),
                BridgeSettings.connectedGlassEnabled());
        if (level != currentLevel || (currentLevel != null && (modelSource != currentModels
                || atlasSource != currentTexture || !currentOptions.equals(meshOptions)))) {
            level = currentLevel;
            modelSource = currentModels;
            atlasSource = currentTexture;
            meshOptions = currentOptions;
            terrain.reset();
            effects.reset();
            dynamic.reset();
            precipitation.reset();
            waterGrid.reset();
            lightning.reset();
            sceneReady = false;
            terrainFailed = false;
            sampleEnvironment = false;
            advertisedEnvironment = null;
            hasEyePose = false;
            hasEyeHeight = false;
            lastGameplayLook = false;
            overrideEpoch++;
            teleportEpoch++;
            connection.changeInputScene(sceneSerial + 1);
            if (!connection.sendTerrain(BridgeWire.frame(BridgeWire.WORLD, 12).putInt(++sceneSerial)
                    .putInt(level == null ? 0 : level.getMinY()).putInt(level == null ? 0 : level.getHeight()).flip())) {
                return;
            }
            publishControlState(minecraft, false);
            if (level != null) {
                try {
                    terrain.start(minecraft, currentLevel, currentAtlas,
                            connection.controlShm.supports(ControlSharedMemory.CAP_STAINED_GLASS));
                    dynamic.trackBlockAtlas(currentAtlas);
                } catch (RuntimeException failure) {
                    fallBackToMinecraft(minecraft, failure);
                }
            }
        }
        publishControlState(minecraft, true);
        if (level == null || terrainFailed || advertisedVideo == null) {
            return;
        }
        sampleEnvironment = true;
        if (!sceneReady && connection.readyScene == sceneSerial && minecraft.getConnection() != null) {
            // Replace the vanilla compiled-section signal with confirmation from the renderer owning the scene.
            Runnable ready = minecraft.getConnection().getPlayerCompiledSectionCallback();
            if (ready != null) {
                ready.run();
                sceneReady = true;
            }
        }

        try {
            Vec3 eye = minecraft.player.getEyePosition();
            // The UE acknowledgement follows the block atlas and initial terrain meshes in the reliable stream.
            if (connection.readyScene == sceneSerial) {
                effects.tick(connection, sceneSerial, minecraft, level, modelSource, eye);
                waterGrid.tick(connection, sceneSerial, level, minecraft.player.chunkPosition(),
                        advertisedVideo.viewDistance());
                precipitation.tick(connection, sceneSerial, level, minecraft.player.chunkPosition(),
                        advertisedVideo.viewDistance());
                lightning.tick(connection, sceneSerial, level);
            }
            terrain.tick(connection, sceneSerial, SectionPos.of(minecraft.player),
                    eye, advertisedVideo.viewDistance());
        } catch (RuntimeException failure) {
            fallBackToMinecraft(minecraft, failure);
        }
    }

    private void fallBackToMinecraft(Minecraft minecraft, RuntimeException failure) {
        LOGGER.error("Minecraft UE terrain export failed, rendering this world in Minecraft", failure);
        terrainFailed = true;
        terrain.reset();
        effects.reset();
        dynamic.reset();
        hasEyePose = false;
        hasEyeHeight = false;
        lastGameplayLook = false;
        overrideEpoch++;
        connection.changeInputScene(sceneSerial + 1);
        connection.sendTerrain(BridgeWire.frame(BridgeWire.WORLD, 12).putInt(++sceneSerial).putLong(0).flip());
        publishControlState(minecraft, false);
    }

    public void onServerForcedLook(Minecraft minecraft) {
        teleportEpoch++;
        publishControlState(minecraft, false, true);
    }

    public void onServerPositionSync(Minecraft minecraft) {
        teleportEpoch++;
        publishControlState(minecraft, false, false);
    }

    public void onTeleport(Minecraft minecraft) {
        hasEyeHeight = false;
        teleportEpoch++;
        publishControlState(minecraft, false, true);
    }

    public void syncControlState(Minecraft minecraft) {
        publishControlState(minecraft, false, false);
    }

    public void requestLookSync(Minecraft minecraft) {
        publishControlState(minecraft, false, true);
    }

    public void updateHandEquip(float mainHeight, float offHeight) {
        mainHandEquip = mainHeight;
        offHandEquip = offHeight;
    }

    boolean canUseZeroLatencyLook(Minecraft minecraft) {
        if (controlShm == null || level == null || terrainFailed) {
            return false;
        }
        LocalPlayer player = minecraft.player;
        return RemoteControls.gameplayScreen(minecraft)
                && player != null
                && minecraft.options.getCameraType().isFirstPerson()
                && !minecraft.options.smoothCamera
                && player.getVehicle() == null
                && !player.isSleeping()
                && minecraft.getCameraEntity() == player;
    }

    boolean applyZeroLatencyLook(Minecraft minecraft, int activeCapture, LocalTransport.Connection peer) {
        if (activeCapture == 0 || !canUseZeroLatencyLook(minecraft)) {
            return false;
        }
        if (!controlShm.readUeLook(ueLook)) {
            return false;
        }
        if (!ueLook.active() || ueLook.worldSerial != sceneSerial || ueLook.captureId != activeCapture
                || ueLook.ackOverrideEpoch != overrideEpoch
                || !Float.isFinite(ueLook.yaw) || !Float.isFinite(ueLook.pitch)) {
            return false;
        }
        float clampedPitch = Math.clamp(ueLook.pitch, -90.0f, 90.0f);
        LocalPlayer player = minecraft.player;
        boolean changed = player.getYRot() != ueLook.yaw || player.getXRot() != clampedPitch;
        player.setYRot(ueLook.yaw);
        player.setXRot(clampedPitch);
        player.yRotO = ueLook.yaw;
        player.xRotO = clampedPitch;
        if (changed) {
            minecraft.getFramerateLimitTracker().onInputReceived();
        }
        if (ueLook.lookSerial != 0) {
            peer.recordAppliedLook(activeCapture, ueLook.lookSerial);
        }
        return true;
    }

    private void publishControlState(Minecraft minecraft, boolean advanceTick) {
        publishControlState(minecraft, advanceTick, false);
    }

    private void publishControlState(Minecraft minecraft, boolean advanceTick, boolean bumpOverride) {
        if (controlShm == null) {
            return;
        }
        LocalPlayer player = minecraft.player;
        boolean inWorld = level != null && player != null && !terrainFailed;
        boolean paused = minecraft.isPaused();
        if (advanceTick && inWorld && !paused) {
            tickCounter++;
            lastTickTimeNs = System.nanoTime();
        }
        boolean gameplayLook = canUseZeroLatencyLook(minecraft);
        if (bumpOverride || (gameplayLook && !lastGameplayLook)) {
            overrideEpoch++;
        }
        lastGameplayLook = gameplayLook;

        int flags = 0;
        if (inWorld) {
            flags |= ControlSharedMemory.FLAG_IN_WORLD;
        }
        if (gameplayLook) {
            flags |= ControlSharedMemory.FLAG_GAMEPLAY_LOOK;
        }
        if (minecraft.gui.screen() != null || minecraft.gui.overlay() != null) {
            flags |= ControlSharedMemory.FLAG_GUI_OPEN;
        }
        if (paused) {
            flags |= ControlSharedMemory.FLAG_IS_PAUSED;
        }
        if (minecraft.options.invertMouseX().get()) {
            flags |= ControlSharedMemory.FLAG_INVERT_X;
        }
        if (minecraft.options.invertMouseY().get()) {
            flags |= ControlSharedMemory.FLAG_INVERT_Y;
        }
        if (hasEyePose && inWorld) {
            flags |= ControlSharedMemory.FLAG_HAS_EYE_POSE;
        }

        double d = minecraft.options.sensitivity().get() * 0.6000000238418579 + 0.20000000298023224;
        double e = d * d * d;
        double mouseFactor = (player != null && minecraft.options.getCameraType().isFirstPerson() && player.isScoping())
                ? e : e * 8.0;

        ControlSharedMemory.StateSnapshot s = shmState;
        s.worldSerial = sceneSerial;
        s.tickId = tickCounter;
        s.tickTimeNs = lastTickTimeNs;
        s.overrideEpoch = overrideEpoch;
        s.teleportEpoch = teleportEpoch;
        s.mouseFactor = mouseFactor;
        s.verticalFov = lastVerticalFov;
        s.aspectRatio = lastAspectRatio;
        s.eyeX = lastEyeX;
        s.eyeY = lastEyeY;
        s.eyeZ = lastEyeZ;
        s.hitBlockX = 0;
        s.hitBlockY = 0;
        s.hitBlockZ = 0;
        s.hitFace = 0;
        VoxelShape selectionOutline = null;

        if (player != null && inWorld) {
            if (player.isShiftKeyDown()) {
                flags |= ControlSharedMemory.FLAG_SNEAKING;
            }
            if (player.isSprinting()) {
                flags |= ControlSharedMemory.FLAG_SPRINTING;
            }
            if (player.isSwimming()) {
                flags |= ControlSharedMemory.FLAG_SWIMMING;
            }
            if (player.isUsingItem()) {
                flags |= ControlSharedMemory.FLAG_USING_ITEM;
            }
            s.prevX = player.xo;
            s.prevY = player.yo;
            s.prevZ = player.zo;
            s.currX = player.getX();
            s.currY = player.getY();
            s.currZ = player.getZ();
            float eyeH = player.getEyeHeight();
            if (!hasEyeHeight) {
                prevEyeHeight = currEyeHeight = eyeH;
                hasEyeHeight = true;
            } else if (advanceTick && !paused) {
                prevEyeHeight = currEyeHeight;
                currEyeHeight += (eyeH - currEyeHeight) * 0.5f;
            }
            s.prevEyeHeight = prevEyeHeight;
            s.currEyeHeight = currEyeHeight;
            s.forcedYaw = player.getYRot();
            s.forcedPitch = player.getXRot();
            s.mainHandSwing = player.getAttackAnim(1.0f);
            s.mainHandEquip = mainHandEquip;
            s.offHandSwing = 0.0f;
            s.offHandEquip = offHandEquip;
            s.useAction = player.isUsingItem() ? (byte) player.getUseItem().getUseAnimation().ordinal() : 0;
            s.useProgress = player.isUsingItem() ? player.getTicksUsingItem() / 20.0f : 0.0f;
            s.mainHandModelHash = computeItemModelHash(player.getItemInHand(InteractionHand.MAIN_HAND));
            s.offHandModelHash = computeItemModelHash(player.getItemInHand(InteractionHand.OFF_HAND));

            if (!minecraft.gui.hud.isHidden() && minecraft.hitResult instanceof BlockHitResult bhr
                    && bhr.getType() == HitResult.Type.BLOCK) {
                BlockPos hitPos = bhr.getBlockPos();
                flags |= ControlSharedMemory.FLAG_HAS_BLOCK_HIT;
                s.hitBlockX = hitPos.getX();
                s.hitBlockY = hitPos.getY();
                s.hitBlockZ = hitPos.getZ();
                s.hitFace = (byte) bhr.getDirection().ordinal();
                if (selectionOutlineSupported && (flags & ControlSharedMemory.FLAG_GUI_OPEN) == 0) {
                    var cameraEntity = minecraft.getCameraEntity();
                    var context = cameraEntity == null ? CollisionContext.empty() : CollisionContext.of(cameraEntity);
                    selectionOutline = level.getBlockState(hitPos).getShape(level, hitPos, context);
                }
            }
        } else {
            hasEyeHeight = false;
            s.prevX = s.prevY = s.prevZ = 0.0;
            s.currX = s.currY = s.currZ = 0.0;
            s.prevEyeHeight = s.currEyeHeight = 0.0f;
            s.forcedYaw = s.forcedPitch = 0.0f;
            s.mainHandSwing = s.offHandSwing = s.useProgress = 0.0f;
            s.useAction = 0;
            s.mainHandModelHash = s.offHandModelHash = 0L;
        }
        s.flags = flags;
        controlShm.publishState(s, selectionOutline);
    }

    private static long computeItemModelHash(ItemStack stack) {
        if (stack.isEmpty()) {
            return 0L;
        }
        String key = BuiltInRegistries.ITEM.getKey(stack.getItem()).toString();
        long h = 0xcbf29ce484222325L;
        for (int i = 0; i < key.length(); i++) {
            h ^= key.charAt(i);
            h *= 0x100000001b3L;
        }
        return h;
    }

    LocalTransport.Connection inputConnection() {
        return BridgePresentation.REQUESTED && !BridgePresentation.isHidden() ? null : presentationConnection();
    }

    void deliverFileDrops(Minecraft minecraft, LocalTransport.Connection peer) {
        LocalTransport.DroppedFiles drop;
        while ((drop = peer.fileDrops.poll()) != null) {
            if (drop.scene() == sceneSerial && minecraft.gui.overlay() == null && minecraft.gui.screen() != null) {
                minecraft.gui.screen().onFilesDrop(drop.paths());
            }
        }
    }

    private LocalTransport.Connection presentationConnection() {
        return connection != null && connection.viewport != 0 && transport.current() == connection ? connection : null;
    }

    public boolean keepWorldRunning(Minecraft minecraft) {
        return inputConnection() != null && RemoteControls.gameplayScreen(minecraft);
    }

    public boolean beginRenderFrame(Minecraft minecraft, boolean renderLevel) {
        refreshConnection(minecraft);
        LocalTransport.Connection peer = presentationConnection();
        if (transport.takeCloseRequest() && BridgePresentation.isHidden()) {
            minecraft.stop();
        }
        if (!BridgePresentation.canCapture(minecraft)) {
            peer = null;
        }
        BridgePresentation.update(minecraft, peer);
        UiCapture.INSTANCE.setConnection(peer);
        sceneBridged = inputConnection() != null && renderLevel && minecraft.level != null && minecraft.player != null
                && (!terrainFailed || minecraft.level != level) && minecraft.level.getHeight() <= BridgeWire.MAX_HEIGHT;
        return sceneBridged;
    }

    public boolean isSceneBridged() { return sceneBridged; }

    public void atlasAnimated(TextureAtlas atlas) {
        dynamic.atlasAnimated(atlas);
    }

    public void captureDynamic(Minecraft minecraft, Camera camera, GameRenderState renderState, DeltaTracker delta,
                               Consumer<PoseStack> handEffects) {
        if (sceneBridged && !terrainFailed && connection != null && level != null && minecraft.level == level
                && minecraft.player != null && connection.readyScene == sceneSerial) {
            dynamic.capture(connection, sceneSerial, minecraft, level, camera, renderState, delta, handEffects);
        }
    }

    public DebugStats debugStats() {
        if (!sceneBridged || connection == null) {
            return null;
        }
        DebugStats stats = connection.debugStats;
        return stats != null && stats.scene() == sceneSerial && System.nanoTime() - stats.receivedAt() < 1_000_000_000L
                ? stats : null;
    }

    public record DebugStats(int scene, float frameMs, int loadedSections, int pendingSections, int receivedColumns,
                             int viewDistance, int fpsLimit, boolean vsync,
                             int panelBottom, long receivedAt) {
        public int panelInset(int guiHeight) {
            return (int) Math.ceil(guiHeight * (panelBottom / 65535.0));
        }
    }

    public void blockChanged(ClientLevel changedLevel, BlockPos pos) {
        terrain.blockChanged(changedLevel, pos);
        if (level == changedLevel) {
            precipitation.blockChanged(pos);
            waterGrid.blockChanged(pos);
        }
    }

    public void lightChanged(ClientLevel changedLevel, SectionPos section) {
        terrain.lightChanged(changedLevel, section);
    }

    public void chunkReplaced(LevelChunk chunk) {
        terrain.chunkReplaced(chunk);
        if (level == chunk.getLevel()) {
            precipitation.chunkReplaced(chunk.getPos());
            waterGrid.chunkChanged(chunk.getPos(), false);
        }
    }

    public void chunkBiomesChanged(LevelChunk chunk) {
        if (level == chunk.getLevel()) {
            precipitation.chunkReplaced(chunk.getPos());
        }
    }

    public void chunkDropped(ChunkPos pos) {
        terrain.chunkDropped(pos);
        precipitation.chunkDropped(pos);
        waterGrid.chunkChanged(pos, true);
    }

    public void captureCamera(Camera camera, CameraRenderState state, float partialTick) {
        if (controlShm == null || connection == null || level == null || terrainFailed || !state.initialized || state.isPanoramicMode) {
            return;
        }
        float verticalScale = Math.abs(state.projectionMatrix.m11());
        float aspect = verticalScale / Math.abs(state.projectionMatrix.m00());
        float verticalFov = (float) Math.toDegrees(2 * Math.atan(1.0 / verticalScale));
        lastEyeX = state.pos.x;
        lastEyeY = state.pos.y;
        lastEyeZ = state.pos.z;
        lastVerticalFov = verticalFov;
        lastAspectRatio = aspect;
        hasEyePose = true;
        Minecraft client = Minecraft.getInstance();
        publishControlState(client, false);

        LocalPlayer player = client.player;
        int playerView = DynamicSceneStream.playerView(camera, player);
        Vec3 playerPosition = playerView != BridgeWire.PLAYER_VIEW_NONE ? player.getPosition(partialTick) : Vec3.ZERO;

        Vector3fc forward = camera.forwardVector();
        Vector3fc up = camera.upVector();
        ByteBuffer pose = cameraPose.clear().putInt(sceneSerial);
        pose.putDouble(state.pos.x).putDouble(state.pos.y).putDouble(state.pos.z);
        pose.putFloat(forward.x()).putFloat(forward.y()).putFloat(forward.z());
        pose.putFloat(up.x()).putFloat(up.y()).putFloat(up.z());
        pose.putFloat(verticalFov).putFloat(aspect);
        pose.putInt(connection.appliedLookSerial());
        pose.put((byte) playerView).putDouble(playerPosition.x).putDouble(playerPosition.y).putDouble(playerPosition.z);
        pose.putInt(connection.appliedLookCapture());
        int movementFlags = 0;
        if (playerView != BridgeWire.PLAYER_VIEW_NONE) {
            movementFlags = ControlSharedMemory.CAMERA_MOVEMENT_KNOWN;
            if (player.onGround() && !player.getAbilities().flying && !player.isFallFlying()
                    && !player.isSpectator() && !player.isPassenger() && !player.isInWater()) {
                movementFlags |= ControlSharedMemory.CAMERA_GROUNDED_ON_FOOT;
            }
        }
        pose.put((byte) movementFlags);
        controlShm.publishCamera(pose);
    }

    public void captureEnvironment(ClientLevel currentLevel, Camera camera, CameraRenderState state, float partialTick) {
        if (!sampleEnvironment || connection == null || level == null || currentLevel != level
                || terrainFailed || !state.initialized || state.isPanoramicMode) {
            return;
        }
        sampleEnvironment = false;
        EnvironmentState environment = EnvironmentState.sample(level, camera, state, partialTick);
        if (!environment.equals(advertisedEnvironment)) {
            connection.sendEnvironment(environment.encode(sceneSerial));
            advertisedEnvironment = environment;
        }
    }

    private record MeshOptions(boolean ambientOcclusion, boolean cutoutLeaves, int biomeBlendRadius,
                               boolean connectedGlass) {}

    private record VideoSettings(int viewDistance, int fpsLimit, int flags, int filtering, int anisotropy) {
        private static VideoSettings read(Minecraft minecraft) {
            int fps = minecraft.options.framerateLimit().get();
            int flags = (minecraft.options.enableVsync().get() ? 1 : 0)
                    | (minecraft.options.inactivityFpsLimit().get() == InactivityFpsLimit.AFK ? 2 : 0);
            int filtering = switch (minecraft.options.textureFiltering().get()) {
                case NONE -> 0;
                case RGSS -> 1;
                case ANISOTROPIC -> 2;
            };
            int anisotropy = filtering == 2 ? minecraft.options.maxAnisotropyValue() : 1;
            return new VideoSettings(Math.clamp(minecraft.options.getEffectiveRenderDistance(), 2, BridgeWire.MAX_VIEW_DISTANCE),
                    fps == BridgeWire.UNLIMITED_FPS ? 0 : fps, flags, filtering, anisotropy);
        }
    }
}
