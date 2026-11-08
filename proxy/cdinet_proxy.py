#!/usr/bin/env python3
"""cdiNet proxy: the web, made small enough for a Philips CD-i on a serial line.

The CD-i (or cdiPC with --serial HOST:PORT) calls this server. The proxy fetches pages
(TLS included), keeps only their text and links, wraps them to the CD-i's columns and
sends them a screen at a time, in the frames of docs/protocol.md.

    python cdinet_proxy.py [--port 6400] [--host 127.0.0.1] [--search wikipedia|wiby|frogfind]

Standard library only."""
import argparse
import html
import io
import html.parser
import re
import socket
import threading
import time
import unicodedata
import urllib.error
import urllib.parse
import urllib.request

try:                                  # pictures need Pillow; without it pages have none
    from PIL import Image
except ImportError:
    Image = None

VERSION = 1
NAME = b"cdiNet proxy 0.1"
USER_AGENT = "Mozilla/5.0 (compatible; cdiNet/0.1; +a CD-i on a serial line)"
MAX_PAYLOAD = 100
CHUNK_DATA = MAX_PAYLOAD - 6          # seq, screen id, offset (2), total (2)
MAX_PAGE_BYTES = 400_000              # bigger pages are cut
IMAGE_PREFIX = "cdinet:image:"        # a link to a picture, shown as one
PAPER = (236, 232, 220)               # the CD-i's page colour, behind transparent pixels
IMAGE_SCREEN = 0xFF                   # in place of the row count: a picture follows

LINK_START, LINK_END, STYLE = 0x01, 0x02, 0x03
BOLD, HEADING, QUOTE = 1, 2, 4

SEARCH = {
    "wikipedia": "https://en.wikipedia.org/w/index.php?search={}",
    "wiby": "https://wiby.me/?q={}",
    "frogfind": "http://frogfind.com/?q={}",
}

HOME_LINKS = [
    ("68k.news - headlines for old computers", "http://68k.news/"),
    ("Wikipedia: CD-i", "https://en.wikipedia.org/wiki/CD-i"),
    ("Wikipedia: CD-Online / Web-i", "https://en.wikipedia.org/wiki/CD-i#Online_services"),
    ("NPR text-only news", "https://text.npr.org/"),
    ("CNN lite", "https://lite.cnn.com/"),
    ("Wiby - search engine for the classic web", "https://wiby.me/"),
    ("FrogFind - the web for vintage computers", "http://frogfind.com/"),
]


# ---------------------------------------------------------------- frames

def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
    return crc


def frame(ftype, payload):
    assert len(payload) <= MAX_PAYLOAD
    body = bytes([ord(ftype), len(payload)]) + payload
    c = crc16(body)
    return b"\x16\x16" + body + bytes([c >> 8, c & 0xFF])


class Deframer:
    """Bytes in, (type, payload) out; noise and bad frames are skipped."""

    def __init__(self):
        self.buf = bytearray()
        self.dropped = 0

    def feed(self, data):
        self.buf += data
        out = []
        while True:
            i = self.buf.find(b"\x16\x16")
            if i < 0:
                self.dropped += max(0, len(self.buf) - 1)
                del self.buf[:max(0, len(self.buf) - 1)]
                return out
            if i:
                self.dropped += i
                del self.buf[:i]
            if len(self.buf) < 4:
                return out
            n = self.buf[3]
            if n > MAX_PAYLOAD:
                del self.buf[:1]
                continue
            if len(self.buf) < 6 + n:
                return out
            body = bytes(self.buf[2:4 + n])
            c = (self.buf[4 + n] << 8) | self.buf[5 + n]
            if crc16(body) == c:
                out.append((chr(body[0]), body[2:]))
                del self.buf[:6 + n]
            else:
                self.dropped += 1
                del self.buf[:1]


# ---------------------------------------------------------------- text

PUNCT = {
    "‘": "'", "’": "'", "‚": "'", "‛": "'", "′": "'",
    "“": '"', "”": '"', "„": '"', "″": '"',
    "–": "-", "—": "-", "―": "-", "−": "-", "‐": "-", "‑": "-",
    "…": "...", "•": "*", "·": "·", "→": "->", "←": "<-",
    "€": "EUR", "™": "(TM)", " ": " ", " ": " ", " ": " ",
    " ": " ", " ": " ", " ": " ", "​": "", "‌": "", "‍": "",
    "﻿": "", "­": "",
}


