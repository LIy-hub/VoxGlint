package dev.minecraftue.bridge;

import com.mojang.blaze3d.vertex.QuadInstance;
import it.unimi.dsi.fastutil.ints.IntArrayList;
import net.minecraft.client.renderer.block.BlockQuadOutput;
import net.minecraft.client.renderer.block.BlockStateModelSet;
import net.minecraft.client.renderer.block.dispatch.BlockStateModelPart;
import net.minecraft.client.renderer.chunk.ChunkSectionLayer;
import net.minecraft.client.resources.model.geometry.BakedQuad;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.core.SectionPos;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.resources.Identifier;
import net.minecraft.tags.BlockTags;
import net.minecraft.tags.FluidTags;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.block.AbstractFurnaceBlock;
import net.minecraft.world.level.block.BambooSaplingBlock;
import net.minecraft.world.level.block.BaseRailBlock;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.DoorBlock;
import net.minecraft.world.level.block.FenceBlock;
import net.minecraft.world.level.block.FenceGateBlock;
import net.minecraft.world.level.block.GrowingPlantBlock;
import net.minecraft.world.level.block.HalfTransparentBlock;
import net.minecraft.world.level.block.HangingRootsBlock;
import net.minecraft.world.level.block.LanternBlock;
import net.minecraft.world.level.block.LeavesBlock;
import net.minecraft.world.level.block.RenderShape;
import net.minecraft.world.level.block.SporeBlossomBlock;
import net.minecraft.world.level.block.SugarCaneBlock;
import net.minecraft.world.level.block.TorchBlock;
import net.minecraft.world.level.block.TransparentBlock;
import net.minecraft.world.level.block.TrapDoorBlock;
import net.minecraft.world.level.block.VegetationBlock;
import net.minecraft.world.level.block.VineBlock;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.material.FluidState;
import net.minecraft.world.level.material.Fluids;
import net.minecraft.world.phys.shapes.BooleanOp;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;
import net.minecraft.world.phys.Vec3;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.HashMap;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Map;

/** Records the semantics of the vertices emitted by one invocation of the vanilla section compiler. */
public final class SectionCapture {
    private static final ThreadLocal<SectionCapture> ACTIVE = new ThreadLocal<>();
    private static final int UNCLASSIFIED = -2;
    private static final int NATIVE = -1;
    static final int BUCKET_MASK = BridgeWire.FACE_INDEX_MASK;
    private static final int BUCKET_BITS = 12;
    private static final int ENTRY_MASK = (1 << BUCKET_BITS) - 1;
    private static final int VEGETATION_BIT = BridgeWire.FACE_VEGETATION;
    static final int MATERIAL_LIGHT_BIT = 1 << 6; // Local capture metadata, stripped by the wire encoder.
    private static final int FACE_SOURCE = 1 << 6;
    private static final int FACE_MASK = FACE_SOURCE - 1;
    private static final int COVERED_FACE = 1 << 7;
    private static final Direction[] FACES = Direction.values();
    // Neighbor volume spanned by the renderer's 0.20-block IES light offset, in wire face order.
    private static final VoxelShape[] LIGHT_COVER_VOLUMES = {
            Shapes.box(0, 0.80, 0, 1, 1, 1), Shapes.box(0, 0, 0, 1, 0.20, 1),
            Shapes.box(0, 0, 0.80, 1, 1, 1), Shapes.box(0, 0, 0, 1, 1, 0.20),
            Shapes.box(0.80, 0, 0, 1, 1, 1), Shapes.box(0, 0, 0, 0.20, 1, 1)
    };
    private final IntArrayList[] runs = new IntArrayList[ChunkSectionLayer.values().length];
    private final IntArrayList lights = new IntArrayList();
    private final Map<Identifier, TerrainEmission.Color> emissionColors;
    private final Map<Integer, List<SurfaceEmission.Patch>> surfaceLights = new HashMap<>();
    private boolean materialLights;
    private boolean textureEmission;
    private boolean mineralGlow;
    private int emissionMode;
    private int surfaceLevel;
    private final IdentityHashMap<BlockStateModelPart, Boolean> emittingParts = new IdentityHashMap<>();
    private Boolean modelHasEmitter;
    private record LavaRun(int firstQuad, int source) {}
    @SuppressWarnings("unchecked")
    private final List<LavaRun>[] lavaRuns = new List[ChunkSectionLayer.values().length];
    private static final Identifier LAVA_STILL = Identifier.withDefaultNamespace("block/lava_still");
    private static final Identifier LAVA_FLOW = Identifier.withDefaultNamespace("block/lava_flow");
    private boolean materialEmitterFace;
    private boolean compactFlame;
    private final boolean vegetationShading;
    private int lightColorIndex = -1;
    private double lightRed;
    private double lightGreen;
    private double lightBlue;
    private BlockGetter region;
    private BlockPos pos;
    private BlockState state;
    private int modelBucket;
    private int modelCutout;
    private boolean foliage;
    private boolean vegetation;
    private boolean glass;
    private FluidState fluid;
    private final WaterCapture water = new WaterCapture();
    @SuppressWarnings("unchecked")
    private final ArrayList<WaterCapture.Run>[] waterRuns = new ArrayList[ChunkSectionLayer.values().length];

