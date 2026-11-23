package dev.minecraftue.bridge;

import com.mojang.blaze3d.pipeline.RenderPipeline;
import com.mojang.blaze3d.platform.BlendFactor;
import com.mojang.blaze3d.platform.BlendOp;
import com.mojang.blaze3d.platform.CompareOp;
import com.mojang.blaze3d.vertex.VertexConsumer;
import com.mojang.logging.LogUtils;
import dev.minecraftue.bridge.mixin.DynamicRenderSetupAccessor;
import dev.minecraftue.bridge.mixin.DynamicRenderTypeAccessor;
import dev.minecraftue.bridge.mixin.DynamicTextureBindingAccessor;
import net.minecraft.client.Minecraft;
import net.minecraft.client.particle.SingleQuadParticle;
import net.minecraft.client.renderer.RenderPipelines;
import net.minecraft.client.renderer.SubmitNodeStorage;
import net.minecraft.client.renderer.feature.BlockModelFeatureRenderer;
import net.minecraft.client.renderer.feature.CustomFeatureRenderer;
import net.minecraft.client.renderer.feature.FeatureFrameContext;
import net.minecraft.client.renderer.feature.FeatureRendererMap;
import net.minecraft.client.renderer.feature.FeatureRendererType;
import net.minecraft.client.renderer.feature.FlameFeatureRenderer;
import net.minecraft.client.renderer.feature.ItemFeatureRenderer;
import net.minecraft.client.renderer.feature.LeashFeatureRenderer;
import net.minecraft.client.renderer.feature.ModelFeatureRenderer;
import net.minecraft.client.renderer.feature.MovingBlockFeatureRenderer;
import net.minecraft.client.renderer.feature.NameTagFeatureRenderer;
import net.minecraft.client.renderer.feature.QuadParticleFeatureRenderer;
import net.minecraft.client.renderer.feature.TextFeatureRenderer;
import net.minecraft.client.renderer.feature.phase.FeatureRenderPhase;
import net.minecraft.client.renderer.feature.submit.SubmitNode;
import net.minecraft.client.renderer.rendertype.RenderType;
import net.minecraft.resources.Identifier;
import org.slf4j.Logger;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.HashSet;
import java.util.IdentityHashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

import static dev.minecraftue.bridge.BridgeWire.*;

/** Runs the vanilla feature builders with a CPU vertex consumer in place of the GPU staging buffer. */
public final class DynamicMeshCapture implements FeatureRenderPhase.Output {
    private static final Logger LOGGER = LogUtils.getLogger();
    private static final ThreadLocal<DynamicMeshCapture> ACTIVE = new ThreadLocal<>();
    private static final float MAX_LOCAL_POSITION = 128.0f;
    private static final float MAX_UV = 1024.0f;
    private final FeatureRendererMap features = new FeatureRendererMap();
    private final Map<RenderType, VertexSink> combined = new IdentityHashMap<>();
    private final Map<SingleQuadParticle.Layer, VertexSink> particleLayers = new LinkedHashMap<>();
    private final Set<Object> warnedParticlePipelines = new HashSet<>();
    private final List<VertexSink> draws = new ArrayList<>();
    private final VertexSink discard = new VertexSink(-1, BLEND_MASKED, 0);
    private final DynamicTextureStream textures;
    private Minecraft minecraft;
    private FeatureFrameContext context;
    private RenderType depthWritingSkin;
    private boolean particleCapture;
    private int blockEmission;
    private int remaining;

    DynamicMeshCapture(DynamicTextureStream textures) {
        this.textures = textures;
        features.put(ModelFeatureRenderer.TYPE, new ModelFeatureRenderer());
        features.put(ItemFeatureRenderer.TYPE, new ItemFeatureRenderer());
        features.put(BlockModelFeatureRenderer.TYPE, new BlockModelFeatureRenderer());
        features.put(MovingBlockFeatureRenderer.TYPE, new MovingBlockFeatureRenderer());
        features.put(CustomFeatureRenderer.TYPE, new CustomFeatureRenderer());
        features.put(FlameFeatureRenderer.TYPE, new FlameFeatureRenderer());
        features.put(LeashFeatureRenderer.TYPE, new LeashFeatureRenderer());
        features.put(TextFeatureRenderer.TYPE, new TextFeatureRenderer());
        features.put(NameTagFeatureRenderer.TYPE, new NameTagFeatureRenderer());
    }