def to_latin1(s):
    """A str as characters the CD-i's font has (ISO 8859-1, printable)."""
    out = []
    for ch in s:
        o = ord(ch)
        if ch in PUNCT:
            out.append(PUNCT[ch])
        elif 0x20 <= o < 0x7F or 0xA0 <= o <= 0xFF:
            out.append(ch)
        elif ch in "\t\n\r":
            out.append(" ")
        elif o < 0x20 or 0x7F <= o < 0xA0:
            continue
        else:
            d = "".join(c for c in unicodedata.normalize("NFKD", ch) if ord(c) < 0x100 and not unicodedata.combining(c))
            out.append(d if d else "?")
    return "".join(out)


class Page:
    """Text cells laid out in rows of at most cols characters. A cell is
    (char, style, link id); rows are lists of cells."""

    def __init__(self, cols):
        self.cols = cols
        self.rows = []
        self.links = []          # link id - 1 -> URL
        self.title = ""
        self.url = ""
        self.para = []           # cells of the paragraph being collected
        self.first_prefix = []   # cells before its first row (a bullet)
        self.indent = 0
        self.pending_blank = False
        self.image = None        # a picture page: (width, height, colours, palette, LZSS data)

    def add_link(self, url):
        self.links.append(url)
        return len(self.links)

    def text(self, s, style, link):
        for ch in to_latin1(s):
            if ch == " ":
                if not self.para or self.para[-1][0] == " ":
                    continue
            self.para.append((ch, style, link))

    def blank(self):
        self.pending_blank = True

    def flush(self):
        cells = self.para
        while cells and cells[-1][0] == " ":
            cells.pop()
        self.para = []
        if not cells:
            return
        if self.pending_blank and self.rows and self.rows[-1]:
            self.rows.append([])
        self.pending_blank = False
        width = max(8, self.cols - self.indent)
        prefix = self.first_prefix
        self.first_prefix = []
        first = True
        line = []
        words = []
        word = []
        for c in cells:
            if c[0] == " ":
                if word:
                    words.append(word)
                    word = []
            else:
                word.append(c)
        if word:
            words.append(word)
        for w in words:
            avail = width - (len(prefix) if first else 0)
            while len(w) > avail:                       # a word longer than a row
                if line:
                    self._emit(line, prefix if first else None)
                    first, line = False, []
                    avail = width
                    continue
                self._emit(w[:avail], prefix if first else None)
                first, w = False, w[avail:]
                avail = width
            if line and len(line) + 1 + len(w) > avail:
                self._emit(line, prefix if first else None)
                first, line = False, []
                avail = width
            if line:
                # the space takes the style and link both words share
                prev = line[-1]
                line.append((" ", prev[1] if prev[1] == w[0][1] else 0, prev[2] if prev[2] == w[0][2] else 0))
            line.extend(w)
        if line:
            self._emit(line, prefix if first else None)

    def _emit(self, cells, prefix):
        pad = [(" ", 0, 0)] * self.indent
        if prefix:
            pad = pad[:max(0, self.indent - len(prefix))] + prefix
        self.rows.append(pad + cells)

    def pre_line(self, s, style):
        for i in range(0, max(1, len(s)), self.cols):
            self.rows.append([(ch, style, 0) for ch in to_latin1(s[i:i + self.cols])])

    def finish(self):
        self.flush()
        while self.rows and not self.rows[-1]:
            self.rows.pop()
        if not self.rows:
            self.rows.append([(ch, 0, 0) for ch in "(this page has no text)"])


def encode_row(cells):
    out = bytearray()
    style, link = 0, 0
    for ch, st, ln in cells:
        if ln != link:
            if link:
                out.append(LINK_END)
            if ln:
                out += bytes([LINK_START, ln >> 8, ln & 0xFF])
            link = ln
        if st != style:
            out += bytes([STYLE, st])
            style = st
        out.append(ord(ch))
    if link:
        out.append(LINK_END)
    return bytes(out)


# ---------------------------------------------------------------- HTML

SKIP_TAGS = {"script", "style", "noscript", "svg", "template", "iframe", "canvas", "video",
             "audio", "object", "embed", "select", "button", "textarea", "nav", "aside",
             "footer", "head", "math"}
