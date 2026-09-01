#!/usr/bin/env python3
# python/benchmark.py — EduDevice latency + throughput benchmark
#
# Measures:
#   - Average latency (µs/op) at queue depth 1
#   - Throughput (ops/sec) across queue depths 1, 2, 4, 8, 16, 32
#   - Compares ioctl submit path vs zero-copy mmap submit path
#
# Run (inside VM as root):
#   cd ~/pcie_prototype_async/userspace
#   sudo PYTHONPATH=. python3 python/benchmark.py
#
# Output:
#   Prints results to stdout and writes benchmark_results.txt

import os
import sys
import time
import statistics
import argparse

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from python.edu_device import EduDevice, EDU_OP_FACTORIAL

# -------------------------------------------------------
# Config
# -------------------------------------------------------

DEVICE_PATH   = "/dev/edu_pci"
WARMUP_OPS    = 32       # thrown away before measurement
MEASURE_OPS   = 256      # ops per queue-depth measurement
LATENCY_OPS   = 512      # ops for latency measurement (QD=1)
TIMEOUT_MS    = 5000
QUEUE_DEPTHS  = [1, 2, 4, 8, 16, 32]

# -------------------------------------------------------
# Helpers
# -------------------------------------------------------

def now_us() -> float:
    return time.perf_counter() * 1e6


def drain(dev: EduDevice, count: int):
    """Collect exactly `count` completions, raise on timeout."""
    for i in range(count):
        c = dev.wait_completion(TIMEOUT_MS)
        if c is None:
            raise RuntimeError(f"Timeout draining completion {i}/{count}")
        if not c.ok():
            raise RuntimeError(f"Completion error: tag={c.tag} status={c.status}")


def run_batch(dev: EduDevice, qd: int, total_ops: int) -> float:
    """
    Submit `total_ops` factorial commands.
    EDU is single-command so qd>1 is not truly parallel —
    we submit one, wait, submit next. qd parameter kept for
    API compatibility but ignored.
    """
    tag  = 0
    t0   = now_us()

    for _ in range(total_ops):
        dev.submit_factorial(tag % 65535, n=(tag % 12) + 1)
        c = dev.wait_completion(TIMEOUT_MS)
        if c is None:
            raise RuntimeError("Timeout in benchmark loop")
        tag += 1

    return now_us() - t0


def measure_latency(dev: EduDevice) -> dict:
    """
    Queue depth 1: submit one, wait, repeat.
    Returns dict with min/mean/median/p99/max in µs.
    """
    samples = []
    tag = 0

    # Warmup
    for _ in range(WARMUP_OPS):
        dev.submit_factorial(tag % 65535, n=(tag % 12) + 1)
        drain(dev, 1)
        tag += 1

    # Measure
    for _ in range(LATENCY_OPS):
        t0 = now_us()
        dev.submit_factorial(tag % 65535, n=(tag % 12) + 1)
        drain(dev, 1)
        samples.append(now_us() - t0)
        tag += 1

    samples.sort()
    return {
        "min_us":    samples[0],
        "mean_us":   statistics.mean(samples),
        "median_us": statistics.median(samples),
        "p99_us":    samples[int(len(samples) * 0.99)],
        "max_us":    samples[-1],
        "stddev_us": statistics.stdev(samples),
    }


def measure_throughput(dev: EduDevice) -> list:
    """
    Sweep queue depths. Returns list of (qd, ops_per_sec, avg_lat_us).
    """
    results = []

    for qd in QUEUE_DEPTHS:
        # Warmup pass
        run_batch(dev, qd, WARMUP_OPS)

        # Measurement pass
        elapsed_us = run_batch(dev, qd, MEASURE_OPS)
        ops_per_sec = MEASURE_OPS / (elapsed_us / 1e6)
        avg_lat_us  = elapsed_us / MEASURE_OPS

        results.append((qd, ops_per_sec, avg_lat_us))

    return results


