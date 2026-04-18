#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", tempfile.mkdtemp(prefix="vdm_rs_mpl_"))
import matplotlib
import pandas as pd

matplotlib.use("Agg")
import matplotlib.pyplot as plt


STAT_LINE_RE = re.compile(r"([A-Za-z0-9_]+)=([^\s]+)")


@dataclass(frozen=True)
class Scenario:
    name: str
    label: str
    k: int
    t: int


SCENARIOS = [
    Scenario(name="no_fec", label="No FEC (k=8, t=0)", k=8, t=0),
    Scenario(name="fec_default", label="FEC (k=8, t=4)", k=8, t=4),
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run seeded UDP stream-demo evaluations and generate plots."
    )
    parser.add_argument(
        "--build-dir", default="build", help="Directory containing tx/rx/link_sim binaries."
    )
    parser.add_argument(
        "--output-dir",
        default="artifacts/evaluation",
        help="Directory for CSV data, logs, plots, and report output.",
    )
    parser.add_argument(
        "--drop-rates",
        default="0.00,0.02,0.05,0.10,0.15,0.20,0.30,0.45",
        help="Comma-separated packet drop probabilities.",
    )
    parser.add_argument(
        "--seeds",
        default="1,2,3,4,5",
        help="Comma-separated RNG seeds for repeatable runs.",
    )
    parser.add_argument(
        "--latency-ms",
        type=int,
        default=25,
        help="Fixed latency to apply in link_sim.",
    )
    parser.add_argument(
        "--jitter-ms",
        type=int,
        default=10,
        help="Additional random latency to apply in link_sim.",
    )
    parser.add_argument(
        "--input-bytes",
        type=int,
        default=65536,
        help="Number of input bytes to send per trial.",
    )
    parser.add_argument(
        "--rx-timeout-ms",
        type=int,
        default=1000,
        help="Receiver stripe timeout in milliseconds.",
    )
    parser.add_argument(
        "--process-timeout-s",
        type=float,
        default=10.0,
        help="Maximum seconds to wait for rx to exit after sending input.",
    )
    return parser.parse_args()


def parse_csv_arg(raw: str, caster):
    return [caster(part.strip()) for part in raw.split(",") if part.strip()]


def parse_stats_line(log_path: Path, prefix: str) -> dict[str, str]:
    stats_line = ""
    for line in log_path.read_text().splitlines():
        if line.startswith(prefix):
            stats_line = line
    if not stats_line:
        return {}
    return {match.group(1): match.group(2) for match in STAT_LINE_RE.finditer(stats_line)}


def make_input_bytes(size: int, seed: int) -> bytes:
    return bytes(((seed * 17) + i * 31) % 256 for i in range(size))


def terminate_process(proc: subprocess.Popen[bytes] | None) -> None:
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=1.0)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=1.0)