    WaterCapture water() {
        return water;
    }

    List<WaterCapture.Run> waterRuns(ChunkSectionLayer layer) {
        var layerRuns = waterRuns[layer.ordinal()];
        return layerRuns == null ? List.of() : layerRuns;
    }
    private TerrainModelFaces modelFaces;
    private BlockStateModelSet bakedModels;
    private ConnectedGlass connectedGlass;

    private SectionCapture(Map<Identifier, TerrainEmission.Color> emissionColors, boolean vegetationShading) {
        this.emissionColors = emissionColors;
        this.vegetationShading = vegetationShading;
    }

    static SectionCapture begin(Map<Identifier, TerrainEmission.Color> emissionColors) {
        return begin(emissionColors, false);
    }

    static SectionCapture begin(Map<Identifier, TerrainEmission.Color> emissionColors, boolean vegetationShading) {
        SectionCapture capture = new SectionCapture(emissionColors, vegetationShading);
        ACTIVE.set(capture);
        return capture;
    }

    static SectionCapture begin(Map<Identifier, TerrainEmission.Color> colors, boolean vegetation, boolean materialLights) {
        return begin(colors, vegetation, materialLights, materialLights);
    }

    static SectionCapture begin(Map<Identifier, TerrainEmission.Color> colors, boolean vegetation, boolean materialLights, boolean textureEmission) {
        return begin(colors, vegetation, materialLights, textureEmission, textureEmission);
    }

    static SectionCapture begin(Map<Identifier, TerrainEmission.Color> colors, boolean vegetation, boolean materialLights,
                                boolean textureEmission, boolean mineralGlow) {
        var capture = begin(colors, vegetation);
        capture.materialLights = materialLights;
        capture.textureEmission = textureEmission;
        capture.mineralGlow = textureEmission && mineralGlow;
        return capture;
    }

    static void end() {
        ACTIVE.remove();
    }

    public static SectionCapture active() {
        return ACTIVE.get();
    }

    public void useBakedModels(BlockStateModelSet models) {
        modelFaces = new TerrainModelFaces(models);
        bakedModels = models;
        connectedGlass = null;
    }

    public void putModelQuad(List<BlockStateModelPart> parts, BlockQuadOutput output,
                             float x, float y, float z, BakedQuad quad, QuadInstance instance) {
        if (glass && bakedModels != null) {
            if (connectedGlass == null) {
                connectedGlass = new ConnectedGlass(bakedModels);
            }
            quad = connectedGlass.connect(region, pos, state, quad);
            if (quad == null) {
                return;
            }
        }
        sampleLightColor(parts, quad, instance, x, y, z);
        if (foliage || quad.materialInfo().layer() == ChunkSectionLayer.CUTOUT) {
            modelFaces.put(parts, output, x, y, z, quad, instance, region, pos, state, foliage);
        } else {
            output.put(x, y, z, quad, instance);
        }
    }

