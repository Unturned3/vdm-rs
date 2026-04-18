#!/usr/bin/env python3

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

from PIL import Image, ImageDraw

from image_transport import (
    encode_stream_tiled_image,
    load_image,
    reconstruct_stream_tiled_image,
    simulate_datagram_tile_pipeline,
    tile_count_for_image,
)


STAT_LINE_RE = re.compile(r"([A-Za-z0-9_]+)=([^\s]+)")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare the integrated tiled-stream transport against a datagram tile baseline."
    )
    parser.add_argument("--image", required=True, help="Input image path.")
    parser.add_argument("--build-dir", default="build", help="Directory containing tx/rx/link_sim.")
    parser.add_argument("--output-dir", default="artifacts/image_pipeline_eval", help="Output directory.")
    parser.add_argument(
        "--drop-rates",
        default="0.00,0.05,0.10,0.20,0.35,0.45",
        help="Comma-separated packet drop probabilities.",
    )
    parser.add_argument("--seed", type=int, default=1, help="Random seed.")
    parser.add_argument("--resize-max", type=int, default=512, help="Max image dimension.")
    parser.add_argument("--controlled-tile-size", type=int, default=64, help="Shared tile size for the controlled comparison.")
    parser.add_argument("--stream-tile-size", type=int, default=64, help="Tile size for stream transport.")
    parser.add_argument("--baseline-tile-size", type=int, default=32, help="Tile size for datagram baseline.")
    parser.add_argument("--k", type=int, default=8, help="Data shards per stripe for tx/rx.")
    parser.add_argument("--t", type=int, default=4, help="Parity shards per stripe for tx/rx.")
    parser.add_argument("--latency-ms", type=int, default=25, help="Link latency.")
    parser.add_argument("--jitter-ms", type=int, default=10, help="Link jitter.")
    parser.add_argument("--rx-timeout-ms", type=int, default=1000, help="Receiver timeout.")
    parser.add_argument("--process-timeout-s", type=float, default=10.0, help="Receiver wait timeout.")
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


def run_stream_trial(
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
    trial_index: int,
) -> tuple[bytes, dict[str, str]]:
    trial_dir = Path(tempfile.mkdtemp(prefix=f"vdm_img_eval_{drop_rate:.2f}_"))
    rx_log = trial_dir / "rx.log"
    link_log = trial_dir / "link.log"
    tx_log = trial_dir / "tx.log"
    recovered_path = trial_dir / "recovered.bin"

    tx_path = build_dir / "tx"
    rx_path = build_dir / "rx"
    link_path = build_dir / "link_sim"

    port_base = 28000 + trial_index * 10
    in_port = str(port_base)
    out_port = str(port_base + 1)

    rx_proc = None
    link_proc = None
    try:
        with rx_log.open("wb") as rx_stderr, link_log.open("wb") as link_stderr, tx_log.open(
            "wb"
        ) as tx_stderr, recovered_path.open("wb") as rx_stdout:
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
        recovered_bytes = recovered_path.read_bytes() if recovered_path.exists() else b""
        rx_stats = parse_stats_line(rx_log, "rx stats:")
        return recovered_bytes, rx_stats
    except subprocess.TimeoutExpired:
        terminate_process(rx_proc)
        return b"", {}
    finally:
        terminate_process(link_proc)
        terminate_process(rx_proc)
        shutil.rmtree(trial_dir, ignore_errors=True)


def build_pipeline_grid(
    reference: Image.Image,
    drop_rates: list[float],
    rows: list[tuple[str, list[tuple[float, Image.Image]]]],
    output_path: Path,
) -> None:
    tile_w, tile_h = reference.size
    left_label_w = 140
    caption_h = 36
    row_h = tile_h + caption_h
    canvas = Image.new(
        "RGB",
        (left_label_w + (len(drop_rates) + 1) * tile_w, caption_h + len(rows) * row_h),
        color=(245, 245, 245),
    )
    draw = ImageDraw.Draw(canvas)

    draw.text((left_label_w + 12, 10), "Original", fill=(20, 20, 20))
    for idx, drop_rate in enumerate(drop_rates, start=1):
        draw.text((left_label_w + idx * tile_w + 12, 10), f"drop={drop_rate:.2f}", fill=(20, 20, 20))

    for row_index, (row_label, row_images) in enumerate(rows):
        y = caption_h + row_index * row_h
        draw.text((12, y + 12), row_label, fill=(20, 20, 20))
        canvas.paste(reference, (left_label_w, y))
        for col_index, (_, image) in enumerate(row_images, start=1):
            canvas.paste(image, (left_label_w + col_index * tile_w, y))

    canvas.save(output_path)


