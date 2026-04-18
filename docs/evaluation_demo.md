# Stream Evaluation Demo

This demo turns the UDP stream example into a repeatable evaluation harness with
plots. It compares:

- `No FEC (k=8, t=0)`: uncoded baseline
- `FEC (k=8, t=4)`: default Reed-Solomon protection

The script runs seeded loss scenarios, captures the final `tx`, `rx`, and
`link_sim` stats, and generates:

- `results.csv`: raw per-trial metrics
- `recovery_rate_vs_drop.png`: fraction of trials with an exact file match
- `byte_recovery_vs_drop.png`: mean recovered byte fraction
- `latency_vs_drop.png`: receiver latency trend
- `report.md`: short written summary of the run

## Build

```bash
cmake -S . -B build
cmake --build build
```

## Run

The evaluation script launches `rx`, `link_sim`, and `tx` locally over UDP, so it
should be run in a normal terminal environment rather than a restricted sandbox.

```bash
python3 tools/evaluate_stream_demo.py
```

Artifacts are written to:

```bash
artifacts/evaluation/
```

## Useful Variants

Smaller quick sweep:

```bash
python3 tools/evaluate_stream_demo.py \
  --drop-rates 0.00,0.05,0.10,0.20,0.45 \
  --seeds 1,2,3
```

Larger input and longer timeout:

```bash
python3 tools/evaluate_stream_demo.py \
  --input-bytes 262144 \
  --process-timeout-s 20 \
  --rx-timeout-ms 1500
```

## What To Look For

- `recovery_rate_vs_drop.png`: shows where parity starts to matter for exact file
  recovery.
- `byte_recovery_vs_drop.png`: shows how much data is still delivered even after
  exact recovery begins to fail.
- `latency_vs_drop.png`: shows the cost of recovery and buffering under loss.
- `results.csv`: useful for checking hangs, invalid packets, stripe loss, and
  EOF-marker behavior on individual seeds.
