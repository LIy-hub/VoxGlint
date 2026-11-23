package dev.minecraftue.bridge;

import com.mojang.blaze3d.vertex.VertexSorting;
import com.mojang.logging.LogUtils;
import net.minecraft.client.Minecraft;
import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.client.renderer.SectionBufferBuilderPack;
import net.minecraft.client.renderer.chunk.RenderRegionCache;
import net.minecraft.client.renderer.chunk.RenderSectionRegion;
import net.minecraft.client.renderer.chunk.SectionCompiler;
import net.minecraft.client.renderer.texture.TextureAtlas;
import net.minecraft.core.BlockPos;
import net.minecraft.core.SectionPos;
import net.minecraft.resources.Identifier;
import net.minecraft.server.level.ChunkTrackingView;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.chunk.LevelChunk;
import net.minecraft.world.level.chunk.status.ChunkStatus;
import net.minecraft.world.phys.Vec3;
import org.slf4j.Logger;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.NavigableSet;
import java.util.Set;
import java.util.TreeSet;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.FutureTask;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class SceneMeshStream {
    private static final int WORKER_THREADS = Math.clamp(Runtime.getRuntime().availableProcessors() / 2, 2, 8);
    private static final Logger LOGGER = LogUtils.getLogger();
    private static final boolean TRACE = Boolean.getBoolean("minecraftue.terrainTrace");
    private static final int BULK_WORKERS = WORKER_THREADS - 1;
    private final AtomicInteger workerIds = new AtomicInteger();
    private final ThreadPoolExecutor worker = compilerPool(BULK_WORKERS, "bulk");
    private final ThreadPoolExecutor priorityWorker = compilerPool(1, "near");
    private final List<SectionBufferBuilderPack> allocatedBuffers = new CopyOnWriteArrayList<>();
    private final ThreadLocal<SectionBufferBuilderPack> workerBuffers = ThreadLocal.withInitial(() -> {
        SectionBufferBuilderPack pack = new SectionBufferBuilderPack();
        allocatedBuffers.add(pack);
        return pack;
    });
    private final Map<ChunkPos, LevelChunk> neighborhood = new HashMap<>();
    private final Set<ChunkPos> tracked = new HashSet<>();
    private final Set<ChunkPos> replacements = new HashSet<>();
    private final NavigableSet<SectionPos> dirty = new TreeSet<>(this::compareSections);
    private final NavigableSet<SectionPos> urgentDirty = new TreeSet<>(this::compareSections);
    private final Map<SectionPos, Long> editTimes = new HashMap<>();
    private final Set<SectionPos> published = new HashSet<>();
    private final Map<SectionPos, byte[]> publishedGlass = new HashMap<>();
    private final LinkedHashMap<SectionPos, CompileJob> jobs = new LinkedHashMap<>();
    private ClientLevel level;
    private SectionCompiler compiler;
    private TerrainAtlasTransfer atlas;
    private Map<Identifier, TerrainEmission.Color> emissionColors = Map.of();
    private GlassFilterMaterials glassFilters = GlassFilterMaterials.EMPTY;
    private boolean atlasSent;
    private SectionPos focus;
    private int trackedRadius;
    private boolean notifiedReady;

    private ThreadPoolExecutor compilerPool(int threads, String name) {
        return new ThreadPoolExecutor(threads, threads, 0L, TimeUnit.NANOSECONDS, new ArrayBlockingQueue<>(threads),
                task -> Thread.ofPlatform().daemon()
                        .name("Minecraft UE Section Compiler-" + name + "-" + workerIds.incrementAndGet()).unstarted(task));
    }

    void start(Minecraft minecraft, ClientLevel level, TextureAtlas blocks, boolean stainedGlass) {
        this.level = level;
        atlas = new TerrainAtlasTransfer(blocks);
        emissionColors = atlas.emissionColors();
        glassFilters = stainedGlass ? GlassFilterMaterials.load(minecraft.getResourceManager()) : GlassFilterMaterials.EMPTY;
        atlasSent = false;
        var models = minecraft.getModelManager();
        compiler = new SectionCompiler(minecraft.options.ambientOcclusion().get(), minecraft.options.cutoutLeaves().get(),
                models.getBlockStateModelSet(), models.getFluidStateModelSet(), minecraft.getBlockColors());
    }

    void reset() {
        jobs.values().forEach(this::cancel);
        jobs.clear();
        level = null;
        compiler = null;
        emissionColors = Map.of();
        glassFilters = GlassFilterMaterials.EMPTY;
        closeAtlas();
        notifiedReady = false;
        neighborhood.clear();
        tracked.clear();
        replacements.clear();
        dirty.clear();
        urgentDirty.clear();
        editTimes.clear();
        published.clear();
        publishedGlass.clear();
        focus = null;
        trackedRadius = 0;
    }

    private void closeAtlas() {
        if (atlas != null) {
            atlas.close();
            atlas = null;
        }
    }

    void close() {
        reset();
        worker.shutdownNow();
        priorityWorker.shutdownNow();
        try {
            worker.awaitTermination(2, TimeUnit.SECONDS);
            priorityWorker.awaitTermination(2, TimeUnit.SECONDS);
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        }
        for (SectionBufferBuilderPack pack : allocatedBuffers) {
            pack.close();
        }
        allocatedBuffers.clear();
    }

    void blockChanged(ClientLevel changedLevel, BlockPos pos) {
        if (level == changedLevel) {
            SectionPos.aroundAndAtBlockPos(pos.asLong(), packed -> markDirty(SectionPos.of(packed), true));
        }
    }

    void lightChanged(ClientLevel changedLevel, SectionPos section) {
        if (level == changedLevel) {
            markDirty(section, true);
        }
    }

    void chunkReplaced(LevelChunk chunk) {
        if (level == chunk.getLevel()) {
            replacements.add(chunk.getPos());
        }
    }

    void chunkDropped(ChunkPos pos) {
        if (neighborhood.containsKey(pos)) {
            replacements.add(pos);
        }
    }

    private void markDirty(SectionPos pos, boolean fromEdit) {
        if (pos.y() < level.getMinSectionY() || pos.y() > level.getMaxSectionY() || !tracked.contains(pos.chunk())) {
            return;
        }
        LevelChunk chunk = neighborhood.get(pos.chunk());
        if (published.contains(pos) || jobs.containsKey(pos)
                || !chunk.getSection(chunk.getSectionIndexFromSectionY(pos.y())).hasOnlyAir()) {
            dirty.add(pos);
            CompileJob running = jobs.get(pos);
            if ((running != null && running.urgent) ||
                    (fromEdit && columnDistance(pos) <= 4 && Math.abs(pos.y() - focus.y()) <= 2)) {
                urgentDirty.add(pos);
                if (TRACE) {
                    editTimes.putIfAbsent(pos, System.nanoTime());
                }
            }
        }
    }

    void tick(LocalTransport.Connection connection, int scene, SectionPos player, Vec3 eye, int radius) {
        if (atlas != null) {
            if (!atlasSent) {
                atlasSent = atlas.flush(connection, scene);
            }
            if (!atlasSent) {
                return;
            }
            if (!connection.terrainDrained()) {
                return;
            }
            closeAtlas();
        }
        ChunkPos center = player.chunk();
        boolean moved = focus == null || !center.equals(focus.chunk());
        if (!player.equals(focus)) {
            List<SectionPos> queued = new ArrayList<>(dirty);
            List<SectionPos> urgent = new ArrayList<>(urgentDirty);
            dirty.clear();
            urgentDirty.clear();
            focus = player;
            dirty.addAll(queued);
            urgentDirty.addAll(urgent);
        }
        if (moved || radius != trackedRadius || !replacements.isEmpty()) {
            updateColumns(connection, center, radius);
            trackedRadius = radius;
        }
        collectJobs(connection, scene, true);
        collectJobs(connection, scene, false);
        if (!notifiedReady && tracked.contains(center)
                && jobs.keySet().stream().noneMatch(section -> section.chunk().equals(center))
                && (dirty.isEmpty() || !dirty.first().chunk().equals(center))) {
            notifiedReady = connection.sendTerrain(BridgeWire.frame(BridgeWire.SCENE_READY, 4).putInt(scene).flip());
        }
        if ((notifiedReady && connection.readyScene != scene) || dirty.isEmpty()) {
            return;
        }

        RenderRegionCache regions = new RenderRegionCache();
        long stopAt = System.nanoTime() + 2_000_000;
        scheduleJobs(connection, scene, center, eye, regions, true, stopAt);
        scheduleJobs(connection, scene, center, eye, regions, false, stopAt);
    }

    private void scheduleJobs(LocalTransport.Connection connection, int scene, ChunkPos center, Vec3 eye,
                              RenderRegionCache regions, boolean urgent, long stopAt) {
        if (!connection.canQueueSection(urgent)) {
            return;
        }
        int slots = urgent ? 1 : BULK_WORKERS;
        for (CompileJob job : jobs.values()) {
            if (job.urgent == urgent) {
                slots--;
            }
        }
        if (slots == 0) {
            return;
        }
        boolean coveredFaces = connection.controlShm.supports(ControlSharedMemory.CAP_COVERED_FACE_LIGHTS);
        boolean vegetationShading = connection.controlShm.supports(ControlSharedMemory.CAP_VEGETATION_SHADING);
        boolean materialLights = connection.controlShm.supports(ControlSharedMemory.CAP_MATERIAL_LIGHTS);
        boolean textureEmission = connection.controlShm.supports(ControlSharedMemory.CAP_TEXTURE_EMISSION);
        boolean mineralGlow = connection.controlShm.supports(ControlSharedMemory.CAP_MINERAL_GLOW);
        Iterator<SectionPos> candidates = (urgent ? urgentDirty : dirty).iterator();
        while (slots > 0 && candidates.hasNext() && connection.canQueueSection(urgent)) {
            SectionPos next = candidates.next();
            if (!notifiedReady && !next.chunk().equals(center)) {
                break;
            }
            if ((!urgent && urgentDirty.contains(next)) || jobs.containsKey(next)) {
                continue;
            }
            LevelChunk chunk = neighborhood.get(next.chunk());
            var chunkSection = chunk.getSection(chunk.getSectionIndexFromSectionY(next.y()));
            if (chunkSection.hasOnlyAir()) {
                var update = new SectionUpdate(SectionMeshEncoder.encode(next, Map.of(), null),
                        SectionCapture.lightFrame(scene, next, 0).flip(), SectionGlass.EMPTY);
                long token = publishSection(connection, scene, next, urgent, update);
                if (token == 0) {
                    break;
                }
                published.remove(next);
                tracePublication(scene, next, urgent, token);
            } else {
                RenderSectionRegion region = regions.createRegion(level, next.asLong());
                SectionCompiler source = compiler;
                var colors = emissionColors;
                var filters = !glassFilters.isEmpty() && chunkSection.maybeHas(glassFilters::contains)
                        ? glassFilters : GlassFilterMaterials.EMPTY;
                var task = new FutureTask<>(() -> compile(source, scene, next, region, eye, colors,
                        coveredFaces, vegetationShading, materialLights, textureEmission, mineralGlow, filters));
                try {
                    (urgent ? priorityWorker : worker).execute(task);
                } catch (RejectedExecutionException busy) {
                    // An old world can still be using a compiler; keep the latest dirty entry queued.
                    break;
                }
                jobs.put(next, new CompileJob(task, urgent));
                slots--;
                if (TRACE && urgent) {
                    LOGGER.info("V27 terrain edit scheduled scene={} section={} dirty={} jobs={} sinceDirtyMs={}",
                            scene, next, dirty.size(), jobs.size(), elapsedEditMs(next));
                }
            }
            candidates.remove();
            if (urgent) {
                dirty.remove(next);
            }
            if (System.nanoTime() >= stopAt) {
                break;
            }
        }
    }

    private void cancel(CompileJob job) {
        job.task.cancel(false);
        (job.urgent ? priorityWorker : worker).remove(job.task);
    }

    private void updateColumns(LocalTransport.Connection connection, ChunkPos center, int radius) {
        Set<ChunkPos> nextTracked = new HashSet<>();
        Set<ChunkPos> changed = new HashSet<>(replacements);
        replacements.clear();
        int border = radius + 1;
        neighborhood.keySet().removeIf(pos -> pos.getChessboardDistance(center) > border);
        for (int z = -border; z <= border; z++) {
            for (int x = -border; x <= border; x++) {
                ChunkPos pos = new ChunkPos(center.x() + x, center.z() + z);
                LevelChunk chunk = level.getChunkSource().getChunk(pos.x(), pos.z(), ChunkStatus.FULL, false);
                LevelChunk previous = chunk == null ? neighborhood.remove(pos) : neighborhood.put(pos, chunk);
                if (chunk != previous) {
                    changed.add(pos);
                }
                if (chunk != null && ChunkTrackingView.isInViewDistance(center.x(), center.z(), radius, pos.x(), pos.z())) {
                    nextTracked.add(pos);
                }
            }
        }
        List<ChunkPos> dropped = tracked.stream().filter(pos -> !nextTracked.contains(pos)).toList();
        if (!dropped.isEmpty()) {
            // Large radius changes must fit into the same bounded send queue as mesh updates.
            ByteBuffer frame = BridgeWire.frame(BridgeWire.DROP_CHUNK, dropped.size() * 8);
            for (ChunkPos pos : dropped) {
                for (int y = level.getMinSectionY(); y <= level.getMaxSectionY(); y++) {
                    SectionPos section = SectionPos.of(pos, y);
                    CompileJob job = jobs.remove(section);
                    if (job != null) {
                        cancel(job);
                    }
                    dirty.remove(section);
                    urgentDirty.remove(section);
                    editTimes.remove(section);
                    published.remove(section);
                    publishedGlass.remove(section);
                }
                frame.putInt(pos.x()).putInt(pos.z());
            }
            connection.sendTerrain(frame.flip());
        }
        Set<ChunkPos> refresh = new HashSet<>();
        for (ChunkPos pos : nextTracked) {
            if (!tracked.contains(pos)) {
                refresh.add(pos);
            }
        }
        for (ChunkPos pos : changed) {
            for (int z = -1; z <= 1; z++) {
                for (int x = -1; x <= 1; x++) {
                    ChunkPos neighbor = new ChunkPos(pos.x() + x, pos.z() + z);
                    if (nextTracked.contains(neighbor)) {
                        refresh.add(neighbor);
                    }
                }
            }
        }
        tracked.clear();
        tracked.addAll(nextTracked);
        for (ChunkPos pos : refresh) {
            for (int y = level.getMinSectionY(); y <= level.getMaxSectionY(); y++) {
                markDirty(SectionPos.of(pos, y), false);
            }
        }
    }

    private int compareSections(SectionPos left, SectionPos right) {
        int distance = Long.compare(columnDistance(left), columnDistance(right));
        if (distance == 0) {
            distance = Integer.compare(Math.abs(left.y() - focus.y()), Math.abs(right.y() - focus.y()));
        }
        return distance != 0 ? distance : Long.compare(left.asLong(), right.asLong());
    }

    private long columnDistance(SectionPos section) {
        long x = section.x() - (long) focus.x();
        long z = section.z() - (long) focus.z();
        return x * x + z * z;
    }

    private void collectJobs(LocalTransport.Connection connection, int scene, boolean urgent) {
        Iterator<Map.Entry<SectionPos, CompileJob>> it = jobs.entrySet().iterator();
        while (it.hasNext() && connection.canQueueSection(urgent)) {
            var entry = it.next();
            CompileJob job = entry.getValue();
            if (job.urgent != urgent || !job.task.isDone()) {
                continue;
            }
            SectionPos section = entry.getKey();
            if (job.task.isCancelled() || dirty.contains(section)) {
                it.remove();
                continue;
            }
            try {
                SectionUpdate update = job.task.get();
                long token = publishSection(connection, scene, section, urgent, update);
                if (token != 0) {
                    published.add(section);
                    tracePublication(scene, section, urgent, token);
                    it.remove();
                }
            } catch (ExecutionException e) {
                throw new IllegalStateException("Minecraft could not compile section " + section, e.getCause());
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                throw new IllegalStateException("Interrupted while collecting the compiled section", e);
            }
        }
    }

    private long elapsedEditMs(SectionPos pos) {
        return TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - editTimes.getOrDefault(pos, System.nanoTime()));
    }

    private long publishSection(LocalTransport.Connection connection, int scene, SectionPos section, boolean urgent,
                                SectionUpdate update) {
        byte[] previous = publishedGlass.get(section);
        byte[] digest = update.glass().digest();
        List<ByteBuffer> parts = List.of();
        if (!Arrays.equals(previous, digest)) {
            parts = digest == null ? List.of(SectionGlass.clear(scene, section)) : update.glass().parts();
        }
        long token = connection.sendSection(scene, urgent, update.mesh(), update.lights(), parts);
        if (token != 0 && !parts.isEmpty()) {
            if (TRACE) {
                LOGGER.info("SectionGlass scene={} section={} parts={} boxes={} token={}",
                        scene, section, parts.size(), update.glass().boxCount(), token);
            }
            if (digest == null) {
                publishedGlass.remove(section);
            } else {
                publishedGlass.put(section, digest);
            }
        }
        return token;
    }

    private void tracePublication(int scene, SectionPos section, boolean urgent, long token) {
        if (TRACE && urgent) {
            LOGGER.info("V27 terrain edit published scene={} token={} section={} dirty={} jobs={} sinceDirtyMs={} publishedEpochMs={}",
                    scene, token, section, dirty.size(), jobs.size(), elapsedEditMs(section), System.currentTimeMillis());
            editTimes.remove(section);
        }
    }

    private SectionUpdate compile(SectionCompiler source, int scene, SectionPos section, RenderSectionRegion region,
                                  Vec3 eye, Map<Identifier, TerrainEmission.Color> emissionColors,
                                  boolean coveredFaces, boolean vegetationShading, boolean materialLights,
                                  boolean textureEmission, boolean mineralGlow, GlassFilterMaterials filters) {
        SectionBufferBuilderPack buffers = workerBuffers.get();
        var sort = VertexSorting.byDistance((float) (eye.x - section.minBlockX()),
                (float) (eye.y - section.minBlockY()), (float) (eye.z - section.minBlockZ()));
        SectionCompiler.Results result = null;
        SectionCapture capture = SectionCapture.begin(emissionColors, vegetationShading, materialLights, textureEmission, mineralGlow);
        try {
            result = source.compile(section, region, sort, buffers);
            var mesh = SectionMeshEncoder.encode(section, result.renderedLayers, capture);
            var glass = filters.isEmpty() ? SectionGlass.EMPTY : SectionGlass.capture(scene, section, region, filters);
            int meshBytes = mesh.remaining() - 5;
            return new SectionUpdate(mesh, capture.encodeLights(scene, section, coveredFaces,
                    TerrainSharedMemory.MAX_BODY_BYTES - meshBytes), glass);
        } finally {
            SectionCapture.end();
            if (result != null) {
                result.release();
                buffers.clearAll();
            } else {
                buffers.discardAll();
            }
        }
    }

    private record CompileJob(FutureTask<SectionUpdate> task, boolean urgent) {}

    private record SectionUpdate(ByteBuffer mesh, ByteBuffer lights, SectionGlass.Snapshot glass) {}
}
