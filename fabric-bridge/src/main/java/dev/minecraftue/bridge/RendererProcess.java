package dev.minecraftue.bridge;

import com.mojang.logging.LogUtils;
import com.sun.jna.Platform;
import net.fabricmc.loader.api.FabricLoader;
import org.slf4j.Logger;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.InvalidPathException;
import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.TimeUnit;

final class RendererProcess {
    private static final Logger LOGGER = LogUtils.getLogger();
    private static final Duration STARTUP_TIMEOUT = Duration.ofMinutes(3);
    private static final String BINARY = "MinecraftUEViewer/Binaries/Win64/MinecraftUEViewer-Win64-Shipping.exe";
    static final RendererProcess INSTANCE = new RendererProcess();

    private final Path executable = findExecutable();
    private volatile boolean closed;
    private volatile Process process;

    boolean enabled() {
        return executable != null;
    }

    boolean isRunning() {
        Process child = process;
        if (child == null || child.isAlive()) {
            return child != null;
        }
        if (!closed) {
            LOGGER.info("Minecraft UE Bridge renderer PID {} exited with code {}", child.pid(), child.exitValue());
        }
        return false;
    }

    int start() {
        Path directory = null;
        Path ready = null;
        try {
            directory = Files.createTempDirectory("minecraftue-");
            ready = directory.resolve("ports");
            Process child = launch(ready);
            if (child == null) {
                return 0;
            }
            long deadline = System.nanoTime() + STARTUP_TIMEOUT.toNanos();
            while (!closed && child.isAlive() && System.nanoTime() - deadline < 0) {
                if (Files.isRegularFile(ready)) {
                    return readPort(Files.readString(ready), child.pid());
                }
                TimeUnit.MILLISECONDS.sleep(100);
            }
            if (!closed && isRunning()) {
                throw new IOException("Renderer did not start listening within " + STARTUP_TIMEOUT.toSeconds() + " seconds");
            }
        } catch (IOException | IllegalArgumentException e) {
            if (!closed) {
                LOGGER.warn("Minecraft UE Bridge could not start the packaged renderer at {}", executable, e);
                close();
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            close();
        } finally {
            if (directory != null) {
                try {
                    Files.deleteIfExists(ready);
                    Files.deleteIfExists(directory.resolve("ports.tmp"));
                    Files.deleteIfExists(directory);
                } catch (IOException e) {
                    LOGGER.debug("Could not remove renderer startup directory {}", directory, e);
                }
            }
        }
        return 0;
    }

    private synchronized Process launch(Path ready) throws IOException {
        if (closed) {
            return null;
        }
        List<String> command = new ArrayList<>(List.of(executable.toString(), "MinecraftUEViewer",
                "-windowed", "-NoSplash", "-NoLoadingScreen", "-NoCrashDialog"));
        if (BridgePresentation.REQUESTED) {
            command.add("-BridgeWaitForMinecraft");
        }
        Path instance = FabricLoader.getInstance().getGameDir().toRealPath();
        String quality = System.getProperty("minecraftue.quality");
        if (quality != null && !quality.isBlank()) {
            command.add("-BridgeQuality=" + quality);
        }
        String glassBudget = System.getProperty("minecraftue.glassRayShadowBudget");
        if (glassBudget != null) {
            int budget;
            try {
                budget = Integer.parseInt(glassBudget.strip());
            } catch (NumberFormatException invalid) {
                budget = -2;
            }
            if (budget >= -1) {
                command.add("-BridgeGlassLocalRayShadowBudget=" + budget);
            } else {
                LOGGER.warn("Ignoring minecraftue.glassRayShadowBudget={} (expected an integer >= -1)", glassBudget);
            }
        }
        BridgeSettings settings = BridgeSettings.read();
        int hudMaxFps = settings.hudMaxFps();
        if (hudMaxFps == 0) {
            hudMaxFps = BridgeSettings.hudLimit(Integer.getInteger("minecraftue.hudMaxFps", 0));
        }
        if (hudMaxFps > 0) {
            command.add("-BridgeHudMaxFps=" + hudMaxFps);
        }
        if (settings.debugPanel() == BridgeSettings.Toggle.OFF) {
            command.add("-BridgeHideDebugPanel");
        }
        ProcessBuilder builder = new ProcessBuilder(command)
                .directory(executable.getParent().toFile())
                .redirectOutput(ProcessBuilder.Redirect.DISCARD)
                .redirectError(ProcessBuilder.Redirect.DISCARD);
        builder.environment().put("MCUE_ATLAS_CACHE_ROOT", instance.resolve(TerrainAtlasCache.DIRECTORY).toString());
        builder.environment().put("MCUE_QUALITY_FILE", instance.resolve(BridgeSettings.QUALITY_FILE).toString());
        builder.environment().put("MCUE_PARENT_PID", Long.toString(ProcessHandle.current().pid()));
        builder.environment().put("MCUE_READY_FILE", ready.toString());
        Runtime.getRuntime().addShutdownHook(Thread.ofPlatform().unstarted(this::close));
        process = builder.start();
        LOGGER.info("Minecraft UE Bridge launched renderer PID {} from {}", process.pid(), executable);
        return process;
    }

    synchronized void close() {
        closed = true;
        if (process != null && process.isAlive()) {
            process.destroy();
        }
    }

    private static int readPort(String status, long pid) throws IOException {
        String[] fields = status.strip().split("\\s+");
        if (fields.length < 2 || Long.parseLong(fields[0]) != pid) {
            throw new IOException("Renderer returned an invalid startup response");
        }
        int version = Integer.parseInt(fields[1]);
        if (version != BridgeWire.VERSION) {
            throw new IOException("Renderer protocol " + version + " differs from Mod protocol " + BridgeWire.VERSION);
        }
        if (fields.length != 3) {
            throw new IOException("Renderer returned an invalid startup response");
        }
        int port = Integer.parseInt(fields[2]);
        if (port < 1 || port > 65535) {
            throw new IOException("Renderer returned an invalid loopback port");
        }
        return port;
    }

    private static Path findExecutable() {
        if (!Platform.isWindows() || !System.getProperty("minecraftue.renderer.autostart", "true").equalsIgnoreCase("true")) {
            return null;
        }
        String configured = System.getProperty("minecraftue.renderer.directory", "minecraftue-renderer");
        try {
            Path root = FabricLoader.getInstance().getGameDir().resolve(configured);
            Path candidate = root.resolve(BINARY).toAbsolutePath().normalize();
            if (Files.isRegularFile(candidate)) {
                return candidate;
            }
            LOGGER.info("Minecraft UE Bridge packaged renderer missing at {}", candidate);
        } catch (InvalidPathException e) {
            LOGGER.warn("Invalid Minecraft UE Bridge renderer directory: {}", configured, e);
        }
        return null;
    }
}