    public void beginBlock(BlockGetter region, BlockPos pos, BlockState state) {
        finishLightColor();
        this.region = region;
        water.useRegion(region);
        this.pos = pos;
        this.state = state;
        materialEmitterFace = false;
        emissionMode = TextureEmission.PACKED;
        surfaceLevel = 0;
        modelHasEmitter = null;
        compactFlame = true;
        modelBucket = UNCLASSIFIED;
        modelCutout = UNCLASSIFIED;
        foliage = isFoliage(state);
        vegetation = !foliage && isVegetation(state);
        glass = BridgeSettings.connectedGlassEnabled() && ConnectedGlass.appliesTo(state);
        int emission = state.getLightEmission();
        if (emission > 0) {
            int flags = lightFaces(region, pos, state);
            if (flags == FACE_SOURCE) {
                return;
            }
            addLight(emission, flags);
            if (!emissionColors.isEmpty()) {
                lightColorIndex = lights.size() - 1;
            }
        }
    }

    private void addLight(int level, int flags) {
        int local = (pos.getX() & 15) | ((pos.getY() & 15) << 4) | ((pos.getZ() & 15) << 8);
        lights.add((local << 12) | (flags << 4) | level);
        lights.add(lightColor(state));
    }

    private boolean hasPreferredFace(BlockStateModelPart part) {
        if (preferred(part.getQuads(null))) return true;
        for (var face : FACES) if (preferred(part.getQuads(face))) return true;
        return false;
    }

    private boolean preferred(List<BakedQuad> quads) {
        for (var quad : quads) {
            var m = quad.materialInfo();
            var c = emissionColors.get(m.sprite().contents().name());
            if (m.lightEmission() > 0 || c != null && !c.generic()) return true;
        }
        return false;
    }

