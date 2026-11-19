#!/usr/bin/env python3
"""Run a motivation workload once per algorithm and draw its figures."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
NS3 = HERE.parents[2]
ALGORITHMS = {"DCQCN": 1, "ATC": 15, "GRC": 18, "Themis": 19, "BRIDGE": 16}
SCENARIO_SUFFIXES = {"motivation": "", "four-long": "-four-long"}
INPUTS = ("config.txt", "topology.txt", "flows.txt")
TARGET = "crossdc-motivation"


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def input_hashes(directory):
    return {name: sha256(directory / name) for name in INPUTS}


def simulate(name, mode, root, binary, workload, suffix=""):
    directory = root / f"{name}{suffix}"
    directory.mkdir(parents=True, exist_ok=True)
    marker = directory / "run.json"
    marker.unlink(missing_ok=True)
    for filename in INPUTS:
        source = workload if filename == "flows.txt" else HERE / filename
        shutil.copyfile(source, directory / filename)
    args = ["--conf=config.txt", f"--algorithm={mode}", "--randomSeed=7", "--RngRun=1",
            "--ns3::RdmaHw::TxRateSampleInterval=0"]
    print(f"Running {name}", flush=True)
    with (directory / "simulation.log").open("w") as log:
        result = subprocess.run([binary, *args], cwd=directory, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f"{name} exited with {result.returncode}. See {directory / 'simulation.log'}")
    completions = [row.split() for row in (directory / "flows-fct.txt").read_text().splitlines()
                   if row and not row.startswith("#")]
    flow_rows = (directory / "flows.txt").read_text().splitlines()[1:]
    expected = {tuple(row.split()[:5]) for row in flow_rows}
    if len(completions) != len(flow_rows) or {tuple(row[:5]) for row in completions} != expected:
        raise RuntimeError(f"{name} did not finish all {len(flow_rows)} flows. See {directory / 'flows-fct.txt'}")
    manifest = {"algorithm": name, "mode": mode, "arguments": args, "binary_sha256": sha256(binary),
                "point_to_point_sha256": sha256(NS3 / "build/lib/libns3.39-point-to-point-optimized.so"),
                "inputs_sha256": input_hashes(directory)}
    marker.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Finished {name}", flush=True)


def build_binary():
    binary = NS3 / "build" / HERE.relative_to(NS3) / f"ns3.39-{TARGET}-optimized"
    cache = NS3 / "cmake-cache/CMakeCache.txt"
    config = cache.read_text().splitlines() if cache.is_file() else []
    if not binary.exists() or "build_profile:INTERNAL=optimized" not in config or "NS3_EXAMPLES:BOOL=ON" not in config:
        subprocess.run(["./ns3", "configure", "--build-profile=optimized", "--enable-examples",
                        "--disable-tests", "--disable-werror"], cwd=NS3, check=True)
    subprocess.run(["cmake", "--build", cache.parent, "--target", TARGET, "-j",
                    str(min(4, os.cpu_count() or 1))], cwd=NS3, check=True)
    return binary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scenario", choices=SCENARIO_SUFFIXES)
    parser.add_argument("--output", type=Path, default=HERE, help="parent of results/ and figures/")
    parser.add_argument("--jobs", type=int, default=2, help="concurrent simulations")
    parser.add_argument("--stage", choices=("all", "simulate", "plot"), default="all")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    suffix = SCENARIO_SUFFIXES[args.scenario or "motivation"]
    workload = HERE / f"flows{suffix}.txt"
    root = args.output.resolve()
    if args.stage != "plot":
        binary = build_binary()
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            jobs = [pool.submit(simulate, name, mode, root / "results", binary, workload, suffix)
                    for name, mode in ALGORITHMS.items()]
            for job in jobs:
                job.result()
    if args.stage != "simulate":
        command = [sys.executable, HERE / "plot.py", "--output", root]
        if args.scenario:
            command.extend(["--scenario", args.scenario])
        subprocess.run(command, check=True)


if __name__ == "__main__":
    main()