def measure_zc_vs_ioctl(dev: EduDevice) -> dict:
    """
    Compare zero-copy submit vs ioctl submit at QD=1.
    Returns dict with mean latency for each path.
    """
    samples_ioctl = []
    samples_zc    = []

    dev.map_rings()

    # Warmup both paths
    for i in range(WARMUP_OPS):
        dev.submit_factorial(i % 65535, n=5)
        drain(dev, 1)
    for i in range(WARMUP_OPS):
        dev.submit_factorial_zc(i % 65535, n=5)
        drain(dev, 1)

    # Measure ioctl path
    for i in range(LATENCY_OPS):
        t0 = now_us()
        dev.submit_factorial(i % 65535, n=5)
        drain(dev, 1)
        samples_ioctl.append(now_us() - t0)

    # Measure zero-copy path
    for i in range(LATENCY_OPS):
        t0 = now_us()
        dev.submit_factorial_zc(i % 65535, n=5)
        drain(dev, 1)
        samples_zc.append(now_us() - t0)

    dev.unmap_rings()

    return {
        "ioctl_mean_us": statistics.mean(samples_ioctl),
        "zc_mean_us":    statistics.mean(samples_zc),
        "improvement_pct": (
            (statistics.mean(samples_ioctl) - statistics.mean(samples_zc))
            / statistics.mean(samples_ioctl) * 100
        ),
    }


# -------------------------------------------------------
# Formatting
# -------------------------------------------------------

def fmt_section(title: str) -> str:
    bar = "=" * 60
    return f"\n{bar}\n  {title}\n{bar}"


def format_latency(r: dict) -> str:
    lines = [
        f"  ops measured : {LATENCY_OPS}",
        f"  min          : {r['min_us']:.1f} µs",
        f"  mean         : {r['mean_us']:.1f} µs",
        f"  median       : {r['median_us']:.1f} µs",
        f"  p99          : {r['p99_us']:.1f} µs",
        f"  max          : {r['max_us']:.1f} µs",
        f"  stddev       : {r['stddev_us']:.1f} µs",
    ]
    return "\n".join(lines)


def format_throughput(results: list) -> str:
    header = f"  {'QD':>4}  {'ops/sec':>12}  {'avg lat (µs)':>14}"
    sep    = "  " + "-" * 36
    rows   = [
        f"  {qd:>4}  {ops:>12,.0f}  {lat:>14.1f}"
        for qd, ops, lat in results
    ]
    return "\n".join([header, sep] + rows)


def format_zc(r: dict) -> str:
    lines = [
        f"  ioctl submit mean  : {r['ioctl_mean_us']:.1f} µs",
        f"  zero-copy mean     : {r['zc_mean_us']:.1f} µs",
        f"  improvement        : {r['improvement_pct']:.1f}%",
    ]
    return "\n".join(lines)


# -------------------------------------------------------
# Main
# -------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="EduDevice latency + throughput benchmark")
    parser.add_argument("--device", default=DEVICE_PATH)
    parser.add_argument("--output", default="benchmark_results.txt")
    parser.add_argument("--skip-zc", action="store_true",
                        help="Skip zero-copy vs ioctl comparison")
    args = parser.parse_args()

    if not os.path.exists(args.device):
        print(f"ERROR: {args.device} not found — run inside VM as root")
        sys.exit(1)

    lines = []

    def emit(s: str):
        print(s)
        lines.append(s)

    emit(f"EduDevice Benchmark — {args.device}")
    emit(f"Warmup ops: {WARMUP_OPS}  |  Measure ops: {MEASURE_OPS}")

    with EduDevice(args.device) as dev:

        # --- Latency at QD=1 ---
        emit(fmt_section("Latency  (queue depth = 1)"))
        lat = measure_latency(dev)
        emit(format_latency(lat))

        # --- Throughput sweep ---
        emit(fmt_section("Throughput  (ops/sec vs queue depth)"))
        tput = measure_throughput(dev)
        emit(format_throughput(tput))

        peak_qd, peak_ops, _ = max(tput, key=lambda x: x[1])
        emit(f"\n  Peak: {peak_ops:,.0f} ops/sec at QD={peak_qd}")

        # --- Zero-copy vs ioctl ---
        if not args.skip_zc:
            emit(fmt_section("Zero-copy mmap vs ioctl submit  (QD=1)"))
            zc = measure_zc_vs_ioctl(dev)
            emit(format_zc(zc))

    # --- Summary line (goes on your resume) ---
    emit(fmt_section("Resume-ready summary"))
    emit(
        f"  {lat['mean_us']:.0f} µs avg latency at QD=1  |  "
        f"{peak_ops:,.0f} ops/sec peak at QD={peak_qd}"
    )

    # Write results file
    with open(args.output, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"\nResults written to {args.output}")


if __name__ == "__main__":
    main()
