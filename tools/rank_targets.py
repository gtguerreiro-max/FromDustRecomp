import glob
import numpy as np
from PIL import Image


def psnr(a, b):
    m = float(np.mean((a - b) ** 2))
    return float("inf") if m == 0 else float(10 * np.log10(1 / m))


def fit_matrix(x, y):
    X, Y = x.reshape(-1, 3), y.reshape(-1, 3)
    A = np.column_stack((X, np.ones((X.shape[0], 1))))
    C, *_ = np.linalg.lstsq(A, Y, rcond=None)
    return np.clip((X @ C[:3] + C[3]).reshape(y.shape), 0, 1)


def main():
    ref = Image.open("build/xenia_ref.png").convert("RGB")
    res = getattr(Image, "Resampling", Image).BILINEAR
    rows, flats = [], []
    for path in sorted(glob.glob("build/replay/target_*.png")):
        img = Image.open(path).convert("RGB")
        W, H = img.size
        x = np.asarray(img, dtype=np.float64) / 255
        name = path.rsplit("/", 1)[-1]
        if x.std() < 2 / 255:
            flats.append((name, W, H))
            continue
        y = np.asarray(ref.resize((W, H), res), dtype=np.float64) / 255
        raw = psnr(y, x)
        fit = psnr(y, fit_matrix(x, y))
        m = x.mean(axis=(0, 1)) * 255
        rows.append((fit, name, W, H, raw, m))
    rows.sort(key=lambda r: r[0], reverse=True)
    for f, n, W, H, raw, m in rows:
        print(f"{n}  {W}x{H}  {raw:.3f}  {f:.3f}  {m[0]:.1f},{m[1]:.1f},{m[2]:.1f}")
    for n, W, H in flats:
        print(f"{n}  {W}x{H}  flat")


if __name__ == "__main__":
    main()