def run_trial(
    scenario: Scenario,
    drop_rate: float,
    seed: int,
    args: argparse.Namespace,
    output_dir: Path,
    trial_index: int,
) -> dict[str, object]:
    build_dir = Path(args.build_dir)
    tx_path = build_dir / "tx"
    rx_path = build_dir / "rx"
    link_path = build_dir / "link_sim"

    trial_dir = output_dir / "trials" / f"{scenario.name}_drop_{drop_rate:.2f}_seed_{seed}"
    trial_dir.mkdir(parents=True, exist_ok=True)

    input_bytes = make_input_bytes(args.input_bytes, seed)
    input_path = trial_dir / "input.bin"
    output_path = trial_dir / "output.bin"
    input_path.write_bytes(input_bytes)

    rx_log = trial_dir / "rx.log"
    link_log = trial_dir / "link.log"
    tx_log = trial_dir / "tx.log"

    port_base = 24000 + trial_index * 10
    in_port = str(port_base)
    out_port = str(port_base + 1)

    rx_proc = None
    link_proc = None
    hung = False
    rx_exit_code = None
    tx_exit_code = None

    with rx_log.open("wb") as rx_stderr, link_log.open("wb") as link_stderr, tx_log.open(
        "wb"
    ) as tx_stderr, output_path.open("wb") as rx_stdout:
        try:
            rx_proc = subprocess.Popen(
                [
                    str(rx_path),
                    "-k",
                    str(scenario.k),
                    "-t",
                    str(scenario.t),
                    "--timeout-ms",
                    str(args.rx_timeout_ms),
                    "--exit-on-eof",
                    "localhost",
                    out_port,
                ],
                stdout=rx_stdout,
                stderr=rx_stderr,
            )
            link_proc = subprocess.Popen(
                [
                    str(link_path),
                    "--drop",
                    f"{drop_rate:.4f}",
                    "--latency-ms",
                    str(args.latency_ms),
                    "--jitter-ms",
                    str(args.jitter_ms),
                    "--seed",
                    str(seed),
                    "localhost",
                    in_port,
                    out_port,
                ],
                stdout=subprocess.DEVNULL,
                stderr=link_stderr,
            )

            time.sleep(0.2)
            tx_result = subprocess.run(
                [
                    str(tx_path),
                    "-k",
                    str(scenario.k),
                    "-t",
                    str(scenario.t),
                    "localhost",
                    in_port,
                ],
                input=input_bytes,
                stdout=subprocess.DEVNULL,
                stderr=tx_stderr,
                check=False,
            )
            tx_exit_code = tx_result.returncode

            try:
                rx_exit_code = rx_proc.wait(timeout=args.process_timeout_s)
            except subprocess.TimeoutExpired:
                hung = True
                terminate_process(rx_proc)
                rx_exit_code = rx_proc.returncode
        finally:
            terminate_process(link_proc)
            terminate_process(rx_proc)

    output_bytes = output_path.read_bytes() if output_path.exists() else b""
    rx_stats = parse_stats_line(rx_log, "rx stats:")
    link_stats = parse_stats_line(link_log, "link_sim stats:")
    tx_stats = parse_stats_line(tx_log, "tx stats:")

    recovered_fraction = len(output_bytes) / len(input_bytes) if input_bytes else 0.0
    exact_match = output_bytes == input_bytes

    row: dict[str, object] = {
        "scenario": scenario.name,
        "scenario_label": scenario.label,
        "k": scenario.k,
        "t": scenario.t,
        "drop_rate": drop_rate,
        "seed": seed,
        "input_bytes": len(input_bytes),
        "output_bytes": len(output_bytes),
        "recovered_byte_fraction": recovered_fraction,
        "exact_match": int(exact_match),
        "hung": int(hung),
        "tx_exit_code": -999 if tx_exit_code is None else tx_exit_code,
        "rx_exit_code": -999 if rx_exit_code is None else rx_exit_code,
    }

    for prefix, stats in (("rx_", rx_stats), ("link_", link_stats), ("tx_", tx_stats)):
        for key, value in stats.items():
            try:
                row[prefix + key] = float(value)
            except ValueError:
                row[prefix + key] = value

    return row


def add_plot(
    df: pd.DataFrame,
    value_column: str,
    ylabel: str,
    title: str,
    output_path: Path,
    y_limits: tuple[float, float] | None = None,
) -> None:
    fig, ax = plt.subplots(figsize=(8, 4.8))
    colors = {"No FEC (k=8, t=0)": "#b33a3a", "FEC (k=8, t=4)": "#1768ac"}

    for scenario_label, scenario_df in df.groupby("scenario_label"):
        grouped = (
            scenario_df.groupby("drop_rate")[value_column]
            .agg(["mean", "std"])
            .reset_index()
            .sort_values("drop_rate")
        )
        x = grouped["drop_rate"].to_numpy()
        y = grouped["mean"].to_numpy()
        yerr = grouped["std"].fillna(0.0).to_numpy()
        color = colors.get(scenario_label, "#333333")

        ax.plot(x, y, marker="o", linewidth=2.0, color=color, label=scenario_label)
        ax.fill_between(x, y - yerr, y + yerr, color=color, alpha=0.12)

    ax.set_title(title)
    ax.set_xlabel("Packet drop probability")
    ax.set_ylabel(ylabel)
    ax.grid(alpha=0.25)
    if y_limits is not None:
        ax.set_ylim(*y_limits)
    ax.legend(frameon=False)
    fig.tight_layout()
    fig.savefig(output_path, dpi=160)
    plt.close(fig)