SKIP_CLASSES = {"mw-editsection", "reference", "navbox", "noprint", "mw-jump-link",
                "metadata", "sidebar", "navigation", "nav", "menu", "cookie", "breadcrumb",
                "mw-cite-backlink", "vector-header", "skip-link", "visually-hidden",
                "sr-only", "hatnote", "printfooter", "catlinks", "shortdescription"}
SKIP_ROLES = {"navigation", "banner", "contentinfo", "search", "complementary"}
BLOCK_TAGS = {"p", "div", "section", "article", "main", "header", "ul", "ol", "dl", "dt",
              "dd", "table", "tr", "blockquote", "figure", "figcaption", "address",
              "center", "form", "fieldset", "details", "summary", "caption", "body"}
HEADINGS = {"h1", "h2", "h3", "h4", "h5", "h6"}
VOID = {"br", "img", "hr", "input", "meta", "link", "area", "base", "col", "source",
        "track", "wbr", "param", "embed"}


class Simplifier(html.parser.HTMLParser):
    def __init__(self, page, base, focus):
        super().__init__(convert_charrefs=True)
        self.p = page
        self.base = base
        self.focus = focus           # (attribute, value) of the main content, or None
        self.focus_tag, self.focus_depth, self.in_focus = None, 0, focus is None
        self.skip_tag, self.skip_depth = None, 0
        self.style = []              # stack of style bits
        self.link = 0
        self.link_text = False
        self.lists = []              # "ul" or ["ol", n]
        self.pre = 0
        self.pre_buf = ""
        self.title_buf, self.in_title = "", False
        self.cell_count = 0

    def cur_style(self):
        s = 0
        for x in self.style:
            s |= x
        return s

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == "title":
            self.in_title = True
            return
        if self.skip_tag:
            if tag == self.skip_tag:
                self.skip_depth += 1
            return
        if not self.in_focus:
            if self.focus_tag is None and self._matches_focus(tag, a):
                self.focus_tag, self.focus_depth, self.in_focus = tag, 1, True
            return
        if self.focus_tag and tag == self.focus_tag:
            self.focus_depth += 1
        classes = set((a.get("class") or "").split())
        hidden = "display:none" in (a.get("style") or "").replace(" ", "") or "hidden" in a
        if tag in SKIP_TAGS or classes & SKIP_CLASSES or a.get("role") in SKIP_ROLES or hidden \
                or a.get("aria-hidden") == "true":
            if tag not in VOID:
                self.skip_tag, self.skip_depth = tag, 1
            return
        p = self.p
        if tag in HEADINGS:
            p.flush()
            p.blank()
            self.style.append(BOLD | HEADING)
        elif tag == "pre":
            p.flush()
            p.blank()
            self.pre += 1
            self.pre_buf = ""
        elif tag in ("ul", "ol"):
            p.flush()
            if not self.lists:
                p.blank()
            self.lists.append("ul" if tag == "ul" else ["ol", 0])
            p.indent = 2 * len(self.lists)
        elif tag == "li":
            p.flush()
            kind = self.lists[-1] if self.lists else "ul"
            if kind == "ul":
                bullet = "* "
            else:
                kind[1] += 1
                bullet = "%d. " % kind[1]
            p.first_prefix = [(c, 0, 0) for c in bullet]
        elif tag == "br":
            if self.pre:
                self.pre_buf += "\n"
            else:
                p.flush()
        elif tag == "hr":
            p.flush()
            p.rows.append([("-", 0, 0)] * p.cols)
        elif tag in ("td", "th"):
            if self.cell_count:
                p.text(" | ", 0, 0)
            self.cell_count += 1
            if tag == "th":
                self.style.append(BOLD)
        elif tag == "tr":
            p.flush()
            self.cell_count = 0
        elif tag == "blockquote":
            p.flush()
            p.blank()
            self.style.append(QUOTE)
            p.indent += 2
        elif tag in ("b", "strong"):
            self.style.append(BOLD)
        elif tag == "a":
            href = a.get("href")
            if href and not href.startswith(("javascript:", "mailto:", "#")):
                url = urllib.parse.urljoin(self.base, href)
                if url.startswith(("http://", "https://")):
                    self.link = p.add_link(url)
                    self.link_text = False
        elif tag == "img":
            self.image(a)
        elif tag == "input" and a.get("type") in (None, "text", "search"):
            p.text(" [____] ", QUOTE, 0)
        elif tag in BLOCK_TAGS or tag == "p":
            p.flush()
            if tag in ("p", "table", "figure", "dl", "details"):
                p.blank()

    def handle_endtag(self, tag):
        if tag == "title":
            self.in_title = False
            return
        if self.skip_tag:
            if tag == self.skip_tag:
                self.skip_depth -= 1
                if self.skip_depth == 0:
                    self.skip_tag = None
            return
        if not self.in_focus:
            return
        if self.focus_tag and tag == self.focus_tag:
            self.focus_depth -= 1
            if self.focus_depth == 0:
                self.p.flush()
                self.in_focus = False
                self.focus_tag = "-done-"      # take only the first match
                return
        p = self.p
        if tag in HEADINGS:
            p.flush()
            p.blank()
            self._pop_style(BOLD | HEADING)
        elif tag == "pre" and self.pre:
            self.pre -= 1
            for line in self.pre_buf.rstrip("\n").split("\n"):
                p.pre_line(line.expandtabs(4), QUOTE)
            self.pre_buf = ""
            p.blank()
        elif tag in ("ul", "ol"):
            p.flush()
            if self.lists:
                self.lists.pop()
            p.indent = 2 * len(self.lists)
            if not self.lists:
                p.blank()
        elif tag == "li":
            p.flush()
        elif tag == "th":
            self._pop_style(BOLD)
        elif tag == "blockquote":
            p.flush()
            self._pop_style(QUOTE)
            p.indent = max(0, p.indent - 2)
            p.blank()
        elif tag in ("b", "strong"):
            self._pop_style(BOLD)
        elif tag == "a":
            self.link = 0
        elif tag in BLOCK_TAGS or tag == "p":
            p.flush()
            if tag in ("p", "table", "figure", "dl", "details"):
                p.blank()

    def image(self, a):
        """A picture becomes a link of its own, [image: alt], even inside another link."""
        alt = " ".join((a.get("alt") or "").split())
        src = a.get("src") or ""
        try:
            small = 0 < int(a.get("width") or 99) < 40 or 0 < int(a.get("height") or 99) < 40
        except ValueError:
            small = False
        url = urllib.parse.urljoin(self.base, src) if src and not src.startswith("data:") else ""
        if Image is None or not url.startswith(("http://", "https://")) or small \
                or urllib.parse.urlparse(url).path.lower().endswith(".svg"):
            if self.link and not self.link_text and alt:
                self.p.text(" [%s] " % alt, self.cur_style(), self.link)
                self.link_text = True
            return
        label = " [image: %s] " % alt[:40] if alt else " [image] "
        self.p.text(label, self.cur_style() | QUOTE, self.p.add_link(IMAGE_PREFIX + url))
        if self.link:
            self.link_text = True

    def _pop_style(self, s):
        for i in range(len(self.style) - 1, -1, -1):
            if self.style[i] == s:
                del self.style[i]
                return

    def handle_data(self, data):
        if self.in_title:
            self.title_buf += data
            return
        if self.skip_tag or not self.in_focus:
            return
        if self.pre:
            self.pre_buf += data
            return
        if data.strip():
            self.link_text = True
        self.p.text(data, self.cur_style(), self.link)

    def _matches_focus(self, tag, a):
        kind, value = self.focus
        if kind == "tag":
            return tag == value
        return a.get(kind) == value


