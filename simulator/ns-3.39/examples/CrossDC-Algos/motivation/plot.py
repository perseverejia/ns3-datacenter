#!/usr/bin/env python3
"""Validate the five runs and plot queues, flow rates and GRC reference rates."""
import argparse
from dataclasses import dataclass
import json
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import MaxNLocator
import numpy as np

from run import ALGORITHMS, HERE, SCENARIO_SUFFIXES, input_hashes

# Row order in TX plots and legend order in RX plots.
FLOWS = {5: ("Short-haul", "#B14D12", "-"), 0: ("Long-haul 1", "#007079", "-"),
         2: ("Long-haul 2", "#69529C", "--")}
FOUR_LONG = {0: ("Host 0", "#007079", "-"), 1: ("Host 1", "#BA591D", "--"),
             2: ("Host 2", "#69529C", "-."), 3: ("Host 3", "#AB365E", ":")}
FLOW_BYTES = 300_000_000
NS_PER_MS = 1_000_000


@dataclass
class Run:
    queues: np.ndarray
    counters: dict
    finish_ns: dict
    join_ms: float | None


def check(condition, path, message):
    if not condition:
        raise ValueError(f"{path}: {message}")


def read_run(path, common_inputs, flows):
    manifest = json.loads((path / "run.json").read_text())
    hashes = input_hashes(path)
    check(hashes == common_inputs == manifest["inputs_sha256"], path, "input snapshots differ")
    algorithm = path.name.removesuffix(SCENARIO_SUFFIXES["four-long"])
    check(algorithm in ALGORITHMS and manifest["algorithm"] == algorithm and manifest["mode"] == ALGORITHMS[algorithm],
          path, "wrong algorithm")
    fct = np.loadtxt(path / "flows-fct.txt", dtype=np.int64, ndmin=2)
    check(fct.shape == (len(flows), 8) and set(fct[:, 0]) == flows.keys(), path, "incomplete flows")
    finish, starts = {}, {}
    for src, dst, pg, _, size, start, duration, remote in fct:
        check(dst == 4 and pg == 3 and size == FLOW_BYTES and duration > 0 and
              remote == (src != 5), path, f"incorrect completion record for host {src}")
        starts[src], finish[src] = start, start + duration
    arrivals = np.loadtxt(path / "arrivals.csv", delimiter=",", skiprows=1, dtype=int, ndmin=2)
    wan = flows.keys() - {5}
    check(arrivals.shape == (len(wan), 2) and set(arrivals[:, 0]) == wan and
          all(starts[src] == 0 for src in wan) and np.all(arrivals[:, 1] > 0),
          path, "invalid WAN starts or first arrivals at the receiver DCI")
    if 5 in flows:
        check(np.all(arrivals[:, 1] < starts[5]), path, "a WAN flow arrived after the short flow started")
    else:
        rtt = np.genfromtxt(path / "rtt.csv", delimiter=",", names=True, dtype=np.int64, ndmin=1)
        check(len(rtt) == len(wan) and set(rtt["src"]) == wan and np.all(rtt["dst"] == 4) and
              len(set(rtt["path_rtt_ns"])) == 1 and len(set(rtt["qp_base_rtt_ns"])) == 1 and
              np.all(rtt["path_rtt_ns"] > 0) and np.all(rtt["qp_base_rtt_ns"] > 0) and
              np.array_equal(rtt["first_ack_ns"] - rtt["first_tx_ns"], rtt["first_ack_rtt_ns"]) and
              np.all(rtt["first_ack_rtt_ns"] > 0), path, "unequal base RTTs or incomplete first ACKs")

    q = np.genfromtxt(path / "queues.csv", delimiter=",", names=True, dtype=np.int64)
    required = ("dci_11_to_8_bytes", "leaf_8_to_4_bytes", "themis_loop_fifo_bytes",
                "themis_loop_resident_data_bytes", "themis_loop_resident_dummy_bytes",
                "atc_voq_to_4_bytes", "dci_queued_bytes")
    check({"time_ns", *required} <= set(q.dtype.names or ()), path,
          "missing queue components; rerun the simulations with run.py")
    check(q.shape == (1001,) and np.array_equal(q["time_ns"], np.arange(1001)*10000) and
          all(np.all(q[field] >= 0) for field in required), path, "incomplete 0–10 ms queue samples")
    fifo = q["themis_loop_fifo_bytes"]
    resident = q["themis_loop_resident_data_bytes"] + q["themis_loop_resident_dummy_bytes"]
    voq = q["atc_voq_to_4_bytes"]
    check(np.array_equal(q["dci_queued_bytes"], q["dci_11_to_8_bytes"] + fifo + voq) and
          np.all(fifo <= resident) and (algorithm == "Themis" or not resident.any()) and
          (algorithm == "ATC" or not voq.any()), path, "inconsistent queue accounting")
    data = np.loadtxt(path / "counters.csv", delimiter=",", skiprows=1, dtype=np.int64)
    check(set(data[:, 1]) == flows.keys(), path, "unexpected sources in byte counters")
    counters = {}
    for src in flows:
        a = data[data[:, 1] == src][:, [0, 2, 3]]
        check(np.all(a[0] == 0) and np.all(np.diff(a[:, 0]) == 50_000) and
              np.all(np.diff(a[:, 1:], axis=0) >= 0) and a[-1, 0] >= max(finish.values()) and
              a[-1, 1] >= FLOW_BYTES and a[-1, 2] == FLOW_BYTES,
              path, f"missing samples or incomplete byte accounting for host {src}")
        # Tail samples past each ACK completion must be flat, even for a retransmitting flow.
        after = np.searchsorted(a[:, 0], finish[src])
        check(np.all(a[after:, 1:] == a[-1, 1:]), path, "payload after flow completion")
        counters[src] = a
    return Run(q, counters, finish, starts[5] / NS_PER_MS if 5 in starts else None)


