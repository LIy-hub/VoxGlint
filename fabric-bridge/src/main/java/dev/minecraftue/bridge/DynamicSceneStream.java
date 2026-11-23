package dev.minecraftue.bridge;

import com.mojang.blaze3d.vertex.PoseStack;
import com.mojang.logging.LogUtils;
import net.minecraft.client.Camera;
import net.minecraft.client.DeltaTracker;
import net.minecraft.client.Minecraft;
import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.client.renderer.SubmitNodeStorage;
import net.minecraft.client.renderer.culling.Frustum;
import net.minecraft.client.renderer.entity.ItemEntityRenderer;
import net.minecraft.client.renderer.entity.state.AvatarRenderState;
import net.minecraft.client.renderer.entity.state.ItemEntityRenderState;
import net.minecraft.client.renderer.feature.FeatureFrameContext;
import net.minecraft.client.renderer.rendertype.RenderType;
import net.minecraft.client.renderer.rendertype.RenderTypes;
import net.minecraft.client.renderer.state.GameRenderState;
import net.minecraft.client.renderer.state.level.CameraRenderState;
import net.minecraft.client.renderer.state.level.ParticlesRenderState;
import net.minecraft.client.renderer.texture.TextureAtlas;
import net.minecraft.core.SectionPos;
import net.minecraft.util.Mth;
import net.minecraft.util.RandomSource;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.LightningBolt;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.item.ItemEntity;
import net.minecraft.world.level.block.entity.BlockEntity;
import net.minecraft.world.level.chunk.status.ChunkStatus;
import net.minecraft.world.phys.Vec3;
import org.slf4j.Logger;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.UUID;
import java.util.function.Consumer;

import static dev.minecraftue.bridge.BridgeWire.*;

final class DynamicSceneStream {
    private static final Logger LOGGER = LogUtils.getLogger();
    private static final int WORLD_RANGE = 64;
    private static final int MAX_WORLD_OBJECTS = 192;
    private static final double TRACKED_OBJECT_RANGE_SQ = 36.0 * 36.0;
    private static final int MAX_POSED_PARTS = 96;
    private final DynamicTextureStream textures = new DynamicTextureStream();
    private final DynamicMeshCapture geometry = new DynamicMeshCapture(textures);
    private boolean textureEmission;
    private final Stream world = new Stream(DYNAMIC_WORLD, MAX_DYNAMIC_WORLD_VERTICES, 30);
    private final Stream hand = new Stream(DYNAMIC_HAND, MAX_DYNAMIC_HAND_VERTICES, 60);
    private final Stream localPlayer = new Stream(DYNAMIC_LOCAL_PLAYER, MAX_DYNAMIC_LOCAL_PLAYER_VERTICES, 60);
    private final Stream particles = new Stream(DYNAMIC_PARTICLES, MAX_PARTICLE_VERTICES, 60);
    private final ParticlesRenderState particleState = new ParticlesRenderState();
    private final RandomSource itemRandom = RandomSource.create(0);
    private final Set<Object> loggedFailures = new HashSet<>();

    void reset() {
        textures.reset();
        world.reset();
        hand.reset();
        localPlayer.reset();
        particles.reset();
        loggedFailures.clear();
    }

    void trackBlockAtlas(TextureAtlas blocks) {
        textures.trackBlockAtlas(blocks);
    }

    void atlasAnimated(TextureAtlas atlas) {
        textures.atlasAnimated(atlas);
    }

    static int playerView(Camera camera, LocalPlayer player) {
        if (player == null || camera.entity() != player || player.isRemoved()) {
            return PLAYER_VIEW_NONE;
        }
        if (camera.isDetached() || player.isSleeping()) {
            return PLAYER_VIEW_WORLD;
        }
        return player.isSpectator() ? PLAYER_VIEW_NONE : PLAYER_VIEW_FIRST_PERSON;
    }

