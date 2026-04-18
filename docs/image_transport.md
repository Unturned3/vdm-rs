# Image Transport Guide

This guide covers all image-related workflows in the repo.

The important point is that **VDM-RS is a byte transport and recovery system**.
The image tooling is only a demo layer that makes transport behavior easy to
inspect visually.

## Where Images Fit

The core system is:

```text
source bytes -> tx -> link_sim -> rx -> recovered bytes
```

- `tx`: stripe bytes into `k` data shards and `t` parity shards
- `link_sim`: simulate packet loss, latency, jitter, and queue pressure
- `rx`: reconstruct stripes and emit recovered bytes

The image workflows only change:

- how image bytes are prepared before `tx`
- how recovered bytes are interpreted after `rx`

System overview:

![VDM-RS system overview](../asset/system_overview.svg)

## Integrated Image Pipeline

The integrated image pipeline keeps the existing `tx` / `rx` / `link_sim`
transport unchanged.

Instead of sending raw raster bytes, it sends a **self-synchronizing stream of
image tile records**.

Implementation:

- [tools/image_transport.py](/Users/haotianlin/Projects/Courses/CMU16761/vdm-rs/tools/image_transport.py)
- [tools/image_loss_demo.py](/Users/haotianlin/Projects/Courses/CMU16761/vdm-rs/tools/image_loss_demo.py)

Pipeline:

```text
image
  -> split into tiles
  -> encode each tile independently as PNG
  -> wrap each tile with metadata + CRC
  -> concatenate tile records into one byte stream
  -> tx -> link_sim -> rx
  -> scan recovered bytes for valid tile records
  -> decode surviving tiles
  -> paste tiles back into the image canvas
```

### Why This Works

Each tile record is independent and self-describing:

- tile id
- canvas size
- tile position `(x, y)`
- tile size `(width, height)`
- payload length
- header CRC
- payload CRC

If some Reed-Solomon stripes are unrecoverable, `rx` does **not** discard the
whole image. It skips the lost byte ranges and continues emitting later bytes.
The post-processing step can then resynchronize on later valid tile headers and
recover those tiles.

So packet loss becomes:

- missing tiles
- localized visual damage
- no global byte-shift corruption

This is the key reason the integrated image demo still makes visual sense under
loss.

## Partial Recovery

Partial recovery is implemented structurally, not heuristically.

It is **not**:

- interpolation
- inpainting
- “best effort” guessing of missing pixels

It is:

1. recover as many bytes as possible through the existing transport
2. find valid tile records in the recovered byte stream
3. decode only tiles whose headers and payloads pass CRC
4. place each surviving tile back at its original coordinates
5. leave missing tiles as gray placeholders

That means one lost stripe only destroys the tile records inside that lost byte
range. Later tile records can still survive and be reconstructed.

## Standalone Baseline

The evaluation also includes a separate baseline:

- [tools/image_pipeline_eval.py](/Users/haotianlin/Projects/Courses/CMU16761/vdm-rs/tools/image_pipeline_eval.py)

This baseline is intentionally different from the integrated pipeline.

It does **not** use the `tx` / `rx` byte-stream transport. Instead, it treats
each image tile as an independent object:

- tile the image
- encode each tile independently
- drop tiles independently under simulated loss
- reconstruct the canvas from surviving tiles

This gives a modern object-style loss pattern: many small local holes rather than
stream-shaped losses.

## What The Comparison Means

The evaluation emits two figures:

- [artifacts/image_pipeline_eval_pittsburgh/controlled_comparison.png](/Users/haotianlin/Projects/Courses/CMU16761/vdm-rs/artifacts/image_pipeline_eval_pittsburgh/controlled_comparison.png)
- [artifacts/image_pipeline_eval_pittsburgh/practical_comparison.png](/Users/haotianlin/Projects/Courses/CMU16761/vdm-rs/artifacts/image_pipeline_eval_pittsburgh/practical_comparison.png)

### Controlled Comparison

This is the fairer comparison.

- integrated stream and baseline use the same tile size
- the main changing factor is transport design

### Practical Comparison

This is the tuned comparison.

- each method uses its own configured tile size
- it reflects how the methods behave when configured differently in practice

### Reading The Rows

Top row, `Integrated Stream`:

- real VDM-RS transport path
- stronger at low loss because Reed-Solomon can recover stream stripes
- can lose larger contiguous image regions when some stripes fail

Bottom row, `Datagram Baseline`:

- independent tile objects, no VDM-RS stream recovery
- weaker at low loss because dropped tiles stay dropped
- often more graceful at high loss because surviving tiles are independent

## Reproduce The Integrated Image Demo

Build first:

```bash
cmake -S . -B build
cmake --build build
./build/unit_tests
```

Run the integrated image-over-transport demo on the provided example image:

```bash
python3 tools/image_loss_demo.py \
  --image asset/pittsburgh.jpg \
  --output-dir artifacts/image_loss_demo_tiled \
  --drop-rates 0.00,0.10,0.20,0.35,0.45 \
  --seed 2 \
  --resize-max 512 \
  --tile-size 64 \
  --process-timeout-s 10
```

Outputs:

- `artifacts/image_loss_demo_tiled/reference.png`
- `artifacts/image_loss_demo_tiled/comparison.png`
- `artifacts/image_loss_demo_tiled/summary.md`

## Reproduce The Integrated-vs-Baseline Evaluation

```bash
python3 tools/image_pipeline_eval.py \
  --image asset/pittsburgh.jpg \
  --output-dir artifacts/image_pipeline_eval_pittsburgh \
  --drop-rates 0.00,0.10,0.20,0.35,0.45 \
  --seed 2 \
  --resize-max 512 \
  --controlled-tile-size 64 \
  --stream-tile-size 64 \
  --baseline-tile-size 32 \
  --process-timeout-s 10
```

Outputs:

- `artifacts/image_pipeline_eval_pittsburgh/reference.png`
- `artifacts/image_pipeline_eval_pittsburgh/controlled_comparison.png`
- `artifacts/image_pipeline_eval_pittsburgh/practical_comparison.png`
- `artifacts/image_pipeline_eval_pittsburgh/summary.md`

## Useful Notes

- Use the integrated pipeline when you want to demonstrate how the actual
  VDM-RS byte-stream transport behaves on image content.
- Use the baseline comparison when you want to discuss transport tradeoffs
  against a modern object-style design.
- Keep the interpretation transport-first: the image layer is only a showcase
  of byte-stream recovery behavior.