def find_focus(src):
    """Where the page's own content is, when it says so."""
    low = src[:MAX_PAGE_BYTES].lower()
    if 'id="mw-content-text"' in low:
        return ("id", "mw-content-text")
    if "<main" in low:
        return ("tag", "main")
    if 'role="main"' in low:
        return ("role", "main")
    if low.count("<article") == 1:
        return ("tag", "article")
    return None


def page_from_html(src, url, cols):
    p = Page(cols)
    p.url = url
    focus = find_focus(src)
    s = Simplifier(p, url, focus)
    s.feed(src[:MAX_PAGE_BYTES])
    s.close()
    p.title = to_latin1(" ".join(s.title_buf.split()))[:60]
    p.finish()
    if focus and len(p.rows) < 3:           # the guess was wrong: the whole page
        p = Page(cols)
        p.url = url
        s = Simplifier(p, url, None)
        s.feed(src[:MAX_PAGE_BYTES])
        s.close()
        p.title = to_latin1(" ".join(s.title_buf.split()))[:60]
        p.finish()
    return p


def page_from_text(src, url, cols):
    p = Page(cols)
    p.url = url
    p.title = url.rsplit("/", 1)[-1][:60]
    for line in src.splitlines():
        p.pre_line(line.expandtabs(4), 0)
    p.finish()
    return p


