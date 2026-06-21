#!/usr/bin/env python3
"""
T-Dongle-C5 display layout simulator + 5x7 font source of truth.

Renders the 0.96" ST7735 (80x160 px) screen exactly as the firmware will,
using the SAME 5x7 bitmap font, so we can validate offline that the values
from the web-UI screenshot (SOC, charging, power, current, WiFi/BLE/MQTT)
fit and stay legible at 80 px width — BEFORE we have hardware to flash.

Two outputs (stdlib only, no Pillow):
  python3 tools/display_sim.py png  [out.png]   -> upscaled preview PNG
  python3 tools/display_sim.py cheader [out.h]  -> main/display_font.h

The font dict below is authored as 7-row x 5-col ASCII art ('#'=on). The
C-header generator packs each glyph into 5 column bytes (bit r = row r,
0=top), which is what display.cpp consumes.
"""
import sys, zlib, struct

# ── 5x7 font, authored as 7 rows of 5 chars ('#'=lit, '.'=off) ────────────────
G = {
' ': ["....." ]*7,
'0': [".###.","#...#","#..##","#.#.#","##..#","#...#",".###."],
'1': ["..#..",".##..","..#..","..#..","..#..","..#..",".###."],
'2': [".###.","#...#","....#","..##.",".#...","#....","#####"],
'3': ["#####","...#.","..##.","...#.","....#","#...#",".###."],
'4': ["...#.","..##.",".#.#.","#..#.","#####","...#.","...#."],
'5': ["#####","#....","####.","....#","....#","#...#",".###."],
'6': ["..##.",".#...","#....","####.","#...#","#...#",".###."],
'7': ["#####","....#","...#.","..#..",".#...",".#...",".#..."],
'8': [".###.","#...#","#...#",".###.","#...#","#...#",".###."],
'9': [".###.","#...#","#...#",".####","....#","...#.",".##.."],
'A': [".###.","#...#","#...#","#####","#...#","#...#","#...#"],
'B': ["####.","#...#","#...#","####.","#...#","#...#","####."],
'C': [".###.","#...#","#....","#....","#....","#...#",".###."],
'D': ["###..","#..#.","#...#","#...#","#...#","#..#.","###.."],
'E': ["#####","#....","#....","####.","#....","#....","#####"],
'F': ["#####","#....","#....","####.","#....","#....","#...."],
'G': [".###.","#...#","#....","#.###","#...#","#...#",".###."],
'H': ["#...#","#...#","#...#","#####","#...#","#...#","#...#"],
'I': [".###.","..#..","..#..","..#..","..#..","..#..",".###."],
'J': ["..###","...#.","...#.","...#.","#..#.","#..#.",".##.."],
'K': ["#...#","#..#.","#.#..","##...","#.#..","#..#.","#...#"],
'L': ["#....","#....","#....","#....","#....","#....","#####"],
'M': ["#...#","##.##","#.#.#","#.#.#","#...#","#...#","#...#"],
'N': ["#...#","##..#","#.#.#","#..##","#...#","#...#","#...#"],
'O': [".###.","#...#","#...#","#...#","#...#","#...#",".###."],
'P': ["####.","#...#","#...#","####.","#....","#....","#...."],
'Q': [".###.","#...#","#...#","#...#","#.#.#","#..#.",".##.#"],
'R': ["####.","#...#","#...#","####.","#.#..","#..#.","#...#"],
'S': [".###.","#...#","#....",".###.","....#","#...#",".###."],
'T': ["#####","..#..","..#..","..#..","..#..","..#..","..#.."],
'U': ["#...#","#...#","#...#","#...#","#...#","#...#",".###."],
'V': ["#...#","#...#","#...#","#...#","#...#",".#.#.","..#.."],
'W': ["#...#","#...#","#...#","#.#.#","#.#.#","##.##","#...#"],
'X': ["#...#","#...#",".#.#.","..#..",".#.#.","#...#","#...#"],
'Y': ["#...#","#...#",".#.#.","..#..","..#..","..#..","..#.."],
'Z': ["#####","....#","...#.","..#..",".#...","#....","#####"],
'a': [".....",".....",".###.","....#",".####","#...#",".####"],
'b': ["#....","#....","####.","#...#","#...#","#...#","####."],
'c': [".....",".....",".###.","#...#","#....","#...#",".###."],
'd': ["....#","....#",".####","#...#","#...#","#...#",".####"],
'e': [".....",".....",".###.","#...#","#####","#....",".###."],
'f': ["..##.",".#..#",".#...","###..",".#...",".#...",".#..."],
'g': [".....",".####","#...#","#...#",".####","....#",".###."],
'h': ["#....","#....","####.","#...#","#...#","#...#","#...#"],
'i': ["..#..",".....",".##..","..#..","..#..","..#..",".###."],
'j': ["...#.",".....","..##.","...#.","...#.","#..#.",".##.."],
'k': ["#....","#....","#..#.","#.#..","##...","#.#..","#..#."],
'l': [".##..","..#..","..#..","..#..","..#..","..#..",".###."],
'm': [".....",".....","##.#.","#.#.#","#.#.#","#...#","#...#"],
'n': [".....",".....","####.","#...#","#...#","#...#","#...#"],
'o': [".....",".....",".###.","#...#","#...#","#...#",".###."],
'p': [".....","####.","#...#","#...#","####.","#....","#...."],
'q': [".....",".####","#...#","#...#",".####","....#","....#"],
'r': [".....",".....","#.##.","##..#","#....","#....","#...."],
's': [".....",".....",".####","#....",".###.","....#","####."],
't': [".#...",".#...","###..",".#...",".#...",".#..#","..##."],
'u': [".....",".....","#...#","#...#","#...#","#..##",".##.#"],
'v': [".....",".....","#...#","#...#","#...#",".#.#.","..#.."],
'w': [".....",".....","#...#","#...#","#.#.#","#.#.#",".#.#."],
'x': [".....",".....","#...#",".#.#.","..#..",".#.#.","#...#"],
'y': [".....","#...#","#...#","#...#",".####","....#",".###."],
'z': [".....",".....","#####","...#.","..#..",".#...","#####"],
'.': [".....",".....",".....",".....",".....",".##..",".##.."],
':': [".....",".##..",".##..",".....",".##..",".##..","....."],
'-': [".....",".....",".....","#####",".....",".....","....."],
'/': ["....#","...#.","..#..","..#..",".#...","#....","#...."],
'%': ["##..#","##.#.","..#..",".#...","#..##","#.#.#","...##"],
'+': [".....","..#..","..#..","#####","..#..","..#..","....."],
'(': ["..##.",".#...","#....","#....","#....",".#...","..##."],
')': [".##..","...#.","....#","....#","....#","...#.",".##.."],
'~': [".....",".....","....."]+[".....",".....",".....","....."],  # placeholder
}
# lightning bolt, addressed as key '`'
G['`'] = ["..##.",".##..","#####","..##.",".##..","#....","#...."]

