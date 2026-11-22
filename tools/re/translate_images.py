"""Replace the German text in the original images with English text (Pillow).

Recreates docs/images/*.png from the German originals in docs/images/de/:

  python3 tools/re/translate_images.py docs/images/de/esp8266-wiring-plan-0.png docs/images/esp8266-wiring-plan-0.png plan0
  python3 tools/re/translate_images.py docs/images/de/esp8266-wiring-plan-3000.png docs/images/esp8266-wiring-plan-3000.png plan3000
  python3 tools/re/translate_images.py docs/images/de/grafana-16-cells.png docs/images/grafana-16-cells.png grafana
  python3 tools/re/translate_images.py docs/images/de/esp8266-webui.png docs/images/esp8266-webui.png webpage

Fonts: DejaVu Sans Bold and Liberation Sans/Serif (fonts-dejavu, fonts-liberation).
"""
import sys
from PIL import Image, ImageDraw, ImageFont

SRC, DST = sys.argv[1], sys.argv[2]
DEJAVU_B = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
LIB_B = "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"
NAVY = (0, 60, 85)
RED = (237, 28, 36)
DARKRED = (176, 0, 0)
BLACK = (0, 0, 0)
WHITE = (255, 255, 255)


def font(path, size):
    return ImageFont.truetype(path, size)


def fit(draw, text, path, max_w, max_size):
    size = max_size
    while size > 6:
        f = font(path, size)
        if draw.textlength(text, font=f) <= max_w:
            return f
        size -= 1
    return font(path, 6)


def erase(d, box, fill=WHITE):
    d.rectangle(box, fill=fill)


def text_center(d, cx, top, text, f, color, underline=False):
    w = d.textlength(text, font=f)
    x = cx - w / 2
    d.text((x, top), text, font=f, fill=color)
    if underline:
        asc, desc = f.getmetrics()
        y = top + asc + 2
        d.line((x, y, x + w, y), fill=color, width=2)


def text_left(d, x, top, text, f, color, underline=False):
    d.text((x, top), text, font=f, fill=color)
    if underline:
        w = d.textlength(text, font=f)
        asc, _ = f.getmetrics()
        y = top + asc + 2
        d.line((x, y, x + w, y), fill=color, width=2)


def cap_top(f, box_top):
    """y for d.text() so that the cap top of the glyphs lands on box_top."""
    return box_top - f.getbbox("M")[1]


def plan0(im):
    d = ImageDraw.Draw(im)
    # MODUL -> MODULE
    erase(d, (390, 12, 522, 44))
    f = font(DEJAVU_B, 31)
    text_center(d, 455, cap_top(f, 16), "MODULE", f, NAVY)
    # ODER -> OR
    erase(d, (402, 300, 520, 338))
    text_center(d, 460, cap_top(f, 308), "OR", f, NAVY)
    # RS485 Modul 1/2 -> Module
    f = font(LIB_B, 14)
    for top in (167, 483):
        erase(d, (3, top - 3, 140, top + 13))
        text_left(d, 5, cap_top(f, top), "RS485 Module " + ("1" if top < 300 else "2"), f, NAVY)
    # warning block
    erase(d, (508, 253, 958, 375))
    big = font(LIB_B, 25)
    mid = font(LIB_B, 19)
    y = 257
    text_left(d, 511, cap_top(big, y), "!!! WARNING !!!", big, RED, underline=True)
    w = d.textlength("!!! WARNING !!! ", font=big)
    text_left(d, 511 + w, cap_top(mid, y + 4), "Some makers had RS485", mid, RED)
    text_left(d, 512, cap_top(mid, 291), "problems with ESP dev boards combined", mid, RED)
    text_left(d, 512, cap_top(mid, 321), "with RS485 module 1!", mid, RED)
    text_left(d, 511, cap_top(big, 348), "Use RS485 module 2!", big, RED, underline=True)
    fix_labels(d, 595, 98, 682, 89, 616, 232)