    List<DynamicGeometry> capture(Minecraft client, FeatureFrameContext frame, SubmitNodeStorage submits, int budget,
                                  RenderType depthWritingSkin) {
        return capture(client, frame, submits, budget, depthWritingSkin, false);
    }

    List<DynamicGeometry> captureParticles(Minecraft client, FeatureFrameContext frame, SubmitNodeStorage submits,
                                            int budget) {
        return capture(client, frame, submits, budget, null, true);
    }

    List<DynamicGeometry> captureBlock(Minecraft client, FeatureFrameContext frame, SubmitNodeStorage submits,
                                       int budget, int emission) {
        blockEmission = emission;
        try {
            return capture(client, frame, submits, budget, null);
        } finally {
            blockEmission = 0;
        }
    }

    private List<DynamicGeometry> capture(Minecraft client, FeatureFrameContext frame, SubmitNodeStorage submits,
                                           int budget, RenderType depthWritingSkin, boolean particleCapture) {
        minecraft = client;
        context = frame;
        remaining = budget;
        this.depthWritingSkin = depthWritingSkin;
        this.particleCapture = particleCapture;
        ACTIVE.set(this);
        try {
            for (var ordered : submits.getSubmitsPerOrder().values()) {
                ordered.solid.sortInto(this);
                ordered.afterTerrain.sortInto(this);
                ordered.translucentBlocksAndItems.sortInto(this);
                ordered.translucentModels.sortInto(this);
                ordered.translucentCustomGeometry.sortInto(this);
                ordered.texts.sortInto(this);
                ordered.nameTags.sortInto(this);
                if (particleCapture) {
                    ordered.seeThroughNameTags.sortInto(this);
                    ordered.alwaysOnTop.sortInto(this);
                }
            }
            List<DynamicGeometry> result = new ArrayList<>(draws.size());
            for (VertexSink draw : draws) {
                draw.finish(result);
            }
            return result;
        } finally {
            ACTIVE.remove();
            combined.clear();
            particleLayers.clear();
            draws.clear();
            minecraft = null;
            context = null;
            this.depthWritingSkin = null;
            this.particleCapture = false;
        }
    }

    @Override
    public void accept(SubmitNode submit, boolean translucent) {
        build(submit.featureType(), List.of(submit));
    }

    @Override
    public <S extends SubmitNode> void acceptFeatureGroup(FeatureRendererType<S> type, Collection<S> submits,
                                                           boolean translucent) {
        build(type, submits);
    }

    private void build(FeatureRendererType<?> type, Collection<? extends SubmitNode> submits) {
        if (remaining == 0) {
            return;
        }
        if (particleCapture && type == QuadParticleFeatureRenderer.TYPE) {
            for (SubmitNode submit : submits) {
                captureParticleLayers((QuadParticleFeatureRenderer.Submit) submit);
            }
        } else if (features.get(type) instanceof FeatureBuilder builder) {
            builder.minecraftUe$buildDynamicGeometry(context, List.copyOf(submits));
        } else if (particleCapture) {
            warnParticle(type, "No vertex capture for Minecraft particle feature {}");
        }
    }

    private void captureParticleLayers(QuadParticleFeatureRenderer.Submit submit) {
        for (var layer : submit.particles().layers()) {
            if (remaining == 0 || layer.translucent() != submit.translucent()) {
                continue;
            }
            VertexSink sink = particleLayers.computeIfAbsent(layer, key -> {
                var pipeline = key.pipeline();
                if (!pipeline.getVertexShader().equals(RenderPipelines.OPAQUE_PARTICLE.getVertexShader())
                        || !pipeline.getFragmentShader().equals(RenderPipelines.OPAQUE_PARTICLE.getFragmentShader())) {
                    warnParticle(pipeline.getLocation(), "Using UE particle shading for Minecraft shader pipeline {}");
                }
                return createSink(key.textureAtlasLocation(), particleBlend(pipeline), 4);
            });
            if (sink != discard) {
                submit.particles().buildLayer(layer, sink);
            }
        }
    }