def message_page(title, lines, cols, links=()):
    p = Page(cols)
    p.title = title
    for line in lines:
        if line:
            p.text(line, 0, 0)
            p.flush()
        else:
            p.flush()
            p.blank()
    for text, url in links:
        p.first_prefix = [(c, 0, 0) for c in "> "]
        p.text(text, 0, p.add_link(url))
        p.flush()
    p.finish()
    return p


def home_page(cols, search):
    p = Page(cols)
    p.title = "cdiNet"
    p.url = "cdinet:home"
    p.text("cdiNet", BOLD | HEADING, 0)
    p.flush()
    p.text("The web on a Philips CD-i, through a serial line and this proxy. "
           "Point at a link and press button 1; button 2 goes back.", 0, 0)
    p.flush()
    p.blank()
    for text, url in HOME_LINKS:
        p.first_prefix = [(c, 0, 0) for c in "> "]
        p.text(text, 0, p.add_link(url))
        p.flush()
    p.blank()
    p.text("Searching uses %s." % search, QUOTE, 0)
    p.flush()
    p.finish()
    return p


# ---------------------------------------------------------------- fetching

def lzss(data, window=4096, maxlen=18):
    """LZSS as the CD-i decodes it: a flag byte before each 8 items (bit 0 first); flag 0 =
    one literal byte, 1 = a copy of 3-18 bytes from 1-4096 back, in two bytes:
    distance-1 (12 bits), then length-3 (4 bits)."""
    out = bytearray()
    i, n = 0, len(data)
    chains = {}
    while i < n:
        flags_at = len(out)
        out.append(0)
        flags = 0
        for bit in range(8):
            if i >= n:
                break
            best, dist = 0, 0
            for j in reversed(chains.get(data[i:i + 3], [])[-48:]):
                if i - j > window:
                    break
                k = 0
                while k < maxlen and i + k < n and data[j + k] == data[i + k]:
                    k += 1
                if k > best:
                    best, dist = k, i - j
                    if k == maxlen:
                        break
            step = best if best >= 3 else 1
            if best >= 3:
                flags |= 1 << bit
                d = dist - 1
                out += bytes([d >> 4, ((d & 15) << 4) | (best - 3)])
            else:
                out.append(data[i])
            for k in range(step):
                chains.setdefault(data[i + k:i + k + 3], []).append(i + k)
            i += step
        out[flags_at] = flags
    return bytes(out)


def image_page(data, url, cols, rows, args):
    """A picture fitted to the page area, in at most args.colours colours (CLUT 16 on)."""
    im = Image.open(io.BytesIO(data))
    im.seek(0)
    im = im.convert("RGBA")
    flat = Image.new("RGB", im.size, PAPER)
    flat.paste(im, mask=im.getchannel("A"))
    maxw = min(args.image_width, cols * 6)
    maxh = min(args.image_height, rows * 10)
    scale = min(maxw / flat.width, maxh / flat.height, 1.0)
    small = flat.resize((max(1, round(flat.width * scale)), max(1, round(flat.height * scale))), Image.LANCZOS)
    n = max(2, min(48, args.colours))
    q = small.quantize(palette=small.quantize(n), dither=Image.Dither.FLOYDSTEINBERG)
    pal = bytes(q.getpalette()[:3 * n]).ljust(3 * n, bytes(1))
    p = Page(cols)
    p.url = url
    p.title = to_latin1(urllib.parse.unquote(urllib.parse.urlparse(url).path.rsplit("/", 1)[-1]))[:60]
    p.image = (q.width, q.height, n, pal, lzss(q.tobytes()))
    return p