    void capture(LocalTransport.Connection connection, int scene, Minecraft client, ClientLevel level, Camera camera,
                 GameRenderState renderState, DeltaTracker delta, Consumer<PoseStack> handEffects) {
        CameraRenderState cameraState = renderState.levelRenderState.cameraRenderState;
        if (!cameraState.initialized || cameraState.isPanoramicMode) {
            return;
        }
        long now = System.nanoTime();
        int playerView = playerView(camera, client.player);
        boolean playerPresent = playerView != PLAYER_VIEW_NONE;
        boolean handsVisible = playerView == PLAYER_VIEW_FIRST_PERSON && !renderState.guiRenderState.isHudHidden
                && client.gameMode != null;
        boolean captureHand = hand.due(connection, now, handsVisible != hand.wasVisible);
        boolean capturePlayer = localPlayer.due(connection, now, playerPresent != localPlayer.wasVisible);
        boolean captureWorld = world.due(connection, now, false);
        textureEmission = connection.controlShm.supports(ControlSharedMemory.CAP_TEXTURE_EMISSION);
        boolean captureParticles = particles.due(connection, now, false);
        if (captureHand || capturePlayer || captureWorld || captureParticles) {
            var entities = client.getEntityRenderDispatcher();
            entities.prepare(camera, client.crosshairPickEntity);
            var context = new FeatureFrameContext(renderState.optionsRenderState, client.font,
                    client.getModelManager().getBlockStateModelSet(), client.getBlockColors(), client.getTextureManager(),
                    client.getAtlasManager(), null, null);
            if (captureHand) {
                hand.begin(now);
                if (handsVisible) {
                    captureHands(client, camera, delta, context, handEffects);
                }
                hand.publish(connection, scene, cameraState.hudFov);
                hand.wasVisible = handsVisible;
            }
            if (capturePlayer) {
                localPlayer.begin(now);
                if (playerPresent) {
                    captureEntity(client, cameraState, context, client.player, camera.getCameraEntityPartialTicks(delta),
                            false, localPlayer);
                }
                localPlayer.publish(connection, scene, 0);
                localPlayer.wasVisible = playerPresent;
            }
            if (captureWorld) {
                world.begin(now);
                captureWorld(client, level, camera, cameraState, delta, context);
                world.publish(connection, scene, 0);
            }
            if (captureParticles) {
                particles.begin(now);
                captureParticles(client, camera, cameraState, delta, context);
                particles.publish(connection, scene, 0);
            }
        }
        textures.flush(client, connection, scene);
    }

    private void captureParticles(Minecraft client, Camera camera, CameraRenderState cameraState, DeltaTracker delta,
                                  FeatureFrameContext context) {
        try {
            Frustum frustum = new Frustum(camera.getCullFrustum()).offset(-3.0f);
            client.particleEngine.extract(particleState, frustum, camera, delta.getGameTimeDeltaPartialTick(false));
            if (!particleState.particles.isEmpty()) {
                var submits = new SubmitNodeStorage();
                particleState.submit(submits, cameraState);
                // Minecraft's particle vertices are relative to the very camera used for extraction.
                particles.add(geometry.captureParticles(client, context, submits, particles.remaining), cameraState.pos, 0);
            }
        } catch (RuntimeException failure) {
            logFailure("particles", failure);
        } finally {
            particleState.reset();
        }
    }

    private void captureHands(Minecraft client, Camera camera, DeltaTracker delta, FeatureFrameContext context,
                              Consumer<PoseStack> handEffects) {
        try {
            float partial = camera.getCameraEntityPartialTicks(delta);
            var entities = client.getEntityRenderDispatcher();
            var pose = new PoseStack();
            handEffects.accept(pose);
            var submits = new SubmitNodeStorage();
            entities.getItemInHandRenderer().submitHandsWithItems(partial, pose, submits, client.player,
                    entities.getPackedLightCoords(client.player, partial));
            RenderType skin = RenderTypes.entityTranslucent(client.player.getSkin().body().texturePath());
            hand.add(geometry.capture(client, context, submits, hand.remaining, skin), Vec3.ZERO, 0);
        } catch (RuntimeException failure) {
            logFailure("first-person hands", failure);
        }
    }

