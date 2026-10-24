#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the topo contributors. Part of topo (fork of btop for Apple Silicon): see NOTICE.
"""Converte una cattura ANSI di un programma a schermo intero (es. topo in un pty) in un'immagine SVG.

Solo libreria standard: si esegue con `python3 -I tools/ansi2svg.py`.

Uso:
    python3 -I tools/ansi2svg.py CATTURA.raw COLONNE RIGHE OUT.svg [--title TITOLO] [--font-size 14]

La cattura e' il flusso di byte scritto dal programma (sequenze CSI di posizionamento, cancellazione e
colore SGR, compresi i colori a 24 bit). Il terminale emulato e' minimale ma basta per btop/topo:
cursore (H f A B C D G d s u, ESC 7/8), cancellazioni (J K X), colori (SGR 0 1 2 3 4 7 22-27 30-49
90-107, 38/48;5 e 38/48;2) e caratteri larghi. Ogni sequenza di celle con gli stessi attributi diventa
un <text> con textLength fisso, cosi' riquadri e grafici braille restano allineati con qualunque font.
"""

import argparse
import html
import re
import sys
import unicodedata

# palette xterm dei 16 colori base
BASE16 = [
    (0, 0, 0), (205, 0, 0), (0, 205, 0), (205, 205, 0), (0, 0, 238), (205, 0, 205), (0, 205, 205), (229, 229, 229),
    (127, 127, 127), (255, 0, 0), (0, 255, 0), (255, 255, 0), (92, 92, 255), (255, 0, 255), (0, 255, 255), (255, 255, 255),
]
DEFAULT_FG = (204, 204, 204)
DEFAULT_BG = (0, 0, 0)


