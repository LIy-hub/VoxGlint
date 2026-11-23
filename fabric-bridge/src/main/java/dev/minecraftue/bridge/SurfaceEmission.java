package dev.minecraftue.bridge;

import net.minecraft.client.model.geom.builders.UVPair;
import net.minecraft.client.resources.model.geometry.BakedQuad;
import net.minecraft.world.phys.Vec3;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.WeakHashMap;

/** Area-weighted light emitted by the part of a sprite actually mapped onto a baked model face. */
final class SurfaceEmission {
    static final int MAX_PATCHES = 32;
    static final int PATCH_BYTES = 23;
    static final int SAMPLE_BYTES = 24; // Four atlas UVs, mode, RGB tint and effective model area.
    private static final int MAX_EDGE = 32;
    record AtlasRect(double u0, double v0, double u1, double v1) {
        boolean contains(double u, double v) { return u>=u0 && v>=v0 && u<=u1 && v<=v1; }
    }

    static final class Tile {
        final int width, height;
        final boolean compactFlame;
        boolean animated;
        int sampleMode;
        AtlasRect bounds;
        private final java.util.BitSet eligible = new java.util.BitSet();
        private final int sourceWidth, sourceHeight;
        private float[] rgb;
        // Values contain only numbers, so discarded model quads can leave this cache.
        private final Map<BakedQuad, List<Patch>> faces = Collections.synchronizedMap(new WeakHashMap<>());

        Tile(int sourceWidth, int sourceHeight) {
            this(sourceWidth, sourceHeight, false);
        }

        Tile(int sourceWidth, int sourceHeight, boolean compactFlame) {
            this.sourceWidth = sourceWidth;
            this.sourceHeight = sourceHeight;
            this.compactFlame = compactFlame;
            width = Math.min(sourceWidth, MAX_EDGE);
            height = Math.min(sourceHeight, MAX_EDGE);
        }

        void add(int x, int y, double r, double g, double b) {
            if (r + g + b == 0) return;
            if (rgb == null) rgb = new float[3 * width * height];
            int i = 3 * ((y * height / sourceHeight) * width + x * width / sourceWidth);
            rgb[i] += (float) (r / 65025.0);
            rgb[i + 1] += (float) (g / 65025.0);
            rgb[i + 2] += (float) (b / 65025.0);
        }

        void allow(int x, int y) {
            eligible.set(y * height / sourceHeight * width + x * width / sourceWidth);
        }

        void finish() {
            if (rgb == null) return;
            for (int y = 0; y < height; y++) {
                int rows = Math.ceilDiv((y + 1) * sourceHeight, height) - Math.ceilDiv(y * sourceHeight, height);
                for (int x = 0; x < width; x++) {
                    int cols = Math.ceilDiv((x + 1) * sourceWidth, width) - Math.ceilDiv(x * sourceWidth, width);
                    int i = 3 * (y * width + x);
                    for (int c = 0; c < 3; c++) rgb[i + c] /= rows * cols;
                }
            }
        }

        List<Patch> patches(BakedQuad quad) {
            return !hasEmission() ? List.of() : faces.computeIfAbsent(quad, q -> new Face(this, q).patches());
        }

        boolean hasEmission() { return rgb != null || animated && !eligible.isEmpty(); }

        @Override public boolean equals(Object other) {
            return other instanceof Tile tile && width == tile.width && height == tile.height &&
                    compactFlame == tile.compactFlame && animated == tile.animated && sampleMode == tile.sampleMode &&
                    eligible.equals(tile.eligible) && Arrays.equals(rgb, tile.rgb);
        }
        @Override public int hashCode() { return 31 * (31 * width + height) + Arrays.hashCode(rgb); }
    }

    record Sample(int uv0, int uv1, int uv2, int uv3, int mode, double area, Vec3 tint) {
        Sample tinted(double r, double g, double b) {
            return new Sample(uv0, uv1, uv2, uv3, mode, area, tint.multiply(r,g,b));
        }
        void write(ByteBuffer out) {
            double gain = Math.max(tint.x, Math.max(tint.y,tint.z));
            int rgb = TerrainEmission.toRgb(tint.x,tint.y,tint.z,0);
            out.putInt(uv0).putInt(uv1).putInt(uv2).putInt(uv3).put((byte) mode);
            out.put((byte)(rgb >> 16)).put((byte)(rgb >> 8)).put((byte)rgb).putFloat((float)(area * gain));
        }
    }