    private int particleBlend(RenderPipeline pipeline) {
        var depth = pipeline.getDepthStencilState();
        int flags = depth == null || depth.depthTest() == CompareOp.ALWAYS_PASS ? PARTICLE_NO_DEPTH_TEST : 0;
        var target = pipeline.getColorTargetState();
        if (target == null || target.blendFunction().isEmpty()) {
            return flags | BLEND_MASKED;
        }
        var blend = target.blendFunction().get().color();
        if (blend.op() == BlendOp.ADD) {
            if (blend.destFactor() == BlendFactor.ONE) {
                if (blend.sourceFactor() == BlendFactor.ONE) {
                    return flags | BLEND_ADD;
                }
                if (blend.sourceFactor() == BlendFactor.SRC_ALPHA) {
                    return flags | BLEND_ADD_ALPHA;
                }
            } else if (blend.destFactor() == BlendFactor.ONE_MINUS_SRC_ALPHA) {
                if (blend.sourceFactor() == BlendFactor.ONE) {
                    return flags | BLEND_PREMULTIPLIED;
                }
                if (blend.sourceFactor() == BlendFactor.SRC_ALPHA) {
                    return flags | BLEND_ALPHA;
                }
            }
        }
        warnParticle(pipeline.getLocation(), "Using alpha blending for Minecraft particle pipeline {}");
        return flags | BLEND_ALPHA;
    }

    private void warnParticle(Object key, String message) {
        if (warnedParticlePipelines.size() < 64 && warnedParticlePipelines.add(key)) {
            LOGGER.debug(message, key);
        }
    }

    public static VertexConsumer activeVertices(RenderType type) {
        DynamicMeshCapture capture = ACTIVE.get();
        return capture == null ? null : capture.getVertices(type);
    }

    private VertexConsumer getVertices(RenderType type) {
        int topology = switch (type.primitiveTopology()) {
            case TRIANGLES -> 3;
            case QUADS -> 4;
            case TRIANGLE_STRIP -> 5;
            case TRIANGLE_FAN -> 6;
            default -> 0;
        };
        if (topology == 0 || type.isOutline() || remaining == 0) {
            return discard;
        }
        return topology <= 4
                ? combined.computeIfAbsent(type, key -> createSink(key, topology))
                : createSink(type, topology);
    }

    private VertexSink createSink(RenderType type, int topology) {
        var setup = ((DynamicRenderTypeAccessor) type).minecraftUe$getRenderSetup();
        Object binding = ((DynamicRenderSetupAccessor) (Object) setup).minecraftUe$getTextures().get("Sampler0");
        Identifier location = binding == null ? null : ((DynamicTextureBindingAccessor) binding).minecraftUe$getTextureLocation();
        if (ItemFeatureRenderer.ENCHANTED_GLINT_ITEM.equals(location) || ItemFeatureRenderer.ENCHANTED_GLINT_ARMOR.equals(location)) {
            return discard;
        }
        // Player skins need depth writes so that a later back face cannot cover the front of the skin.
        int blend = particleCapture ? particleBlend(type.pipeline())
                : type != depthWritingSkin && type.hasBlending() ? BLEND_ALPHA : BLEND_MASKED;
        return createSink(location, blend | (blockEmission << 1), topology);
    }

    private VertexSink createSink(Identifier location, int blend, int topology) {
        if (draws.size() >= MAX_DYNAMIC_INSTANCES) {
            return discard;
        }
        int texture = textures.use(minecraft, location);
        if (texture < 0) {
            return discard;
        }
        var sink = new VertexSink(texture, blend, topology);
        draws.add(sink);
        return sink;
    }

    /** Mixin interface for the vanilla feature renderer's protected buildGroup method. */
    public interface FeatureBuilder {
        void minecraftUe$buildDynamicGeometry(FeatureFrameContext context, List<? extends SubmitNode> submits);
    }

    private final class VertexSink implements VertexConsumer {
        private final int texture;
        private final int blend;
        private final int topology;
        private final int limit;
        private ByteBuffer bytes;
        private boolean open;
        private boolean invalid;
        private boolean invalidPrimitive;
        private int count;
        private float x, y, z, u, v;
        private int color, light;
        private byte nx, ny, nz;

        VertexSink(int texture, int blend, int topology) {
            this.texture = texture;
            this.blend = blend;
            this.topology = topology;
            limit = particleCapture && topology <= 4 ? MAX_PARTICLE_VERTICES : MAX_DYNAMIC_PART_VERTICES;
            bytes = texture < 0 ? null : ByteBuffer.allocate(16 * DYNAMIC_VERTEX_BYTES);
        }

        @Override
        public VertexConsumer addVertex(float x, float y, float z) {
            flushVertex();
            if (bytes != null && remaining > 0 && count < limit) {
                remaining--;
                open = true;
                this.x = x;
                this.y = y;
                this.z = z;
                u = v = 0;
                color = -1;
                light = 0xff;
                nx = nz = 0;
                ny = 127;
            }
            return this;
        }