def xterm256(n):
    if n < 16:
        return BASE16[n]
    if n < 232:
        n -= 16
        levels = [0, 95, 135, 175, 215, 255]
        return (levels[n // 36], levels[(n // 6) % 6], levels[n % 6])
    v = 8 + (n - 232) * 10
    return (v, v, v)


class Attr:
    __slots__ = ("fg", "bg", "bold", "dim", "italic", "underline", "reverse")

    def __init__(self):
        self.reset()

    def reset(self):
        self.fg = None
        self.bg = None
        self.bold = self.dim = self.italic = self.underline = self.reverse = False

    def key(self):
        fg = self.fg or DEFAULT_FG
        bg = self.bg or DEFAULT_BG
        if self.reverse:
            fg, bg = bg, fg
        if self.dim:
            fg = tuple(c * 2 // 3 for c in fg)
        return (fg, bg, self.bold, self.italic, self.underline)


def apply_sgr(attr, params):
    ps = [int(p) if p.isdigit() else 0 for p in params.split(";")] if params else [0]
    i = 0
    while i < len(ps):
        p = ps[i]
        if p == 0:
            attr.reset()
        elif p == 1:
            attr.bold = True
        elif p == 2:
            attr.dim = True
        elif p == 3:
            attr.italic = True
        elif p == 4:
            attr.underline = True
        elif p == 7:
            attr.reverse = True
        elif p == 22:
            attr.bold = attr.dim = False
        elif p == 23:
            attr.italic = False
        elif p == 24:
            attr.underline = False
        elif p == 27:
            attr.reverse = False
        elif 30 <= p <= 37:
            attr.fg = BASE16[p - 30]
        elif p == 39:
            attr.fg = None
        elif 40 <= p <= 47:
            attr.bg = BASE16[p - 40]
        elif p == 49:
            attr.bg = None
        elif 90 <= p <= 97:
            attr.fg = BASE16[p - 90 + 8]
        elif 100 <= p <= 107:
            attr.bg = BASE16[p - 100 + 8]
        elif p in (38, 48) and i + 1 < len(ps):
            if ps[i + 1] == 5 and i + 2 < len(ps):
                color = xterm256(ps[i + 2] % 256)
                i += 2
            elif ps[i + 1] == 2 and i + 4 < len(ps):
                color = (ps[i + 2] % 256, ps[i + 3] % 256, ps[i + 4] % 256)
                i += 4
            else:
                color = None
            if p == 38:
                attr.fg = color
            else:
                attr.bg = color
        i += 1


def char_width(c):
    if unicodedata.combining(c):
        return 0
    return 2 if unicodedata.east_asian_width(c) in ("W", "F") else 1


def emulate(data, cols, rows):
    blank = lambda a: (" ", a.key())
    attr = Attr()
    screen = [[blank(attr) for _ in range(cols)] for _ in range(rows)]
    y = x = 0
    saved = (0, 0)
    csi = re.compile(r"\x1b\[([0-9;?<>=]*)([ -/]*)([@-~])")
    i, n = 0, len(data)

    def clamp():
        nonlocal y, x
        y = max(0, min(rows - 1, y))
        x = max(0, min(cols - 1, x))

    while i < n:
        c = data[i]
        if c == "\x1b":
            m = csi.match(data, i)
            if m:
                params, _, final = m.groups()
                i = m.end()
                if params.startswith(("?", "<", ">", "=")):
                    continue
                ps = [int(p) if p.isdigit() else 0 for p in params.split(";")] if params else []
                p1 = ps[0] if ps and ps[0] else 1
                if final == "m":
                    apply_sgr(attr, params)
                    continue
                if final in "Hf":
                    y = (ps[0] if len(ps) > 0 and ps[0] else 1) - 1
                    x = (ps[1] if len(ps) > 1 and ps[1] else 1) - 1
                elif final == "A":
                    y -= p1
                elif final == "B":
                    y += p1
                elif final == "C":
                    x += p1
                elif final == "D":
                    x -= p1
                elif final == "G":
                    x = p1 - 1
                elif final == "d":
                    y = p1 - 1
                elif final == "J":
                    mode = ps[0] if ps else 0
                    if mode in (2, 3):
                        screen = [[blank(attr) for _ in range(cols)] for _ in range(rows)]
                    elif mode == 0:
                        for xx in range(x, cols):
                            screen[y][xx] = blank(attr)
                        for yy in range(y + 1, rows):
                            screen[yy] = [blank(attr) for _ in range(cols)]
                    elif mode == 1:
                        for yy in range(0, y):
                            screen[yy] = [blank(attr) for _ in range(cols)]
                        for xx in range(0, x + 1):
                            screen[y][xx] = blank(attr)
                elif final == "K":
                    mode = ps[0] if ps else 0
                    rng = range(x, cols) if mode == 0 else (range(0, x + 1) if mode == 1 else range(cols))
                    for xx in rng:
                        screen[y][xx] = blank(attr)
                elif final == "X":
                    for xx in range(x, min(cols, x + p1)):
                        screen[y][xx] = blank(attr)
                elif final == "s":
                    saved = (y, x)
                elif final == "u":
                    y, x = saved
                clamp()
                continue
            if data.startswith("\x1b]", i):  # OSC ... BEL / ST
                ends = [e for e in (data.find("\x07", i), data.find("\x1b\\", i)) if e != -1]
                if not ends:
                    break
                e = min(ends)
                i = e + (1 if data[e] == "\x07" else 2)
                continue
            nxt = data[i + 1:i + 2]
            if nxt == "7":
                saved = (y, x)
            elif nxt == "8":
                y, x = saved
            i += 3 if nxt in ("(", ")") else 2
            continue
        i += 1
        if c == "\r":
            x = 0
        elif c == "\n":
            y = min(rows - 1, y + 1)
        elif c == "\b":
            x = max(0, x - 1)
        elif c < " " or c == "\x7f":
            pass
        else:
            w = char_width(c)
            if w == 0:
                continue
            if x < cols:
                screen[y][x] = (c, attr.key())
                if w == 2 and x + 1 < cols:
                    screen[y][x + 1] = ("", attr.key())
            x = min(cols - 1, x + w) if x + w < cols else cols - 1
    return screen


def hexcolor(rgb):
    return "#{:02x}{:02x}{:02x}".format(*rgb)


def to_svg(screen, cols, rows, font_size, title):
    cw = font_size * 0.6
    ch = font_size * 1.25
    pad = 8
    top = pad + (ch + 6 if title else 0)
    width = cols * cw + 2 * pad
    height = rows * ch + top + pad
    page_bg = screen[0][0][1][1] if screen and screen[0] else DEFAULT_BG
    out = [
        '<svg xmlns="http://www.w3.org/2000/svg" width="{:.0f}" height="{:.0f}" viewBox="0 0 {:.1f} {:.1f}">'.format(width, height, width, height),
        "<style>text{{font-family:Menlo,'SF Mono','DejaVu Sans Mono',Consolas,monospace;font-size:{}px;white-space:pre}}</style>".format(font_size),
        '<rect width="100%" height="100%" rx="6" fill="{}"/>'.format(hexcolor(page_bg)),
    ]
    if title:
        out.append('<text x="{:.1f}" y="{:.1f}" fill="#9a9a9a">{}</text>'.format(pad, pad + font_size, html.escape(title)))
    for yy, row in enumerate(screen):
        ty = top + yy * ch
        # sfondi: una rect per sequenza di celle con lo stesso colore (diverso da quello di pagina)
        xx = 0
        while xx < cols:
            bg = row[xx][1][1]
            start = xx
            while xx < cols and row[xx][1][1] == bg:
                xx += 1
            if bg != page_bg:
                out.append('<rect x="{:.1f}" y="{:.1f}" width="{:.1f}" height="{:.1f}" fill="{}"/>'.format(
                    pad + start * cw, ty, (xx - start) * cw + 0.3, ch + 0.3, hexcolor(bg)))
        # testo: una <text> per sequenza con stessi attributi, lunghezza imposta sulla griglia
        xx = 0
        while xx < cols:
            key = row[xx][1]
            start = xx
            chars = []
            while xx < cols and row[xx][1] == key:
                chars.append(row[xx][0])
                xx += 1
            text = "".join(chars)
            if not text.strip():
                continue
            fg, _, bold, italic, underline = key
            span = len(chars)
            style = (' font-weight="bold"' if bold else "") + (' font-style="italic"' if italic else "") + \
                (' text-decoration="underline"' if underline else "")
            out.append('<text x="{:.1f}" y="{:.1f}" fill="{}" textLength="{:.1f}" lengthAdjust="spacingAndGlyphs"{}>{}</text>'.format(
                pad + start * cw, ty + ch * 0.8, hexcolor(fg), span * cw, style, html.escape(text)))
    out.append("</svg>")
    return "\n".join(out) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("capture")
    ap.add_argument("cols", type=int)
    ap.add_argument("rows", type=int)
    ap.add_argument("out")
    ap.add_argument("--title", default="")
    ap.add_argument("--font-size", type=int, default=14)
    a = ap.parse_args(argv)
    data = open(a.capture, "rb").read().decode("utf-8", "replace")
    screen = emulate(data, a.cols, a.rows)
    with open(a.out, "w", encoding="utf-8") as f:
        f.write(to_svg(screen, a.cols, a.rows, a.font_size, a.title))
    return 0


if __name__ == "__main__":
    sys.exit(main())
