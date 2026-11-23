package dev.minecraftue.bridge;

import net.minecraft.SharedConstants;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.core.registries.Registries;
import net.minecraft.gametest.framework.GameTestServer;
import net.minecraft.nbt.CompoundTag;
import net.minecraft.network.protocol.Packet;
import net.minecraft.network.protocol.game.ClientGamePacketListener;
import net.minecraft.network.protocol.game.ClientboundAddEntityPacket;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.Bootstrap;
import net.minecraft.server.commands.SummonCommand;
import net.minecraft.server.level.ServerEntity;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.server.packs.repository.ServerPacksSource;
import net.minecraft.util.Util;
import net.minecraft.world.entity.EntityTypes;
import net.minecraft.world.entity.LightningBolt;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.storage.LevelStorageSource;
import net.minecraft.world.phys.Vec3;

import java.lang.reflect.Method;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.Optional;
import java.util.function.Predicate;

final class NativeWeatherWorldContract {
    public static void main(String[] args) throws Exception {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();
        try {
            verify(Files.createDirectories(Path.of(args[0])));
        } finally {
            Util.shutdownExecutors();
        }
    }

    private static void verify(Path output) throws Exception {
        Path saves = Files.createTempDirectory(output, "isolated-native-world-");
        var access = LevelStorageSource.createDefault(saves).createAccess("weather-test");
        var packs = ServerPacksSource.createPackRepository(access);
        try (var server = GameTestServer.create(Thread.currentThread(), access, packs, Optional.empty(), false, 1)) {
            var init = GameTestServer.class.getDeclaredMethod("initServer");
            init.setAccessible(true);
            check((boolean) init.invoke(server), "GameTest world initialization");
            var world = server.overworld();
            var tickWeather = ServerLevel.class.getDeclaredMethod("advanceWeatherCycle");
            tickWeather.setAccessible(true);

            server.setWeatherParameters(6000, 0, false, false);
            ticks(tickWeather, world, 120);
            weather(world, "clear", 3);
            server.setWeatherParameters(0, 6000, true, false);
            ticks(tickWeather, world, 10);
            weather(world, "ramping-in", 3);
            check(world.getRainLevel(1) > 0, "Rain visually ramps before gameplay condition");
            ticks(tickWeather, world, 110);
            weather(world, "rain", 7);
            server.setWeatherParameters(0, 6000, true, true);
            ticks(tickWeather, world, 60);
            weather(world, "thunder-ramping-in", 7);
            check(world.getThunderLevel(1) > 0, "Thunder visually ramps before gameplay condition");
            ticks(tickWeather, world, 60);
            weather(world, "thunder", 15);

            final int scene = 29;
            final double struckX = -4.125;
            final double struckY = 76.5;
            final double struckZ = 5.875;
            var type = BuiltInRegistries.ENTITY_TYPE.getOrThrow(
                    ResourceKey.create(Registries.ENTITY_TYPE, Identifier.withDefaultNamespace("lightning_bolt")));
            var spawned = SummonCommand.createEntity(server.createCommandSourceStack(), type,
                    new Vec3(struckX, struckY, struckZ), new CompoundTag(), true);
            if (!(spawned instanceof LightningBolt serverBolt)) {
                throw new AssertionError("Server command implementation must create a LightningBolt entity");
            }
            var tracking = new ServerEntity(world, serverBolt, 1, false, new ServerEntity.Synchronizer() {
                @Override public void sendToTrackingPlayers(Packet<? super ClientGamePacketListener> packet) {}
                @Override public void sendToTrackingPlayersAndSelf(Packet<? super ClientGamePacketListener> packet) {}
                @Override public void sendToTrackingPlayersFiltered(Packet<? super ClientGamePacketListener> packet,
                        Predicate<ServerPlayer> filter) {}
            });
            var packet = (ClientboundAddEntityPacket) serverBolt.getAddEntityPacket(tracking);
            check(packet.getType() == EntityTypes.LIGHTNING_BOLT, "Server's AddEntity packet has the lightning type");
            var copiedBolt = new LightningBolt(EntityTypes.LIGHTNING_BOLT, world);
            copiedBolt.recreateFromPacket(packet);
            var life = LightningBolt.class.getDeclaredField("life");
            var flashes = LightningBolt.class.getDeclaredField("flashes");
            life.setAccessible(true);
            flashes.setAccessible(true);
            var frame = LightningStream.encodeStrike(scene, copiedBolt, life.getInt(copiedBolt), flashes.getInt(copiedBolt));
            var bytes = frame.duplicate();
            check(bytes.getInt() == 41 && bytes.get() == 36 && bytes.getInt() == scene, "v28 lightning type, size and scene");
            check(bytes.getInt() == serverBolt.getId(), "Server entity ID survives vanilla + bridge transport");
            check(bytes.getDouble() == struckX && bytes.getDouble() == struckY && bytes.getDouble() == struckZ,
                    "Exact negative and fractional coordinates survive vanilla + bridge transport");
            check(bytes.getInt() == (int) (copiedBolt.seed ^ (copiedBolt.seed >>> 32)), "Renderer seed uses the receiving entity");
            float remaining = bytes.getFloat();
            check(remaining >= 0.12f && remaining <= 0.65f && !bytes.hasRemaining(), "Finite duration and exact frame size");
            Files.write(output.resolve("lightning-strike-v28.bin"), frame.array());
            System.out.printf("lightning scene=%d id=%d coords=(%.3f,%.3f,%.3f) remaining=%.3f bytes=%d%n",
                    scene, serverBolt.getId(), struckX, struckY, struckZ, remaining, frame.remaining());

            var nether = server.getLevel(Level.NETHER);
            var end = server.getLevel(Level.END);
            check(nether != null && end != null, "World test also initialized the other vanilla dimensions");
            for (var dim : List.of(nether, end)) {
                dim.setRainLevel(1.0f);
                dim.setThunderLevel(1.0f);
                weather(dim, "raw-storm-without-weather", 1);
            }
            server.setWeatherParameters(6000, 0, false, false);
            ticks(tickWeather, world, 120);
            weather(world, "clear-after-thunder", 3);
            System.out.println("Native world weather and lightning probe passed.");
        }
    }

    private static void weather(ServerLevel level, String label, int expected) {
        int flags = EnvironmentState.weatherFlags(level);
        check(flags == expected, "Unexpected flags " + flags + " for " + label + ", expected " + expected);
        System.out.printf("%s %s rain=%.3f thunder=%.3f flags=0x%02x%n", level.dimension().identifier(), label,
                level.getRainLevel(1), level.getThunderLevel(1), flags);
    }

    private static void ticks(Method method, ServerLevel world, int count) throws Exception {
        for (int tick = 0; tick < count; ++tick) method.invoke(world);
    }

    private static void check(boolean condition, String description) {
        if (!condition) throw new AssertionError(description);
    }
}