    record Patch(Vec3 center, Vec3 normal, Vec3 axis, double width, double height, Vec3 energy, Sample sample) {
        Patch(Vec3 center, Vec3 normal, Vec3 axis, double width, double height, Vec3 energy) {
            this(center, normal, axis, width, height, energy, null);
        }
        double power() { return Math.max(energy.x, Math.max(energy.y, energy.z)); }

        Patch placed(double dx, double dy, double dz, double r, double g, double b) {
            return new Patch(center.add(dx, dy, dz), normal, axis, width, height, energy.multiply(r, g, b),
                    sample == null ? null : sample.tinted(r,g,b));
        }

        void write(ByteBuffer out) {
            out.putShort((short) Math.round(center.x * 1024)).putShort((short) Math.round(center.y * 1024))
                    .putShort((short) Math.round(center.z * 1024));
            direction(out, normal);
            direction(out, axis);
            out.putShort((short) (width == 0 ? 0 : Math.max(1, Math.round(width * 1024))));
            out.putShort((short) Math.max(1, Math.round(height * 1024)));
            int color = TerrainEmission.toRgb(energy.x, energy.y, energy.z, 0);
            out.put((byte) (color >> 16)).put((byte) (color >> 8)).put((byte) color).putFloat((float) Math.max(1.e-8, power()));
        }

        private static void direction(ByteBuffer out, Vec3 dir) {
            out.put((byte) Math.round(dir.x * 127)).put((byte) Math.round(dir.y * 127)).put((byte) Math.round(dir.z * 127));
        }
    }

    static void compactFlame(List<Patch> patches) {
        Vec3 energy = Vec3.ZERO, tip = Vec3.ZERO, normal = Vec3.ZERO;
        double topWeight = 0, radius = 0;
        for (var patch : patches) {
            if (patch.width > 0.26 || patch.height > 0.26) return;
            energy = energy.add(patch.energy);
            if (patch.normal.y < 0.8) continue;
            double weight = patch.power();
            tip = tip.add(patch.center.scale(weight));
            normal = normal.add(patch.normal.scale(weight));
            topWeight += weight;
            radius = Math.max(radius, 0.5 * Math.max(patch.width, patch.height));
        }
        if (topWeight == 0) return;
        Vec3 up = normal.normalize();
        Vec3 tangent = up.cross(new Vec3(0, 0, 1)).normalize();
        if (tangent.lengthSqr() < 0.9) return;
        // An isotropic point emits into 4π sr; each diffuse patch has π sr of projected solid angle.
        var point = new Patch(tip.scale(1 / topWeight), up, tangent, 0, radius, energy.scale(0.25));
        patches.clear();
        patches.add(point);
    }

    private static final class Face {
        final Vec3[] points = new Vec3[4];
        final Vec3 normal, axis;
        final int w, h;
        final double[] samples;
        final double[] atlasU = new double[4], atlasV = new double[4];
        final boolean[] possible;
        final Tile tile;
        final List<Patch> output = new ArrayList<>(4);