def fix_labels(d, rx, ry, hx, hy, tx, ty):
    """XY-017 annotations: fix typo and machine translations (positions measured per image)."""
    f = font(LIB_B, 11)
    # "Reveive Indicators" -> "Receive Indicators"
    erase(d, (rx - 1, ry - 1, rx + 113, ry + 9))
    text_left(d, rx, cap_top(f, ry), "Receive Indicators", f, DARKRED)
    # "Heat recovery fuse" -> "Resettable fuse (PTC)"
    erase(d, (hx - 2, hy - 1, hx + 108, hy + 11))
    text_left(d, hx, cap_top(f, hy), "Resettable fuse (PTC)", f, DARKRED)
    # second line "Inhibit Diodes" -> "protection diodes" (first line "TVS" stays)
    erase(d, (tx - 40, ty - 4, tx + 44, ty + 11))
    text_center(d, tx, cap_top(f, ty), "protection diodes", f, DARKRED)


def plan3000(im):
    d = ImageDraw.Draw(im)
    # title
    erase(d, (55, 22, 935, 60))
    f = fit(d, "DIY Wiring Plan 3000 - Seplos Controller by BavarianSuperGuy", DEJAVU_B, 800, 24)
    title = "DIY Wiring Plan 3000 - Seplos Controller by BavarianSuperGuy"
    tw = d.textlength(title, font=f)
    small = font(DEJAVU_B, 11)
    total = tw + 4 + d.textlength("v1.0", font=small)
    x = 492 - total / 2
    d.text((x, cap_top(f, 33)), title, font=f, fill=NAVY)
    d.text((x + tw + 4, cap_top(small, 44)), "v1.0", font=small, fill=BLACK)
    # MODUL / ODER
    erase(d, (400, 70, 540, 104))
    f = font(DEJAVU_B, 31)
    text_center(d, 470, cap_top(f, 76), "MODULE", f, NAVY)
    erase(d, (410, 372, 528, 406))
    text_center(d, 468, cap_top(f, 377), "OR", f, NAVY)
    # tested with module 1
    erase(d, (428, 106, 830, 127))
    f = font(LIB_B, 16)
    text_center(d, 625, cap_top(f, 110), "ESP Seplos Controller tested with this module 1", f, RED)
    # RS485 Modul 1/2
    f = font(LIB_B, 14)
    for top, n in ((231, "1"), (557, "2")):
        erase(d, (3, top - 3, 140, top + 13))
        text_left(d, 5, cap_top(f, top), "RS485 Module " + n, f, NAVY)
    # address note
    erase(d, (780, 678, 984, 832))
    f = font(LIB_B, 14)
    lines = ["Currently the ESP", "Seplos Controller", "only talks to BMS", "address 0!", "Set all DIP", "switches OFF,", "please."]
    for i, line in enumerate(lines):
        text_left(d, 787, cap_top(f, 683 + i * 21), line, f, BLACK)
    fix_labels(d, 611, 160, 703, 151, 635, 299)


def grafana(im):
    d = ImageDraw.Draw(im)
    bg = im.load()[880, 12]
    erase(d, (884, 5, 940, 19), fill=bg)
    f = font(LIB_B, 13)
    text_center(d, 911, cap_top(f, 7), "16 cells", f, (199, 208, 217))


def webpage(im):
    d = ImageDraw.Draw(im)
    # "Online seit" -> "Online since" (Times-like serif, as rendered by the browser)
    erase(d, (12, 189, 78, 202))
    f = ImageFont.truetype("/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf", 11)
    d.text((13, cap_top(f, 191)), "Online since", font=f, fill=BLACK)


JOBS = {"plan0": plan0, "plan3000": plan3000, "grafana": grafana, "webpage": webpage}

im = Image.open(SRC)
mode = im.mode
im = im.convert("RGBA" if mode == "RGBA" else "RGB")
JOBS[sys.argv[3]](im)
im.save(DST, optimize=True)
