from __future__ import annotations

import io
import random
import struct
import zlib
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw


STREAM_TILE_MAGIC = b"VDMIMGTL"
STREAM_TILE_VERSION = 1
STREAM_TILE_ENCODING_PNG = 1
STREAM_TILE_HEADER_PREFIX = ">8sBBHHHHHHHII"
STREAM_TILE_HEADER_SIZE = struct.calcsize(STREAM_TILE_HEADER_PREFIX) + 4


@dataclass(frozen=True)
class EncodedTile:
    tile_id: int
    x: int
    y: int
    width: int
    height: int
    payload: bytes


@dataclass(frozen=True)
class ReconstructionResult:
    image: Image.Image
    recovered_tile_fraction: float
    recovered_pixel_fraction: float
    exact_match: bool
    recovered_tile_count: int
    total_tile_count: int


def tile_count_for_image(image: Image.Image, tile_size: int) -> int:
    width, height = image.size
    tiles_x = (width + tile_size - 1) // tile_size
    tiles_y = (height + tile_size - 1) // tile_size
    return tiles_x * tiles_y


def load_image(image_path: str | Path, resize_max: int) -> Image.Image:
    image = Image.open(image_path).convert("RGB")
    image.thumbnail((resize_max, resize_max))
    return image


def split_image_into_tiles(image: Image.Image, tile_size: int) -> list[EncodedTile]:
    width, height = image.size
    tiles: list[EncodedTile] = []
    tile_id = 0
    for top in range(0, height, tile_size):
        for left in range(0, width, tile_size):
            tile = image.crop((left, top, min(left + tile_size, width), min(top + tile_size, height)))
            buffer = io.BytesIO()
            tile.save(buffer, format="PNG")
            tiles.append(
                EncodedTile(
                    tile_id=tile_id,
                    x=left,
                    y=top,
                    width=tile.size[0],
                    height=tile.size[1],
                    payload=buffer.getvalue(),
                )
            )
            tile_id += 1
    return tiles


def encode_stream_tiled_image(image: Image.Image, tile_size: int) -> bytes:
    records = bytearray()
    canvas_width, canvas_height = image.size
    for tile in split_image_into_tiles(image, tile_size):
        payload_crc = zlib.crc32(tile.payload) & 0xFFFFFFFF
        header_without_crc = struct.pack(
            STREAM_TILE_HEADER_PREFIX,
            STREAM_TILE_MAGIC,
            STREAM_TILE_VERSION,
            STREAM_TILE_ENCODING_PNG,
            canvas_width,
            canvas_height,
            tile.tile_id,
            tile.x,
            tile.y,
            tile.width,
            tile.height,
            len(tile.payload),
            payload_crc,
        )
        header_crc = zlib.crc32(header_without_crc) & 0xFFFFFFFF
        records.extend(header_without_crc)
        records.extend(struct.pack(">I", header_crc))
        records.extend(tile.payload)
    return bytes(records)


def reconstruct_stream_tiled_image(
    recovered_bytes: bytes,
    expected_size: tuple[int, int],
    expected_tile_count: int,
    placeholder_color: tuple[int, int, int] = (96, 96, 96),
) -> ReconstructionResult:
    offset = 0
    canvas: Image.Image | None = None
    total_tiles: set[int] = set()
    recovered_tiles: set[int] = set()
    recovered_pixels = 0

    while True:
        start = recovered_bytes.find(STREAM_TILE_MAGIC, offset)
        if start < 0:
            break
        if start + STREAM_TILE_HEADER_SIZE > len(recovered_bytes):
            break

        header_without_crc = recovered_bytes[start : start + STREAM_TILE_HEADER_SIZE - 4]
        header_crc = struct.unpack(">I", recovered_bytes[start + STREAM_TILE_HEADER_SIZE - 4 : start + STREAM_TILE_HEADER_SIZE])[0]
        if (zlib.crc32(header_without_crc) & 0xFFFFFFFF) != header_crc:
            offset = start + 1
            continue

        (
            magic,
            version,
            encoding,
            canvas_width,
            canvas_height,
            tile_id,
            tile_x,
            tile_y,
            tile_width,
            tile_height,
            payload_len,
            payload_crc,
        ) = struct.unpack(STREAM_TILE_HEADER_PREFIX, header_without_crc)

        if magic != STREAM_TILE_MAGIC or version != STREAM_TILE_VERSION or encoding != STREAM_TILE_ENCODING_PNG:
            offset = start + 1
            continue

        payload_start = start + STREAM_TILE_HEADER_SIZE
        payload_end = payload_start + payload_len
        if payload_end > len(recovered_bytes):
            break

        payload = recovered_bytes[payload_start:payload_end]
        if (zlib.crc32(payload) & 0xFFFFFFFF) != payload_crc:
            offset = start + 1
            continue

        if canvas is None:
            canvas = Image.new("RGB", (canvas_width, canvas_height), placeholder_color)
        total_tiles.add(tile_id)

        if tile_id not in recovered_tiles:
            try:
                tile_image = Image.open(io.BytesIO(payload)).convert("RGB")
            except Exception:
                offset = start + 1
                continue
            canvas.paste(tile_image, (tile_x, tile_y))
            recovered_tiles.add(tile_id)
            recovered_pixels += tile_width * tile_height

        offset = payload_end

    if canvas is None:
        canvas = Image.new("RGB", expected_size, placeholder_color)

    total_tile_count = max(1, expected_tile_count)
    canvas_pixels = canvas.size[0] * canvas.size[1]
    recovered_tile_fraction = len(recovered_tiles) / total_tile_count
    recovered_pixel_fraction = recovered_pixels / canvas_pixels if canvas_pixels else 0.0
    exact_match = recovered_tile_fraction == 1.0 and recovered_pixel_fraction == 1.0
    return ReconstructionResult(
        image=canvas,
        recovered_tile_fraction=recovered_tile_fraction,
        recovered_pixel_fraction=recovered_pixel_fraction,
        exact_match=exact_match,
        recovered_tile_count=len(recovered_tiles),
        total_tile_count=total_tile_count,
    )