def rates(run, src):
    a = run.counters[src]
    # Bins share a global clock. Include the last bin containing the final ACK.
    stop = np.searchsorted(a[:, 0], run.finish_ns[src])
    indices = np.arange(0, stop + 1, 20)  # 20 x 50 us = 1 ms
    if indices[-1] != stop:
        indices = np.append(indices, stop)
    samples = a[indices]
    gbps = np.diff(samples[:, 1:], axis=0) * 8 / np.diff(samples[:, 0])[:, None]
    return samples[:, 0] / NS_PER_MS, gbps


def layout(rows, height, names):
    fig, axes = plt.subplots(rows, len(names), figsize=(13.2, height), squeeze=False,
                             sharex="col", sharey="row")
    for col, name in enumerate(names):
        axes[0, col].set_title(name, fontweight="semibold", pad=6)
        for ax in axes[:, col]:
            ax.set_axisbelow(True)
            ax.grid(axis="y", color="#DBDFE1", linewidth=0.55)
            ax.tick_params(length=3, width=0.65, pad=3)
            ax.spines[["top", "right"]].set_visible(False)
            ax.spines[["bottom", "left"]].set_color("#879098")
    return fig, axes


def save(fig, directory, name):
    fig.savefig(directory / f"{name}.pdf", metadata={"CreationDate": None, "ModDate": None})
    fig.savefig(directory / f"{name}.png", dpi=220)
    plt.close(fig)


def mark_short_start(ax, join_ms):
    left, right = ax.get_xlim()
    if join_ms is None or not left <= join_ms <= right:
        return
    return ax.axvline(join_ms, ymax=0.985, color="#98445F", lw=1.0, ls="--",
                      marker="v", markevery=[1], markersize=3.6, markeredgewidth=0,
                      label=f"Short flow starts at {join_ms:.3f} ms")


