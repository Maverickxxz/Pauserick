"""Turns GitHub's mark (the 16 x 16 "mark-github" octicon) into the polygon that the
settings window draws in its header: GH_MARK in src/pauserick.c.

    python tools/github-mark.py              prints the C array
    python tools/github-mark.py preview.png  also draws it, large and at 16 px (needs Pillow)

The path is one closed outline: the circle, with the cat cut in from its bottom edge.
Curves are split until they stay within TOL of the straight lines that replace them.
"""
import math
import sys

PATH = ("M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49"
        "-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 "
        "1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.06-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59"
        ".82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27.68 0 1.36.09 2 .27 "
        "1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 "
        "3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.013 8.013 0 0016 8"
        "c0-4.42-3.58-8-8-8z")
TOL = 0.02  # in units of the 16 x 16 box: 0.06 px even at 300 % scaling (48 px)
SCALE = 100  # the C array holds hundredths of a unit


class Reader:
    def __init__(self, s):
        self.s, self.i = s, 0

    def skip(self):
        while self.i < len(self.s) and self.s[self.i] in " ,\t\n":
            self.i += 1

    def command(self):
        self.skip()
        if self.i < len(self.s) and self.s[self.i].isalpha():
            self.i += 1
            return self.s[self.i - 1]
        return None  # more numbers: the previous command repeats

    def done(self):
        self.skip()
        return self.i >= len(self.s)

    def number(self):
        self.skip()
        j = self.i
        if self.s[j] in "+-":
            j += 1
        dot = False
        while j < len(self.s) and (self.s[j].isdigit() or (self.s[j] == "." and not dot)):
            dot |= self.s[j] == "."
            j += 1
        v = float(self.s[self.i:j])
        self.i = j
        return v

    def flag(self):  # arc flags are single digits, often written with no separator ("0016")
        self.skip()
        self.i += 1
        return self.s[self.i - 1] == "1"


def cubic(p0, p1, p2, p3, out):
    def flat(a, b, c, d):
        ux, uy = d[0] - a[0], d[1] - a[1]
        n = math.hypot(ux, uy) or 1e-12
        return max(abs((q[0] - a[0]) * uy - (q[1] - a[1]) * ux) / n for q in (b, c)) <= TOL

    def mid(a, b):
        return ((a[0] + b[0]) / 2, (a[1] + b[1]) / 2)

    def split(a, b, c, d):
        if flat(a, b, c, d):
            out.append(d)
            return
        ab, bc, cd = mid(a, b), mid(b, c), mid(c, d)
        abc, bcd = mid(ab, bc), mid(bc, cd)
        m = mid(abc, bcd)
        split(a, ab, abc, m)
        split(m, bcd, cd, d)

    split(p0, p1, p2, p3)


def arc(p0, rx, ry, rot, large, sweep, p1, out):
    """SVG endpoint arc -> centre form (SVG 1.1, appendix F.6.5), then sampled."""
    phi = math.radians(rot)
    cp, sp = math.cos(phi), math.sin(phi)
    dx, dy = (p0[0] - p1[0]) / 2, (p0[1] - p1[1]) / 2
    x1, y1 = cp * dx + sp * dy, -sp * dx + cp * dy
    lam = (x1 / rx) ** 2 + (y1 / ry) ** 2
    if lam > 1:
        rx, ry = rx * math.sqrt(lam), ry * math.sqrt(lam)
    num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1
    den = rx * rx * y1 * y1 + ry * ry * x1 * x1
    k = math.sqrt(max(0.0, num / den)) * (-1 if large == sweep else 1)
    cx1, cy1 = k * rx * y1 / ry, -k * ry * x1 / rx
    cx = cp * cx1 - sp * cy1 + (p0[0] + p1[0]) / 2
    cy = sp * cx1 + cp * cy1 + (p0[1] + p1[1]) / 2
    t0 = math.atan2((y1 - cy1) / ry, (x1 - cx1) / rx)
    t1 = math.atan2((-y1 - cy1) / ry, (-x1 - cx1) / rx)
    dt = t1 - t0
    if sweep and dt < 0:
        dt += 2 * math.pi
    if not sweep and dt > 0:
        dt -= 2 * math.pi
    step = 2 * math.acos(1 - TOL / max(rx, ry))  # chord error within TOL
    n = max(1, math.ceil(abs(dt) / step))
    for i in range(1, n + 1):
        t = t0 + dt * i / n
        x, y = rx * math.cos(t), ry * math.sin(t)
        out.append((cp * x - sp * y + cx, sp * x + cp * y + cy))
    out[-1] = p1


