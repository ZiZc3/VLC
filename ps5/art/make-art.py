#!/usr/bin/env python3
"""Draw VLC for PS5's home-screen art from the VLC logo.

    python3 ps5/art/make-art.py [out-dir]

Reads ps5/art/vlc-logo.png (transparent background). Writes:
  icon.png        1024x1024: the cone on black, an orange glow behind it and a
                  thin light along its edges
  background.png  3840x2160: no text; flowing orange waves on dark, the cone
                  at the far right
PS5_Vulkan's tools/prepare-assets.sh turns them into sce_sys/icon0.png,
pic0.dds and pic1.dds.
"""
import math
import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
ORANGE = (255, 120, 0)


def load_logo(height):
    logo = Image.open(os.path.join(HERE, "vlc-logo.png")).convert("RGBA")
    logo = logo.crop(logo.getchannel("A").getbbox())
    w = round(logo.width * height / logo.height)
    return logo.resize((w, height), Image.LANCZOS)


def solid(size, color, alpha):
    layer = Image.new("RGBA", size, color + (255,))
    layer.putalpha(alpha)
    return layer


def place_logo(canvas, logo, x, y, glow=1.0):
    """The logo with a glow behind it and a rim light on its edges."""
    a = Image.new("L", canvas.size, 0)
    a.paste(logo.getchannel("A"), (x, y))
    s = logo.height
    # Wide warm glow, then a tighter brighter one.
    for radius, strength in ((s * 0.16, 0.55), (s * 0.05, 0.6)):
        g = a.filter(ImageFilter.GaussianBlur(radius)).point(lambda v, k=strength * glow: int(v * k))
        canvas.alpha_composite(solid(canvas.size, ORANGE, g))
    canvas.alpha_composite(logo, (x, y))
    # Rim light: the outline (alpha minus its eroded self), softened, warm white.
    eroded = a.filter(ImageFilter.MinFilter(max(3, (s // 120) | 1)))
    rim = ImageChops.subtract(a, eroded).filter(ImageFilter.GaussianBlur(max(1, s // 400)))
    rim = rim.point(lambda v: int(v * 0.75))
    canvas.alpha_composite(solid(canvas.size, (255, 214, 160), rim))


def icon(path):
    size = 1024
    img = Image.new("RGBA", (size, size), (0, 0, 0, 255))
    logo = load_logo(700)
    place_logo(img, logo, (size - logo.width) // 2, (size - logo.height) // 2 + 10)
    img.convert("RGB").save(path)


def waves(size):
    """Layered translucent ribbons along slow sine curves, blurred: an orange
    flow over near-black, brightest on the right where the logo sits."""
    w, h = size
    sw, sh = w, h  # full 4K: thin strands stay smooth
    layer = Image.new("RGBA", (sw, sh), (0, 0, 0, 0))
    strands = 46
    for k in range(strands):
        u = k / (strands - 1)                  # 0..1 across the band
        # Each strand: two slow sines; the phase drifts across the band, so the
        # strands twist around each other like silk.
        ph = u * 2.6
        pts = []
        for i in range(0, sw + 1, 6):
            t = i / sw
            band = 0.16 + 0.20 * math.sin(math.pi * t * 0.9 + 0.4)   # band width breathes
            y = (0.70 - 0.22 * t                                    # rises toward the logo
                 + 0.07 * math.sin(2 * math.pi * (t * 0.85) + ph)
                 + 0.03 * math.sin(2 * math.pi * (t * 1.9) - ph * 1.7)
                 + (u - 0.5) * band)
            pts.append((i, y * sh))
        # Middle strands brightest; warm white core, deep orange edges.
        centre = 1 - abs(u - 0.5) * 2
        col = (255, int(90 + 110 * centre), int(10 + 60 * centre))
        alpha = int(70 + 185 * centre ** 1.5)
        # Its own mask, blended in: ImageDraw overwrites pixels, so strands drawn
        # straight onto one layer would cut each other into steps where they cross.
        mask = Image.new("L", (sw, sh), 0)
        ImageDraw.Draw(mask).line(pts, fill=alpha, width=5, joint="curve")
        layer.alpha_composite(solid((sw, sh), col, mask.filter(ImageFilter.GaussianBlur(1.2))))
    # Fade in from the left so the flow comes out of the dark.
    fade = ImageChops.invert(Image.linear_gradient("L").rotate(90).resize((sw, sh)))
    fade = fade.point(lambda v: int(min(255, (v / 255) ** 0.7 * 255)))
    layer.putalpha(ImageChops.multiply(layer.getchannel("A"), fade))
    out = Image.new("RGBA", (sw, sh), (0, 0, 0, 0))
    out.alpha_composite(layer.filter(ImageFilter.GaussianBlur(sh * 0.03)))   # glow
    out.alpha_composite(layer.filter(ImageFilter.GaussianBlur(sh * 0.008)))  # haze
    out.alpha_composite(layer.filter(ImageFilter.GaussianBlur(2)))           # strands
    return out.resize(size, Image.BICUBIC)


def background(path):
    w, h = 3840, 2160
    img = Image.new("RGBA", (w, h), (6, 5, 6, 255))
    # A faint warm haze on the right, behind the logo.
    haze = Image.new("L", (w // 8, h // 8), 0)
    px = haze.load()
    for y in range(haze.height):
        for x in range(haze.width):
            d = math.hypot((x - haze.width * 0.86) / haze.width, (y - haze.height * 0.5) / haze.height)
            k = max(0.0, 1 - d / 0.7)
            px[x, y] = int(90 * k * k)
    img.alpha_composite(solid((w, h), (120, 40, 0), haze.resize((w, h), Image.BICUBIC)))
    img.alpha_composite(waves((w, h)))
    logo = load_logo(1150)
    place_logo(img, logo, w - logo.width - 170, (h - logo.height) // 2)
    img.convert("RGB").save(path)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "out")
    os.makedirs(out, exist_ok=True)
    icon(os.path.join(out, "icon.png"))
    background(os.path.join(out, "background.png"))
    print("art in", out)


if __name__ == "__main__":
    main()