def draw_queues(runs, output, suffix=""):
    fig, axes = layout(2, 4.1, runs)
    for col, run in enumerate(runs.values()):
        for row, (field, color) in enumerate((("dci_queued_bytes", "#205772"),
                                              ("leaf_8_to_4_bytes", "#4A7750"))):
            ax = axes[row, col]
            ax.plot(run.queues["time_ns"]/NS_PER_MS, run.queues[field]/1e6, color=color, lw=1.0)
            peak = run.queues[field].max()
            peak_label = f"{peak / 1e6:.2f} MB" if peak >= 100_000 else f"{peak / 1e3:.2f} kB"
            ax.text(0.035, 0.92, f"Peak {peak_label}", transform=ax.transAxes,
                    ha="left", va="top", fontsize=8, color="#35434B")
            ax.set_xlim(0, 10)
            marker = mark_short_start(ax, run.join_ms)
            ax.set_xticks([0, 2, 4, 6, 8, 10])
    for row, label in enumerate(("DCI queue (MB)", "Leaf queue (MB)")):
        axes[row, 0].set_ylabel(label)
        axes[row, 0].set_ylim(bottom=-0.025 * axes[row, 0].get_ylim()[1])
        axes[row, 0].yaxis.set_major_locator(MaxNLocator(nbins=4))
    if marker is not None:
        fig.legend([marker], [marker.get_label()],
                   loc="upper right", frameon=False, bbox_to_anchor=(0.48, 1))
    fig.text(0.51 if marker is not None else 0.5, 0.968, "DCI includes the ATC VOQ and Themis loop FIFO",
             va="center", ha="left" if marker is not None else "center", fontsize=9)
    fig.supxlabel("Time from long-haul flow start (ms)", y=0.025)
    fig.subplots_adjust(left=0.064, right=0.987, bottom=0.165, top=0.835, hspace=0.22, wspace=0.15)
    save(fig, output, f"startup-queues{suffix}")


def draw_rates(runs, output, tx, flows, suffix=""):
    rows = len(flows) if tx else 1
    height = 5.1 + 1.25 * (rows - 3) if tx else 2.95
    fig, axes = layout(rows, height, runs)
    for col, run in enumerate(runs.values()):
        for row, (src, (label, color, style)) in enumerate(flows.items()):
            edges, values = rates(run, src)
            ax = axes[row if tx else 0, col]
            ax.stairs(values[:, 0 if tx else 1], edges, baseline=None,
                      color=color, linewidth=0.85, linestyle="-" if tx else style, label=label)
        ax.set_xlim(0, max(run.finish_ns.values()) / NS_PER_MS + 0.1)
        ax.xaxis.set_major_locator(MaxNLocator(nbins=4, steps=[1, 2, 2.5, 5, 10]))
        for panel in axes[:, col]:
            marker = mark_short_start(panel, run.join_ms)
    for row in range(len(axes)):
        axes[row, 0].set_ylim(-0.75, 40.5)
        axes[row, 0].set_yticks([0, 10, 20, 30, 40])
    if tx:
        for ax, (label, _, _) in zip(axes[:, 0], flows.values()):
            ax.set_ylabel(f"{label}\nTX (Gb/s)")
        if marker is not None:
            fig.legend([marker], [marker.get_label()], loc="upper center", frameon=False)
    else:
        axes[0, 0].set_ylabel("RX goodput (Gb/s)")
        handles, labels = axes[0, 0].get_legend_handles_labels()
        fig.legend(handles, labels, loc="upper center", ncol=len(labels), frameon=False)
    fig.supxlabel("Time from long-haul flow start (ms)", y=0.025)
    top = 0.92 if tx else 0.76
    if tx and marker is not None:
        top = 0.86
    fig.subplots_adjust(left=0.068, right=0.987, bottom=0.135 if tx else 0.225,
                        top=top, hspace=0.22, wspace=0.17)
    name = "tx-rate" if tx else "rx-rate"
    save(fig, output, name + suffix)


