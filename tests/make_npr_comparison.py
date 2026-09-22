"""Lay out actual program outputs for review; this script does no stylization.

Run the project's examples/npr/demo.txt first, then run this script with Pillow.
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


root = Path(__file__).resolve().parents[1]
folder = root / "examples" / "npr"
styles = [
    ("OIL PAINT", "npr-paint-advanced", "advanced.png"),
    ("CEL / CARTOON", "npr-cartoon", "cartoon.png"),
    ("WATERCOLOR", "npr-watercolor", "watercolor.png"),
]
images = [Image.open(folder / filename).convert("RGB") for _, _, filename in styles]
assert all(image.size == images[0].size for image in images), "Demo dimensions differ"
width, height = images[0].size
margin = 24
sheet = Image.new("RGB", (width * 3 + margin * 4, height + 146), "#eeeae2")
draw = ImageDraw.Draw(sheet)


def font(size, bold=False):
    path = Path("C:/Windows/Fonts") / ("segoeuib.ttf" if bold else "segoeui.ttf")
    return ImageFont.truetype(str(path), size) if path.exists() else ImageFont.load_default(size=size)


draw.text((margin, 13), "THREE ADVANCED NPR STYLES", fill="#222222", font=font(30, True))
draw.text((margin, 54), "Same source: wiz.tga  |  Rendered by the C++ image editor", fill="#5e5b54", font=font(18))
for i, ((label, command, _), image) in enumerate(zip(styles, images)):
    x = margin + i * (width + margin)
    sheet.paste(image, (x, 93))
    draw.text((x, height + 99), label, fill="#24231f", font=font(20, True))
    text_width = draw.textlength(command, font=font(18))
    draw.text((x + width - text_width, height + 102), command, fill="#666158", font=font(18))
sheet.save(folder / "comparison.png")
print(folder / "comparison.png")
