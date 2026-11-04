"""Decode the GPU-synchronized frame_reference.bin written by frame_capture.

Usage: python tools/frame_reference.py [capture.bin] [output.png]
"""
import sys

import numpy as np
from PIL import Image

from replay_pack import load_textures
from xenos_fetch import decode_texture_fetch
from xenos_texture import decode_texture


def main():
    source = sys.argv[1] if len(sys.argv) > 1 else "build/frame_reference.bin"
    output = sys.argv[2] if len(sys.argv) > 2 else "build/frame_reference.png"
    fetch, raw = next(iter(load_textures(source).values()))
    info = decode_texture_fetch(fetch)
    width, height, rgba = decode_texture(raw, info)
    pixels = np.frombuffer(rgba, np.uint8).reshape(height, width, 4)
    channels = [pixels[..., n] if n < 4 else np.full((height, width), 255 if n == 5 else 0, np.uint8)
                for n in info["swizzle"]]
    Image.fromarray(np.stack(channels, axis=2)).save(output)
    print("Saved synchronized reference %dx%d: %s" % (width, height, output))


if __name__ == "__main__":
    main()