    private void sampleLightColor(List<BlockStateModelPart> parts, BakedQuad quad, QuadInstance instance,
                                  float modelX, float modelY, float modelZ) {
        materialEmitterFace = false;
        emissionMode = TextureEmission.PACKED;
        surfaceLevel = 0;
        if (lightColorIndex < 0 && (!materialLights || state == null || state.getLightEmission() != 0)) {
            return;
        }
        var material = quad.materialInfo();
        var color = emissionColors.get(material.sprite().contents().name());
        if (color == null) {
            return;
        }
        boolean elementLight = textureEmission && material.lightEmission() > 0;
        if (!color.authored() && state.getLightEmission() == 0 && !elementLight) {
            if (mineralGlow && color.ore()) emissionMode = TextureEmission.ORE_DIM;
            else if (textureEmission && !color.generic()) emissionMode = TextureEmission.DARK;
            return;
        }
        if (color.generic()) {
            if (!textureEmission) return;
            if (!elementLight) {
                if (modelHasEmitter == null) {
                    modelHasEmitter = parts.stream().anyMatch(p -> emittingParts.computeIfAbsent(p, this::hasPreferredFace));
                }
                if (modelHasEmitter) {
                    materialEmitterFace = true;
                    emissionMode = TextureEmission.DARK;
                    return;
                }
            }
        }
        if (textureEmission) {
            emissionMode = color.mode();
            if (!color.authored()) surfaceLevel = Math.max(state.getLightEmission(), material.lightEmission());
        }
        if (lightColorIndex < 0) {
            // Resource-pack-only emission is visual; the authoritative MC light engine stays untouched.
            if (color.tile() == null || color.tile().patches(quad).isEmpty()) return;
            int flags = lightFaces(region, pos, state);
            if (flags != 0 && (flags & (1 << quad.direction().get3DDataValue())) == 0) return;
            addLight(elementLight ? material.lightEmission() : 15, flags);
            lightColorIndex = lights.size() - 1;
        }
        // Half the cross product of the diagonals gives the area of the baked face.
        var a = quad.position0();
        var b = quad.position1();
        var c = quad.position2();
        var d = quad.position3();
        double ax = c.x() - a.x(), ay = c.y() - a.y(), az = c.z() - a.z();
        double bx = d.x() - b.x(), by = d.y() - b.y(), bz = d.z() - b.z();
        double x = ay * bz - az * by, y = az * bx - ax * bz, z = ax * by - ay * bx;
        double area = 0.5 * Math.sqrt(x * x + y * y + z * z);
        double red = 1.0, green = 1.0, blue = 1.0;
        if (material.isTinted()) {
            red = green = blue = 0.0;
            for (int i = 0; i < BakedQuad.VERTEX_COUNT; i++) {
                int tint = instance.getColor(i);
                red += TerrainEmission.linear((tint >>> 16) & 255);
                green += TerrainEmission.linear((tint >>> 8) & 255);
                blue += TerrainEmission.linear(tint & 255);
            }
            red /= BakedQuad.VERTEX_COUNT;
            green /= BakedQuad.VERTEX_COUNT;
            blue /= BakedQuad.VERTEX_COUNT;
        }
        if (materialLights && color.tile() != null) {
            materialEmitterFace = true;
            int info = lights.getInt(lightColorIndex - 1);
            var patches = surfaceLights.computeIfAbsent(lightColorIndex - 1, unused -> new ArrayList<>());
            int flags = (info >>> 4) & 255;
            if (flags == 0 || (flags & (1 << quad.direction().get3DDataValue())) != 0) {
                // Compact filaments have higher radiance per unit area than a full luminous block.
                var pattern = TerrainEmission.builtinPattern(material.sprite().contents().name());
                double gain = pattern == TerrainEmission.Pattern.LANTERN ? 6 :
                        (pattern == TerrainEmission.Pattern.TORCH || pattern == TerrainEmission.Pattern.REDSTONE_TORCH ? 40 : 1);
                double dx = modelX - (pos.getX() & 15);
                double dy = modelY - (pos.getY() & 15);
                double dz = modelZ - (pos.getZ() & 15);
                for (var patch : color.tile().patches(quad)) {
                    var placed = patch.placed(dx, dy, dz, red * gain, green * gain, blue * gain);
                    if (placed.power() > 0 || textureEmission && placed.sample() != null && placed.sample().tint().lengthSqr() > 0) {
                        compactFlame &= color.tile().compactFlame;
                        patches.add(placed);
                    }
                }
            }
        }
        lightRed += color.red() * red * area;
        lightGreen += color.green() * green * area;
        lightBlue += color.blue() * blue * area;
    }

    private void finishLightColor() {
        if (lightColorIndex >= 0) {
            var patches = surfaceLights.get(lightColorIndex - 1);
            if (compactFlame && patches != null) SurfaceEmission.compactFlame(patches);
            int color = TerrainEmission.toRgb(lightRed, lightGreen, lightBlue, lights.getInt(lightColorIndex));
            lights.set(lightColorIndex, color);
            lightColorIndex = -1;
            lightRed = lightGreen = lightBlue = 0.0;
        }
    }

    static boolean isFoliage(BlockState state) {
        return state.getBlock() instanceof LeavesBlock || state.is(BlockTags.LEAVES);
    }

    static boolean isVegetation(BlockState state) {
        Block block = state.getBlock();
        return block instanceof VegetationBlock
                || block instanceof SugarCaneBlock
                || block instanceof VineBlock
                || block instanceof GrowingPlantBlock
                || block instanceof HangingRootsBlock
                || block instanceof BambooSaplingBlock
                || block instanceof SporeBlossomBlock
                || state.is(BlockTags.FLOWERS)
                || state.is(BlockTags.CROPS);
    }

    private static boolean transparentBlock(BlockState state) {
        Block block = state.getBlock();
        return block instanceof TransparentBlock || block instanceof HalfTransparentBlock || state.is(Blocks.BEACON);
    }