def glyph(ch):
    return G.get(ch, ["#####","#...#","#...#","#...#","#...#","#...#","#####"] if ch != ' ' else G[' '])

# ── tiny stdlib PNG writer ────────────────────────────────────────────────────
def write_png(path, w, h, rgb):
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for x in range(w):
            raw += bytes(rgb[y][x])
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)

# ── logical 80x160 canvas ─────────────────────────────────────────────────────
W, H = 80, 160
BG    = (244, 245, 247)
CARD  = (255, 255, 255)
INK   = (27, 27, 27)
GREY  = (140, 146, 154)
GREEN = (47, 158, 68)
GREENL= (200, 230, 208)

canvas = [[BG for _ in range(W)] for _ in range(H)]

def px(x, y, col):
    if 0 <= x < W and 0 <= y < H:
        canvas[y][x] = col

def rrect(x0, y0, x1, y1, col):
    for y in range(y0, y1):
        for x in range(x0, x1):
            px(x, y, col)

def text(x, y, s, col, scale=1):
    cx = x
    for ch in s:
        gl = glyph(ch)
        for r in range(7):
            row = gl[r]
            for c in range(5):
                if c < len(row) and row[c] == '#':
                    for dy in range(scale):
                        for dx in range(scale):
                            px(cx + c*scale + dx, y + r*scale + dy, col)
        cx += (5 + 1) * scale          # 1 px inter-glyph gap
    return cx

def textw(s, scale=1):
    return len(s) * 6 * scale

