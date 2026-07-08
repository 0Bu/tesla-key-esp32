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

The BOOT button rotates the panel 90° per press; the PORTRAIT (80x160) layout — a vertical
battery + two-row header — is rendered by compose_portrait() (mirrors display.cpp draw_portrait).

Outputs (stdlib only, no Pillow):
  python3 tools/display_sim.py png  [out.png]             -> the charging hero frame (preview)
  python3 tools/display_sim.py states [out.png]           -> a montage of all LANDSCAPE states
  python3 tools/display_sim.py states-portrait [out.png]  -> a montage of all PORTRAIT states
  python3 tools/display_sim.py cheader [out.h]            -> main/display_font.h (font + glyphs)

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
BLE_LIGHT = (170, 232, 158)  # searching bars: highlighted (light green) — high contrast
BLE_GREEN = ( 16,  78,  36)  # searching bars: resting (deep/dark green)

# red → amber → light-green → green → deep-green over SoC 0..100
GRAD = [
    (0.00, (231,  76,  60)),   # 0%   red
    (0.18, (240, 190,  40)),   # ~18% amber/yellow
    (0.45, (120, 200,  90)),   # light green
    (0.80, ( 60, 175,  80)),   # green
    (1.00, ( 30, 140,  60)),   # 100% deep green
]

def _iround(x):
    # Round half away from zero, matching C++ lroundf — and round the DELTA (b-a)*t, exactly
    # like display.cpp/soc_gradient.hpp lerp8 (a + lroundf((b-a)*t)). Python's built-in round()
    # is banker's rounding on the SUM, which disagrees by 1 where a delta lands on a clean .5
    # (e.g. the asleep-dim blend); this keeps the sim pixel-exact to the firmware.
    return int(x + 0.5) if x >= 0 else -int(-x + 0.5)

def lerp(a, b, t):
    return tuple(a[i] + _iround((b[i] - a[i]) * t) for i in range(3))

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
    # Default 160x80 (landscape); pass (80, 160) for the portrait layout. All primitives clip
    # to self.w/self.h, so the same drawing code serves either orientation.
    def __init__(self, w=W, h=H):
        self.w, self.h = w, h
        self.c = [[BG for _ in range(w)] for _ in range(h)]
    def px(self, x, y, col):
        if 0 <= x < self.w and 0 <= y < self.h:
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
    def text_clip(self, x, y, s, col, scale, cx0, cx1):
        # like text() but only sets pixels with cx0 <= px < cx1 (for the scroll box)
        cx = x
        for ch in s:
            gl = glyph(ch)
            for r in range(7):
                row = gl[r]
                for c in range(5):
                    if c < len(row) and row[c] == '#':
                        for dy in range(scale):
                            for dx in range(scale):
                                px = cx + c*scale + dx
                                if cx0 <= px < cx1:
                                    self.px(px, y + r*scale + dy, col)
            cx += 6 * scale
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

# Horizontal marquee for an over-long SSID: ping-pong (pause, scroll out to reveal
# the end, pause, scroll back) so the whole name is readable. Returns the left
# offset 0..span. Mirrors scroll_offset() in display.cpp.
SCROLL_PAUSE = 8     # ticks paused at each end
SCROLL_SPEED = 2     # px per tick
def scroll_offset(tick, span):
    if span <= 0:
        return 0
    travel = (span + SCROLL_SPEED - 1) // SCROLL_SPEED      # ticks to cover the span
    period = 2 * SCROLL_PAUSE + 2 * travel
    p = tick % period
    if p < SCROLL_PAUSE:
        return 0
    if p < SCROLL_PAUSE + travel:
        return min((p - SCROLL_PAUSE) * SCROLL_SPEED, span)
    if p < 2 * SCROLL_PAUSE + travel:
        return span
    return max(span - (p - 2 * SCROLL_PAUSE - travel) * SCROLL_SPEED, 0)

def signal_bars(cv, x, y, level, on_col=BAR_ON):
    """4 ascending bars, bottom-aligned at y+14; lit green, unlit dark."""
    for i in range(4):
        bh = 4 + i*3
        col = on_col if i < level else BAR_OFF
        cv.rect(x + i*4, y + (14 - bh), x + i*4 + 3, y + 14, col)

