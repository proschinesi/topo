#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the topo contributors. Part of topo (fork of btop for Apple Silicon): see NOTICE.
"""Converte una cattura ANSI di un programma a schermo intero (es. topo in un pty) in un'immagine SVG.

Solo libreria standard: si esegue con `python3 -I tools/ansi2svg.py`.

Uso:
    python3 -I tools/ansi2svg.py CATTURA.raw COLONNE RIGHE OUT.svg [--title TITOLO] [--font-size 14]
    python3 -I tools/ansi2svg.py animate CATT1.raw CATT2.raw ... --size 120x40 -o OUT.svg [--frame-ms 1000]
    python3 -I tools/ansi2svg.py record --size 120x40 --frames 20 --warmup 40 -o OUT.svg [--still S.svg] -- CMD ...

`animate` monta piu' catture in un SVG animato; `record` esegue CMD in un pty, ne prende un aggiornamento ogni
--frame-ms dopo --warmup secondi e scrive l'SVG animato (e con --still l'ultimo fotogramma come SVG statico).
L'animazione e' solo CSS (@keyframes a gradini, in loop), senza script: si vede anche dentro <img>.

La cattura e' il flusso di byte scritto dal programma (sequenze CSI di posizionamento, cancellazione e
colore SGR, compresi i colori a 24 bit). Il terminale emulato e' minimale ma basta per btop/topo:
cursore (H f A B C D G d s u, ESC 7/8), cancellazioni (J K X), colori (SGR 0 1 2 3 4 7 22-27 30-49
90-107, 38/48;5 e 38/48;2) e caratteri larghi. Ogni sequenza di celle con gli stessi attributi diventa
un <text> con textLength fisso, cosi' riquadri e grafici braille restano allineati con qualunque font.
"""

import argparse
import codecs
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


def emulate(data, cols, rows, marks=None):
    """Emula il flusso e restituisce lo schermo finale; con `marks` (lista di offset nella stringa, anche vuota)
    restituisce invece una lista con una copia dello schermo per ogni offset."""
    blank = lambda a: (" ", a.key())
    pending = sorted(marks) if marks is not None else []
    mi = 0
    shots = []
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
        while mi < len(pending) and i >= pending[mi]:
            shots.append([row[:] for row in screen])
            mi += 1
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
    if marks is None:
        return screen
    shots.extend([row[:] for row in screen] for _ in pending[mi:])
    return shots


def hexcolor(rgb):
    return "#{:02x}{:02x}{:02x}".format(*rgb)


def num(v):
    return ("%.1f" % v).rstrip("0").rstrip(".")