    private void captureWorld(Minecraft client, ClientLevel level, Camera camera, CameraRenderState cameraState,
                              DeltaTracker delta, FeatureFrameContext context) {
        var blocks = client.getBlockEntityRenderDispatcher();
        Vec3 eye = cameraState.pos;
        blocks.prepare(eye);
        List<WorldObject> nearby = new ArrayList<>();
        int viewDistance = client.options.getEffectiveRenderDistance();
        double radius = Math.min(WORLD_RANGE, viewDistance * 16);
        double rangeSquared = radius * radius;
        Entity.setViewScale(Math.clamp(viewDistance / 8.0, 1.0, 2.5) * client.options.entityDistanceScaling().get());
        for (Entity entity : level.entitiesForRendering()) {
            if (entity == client.player
                    || entity.isRemoved()
                    || entity instanceof LightningBolt
                    || (entity == camera.entity() && !camera.isDetached()
                        && !(entity instanceof LivingEntity living && living.isSleeping()))) {
                continue;
            }
            double distance = entity.distanceToSqr(eye);
            // Capture around the camera, including behind it, so a UE-local turn can reveal objects immediately.
            if (distance <= rangeSquared && entity.shouldRender(eye.x, eye.y, eye.z)) {
                nearby.add(new WorldObject(distance, entity, null));
            }
        }
        int firstX = SectionPos.blockToSectionCoord(Mth.floor(eye.x - radius));
        int lastX = SectionPos.blockToSectionCoord(Mth.floor(eye.x + radius));
        int firstZ = SectionPos.blockToSectionCoord(Mth.floor(eye.z - radius));
        int lastZ = SectionPos.blockToSectionCoord(Mth.floor(eye.z + radius));
        for (int z = firstZ; z <= lastZ; z++) {
            for (int x = firstX; x <= lastX; x++) {
                var chunk = level.getChunkSource().getChunk(x, z, ChunkStatus.FULL, false);
                if (chunk == null) {
                    continue;
                }
                for (BlockEntity block : chunk.getBlockEntities().values()) {
                    double distance = block.getBlockPos().distToCenterSqr(eye.x, eye.y, eye.z);
                    if (distance <= rangeSquared && !block.isRemoved() && blocks.getRenderer(block) != null) {
                        nearby.add(new WorldObject(distance, null, block));
                    }
                }
            }
        }
        nearby.sort(Comparator.comparingDouble(WorldObject::distance));
        int count = Math.min(nearby.size(), MAX_WORLD_OBJECTS);
        for (int i = 0; i < count && world.hasRoom(); i++) {
            WorldObject object = nearby.get(i);
            if (object.entity != null) {
                float partial = delta.getGameTimeDeltaPartialTick(!level.tickRateManager().isEntityFrozen(object.entity));
                captureEntity(client, cameraState, context, object.entity, partial,
                        object.distance <= TRACKED_OBJECT_RANGE_SQ, world);
            } else {
                captureBlockEntity(client, cameraState, delta, context, object.block,
                        object.distance <= TRACKED_OBJECT_RANGE_SQ);
            }
        }
    }

    private void captureEntity(Minecraft client, CameraRenderState cameraState, FeatureFrameContext context,
                               Entity entity, float partial, boolean trackPose, Stream stream) {
        try {
            var entities = client.getEntityRenderDispatcher();
            if (entity.tickCount == 0) {
                entity.xOld = entity.getX();
                entity.yOld = entity.getY();
                entity.zOld = entity.getZ();
            }
            var state = entities.extractEntity(entity, partial);
            var pose = new PoseStack();
            var submits = new SubmitNodeStorage();
            boolean local = stream == localPlayer;
            Vec3 position = local ? Vec3.ZERO : new Vec3(state.x, state.y, state.z);
            float yaw = 0;
            UUID entityKey = null;
            boolean interpolate = false;
            RenderType skin = null;
            if (state instanceof AvatarRenderState avatar) {
                skin = RenderTypes.entityTranslucent(avatar.skin.body().texturePath());
                if (local && !entity.isPassenger() && !client.player.isSleeping() && !avatar.isUpsideDown) {
                    // The local camera sees the latest look input before the player's head/body tick runs.
                    float viewYaw = client.player.getViewYRot(partial);
                    float headYaw = Mth.wrapDegrees(viewYaw - avatar.bodyRot);
                    avatar.yRot = Math.clamp(headYaw, -50.0f, 50.0f);
                    avatar.bodyRot = viewYaw - avatar.yRot;
                }
            }
            if (state instanceof ItemEntityRenderState item && !item.item.isEmpty()
                    && state.nameTag == null && state.passengerOffset == null && !state.displayFireAnimation
                    && entities.getRenderer(entity).getClass() == ItemEntityRenderer.class) {
                // Factor vanilla bob/spin out of the vertices so equal drops can share a mesh.
                double minY = item.item.getModelBoundingBox().minY;
                float bob = Mth.sin(item.ageInTicks / 10.0f + item.bobOffset) * 0.1f + 0.1f;
                position = position.add(0, bob + 0.0625f - (float) minY, 0);
                yaw = ItemEntity.getSpin(item.ageInTicks, item.bobOffset);
                ItemEntityRenderer.submitMultipleFromCount(pose, submits, item.lightCoords, item, itemRandom);
                entityKey = entity.getUUID();
                interpolate = true;
            } else {
                entities.submit(state, cameraState, 0, 0, 0, pose, submits);
                if (trackPose) {
                    entityKey = entity.getUUID();
                }
            }
            var object = entityKey == null ? null : new ObjectKey(entityKey, 0, interpolate);
            stream.add(geometry.capture(client, context, submits, stream.remaining, skin), position, yaw, object);
        } catch (RuntimeException failure) {
            logFailure(entity.getType(), failure);
        }
    }

