"""app.ico 를 만든다: 강조색 둥근 사각형 위의 흰 자물쇠. 외부 라이브러리 없이 거리 함수로 그린다."""
import struct, sys, zlib, math

ACC = (15, 108, 189)

def rrect(px, py, cx, cy, hw, hh, r):
    dx = max(abs(px - cx) - (hw - r), 0.0); dy = max(abs(py - cy) - (hh - r), 0.0)
    inside = min(max(abs(px - cx) - (hw - r), abs(py - cy) - (hh - r)), 0.0)
    return math.hypot(dx, dy) + inside - r          # < 0 안쪽

def shape(x, y):
    """(배경 안쪽?, 흰색?) — 좌표는 0..1"""
    bg = rrect(x, y, 0.5, 0.5, 0.46, 0.46, 0.20) < 0
    body = rrect(x, y, 0.5, 0.635, 0.215, 0.165, 0.045) < 0
    ring = abs(math.hypot(x - 0.5, y - 0.40) - 0.135) < 0.036 and y <= 0.40
    legs = (abs(abs(x - 0.5) - 0.135) < 0.036) and 0.40 <= y <= 0.50
    hole = math.hypot(x - 0.5, y - 0.61) < 0.045 or (abs(x - 0.5) < 0.020 and 0.61 <= y <= 0.715)
    return bg, (body or ring or legs) and not hole

def render(n, ss=6):
    out = bytearray()
    for j in range(n):
        for i in range(n):
            a = w = 0
            for sj in range(ss):
                for si in range(ss):
                    bg, white = shape((i + (si + .5) / ss) / n, (j + (sj + .5) / ss) / n)
                    a += bg; w += bg and white
            t = ss * ss
            if a == 0:
                out += bytes((0, 0, 0, 0)); continue
            f = w / a
            r, g, b = (round(ACC[k] * (1 - f) + 255 * f) for k in range(3))
            out += bytes((r, g, b, round(255 * a / t)))
    return bytes(out)

def png(n, rgba):
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)
    raw = b"".join(b"\0" + rgba[y * n * 4:(y + 1) * n * 4] for y in range(n))
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", n, n, 8, 6, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")

def dib(n, rgba):
    """ICO 안의 32비트 BMP: 높이는 두 배(XOR + AND), 아래 줄부터, BGRA."""
    hdr = struct.pack("<IiiHHIIiiII", 40, n, n * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    xor = bytearray()
    for y in range(n - 1, -1, -1):
        row = rgba[y * n * 4:(y + 1) * n * 4]
        for x in range(n):
            r, g, b, a = row[x * 4:x * 4 + 4]
            xor += bytes((b, g, r, a))
    stride = ((n + 31) // 32) * 4
    return hdr + bytes(xor) + bytes(stride * n)

sizes = [16, 20, 24, 32, 40, 48, 64, 256]
images = []
for n in sizes:
    rgba = render(n)
    images.append(png(n, rgba) if n == 256 else dib(n, rgba))
    if n in (256, 32, 16):
        open(sys.argv[2] + "/icon%d.png" % n, "wb").write(png(n, rgba))
off = 6 + 16 * len(sizes)
ico = struct.pack("<HHH", 0, 1, len(sizes))
for n, img in zip(sizes, images):
    ico += struct.pack("<BBBBHHII", n % 256, n % 256, 0, 0, 1, 32, len(img), off); off += len(img)
open(sys.argv[1], "wb").write(ico + b"".join(images))
