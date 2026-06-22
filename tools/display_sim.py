#!/usr/bin/env python3
"""
T-Dongle-S3 / -C5 display layout simulator + 5x7 font source of truth.

Renders the 0.96" ST7735 screen in LANDSCAPE (160x80 px) exactly as the
firmware will, using the SAME 5x7 bitmap font and the SAME drawing primitives,
so we can validate the layout offline — BEFORE flashing hardware.

Layout (matches main/display.cpp compose()):
  ┌──────────────────────────────────────────┐
  │ ▂▄▆█  Jupiter                  *  ▂▄▆█    │  header: WiFi bars+SSID | BLE sym+bars
  ├──────────────────────────────────────────┤
  │  ███████████░░░░░░░░░░░░░░░░░░░░░░░░  ▐    │  battery: gradient fill to SoC,
  │  ███████████░░░░░⚡ 64%░░░░░░░░░░░░░  ▐    │           bolt while charging (<100%)
  └──────────────────────────────────────────┘
Battery fill colour is a red→amber→green gradient over the SoC. States:
  charging (<100%) → bolt; 100% → no bolt; asleep → dimmed; unreachable → empty.

Three outputs (stdlib only, no Pillow):
  python3 tools/display_sim.py png  [out.png]   -> the charging hero frame (preview)
  python3 tools/display_sim.py states [out.png] -> a montage of all states
  python3 tools/display_sim.py cheader [out.h]  -> main/display_font.h (font + glyphs)

The font dict below is authored as 7-row x 5-col ASCII art ('#'=on). The C-header
generator packs each glyph into 5 column bytes (bit r = row r, 0=top), and also
emits the Bluetooth glyph and the big charging bolt as row-major bitmaps — which
is what display.cpp consumes. Keep BT/BOLT here in sync with display.cpp.
"""
import sys, zlib, struct, math

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
# small lightning bolt, addressed as key '`' (kept for back-compat / inline text)
G['`'] = ["..##.",".##..","#####","..##.",".##..","#....","#...."]

def glyph(ch):
    return G.get(ch, ["#####","#...#","#...#","#...#","#...#","#...#","#####"] if ch != ' ' else G[' '])

# ── Bluetooth glyph (5 wide x 9 tall) and big charging bolt (10 wide x 16 tall)
# Authored row-major ('#'=lit). The cheader generator emits these as bitmaps
# (DISPLAY_BT_ROWS / DISPLAY_BOLT_ROWS) consumed by display.cpp — keep in sync.
BT_ROWS = [
    "..#..",
    "..##.",
    "#.#.#",
    ".###.",
    "..#..",
    ".###.",
    "#.#.#",
    "..##.",
    "..#..",
]
BOLT_ROWS = [
    "......###.",
    ".....###..",
    "....###...",
    "...###....",
    "..###.....",
    ".#########",
    ".########.",
    "..######..",
    "....###...",
    "...###....",
    "..###.....",
    ".###......",
    "###.......",
    "##........",
    "#.........",
    "#.........",
]
# WiFi glyph (7 wide x 6 tall): two broadcast arcs over a dot. Used as the icon for
# the WiFi searching animation (emitted as DISPLAY_WIFI_ROWS; keep in sync).
WIFI_ROWS = [
    ".#####.",
    "#.....#",
    "..###..",
    ".#...#.",
    ".......",
    "...#...",
]

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

# ── logical 160x80 landscape canvas (DARK theme to match the device mockup) ───
W, H = 160, 80
BG      = (13, 16, 22)       # near-black screen background
PANEL   = (26, 32, 45)       # battery interior / empty cells
BORDER  = (236, 238, 242)    # battery shell + terminal (white)
INK     = (240, 242, 245)    # primary text (white)
GREY    = (128, 138, 152)    # muted labels / disconnected
DIV     = (38, 46, 60)       # header divider
BAR_ON  = (74, 175, 80)      # lit signal bar (green)
BAR_OFF = (40, 50, 64)       # unlit signal bar
AMBER   = (235, 170, 40)     # unreachable accent
BLE_LIGHT = (170, 232, 158)  # searching bars: resting (light green)
BLE_GREEN = ( 16,  78,  36)  # searching bars: highlighted (deep/dark green) — high contrast

