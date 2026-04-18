#!/usr/bin/env python3

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

from image_transport import (
    build_comparison_figure,
    encode_stream_tiled_image,
    load_image,
    reconstruct_stream_tiled_image,
    tile_count_for_image,
)


STAT_LINE_RE = re.compile(r"([A-Za-z0-9_]+)=([^\s]+)")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Send a tiled image through tx/rx/link_sim and visualize recovery."
    )
    parser.add_argument("--image", required=True, help="Input image path.")
    parser.add_argument("--build-dir", default="build", help="Directory containing tx/rx/link_sim.")
    parser.add_argument(
        "--output-dir",
        default="artifacts/image_loss_demo",
        help="Directory for the compact output artifacts.",
    )
    parser.add_argument(
        "--drop-rates",
        default="0.00,0.05,0.10,0.20,0.35,0.45",
        help="Comma-separated packet drop probabilities.",
    )
    parser.add_argument("--seed", type=int, default=1, help="Random seed for link_sim.")
    parser.add_argument("--k", type=int, default=8, help="Data shards per stripe.")
    parser.add_argument("--t", type=int, default=4, help="Parity shards per stripe.")
    parser.add_argument("--tile-size", type=int, default=64, help="Tile size in pixels.")
    parser.add_argument("--resize-max", type=int, default=512, help="Max image dimension.")
    parser.add_argument("--latency-ms", type=int, default=25, help="Fixed link latency.")
    parser.add_argument("--jitter-ms", type=int, default=10, help="Additional link jitter.")
    parser.add_argument(
        "--rx-timeout-ms", type=int, default=1000, help="Receiver stripe timeout in milliseconds."
    )
    parser.add_argument(
        "--process-timeout-s",
        type=float,
        default=10.0,
        help="Max seconds to wait for the receiver to exit.",
    )
    parser.add_argument(
        "--keep-intermediates",
        action="store_true",
        help="Keep per-drop raw logs and recovered stream files for debugging.",
    )
    return parser.parse_args()


def parse_stats_line(log_path: Path, prefix: str) -> dict[str, str]:
    stats_line = ""
    for line in log_path.read_text().splitlines():
        if line.startswith(prefix):
            stats_line = line
    if not stats_line:
        return {}
    return {match.group(1): match.group(2) for match in STAT_LINE_RE.finditer(stats_line)}


def terminate_process(proc: subprocess.Popen[bytes] | None) -> None:
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=1.0)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=1.0)


def run_loss_trial(
    build_dir: Path,
    stream_bytes: bytes,
    k: int,
    t: int,
    drop_rate: float,
    seed: int,
    latency_ms: int,
    jitter_ms: int,
    rx_timeout_ms: int,
    process_timeout_s: float,
    output_dir: Path,
    trial_index: int,
    keep_intermediates: bool,
) -> tuple[bytes, dict[str, str]]:
    tx_path = build_dir / "tx"
    rx_path = build_dir / "rx"
    link_path = build_dir / "link_sim"

    drop_tag = f"{drop_rate:.2f}"
    if keep_intermediates:
        trial_dir = output_dir / f"drop_{drop_tag}"
        trial_dir.mkdir(parents=True, exist_ok=True)
    else:
        trial_dir = Path(tempfile.mkdtemp(prefix=f"vdm_img_stream_{drop_tag}_"))

    rx_log = trial_dir / "rx.log"
    link_log = trial_dir / "link.log"
    tx_log = trial_dir / "tx.log"
    recovered_path = trial_dir / "recovered.bin"

    port_base = 27000 + trial_index * 10
    in_port = str(port_base)
    out_port = str(port_base + 1)

    rx_proc = None
    link_proc = None

    with rx_log.open("wb") as rx_stderr, link_log.open("wb") as link_stderr, tx_log.open(
        "wb"
    ) as tx_stderr, recovered_path.open("wb") as rx_stdout:
        try:
            rx_proc = subprocess.Popen(
                [
                    str(rx_path),
                    "-k",
                    str(k),
                    "-t",
                    str(t),
                    "--timeout-ms",
                    str(rx_timeout_ms),
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
                    str(latency_ms),
                    "--jitter-ms",
                    str(jitter_ms),
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
            subprocess.run(
                [str(tx_path), "-k", str(k), "-t", str(t), "localhost", in_port],
                input=stream_bytes,
                stdout=subprocess.DEVNULL,
                stderr=tx_stderr,
                check=False,
            )
            rx_proc.wait(timeout=process_timeout_s)
        except subprocess.TimeoutExpired:
            terminate_process(rx_proc)
        finally:
            terminate_process(link_proc)
            terminate_process(rx_proc)

    recovered_bytes = recovered_path.read_bytes() if recovered_path.exists() else b""
    rx_stats = parse_stats_line(rx_log, "rx stats:")

    if not keep_intermediates:
        shutil.rmtree(trial_dir, ignore_errors=True)
    return recovered_bytes, rx_stats


def main() -> int:
    args = parse_args()
    output_dir = Path(args.output_dir)
    if output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    image = load_image(args.image, args.resize_max)
    image.save(output_dir / "reference.png")
    stream_bytes = encode_stream_tiled_image(image, args.tile_size)
    total_tile_count = tile_count_for_image(image, args.tile_size)

    drop_rates = [float(part.strip()) for part in args.drop_rates.split(",") if part.strip()]
    build_dir = Path(args.build_dir)

    labeled_images: list[tuple[str, object]] = []
    summary_lines = [
        "# Image Loss Demo",
        "",
        f"- Source image: `{args.image}`",
        f"- Resized image: `{image.size[0]}x{image.size[1]}`",
        f"- Tile size: `{args.tile_size}`",
        f"- Reed-Solomon config: `k={args.k}, t={args.t}`",
        f"- Seed: `{args.seed}`",
        "",
        "| drop | recovered tiles | recovered pixels | lost stripes | eof markers |",
        "| --- | --- | --- | --- | --- |",
    ]

    for trial_index, drop_rate in enumerate(drop_rates):
        recovered_bytes, rx_stats = run_loss_trial(
            build_dir=build_dir,
            stream_bytes=stream_bytes,
            k=args.k,
            t=args.t,
            drop_rate=drop_rate,
            seed=args.seed,
            latency_ms=args.latency_ms,
            jitter_ms=args.jitter_ms,
            rx_timeout_ms=args.rx_timeout_ms,
            process_timeout_s=args.process_timeout_s,
            output_dir=output_dir,
            trial_index=trial_index,
            keep_intermediates=args.keep_intermediates,
        )

        result = reconstruct_stream_tiled_image(
            recovered_bytes,
            expected_size=image.size,
            expected_tile_count=total_tile_count,
        )
        labeled_images.append(
            (
                f"drop={drop_rate:.2f} tiles={result.recovered_tile_fraction:.2f}",
                result.image,
            )
        )
        summary_lines.append(
            f"| `{drop_rate:.2f}` | `{result.recovered_tile_fraction:.3f}` | "
            f"`{result.recovered_pixel_fraction:.3f}` | "
            f"`{rx_stats.get('lost_stripes', '0')}` | "
            f"`{rx_stats.get('eof_markers_received', '0')}` |"
        )

    build_comparison_figure(image, labeled_images, output_dir / "comparison.png")
    (output_dir / "summary.md").write_text("\n".join(summary_lines) + "\n")
    print(f"wrote image demo artifacts to {output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
