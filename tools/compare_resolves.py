"""Compare fd_replay's resolve copies with Xenia's.

The capture runs Xenia with readback_resolve=full, so build/textures.bin holds
the copies Xenia wrote to guest memory. fd_replay dumps each of its copies as
build/replay/resolve_<order>_<address>.png; this prints, in frame order, the
PSNR of each against Xenia's texture at the same address (best of RGBA and
BGRA order, since copies may swap red and blue in memory). The first copy that
diverges points at the pass to look at.

Usage: python tools/compare_resolves.py [--save]  (--save writes xenia_<address>.png)
"""
import glob, os, re, struct, sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import replay_pack as rp

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def psnr(a, b):
    mse = ((a - b) ** 2).mean()
    return 99.0 if mse == 0 else 10 * np.log10(255.0 ** 2 / mse)


def main():
    textures = rp.load_textures(os.path.join(ROOT, "build", "textures.bin"))
    index, table = rp.build_texture_table(textures)
    count, at, images = struct.unpack_from("<I", table, 0)[0], 4, []
    for _ in range(count):
        w, h = struct.unpack_from("<2I", table, at)
        if w >> 31:  # float32 texels (16-bit fixed point): shown as 0-255 of [0, 1]
            w &= 0x7FFFFFFF
            images.append(np.clip(np.frombuffer(table, np.float32, w * h * 4, at + 8).reshape(h, w, 4) * 255, 0, 255))
            at += 8 + w * h * 16
            continue
        images.append(np.frombuffer(table, np.uint8, w * h * 4, at + 8).reshape(h, w, 4).astype(float))
        at += 8 + w * h * 4
    for path in sorted(glob.glob(os.path.join(ROOT, "build", "replay", "resolve_*.png"))):
        order, dest = re.search(r"resolve_(\d+)_([0-9A-F]+)\.png", path).groups()
        ours = np.asarray(Image.open(path).convert("RGBA")).astype(float)
        i = index.get(int(dest, 16))
        if i is None:
            print("%s %s %4dx%-4d  no Xenia copy" % (order, dest, ours.shape[1], ours.shape[0]))
            continue
        xenia = images[i]
        h, w = min(ours.shape[0], xenia.shape[0]), min(ours.shape[1], xenia.shape[1])
        a, x = ours[:h, :w], xenia[:h, :w]
        rgba, bgra = psnr(a, x), psnr(a[..., [2, 1, 0, 3]], x)
        print("%s %s %4dx%-4d  PSNR %5.1f dB (%s)  ours mean %s  xenia mean %s" % (
            order, dest, w, h, max(rgba, bgra), "rgba" if rgba >= bgra else "bgra",
            a.reshape(-1, 4).mean(0).round(0), x.reshape(-1, 4).mean(0).round(0)))
        if "--save" in sys.argv:
            Image.fromarray(xenia.astype(np.uint8)).save(os.path.join(ROOT, "build", "replay", "xenia_%s.png" % dest))


if __name__ == "__main__":
    main()
