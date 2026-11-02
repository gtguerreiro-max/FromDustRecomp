import argparse
import numpy as np
from PIL import Image


def rmse(a, b):
    return float(np.sqrt(np.mean((a - b) ** 2)) * 255)


def psnr(a, b):
    m = float(np.mean((a - b) ** 2))
    return float("inf") if m == 0 else float(10 * np.log10(1 / m))


def fit_per_channel_affine(x, y):
    X = x.reshape(-1, 3)
    Y = y.reshape(-1, 3)
    out = np.empty_like(x)
    aff = []
    for c in range(3):
        A = np.column_stack((X[:, c], np.ones(X.shape[0])))
        coef, *_ = np.linalg.lstsq(A, Y[:, c], rcond=None)
        aff.append(coef)
        out.reshape(-1, 3)[:, c] = A @ coef
    return out, aff


def main():
    p = argparse.ArgumentParser()
    p.add_argument("rendered")
    p.add_argument("reference")
    p.add_argument("--save")
    a = p.parse_args()

    R = Image.open(a.rendered).convert("RGB")
    F = Image.open(a.reference).convert("RGB").resize(R.size, getattr(Image, "Resampling", Image).BILINEAR)
    x = np.asarray(R, dtype=np.float64) / 255.
    y = np.asarray(F, dtype=np.float64) / 255.
    X = x.reshape(-1, 3)
    Y = y.reshape(-1, 3)

    # (a) per-channel affine ref = a*x + b
    yaff, aff = fit_per_channel_affine(x, y)

    # (b) per-channel gamma ref = x**g, log grid 0.2..5
    gs = np.logspace(np.log10(0.2), np.log10(5), 200)
    gam = []
    ygam = np.empty_like(y)
    for c in range(3):
        g = min(gs, key=lambda g: rmse(Y[:, c], X[:, c] ** g))
        gam.append(g)
        ygam.reshape(-1, 3)[:, c] = X[:, c] ** g

    # (c) 3x3 matrix + offset over all channels
    A = np.column_stack((X, np.ones((X.shape[0], 1))))
    C, *_ = np.linalg.lstsq(A, Y, rcond=None)
    M, off = C[:3], C[3]
    ymat = (X @ M + off).reshape(y.shape)

    for arr in (yaff, ygam, ymat):
        np.clip(arr, 0.0, 1.0, out=arr)

    print("RMSE x255")
    for c, name in enumerate("RGB"):
        print(f" {name} affine {rmse(Y[:, c], yaff.reshape(-1, 3)[:, c]):.3f}")
        print(f" {name} gamma  {rmse(Y[:, c], ygam.reshape(-1, 3)[:, c]):.3f}")
    print(f" matrix {rmse(Y, ymat.reshape(-1, 3)):.3f}")

    print("parameters")
    for c in range(3):
        print(f" ch{c} affine a={aff[c][0]:.6f} b={aff[c][1]:.6f} gamma={gam[c]:.6f}")
    print(" M =", [[f"{v:.6f}" for v in row] for row in M])
    print(" c =", [f"{v:.6f}" for v in off])

    print("PSNR dB")
    print(f" original {psnr(y, x):.3f}")
    print(f" affine   {psnr(y, yaff):.3f}")
    print(f" gamma    {psnr(y, ygam):.3f}")
    print(f" matrix   {psnr(y, ymat):.3f}")

    print("Channel permutation PSNR (best first)")
    rows = []
    for order in ("RGB", "RBG", "GRB", "GBR", "BRG", "BGR"):
        idx = tuple(order.index(ch) for ch in "RGB")
        xp = x[..., idx]
        before = psnr(y, xp)
        yaff_p, _ = fit_per_channel_affine(xp, y)
        np.clip(yaff_p, 0.0, 1.0, out=yaff_p)
        after = psnr(y, yaff_p)
        rows.append((order, before, after))

    rows.sort(key=lambda r: (r[2], r[1]), reverse=True)
    for order, before, after in rows:
        print(f" {order} before {before:.3f} after {after:.3f}")

    if a.save:
        Image.fromarray((ymat * 255).round().astype("uint8")).save(a.save)


if __name__ == "__main__":
    main()