def run_comparison_mode(
    mode_name: str,
    image: Image.Image,
    drop_rates: list[float],
    build_dir: Path,
    args: argparse.Namespace,
    stream_tile_size: int,
    baseline_tile_size: int,
    trial_offset: int,
    output_dir: Path,
) -> list[str]:
    stream_bytes = encode_stream_tiled_image(image, stream_tile_size)
    total_stream_tiles = tile_count_for_image(image, stream_tile_size)

    stream_row: list[tuple[float, Image.Image]] = []
    baseline_row: list[tuple[float, Image.Image]] = []
    summary_lines = [
        f"## {mode_name}",
        "",
        f"- Stream transport tiles: `{stream_tile_size}`",
        f"- Datagram baseline tiles: `{baseline_tile_size}`",
        "",
        "| pipeline | drop | recovered tiles | recovered pixels | exact |",
        "| --- | --- | --- | --- | --- |",
    ]

    for trial_index, drop_rate in enumerate(drop_rates):
        recovered_bytes, _ = run_stream_trial(
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
            trial_index=trial_offset + trial_index,
        )
        stream_result = reconstruct_stream_tiled_image(
            recovered_bytes,
            expected_size=image.size,
            expected_tile_count=total_stream_tiles,
        )
        stream_row.append((drop_rate, stream_result.image))
        summary_lines.append(
            f"| `stream_tiled` | `{drop_rate:.2f}` | `{stream_result.recovered_tile_fraction:.3f}` | "
            f"`{stream_result.recovered_pixel_fraction:.3f}` | `{int(stream_result.exact_match)}` |"
        )

        baseline_result = simulate_datagram_tile_pipeline(
            image=image,
            tile_size=baseline_tile_size,
            drop_rate=drop_rate,
            seed=args.seed,
        )
        baseline_row.append((drop_rate, baseline_result.image))
        summary_lines.append(
            f"| `datagram_tiled` | `{drop_rate:.2f}` | `{baseline_result.recovered_tile_fraction:.3f}` | "
            f"`{baseline_result.recovered_pixel_fraction:.3f}` | `{int(baseline_result.exact_match)}` |"
        )

    figure_name = "controlled_comparison.png" if mode_name == "Controlled Comparison" else "practical_comparison.png"
    build_pipeline_grid(
        reference=image,
        drop_rates=drop_rates,
        rows=[
            ("Integrated Stream", stream_row),
            ("Datagram Baseline", baseline_row),
        ],
        output_path=output_dir / figure_name,
    )
    summary_lines.append("")
    return summary_lines


def main() -> int:
    args = parse_args()
    output_dir = Path(args.output_dir)
    if output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    image = load_image(args.image, args.resize_max)
    image.save(output_dir / "reference.png")

    drop_rates = [float(part.strip()) for part in args.drop_rates.split(",") if part.strip()]
    build_dir = Path(args.build_dir)
    summary_lines = [
        "# Image Pipeline Evaluation",
        "",
        f"- Source image: `{args.image}`",
        f"- Resized image: `{image.size[0]}x{image.size[1]}`",
        "",
        "This evaluation emits two views:",
        "",
        "- `controlled_comparison.png`: both methods use the same tile size",
        "- `practical_comparison.png`: each method uses its own configured tile size",
        "",
    ]
    summary_lines.extend(
        run_comparison_mode(
            mode_name="Controlled Comparison",
            image=image,
            drop_rates=drop_rates,
            build_dir=build_dir,
            args=args,
            stream_tile_size=args.controlled_tile_size,
            baseline_tile_size=args.controlled_tile_size,
            trial_offset=0,
            output_dir=output_dir,
        )
    )
    summary_lines.extend(
        run_comparison_mode(
            mode_name="Practical Comparison",
            image=image,
            drop_rates=drop_rates,
            build_dir=build_dir,
            args=args,
            stream_tile_size=args.stream_tile_size,
            baseline_tile_size=args.baseline_tile_size,
            trial_offset=1000,
            output_dir=output_dir,
        )
    )
    (output_dir / "summary.md").write_text("\n".join(summary_lines) + "\n")
    print(f"wrote pipeline evaluation artifacts to {output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