def encode_datagram_tiles(image: Image.Image, tile_size: int, max_payload_bytes: int) -> list[EncodedTile]:
    encoded: list[EncodedTile] = []
    for tile in split_image_into_tiles(image, tile_size):
        tile_image = image.crop((tile.x, tile.y, tile.x + tile.width, tile.y + tile.height))
        quality = 90
        payload = b""
        while quality >= 40:
            buffer = io.BytesIO()
            tile_image.save(buffer, format="JPEG", quality=quality, optimize=True)
            payload = buffer.getvalue()
            if len(payload) <= max_payload_bytes:
                break
            quality -= 10
        if len(payload) > max_payload_bytes:
            raise ValueError(f"tile {tile.tile_id} exceeds payload budget; lower tile size")
        encoded.append(
            EncodedTile(
                tile_id=tile.tile_id,
                x=tile.x,
                y=tile.y,
                width=tile.width,
                height=tile.height,
                payload=payload,
            )
        )
    return encoded


def simulate_datagram_tile_pipeline(
    image: Image.Image,
    tile_size: int,
    drop_rate: float,
    seed: int,
    max_payload_bytes: int = 1200,
    placeholder_color: tuple[int, int, int] = (96, 96, 96),
) -> ReconstructionResult:
    encoded_tiles = encode_datagram_tiles(image, tile_size, max_payload_bytes)
    rng = random.Random(seed)
    canvas = Image.new("RGB", image.size, placeholder_color)
    recovered_tiles = 0
    recovered_pixels = 0

    for tile in encoded_tiles:
        if rng.random() < drop_rate:
            continue
        tile_image = Image.open(io.BytesIO(tile.payload)).convert("RGB")
        canvas.paste(tile_image, (tile.x, tile.y))
        recovered_tiles += 1
        recovered_pixels += tile.width * tile.height

    total_tiles = max(1, len(encoded_tiles))
    pixel_total = image.size[0] * image.size[1]
    return ReconstructionResult(
        image=canvas,
        recovered_tile_fraction=recovered_tiles / total_tiles,
        recovered_pixel_fraction=recovered_pixels / pixel_total if pixel_total else 0.0,
        exact_match=recovered_tiles == total_tiles,
        recovered_tile_count=recovered_tiles,
        total_tile_count=total_tiles,
    )


def build_comparison_figure(
    reference: Image.Image,
    labeled_images: list[tuple[str, Image.Image]],
    output_path: str | Path,
) -> None:
    tile_w, tile_h = reference.size
    caption_h = 42
    columns = len(labeled_images) + 1
    canvas = Image.new("RGB", (columns * tile_w, tile_h + caption_h), color=(245, 245, 245))
    draw = ImageDraw.Draw(canvas)
    canvas.paste(reference, (0, caption_h))
    draw.text((12, 12), "Original", fill=(20, 20, 20))

    for idx, (label, image) in enumerate(labeled_images, start=1):
        x = idx * tile_w
        canvas.paste(image, (x, caption_h))
        draw.text((x + 12, 12), label, fill=(20, 20, 20))

    canvas.save(output_path)