# Big BLE "searching for a connection" animation, drawn where the battery would
# be (no battery while disconnected): a Bluetooth symbol on the left and a compact
# cluster of 5 ascending bars on the right; a bright light-green highlight sweeps
# across the dark-green resting bars, with the bars near the peak lit half-bright
# (a soft colour gradient) to read as "scanning for the BLE link".
SRCH_N, SRCH_BW, SRCH_GAP, SRCH_X0, SRCH_BASE = 5, 10, 6, 80, 70   # compact, right-flush
SRCH_ICON_SCALE = 3
SRCH_BT_X, SRCH_BT_Y = 24, 36                     # Bluetooth glyph, off the left edge
SRCH_STEPS = 3                                    # animation sub-frames per bar
SRCH_FALLOFF = 1.5                                # highlight width, in bars
SRCH_HALF = (SRCH_N - 1) * SRCH_STEPS             # frames for one direction (small↔large)
SRCH_CYCLE = 2 * SRCH_HALF                        # full ping-pong period

# The swept bars are identical for WiFi and BLE searches; the highlight ping-pongs
# (a triangle wave) out and back across them — not a one-way scan.
def draw_search_bars(cv, frame):
    p = frame % SRCH_CYCLE
    hp = (p / SRCH_STEPS) if p <= SRCH_HALF else ((SRCH_CYCLE - p) / SRCH_STEPS)  # bounce
    for i in range(SRCH_N):
        h = 12 + i * 7
        t = max(0.0, 1.0 - abs(i - hp) / SRCH_FALLOFF)
        col = lerp(BLE_GREEN, BLE_LIGHT, t)
        x = SRCH_X0 + i * (SRCH_BW + SRCH_GAP)
        cv.rect(x, SRCH_BASE - h, x + SRCH_BW, SRCH_BASE, col)

# label = ("icon", rows, x, y) draws a bitmap glyph; ("text", str, scale, x, y)
# draws a word — used so the WiFi search reads "WiFi" while BLE keeps its symbol.
def draw_searching(cv, frame, label):
    if label[0] == "icon":
        _, rows, ix, iy = label
        cv.bitmap(ix, iy, rows, INK, SRCH_ICON_SCALE)
    else:
        _, s, scale, ix, iy = label
        cv.text(ix, iy, s, INK, scale)
    draw_search_bars(cv, frame)

SRCH_ICON_BLE  = ("icon", BT_ROWS, SRCH_BT_X, SRCH_BT_Y)
SRCH_ICON_WIFI = ("text", "WiFi", 2, 14, 42)   # "WiFi" word instead of the glyph

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
# ── pure decision ("what to show") — MUST mirror tk::display::compose() in
# main/logic/display_model.hpp. scripts/check-display-sim-parity.sh diffs this against golden
# vectors the C++ presenter emits, so a drift between the firmware and this pixel sim fails CI
# instead of going unnoticed by hand. compose() below draws exactly this model — the same
# presenter/renderer split the firmware uses. Bars (rssi→level) are not decided here (the sim
# is fed levels directly); everything else is.
def decide(inp):
    link     = inp["link"]
    wifi_on  = inp["wifi_on"]
    ssid     = inp["ssid"]
    ble_conn = inp["ble_connected"]
    have_soc = inp["have_soc"]
    charging = inp["charging"]
    paired   = inp["paired"]
    tick     = inp["tick"]

    unreachable    = link == "unreachable"
    wifi_searching = not wifi_on
    pairing        = ble_conn and not paired          # BLE link up but not yet paired
    battery_ok     = (not unreachable) and have_soc
    # BLE search bars appear ONLY when nothing else claims the centre (car out of range).
    ble_bars       = not (wifi_searching or pairing or battery_ok)

    if   wifi_searching: hero = "wifi_search"
    elif pairing:        hero = "pairing"
    elif not battery_ok: hero = "ble_search"
    else:                hero = "battery"

    m = {"hero": hero, "show_wifi": not wifi_searching, "show_ble": not ble_bars,
         "ssid_avail": 0, "ssid_scrolling": False, "ssid_off": 0, "soc": 0,
         "fill_r": 0, "fill_g": 0, "fill_b": 0, "asleep": False, "show_bolt": False,
         "animating": False}

    if m["show_wifi"]:
        if inp.get("orient", "landscape") == "portrait":
            avail, scale = 72, 1                       # SSID own row, full width, scale 1
        else:
            avail, scale = ((158 - 28) if ble_bars else (158 - 28 - 32)), 2
        m["ssid_avail"] = avail
        if textw(ssid, scale) > avail:                # too long → horizontal marquee
            m["ssid_scrolling"] = True
            m["ssid_off"] = scroll_offset(tick, textw(ssid, scale) - avail)

    if hero == "battery":
        s = max(0, min(100, int(inp["soc"])))
        m["soc"] = s
        m["asleep"] = (link == "asleep")
        col = dim(soc_color(s)) if m["asleep"] else soc_color(s)
        m["fill_r"], m["fill_g"], m["fill_b"] = col
        m["show_bolt"] = charging and s < 100 and not m["asleep"]

    m["animating"] = (hero != "battery") or m["ssid_scrolling"]
    return m