def fetch(url, cols, rows, args):
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, "Accept-Language": "en,it;q=0.8"})
    with urllib.request.urlopen(req, timeout=25) as r:
        final = r.geturl()
        ctype = r.headers.get_content_type()
        data = r.read(MAX_PAGE_BYTES * 2)
        charset = r.headers.get_content_charset()
    if ctype.startswith("image/") and Image is not None:
        return image_page(data, final, cols, rows, args)
    if ctype not in ("text/html", "application/xhtml+xml", "text/plain"):
        return message_page("Not a page", ["%s is %s, which a CD-i cannot show." % (final, ctype)], cols)
    if not charset:
        m = re.search(rb'<meta[^>]+charset=["\']?([A-Za-z0-9_-]+)', data[:4096], re.I)
        charset = m.group(1).decode("ascii") if m else "utf-8"
    try:
        src = data.decode(charset, "replace")
    except LookupError:
        src = data.decode("utf-8", "replace")
    if ctype == "text/plain":
        return page_from_text(src, final, cols)
    return page_from_html(src, final, cols)


def resolve(text, search):
    """What a G request asks for: a URL, or a search."""
    t = text.strip()
    if re.match(r"^https?://", t, re.I):
        return t
    if " " not in t and re.match(r"^[\w-]+(\.[\w-]+)+(:\d+)?(/.*)?$", t):
        return "http://" + t
    return SEARCH[search].format(urllib.parse.quote_plus(t))


# ---------------------------------------------------------------- a session

