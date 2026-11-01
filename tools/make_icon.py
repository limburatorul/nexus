# Run: python tools/make_icon.py (needs Pillow). Writes res/nexus.ico, res/nexus.png and res/nexus.svg.
# Nexus in the Protagonist Labs icon family: a diagonal gradient on a rounded square with a shadowed dark glyph
# and one bright accent. The glyph is two linked diamonds, dark and white, each passing over the other once.
import io
import os
import struct
from PIL import Image, ImageDraw, ImageFilter

RES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "res")
SIZE, RADIUS = 512, 104  # the family's proportions
FROM, TO = (0x19, 0xe3, 0xff), (0x7b, 0x3d, 0xff)
DARK, LIGHT = "#170a2c", "#ffffff"
STROKE, REACH = 58, 118  # the diamonds' line width and half diagonal
LEFT, RIGHT = 180, 332  # their centres; both at mid height
MID = SIZE // 2
# Where the white line crosses back over the dark one, below the middle.
CROSS = (MID, MID + (REACH - (RIGHT - LEFT) / 2))
OVER = STROKE * 0.72


def diamond(cx):
    return [(cx, MID - REACH), (cx + REACH, MID), (cx, MID + REACH), (cx - REACH, MID)]


def over_segment():
    x, y = CROSS
    return [(x - OVER, y - OVER), (x + OVER, y + OVER)]


def svg():
    points = lambda pts: " ".join("%g,%g" % p for p in pts)  # noqa: E731
    (x1, y1), (x2, y2) = over_segment()
    return f"""<svg xmlns="http://www.w3.org/2000/svg" width="{SIZE}" height="{SIZE}" viewBox="0 0 {SIZE} {SIZE}"><defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#{'%02x%02x%02x' % FROM}"/><stop offset="1" stop-color="#{'%02x%02x%02x' % TO}"/></linearGradient><filter id="s" x="-20%" y="-20%" width="140%" height="140%"><feDropShadow dx="0" dy="10" stdDeviation="11" flood-color="#000" flood-opacity=".35"/></filter></defs><rect width="{SIZE}" height="{SIZE}" rx="{RADIUS}" fill="url(#g)"/><g fill="none" stroke-width="{STROKE}" stroke-linejoin="round" filter="url(#s)"><polygon points="{points(diamond(RIGHT))}" stroke="{LIGHT}"/><polygon points="{points(diamond(LEFT))}" stroke="{DARK}"/><line x1="{x1:g}" y1="{y1:g}" x2="{x2:g}" y2="{y2:g}" stroke="{LIGHT}"/></g></svg>
"""


def raster():
    k = 4
    n = SIZE * k
    # The gradient runs along the diagonal, drawn small and enlarged.
    small = Image.new("RGB", (64, 64))
    small.putdata([tuple(int(FROM[c] + (TO[c] - FROM[c]) * (x + y) / 126) for c in range(3))
                   for y in range(64) for x in range(64)])
    bg = small.resize((n, n), Image.BICUBIC)
    mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, n - 1, n - 1), radius=RADIUS * k, fill=255)
    im = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    im.paste(bg, (0, 0), mask)

    glyph = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(glyph)
    for cx, colour in ((RIGHT, LIGHT), (LEFT, DARK)):
        pts = [(x * k, y * k) for x, y in diamond(cx)]
        d.line(pts + pts[:2], fill=colour, width=STROKE * k, joint="curve")
    d.line([(x * k, y * k) for x, y in over_segment()], fill=LIGHT, width=STROKE * k)

    shadow = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    shadow.putalpha(glyph.getchannel("A").point(lambda a: int(a * 0.35)))
    shadow = shadow.filter(ImageFilter.GaussianBlur(11 * k))
    im.alpha_composite(shadow, (0, 10 * k))
    im.alpha_composite(glyph)
    # The shadow must not spill past the rounded square.
    im.putalpha(Image.composite(im.getchannel("A"), Image.new("L", (n, n), 0), mask))
    return im.resize((SIZE, SIZE), Image.LANCZOS)


def write_ico(im, path):
    # Written by hand: Pillow stores every size as PNG, which Windows XP cannot show (no title bar or tray
    # icon there). Sizes up to 64 go in as 32-bit BMP with alpha, which XP draws; 256 stays PNG for Vista+.
    frames = []
    for size in (16, 20, 24, 32, 40, 48, 64, 256):
        px = im.resize((size, size), Image.LANCZOS)
        if size == 256:
            buf = io.BytesIO()
            px.save(buf, "PNG")
            frames.append((size, buf.getvalue()))
            continue
        rows = []
        for y in range(size - 1, -1, -1):  # bottom-up, BGRA
            for x in range(size):
                r, g, b, a = px.getpixel((x, y))
                rows.append(bytes((b, g, r, a)))
        mask_row = ((size + 31) // 32) * 4
        data = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0) + b"".join(rows) + bytes(mask_row * size)
        frames.append((size, data))
    with open(path, "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, len(frames)))
        offset = 6 + 16 * len(frames)
        for size, data in frames:
            f.write(struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset))
            offset += len(data)
        for _, data in frames:
            f.write(data)


im = raster()
im.save(os.path.join(RES, "nexus.png"), optimize=True)
write_ico(im, os.path.join(RES, "nexus.ico"))
with open(os.path.join(RES, "nexus.svg"), "w", newline="\n") as f:
    f.write(svg())
print("res/nexus.ico, nexus.png, nexus.svg")