def center(s, scale=1):
    return max(0, (W - textw(s, scale)) // 2)

def fit(s, maxpx, scale=1):
    """Truncate with a trailing '.' marker if too wide for maxpx."""
    if textw(s, scale) <= maxpx:
        return s
    n = max(0, maxpx // (6*scale))
    return (s[:max(0, n-1)] + ".") if n >= 1 else ""

def wifi_bars(x, y, level):  # level 0..4, green lit + grey unlit, like the UI
    for i in range(4):
        bh = 2 + i*2
        col = GREEN if i < level else GREENL
        rrect(x + i*4, y + (8-bh), x + i*4 + 3, y + 8, col)

def ring(cx, cy, rad, frac, col, bg):
    # light full ring + green arc covering `frac` of the circle (top, clockwise)
    import math
    for a in range(0, 360):
        for w in range(3):
            rr = rad - w
            xx = int(round(cx + rr*math.sin(math.radians(a))))
            yy = int(round(cy - rr*math.cos(math.radians(a))))
            lit = (a/360.0) <= frac
            px(xx, yy, col if lit else bg)

# ── compose: WiFi + BLE, then the status block (no MQTT) ──────────────────────
# CARD 1 — CONNECTIONS (WiFi + BLE only)
rrect(3, 3, W-3, 66, CARD)
text(6, 6, "CONNECTIONS", GREY)

wifi_bars(6, 20, 3)
text(24, 20, "-64dBm", INK)
text(8, 31, fit("Jupiter", W-14), GREEN)            # SSID

wifi_bars(6, 47, 2)
text(24, 47, "-83dBm", INK)
text(8, 58, fit("..72:ac:0d", W-14), GREEN)         # peer MAC, abbreviated for 80px

# CARD 2 — STATUS (the bottom UI block: asleep / charging / unreachable).
# Shown here in the awake+charging state to match the screenshot.
rrect(3, 72, W-3, H-4, CARD)
text(center("`Charging"), 78, "`Charging", INK)     # ` = bolt glyph
ring(W//2, 116, 26, 0.96, GREEN, GREENL)
soc = "96"
sw = textw(soc, 3) + textw("%", 2)                  # "96" @3x + "%" @2x
x0 = (W - sw) // 2
text(x0, 104, soc, GREEN, 3)
text(x0 + textw(soc, 3), 111, "%", GREEN, 2)
text(center("4kW   6A"), 148, "4kW   6A", INK)

def cmd_png(out="tools/display_preview.png"):
    S = 6
    bw, bh = W*S, H*S
    big = [[canvas[y//S][x//S] for x in range(bw)] for y in range(bh)]
    write_png(out, bw, bh, big)
    print("wrote", out, f"({bw}x{bh}, logical {W}x{H})")

def cmd_cheader(out="main/display_font.h"):
    lines = []
    lines.append("// Auto-generated by tools/display_sim.py — do not edit by hand.")
    lines.append("// 5x7 column-major font: glyph[c] bit r (0=top) = pixel (col c, row r).")
    lines.append("#pragma once")
    lines.append("#include <stdint.h>")
    lines.append("// Printable ASCII 0x20..0x7E; index = ch - 0x20.")
    lines.append("static const uint8_t DISPLAY_FONT5X7[][5] = {")
    for code in range(0x20, 0x7F):
        ch = chr(code)
        gl = glyph(ch)
        cols = []
        for c in range(5):
            b = 0
            for r in range(7):
                if c < len(gl[r]) and gl[r][c] == '#':
                    b |= (1 << r)
            cols.append(b)
        lines.append("  {%s}, // 0x%02X %s" % (
            ",".join("0x%02x" % v for v in cols), code, ch if ch != ' ' else "SP"))
    lines.append("};")
    # bolt glyph (not in printable range)
    gl = glyph('`')
    cols = []
    for c in range(5):
        b = 0
        for r in range(7):
            if c < len(gl[r]) and gl[r][c] == '#':
                b |= (1 << r)
        cols.append(b)
    lines.append("static const uint8_t DISPLAY_GLYPH_BOLT[5] = {%s};" %
                 ",".join("0x%02x" % v for v in cols))
    open(out, "w").write("\n".join(lines) + "\n")
    print("wrote", out)

if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "png"
    arg  = sys.argv[2] if len(sys.argv) > 2 else None
    if mode == "png":
        cmd_png(arg or "tools/display_preview.png")
    elif mode == "cheader":
        cmd_cheader(arg or "main/display_font.h")
    else:
        print("usage: display_sim.py [png|cheader] [outfile]")
