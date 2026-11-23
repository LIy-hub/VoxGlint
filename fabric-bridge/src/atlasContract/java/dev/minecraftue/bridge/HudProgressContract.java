package dev.minecraftue.bridge;

final class HudProgressContract {
    private static final long FRAME = 100_000_000L;

    public static void main(String[] args) {
        var watchdog = new GpuUiCapture.ProgressWatchdog();
        for (long frame = 0; frame < 20; frame++) {
            check(!watchdog.waiting(frame * FRAME), "continuous wait shorter than two seconds");
        }
        check(watchdog.waiting(20 * FRAME), "continuous stall falls back after two seconds");

        watchdog.progressed();
        check(!watchdog.waiting(21 * FRAME), "reclaimed slot resets the stall clock");
        check(!watchdog.waiting(61 * FRAME), "a four second capture pause restarts the clock");
        for (long frame = 62; frame < 81; frame++) {
            check(!watchdog.waiting(frame * FRAME), "capture pause does not add to the active wait");
        }
        check(watchdog.waiting(81 * FRAME), "genuine stall after resume still falls back");

        var retry = new GpuUiCapture.RetryPolicy();
        check(retry.ready(0L), "fresh retry policy allows immediate GPU interop");
        long backoff1 = retry.scheduleRetry(10 * FRAME);
        check(backoff1 == GpuUiCapture.RetryPolicy.INITIAL_BACKOFF_NS, "first transient stall uses initial backoff");
        check(!retry.ready(10 * FRAME + backoff1 - 1L), "GPU interop stays in cooldown before backoff expires");
        check(retry.ready(10 * FRAME + backoff1), "GPU interop retries once backoff expires");
        long backoff2 = retry.scheduleRetry(10 * FRAME + backoff1);
        check(backoff2 == backoff1 * 2L, "consecutive transient stalls double the backoff");
        retry.succeeded();
        check(!retry.hasPendingRetries() && retry.ready(0L), "reclaimed slot resets retry backoff");

        long clock = 100 * FRAME;
        for (int attempt = 0; attempt < GpuUiCapture.RetryPolicy.MAX_RETRIES; attempt++) {
            long delay = retry.scheduleRetry(clock);
            check(delay > 0L, "attempts within limit remain retryable");
            clock += delay;
        }
        check(retry.scheduleRetry(clock) == 0L && retry.exhausted() && !retry.ready(clock + 600 * FRAME),
                "exceeding retry limit permanently disables GPU interop until reconnect");
        System.out.println("HUD fence progress contract passed");
    }

    private static void check(boolean result, String message) {
        if (!result) {
            throw new AssertionError(message);
        }
    }
}