# red → amber → light-green → green → deep-green over SoC 0..100
GRAD = [
    (0.00, (231,  76,  60)),   # 0%   red
    (0.18, (240, 190,  40)),   # ~18% amber/yellow
    (0.45, (120, 200,  90)),   # light green
    (0.80, ( 60, 175,  80)),   # green
    (1.00, ( 30, 140,  60)),   # 100% deep green
]

def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))

def soc_color(soc):
    p = max(0.0, min(1.0, soc / 100.0))
    for i in range(len(GRAD) - 1):
        p0, c0 = GRAD[i]
        p1, c1 = GRAD[i + 1]
        if p <= p1:
            t = 0 if p1 == p0 else (p - p0) / (p1 - p0)
            return lerp(c0, c1, t)
    return GRAD[-1][1]

def dim(col, t=0.5):   # blend a colour toward the panel (for the asleep state)
    return lerp(col, PANEL, t)

class Canvas:
    def __init__(self):
        self.c = [[BG for _ in range(W)] for _ in range(H)]
    def px(self, x, y, col):
        if 0 <= x < W and 0 <= y < H:
            self.c[y][x] = col
    def rect(self, x0, y0, x1, y1, col):
        for y in range(y0, y1):
            for x in range(x0, x1):
                self.px(x, y, col)
    def frame(self, x0, y0, x1, y1, t, col):   # hollow rounded-ish border of thickness t
        self.rect(x0, y0, x1, y0 + t, col)
        self.rect(x0, y1 - t, x1, y1, col)
        self.rect(x0, y0, x0 + t, y1, col)
        self.rect(x1 - t, y0, x1, y1, col)
        for (cx, cy) in [(x0, y0), (x1 - 1, y0), (x0, y1 - 1), (x1 - 1, y1 - 1)]:
            self.px(cx, cy, BG)   # nibble the hard corners for a softer look
    def char(self, x, y, ch, col, scale):
        gl = glyph(ch)
        for r in range(7):
            row = gl[r]
            for c in range(5):
                if c < len(row) and row[c] == '#':
                    for dy in range(scale):
                        for dx in range(scale):
                            self.px(x + c*scale + dx, y + r*scale + dy, col)
    def text(self, x, y, s, col, scale=1):
        cx = x
        for ch in s:
            self.char(cx, y, ch, col, scale)
            cx += 6 * scale
        return cx
    def text_outline(self, x, y, s, col, scale, oc):
        for ox, oy in [(-1,0),(1,0),(0,-1),(0,1)]:
            self.text(x+ox, y+oy, s, oc, scale)
        self.text(x, y, s, col, scale)
    def bitmap(self, x, y, rows, col, scale=1, outline=None):
        h, w = len(rows), max(len(r) for r in rows)
        if outline is not None:
            for ox, oy in [(-1,0),(1,0),(0,-1),(0,1),(-1,-1),(1,1),(-1,1),(1,-1)]:
                self._blit(x+ox, y+oy, rows, outline, scale)
        self._blit(x, y, rows, col, scale)
        return w * scale, h * scale
    def _blit(self, x, y, rows, col, scale):
        for r, row in enumerate(rows):
            for c, ch in enumerate(row):
                if ch == '#':
                    for dy in range(scale):
                        for dx in range(scale):
                            self.px(x + c*scale + dx, y + r*scale + dy, col)

def textw(s, scale=1):
    return len(s) * 6 * scale