    private static boolean closesFace(BlockGetter level, BlockPos pos, BlockState state, Direction face) {
        return state.getRenderShape() == RenderShape.MODEL && !transparentBlock(state)
                && (state.isSolidRender() || Block.isShapeFullBlock(state.getFaceOcclusionShape(face))
                    || state.isCollisionShapeFullBlock(level, pos));
    }

    static int lightFaces(BlockGetter level, BlockPos pos, BlockState state) {
        boolean lava = isLava(state);
        if (!lava && (state.getRenderShape() != RenderShape.MODEL || transparentBlock(state)
                || !(state.isSolidRender() || state.isCollisionShapeFullBlock(level, pos)))) {
            return 0;
        }
        Direction front = state.getBlock() instanceof AbstractFurnaceBlock
                ? state.getValue(AbstractFurnaceBlock.FACING) : null;
        int flags = FACE_SOURCE;
        BlockPos.MutableBlockPos adjacent = new BlockPos.MutableBlockPos();
        for (Direction face : FACES) {
            if (front != null && face != front) {
                continue;
            }
            adjacent.setWithOffset(pos, face);
            BlockState neighbor = level.getBlockState(adjacent);
            if (!(lava && isLava(neighbor))
                    && !closesFace(level, adjacent, neighbor, face.getOpposite())) {
                flags |= 1 << face.get3DDataValue();
            }
        }
        int exposed = flags & FACE_MASK;
        if (Integer.bitCount(exposed) == 1) {
            int face = Integer.numberOfTrailingZeros(exposed);
            adjacent.setWithOffset(pos, FACES[face]);
            BlockState cover = level.getBlockState(adjacent);
            if (cover.getRenderShape() == RenderShape.MODEL && !transparentBlock(cover)
                    && Shapes.joinIsNotEmpty(cover.getCollisionShape(level, adjacent),
                    LIGHT_COVER_VOLUMES[face], BooleanOp.AND)) {
                flags |= COVERED_FACE;
            }
        }
        return flags;
    }

    private static boolean isLava(BlockState state) {
        FluidState fluid = state.getFluidState();
        return fluid.getType().isSame(Fluids.LAVA) || fluid.is(FluidTags.LAVA);
    }

    public void setFluid(FluidState fluid) {
        this.fluid = fluid;
        emissionMode = TextureEmission.PACKED;
        materialEmitterFace = false;
        surfaceLevel = 0;
        if (textureEmission && fluid != null && fluid.getType().isSame(Fluids.LAVA)) {
            var lava = emissionColors.get(LAVA_STILL);
            if (lava != null) {
                emissionMode = lava.mode();
                if (!lava.authored()) surfaceLevel = state.getLightEmission();
            }
        }
    }

