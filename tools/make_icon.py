# Run: python tools/make_icon.py res/nexus.ico (needs Pillow).
# Nexus icon: two open diamonds linked like a chain, violet over cyan (redrawn from the user's artwork).
import sys
from PIL import Image, ImageDraw
out = sys.argv[1]
N = 1024
k = N / 362  # the artwork was drawn on a 362 px square

def P(x, y): return (x * k, y * k)
def lerp(a, b, t): return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)

# Background: rounded square, dark violet at the top fading to dark teal at the bottom.
bg = Image.new("RGBA", (N, N))
top, bottom = (33, 27, 46), (14, 40, 44)
for y in range(N):
    t = y / (N - 1)
    ImageDraw.Draw(bg).line([(0, y), (N, y)], fill=tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)))
mask = Image.new("L", (N, N), 0)
ImageDraw.Draw(mask).rounded_rectangle((16, 16, N - 16, N - 16), radius=190, fill=255)
im = Image.new("RGBA", (N, N), (0, 0, 0, 0))
im.paste(bg, (0, 0), mask)

T, R, B, L = (180, 58), (258, 136), (180, 214), (102, 136)       # violet diamond
A, C = lerp(R, B, 0.42), lerp(B, R, 0.24)                          # its gap, where cyan passes over
violet = [A, R, T, L, B, C]
cyan = [(360 - x, 364 - y) for x, y in violet]                     # the same shape turned half round
w = int(27 * k)
d = ImageDraw.Draw(im)
for path, colour in ((cyan, "#19d6f2"), (violet, "#9d5cf0")):
    pts = [P(*p) for p in path]
    d.line(pts, fill=colour, width=w, joint="curve")
# Written by hand: Pillow stores every size as PNG, which Windows XP cannot show (no title bar or tray
# icon there). Sizes up to 64 go in as 32-bit BMP with alpha, which XP draws; 256 stays PNG for Vista+.
import io, struct
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
with open(out, "wb") as f:
    f.write(struct.pack("<HHH", 0, 1, len(frames)))
    offset = 6 + 16 * len(frames)
    for size, data in frames:
        f.write(struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset))
        offset += len(data)
    for _, data in frames:
        f.write(data)