        Face(Tile tile, BakedQuad quad) {
            this.tile = tile;
            var sprite = quad.materialInfo().sprite();
            double[] u = new double[4], v = new double[4];
            for (int i = 0; i < 4; i++) {
                var p = quad.position(i);
                points[i] = new Vec3(p.x(), p.y(), p.z());
                long uv = quad.packedUV(i);
                atlasU[i] = UVPair.unpackU(uv);
                atlasV[i] = UVPair.unpackV(uv);
                u[i] = (atlasU[i] - sprite.getU0()) * tile.width / (sprite.getU1() - sprite.getU0());
                v[i] = (atlasV[i] - sprite.getV0()) * tile.height / (sprite.getV1() - sprite.getV0());
            }
            Vec3 edge = points[1].subtract(points[0]), diagonal = points[2].subtract(points[0]);
            Vec3 first = edge.cross(diagonal), second = diagonal.cross(points[3].subtract(points[0]));
            Vec3 n = first.add(second).normalize();
            var facing = quad.direction();
            if (n.x * facing.getStepX() + n.y * facing.getStepY() + n.z * facing.getStepZ() < 0) n = n.scale(-1);
            normal = n;
            axis = edge.subtract(n.scale(edge.dot(n))).normalize();
            w = steps(u, v, 0, 1, 3, 2);
            h = steps(u, v, 0, 3, 1, 2);
            samples = new double[w * h * 3];
            possible = tile.animated ? new boolean[w * h] : null;
            double area1 = first.length() / (w * h), area2 = second.length() / (w * h);
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                double s = (x + 0.5) / w, t = (y + 0.5) / h;
                int sx = Math.clamp((int) Math.floor(interpolate(u, s, t)), 0, tile.width - 1);
                int sy = Math.clamp((int) Math.floor(interpolate(v, s, t)), 0, tile.height - 1);
                int src = 3 * (sy * tile.width + sx), dst = 3 * (y * w + x);
                if (possible != null) possible[y*w+x] = tile.eligible.get(sy*tile.width+sx);
                if (tile.rgb != null) for (int c = 0; c < 3; c++) samples[dst + c] = tile.rgb[src + c] * (s >= t ? area1 : area2);
            }
        }

        private static int steps(double[] u, double[] v, int a, int b, int c, int d) {
            return Math.clamp((int) Math.ceil(Math.max(Math.hypot(u[a] - u[b], v[a] - v[b]),
                    Math.hypot(u[c] - u[d], v[c] - v[d])) - 0.0001), 1, MAX_EDGE);
        }

        private static double interpolate(double[] value, double s, double t) {
            return value[0] * (1 - Math.max(s, t)) + value[2] * Math.min(s, t) +
                    (s >= t ? value[1] * (s - t) : value[3] * (t - s));
        }

        private Vec3 position(double s, double t) {
            return points[0].scale(1 - Math.max(s, t)).add(points[2].scale(Math.min(s, t)))
                    .add(s >= t ? points[1].scale(s - t) : points[3].scale(t - s));
        }

        List<Patch> patches() {
            if (normal.lengthSqr() < 0.9 || axis.lengthSqr() < 0.9) return List.of();
            if (tile.animated) {
                // Fixed regions survive a moving highlight or an initially black animation frame.
                int hw = (w+1)/2, hh = (h+1)/2;
                for (int y = 0; y < h; y += hh) for (int x = 0; x < w; x += hw)
                    split(x,y,Math.min(w,x+hw),Math.min(h,y+hh),2);
            } else split(0, 0, w, h, 0);
            return List.copyOf(output);
        }