    public void selectLayer(ChunkSectionLayer layer, int firstVertex) {
        if (textureEmission) {
            boolean lava = fluid != null && fluid.getType().isSame(Fluids.LAVA);
            int owner = lava && lightColorIndex >= 0 ? lightColorIndex - 1 : -1;
            var spans = lavaRuns[layer.ordinal()];
            if (spans == null && owner >= 0) lavaRuns[layer.ordinal()] = spans = new ArrayList<>();
            if (spans != null && (spans.isEmpty() || spans.getLast().source != owner)) {
                if (!spans.isEmpty() && spans.getLast().firstQuad == firstVertex / 4) spans.removeLast();
                spans.add(new LavaRun(firstVertex/4, owner));
            }
        }
        int bucket = nativeBucket(layer);
        if (fluid != null) {
            if (fluid.is(FluidTags.WATER)) {
                bucket = BridgeWire.WATER;
                var layerRuns = waterRuns[layer.ordinal()];
                if (layerRuns == null) {
                    layerRuns = new ArrayList<>();
                    waterRuns[layer.ordinal()] = layerRuns;
                }
                var run = new WaterCapture.Run(firstVertex / 4, water.cell(pos));
                int lastRun = layerRuns.size() - 1;
                if (lastRun >= 0 && layerRuns.get(lastRun).firstQuad() == run.firstQuad()) {
                    layerRuns.set(lastRun, run);
                } else {
                    layerRuns.add(run);
                }
            }
        } else if (state != null) {
            if (foliage) {
                bucket = BridgeWire.FOLIAGE;
            } else {
                if (modelBucket == UNCLASSIFIED) {
                    modelBucket = classifyModel(region, pos, state);
                }
                // A thin model stays in its semantic bucket even if its sprite has fractional alpha.
                if (modelBucket == BridgeWire.CUTOUT_NO_SHADOW ||
                        (modelBucket == BridgeWire.CUTOUT && layer != ChunkSectionLayer.TRANSLUCENT)) {
                    bucket = modelBucket;
                } else if (layer == ChunkSectionLayer.CUTOUT) {
                    if (modelCutout == UNCLASSIFIED) {
                        modelCutout = state.getCollisionShape(region, pos).isEmpty()
                                ? BridgeWire.CUTOUT_NO_SHADOW : BridgeWire.CUTOUT;
                    }
                    bucket = modelCutout;
                }
            }
        }

        int entry = (vegetationShading && vegetation && bucket == BridgeWire.CUTOUT_NO_SHADOW)
                ? (bucket | VEGETATION_BIT) : bucket;
        if (fluid == null && materialEmitterFace) entry |= MATERIAL_LIGHT_BIT;
        entry |= emissionMode << 3 | surfaceLevel << 8;
        IntArrayList changes = runs[layer.ordinal()];
        if (changes == null) {
            changes = new IntArrayList();
            changes.add(nativeBucket(layer));
            runs[layer.ordinal()] = changes;
        }
        int lastIndex = changes.size() - 1;
        int last = changes.getInt(lastIndex);
        if ((last & ENTRY_MASK) != entry) {
            int marker = (firstVertex << BUCKET_BITS) | entry;
            if ((last >>> BUCKET_BITS) == firstVertex) {
                changes.set(lastIndex, marker);
            } else {
                changes.add(marker);
            }
        }
    }

    int lavaOwner(ChunkSectionLayer layer, int quad) {
        var runs = lavaRuns[layer.ordinal()];
        if (runs == null) return -1;
        int lo = 0, hi = runs.size();
        while (lo < hi) {
            int mid = (lo+hi) >>> 1;
            if (runs.get(mid).firstQuad <= quad) lo=mid+1; else hi=mid;
        }
        return lo == 0 ? -1 : runs.get(lo-1).source;
    }

    int addLavaFace(int owner, Vec3[] p, int[] uv, int face) {
        var color = emissionColors.get(LAVA_FLOW);
        double u=0, v=0;
        for (int coord : uv) { u += coord >>> 16; v += coord & 65535; }
        if (color == null || color.tile().bounds == null || !color.tile().bounds.contains(u/(4*65535),v/(4*65535)))
            color = emissionColors.get(LAVA_STILL);
        if (color == null) return TextureEmission.PACKED;
        int info = lights.getInt(owner);
        var list = surfaceLights.computeIfAbsent(owner, unused -> new ArrayList<>());
        int flags = (info >>> 4) & FACE_MASK;
        if (face < 6 && (flags & (1 << face)) == 0) return color.mode();
        Vec3 a=p[1].subtract(p[0]), d=p[2].subtract(p[0]), b=p[3].subtract(p[0]);
        Vec3 n1=a.cross(d), n2=d.cross(b), normal=n1.add(n2).normalize();
        double area=.5*(n1.length()+n2.length());
        if (area < 1.e-6 || normal.lengthSqr()<.9 || !color.tile().hasEmission()) return color.mode();
        Vec3 axis=a.subtract(normal.scale(a.dot(normal))).normalize();
        if (axis.lengthSqr()<.9) return color.mode();
        int local=info>>>12;
        Vec3 center=p[0].add(p[1]).add(p[2]).add(p[3]).scale(.25).subtract(local&15,(local>>>4)&15,(local>>>8)&15);
        double width=a.length();
        var sample=new SurfaceEmission.Sample(uv[0],uv[1],uv[2],uv[3],color.mode(),area,new Vec3(1,1,1));
        list.add(new SurfaceEmission.Patch(center,normal,axis,width,area/width,
                new Vec3(color.red(),color.green(),color.blue()).scale(area),sample));
        return color.mode();
    }