def flatten(path):
    r = Reader(path)
    pts, cur, start, cmd, last_ctrl = [], (0.0, 0.0), (0.0, 0.0), None, None
    while not r.done():
        c = r.command() or cmd
        rel = c.islower()
        u = c.upper()
        base = cur if rel else (0.0, 0.0)

        def pt():
            x = r.number()
            return (base[0] + x, base[1] + r.number())

        if u == "M":
            cur = start = pt()
            pts.append(cur)
            cmd = "l" if rel else "L"  # numbers after a moveto are linetos
            last_ctrl = None
            continue
        if u == "L":
            cur = pt()
            pts.append(cur)
            last_ctrl = None
        elif u == "C":
            p1, p2, p3 = pt(), pt(), pt()
            cubic(cur, p1, p2, p3, pts)
            cur, last_ctrl = p3, p2
        elif u == "S":
            p1 = (2 * cur[0] - last_ctrl[0], 2 * cur[1] - last_ctrl[1]) if last_ctrl else cur
            p2, p3 = pt(), pt()
            cubic(cur, p1, p2, p3, pts)
            cur, last_ctrl = p3, p2
        elif u == "A":
            rx, ry, rot = r.number(), r.number(), r.number()
            large, sweep = r.flag(), r.flag()
            p1 = pt()
            arc(cur, rx, ry, rot, large, sweep, p1, pts)
            cur, last_ctrl = p1, None
        elif u == "Z":
            cur, last_ctrl = start, None
        else:
            raise ValueError("path command not handled: " + c)
        cmd = c
    return pts


def compact(pts):
    q = [(round(x * SCALE), round(y * SCALE)) for x, y in pts]
    out = []
    for p in q:
        if not out or p != out[-1]:
            out.append(p)
    if len(out) > 1 and out[0] == out[-1]:
        out.pop()
    changed = True
    while changed:  # drop points in a straight line with their neighbours
        changed = False
        for i in range(len(out)):
            a, b, c = out[i - 1], out[i], out[(i + 1) % len(out)]
            if (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]) == 0:
                del out[i]
                changed = True
                break
    return out


def preview(poly, path):
    from PIL import Image, ImageDraw
    big = Image.new("L", (512, 512), 255)
    ImageDraw.Draw(big).polygon([(x * 512 / 1600, y * 512 / 1600) for x, y in poly], fill=0)
    small = Image.new("L", (64, 64), 255)
    ImageDraw.Draw(small).polygon([(x * 64 / 1600, y * 64 / 1600) for x, y in poly], fill=0)
    sheet = Image.new("L", (512 + 32 + 64, 512), 255)
    sheet.paste(big, (0, 0))
    sheet.paste(small.resize((16, 16), Image.LANCZOS).resize((64, 64), Image.NEAREST), (544, 0))
    sheet.save(path)


def main():
    poly = compact(flatten(PATH))
    rows = ["    " + ", ".join("{%d, %d}" % p for p in poly[i:i + 8]) + "," for i in range(0, len(poly), 8)]
    print("static const short GH_MARK[][2] = { /* %d points, tools/github-mark.py */" % len(poly))
    print("\n".join(rows))
    print("};")
    if len(sys.argv) > 1:
        preview(poly, sys.argv[1])


if __name__ == "__main__":
    main()