def compose(cv, st):
    cv.rect(0, 0, W, H, BG)
    inp = {
        "link":          st.get("link", "awake"),
        "wifi_on":       st.get("wifi", 0) > 0,
        "ssid":          st.get("ssid") or "-",
        "ble_connected": st.get("ble_on", False),
        "have_soc":      st.get("soc") is not None,
        "soc":           st.get("soc") or 0,
        "charging":      st.get("charging", False),
        "paired":        st.get("paired", True),
        "tick":          st.get("frame", 0),
    }
    m = decide(inp)
    f = inp["tick"]

    # ── header: WiFi bars + SSID (left) | BLE symbol + bars (right) ──────────
    # Hide whichever small indicator is the active search hero. Taller, more legible status
    # bar: scale-2 SSID, taller bars, divider at y22. Bars are the sim's direct level inputs.
    if m["show_wifi"]:
        signal_bars(cv, 4, 4, st.get("wifi", 0))
        ssid = inp["ssid"]
        if not m["ssid_scrolling"]:
            cv.text(28, 4, ssid, INK, 2)
        else:
            cv.text_clip(28 - m["ssid_off"], 4, ssid, INK, 2, 28, 28 + m["ssid_avail"])

    if m["show_ble"]:
        bx = 158 - 16                  # 4 bars * 4px
        signal_bars(cv, bx, 4, st.get("ble", 0) if inp["ble_connected"] else 0)
        cv.bitmap(bx - 13, 1, BT_ROWS, INK if inp["ble_connected"] else GREY, 2)  # BT glyph, scale 2

    cv.rect(3, 22, W - 3, 23, DIV)     # divider under the (taller) header

    # ── centre hero: WiFi search > pairing > BLE search > battery ────────────
    if m["hero"] == "wifi_search":
        draw_searching(cv, f, SRCH_ICON_WIFI); return True
    if m["hero"] == "pairing":
        draw_pairing(cv, f); return True
    if m["hero"] == "ble_search":
        draw_searching(cv, f, SRCH_ICON_BLE); return True

    # ── battery shell ───────────────────────────────────────────────────────
    bx0, by0, bx1, by1 = 6, 24, 146, 74
    cv.rect(bx0+2, by0+2, bx1-2, by1-2, PANEL)        # interior
    cv.frame(bx0, by0, bx1, by1, 3, BORDER)           # shell
    cv.rect(bx1, 40, bx1 + 6, 58, BORDER)             # + terminal nub
    ix0, iy0, ix1, iy1 = bx0+3, by0+3, bx1-3, by1-3   # interior bounds
    iw = ix1 - ix0

    soc = m["soc"]
    fw = int(round(iw * soc / 100.0))
    if fw > 0:
        cv.rect(ix0, iy0, ix0 + fw, iy1, (m["fill_r"], m["fill_g"], m["fill_b"]))

    num = "%d%%" % soc
    if m["show_bolt"]:
        # centred group: [bolt][gap][NN%]
        bw = max(len(r) for r in BOLT_ROWS) * 2
        gap = 6
        grp = bw + gap + textw(num, 3)
        gx = (W - grp)//2
        cv.bitmap(gx, (H + 18 - len(BOLT_ROWS)*2)//2 - 2, BOLT_ROWS, (255, 235, 120), 2, outline=BG)
        cv.text_outline(gx + bw + gap, 41, num, INK, 3, BG)
    else:
        cv.text_outline((W - textw(num, 3))//2, 41, num, GREY if m["asleep"] else INK, 3, BG)

    if m["asleep"]:
        cv.text((W - textw("ASLEEP", 1))//2, 64, "ASLEEP", GREY, 1)
    return m["animating"]   # battery view still needs fast refresh while the SSID scrolls

# ── PORTRAIT (80x160) layout — mirrors main/display.cpp draw_portrait() 1:1 ────
# The BOOT button rotates the panel 90° per press; this is the portrait design (a VERTICAL
# battery filling bottom→top, a two-row header). Bytes-for-bytes the same "what to show"
# decision (decide() with orient="portrait") as the firmware; only the pixel coordinates differ.
PW, PH = 80, 160
PSB_N, PSB_BW, PSB_GAP, PSB_BASE = 5, 8, 5, 138       # centered searching bars

def draw_portrait_search_bars(cv, frame):
    total = PSB_N * PSB_BW + (PSB_N - 1) * PSB_GAP     # 60 px
    x0 = (cv.w - total) // 2
    p = frame % SRCH_CYCLE
    hp = (p / SRCH_STEPS) if p <= SRCH_HALF else ((SRCH_CYCLE - p) / SRCH_STEPS)
    for i in range(PSB_N):
        h = 14 + i * 9
        t = max(0.0, 1.0 - abs(i - hp) / SRCH_FALLOFF)
        col = lerp(BLE_GREEN, BLE_LIGHT, t)
        x = x0 + i * (PSB_BW + PSB_GAP)
        cv.rect(x, PSB_BASE - h, x + PSB_BW, PSB_BASE, col)

def draw_portrait_search(cv, frame, wifi):
    if wifi:
        cv.text((cv.w - textw("WiFi", 2)) // 2, 52, "WiFi", INK, 2)
    else:
        cv.bitmap((cv.w - 5 * 3) // 2, 46, BT_ROWS, INK, 3)   # BT glyph, scale 3
    draw_portrait_search_bars(cv, frame)

def draw_portrait_pairing(cv, frame):
    nd = (frame // 4) % 4
    cv.bitmap((cv.w - 5 * 3) // 2, 50, BT_ROWS, INK, 3)
    cv.text((cv.w - textw("Pairing...", 1)) // 2, 96, "Pairing" + "." * nd, INK, 1)

def compose_portrait(cv, st):
    cv.rect(0, 0, cv.w, cv.h, BG)
    inp = {
        "link":          st.get("link", "awake"),
        "wifi_on":       st.get("wifi", 0) > 0,
        "ssid":          st.get("ssid") or "-",
        "ble_connected": st.get("ble_on", False),
        "have_soc":      st.get("soc") is not None,
        "soc":           st.get("soc") or 0,
        "charging":      st.get("charging", False),
        "paired":        st.get("paired", True),
        "tick":          st.get("frame", 0),
        "orient":        "portrait",
    }
    m = decide(inp)
    f = inp["tick"]

    # header row 1: WiFi bars (left) | BT glyph + BLE bars (right); row 2: SSID; divider at y31
    if m["show_wifi"]:
        signal_bars(cv, 4, 4, st.get("wifi", 0))
        ssid = inp["ssid"]
        if not m["ssid_scrolling"]:
            cv.text((cv.w - textw(ssid, 1)) // 2, 21, ssid, INK, 1)
        else:
            cv.text_clip(4 - m["ssid_off"], 21, ssid, INK, 1, 4, 4 + m["ssid_avail"])
    if m["show_ble"]:
        bx = cv.w - 4 - 16
        signal_bars(cv, bx, 4, st.get("ble", 0) if inp["ble_connected"] else 0)
        cv.bitmap(bx - 12, 1, BT_ROWS, INK if inp["ble_connected"] else GREY, 2)
    cv.rect(4, 31, cv.w - 4, 32, DIV)

    if m["hero"] == "wifi_search":
        draw_portrait_search(cv, f, True);  return True
    if m["hero"] == "pairing":
        draw_portrait_pairing(cv, f);       return True
    if m["hero"] == "ble_search":
        draw_portrait_search(cv, f, False); return True

    # vertical battery: terminal nub on top, fill grows bottom→top
    cv.rect(30, 34, 50, 40, BORDER)
    bx0, by0, bx1, by1 = 10, 40, 70, 152
    cv.rect(bx0 + 2, by0 + 2, bx1 - 2, by1 - 2, PANEL)
    cv.frame(bx0, by0, bx1, by1, 3, BORDER)
    ix0, iy0, ix1, iy1 = bx0 + 3, by0 + 3, bx1 - 3, by1 - 3   # 13,43 .. 67,149
    ih = iy1 - iy0

    soc = m["soc"]
    fh = int(round(ih * soc / 100.0))
    if fh > 0:
        cv.rect(ix0, iy1 - fh, ix1, iy1, (m["fill_r"], m["fill_g"], m["fill_b"]))

    num = "%d%%" % soc
    nscale = 3 if soc < 100 else 2                # "100%" overflows 80 px at scale 3
    if m["show_bolt"]:
        bw = max(len(r) for r in BOLT_ROWS) * 2
        cv.bitmap((cv.w - bw) // 2, 60, BOLT_ROWS, (255, 235, 120), 2, outline=BG)
        cv.text_outline((cv.w - textw(num, nscale)) // 2, 100, num, INK, nscale, BG)
    else:
        cv.text_outline((cv.w - textw(num, nscale)) // 2, 92, num,
                        GREY if m["asleep"] else INK, nscale, BG)

    if m["asleep"]:
        cv.text((cv.w - textw("ASLEEP", 1)) // 2, 132, "ASLEEP", GREY, 1)
    return m["animating"]

# ── render helpers ────────────────────────────────────────────────────────────
def upscale(cv, S):
    bw, bh = cv.w*S, cv.h*S
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

def cmd_states_portrait(out="tools/display_states_portrait.png"):
    """Montage of the PORTRAIT (80x160) layout — the BOOT-rotated design — in every state."""
    states = [
        dict(wifi=4, ssid="Jupiter", ble=3, ble_on=True,  soc=64,  charging=True,  link="awake"),
        dict(wifi=3, ssid="Jupiter", ble=2, ble_on=True,  soc=5,   charging=False, link="awake"),
        dict(wifi=4, ssid="Jupiter", ble=3, ble_on=True,  soc=45,  charging=False, link="awake"),
        dict(wifi=4, ssid="Jupiter", ble=4, ble_on=True,  soc=100, charging=True,  link="awake"),
        dict(wifi=2, ssid="Jupiter", ble=2, ble_on=True,  soc=72,  charging=False, link="asleep"),
        dict(wifi=3, ssid="Jupiter", ble=0, ble_on=False, soc=None, charging=False, link="unreachable", frame=6),
        dict(wifi=0, ssid="Jupiter", ble=0, ble_on=False, soc=None, charging=False, link="unreachable", frame=6),
        dict(wifi=3, ssid="Jupiter", ble=4, ble_on=True,  paired=False, soc=None, charging=False, link="unreachable", frame=8),
    ]
    labels = ["charging 64%", "5% (red)", "45% (green)", "100% (full)",
              "asleep 72% (dim)", "searching BLE", "searching WiFi", "pairing"]
    S = 3
    pad, cap = 14, 26
    cellw, cellh = PW*S, PH*S + cap
    cols = 4
    rows = (len(states) + cols - 1)//cols
    bw = cols*cellw + (cols+1)*pad
    bh = rows*cellh + (rows+1)*pad
    big = [[(8,9,12) for _ in range(bw)] for _ in range(bh)]
    for idx, st in enumerate(states):
        cv = Canvas(PW, PH)
        compose_portrait(cv, st)
        up = upscale(cv, S)
        r, cc = idx//cols, idx % cols
        ox, oy = pad + cc*(cellw + pad), pad + r*(cellh + pad)
        for y in range(PH*S):
            for x in range(PW*S):
                big[oy+y][ox+x] = up[y][x]
        capcv = Canvas(PW*2, cap)
        capcv.text(2, 4, labels[idx][:18], INK, 1)
        capup = upscale(capcv, 1)
        for y in range(cap):
            for x in range(min(cellw, PW*2)):
                big[oy + PH*S + y][ox + x] = capup[y][x]
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

def cmd_scroll(out="tools/display_scroll.png"):
    """Render a too-long SSID at several marquee offsets to verify scroll + clipping."""
    st = dict(wifi=3, ssid="MyHomeNetwork5GHz", ble=3, ble_on=True, soc=64,
              charging=True, link="awake")
    # pick frames across one full ping-pong period for this SSID
    avail = 158 - 28 - 32
    span = textw(st["ssid"], 2) - avail
    travel = (span + SCROLL_SPEED - 1) // SCROLL_SPEED
    period = 2 * SCROLL_PAUSE + 2 * travel
    frames = [int(period * k / 8) for k in range(8)]
    S = 4
    pad, cap = 10, 16
    cellw, cellh = W*S, H*S + cap
    cols = 2
    rows = (len(frames) + cols - 1)//cols
    bw = cols*cellw + (cols+1)*pad
    bh = rows*cellh + (rows+1)*pad
    big = [[(8,9,12) for _ in range(bw)] for _ in range(bh)]
    for idx, f in enumerate(frames):
        cv = Canvas()
        compose(cv, dict(st, frame=f))
        up = upscale(cv, S)
        r, cc = idx//cols, idx % cols
        ox, oy = pad + cc*(cellw + pad), pad + r*(cellh + pad)
        for y in range(H*S):
            for x in range(W*S):
                big[oy+y][ox+x] = up[y][x]
        capcv = Canvas(); capcv.text(2, 4, "frame %d" % f, INK, 1)
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
    open(out, "w").write("\n".join(L) + "\n")
    print("wrote", out)

# ── parity: verify decide() against the C++ presenter's golden vectors ──────────
# Reads the TSV emitted by test/display_golden_dump.cpp (input columns + the C++
# tk::display::compose() decision), re-runs decide() on each input here, and diffs the
# decisions. Exit non-zero on any mismatch. Wired via scripts/check-display-sim-parity.sh.
OUT_FIELDS = ["hero", "show_wifi", "show_ble", "ssid_avail", "ssid_scrolling", "ssid_off",
              "out_soc", "fill_r", "fill_g", "fill_b", "asleep", "show_bolt", "animating"]
def _b01(x):
    return "1" if x else "0"
def cmd_parity(golden):
    rows = [ln.rstrip("\n") for ln in open(golden) if ln.strip() and not ln.startswith("#")]
    header = rows[0].split("\t")
    fails = n = 0
    for ln in rows[1:]:
        rec = dict(zip(header, ln.split("\t")))
        inp = {"link": rec["link"], "wifi_on": rec["wifi_on"] == "1", "ssid": rec["ssid"],
               "ble_connected": rec["ble_connected"] == "1", "have_soc": rec["have_soc"] == "1",
               "soc": int(rec["soc"]), "charging": rec["charging"] == "1",
               "paired": rec["paired"] == "1", "tick": int(rec["tick"]),
               "orient": rec.get("orient", "landscape")}
        m = decide(inp)
        got = {"hero": m["hero"], "show_wifi": _b01(m["show_wifi"]), "show_ble": _b01(m["show_ble"]),
               "ssid_avail": str(m["ssid_avail"]), "ssid_scrolling": _b01(m["ssid_scrolling"]),
               "ssid_off": str(m["ssid_off"]), "out_soc": str(m["soc"]),
               "fill_r": str(m["fill_r"]), "fill_g": str(m["fill_g"]), "fill_b": str(m["fill_b"]),
               "asleep": _b01(m["asleep"]), "show_bolt": _b01(m["show_bolt"]), "animating": _b01(m["animating"])}
        n += 1
        for k in OUT_FIELDS:
            if rec.get(k) != got[k]:
                fails += 1
                print("  MISMATCH case %d field %s: C++=%r sim=%r  (link=%s wifi_on=%s ssid=%r "
                      "ble=%s soc=%s chg=%s paired=%s tick=%s)" % (
                          n, k, rec.get(k), got[k], inp["link"], inp["wifi_on"], inp["ssid"],
                          inp["ble_connected"], inp["soc"], inp["charging"], inp["paired"], inp["tick"]))
    if fails == 0:
        print("OK  sim decide() matches the C++ presenter on %d golden cases" % n)
        return 0
    print("FAILED  %d field mismatch(es) across %d cases" % (fails, n))
    return 1

if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "png"
    arg  = sys.argv[2] if len(sys.argv) > 2 else None
    if mode == "png":
        cmd_png(arg or "tools/display_preview.png")
    elif mode == "states":
        cmd_states(arg or "tools/display_states.png")
    elif mode == "states-portrait":
        cmd_states_portrait(arg or "tools/display_states_portrait.png")
    elif mode == "search":
        cmd_search(arg or "tools/display_search.png")
    elif mode == "scroll":
        cmd_scroll(arg or "tools/display_scroll.png")
    elif mode == "cheader":
        cmd_cheader(arg or "main/display_font.h")
    elif mode == "parity":
        sys.exit(cmd_parity(arg or "build_mock/display_golden.tsv"))
    else:
        print("usage: display_sim.py [png|states|states-portrait|search|scroll|cheader|parity] [outfile]")