    private void captureBlockEntity(Minecraft client, CameraRenderState cameraState, DeltaTracker delta,
                                    FeatureFrameContext context, BlockEntity block, boolean trackPose) {
        try {
            var blocks = client.getBlockEntityRenderDispatcher();
            var renderer = blocks.getRenderer(block);
            if (renderer == null) {
                return;
            }
            var state = blocks.tryExtractRenderState(block, delta.getGameTimeDeltaPartialTick(false), null,
                    renderer.shouldRenderOffScreen());
            if (state != null) {
                var submits = new SubmitNodeStorage();
                blocks.submit(state, new PoseStack(), submits, cameraState);
                var object = trackPose ? new ObjectKey(null, block.getBlockPos().asLong(), false) : null;
                int emission = 0;
                if (textureEmission && block instanceof net.minecraft.world.level.block.entity.EnderChestBlockEntity) emission = 1;
                if (textureEmission && block instanceof net.minecraft.world.level.block.entity.ConduitBlockEntity conduit) emission = conduit.isActive() ? 2 : 3;
                world.add(geometry.captureBlock(client, context, submits, world.remaining, emission), Vec3.atLowerCornerOf(block.getBlockPos()),
                        0, object);
            }
        } catch (RuntimeException failure) {
            logFailure(block.getType(), failure);
        }
    }

    private void logFailure(Object type, RuntimeException failure) {
        if (loggedFailures.add(type)) {
            LOGGER.warn("Could not export Minecraft UE dynamic renderer {}", type, failure);
        }
    }

    private record WorldObject(double distance, Entity entity, BlockEntity block) {}

    private static final class Stream {
        private final int space;
        private final int vertexBudget;
        private final long intervalNs;
        private Map<PartKey, Integer> previousParts = new HashMap<>();
        private Map<PartKey, Integer> parts = new HashMap<>();
        private Map<DynamicGeometry, Integer> previousMeshes = new HashMap<>();
        private List<Instance> previousInstances = List.of();
        private float previousFov = -1;
        private Map<DynamicGeometry, Integer> meshes = new HashMap<>();
        private List<Instance> instances = new ArrayList<>();
        private long nextCapture;
        private int nextId;
        private int nextPartId;
        private int posedPartCount;
        private int remaining;
        private boolean wasVisible;

        private Stream(int space, int vertexBudget, int fps) {
            this.space = space;
            this.vertexBudget = vertexBudget;
            intervalNs = 1_000_000_000L / fps;
        }

        void reset() {
            previousMeshes.clear();
            previousParts.clear();
            parts.clear();
            previousInstances = List.of();
            previousFov = -1;
            meshes.clear();
            instances.clear();
            nextCapture = 0;
            nextId = 0;
            nextPartId = 0;
            wasVisible = false;
        }

        boolean due(LocalTransport.Connection connection, long now, boolean force) {
            return (force || now - nextCapture >= 0) && connection.canQueueDynamic(space);
        }

