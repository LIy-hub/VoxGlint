package dev.minecraftue.bridge;

import dev.minecraftue.bridge.mixin.LightningBoltAccessor;
import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.LightningBolt;

import java.nio.ByteBuffer;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

/** Streams real Minecraft LightningBolt strikes to UE so thunderbolts and cloud illumination match gameplay. */
final class LightningStream {
    private final Map<Integer, Long> sentSeeds = new HashMap<>();

    void reset() {
        sentSeeds.clear();
    }

    void tick(LocalTransport.Connection connection, int scene, ClientLevel level) {
        Set<Integer> alive = null;
        for (Entity entity : level.entitiesForRendering()) {
            if (!(entity instanceof LightningBolt bolt) || bolt.isRemoved()) {
                continue;
            }
            int id = bolt.getId();
            if (alive == null) {
                alive = new HashSet<>();
            }
            alive.add(id);
            long seed = bolt.seed;
            Long previousSeed = sentSeeds.get(id);
            if (previousSeed != null && previousSeed == seed) {
                continue;
            }
            LightningBoltAccessor accessor = (LightningBoltAccessor) bolt;
            ByteBuffer frame = encodeStrike(scene, bolt, accessor.minecraftUe$getLife(), accessor.minecraftUe$getFlashes());
            if (connection.sendControl(frame)) {
                sentSeeds.put(id, seed);
            }
        }
        if (alive == null) {
            if (!sentSeeds.isEmpty()) {
                sentSeeds.clear();
            }
        } else {
            Set<Integer> keep = alive;
            sentSeeds.keySet().removeIf(id -> !keep.contains(id));
        }
    }

    static ByteBuffer encodeStrike(int scene, LightningBolt bolt, int life, int flashes) {
        long seed = bolt.seed;
        float remainingSeconds = Math.clamp((Math.max(0, life) + 1 + Math.max(0, flashes) * 1.5f) * 0.05f, 0.12f, 0.65f);
        return BridgeWire.frame(BridgeWire.LIGHTNING_STRIKE, BridgeWire.LIGHTNING_STRIKE_BYTES)
                .putInt(scene)
                .putInt(bolt.getId())
                .putDouble(bolt.getX())
                .putDouble(bolt.getY())
                .putDouble(bolt.getZ())
                .putInt((int) (seed ^ (seed >>> 32)))
                .putFloat(remainingSeconds)
                .flip();
    }
}
