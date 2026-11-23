"""Summarize frame, CPU thread and GPU pass times from a Bridge profile."""

import argparse
import csv
import math
import statistics
import subprocess
from collections import defaultdict
from pathlib import Path


THREADS = {
    "FrameTime": "整帧",
    "GameThreadTime": "Game Thread",
    "RenderThreadTime": "Render Thread",
    "RHIThreadTime": "RHI Thread",
    "GPUTime": "GPU 总耗时",
}


def number(value):
    if not value:
        return None
    try:
        parsed = float(value)
    except ValueError:
        return None
    return parsed if math.isfinite(parsed) and parsed >= 0 else None


def summarize(values):
    ordered = sorted(values)
    return (
        statistics.fmean(ordered),
        statistics.median(ordered),
        ordered[math.ceil(0.95 * len(ordered)) - 1],
        ordered[math.ceil(0.99 * len(ordered)) - 1],
    )


def load_capture(directory):
    status = (directory / "status.txt").read_text(encoding="utf-8-sig").strip()
    if status != "complete":
        raise ValueError(f"{directory} 的采集状态为 {status}")
    settings = dict(
        line.split("=", 1) for line in (directory / "settings.txt").read_text(encoding="utf-8-sig").splitlines()
        if "=" in line
    )
    with (directory / "frames.csv").open(encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        rows = [row for row in reader if (number(row.get("FrameTime")) or 0) > 0]
    if len(rows) != int(settings["frames"]) or not rows:
        raise ValueError(f"{directory} 只有 {len(rows)} 个有效帧，预期 {settings['frames']} 帧")
    return settings, rows


def print_matrix_row(name, values):
    mean, p50, p95, p99 = summarize(values)
    print(f"| {name} | {mean:.2f} | {p50:.2f} | {p95:.2f} | {p99:.2f} | {len(values)} |")


def ranked_scopes(rows, prefix):
    scopes = []
    for column in rows[0]:
        if column is None or not column.startswith(prefix):
            continue
        values = [number(row.get(column)) or 0.0 for row in rows]
        active = sum(value > 0 for value in values)
        if active:
            scopes.append((statistics.fmean(values), column[len(prefix):], active, values))
    return sorted(scopes, reverse=True)


def export_trace(directory, engine_root):
    insights = engine_root / "Engine/Binaries/Win64/UnrealInsights.exe"
    if not insights.is_file():
        raise ValueError(f"找不到 Unreal Insights {insights}")
    directory = directory.resolve()
    root = directory.as_posix()
    columns = "-columns=ThreadName,TimerName,StartTime,EndTime,Depth"
    gpu = directory / "gpu-timing.tsv"
    cpu = directory / "bridge-cpu-timing.tsv"
    request = directory / "insights-export.rsp"
    request.write_text(
        f'TimingInsights.ExportTimingEvents "{root}/{gpu.name}" -threads=GPU* {columns}\n'
        f'TimingInsights.ExportTimingEvents "{root}/{cpu.name}" '
        f'-threads=GameThread,RenderThread*,RHIThread*,*Worker* -timers=Minecraft* {columns}\n',
        encoding="utf-8",
    )
    subprocess.run(
        [str(insights), f"-OpenTraceFile={root}/timing.utrace", f"-ABSLOG={root}/insights-export.log",
         "-NoUI", "-AutoQuit", f"-ExecOnAnalysisCompleteCmd=@={request.as_posix()}"],
        check=True, timeout=120, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
    )
    return gpu, cpu


def trace_scopes(path, track_filter, min_depth):
    groups = defaultdict(list)
    with path.open(encoding="utf-8-sig", newline="") as stream:
        for row in csv.DictReader(stream, delimiter="\t"):
            track, scope = row["ThreadName"], row["TimerName"]
            if not track_filter(track) or int(row["Depth"]) < min_depth:
                continue
            duration = 1000.0 * (float(row["EndTime"]) - float(row["StartTime"]))
            if math.isfinite(duration) and duration > 0:
                groups[(track, scope, row["Depth"])].append(duration)
    return sorted(((summarize(values)[1], key, values) for key, values in groups.items()), reverse=True)


def print_trace_scopes(title, scopes, limit):
    print(f"\n### {title}\n")
    if not scopes:
        print("这次追踪未导出该类事件。")
        return
    print("| 轨道 | 事件 | 单次 P50（ms） | 单次 P95（ms） | 次数 |")
    print("|---|---|---:|---:|---:|")
    for p50, (track, scope, depth), values in scopes[:limit]:
        _, _, p95, _ = summarize(values)
        print(f"| `{track}` | `{scope}`（深度 {depth}） | {p50:.2f} | {p95:.2f} | {len(values)} |")


def report(directory, settings, rows, limit):
    frame_mean = statistics.fmean(float(row["FrameTime"]) for row in rows)
    print(f"## {directory.name}\n")
    print(f"{settings['preset']} · 内部 {settings['input']} → 输出 {settings['output']} · {len(rows)} 帧 · {1000 / frame_mean:.1f} FPS\n")
    print("| 计时（ms） | 平均 | P50 | P95 | P99 | 有效样本 |")
    print("|---|---:|---:|---:|---:|---:|")
    for column, label in THREADS.items():
        values = [value for row in rows if (value := number(row.get(column))) is not None]
        if values and any(values):
            print_matrix_row(label, values)
        else:
            print(f"| {label} | — | — | — | — | 0 |")

    cpu_rows = []
    for thread in ("GameThread", "RenderThread", "RHIThread"):
        for mean, name, _, values in ranked_scopes(rows, f"Exclusive/{thread}/")[:5]:
            _, p50, p95, _ = summarize(values)
            cpu_rows.append(f"| {thread} | `{name}` | {mean:.2f} | {p50:.2f} | {p95:.2f} |")
    if cpu_rows:
        print("\n### CPU 线程内的主要计时\n")
        print("| 线程 | UE 自耗时计数器（ms） | 平均 | P50 | P95 |")
        print("|---|---|---:|---:|---:|")
        print("\n".join(cpu_rows))
        print("\n`EventWait` 等行记录线程等待。桥接模块及工作线程的具体调用可在 `timing.utrace` 中查看。")

    passes = ranked_scopes(rows, "GPU/")
    if passes:
        print("\n### GPU 通道\n")
        print("| 通道（ms） | 平均 | P50 | P95 | 有计时帧 |")
        print("|---|---:|---:|---:|---:|")
        for mean, name, active, values in passes[:limit]:
            _, p50, p95, _ = summarize(values)
            print(f"| `{name}` | {mean:.2f} | {p50:.2f} | {p95:.2f} | {active}/{len(rows)} |")
        print("\n通道均值按全部采集帧统计，未执行的帧按 0 计。嵌套通道和并行队列存在重叠，请在 Insights 查看时间线。")
    else:
        print("\n本次 CSV 缺少 GPU 通道样本。打开 `timing.utrace` 检查 GPU 轨道和运行时 RHI。")
    print()
    return frame_mean


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", type=Path, nargs="+", help="capture-* directories")
    parser.add_argument("--limit", type=int, default=12, help="number of GPU passes to show")
    parser.add_argument("--engine-root", type=Path, help="UE installation to export async GPU and bridge CPU trace scopes")
    args = parser.parse_args()
    if args.limit < 1:
        parser.error("--limit must be positive")
    try:
        captures = [(directory, *load_capture(directory)) for directory in args.captures]
        means = []
        for capture in captures:
            trace = export_trace(capture[0], args.engine_root) if args.engine_root else None
            means.append(report(*capture, args.limit))
            if trace:
                gpu, cpu = trace
                print_trace_scopes("GPU 异步计算队列", trace_scopes(gpu, lambda track: "-Compute" in track, 2), args.limit)
                print_trace_scopes("Minecraft 桥接 CPU 事件", trace_scopes(cpu, lambda track: True, 0), args.limit)
                print("\n时间线按单次事件统计。父子事件及不同队列的时间可能重叠。\n")
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
        parser.exit(1, f"{exc}\n")
    if len(means) > 1:
        print("## 相对第一组采集的整帧平均差值\n")
        print("对比时请核对 `settings.txt` 中的分辨率、镜头和渲染设置。\n")
        for directory, mean in zip(args.captures[1:], means[1:]):
            print(f"- {directory.name} · {mean - means[0]:+.2f} ms · {(mean / means[0] - 1):+.1%}")


if __name__ == "__main__":
    main()