def draw_grc_reference(run, directory, output, suffix=""):
    path = directory / "grc-trace" / "rates.csv"
    data = np.genfromtxt(path, delimiter=",", names=True, dtype=np.int64,
                         usecols=("time_ns", "src_as", "dst_as", "ref_Bps"))
    fig, (full, zoom) = plt.subplots(1, 2, figsize=(10.2, 3.6), sharey=True,
                                     gridspec_kw={"width_ratios": [1.8, 1]})
    paths = [(0, 9, "#205772", "-", 2.3), (1, 10, "#BA591D", "--", 1.4)]
    bounds = []
    stop = max(run.finish_ns.values())
    for source_dc, dci, color, style, width in paths:
        rows = data[(data["src_as"] == source_dc) & (data["dst_as"] == 2)]
        ticks = rows["time_ns"]
        check(len(ticks) >= 2 and ticks[0] == 0 and ticks[1] > 0 and
              np.all(np.diff(ticks) == ticks[1]) and np.all(rows["ref_Bps"] >= 0),
              path, f"incomplete reference rate series for source DC {source_dc}")
        check(ticks[-1] >= stop, path, "missing final reference rate epoch")
        last = np.searchsorted(ticks, stop)
        # The controller logs ref_before at t. It covers [previous tick, t).
        edges = np.append(ticks[:last], stop) / NS_PER_MS
        values = rows["ref_Bps"][1:last + 1] * 8e-9
        bounds.extend([values.min(), values.max()])
        for ax in (full, zoom):
            ax.stairs(values, edges, baseline=None, label=f"DCI {dci} (DC {source_dc} → DC 2)",
                      color=color, linestyle=style, linewidth=width)

    end_ms = stop / NS_PER_MS
    padding = max(0.05, (max(bounds) - min(bounds)) * 0.1)
    full.set_ylim(max(0, min(bounds) - padding), max(bounds) + padding)
    for ax, end, title in [(full, end_ms, "Full run"), (zoom, min(35., end_ms), "Startup detail")]:
        ax.set(xlim=(0, end), title=title, xlabel="Time (ms)")
        mark_short_start(ax, run.join_ms)
        ax.spines[["top", "right"]].set_visible(False)
        ax.grid(axis="y", color="#DBDFE1", linewidth=0.6)
        ax.set_axisbelow(True)
    full.set_ylabel("GRC reference rate (Gb/s)")
    handles, labels = full.get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncol=len(labels), frameon=False)
    fig.subplots_adjust(left=0.075, right=0.985, bottom=0.155, top=0.81, wspace=0.12)
    save(fig, output, f"grc-ref-rate{suffix}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=HERE, help="parent of results/ and figures/")
    parser.add_argument("--scenario", choices=SCENARIO_SUFFIXES)
    args = parser.parse_args()
    root = args.output.resolve()
    raw = root / "results"
    # Older standalone exports use plain algorithm names for either workload.
    plain = (raw / "DCQCN").is_dir()
    four = SCENARIO_SUFFIXES["four-long"]
    use_four = args.scenario == "four-long" or (args.scenario is None and not plain)
    result_suffix = four if use_four and (raw / f"DCQCN{four}").is_dir() else ""
    directories = {name: raw / f"{name}{result_suffix}" for name in ALGORITHMS}
    baseline = directories["DCQCN"]
    hashes = input_hashes(baseline)
    sources = np.loadtxt(baseline / "flows.txt", skiprows=1, usecols=0, dtype=int, ndmin=1)
    scenario = "motivation" if 5 in sources else "four-long"
    flows = FLOWS if scenario == "motivation" else FOUR_LONG
    check(args.scenario in (None, scenario) and set(sources) == flows.keys() and len(sources) == len(flows),
          baseline, "unexpected motivation workload")
    runs = {name: read_run(directory, hashes, flows) for name, directory in directories.items()}
    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 9, "axes.titlesize": 11,
                         "figure.labelsize": 10, "axes.linewidth": 0.68, "pdf.fonttype": 42})
    output = root / "figures"
    output.mkdir(parents=True, exist_ok=True)
    suffix = SCENARIO_SUFFIXES[scenario]
    draw_queues(runs, output, suffix)
    draw_rates(runs, output, tx=True, flows=flows, suffix=suffix)
    draw_rates(runs, output, tx=False, flows=flows, suffix=suffix)
    draw_grc_reference(runs["GRC"], directories["GRC"], output, suffix)
    print(f"Verified {len(flows) * len(runs)} completed flows. Wrote four PDF/PNG figures in {output}")


if __name__ == "__main__":
    main()
