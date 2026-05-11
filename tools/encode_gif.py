#!/usr/bin/env python3
"""
Encode an animated GIF as a C header for src/character_gif.h.

Usage:
    python tools/encode_gif.py assets/your-char.gif > src/character_gif.h

What it does:
- Reads every frame of the input GIF.
- Resizes each frame to 135x135 (the M5StickS3 panel width, with the
  character anchored to the bottom of the 240-tall screen).
- Alpha-composites each frame onto black, so transparent-background
  source art renders cleanly against the device's black UI background
  (no halo from anti-aliased edges).
- Quantizes every frame against a single shared global palette built
  from a vertical composite of all frames. This is important: per-frame
  local palettes cause visible hue shifts during playback with the
  embedded GIF decoder we use (bitbank2/AnimatedGIF).
- Emits a C header with W/H constants, length, and a uint8_t[] array
  ready to #include into the firmware.

Requirements:
    pip install Pillow
"""
import sys
from PIL import Image

W = H = 135  # Panel width; character is bottom-anchored on a 135x240 screen.


def main(src_path: str) -> int:
    g = Image.open(src_path)
    rgb_frames, durations = [], []
    n = 0
    while True:
        try:
            g.seek(n)
        except EOFError:
            break
        rgba = g.convert("RGBA").resize((W, H), Image.LANCZOS)
        bg = Image.new("RGBA", (W, H), (0, 0, 0, 255))
        rgb = Image.alpha_composite(bg, rgba).convert("RGB")
        rgb_frames.append(rgb)
        durations.append(g.info.get("duration", 80))
        n += 1
    if not rgb_frames:
        print(f"no frames found in {src_path}", file=sys.stderr)
        return 1

    # Single global palette across all frames (no per-frame surprises).
    composite = Image.new("RGB", (W, H * len(rgb_frames)))
    for i, f in enumerate(rgb_frames):
        composite.paste(f, (0, i * H))
    master = composite.quantize(colors=128, method=Image.Quantize.MEDIANCUT)
    qframes = [
        f.quantize(palette=master, dither=Image.Dither.NONE) for f in rgb_frames
    ]

    # Save the re-encoded GIF to a temp buffer, then emit C bytes.
    import io
    buf = io.BytesIO()
    qframes[0].save(
        buf,
        format="GIF",
        save_all=True,
        append_images=qframes[1:],
        duration=durations,
        loop=0,
        optimize=False,
        disposal=2,
    )
    data = buf.getvalue()

    out = sys.stdout
    out.write(f"// Auto-generated from {src_path} by tools/encode_gif.py.\n")
    out.write(f"// {W}x{H}, single global palette, alpha-composited onto black.\n")
    out.write("#pragma once\n")
    out.write("#include <stdint.h>\n\n")
    out.write(f"static constexpr uint16_t CHAR_W = {W};\n")
    out.write(f"static constexpr uint16_t CHAR_H = {H};\n")
    out.write(f"static constexpr size_t   CHAR_GIF_LEN = {len(data)};\n\n")
    out.write("static const uint8_t char_gif[] = {\n")
    per_line = 16
    for i in range(0, len(data), per_line):
        chunk = data[i : i + per_line]
        out.write("  " + ",".join(f"0x{b:02x}" for b in chunk) + ",\n")
    out.write("};\n")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: encode_gif.py <input.gif>", file=sys.stderr)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