    byte[] bucketsFor(ChunkSectionLayer layer, int vertices, int[] counts) {
        return bucketsFor(layer,vertices,counts,null);
    }

    byte[] bucketsFor(ChunkSectionLayer layer, int vertices, int[] counts, byte[] levels) {
        byte[] buckets = new byte[vertices / 4];
        IntArrayList changes = runs[layer.ordinal()];
        if (changes == null) {
            int bucket = nativeBucket(layer);
            Arrays.fill(buckets, (byte) bucket);
            counts[bucket] += vertices;
            return buckets;
        }
        for (int i = 0; i < changes.size(); ++i) {
            int marker = changes.getInt(i);
            int start = marker >>> BUCKET_BITS;
            int end = i + 1 < changes.size() ? changes.getInt(i + 1) >>> BUCKET_BITS : vertices;
            if ((start & 3) != 0 || (end & 3) != 0 || end > vertices || start > end) {
                throw new IllegalArgumentException("Section compiler emitted a partial quad in " + layer);
            }
            int entry = marker & ENTRY_MASK;
            int bucket = entry & BUCKET_MASK;
            byte packed = (byte) entry;
            Arrays.fill(buckets, start / 4, end / 4, packed);
            if (levels != null) Arrays.fill(levels,start/4,end/4,(byte)(entry >>> 8));
            counts[bucket] += end - start;
        }
        return buckets;
    }

    ByteBuffer encodeLights(int scene, SectionPos section, boolean supportsCoveredFaces) {
        return encodeLights(scene, section, supportsCoveredFaces, BridgeWire.MAX_MATERIAL_LIGHTS_BYTES);
    }

    ByteBuffer encodeLights(int scene, SectionPos section, boolean supportsCoveredFaces, int maxBytes) {
        finishLightColor();
        int count = lights.size() / 2;
        int bytes = BridgeWire.SECTION_LIGHTS_HEADER_BYTES + count * BridgeWire.SECTION_LIGHT_BYTES;
        // Keep the v28 mapping size even when an exceptionally large mesh uses the reserved large lane.
        var included = new java.util.BitSet(count);
        var sampled = new HashMap<Integer, Integer>();
        for (int i = 0; i < lights.size(); i += 2) {
            var patches = surfaceLights.get(i);
            if (patches == null) continue;
            if (patches.size() > SurfaceEmission.MAX_PATCHES) {
                patches.sort(Comparator.comparingDouble(SurfaceEmission.Patch::power).reversed());
            }
            int extra = 1 + Math.min(patches.size(), SurfaceEmission.MAX_PATCHES) * SurfaceEmission.PATCH_BYTES;
            int bits = 0;
            if (textureEmission) {
                for (int j = 0; j < Math.min(patches.size(), SurfaceEmission.MAX_PATCHES); j++)
                    if (patches.get(j).sample() != null) bits |= 1 << j;
            }
            int liveBytes = bits == 0 ? 0 : Integer.BYTES + Integer.bitCount(bits) * SurfaceEmission.SAMPLE_BYTES;
            if (bits != 0 && extra + liveBytes <= maxBytes - bytes) {
                sampled.put(i,bits);
                extra += liveBytes;
            }
            if (extra <= maxBytes - bytes) {
                included.set(i / 2);
                bytes += extra;
            }
        }
        ByteBuffer frame = BridgeWire.frame(BridgeWire.SECTION_LIGHTS, bytes)
                .putInt(scene).putInt(section.x()).putInt(section.y()).putInt(section.z()).putShort((short) count);
        for (int i = 0; i < lights.size(); i += 2) {
            int locationAndLevel = lights.getInt(i);
            int color = lights.getInt(i + 1);
            frame.putShort((short) (locationAndLevel >>> 12));
            frame.put((byte) (color >>> 16)).put((byte) (color >>> 8)).put((byte) color);
            boolean spatial = included.get(i / 2);
            int bits = sampled.getOrDefault(i,0);
            frame.put((byte) ((locationAndLevel & 15) | (spatial ? 0x80 : 0) | (bits != 0 ? 0x40 : 0)));
            int flags = (locationAndLevel >>> 4) & 0xff;
            frame.put((byte) (supportsCoveredFaces ? flags : flags & ~COVERED_FACE));
            if (spatial) {
                var patches = surfaceLights.get(i);
                int n = Math.min(patches.size(), SurfaceEmission.MAX_PATCHES);
                frame.put((byte) n);
                if (bits != 0) frame.putInt(bits);
                for (int j = 0; j < n; ++j) {
                    var p = patches.get(j);
                    p.write(frame);
                    if ((bits & (1 << j)) != 0) p.sample().write(frame);
                }
            }
        }
        return frame.flip();
    }

