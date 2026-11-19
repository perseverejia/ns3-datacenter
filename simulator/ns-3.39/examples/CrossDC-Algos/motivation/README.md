# Motivation experiments

## Setup

Both scenarios compare DCQCN, ATC, GRC, Themis, and BRIDGE (`dciext` in the simulator) on the GRC topology. Each flow sends 300,000,000 bytes to host 4 through the 40 Gb/s link from leaf 8. Each WAN link has a one-way propagation delay of 5 ms. Both scenarios use ns-3 seed 7 and run number 1. [Topology](topology.txt) · [Configuration](config.txt)

| Scenario | Sources and start times |
| --- | --- |
| [Two long-haul flows and one short-haul flow](flows.txt) | Hosts 0 and 2 start at 0 ms over separate WAN links. Host 5 starts at 5.010 ms in the receiver's data center. |
| [Four long-haul flows](flows-four-long.txt) | All four hosts start at 0 ms. Hosts 0 and 1 send through DCI 9, and hosts 2 and 3 send through DCI 10. All paths enter DCI 11 and have a propagation RTT of 10.012 ms. |

Use a C++17 compiler, CMake, and a Python 3.10+ environment. From the repository root, install the plotting dependencies.

```bash
cd simulator/ns-3.39/examples/CrossDC-Algos/motivation
python3 -m pip install -r requirements.txt
```

## Run

From the motivation directory, run either command to build the simulator, run all five algorithms, and generate the figures for that scenario.

```bash
python3 run.py --jobs 5
python3 run.py --scenario four-long --jobs 5
```

Append `--stage simulate` to run only the simulations or `--stage plot` to redraw from an existing run. Both scenarios write to `results/` and `figures/`. Four-flow run directories and figure names carry the `-four-long` suffix, for example `results/BRIDGE-four-long/` and `figures/rx-rate-four-long.pdf`. Use `--output DIR` to select a different output directory for both scenarios.
