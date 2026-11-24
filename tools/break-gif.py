"""Builds docs/break.gif, the animation at the top of the README (needs Python and Pillow).

The break screen and the warning are Pauserick's own drawing: the test build renders them
into bitmaps, without showing any window. Only the desktop behind them is made up here: a
neutral wallpaper, a text editor and a taskbar with the tray icon.

  PauserickTest.exe --config <scratch.ini> --render <frames dir> 1920 1080 144
  python tools/break-gif.py <frames dir> src/pauserick.ico docs/break.gif

The scratch ini picks the language ([General] Language=0 for English).
"""
import sys
from PIL import Image, ImageDraw, ImageFont

FRAMES, ICO, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
W, H, SCALE = 1920, 1080, 1.5          # a 1920x1080 monitor at 150 % scaling
OUT_W, OUT_H = 1280, 720
TASKBAR = int(48 * SCALE)
FONTS = "C:/Windows/Fonts/"


def font(name, px):
    return ImageFont.truetype(FONTS + name, int(px))


def px(v):
    return int(round(v * SCALE))


def desktop():
    im = Image.new("RGB", (W, H), (38, 70, 83))
    d = ImageDraw.Draw(im)
    # two soft, flat shapes on the wallpaper (flat colours keep the GIF small)
    d.ellipse((1100, -500, 2600, 900), fill=(42, 80, 94))
    d.ellipse((-600, 500, 900, 1700), fill=(34, 62, 74))

    # a text editor window
    x0, y0, x1, y1 = px(170), px(70), px(900), px(520)
    d.rounded_rectangle((x0 + 6, y0 + 10, x1 + 6, y1 + 12), px(8), fill=(30, 55, 65))   # shadow
    d.rounded_rectangle((x0, y0, x1, y1), px(8), fill=(251, 251, 251), outline=(200, 200, 200))
    d.rectangle((x0 + 1, y0 + px(8), x1 - 1, y0 + px(34)), fill=(243, 243, 243))
    d.rounded_rectangle((x0 + 1, y0 + 1, x1 - 1, y0 + px(34)), px(8), fill=(243, 243, 243))
    d.line((x0 + 1, y0 + px(34), x1 - 1, y0 + px(34)), fill=(225, 225, 225), width=1)
    d.text((x0 + px(16), y0 + px(9)), "notes.txt - Editor", font=font("segoeui.ttf", px(12)), fill=(30, 30, 30))
    cy = y0 + px(17)
    for i, kind in enumerate(("close", "max", "min")):
        cx = x1 - px(23) - i * px(46)
        if kind == "close":
            d.line((cx - px(5), cy - px(5), cx + px(5), cy + px(5)), fill=(40, 40, 40), width=2)
            d.line((cx - px(5), cy + px(5), cx + px(5), cy - px(5)), fill=(40, 40, 40), width=2)
        elif kind == "max":
            d.rectangle((cx - px(5), cy - px(5), cx + px(5), cy + px(5)), outline=(40, 40, 40), width=2)
        else:
            d.line((cx - px(5), cy, cx + px(5), cy), fill=(40, 40, 40), width=2)

    body = font("segoeui.ttf", px(15))
    head = font("segoeuib.ttf", px(20))
    lines = [
        (head, "Weekly notes"),
        None,
        (body, "- Draft the release notes for the next version"),
        (body, "- Go through the open issues and close the stale ones"),
        (body, "- Retake the screenshots for the README"),
        (body, "- Ask for feedback on the new settings window"),
        None,
        (head, "Ideas"),
        None,
        (body, "Keep every option explained in one short line, so that"),
        (body, "nobody needs a manual. Defaults should be good enough"),
        (body, "for most people: a mini break every 30 minutes and a"),
        (body, "longer break every hour."),
    ]
    y = y0 + px(56)
    for ln in lines:
        if ln is None:
            y += px(10)
            continue
        f, t = ln
        d.text((x0 + px(28), y), t, font=f, fill=(32, 32, 32))
        y += px(26) if f is body else px(30)
    d.line((x0 + px(28) + int(d.textlength("longer break every hour.", font=body)) + 3, y - px(26) + 4,
            x0 + px(28) + int(d.textlength("longer break every hour.", font=body)) + 3, y - px(26) + px(21)),
           fill=(20, 20, 20), width=2)                                                # caret

    # taskbar: clock and the Pauserick tray icon, nothing else
    d.rectangle((0, H - TASKBAR, W, H), fill=(32, 32, 32))
    d.line((0, H - TASKBAR, W, H - TASKBAR), fill=(60, 60, 60), width=1)
    small = font("segoeui.ttf", px(12))
    for txt, dy in (("10:29", px(7)), ("24/11/2026", px(25))):
        tw = d.textlength(txt, font=small)
        d.text((W - px(14) - tw, H - TASKBAR + dy), txt, font=small, fill=(235, 235, 235))
    icon = Image.open(ICO)
    icon.size = max(icon.info.get("sizes", [icon.size]))
    icon = icon.convert("RGBA").resize((px(16), px(16)), Image.LANCZOS)
    im.paste(icon, (W - px(110), H - TASKBAR + (TASKBAR - px(16)) // 2), icon)
    return im


def load(name):
    return Image.open(f"{FRAMES}/{name}").convert("RGB")


base = desktop()
warn_pos = (W - px(250) - px(16), H - TASKBAR - px(54) - px(16))   # warn_show(): work area corner


def with_warning(s):
    im = base.copy()
    im.paste(load(f"warn_{s}.bmp"), warn_pos)
    return im


seq = [(base, 1200)]
seq += [(with_warning(s), 800) for s in range(5, 0, -1)]
seq += [(load("break_30.bmp"), 1000), (load("break_29.bmp"), 1000)]
seq += [(load(f"break_{s:02d}.bmp"), 90) for s in range(28, 1, -1)]
seq += [(load("break_01.bmp"), 800), (base, 2200)]

frames = [im.resize((OUT_W, OUT_H), Image.LANCZOS) for im, _ in seq]
durations = [ms for _, ms in seq]

# one palette for every frame, taken from a desktop frame with the warning and a break frame
# Desktop frames: a palette of their own, plus the warning's accent colour, which covers too
# few pixels to survive quantizing. Break frames: pure greys (the black screen's text is grey;
# ClearType's colour fringes, invisible on a real screen, would only add noise).
# One palette for all frames (no palette switches between frames): 188 desktop colours,
# 4 exact warning colours, 64 greys.
desk = frames[1].quantize(colors=188, method=Image.Quantize.MEDIANCUT).getpalette()[:188 * 3]
accent = [15, 118, 110, 28, 28, 28, 240, 240, 240, 32, 32, 32]
greys = [c for i in range(64) for c in (i * 255 // 63,) * 3]
pal = Image.new("P", (1, 1))
pal.putpalette(desk + accent + greys)
pframes = []
for (src, _), f in zip(seq, frames):
    if src.getpixel((10, 10)) == (0, 0, 0):   # break screen
        f = f.convert("L").convert("RGB")
    pframes.append(f.quantize(palette=pal, dither=Image.Dither.NONE))
pframes[0].save(OUT, save_all=True, append_images=pframes[1:], duration=durations, loop=0,
                optimize=False, disposal=1)
print(OUT, sum(durations) / 1000, "s,", len(pframes), "frames")