        @Override
        public VertexConsumer setColor(int r, int g, int b, int a) {
            return setColor((a << 24) | (r << 16) | (g << 8) | b);
        }

        @Override
        public VertexConsumer setColor(int argb) {
            color = argb;
            return this;
        }

        @Override
        public VertexConsumer setUv(float u, float v) {
            this.u = u;
            this.v = v;
            return this;
        }

        @Override
        public VertexConsumer setUv2(int block, int sky) {
            light = (Math.clamp(sky >> 4, 0, 15) << 4) | Math.clamp(block >> 4, 0, 15);
            return this;
        }

        @Override
        public VertexConsumer setNormal(float x, float y, float z) {
            nx = packNormal(x);
            ny = packNormal(y);
            nz = packNormal(z);
            return this;
        }

        @Override
        public VertexConsumer setUv1(int u, int v) {
            return this;
        }

        @Override
        public VertexConsumer setLineWidth(float width) {
            return this;
        }

        private void flushVertex() {
            if (!open) {
                return;
            }
            open = false;
            float maxPosition = particleCapture ? MAX_PARTICLE_LOCAL_POSITION : MAX_LOCAL_POSITION;
            if (!(Math.abs(x) <= maxPosition && Math.abs(y) <= maxPosition && Math.abs(z) <= maxPosition
                    && Math.abs(u) <= MAX_UV && Math.abs(v) <= MAX_UV)) {
                if (particleCapture && topology <= 4) {
                    invalidPrimitive = true;
                } else {
                    invalid = true;
                }
            }
            if (!invalid) {
                if (!bytes.hasRemaining()) {
                    ByteBuffer grown = ByteBuffer.allocate(bytes.capacity() * 2);
                    grown.put(bytes.flip());
                    bytes = grown;
                }
                bytes.putFloat(x).putFloat(y).putFloat(z).putFloat(u).putFloat(v)
                        .put((byte) (color >>> 16)).put((byte) (color >>> 8)).put((byte) color).put((byte) (color >>> 24))
                        .put(nx).put(ny).put(nz).put((byte) light);
                count++;
                if (invalidPrimitive && count % topology == 0) {
                    count -= topology;
                    bytes.position(count * DYNAMIC_VERTEX_BYTES);
                    invalidPrimitive = false;
                }
            }
        }

        private void finish(List<DynamicGeometry> result) {
            flushVertex();
            int usable = topology <= 4 ? count - count % topology : count;
            if (invalid || usable < 3) {
                return;
            }
            int partSize = topology <= 4 ? MAX_DYNAMIC_PART_VERTICES / topology * topology : MAX_DYNAMIC_PART_VERTICES;
            bytes.flip();
            if (particleCapture && (blend & ~PARTICLE_NO_DEPTH_TEST) != BLEND_MASKED && topology <= 4
                    && usable > topology) {
                sortParticlePrimitives(usable);
            }
            for (int first = 0; first < usable; first += partSize) {
                int size = Math.min(partSize, usable - first);
                var part = bytes.slice(first * DYNAMIC_VERTEX_BYTES, size * DYNAMIC_VERTEX_BYTES).asReadOnlyBuffer();
                result.add(new DynamicGeometry(texture, blend, topology, part));
            }
        }

        private void sortParticlePrimitives(int usable) {
            int stride = topology * DYNAMIC_VERTEX_BYTES;
            long[] order = new long[usable / topology];
            for (int primitive = 0; primitive < order.length; primitive++) {
                float x = 0, y = 0, z = 0;
                for (int vertex = 0; vertex < topology; vertex++) {
                    int offset = primitive * stride + vertex * DYNAMIC_VERTEX_BYTES;
                    x += bytes.getFloat(offset);
                    y += bytes.getFloat(offset + Float.BYTES);
                    z += bytes.getFloat(offset + 2 * Float.BYTES);
                }
                // Positions are camera-relative. Sort far to near and break ties in submission order.
                int distance = Float.floatToRawIntBits(x * x + y * y + z * z);
                order[primitive] = ((long) ~distance << Integer.SIZE) | primitive;
            }
            Arrays.sort(order);
            ByteBuffer sorted = ByteBuffer.allocate(usable * DYNAMIC_VERTEX_BYTES);
            for (long primitive : order) {
                sorted.put(bytes.array(), (int) primitive * stride, stride);
            }
            bytes = sorted.flip();
        }

        private static byte packNormal(float value) {
            return (byte) Math.round(Math.clamp(value, -1.0f, 1.0f) * 127);
        }
    }
}