class Canvas:
    """Geometria della griglia e tabella dei colori (una classe CSS per colore)."""

    def __init__(self, cols, rows, font_size, title):
        self.cols, self.rows, self.font_size, self.title = cols, rows, font_size, title
        self.cw = font_size * 0.6
        self.ch = font_size * 1.25
        self.pad = 8
        self.top = self.pad + (self.ch + 6 if title else 0)
        self.width = cols * self.cw + 2 * self.pad
        self.height = rows * self.ch + self.top + self.pad
        self.colors = {}

    def cls(self, rgb):
        c = hexcolor(rgb)
        if c not in self.colors:
            self.colors[c] = "c%d" % len(self.colors)
        return self.colors[c]

    def elements(self, screen, page_bg):
        """Elementi SVG di uno schermo: (rect di sfondo, testi). Ogni elemento e' una stringa completa."""
        cw, ch, pad = self.cw, self.ch, self.pad
        rects, texts = [], []
        for yy, row in enumerate(screen):
            ty = self.top + yy * ch
            # sfondi: una rect per sequenza di celle con lo stesso colore (diverso da quello di pagina)
            xx = 0
            while xx < self.cols:
                bg = row[xx][1][1]
                start = xx
                while xx < self.cols and row[xx][1][1] == bg:
                    xx += 1
                if bg != page_bg:
                    rects.append('<rect x="{}" y="{}" width="{}" height="{}" class="{}"/>'.format(
                        num(pad + start * cw), num(ty), num((xx - start) * cw + 0.3), num(ch + 0.3), self.cls(bg)))
            # testo: una <text> per sequenza con stessi attributi, lunghezza imposta sulla griglia
            xx = 0
            while xx < self.cols:
                key = row[xx][1]
                start = xx
                chars = []
                while xx < self.cols and row[xx][1] == key:
                    chars.append(row[xx][0])
                    xx += 1
                text = "".join(chars)
                if not text.strip():
                    continue
                fg, _, bold, italic, underline = key
                style = (' font-weight="bold"' if bold else "") + (' font-style="italic"' if italic else "") + \
                    (' text-decoration="underline"' if underline else "")
                texts.append(
                    '<text x="{}" y="{}" class="{}" textLength="{}" lengthAdjust="spacingAndGlyphs"{}>{}</text>'.format(
                        num(pad + start * cw), num(ty + ch * 0.8), self.cls(fg), num(len(chars) * cw), style,
                        html.escape(text)))
        return rects, texts

    def document(self, page_bg, body, extra_css=""):
        style = ("text{{font-family:Menlo,'SF Mono','DejaVu Sans Mono',Consolas,monospace;"
                 "font-size:{}px;white-space:pre}}").format(self.font_size)
        style += "".join(".{}{{fill:{}}}".format(k, c) for c, k in self.colors.items()) + extra_css
        out = [
            '<svg xmlns="http://www.w3.org/2000/svg" width="{:.0f}" height="{:.0f}" viewBox="0 0 {} {}">'.format(
                self.width, self.height, num(self.width), num(self.height)),
            "<style>" + style + "</style>",
            '<rect width="100%" height="100%" rx="6" fill="{}"/>'.format(hexcolor(page_bg)),
        ]
        if self.title:
            out.append('<text x="{}" y="{}" fill="#9a9a9a">{}</text>'.format(
                num(self.pad), num(self.pad + self.font_size), html.escape(self.title)))
        out.extend(body)
        out.append("</svg>")
        return "\n".join(out) + "\n"


def page_background(screen):
    return screen[0][0][1][1] if screen and screen[0] else DEFAULT_BG


def to_svg(screen, cols, rows, font_size, title):
    cv = Canvas(cols, rows, font_size, title)
    page_bg = page_background(screen)
    rects, texts = cv.elements(screen, page_bg)
    return cv.document(page_bg, rects + texts)


def to_animated_svg(screens, cols, rows, font_size, title, frame_ms):
    """SVG animato: ogni elemento distinto compare una sola volta, in un gruppo per insieme di fotogrammi in cui
    e' visibile; un'animazione CSS a gradini (step-end, in loop) accende e spegne ogni gruppo. Niente script:
    funziona anche dentro <img>, per esempio nel README su GitHub."""
    n = len(screens)
    cv = Canvas(cols, rows, font_size, title)
    page_bg = page_background(screens[0])
    layers = ({}, {})  # elemento -> maschera dei fotogrammi, sfondi e testi separati (i testi sopra)
    for f, screen in enumerate(screens):
        for layer, elems in zip(layers, cv.elements(screen, page_bg)):
            for e in elems:
                layer[e] = layer.get(e, 0) | (1 << f)
    full = (1 << n) - 1
    masks = {}
    body = []
    for layer in layers:
        groups = {}
        for e, m in layer.items():
            groups.setdefault(m, []).append(e)
        if full in groups:
            body.extend(groups.pop(full))
        for m, elems in groups.items():
            k = masks.setdefault(m, "k%d" % len(masks))
            body.append('<g class="a {}">'.format(k))
            body.extend(elems)
            body.append("</g>")
    css = [".a{{opacity:0;animation:{}ms step-end infinite}}".format(frame_ms * n)]
    for m, k in masks.items():
        steps, prev = [], None
        for f in range(n):
            on = (m >> f) & 1
            if on != prev:
                steps.append("{}%{{opacity:{}}}".format(("%.3f" % (100.0 * f / n)).rstrip("0").rstrip("."), on))
                prev = on
        css.append(".{0}{{animation-name:{0}}}@keyframes {0}{{{1}}}".format(k, "".join(steps)))
    return cv.document(page_bg, body, "".join(css))


