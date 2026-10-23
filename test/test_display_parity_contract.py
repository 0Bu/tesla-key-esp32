#!/usr/bin/env python3
"""Reject incomplete/malformed actual display goldens without a board or third-party module."""
from pathlib import Path
import subprocess
import sys
import tempfile
import os
import re
import runpy
import shutil
import argparse

ROOT = Path(__file__).resolve().parent.parent

def text_pixel_probes(sanitize=False) -> int:
    """Execute the real framebuffer/text primitives, including clipping and byte fallback."""
    sim = runpy.run_path(str(ROOT / "tools/display_sim.py"))
    assert set(map(chr, range(32, 127))) <= set(sim["G"]), "incomplete printable font"
    for char in map(chr, range(33, 127)):
        rows = sim["glyph"](char)
        assert len(rows) == 7 and all(len(row) == 5 and set(row) <= {"#", "."} for row in rows)
        assert any("#" in row for row in rows), ("blank printable glyph", char)
    assert sim["glyph"]("p") != sim["glyph"]("P")
    assert sim["glyph"]("g") != sim["glyph"]("9")
    source = (ROOT / "main/display.cpp").read_text()
    body_in = runpy.run_path(str(ROOT / "test/test_runtime_boundary_contract.py"))["function_body_in"]
    functions = []
    for name in ("put", "text_w", "draw_char", "draw_text", "draw_text_outline", "draw_text_clip"):
        match = re.search(r"^static (?:inline )?(?:void|int) " + name + r"\(", source, re.M)
        assert match is not None, name
        body = body_in(source, name)
        functions.append(source[match.start():source.index(body, match.start())] + body)
    # All byte values exercise native signed-char rejection. String cases cover UTF-8,
    # embedded NUL, real punctuation, outlines and clips at three integer scales.
    cases = [(0, byte, scale, -2, 1) for byte in range(256) for scale in (1, 2, 3)]
    for text in ("", "Pairing...", "pPg9", "Demo_2.4G!", "[]{}~!?", "Grüße", "A☀B", "A\0B"):
        cases.extend((mode, text, scale, -2, -1) for mode in (1, 2, 3) for scale in (1, 2, 3))
    lines, expected = [], []
    for mode, value, scale, x, y in cases:
        canvas = sim["Canvas"](48, 28)
        ink, outline = (1, 1, 1), (2, 2, 2)
        if mode == 0:
            canvas.char(x, y, chr(value), ink, scale)
            width, end = 6 * scale, x + 6 * scale
            call = f"draw_char({x}, {y}, static_cast<char>({value}), 1, {scale});"
            native = f"int width = {width}, end = {end}; {call}"
        else:
            data = ",".join(f"static_cast<char>({byte})" for byte in value.encode("utf-8"))
            native = "const char text[] = {" + (data + "," if data else "") + "0}; "
            width = sim["textw"](value, scale)
            end = x + width
            native += f"int width = text_w(text, {scale}), end = {x} + width; "
            if mode == 1:
                assert canvas.text(x, y, value, ink, scale) == end
                native += f"end = draw_text({x}, {y}, text, 1, {scale});"
            elif mode == 2:
                canvas.text_clip(x, y, value, ink, scale, 4, 23)
                native += f"draw_text_clip({x}, {y}, text, 1, {scale}, 4, 23);"
            else:
                canvas.text_outline(x, y, value, ink, scale, outline)
                native += f"draw_text_outline({x}, {y}, text, 1, {scale}, 2);"
        pixels = "".join(str(1 if pixel == ink else 2 if pixel == outline else 0)
                         for row in canvas.c for pixel in row)
        expected.append(f"{width}\t{end}\t{pixels}")
        lines.append("{ std::fill(std::begin(s_fb), std::end(s_fb), 0); " + native +
                     ' printf("%d\\t%d\\t", width, end); for (auto pixel : s_fb) putchar(\'0\' + pixel); putchar(\'\\n\'); }')
    code = '''#include <algorithm>
#include <iterator>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "display_font.h"
static int W = 48, H = 28;
static uint16_t s_fb[48 * 28]{};
''' + "\n".join(functions) + "\nint main() {\n" + "\n".join(lines) + "\n}\n"
    compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
    assert compiler, "a C++17 compiler is required"
    with tempfile.TemporaryDirectory(prefix="display-text-pixels-") as directory:
        work = Path(directory)
        cpp, binary = work / "text.cpp", work / "text"
        cpp.write_text(code)
        extra = ["-O1", "-g", "-fno-omit-frame-pointer", "-fno-sanitize-recover=all",
                 "-fsanitize=address,undefined,leak"] if sanitize else []
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsigned-char", *extra,
                        "-I" + str(ROOT / "main"), str(cpp), "-o", str(binary)], check=True, timeout=60)
        result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=10)
    rows = result.stdout.splitlines()
    assert len(rows) == len(expected), "incomplete actual text pixel output"
    for index, (actual, want) in enumerate(zip(rows, expected)):
        assert actual == want, ("actual text pixels or byte width", cases[index])
    return len(cases)