def write_report(df: pd.DataFrame, args: argparse.Namespace, output_dir: Path) -> None:
    summary = (
        df.groupby(["scenario_label", "drop_rate"])[["exact_match", "recovered_byte_fraction"]]
        .mean()
        .reset_index()
    )

    worst_match = summary.loc[summary["recovered_byte_fraction"].idxmin()]
    best_gap = None
    for drop_rate, drop_df in summary.groupby("drop_rate"):
        if len(drop_df) < 2:
            continue
        ordered = drop_df.sort_values("recovered_byte_fraction", ascending=False)
        gap = ordered.iloc[0]["recovered_byte_fraction"] - ordered.iloc[-1]["recovered_byte_fraction"]
        candidate = (
            gap,
            drop_rate,
            ordered.iloc[0]["scenario_label"],
            ordered.iloc[-1]["scenario_label"],
        )
        if best_gap is None or candidate[0] > best_gap[0]:
            best_gap = candidate

    report_lines = [
        "# Stream Evaluation Visualization",
        "",
        "## Procedure",
        "",
        f"- Input size per trial: `{args.input_bytes}` bytes.",
        f"- Drop rates: `{args.drop_rates}`.",
        f"- Seeds: `{args.seeds}`.",
        f"- Link model: `--latency-ms {args.latency_ms} --jitter-ms {args.jitter_ms}`.",
        "- Compared scenarios:",
    ]
    for scenario in SCENARIOS:
        report_lines.append(f"  - `{scenario.label}`")

    report_lines.extend(
        [
            "",
            "## Key Takeaways",
            "",
            (
                f"- Lowest observed byte recovery was `{worst_match['recovered_byte_fraction']:.3f}` "
                f"for `{worst_match['scenario_label']}` at drop rate `{worst_match['drop_rate']:.2f}`."
            ),
        ]
    )

    if best_gap is not None:
        report_lines.append(
            (
                f"- Largest recovery gap was `{best_gap[0]:.3f}` at drop rate `{best_gap[1]:.2f}`: "
                f"`{best_gap[2]}` outperformed `{best_gap[3]}`."
            )
        )

    hung_trials = int(df["hung"].sum())
    report_lines.append(f"- Hung trials observed: `{hung_trials}`.")
    report_lines.extend(
        [
            "",
            "## Artifacts",
            "",
            "- `results.csv`: raw per-trial metrics.",
            "- `recovery_rate_vs_drop.png`: exact file recovery success rate.",
            "- `byte_recovery_vs_drop.png`: mean recovered byte fraction.",
            "- `latency_vs_drop.png`: mean receiver latency from `rx stats`.",
            "",
        ]
    )

    (output_dir / "report.md").write_text("\n".join(report_lines) + "\n")


def main() -> int:
    args = parse_args()
    output_dir = Path(args.output_dir)
    if output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    mpl_dir = output_dir / "mplconfig"
    mpl_dir.mkdir(parents=True, exist_ok=True)
    os.environ["MPLCONFIGDIR"] = str(mpl_dir)

    drop_rates = parse_csv_arg(args.drop_rates, float)
    seeds = parse_csv_arg(args.seeds, int)

    rows: list[dict[str, object]] = []
    trial_index = 0
    for scenario in SCENARIOS:
        for drop_rate in drop_rates:
            for seed in seeds:
                print(
                    f"running scenario={scenario.name} drop={drop_rate:.2f} seed={seed}",
                    flush=True,
                )
                row = run_trial(scenario, drop_rate, seed, args, output_dir, trial_index)
                rows.append(row)
                trial_index += 1

    df = pd.DataFrame(rows).sort_values(["scenario", "drop_rate", "seed"])
    df.to_csv(output_dir / "results.csv", index=False)

    add_plot(
        df,
        value_column="exact_match",
        ylabel="Exact recovery rate",
        title="Full File Recovery vs Packet Loss",
        output_path=output_dir / "recovery_rate_vs_drop.png",
        y_limits=(0.0, 1.05),
    )
    add_plot(
        df,
        value_column="recovered_byte_fraction",
        ylabel="Recovered byte fraction",
        title="Recovered Byte Fraction vs Packet Loss",
        output_path=output_dir / "byte_recovery_vs_drop.png",
        y_limits=(0.0, 1.05),
    )

    if "rx_avg_latency_ms" in df.columns:
        add_plot(
            df,
            value_column="rx_avg_latency_ms",
            ylabel="Average latency (ms)",
            title="Receiver Latency vs Packet Loss",
            output_path=output_dir / "latency_vs_drop.png",
            y_limits=(0.0, max(1.0, float(df["rx_avg_latency_ms"].max()) * 1.15)),
        )

    write_report(df, args, output_dir)
    print(f"wrote evaluation artifacts to {output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