SYNC_END = b"\x1b[?2026l"  # fine di un aggiornamento con la sincronizzazione del terminale (btop/topo)


def record(cmd, cols, rows, frames, interval_ms, warmup_s, extra_s=2.0):
    """Esegue `cmd` in un pty COLS x ROWS e ne registra l'uscita. Restituisce (testo, offset dei fotogrammi):
    il primo fotogramma e' il primo aggiornamento completo dopo `warmup_s`, poi uno ogni `interval_ms` circa."""
    import fcntl
    import os
    import pty
    import select
    import signal
    import struct
    import termios
    import time

    pid, fd = pty.fork()
    if pid == 0:
        try:
            fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
            os.environ["TERM"] = "xterm-256color"
            os.environ["COLORTERM"] = "truecolor"
            os.environ.setdefault("LANG", "en_US.UTF-8")
            os.execvp(cmd[0], cmd)
        finally:
            os._exit(127)
    data = bytearray()
    chunks = []  # (offset di fine, istante)
    t0 = time.monotonic()
    stop = warmup_s + frames * interval_ms / 1000.0 + extra_s
    while time.monotonic() - t0 < stop:
        r, _, _ = select.select([fd], [], [], 0.05)
        if not r:
            continue
        try:
            got = os.read(fd, 65536)
        except OSError:
            break
        if not got:
            break
        data += got
        chunks.append((len(data), time.monotonic() - t0))
    try:
        os.write(fd, b"q")
        time.sleep(0.5)
        if os.waitpid(pid, os.WNOHANG)[0] == 0:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
    except (OSError, ChildProcessError):
        pass
    os.close(fd)

    # fine di ogni aggiornamento: dopo SYNC_END se c'e', altrimenti la fine di ogni lettura
    ends = []
    pos = data.find(SYNC_END)
    if pos == -1:
        ends = chunks
    else:
        ci = 0
        while pos != -1:
            end = pos + len(SYNC_END)
            while chunks[ci][0] < end:
                ci += 1
            ends.append((end, chunks[ci][1]))
            pos = data.find(SYNC_END, end)
    picked, last_t = [], None
    for off, t in ends:
        if t < warmup_s or (last_t is not None and t < last_t + interval_ms * 0.0008):
            continue
        picked.append(off)
        last_t = t
        if len(picked) == frames:
            break
    if len(picked) < frames:
        raise SystemExit("solo {} fotogrammi su {}: aumentare la durata o controllare il comando".format(
            len(picked), frames))
    # decodifica in un solo passaggio; un offset che cade dentro un carattere UTF-8 (fine di una lettura)
    # diventa il confine di carattere precedente, perche' il decoder incrementale trattiene i byte parziali
    decoder = codecs.getincrementaldecoder("utf-8")("replace")
    parts, nchars, prev, marks = [], 0, 0, []
    for off in picked:
        part = decoder.decode(bytes(data[prev:off]))
        parts.append(part)
        nchars += len(part)
        marks.append(nchars)
        prev = off
    parts.append(decoder.decode(bytes(data[prev:]), final=True))
    return "".join(parts), marks


def parse_size(s):
    """Tipo argparse per COLONNExRIGHE (es. 120x40)."""
    cols, _, rows = s.lower().partition("x")
    try:
        size = int(cols), int(rows)
    except ValueError:
        raise argparse.ArgumentTypeError("dimensione non valida: {!r} (atteso COLONNExRIGHE, es. 120x40)".format(s))
    if min(size) < 1:
        raise argparse.ArgumentTypeError("dimensione non valida: {!r}".format(s))
    return size


