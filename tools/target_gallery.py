import sys, os, re, math
from PIL import Image, ImageDraw, ImageFont

def main():
    d, out = sys.argv[1], sys.argv[2]
    files = [f for f in os.listdir(d) if re.fullmatch(r"target_(\d+)\.png", f)]
    files.sort(key=lambda f: int(re.search(r"target_(\d+)", f).group(1)))
    cols, cw, ih, th = 5, 256, 160, 36
    rows = max(1, math.ceil(len(files) / cols))
    canvas = Image.new("RGB", (cols * cw, rows * (ih + th)), (40, 40, 40))
    draw = ImageDraw.Draw(canvas)
    font = ImageFont.load_default()
    for i, name in enumerate(files):
        im = Image.open(os.path.join(d, name)).convert("RGBA")
        ow, oh = im.size
        s = min(cw / ow, ih / oh, 1.0)
        rw, rh = max(1, int(ow * s)), max(1, int(oh * s))
        im = im.resize((rw, rh), Image.LANCZOS)
        x = (i % cols) * cw
        y = (i // cols) * (ih + th)
        canvas.paste(im, (x + (cw - rw) // 2, y + (ih - rh) // 2), im)
        for j, t in enumerate((name, f"{ow}x{oh}")):
            bb = draw.textbbox((0, 0), t, font=font)
            draw.text((x + (cw - (bb[2] - bb[0])) // 2, y + ih + 6 + j * 14),
                      t, fill="white", font=font)
    canvas.save(out)

main()
