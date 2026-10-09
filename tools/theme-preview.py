#!/usr/bin/env python3
"""Render themes/previews/<name>.svg for every themes/*.theme.

These are illustrative mock-ups drawn from the theme file (colors, border, titlebar, corner
radius, button size, shadow), not screenshots of the compositor. With --check nothing is
written; the exit status is 1 when a preview is missing or out of date (used by the tests).

Usage: tools/theme-preview.py [--check] [themes-dir] [out-dir]
"""
import configparser
import glob
import os
import sys

W, H = 560, 330

FONTS = {
    "sans": "sans-serif",
    "monospace": "monospace",
    "serif": "serif",
}


def load(path):
    cp = configparser.ConfigParser(interpolation=None, inline_comment_prefixes=None)
    cp.optionxform = str
    with open(path, encoding="utf-8") as f:
        cp.read_string("[top]\n" + f.read())
    return cp


def color(value):
    """'#rrggbb' or '#rrggbbaa' -> (svg color, opacity)"""
    v = value.strip().lstrip("#")
    if len(v) == 8:
        return "#" + v[:6], int(v[6:], 16) / 255
    return "#" + v, 1.0


def mix(a, b, amount):
    """blend color a toward color b (both '#rrggbb') by `amount` (0..1)"""
    ca = [int(a[i:i + 2], 16) for i in (1, 3, 5)]
    cb = [int(b[i:i + 2], 16) for i in (1, 3, 5)]
    return "#" + "".join("%02x" % round(x + (y - x) * amount) for x, y in zip(ca, cb))


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")


def window(t, x, y, w, h, title, focused, content_bars):
    bw = t["bw"]
    th = t["th"]
    r = t["r"]
    border = t["bf"] if focused else t["bu"]
    bar = t["tf"] if focused else t["tu"]
    out = []
    # frame: border colored rounded rect, then titlebar and client area inside it
    out.append(f'<g filter="url(#shadow)">' if t["shadow"] else "<g>")
    out.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{r}" fill="{border}"/>')
    out.append(
        f'<path d="M{x + bw} {y + th + bw} V{y + bw + r} Q{x + bw} {y + bw} {x + bw + r} {y + bw} '
        f'H{x + w - bw - r} Q{x + w - bw} {y + bw} {x + w - bw} {y + bw + r} V{y + th + bw} Z" fill="{bar}"/>'
        if r else f'<rect x="{x + bw}" y="{y + bw}" width="{w - 2 * bw}" height="{th}" fill="{bar}"/>'
    )
    cy = y + bw + th
    out.append(f'<rect x="{x + bw}" y="{cy}" width="{w - 2 * bw}" height="{h - th - 2 * bw}" fill="{mix(t["bg"], t["tx"], 0.07)}"/>')
    out.append("</g>")
    # title text
    fs = t["fs"] + 2
    out.append(
        f'<text x="{x + bw + 12}" y="{y + bw + th / 2 + fs * 0.35:.1f}" font-family="{esc(t["font"])}" '
        f'font-size="{fs}" fill="{t["tx"]}">{esc(title)}</text>'
    )
    # buttons, right aligned: close, maximize, minimize (right to left)
    bs = t["bs"]
    bx = x + w - bw - 10 - bs / 2
    by = y + bw + th / 2
    for c in (t["c"], t["m"], t["n"]):
        out.append(f'<circle cx="{bx:.1f}" cy="{by:.1f}" r="{bs / 2:.1f}" fill="{c}"/>')
        bx -= bs + t["sp"]
    # mock client content
    for i, ratio in enumerate(content_bars):
        out.append(
            f'<rect x="{x + bw + 14}" y="{cy + 16 + i * 18}" width="{(w - 2 * bw - 28) * ratio:.0f}" height="8" '
            f'rx="3" fill="{t["tx"]}" opacity="0.28"/>'
        )
    return "\n".join(out)


def render(path):
    cp = load(path)
    g = lambda sec, key, default=None: cp.get(sec, key, fallback=default)
    sh_color, sh_op = color(g("shadow", "color", "#00000066"))
    t = {
        "name": g("top", "name", os.path.basename(path)),
        "bg": color(g("colors", "background"))[0],
        "bf": color(g("colors", "border_focused"))[0],
        "bu": color(g("colors", "border_unfocused"))[0],
        "tf": color(g("colors", "titlebar_focused"))[0],
        "tu": color(g("colors", "titlebar_unfocused"))[0],
        "tx": color(g("colors", "title_text"))[0],
        "c": color(g("colors", "close_button"))[0],
        "m": color(g("colors", "maximize_button"))[0],
        "n": color(g("colors", "minimize_button"))[0],
        "bw": int(g("geometry", "border_width")),
        "th": int(g("geometry", "titlebar_height")),
        "r": int(g("geometry", "corner_radius")),
        "bs": int(g("geometry", "button_size")),
        "sp": int(g("geometry", "button_spacing")),
        "shadow": g("shadow", "enabled", "true").lower() in ("true", "yes", "on", "1"),
        "sh_r": int(g("shadow", "radius", "0")),
        "sh_dy": int(g("shadow", "offset_y", "0")),
        "sh_c": sh_color,
        "sh_o": sh_op,
        "font": FONTS.get(g("font", "family", "sans").lower(), g("font", "family", "sans") + ", sans-serif"),
        "fs": int(g("font", "size", "10")),
    }
    # the desktop shows the theme's background (what a wallpaper tool would use); the client
    # area of the mock windows is that color lifted slightly toward the text color
    desk = t["bg"]
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" role="img" '
        f'aria-label="Mock-up of the {esc(t["name"])} theme">',
        f"<title>{esc(t['name'])} (mock-up generated from the theme file)</title>",
        "<defs>",
    ]
    if t["shadow"]:
        parts.append(
            f'<filter id="shadow" x="-20%" y="-20%" width="140%" height="150%">'
            f'<feDropShadow dx="0" dy="{t["sh_dy"]}" stdDeviation="{max(t["sh_r"] / 2, 0.1):.1f}" '
            f'flood-color="{t["sh_c"]}" flood-opacity="{t["sh_o"]:.2f}"/></filter>'
        )
    parts.append("</defs>")
    parts.append(f'<rect width="{W}" height="{H}" fill="{desk}"/>')
    parts.append(window(t, 36, 40, 330, 190, "Files", False, [0.8, 0.55, 0.7, 0.4]))
    parts.append(window(t, 170, 105, 350, 190, "Terminal", True, [0.5, 0.75, 0.35, 0.6, 0.45]))
    parts.append("</svg>")
    return "\n".join(parts) + "\n"


def main(argv):
    check = "--check" in argv
    args = [a for a in argv if not a.startswith("--")]
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    themes = args[0] if args else os.path.join(root, "themes")
    out = args[1] if len(args) > 1 else os.path.join(root, "themes", "previews")
    stale = []
    for path in sorted(glob.glob(os.path.join(themes, "*.theme"))):
        name = os.path.splitext(os.path.basename(path))[0]
        svg = render(path)
        dst = os.path.join(out, name + ".svg")
        current = open(dst, encoding="utf-8").read() if os.path.exists(dst) else None
        if current != svg:
            stale.append(name)
            if not check:
                os.makedirs(out, exist_ok=True)
                with open(dst, "w", encoding="utf-8") as f:
                    f.write(svg)
    if check:
        if stale:
            print("previews missing or out of date (run tools/theme-preview.py): " + ", ".join(stale))
            return 1
        print("theme previews are up to date")
        return 0
    print("wrote %d previews" % len(stale) if stale else "previews already up to date")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
