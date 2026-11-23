package dev.minecraftue.bridge;

/** Decodes the shaderLABS LabPBR 1.3 normal, specular and porosity channels used by the bridge. */
final class LabPbr {
    static final int FLAT_NORMAL = 0xff8080ff;

    private LabPbr() {}

    static int decodeNormal(int source) {
        return decodeNormal(source, 255);
    }

    static int decodeNormal(int source, int porosity) {
        // LabPBR and UE both use tangent X to the right and tangent Y down the texture.
        double x = ((source >>> 16) & 255) * (2.0 / 255.0) - 1.0;
        double y = ((source >>> 8) & 255) * (2.0 / 255.0) - 1.0;
        return normal(x, y, Math.sqrt(Math.max(0.0, 1.0 - x * x - y * y)), porosity);
    }

    static int decodeSpecular(int source, int occlusion) {
        int inverseSmoothness = 255 - ((source >>> 16) & 255);
        int roughness = (inverseSmoothness * inverseSmoothness + 127) / 255;
        // The metallic workflow uses the albedo as the conductor tint for all LabPBR metal codes.
        int metallic = ((source >>> 8) & 255) >= 230 ? 255 : 0;
        int alpha = source >>> 24;
        int emissive = alpha == 255 ? 0 : (alpha * 255 + 127) / 254;
        return packed(roughness, metallic, emissive, occlusion);
    }

    /** LabPBR 1.3 encodes porosity in specular blue 0..64; values 65..255 encode SSS (non-porous). */
    static int decodePorosity(int specularSource) {
        int blue = specularSource & 255;
        return blue <= 64 ? (blue * 255 + 32) / 64 : 0;
    }

    static int withOcclusion(int packed, int occlusion) {
        return (packed & 0x00ff_ffff) | (occlusion << 24);
    }

    static int withPorosity(int normalPacked, int porosity) {
        return (normalPacked & 0x00ff_ffff) | ((porosity & 255) << 24);
    }

    static int packed(int roughness, int metallic, int emissive, int occlusion) {
        return ((occlusion & 255) << 24) | ((roughness & 255) << 16) | ((metallic & 255) << 8) | (emissive & 255);
    }

    static int normal(double x, double y, double z) {
        return normal(x, y, z, 255);
    }

    static int normal(double x, double y, double z, int porosity) {
        double length = Math.sqrt(x * x + y * y + z * z);
        if (length == 0.0) {
            return withPorosity(FLAT_NORMAL, porosity);
        }
        double scale = 127.5 / length;
        return packed((int) Math.round(x * scale + 127.5), (int) Math.round(y * scale + 127.5),
                (int) Math.round(z * scale + 127.5), porosity);
    }

    static int averageNormal(int a, int b, int c, int d) {
        return normal(sumChannel(a, b, c, d, 16) - 510, sumChannel(a, b, c, d, 8) - 510,
                sumChannel(a, b, c, d, 0) - 510, averageChannel(a, b, c, d, 24));
    }

    static int averagePacked(int a, int b, int c, int d) {
        return packed(averageChannel(a, b, c, d, 16), averageChannel(a, b, c, d, 8),
                averageChannel(a, b, c, d, 0), averageChannel(a, b, c, d, 24));
    }

    private static int averageChannel(int a, int b, int c, int d, int shift) {
        return (sumChannel(a, b, c, d, shift) + 2) / 4;
    }

    private static int sumChannel(int a, int b, int c, int d, int shift) {
        return ((a >>> shift) & 255) + ((b >>> shift) & 255) + ((c >>> shift) & 255) + ((d >>> shift) & 255);
    }
}