        private void split(int x0, int y0, int x1, int y1, int depth) {
            int left = x1, right = x0, top = y1, bottom = y0, lit = 0;
            double r = 0, g = 0, b = 0, ws = 0, wt = 0, mass = 0;
            double[] lo = {1, 1, 1}, hi = {0, 0, 0};
            for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
                int i = (y * w + x) * 3;
                double power = Math.max(samples[i], Math.max(samples[i + 1], samples[i + 2]));
                if (possible != null ? !possible[y*w+x] : power <= 0) continue;
                left = Math.min(left, x); right = Math.max(right, x + 1);
                top = Math.min(top, y); bottom = Math.max(bottom, y + 1);
                lit++; r += samples[i]; g += samples[i + 1]; b += samples[i + 2];
                double weight = possible == null ? power : 1;
                mass += weight; ws += (x + 0.5) * weight; wt += (y + 0.5) * weight;
                if (power == 0) continue;
                for (int c = 0; c < 3; c++) {
                    double hue = samples[i + c] / power;
                    lo[c] = Math.min(lo[c], hue); hi[c] = Math.max(hi[c], hue);
                }
            }
            if (lit == 0) return;
            int dx = right - left, dy = bottom - top;
            boolean mixed = Math.max(hi[0] - lo[0], Math.max(hi[1] - lo[1], hi[2] - lo[2])) > 0.45;
            if (depth < 2 && (dx > 1 || dy > 1) && (mixed || lit < 0.70 * dx * dy)) {
                int mx = (left + right) / 2, my = (top + bottom) / 2;
                double cx = dx > 1 ? contrast(left, top, right, bottom, mx, true) : -1;
                double cy = dy > 1 ? contrast(left, top, right, bottom, my, false) : -1;
                if (cx > cy || (cx == cy && dx >= dy)) {
                    split(left, top, mx, bottom, depth + 1); split(mx, top, right, bottom, depth + 1);
                } else {
                    split(left, top, right, my, depth + 1); split(left, my, right, bottom, depth + 1);
                }
                return;
            }
            double centerS = ws / mass, centerT = wt / mass;
            double coverage = (double) lit / (dx * dy);
            if (lit < dx * dy) {
                // A crack can wrap around dark crust. Anchor its proxy on supported, emitting texels.
                double best = Double.POSITIVE_INFINITY;
                double chosenS = centerS, chosenT = centerT;
                for (int y = top; y < bottom; ++y) for (int x = left; x < right; ++x) {
                    int i = 3 * (y * w + x);
                    if (possible != null ? !possible[y*w+x] : samples[i] + samples[i + 1] + samples[i + 2] <= 0) continue;
                    double ds = x + 0.5 - centerS, dt = y + 0.5 - centerT;
                    double distance = ds * ds + dt * dt;
                    if (distance < best) { best = distance; chosenS = x + 0.5; chosenT = y + 0.5; }
                }
                centerS = chosenS;
                centerT = chosenT;
            }
            Vec3 center = position(centerS / w, centerT / h);
            Vec3 across = normal.cross(axis);
            double minU = Double.MAX_VALUE, maxU = -Double.MAX_VALUE, minV = minU, maxV = maxU;
            for (int x : new int[]{left, right}) for (int y : new int[]{top, bottom}) {
                Vec3 p = position((double) x / w, (double) y / h);
                minU = Math.min(minU, p.dot(axis)); maxU = Math.max(maxU, p.dot(axis));
                minV = Math.min(minV, p.dot(across)); maxV = Math.max(maxV, p.dot(across));
            }
            if (Math.max(Math.abs(center.x), Math.max(Math.abs(center.y), Math.abs(center.z))) < 31 &&
                    maxU > minU && maxV > minV && maxU - minU < 63 && maxV - minV < 63) {
                double extentScale = Math.sqrt(coverage);
                Sample sample = null;
                if (tile.animated) {
                    double x0uv = (double) left/w, x1uv = (double) right/w;
                    double y0uv = (double) top/h, y1uv = (double) bottom/h;
                    Vec3 origin = position(x0uv,y0uv), diag = position(x1uv,y1uv).subtract(origin);
                    double area = .5 * (position(x1uv,y0uv).subtract(origin).cross(diag).length()
                            + diag.cross(position(x0uv,y1uv).subtract(origin)).length());
                    sample = new Sample(uv(x0uv,y0uv),uv(x1uv,y0uv),uv(x1uv,y1uv),uv(x0uv,y1uv),
                            tile.sampleMode, area, new Vec3(1,1,1));
                }
                output.add(new Patch(center, normal, axis, (maxU - minU) * extentScale,
                        (maxV - minV) * extentScale, new Vec3(r, g, b),sample));
            }
        }

        private int uv(double s, double t) {
            int u = (int) Math.round(65535 * Math.clamp(interpolate(atlasU,s,t),0,1));
            int v = (int) Math.round(65535 * Math.clamp(interpolate(atlasV,s,t),0,1));
            return u << 16 | v;
        }

        private double contrast(int left, int top, int right, int bottom, int boundary, boolean alongX) {
            int countsA = alongX ? (boundary - left) * (bottom - top) : (boundary - top) * (right - left);
            int countsB = (right - left) * (bottom - top) - countsA;
            double[] difference = new double[3];
            for (int y = top; y < bottom; y++) for (int x = left; x < right; x++) {
                double scale = (alongX ? x : y) < boundary ? 1.0 / countsA : -1.0 / countsB;
                for (int c = 0; c < 3; c++) difference[c] += scale * samples[3 * (y * w + x) + c];
            }
            return difference[0] * difference[0] + difference[1] * difference[1] + difference[2] * difference[2];
        }
    }
}