        void begin(long now) {
            long scheduled = nextCapture + intervalNs;
            nextCapture = scheduled > now && nextCapture <= now ? scheduled : now + intervalNs;
            remaining = vertexBudget;
            meshes.clear();
            parts.clear();
            posedPartCount = 0;
            instances.clear();
        }

        boolean hasRoom() {
            return remaining >= 3 && instances.size() < MAX_DYNAMIC_INSTANCES;
        }

        void add(List<DynamicGeometry> parts, Vec3 position, float yaw) {
            add(parts, position, yaw, null);
        }

        void add(List<DynamicGeometry> geometryParts, Vec3 position, float yaw, ObjectKey object) {
            if (!(Math.abs(position.x) <= 32_000_000 && Math.abs(position.y) <= 32_000_000
                    && Math.abs(position.z) <= 32_000_000 && Float.isFinite(yaw))) {
                return;
            }
            float sin = (float) Math.sin(yaw * 0.5);
            float cos = (float) Math.cos(yaw * 0.5);
            boolean trackObject = object != null && (object.interpolate || posedPartCount + geometryParts.size() <= MAX_POSED_PARTS);
            Map<PartLayer, Integer> slots = trackObject ? new HashMap<>() : null;
            for (DynamicGeometry part : geometryParts) {
                if (!hasRoom() || part.vertexCount() > remaining) {
                    break;
                }
                int id = meshes.computeIfAbsent(part, mesh -> {
                    Integer old = previousMeshes.get(mesh);
                    return old == null ? ++nextId : old;
                });
                int partId = 0;
                if (slots != null) {
                    var layer = new PartLayer(part.texture(), part.blend(), part.topology());
                    int ordinal = slots.merge(layer, 1, Integer::sum);
                    var key = new PartKey(object, layer, ordinal);
                    Integer existing = previousParts.get(key);
                    partId = existing == null ? ++nextPartId : existing;
                    if (!object.interpolate) {
                        partId |= Integer.MIN_VALUE;
                        ++posedPartCount;
                    }
                    parts.put(key, partId);
                }
                instances.add(new Instance(id, partId, position, sin, cos));
                remaining -= part.vertexCount();
            }
        }

        void publish(LocalTransport.Connection connection, int scene, float verticalFov) {
            if (space == DYNAMIC_HAND && !(verticalFov > 0.1f && verticalFov < 179)) {
                verticalFov = 70;
            }
            if (verticalFov == previousFov && instances.equals(previousInstances)) {
                return;
            }
            List<ByteBuffer> messages = new ArrayList<>();
            for (var entry : meshes.entrySet()) {
                if (!previousMeshes.containsKey(entry.getKey())) {
                    messages.add(entry.getKey().encode(scene, space, entry.getValue()));
                }
            }
            ByteBuffer snapshot = frame(INSTANCE_BATCH, DYNAMIC_BATCH_HEADER_BYTES + instances.size() * DYNAMIC_INSTANCE_BYTES)
                    .putInt(scene).put((byte) space).putFloat(verticalFov).putShort((short) instances.size());
            for (Instance instance : instances) {
                Vec3 p = instance.position;
                snapshot.putInt(instance.meshId).putInt(instance.partId).putDouble(p.x).putDouble(p.y).putDouble(p.z)
                        .putFloat(0).putFloat(instance.sin).putFloat(0).putFloat(instance.cos).putFloat(1);
            }
            messages.add(snapshot.flip());
            if (connection.sendDynamic(space, messages)) {
                var previous = previousMeshes;
                previousMeshes = meshes;
                meshes = previous;
                meshes.clear();
                var oldParts = previousParts;
                previousParts = parts;
                parts = oldParts;
                parts.clear();
                previousInstances = List.copyOf(instances);
                previousFov = verticalFov;
            }
        }
    }

    private record ObjectKey(UUID entity, long block, boolean interpolate) {}
    private record PartLayer(int texture, int blend, int topology) {}
    private record PartKey(ObjectKey object, PartLayer layer, int ordinal) {}
    private record Instance(int meshId, int partId, Vec3 position, float sin, float cos) {}
}