class Session:
    def __init__(self, conn, addr, args):
        self.conn, self.addr, self.args = conn, addr, args
        self.cols, self.rows = 64, 24
        self.history = []            # pages before the current one
        self.page = None
        self.screen = 0              # which screen of the page
        self.screen_id = 0
        self.blob = b""
        self.loading = None          # thread
        self.error = None
        self.last_seq, self.last_reply = None, None
        self.lock = threading.Lock()

    def log(self, msg):
        print("[%s] %s" % (time.strftime("%H:%M:%S"), msg), flush=True)

    # the screen as the CD-i gets it
    def build_screen(self):
        p = self.page
        pages = max(1, (len(p.rows) + self.rows - 1) // self.rows)
        self.screen = max(0, min(self.screen, pages - 1))
        rows = p.rows[self.screen * self.rows:(self.screen + 1) * self.rows]
        while len(rows) > 1 and not rows[0]:
            rows = rows[1:]                     # a screen does not start with a gap
        title = p.title.encode("latin-1", "replace")[:60]
        url = p.url.encode("latin-1", "replace")[:120]
        out = bytearray()
        out += bytes([self.screen >> 8, self.screen & 0xFF, pages >> 8, pages & 0xFF])
        out += bytes([len(title)]) + title + bytes([len(url)]) + url
        if p.image:
            w, h, n, pal, data = p.image
            out.append(IMAGE_SCREEN)
            out += bytes([w >> 8, w & 0xFF, h >> 8, h & 0xFF, n]) + pal + data
        else:
            out.append(len(rows))
            for r in rows:
                e = encode_row(r)
                out += bytes([len(e)]) + e
        if len(out) > 0xFFFF:
            out = out[:0xFFFF]                  # offsets are 16 bits: the picture ends early
        self.screen_id = self.screen_id % 255 + 1
        self.blob = bytes(out)
        what = "%dx%d picture, %d colours" % p.image[:3] if p.image else "%d rows" % len(rows)
        self.log("screen %d/%d of %s: %s, %d bytes" % (self.screen + 1, pages, p.url or p.title, what, len(self.blob)))

    def chunk(self, seq, offset):
        data = self.blob[offset:offset + CHUNK_DATA]
        n = len(self.blob)
        return frame("C", bytes([seq, self.screen_id, offset >> 8, offset & 0xFF, n >> 8, n & 0xFF]) + data)

    def show(self, page, push=True):
        with self.lock:
            if push and self.page is not None:
                self.history.append(self.page)
                del self.history[:-20]
            self.page = page
            self.screen = 0
            self.build_screen()

    def load(self, url):
        def run():
            t = time.time()
            try:
                if url == "cdinet:home":
                    page = home_page(self.cols, self.args.search)
                else:
                    page = fetch(url[len(IMAGE_PREFIX):] if url.startswith(IMAGE_PREFIX) else url,
                                 self.cols, self.rows, self.args)
                self.log("loaded %s in %.1f s: %d rows, %d links" % (url, time.time() - t, len(page.rows), len(page.links)))
                self.show(page)
            except Exception as e:  # noqa: BLE001 - every failure becomes a message on the CD-i
                self.log("failed %s: %s" % (url, e))
                with self.lock:
                    self.error = to_latin1("Cannot load: %s" % e)[:90]
            finally:
                with self.lock:
                    self.loading = None
        with self.lock:
            self.error = None
            self.loading = threading.Thread(target=run, daemon=True)
            self.loading.start()

    def reply_after_nav(self, seq, offset=0):
        """W while loading, E once after a failure, else the screen from offset."""
        with self.lock:
            if self.loading:
                return frame("W", bytes([seq]))
            if self.error:
                e, self.error = self.error, None
                return frame("E", bytes([seq]) + e.encode("latin-1")[:MAX_PAYLOAD - 1])
            if not self.blob:
                return frame("E", bytes([seq]) + b"No page yet")
            return self.chunk(seq, offset)

    def handle(self, ftype, payload):
        if not payload:
            return None
        seq, args = payload[0], payload[1:]
        if seq == self.last_seq and self.last_reply is not None and ftype not in "HN":
            return self.last_reply              # a resent request: do not act twice
        if ftype == "H":
            self.cols = max(20, min(120, args[1])) if len(args) > 1 else 64
            self.rows = max(4, min(60, args[2])) if len(args) > 2 else 24
            self.log("hello from the CD-i: %d x %d" % (self.cols, self.rows))
            self.history, self.page = [], None
            r = frame("h", bytes([seq, VERSION]) + NAME)
        elif ftype == "G":
            target = args.decode("latin-1").strip()
            url = resolve(target, self.args.search) if target else "cdinet:home"
            self.log("go %s" % url)
            self.load(url)
            r = self.reply_after_nav(seq)
        elif ftype == "L":
            n = (args[0] << 8) | args[1] if len(args) >= 2 else 0
            if self.page and 1 <= n <= len(self.page.links):
                url = self.page.links[n - 1]
                self.log("link %d: %s" % (n, url))
                self.load(url)
                r = self.reply_after_nav(seq)
            else:
                r = frame("E", bytes([seq]) + b"No such link")
        elif ftype == "B":
            if self.history:
                self.show(self.history.pop(), push=False)
                r = self.chunk(seq, 0)
            else:
                r = frame("E", bytes([seq]) + b"Nothing to go back to")
        elif ftype == "P":
            n = (args[0] << 8) | args[1] if len(args) >= 2 else 0
            if self.page:
                with self.lock:
                    self.screen = n
                    self.build_screen()
                r = self.chunk(seq, 0)
            else:
                r = frame("E", bytes([seq]) + b"No page yet")
        elif ftype == "N":
            off = (args[0] << 8) | args[1] if len(args) >= 2 else 0
            r = self.reply_after_nav(seq, off)
        else:
            return None
        self.last_seq, self.last_reply = seq, r
        return r

    def run(self):
        self.log("the CD-i called from %s:%d" % self.addr)
        d = Deframer()
        self.conn.settimeout(None)
        try:
            while True:
                data = self.conn.recv(4096)
                if not data:
                    break
                for ftype, payload in d.feed(data):
                    r = self.handle(ftype, payload)
                    if r:
                        self.conn.sendall(r)
        except OSError as e:
            self.log("line error: %s" % e)
        self.log("the CD-i hung up (%d noise bytes skipped)" % d.dropped)
        self.conn.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", default="127.0.0.1", help="address to listen on (0.0.0.0 for a real CD-i through a network modem)")
    ap.add_argument("--port", type=int, default=6400)
    ap.add_argument("--search", choices=sorted(SEARCH), default="wikipedia")
    ap.add_argument("--once", action="store_true", help="stop when the first CD-i hangs up (tests)")
    ap.add_argument("--colours", type=int, default=32, help="colours in a picture (2-48)")
    ap.add_argument("--image-width", type=int, default=256)
    ap.add_argument("--image-height", type=int, default=180)
    args = ap.parse_args()
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(4)
    print("cdiNet proxy waiting for a CD-i on %s:%d (search: %s; pictures: %s)" % (
        args.host, args.port, args.search, "%d colours" % args.colours if Image else "off, no Pillow"), flush=True)
    while True:
        conn, addr = srv.accept()
        if args.once:
            Session(conn, addr, args).run()
            return
        threading.Thread(target=Session(conn, addr, args).run, daemon=True).start()


if __name__ == "__main__":
    main()