def frame_probes(sanitize=False) -> int:
    """Execute actual frame assembly/layout bodies with passive drawing and cache doubles."""
    source = (ROOT / "main/display.cpp").read_text()
    body_in = runpy.run_path(str(ROOT / "test/test_runtime_boundary_contract.py"))["function_body_in"]
    functions = {}
    for name in ("text_w", "draw_landscape", "draw_portrait", "render_frame"):
        match = re.search(r"^static (?:bool|int) " + name + r"\(", source, re.M)
        assert match is not None, name
        body = body_in(source, name)
        functions[name] = source[match.start():source.index(body, match.start())] + body
    code = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "logic/display_model.hpp"
#include "display_font.h"
static int W = 160, H = 80, s_rot;
struct RotDef { bool portrait; };
static constexpr RotDef ROTS[] = {{false}, {true}};
static constexpr uint16_t C_BG = 0, C_PANEL = 1, C_BORDER = 2, C_INK = 3,
    C_GREY = 4, C_DIV = 5, C_BAR_ON = 6, C_BOLT = 7;
static constexpr int SRCH_BT_X = 24, SRCH_BT_Y = 36;
struct Text { int x, y, scale; std::string text; bool outlined; };
struct Border { int x0, y0, x1, y1, thickness; };
static std::vector<Text> texts;
static std::vector<Border> borders;
static bool recovery, fail_cache;
static unsigned cache_reads, safe_reads, fills;
static tk::NetLink net_value = tk::NetLink::Eth;
struct CacheFault {};
struct VehicleController {
    tk::UiSnapshot ui_snapshot() {
        ++cache_reads;
        if (fail_cache) throw CacheFault{};
        tk::UiSnapshot s{};
        s.link_state = tk::LinkState::Awake; s.have_soc = true; s.soc = 55;
        return s;
    }
};
namespace tk {
bool safe_mode_active() { ++safe_reads; return recovery; }
NetLink net_kind() { return net_value; }
bool net_eth_present() { return true; }
bool net_wifi_signal(int*, char*) { return false; }
}
''' + functions["text_w"] + r'''
static int draw_text(int x, int y, const char* text, uint16_t, int scale) {
    texts.push_back({x, y, scale, text, false}); return x + text_w(text, scale);
}
static void draw_text_outline(int x, int y, const char* text, uint16_t, int scale, uint16_t) {
    texts.push_back({x, y, scale, text, true});
}
static void border(int x0, int y0, int x1, int y1, int thickness, uint16_t) {
    borders.push_back({x0, y0, x1, y1, thickness});
}
static void fill(uint16_t) { ++fills; }
template<typename... T> void rect(T...) {}
template<typename... T> void signal_bars(T...) {}
template<typename... T> void blit_rows(T...) {}
template<typename... T> void blit_rows_outline(T...) {}
template<typename... T> void draw_text_clip(T...) {}
template<typename... T> void draw_searching_text(T...) {}
template<typename... T> void draw_searching_icon(T...) {}
template<typename... T> void draw_pairing(T...) {}
template<typename... T> void draw_portrait_search(T...) {}
template<typename... T> void draw_portrait_pairing(T...) {}
uint16_t rgb565(uint8_t, uint8_t, uint8_t) { return 0; }
''' + "\n".join(functions[name] for name in ("draw_landscape", "draw_portrait", "render_frame")) + r'''
static bool has_text(const char* text) {
    return std::any_of(texts.begin(), texts.end(), [text](const Text& item) { return item.text == text; });
}
static void require_recovery_text_bounds() {
    for (size_t i = 0; i < texts.size(); ++i) {
        const auto& a = texts[i];
        assert(a.scale > 0 && !a.text.empty());
        const int right = a.x + text_w(a.text.c_str(), a.scale) - a.scale;
        const int bottom = a.y + 7 * a.scale;
        assert(right > a.x && bottom > a.y);
        assert(!a.outlined && a.x >= 0 && a.y >= 0 && right <= W && bottom <= H);
        for (size_t j = 0; j < i; ++j) {
            const auto& b = texts[j];
            const int other_right = b.x + text_w(b.text.c_str(), b.scale) - b.scale;
            assert(right <= b.x || other_right <= a.x || bottom <= b.y || b.y + 7 * b.scale <= a.y);
        }
    }
}
int main() {
    VehicleController vehicle;
    for (s_rot = 0; s_rot < 2; ++s_rot) {
        W = s_rot ? 80 : 160; H = s_rot ? 160 : 80;
        for (bool safe : {false, true}) for (bool fail : {false, true}) {
            recovery = safe; fail_cache = fail; cache_reads = safe_reads = fills = 0;
            texts.clear(); borders.clear(); bool threw = false;
            try { assert(!render_frame(vehicle, 0, true)); } catch (const CacheFault&) { threw = true; }
            assert(threw == (!safe && fail));
            assert(safe_reads == 1 && cache_reads == unsigned(!safe));
            assert(fills == unsigned(!threw));
            if (safe) {
                require_recovery_text_bounds();
                assert(borders.empty() && has_text("Open web UI") && !has_text("Pairing"));
                assert(s_rot ? has_text("SAFE") && has_text("MODE") : has_text("SAFE MODE"));
            } else if (!threw) { assert(borders.size() == 1 && has_text("55%")); }
        }
        net_value = tk::NetLink::None;
        for (bool fail : {false, true}) {
            recovery = true; fail_cache = fail; cache_reads = safe_reads = fills = 0;
            texts.clear(); borders.clear();
            assert(!render_frame(vehicle, 0, true));
            assert(cache_reads == 0 && safe_reads == 1 && fills == 1 && borders.empty());
            require_recovery_text_bounds();
            assert(has_text("Check network") && !has_text("Open web UI"));
            assert(s_rot ? has_text("SAFE") && has_text("MODE") : has_text("SAFE MODE"));
        }
        net_value = tk::NetLink::Eth;
        for (int soc = 0; soc <= 100; ++soc) for (bool charging : {false, true})
            for (bool asleep : {false, true}) {
                tk::UiSnapshot s{}; s.net = tk::NetLink::Eth; s.have_soc = true;
                s.link_state = asleep ? tk::LinkState::Asleep : tk::LinkState::Awake;
                s.soc = soc; s.charging = charging; s.paired = true;
                const auto model = tk::display::compose(s, 0, s_rot ? tk::display::Orient::Portrait : tk::display::Orient::Landscape);
                texts.clear(); borders.clear();
                assert(!(s_rot ? draw_portrait(model, s, 0) : draw_landscape(model, s, 0)));
                assert(borders.size() == 1);
                const auto b = borders.front(); unsigned numbers = 0;
                for (const auto& text : texts) if (text.outlined) {
                    ++numbers;
                    // Bound the actual painted glyph extent plus its one-pixel outline.
                    assert(text.x - 1 >= b.x0 + b.thickness);
                    assert(text.x + text_w(text.text.c_str(), text.scale) - text.scale + 1 <= b.x1 - b.thickness);
                    assert(text.y - 1 >= b.y0 + b.thickness);
                    assert(text.y + 7 * text.scale + 1 <= b.y1 - b.thickness);
                    if (asleep && !s_rot) {
                        const auto label = std::find_if(texts.begin(), texts.end(), [](const Text& t) { return t.text == "ASLEEP"; });
                        assert(label != texts.end() && label->y - (text.y + 7 * text.scale + 1) >= 4);
                    }
                }
                assert(numbers == 1);
            }
    }
}
'''
    compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
    assert compiler, "a C++17 compiler is required"
    extra = ["-O1", "-g", "-fno-omit-frame-pointer", "-fno-sanitize-recover=all",
             "-fsanitize=address,undefined,leak"] if sanitize else []
    with tempfile.TemporaryDirectory(prefix="display-frame-native-") as directory:
        cpp, binary = Path(directory) / "frame.cpp", Path(directory) / "frame"
        cpp.write_text(code)
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", *extra,
                        "-I" + str(ROOT / "main"), str(cpp), "-o", str(binary)], check=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=10)
    return 820

def run(mode: str, path: Path) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(ROOT / "tools/display_sim.py"), mode, str(path)],
                          capture_output=True, text=True, timeout=10)

def consumer_probes() -> int:
    import importlib.util
    spec = importlib.util.spec_from_file_location("actual_display_sim", ROOT / "tools/display_sim.py")
    assert spec is not None and spec.loader is not None
    sim = importlib.util.module_from_spec(spec); spec.loader.exec_module(sim)
    original_decide, original_bars = sim.decide, sim.signal_bars
    decisions, calls = [], []
    def observe(inp):
        model = original_decide(inp); decisions.append((dict(inp), dict(model))); return model
    def bars(_canvas, _x, _y, level, *_args, **_kwargs):
        calls.append(level)
    class PassiveCanvas(sim.Canvas):
        def __init__(self, width, height):
            super().__init__(width, height); self.header_glyph = []; self.network_labels = []
            self.recovery_labels = []; self.all_text = []
        def text(self, x, y, text, color, scale=1):
            self.all_text.append(text)
            if text in ("LAN", "WiFi"):
                self.network_labels.append(text)
            if text in ("SAFE MODE", "SAFE", "MODE", "Open web UI", "Check network"):
                self.recovery_labels.append(text)
            return super().text(x, y, text, color, scale)
        def bitmap(self, x, y, rows, color, scale=1, outline=None):
            if rows is sim.BT_ROWS and y == 1 and scale == 2:
                self.header_glyph.append(color)
    sim.decide, sim.signal_bars = observe, bars
    count = 0
    try:
        for portrait in (False, True):
            for net in ("none", "wifi", "eth"):
                for connected in (False, True):
                    for valid in (False, True):
                        for wifi in (-95, -80, -70, -60, -50):
                            for ble in (-95, -80, -70, -60, -50):
                                state = {"net": net, "ssid": "Home", "link": "idle", "soc": 55,
                                         "paired": True, "ble_on": connected, "wifi_rssi": wifi,
                                         "ble_rssi": ble, "ble_rssi_valid": valid, "frame": 0}
                                decisions.clear(); calls.clear()
                                canvas = PassiveCanvas(80 if portrait else 160, 160 if portrait else 80)
                                (sim.compose_portrait if portrait else sim.compose)(canvas, state)
                                assert len(decisions) == 1
                                inp, model = decisions[0]
                                expected_input = {"link": "idle", "net": net, "eth_present": False, "safe_mode": False,
                                                  "ssid": "Home", "ble_connected": connected,
                                                  "have_soc": True, "soc": 55, "charging": False,
                                                  "paired": True, "tick": 0, "wifi_rssi": wifi,
                                                  "ble_rssi_valid": valid, "ble_rssi": ble,
                                                  "orient": "portrait" if portrait else "landscape"}
                                actual_input = dict(inp)
                                actual_input.setdefault("orient", "landscape")
                                assert actual_input == expected_input, ("actual decide inputs", portrait, state, actual_input)
                                expected = ([model["wifi_bars"]] if model["show_wifi"] else []) + ([model["ble_bars"]] if model["show_ble"] else [])
                                assert calls == expected, ("actual signal consumer", portrait, state, calls, expected)
                                glyph = [sim.INK if model["ble_glyph_on"] else sim.GREY] if model["show_ble"] else []
                                assert canvas.header_glyph == glyph, ("actual glyph consumer", portrait, state)
                                count += 1
        for portrait in (False, True):
            for ethernet in (False, True):
                canvas = PassiveCanvas(80 if portrait else 160, 160 if portrait else 80)
                state = {"net": "none", "eth_present": ethernet, "soc": None, "frame": 6}
                (sim.compose_portrait if portrait else sim.compose)(canvas, state)
                assert canvas.network_labels == ["LAN" if ethernet else "WiFi"], ("actual network search label", portrait, ethernet)
                count += 1
        for portrait in (False, True):
            for net in ("none", "wifi", "eth"):
                for ssid in ("Home", "LongDemoNetworkName"):
                    canvas = PassiveCanvas(80 if portrait else 160, 160 if portrait else 80)
                    state = {"net": net, "safe_mode": True, "ssid": ssid,
                             "soc": 99, "charging": True, "ble_on": True, "paired": False}
                    decisions.clear()
                    (sim.compose_portrait if portrait else sim.compose)(canvas, state)
                    assert len(decisions) == 1 and decisions[0][0]["safe_mode"] is True
                    assert decisions[0][1]["hero"] == "safe_mode"
                    hint = "Check network" if net == "none" else "Open web UI"
                    expected = ["SAFE", "MODE", hint] if portrait else ["SAFE MODE", hint]
                    assert canvas.recovery_labels == expected, ("actual recovery labels", portrait, net, ssid)
                    assert not canvas.header_glyph and not any("%" in text for text in canvas.all_text)
                    count += 1
    finally:
        sim.decide, sim.signal_bars = original_decide, original_bars
    return count

def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--render-only", action="store_true")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    print(f"display text pixels: PASS ({text_pixel_probes(args.sanitize)} actual native primitive cases)")
    print(f"display frames: PASS ({frame_probes(args.sanitize)} actual assembly/layout cases with passive drawing doubles)")
    if args.render_only:
        return
    negatives = 0
    with tempfile.TemporaryDirectory(prefix="display-golden-controls-") as directory:
        work = Path(directory)
        for mode, name in (("parity", "display_golden.tsv"), ("font-parity", "display_font_golden.tsv")):
            source = ROOT / "build_mock" / name
            assert run(mode, source).returncode == 0
            rows = source.read_text().splitlines(True)
            changes = {"empty": "", "header-only": rows[0], "one-record": "".join(rows[:2]),
                       "missing-last": "".join(rows[:-1]),
                       "malformed-row": rows[0] + "\t".join(rows[1].rstrip("\n").split("\t")[:-1]) + "\n" + "".join(rows[2:]),
                       "duplicate-case": "".join(rows[:-1]) + rows[1],
                       "unknown-header": rows[0].replace(rows[0].split("\t")[0], "unknown", 1) + "".join(rows[1:])}
            if mode == "parity":
                header = rows[0].rstrip("\n").split("\t")
                for field, value in ([(field, "2") for field in ("eth_present", "ble_connected", "have_soc", "charging", "paired", "ble_rssi_valid", "safe_mode")]
                                     + [("link", "bad"), ("net", "bad"), ("orient", "bad"),
                                        ("soc", "+01"), ("tick", "-1"), ("ble_rssi", "2147483648")]):
                    values = rows[1].rstrip("\n").split("\t"); values[header.index(field)] = value
                    changes["invalid-input-" + field] = rows[0] + "\t".join(values) + "\n" + "".join(rows[2:])
            for label, data in changes.items():
                path = work / (mode + "-" + label + ".tsv"); path.write_text(data)
                result = run(mode, path)
                assert result.returncode != 0, (mode, label, result.stdout)
                negatives += 1
            assert run(mode, source).returncode == 0
    consumer_cases = consumer_probes()
    print(f"display consumers: PASS ({consumer_cases} actual layout calls)")
    print(f"display golden contract: PASS (4 positive, {negatives} rejection cases)")

if __name__ == "__main__":
    main()
