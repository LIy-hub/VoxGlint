package dev.minecraftue.bridge;

record TerrainReliefSettings(int strengthPercent, int thresholdTenthsPercent, int maxTiltDegrees) {
    static final int MAX_STRENGTH_PERCENT = 200;
    static final int MAX_THRESHOLD_TENTHS_PERCENT = 100;
    static final int MIN_TILT_DEGREES = 5;
    static final int MAX_TILT_DEGREES = 45;
    static final TerrainReliefSettings DEFAULT = new TerrainReliefSettings(100, 25, 25);
    static final TerrainReliefSettings DISABLED = DEFAULT.withStrength(0);

    TerrainReliefSettings withStrength(int value) {
        return new TerrainReliefSettings(value, thresholdTenthsPercent, maxTiltDegrees);
    }

    TerrainReliefSettings withThreshold(int value) {
        return new TerrainReliefSettings(strengthPercent, value, maxTiltDegrees);
    }

    TerrainReliefSettings withMaxTilt(int value) {
        return new TerrainReliefSettings(strengthPercent, thresholdTenthsPercent, value);
    }
}