def fit(s, maxpx, scale=1):
    if textw(s, scale) <= maxpx:
        return s
    n = max(0, maxpx // (6*scale))
    return (s[:max(0, n-1)] + ".") if n >= 1 else ""

def signal_bars(cv, x, y, level, on_col=BAR_ON):
    """4 ascending bars, bottom-aligned at y+10; lit green, unlit dark."""
    for i in range(4):
        bh = 3 + i*2
        col = on_col if i < level else BAR_OFF
        cv.rect(x + i*4, y + (10 - bh), x + i*4 + 3, y + 10, col)

# Big BLE "searching for a connection" animation, drawn where the battery would
# be (no battery while disconnected): a Bluetooth symbol on the left and a compact
# cluster of 5 ascending bars on the right; a dark-green highlight sweeps across
# light-green bars (a flowing colour gradient) to read as "scanning for the BLE link".
SRCH_N, SRCH_BW, SRCH_GAP, SRCH_X0, SRCH_BASE = 5, 10, 6, 80, 70   # compact, right-flush
SRCH_ICON_SCALE = 3
SRCH_BT_X, SRCH_BT_Y = 24, 36                     # Bluetooth glyph, off the left edge
SRCH_WIFI_X, SRCH_WIFI_Y = 16, 38                 # WiFi glyph (wider), off the left edge
SRCH_STEPS = 3                                    # animation sub-frames per bar
SRCH_FALLOFF = 1.5                                # highlight width, in bars
SRCH_HALF = (SRCH_N - 1) * SRCH_STEPS             # frames for one direction (small↔large)
SRCH_CYCLE = 2 * SRCH_HALF                        # full ping-pong period

# icon = (rows, x, y); the swept bars are identical for WiFi and BLE searches.
# The highlight ping-pongs: smallest→largest bar, then largest→smallest, repeating
# (a triangle wave) — a wave that runs out and back, not a one-way scan.
def draw_searching(cv, frame, icon):
    rows, ix, iy = icon
    cv.bitmap(ix, iy, rows, INK, SRCH_ICON_SCALE)           # which link is being searched
    p = frame % SRCH_CYCLE
    hp = (p / SRCH_STEPS) if p <= SRCH_HALF else ((SRCH_CYCLE - p) / SRCH_STEPS)  # bounce
    for i in range(SRCH_N):
        h = 12 + i * 7
        t = max(0.0, 1.0 - abs(i - hp) / SRCH_FALLOFF)
        col = lerp(BLE_LIGHT, BLE_GREEN, t)
        x = SRCH_X0 + i * (SRCH_BW + SRCH_GAP)
        cv.rect(x, SRCH_BASE - h, x + SRCH_BW, SRCH_BASE, col)

SRCH_ICON_BLE  = (BT_ROWS,   SRCH_BT_X,   SRCH_BT_Y)
SRCH_ICON_WIFI = (WIFI_ROWS, SRCH_WIFI_X, SRCH_WIFI_Y)

# Shown (instead of the search bars) once a BLE link is up but pairing isn't done:
# big "Pairing" with an animated 0–3 dot ellipsis (fixed left so it doesn't jiggle).
def draw_pairing(cv, frame):
    nd = (frame // 4) % 4
    x0 = (W - textw("Pairing...", 2)) // 2
    cv.text(x0, 42, "Pairing" + "." * nd, INK, 2)

# ── compose one frame from a state dict ───────────────────────────────────────
# state = { wifi:0..4, ssid:str, ble:0..4, ble_on:bool, frame:int,
#           soc:int|None, charging:bool, link:"awake"|"asleep"|"unreachable" }
# Returns True when it drew the searching animation (the caller animates faster).
def compose(cv, st):
    cv.rect(0, 0, W, H, BG)

    soc = st.get("soc")
    link = st.get("link", "awake")
    charging = st.get("charging", False)
    wifi_on = st.get("wifi", 0) > 0
    paired = st.get("paired", True)
    ble_conn = st.get("ble_on", False)
    # Centre state, by priority: WiFi search > pairing > battery > BLE search bars.
    wifi_searching = not wifi_on
    pairing = ble_conn and not paired                 # BLE link up but not yet paired
    battery_ok = (link != "unreachable") and (soc is not None)
    # BLE search bars appear ONLY when the car is out of range (no link, no data).
    ble_bars = not (wifi_searching or pairing or battery_ok)

    # ── header: WiFi bars + SSID (left) | BLE symbol + bars (right) ──────────
    # Hide whichever small indicator is the active search hero; the big centre
    # animation represents it instead.
    if not wifi_searching:
        signal_bars(cv, 4, 3, st.get("wifi", 0))
        name_max = (158 - 26) if ble_bars else (158 - 26 - 36)
        cv.text(26, 4, fit(st.get("ssid") or "-", name_max), INK)

    if not ble_bars:
        bx = 158 - 16                  # 4 bars * 4px
        signal_bars(cv, bx, 3, st.get("ble", 0) if ble_conn else 0)
        cv.bitmap(bx - 9, 2, BT_ROWS, INK if ble_conn else GREY)

    cv.rect(3, 17, W - 3, 18, DIV)     # divider under the header

    # ── centre: WiFi search > pairing > battery > BLE search bars ────────────
    f = st.get("frame", 0)
    if wifi_searching:
        draw_searching(cv, f, SRCH_ICON_WIFI)
        return True
    if pairing:
        draw_pairing(cv, f)
        return True
    if not battery_ok:
        draw_searching(cv, f, SRCH_ICON_BLE)
        return True

    # ── battery shell ───────────────────────────────────────────────────────
    bx0, by0, bx1, by1 = 6, 24, 146, 74
    cv.rect(bx0+2, by0+2, bx1-2, by1-2, PANEL)        # interior
    cv.frame(bx0, by0, bx1, by1, 3, BORDER)           # shell
    cv.rect(bx1, 40, bx1 + 6, 58, BORDER)             # + terminal nub
    ix0, iy0, ix1, iy1 = bx0+3, by0+3, bx1-3, by1-3   # interior bounds
    iw = ix1 - ix0

    soc = max(0, min(100, int(soc)))
    asleep = link == "asleep"
    fill_col = dim(soc_color(soc)) if asleep else soc_color(soc)
    fw = int(round(iw * soc / 100.0))
    if fw > 0:
        cv.rect(ix0, iy0, ix0 + fw, iy1, fill_col)

    txt_col = GREY if asleep else INK
    num = "%d%%" % soc
    show_bolt = charging and soc < 100 and not asleep

    if show_bolt:
        # centred group: [bolt][gap][NN%]
        bw = max(len(r) for r in BOLT_ROWS) * 2
        gap = 6
        grp = bw + gap + textw(num, 3)
        gx = (W - grp)//2
        cv.bitmap(gx, (H + 18 - len(BOLT_ROWS)*2)//2 - 2, BOLT_ROWS, (255, 235, 120), 2, outline=BG)
        cv.text_outline(gx + bw + gap, 41, num, INK, 3, BG)
    else:
        cv.text_outline((W - textw(num, 3))//2, 41, num, txt_col, 3, BG)

    if asleep:
        cv.text((W - textw("ASLEEP", 1))//2, 64, "ASLEEP", GREY, 1)
    return False

# ── render helpers ────────────────────────────────────────────────────────────
def upscale(cv, S):
    bw, bh = W*S, H*S
    return [[cv.c[y//S][x//S] for x in range(bw)] for y in range(bh)]

def cmd_png(out="tools/display_preview.png"):
    cv = Canvas()
    compose(cv, dict(wifi=4, ssid="Jupiter", ble=3, ble_on=True,
                     soc=64, charging=True, link="awake"))
    S = 6
    write_png(out, W*S, H*S, upscale(cv, S))
    print("wrote", out, f"({W*S}x{H*S}, logical {W}x{H})")

def cmd_states(out="tools/display_states.png"):
    states = [
        dict(wifi=4, ssid="Jupiter", ble=3, ble_on=True,  soc=64,  charging=True,  link="awake"),
        dict(wifi=3, ssid="Jupiter", ble=2, ble_on=True,  soc=5,   charging=False, link="awake"),
        dict(wifi=3, ssid="Jupiter", ble=3, ble_on=True,  soc=20,  charging=False, link="awake"),
        dict(wifi=4, ssid="Jupiter", ble=3, ble_on=True,  soc=45,  charging=False, link="awake"),
        dict(wifi=4, ssid="Jupiter", ble=4, ble_on=True,  soc=80,  charging=True,  link="awake"),
        dict(wifi=4, ssid="Jupiter", ble=4, ble_on=True,  soc=100, charging=True,  link="awake"),
        dict(wifi=2, ssid="Jupiter", ble=2, ble_on=True,  soc=72,  charging=False, link="asleep"),
        dict(wifi=3, ssid="Jupiter", ble=0, ble_on=False, soc=None,charging=False, link="unreachable", frame=6),
        dict(wifi=0, ssid="Jupiter", ble=0, ble_on=False, soc=None,charging=False, link="unreachable", frame=6),
        dict(wifi=3, ssid="Jupiter", ble=4, ble_on=True,  paired=False, soc=None, charging=False, link="unreachable", frame=8),
    ]
    labels = ["awake / charging 64%", "5% (red)", "20% (amber)", "45% (green)",
              "charging 80%", "100% (no bolt)", "asleep (dim)", "searching BLE (car unreachable)",
              "searching WiFi (priority)", "pairing (BLE up, not paired)"]
    S = 4
    pad = 10
    cap = 16
    cellw, cellh = W*S, H*S + cap
    cols = 2
    rows = (len(states) + cols - 1)//cols
    bw = cols*cellw + (cols+1)*pad
    bh = rows*cellh + (rows+1)*pad
    big = [[(8,9,12) for _ in range(bw)] for _ in range(bh)]
    # tiny label font via a throwaway canvas region is overkill; draw caption text
    # by rendering each frame and stamping a 1x caption with the Canvas font.
    for idx, st in enumerate(states):
        cv = Canvas()
        compose(cv, st)
        up = upscale(cv, S)
        r, cc = idx//cols, idx % cols
        ox = pad + cc*(cellw + pad)
        oy = pad + r*(cellh + pad)
        for y in range(H*S):
            for x in range(W*S):
                big[oy+y][ox+x] = up[y][x]
        # caption strip
        capcv = Canvas()
        capcv.text(2, 4, labels[idx][:26], INK, 1)
        capup = upscale(capcv, 2)
        for y in range(cap):
            for x in range(min(cellw, W*2)):
                big[oy + H*S + y][ox + x] = capup[y][x]
    write_png(out, bw, bh, big)
    print("wrote", out, f"({bw}x{bh})")

def cmd_search(out="tools/display_search.png"):
    """Render the WiFi (left col) and BLE (right col) searching sweeps frame-by-frame."""
    frames = list(range(0, SRCH_CYCLE, 2))     # every other frame across one sweep
    # col 0 = WiFi search (wifi down → priority); col 1 = BLE search (wifi up, car gone)
    wifi_st = dict(wifi=0, ssid="Jupiter", soc=None, charging=False, link="unreachable", ble_on=False)
    ble_st  = dict(wifi=3, ssid="Jupiter", soc=None, charging=False, link="unreachable", ble_on=False)
    S = 4
    pad, cap = 10, 16
    cellw, cellh = W*S, H*S + cap
    cols = 2
    rows = len(frames)
    bw = cols*cellw + (cols+1)*pad
    bh = rows*cellh + (rows+1)*pad
    big = [[(8,9,12) for _ in range(bw)] for _ in range(bh)]
    for r, f in enumerate(frames):
        for cc, (st, tag) in enumerate([(wifi_st, "WiFi"), (ble_st, "BLE")]):
            cv = Canvas()
            compose(cv, dict(st, frame=f))
            up = upscale(cv, S)
            ox, oy = pad + cc*(cellw + pad), pad + r*(cellh + pad)
            for y in range(H*S):
                for x in range(W*S):
                    big[oy+y][ox+x] = up[y][x]
            capcv = Canvas(); capcv.text(2, 4, "%s f%d" % (tag, f), INK, 1)
            capup = upscale(capcv, 2)
            for y in range(cap):
                for x in range(min(cellw, W*2)):
                    big[oy + H*S + y][ox + x] = capup[y][x]
    write_png(out, bw, bh, big)
    print("wrote", out, f"({bw}x{bh})")

def cmd_cheader(out="main/display_font.h"):
    def cols_of(gl):
        cols = []
        for c in range(5):
            b = 0
            for r in range(7):
                if c < len(gl[r]) and gl[r][c] == '#':
                    b |= (1 << r)
            cols.append(b)
        return cols
    def rows_bits(rows):       # row-major, bit (W-1-c) = leftmost column
        w = max(len(r) for r in rows)
        out = []
        for row in rows:
            b = 0
            for c in range(w):
                if c < len(row) and row[c] == '#':
                    b |= (1 << (w - 1 - c))
            out.append(b)
        return w, out

    L = []
    L.append("// Auto-generated by tools/display_sim.py — do not edit by hand.")
    L.append("// 5x7 column-major font: glyph[c] bit r (0=top) = pixel (col c, row r).")
    L.append("#pragma once")
    L.append("#include <stdint.h>")
    L.append("// Printable ASCII 0x20..0x7E; index = ch - 0x20.")
    L.append("static const uint8_t DISPLAY_FONT5X7[][5] = {")
    for code in range(0x20, 0x7F):
        ch = chr(code)
        cols = cols_of(glyph(ch))
        # Sanitize the label char: space and backslash would otherwise produce an
        # empty/line-continuation comment (-Werror=comment on the trailing '\').
        label = {' ': 'SP', '\\': 'BSL'}.get(ch, ch)
        L.append("  {%s}, // 0x%02X %s" % (",".join("0x%02x" % v for v in cols), code, label))
    L.append("};")
    # small inline bolt glyph (back-compat; '`' is outside the printable range used above)
    L.append("static const uint8_t DISPLAY_GLYPH_BOLT[5] = {%s};" %
             ",".join("0x%02x" % v for v in cols_of(glyph('`'))))
    # Bluetooth glyph (row-major bitmap)
    bw, brows = rows_bits(BT_ROWS)
    L.append("// Bluetooth glyph, row-major: bit (W-1-c) of row r = pixel (col c, row r).")
    L.append("#define DISPLAY_BT_W %d" % bw)
    L.append("#define DISPLAY_BT_H %d" % len(brows))
    L.append("static const uint8_t DISPLAY_BT_ROWS[%d] = {%s};" %
             (len(brows), ",".join("0x%02x" % v for v in brows)))
    # Big charging bolt (row-major bitmap)
    lw, lrows = rows_bits(BOLT_ROWS)
    L.append("// Big charging bolt, row-major (same bit convention as the BT glyph).")
    L.append("#define DISPLAY_BOLT_W %d" % lw)
    L.append("#define DISPLAY_BOLT_H %d" % len(lrows))
    L.append("static const uint16_t DISPLAY_BOLT_ROWS[%d] = {%s};" %
             (len(lrows), ",".join("0x%04x" % v for v in lrows)))
    # WiFi glyph (row-major bitmap) — icon for the WiFi searching animation
    ww, wrows = rows_bits(WIFI_ROWS)
    L.append("// WiFi glyph (broadcast arcs + dot), row-major (same bit convention as BT).")
    L.append("#define DISPLAY_WIFI_W %d" % ww)
    L.append("#define DISPLAY_WIFI_H %d" % len(wrows))
    L.append("static const uint8_t DISPLAY_WIFI_ROWS[%d] = {%s};" %
             (len(wrows), ",".join("0x%02x" % v for v in wrows)))
    open(out, "w").write("\n".join(L) + "\n")
    print("wrote", out)

if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "png"
    arg  = sys.argv[2] if len(sys.argv) > 2 else None
    if mode == "png":
        cmd_png(arg or "tools/display_preview.png")
    elif mode == "states":
        cmd_states(arg or "tools/display_states.png")
    elif mode == "search":
        cmd_search(arg or "tools/display_search.png")
    elif mode == "cheader":
        cmd_cheader(arg or "main/display_font.h")
    else:
        print("usage: display_sim.py [png|states|search|cheader] [outfile]")