def positive_int(s):
    """Tipo argparse per un intero >= 1."""
    try:
        v = int(s)
    except ValueError:
        raise argparse.ArgumentTypeError("non e' un intero: {!r}".format(s))
    if v < 1:
        raise argparse.ArgumentTypeError("deve essere >= 1: {}".format(v))
    return v


def main_animate(argv):
    ap = argparse.ArgumentParser(prog="ansi2svg.py animate",
                                 description="SVG animato da piu' catture ANSI (una per fotogramma).")
    ap.add_argument("captures", nargs="+")
    ap.add_argument("--size", required=True, type=parse_size, help="COLONNExRIGHE, es. 120x40")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--frame-ms", type=positive_int, default=1000)
    ap.add_argument("--title", default="")
    ap.add_argument("--font-size", type=int, default=14)
    a = ap.parse_args(argv)
    cols, rows = a.size
    screens = [emulate(open(p, "rb").read().decode("utf-8", "replace"), cols, rows) for p in a.captures]
    write(a.out, to_animated_svg(screens, cols, rows, a.font_size, a.title, a.frame_ms))
    return 0


def main_record(argv):
    ap = argparse.ArgumentParser(
        prog="ansi2svg.py record",
        description="Esegue un programma a schermo intero in un pty, ne cattura N aggiornamenti "
                    "e scrive un SVG animato.",
        epilog="Esempio: python3 -I tools/ansi2svg.py record --size 120x40 --frames 20 --warmup 40 "
               "-o docs/topo-demo.svg -- build/topo -c demo.conf")
    ap.add_argument("--size", required=True, type=parse_size, help="COLONNExRIGHE, es. 120x40")
    ap.add_argument("-o", "--out", required=True, help="SVG animato")
    ap.add_argument("--frames", type=positive_int, default=20)
    ap.add_argument("--frame-ms", type=positive_int, default=1000,
                    help="durata di un fotogramma e distanza tra le catture")
    ap.add_argument("--warmup", type=float, default=10.0, help="secondi prima del primo fotogramma (riempie i grafici)")
    ap.add_argument("--still", help="scrive anche l'ultimo fotogramma come SVG statico")
    ap.add_argument("--raw", help="salva il flusso ANSI registrato")
    ap.add_argument("--title", default="")
    ap.add_argument("--font-size", type=int, default=14)
    ap.add_argument("cmd", nargs=argparse.REMAINDER, help="-- programma e argomenti")
    a = ap.parse_args(argv)
    cmd = a.cmd[1:] if a.cmd[:1] == ["--"] else a.cmd
    if not cmd:
        ap.error("manca il comando dopo --")
    cols, rows = a.size
    text, marks = record(cmd, cols, rows, a.frames, a.frame_ms, a.warmup)
    if a.raw:
        write(a.raw, text)
    screens = emulate(text, cols, rows, marks)
    write(a.out, to_animated_svg(screens, cols, rows, a.font_size, a.title, a.frame_ms))
    if a.still:
        write(a.still, to_svg(screens[-1], cols, rows, a.font_size, a.title))
    return 0


def write(path, content):
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv[:1] == ["animate"]:
        return main_animate(argv[1:])
    if argv[:1] == ["record"]:
        return main_record(argv[1:])
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 epilog="Altri modi: 'ansi2svg.py record -h' e 'ansi2svg.py animate -h'.")
    ap.add_argument("capture")
    ap.add_argument("cols", type=int)
    ap.add_argument("rows", type=int)
    ap.add_argument("out")
    ap.add_argument("--title", default="")
    ap.add_argument("--font-size", type=int, default=14)
    a = ap.parse_args(argv)
    data = open(a.capture, "rb").read().decode("utf-8", "replace")
    screen = emulate(data, a.cols, a.rows)
    write(a.out, to_svg(screen, a.cols, a.rows, a.font_size, a.title))
    return 0


if __name__ == "__main__":
    sys.exit(main())
