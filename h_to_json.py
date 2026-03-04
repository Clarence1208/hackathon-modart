#!/usr/bin/env python3
"""
Convert LED Matrix Studio .h animation files to JSON format
for upload to the ModArt ESP32-S2 LED controller.

JSON format:
  {
    "frameCount": N,
    "fps": F,
    "data": "RRGGBBRRGGBB..."
  }

  "data" is a flat hex string of all pixel RGB values,
  column-major (x outer, y inner), 6 hex chars per pixel.

Usage:
  python h_to_json.py input.h [output.json] [--fps 6]
"""

import re
import json
import argparse
from pathlib import Path

WIDTH = 32
HEIGHT = 16


def parse_h_file(path: str) -> list[list[list[int]]]:
    """Parse a .h file and return frames as [frame][x][y] = 0xRRGGBB."""
    text = Path(path).read_text()

    hex_values = re.findall(r'0x([0-9A-Fa-f]{6})', text)

    pixels_per_frame = WIDTH * HEIGHT
    if len(hex_values) % pixels_per_frame != 0:
        raise ValueError(
            f"Found {len(hex_values)} hex values, not a multiple of "
            f"{pixels_per_frame} (32x16)"
        )

    frame_count = len(hex_values) // pixels_per_frame
    frames = []

    for f in range(frame_count):
        frame = []
        base = f * pixels_per_frame
        for x in range(WIDTH):
            col = []
            for y in range(HEIGHT):
                idx = base + x * HEIGHT + y
                col.append(int(hex_values[idx], 16))
            frame.append(col)
        frames.append(frame)

    return frames


def frames_to_json(frames: list[list[list[int]]], fps: int) -> dict:
    """Convert frames to the JSON dict for the ESP32 endpoint."""
    hex_parts = []
    for frame in frames:
        for x in range(WIDTH):
            for y in range(HEIGHT):
                color = frame[x][y]
                hex_parts.append(f"{color:06X}")

    return {
        "frameCount": len(frames),
        "fps": fps,
        "data": "".join(hex_parts),
    }


def main():
    parser = argparse.ArgumentParser(
        description="Convert .h animation to JSON for ModArt ESP32"
    )
    parser.add_argument("input", help="Input .h file")
    parser.add_argument("output", nargs='?', help="Output .json file (default: same name)")
    parser.add_argument("--fps", type=int, default=6, help="Frames per second (default: 6)")
    args = parser.parse_args()

    if args.output is None:
        args.output = str(Path(args.input).with_suffix('.json'))

    frames = parse_h_file(args.input)
    payload = frames_to_json(frames, args.fps)

    with open(args.output, 'w') as f:
        json.dump(payload, f)

    data_len = len(payload["data"])
    print(f"{payload['frameCount']} frames, {args.fps} fps "
          f"-> {args.output} ({data_len // 6} pixels, {data_len} hex chars)")


if __name__ == "__main__":
    main()