    static ByteBuffer lightFrame(int scene, SectionPos section, int count) {
        return BridgeWire.frame(BridgeWire.SECTION_LIGHTS,
                        BridgeWire.SECTION_LIGHTS_HEADER_BYTES + count * BridgeWire.SECTION_LIGHT_BYTES)
                .putInt(scene).putInt(section.x()).putInt(section.y()).putInt(section.z()).putShort((short) count);
    }

    private static int classifyModel(BlockGetter region, BlockPos pos, BlockState state) {
        if (state.isSolidRender()) {
            return NATIVE;
        }
        var block = state.getBlock();
        if (state.is(Blocks.LADDER) || block instanceof FenceBlock || block instanceof FenceGateBlock
                || block instanceof DoorBlock || block instanceof TrapDoorBlock
                || block instanceof TorchBlock || block instanceof LanternBlock) {
            return BridgeWire.CUTOUT;
        }
        if (state.is(Blocks.REDSTONE_WIRE) || state.is(Blocks.LILY_PAD)
                || block instanceof BaseRailBlock || state.is(BlockTags.RAILS) || state.is(BlockTags.FLOWERS)
                || (block instanceof VegetationBlock && state.getCollisionShape(region, pos).isEmpty())) {
            return BridgeWire.CUTOUT_NO_SHADOW;
        }
        return NATIVE;
    }

    private static int nativeBucket(ChunkSectionLayer layer) {
        return switch (layer) {
            case SOLID -> BridgeWire.SOLID;
            case CUTOUT -> BridgeWire.CUTOUT;
            case TRANSLUCENT -> BridgeWire.TRANSLUCENT;
        };
    }

    private static int lightColor(BlockState state) {
        var id = BuiltInRegistries.BLOCK.getKey(state.getBlock());
        if (!id.getNamespace().equals("minecraft")) {
            return 0xFFF0DC;
        }
        return switch (id.getPath()) {
            case "soul_torch", "soul_wall_torch", "soul_lantern", "soul_campfire", "soul_fire" -> 0x63DFFF;
            case "sea_lantern", "end_rod", "beacon", "pearlescent_froglight" -> 0xD9F4FF;
            case "crying_obsidian", "respawn_anchor" -> 0x9315FF;
            case "amethyst_cluster", "large_amethyst_bud", "medium_amethyst_bud", "small_amethyst_bud" -> 0xB58CFF;
            case "redstone_torch", "redstone_wall_torch", "redstone_ore", "deepslate_redstone_ore" -> 0xFF4433;
            case "lava", "magma_block", "fire", "campfire", "furnace", "blast_furnace", "smoker" -> 0xFF692D;
            case "copper_torch", "copper_wall_torch", "copper_lantern", "exposed_copper_lantern",
                 "weathered_copper_lantern", "oxidized_copper_lantern", "waxed_copper_lantern",
                 "waxed_exposed_copper_lantern", "waxed_weathered_copper_lantern", "waxed_oxidized_copper_lantern" -> 0x8DFFAC;
            default -> 0xFFC474;
        };
    }
}
