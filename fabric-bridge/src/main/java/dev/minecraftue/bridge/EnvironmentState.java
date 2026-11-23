package dev.minecraftue.bridge;

import dev.minecraftue.bridge.mixin.ClientLevelAccessor;
import net.minecraft.client.Camera;
import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.client.renderer.state.level.CameraRenderState;
import net.minecraft.util.Mth;
import net.minecraft.world.attribute.EnvironmentAttributes;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.material.FogType;

import java.nio.ByteBuffer;

record EnvironmentState(int dimension, int medium, int moonPhase, int flags,
                        float sunAngle, float globalRain, float thunder,
                        float localRain, float localSnow, float skyFlash,
                        int skyRgb, float fogR, float fogG, float fogB,
                        float gameTimeSec) {
    private static final int OVERWORLD = 0;
    private static final int NETHER = 1;
    private static final int END = 2;
    private static final int OTHER = 3;
    private static final int WEATHER_CAPABILITY_KNOWN = 1 << 0;
    private static final int WEATHER_ALLOWED = 1 << 1;
    private static final int WEATHER_RAINING = 1 << 2;
    private static final int WEATHER_THUNDERING = 1 << 3;

    static EnvironmentState sample(ClientLevel level, Camera camera, CameraRenderState state, float partialTick) {
        int dimension = level.dimension().equals(Level.NETHER) ? NETHER : switch (level.dimensionType().skybox()) {
            case OVERWORLD -> OVERWORLD;
            case END -> END;
            case NONE -> OTHER;
        };
        int medium = state.fogType == FogType.WATER ? 1 : state.fogType == FogType.LAVA ? 2 : 0;
        var attributes = camera.attributeProbe();
        float degrees = Mth.positiveModulo(attributes.getValue(EnvironmentAttributes.SUN_ANGLE, partialTick), 360.0f);
        float sunAngle = (float) Math.toRadians(degrees);
        int moonPhase = attributes.getValue(EnvironmentAttributes.MOON_PHASE, partialTick).index() & 7;
        int sky = attributes.getValue(EnvironmentAttributes.SKY_COLOR, partialTick) & 0xFFFFFF;
        var fog = state.fogData.color;

        float globalRain = Mth.clamp(level.getRainLevel(partialTick), 0.0f, 1.0f);
        float thunder = Mth.clamp(level.getThunderLevel(partialTick), 0.0f, 1.0f);
        float localRain = 0.0f;
        float localSnow = 0.0f;
        int flags = weatherFlags(level);
        if ((flags & WEATHER_ALLOWED) != 0 && globalRain > 0.0f) {
            switch (level.getPrecipitationAt(camera.blockPosition())) {
                case RAIN -> localRain = globalRain;
                case SNOW -> localSnow = globalRain;
                case NONE -> { }
            }
        }
        int rawFlash = ((ClientLevelAccessor) level).minecraftUe$getSkyFlashTime();
        float skyFlash = rawFlash > 0 ? Mth.clamp((rawFlash - partialTick) * 0.5f, 0.0f, 1.0f) : 0.0f;
        float gameTimeSec = (float) (((level.getGameTime() % 1_728_000L) + partialTick) * 0.05);
        return new EnvironmentState(dimension, medium, moonPhase, flags,
                sunAngle, globalRain, thunder, localRain, localSnow, skyFlash,
                sky, fog.x, fog.y, fog.z, gameTimeSec);
    }

    static int weatherFlags(Level level) {
        return WEATHER_CAPABILITY_KNOWN | (level.canHaveWeather() ? WEATHER_ALLOWED : 0)
                | (level.isRaining() ? WEATHER_RAINING : 0) | (level.isThundering() ? WEATHER_THUNDERING : 0);
    }

    ByteBuffer encode(int scene) {
        return BridgeWire.frame(BridgeWire.ENVIRONMENT_STATE, BridgeWire.ENVIRONMENT_STATE_BYTES)
                .putInt(scene)
                .put((byte) dimension).put((byte) medium).put((byte) moonPhase).put((byte) flags)
                .putFloat(sunAngle).putFloat(globalRain).putFloat(thunder)
                .putFloat(localRain).putFloat(localSnow).putFloat(skyFlash)
                .putInt(skyRgb)
                .putFloat(fogR).putFloat(fogG).putFloat(fogB)
                .putFloat(gameTimeSec)
                .flip();
    }
}
