"""TR2013 HUD Fix+ — graphics v2 (game-HUD look: clean white strips, map/compass backdrop).
Run: python3 generate.py            -> nexus, github, icon, compare template
     python3 generate.py compare before.jpg after.jpg "Caption" out_name
"""
import base64, math, random, os, sys
import numpy as np
from fontTools.ttLib import TTFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "graphics")
ACC = "#9e1c3a"          # crimson accent sampled from the in-game "[KEY]" prompts
FONTS = {"bold": ("barlow-semi-condensed-latin-700-normal.woff2", "TRH Bold", 700),
         "semi": ("barlow-semi-condensed-latin-600-normal.woff2", "TRH Semi", 600),
         "med":  ("barlow-semi-condensed-latin-500-normal.woff2", "TRH Med", 500)}
_fc = {}


def font(k):
    if k not in _fc:
        p = os.path.join(HERE, "fonts", FONTS[k][0])
        _fc[k] = (TTFont(p), base64.b64encode(open(p, "rb").read()).decode())
    return _fc[k]


def tw(k, s, size, sp=0.0):
    f = font(k)[0]; cm = f.getBestCmap(); hm = f["hmtx"]; u = f["head"].unitsPerEm
    s = s.replace("&amp;", "&")
    return sum(hm[cm.get(ord(c), cm[32])][0] for c in s) * size / u + sp * max(len(s) - 1, 0)


def css():
    out = []
    for k, (fn, fam, w) in FONTS.items():
        out.append(f"@font-face{{font-family:'{fam}';font-weight:{w};src:url(data:font/woff2;base64,{font(k)[1]}) format('woff2');}}")
        out.append(f".{k}{{font-family:'{fam}','Bahnschrift','DIN Alternate','Arial Narrow',sans-serif;font-weight:{w}}}")
    return "<style>" + "".join(out) + "</style>"


def T(k, x, y, s, size, fill="#fff", anchor="start", sp=0, op=1):
    return (f'<text class="{k}" x="{x:.1f}" y="{y:.1f}" font-size="{size}" fill="{fill}" fill-opacity="{op}" '
            f'letter-spacing="{sp}" text-anchor="{anchor}">{s}</text>')


CAP = 0.70  # cap height ratio of Barlow


# ------------------------------------------------------------------ backdrop
def defs(seed):
    return f"""
<radialGradient id="bg" cx="58%" cy="45%" r="80%">
  <stop offset="0" stop-color="#3b3e37"/><stop offset=".45" stop-color="#262823"/><stop offset="1" stop-color="#0c0d0b"/>
</radialGradient>
<filter id="grain" x="0" y="0" width="100%" height="100%">
  <feTurbulence type="fractalNoise" baseFrequency=".9" numOctaves="3" seed="{seed}"/>
  <feColorMatrix type="matrix" values="0 0 0 0 1 0 0 0 0 1 0 0 0 0 1 0 0 0 -1.6 1.05"/>
</filter>
<filter id="stain" x="0" y="0" width="100%" height="100%">
  <feTurbulence type="fractalNoise" baseFrequency=".005 .009" numOctaves="4" seed="{seed+5}"/>
  <feColorMatrix type="matrix" values="0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 -2.6 1.3"/>
</filter>
<filter id="rag" x="-10%" y="-50%" width="120%" height="200%">
  <feTurbulence type="fractalNoise" baseFrequency=".012 .03" numOctaves="4" seed="{seed+9}" result="t"/>
  <feDisplacementMap in="SourceGraphic" in2="t" scale="90" xChannelSelector="R" yChannelSelector="G"/>
</filter>
<filter id="blur8"><feGaussianBlur stdDeviation="8"/></filter>
"""


def contours(W, H, seed, scale):
    rng = np.random.default_rng(seed)
    nx, ny = 160, int(160 * H / W)
    xs, ys = np.meshgrid(np.linspace(0, 1, nx), np.linspace(0, H / W, ny))
    z = np.zeros_like(xs)
    for _ in range(14):
        cx, cy = rng.uniform(-.1, 1.1), rng.uniform(-.1, H / W + .1)
        s = rng.uniform(.06, .25)
        z += rng.uniform(-1, 1.4) * np.exp(-((xs - cx) ** 2 + (ys - cy) ** 2) / (2 * s * s))
    import matplotlib; matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    out = []
    cf = plt.contourf(xs * W, ys * W, z, levels=9)
    for i, segs in enumerate(cf.allsegs):
        for seg in segs:
            if len(seg) < 4: continue
            d = "M" + " L".join(f"{x:.0f} {y:.0f}" for x, y in seg[::2]) + "Z"
            out.append(f'<path d="{d}" fill="#e3e8d3" fill-opacity="0.010"/>')
    cs = plt.contour(xs * W, ys * W, z, levels=26)
    for i, segs in enumerate(cs.allsegs):
        major = i % 5 == 0
        for seg in segs:
            if len(seg) < 4: continue
            d = "M" + " L".join(f"{x:.0f} {y:.0f}" for x, y in seg[::2])
            out.append(f'<path d="{d}" fill="none" stroke="#d7dccb" stroke-opacity="{.15 if major else .075}" '
                       f'stroke-width="{(1.6 if major else 1.0)*scale:.2f}"/>')
    plt.close("all")
    return "".join(out)


def compass(cx, cy, r, scale, op=.28):
    o = [f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="none" stroke="#fff" stroke-opacity="{op*.6}" stroke-width="{1.2*scale}"/>',
         f'<circle cx="{cx}" cy="{cy}" r="{r*0.86:.0f}" fill="none" stroke="#fff" stroke-opacity="{op*.35}" stroke-width="{1*scale}"/>']
    for a in range(0, 360, 2):
        L = 26 if a % 10 == 0 else 11
        rad = math.radians(a - 90)
        x1, y1 = cx + math.cos(rad) * r, cy + math.sin(rad) * r
        x2, y2 = cx + math.cos(rad) * (r - L * scale), cy + math.sin(rad) * (r - L * scale)
        o.append(f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" stroke="#fff" stroke-opacity="{op}" stroke-width="{1.3*scale:.2f}"/>')
        if a % 30 == 0:
            tx, ty = cx + math.cos(rad) * (r - 52 * scale), cy + math.sin(rad) * (r - 52 * scale)
            o.append(f'<text class="med" x="{tx:.1f}" y="{ty+8*scale:.1f}" font-size="{22*scale:.1f}" fill="#fff" fill-opacity="{op}" '
                     f'text-anchor="middle" transform="rotate({a} {tx:.1f} {ty:.1f})">{a}</text>')
    return "".join(o)


def scratches(W, H, rnd, n, scale):
    o = []
    for _ in range(n):
        x, y = rnd.uniform(0, W), rnd.uniform(0, H)
        a = math.radians(rnd.gauss(-15, 25)); L = rnd.uniform(20, 300) * scale
        o.append(f'<line x1="{x:.0f}" y1="{y:.0f}" x2="{x+math.cos(a)*L:.0f}" y2="{y+math.sin(a)*L:.0f}" stroke="#fff" '
                 f'stroke-opacity="{rnd.uniform(.02,.07):.3f}" stroke-width="{rnd.uniform(.5,1.4)*scale:.2f}"/>')
    return "".join(o)


def ragged_edges(W, H, scale):
    """Burnt/torn dark silhouettes at top and bottom, like the in-game map frame."""
    b = 70 * scale
    return (f'<g filter="url(#rag)" fill="#070807">'
            f'<rect x="-100" y="{H-b:.0f}" width="{W+200}" height="{b*2:.0f}"/>'
            f'<rect x="-100" y="{-b*1.4:.0f}" width="{W+200}" height="{b*2:.0f}" opacity=".85"/></g>')


def backdrop(W, H, seed, scale, compass_at=None, cross=None):
    rnd = random.Random(seed)
    o = [f'<rect width="{W}" height="{H}" fill="url(#bg)"/>',
         f'<rect width="{W}" height="{H}" filter="url(#stain)" opacity=".45"/>',
         contours(W, H, seed, scale)]
    step = 160 * scale
    grid = "".join(f'<line x1="{x:.0f}" y1="0" x2="{x:.0f}" y2="{H}"/>' for x in np.arange(step / 2, W, step))
    grid += "".join(f'<line x1="0" y1="{y:.0f}" x2="{W}" y2="{y:.0f}"/>' for y in np.arange(step / 2, H, step))
    o.append(f'<g stroke="#fff" stroke-opacity=".035" stroke-width="{scale:.2f}">{grid}</g>')
    if compass_at:
        o.append(compass(*compass_at, scale))
    if cross:
        cx, cy = cross
        o.append(f'<g stroke="#fff" stroke-opacity=".28" stroke-width="{1.5*scale:.2f}"><line x1="0" y1="{cy}" x2="{W}" y2="{cy}"/>'
                 f'<line x1="{cx}" y1="0" x2="{cx}" y2="{H}"/></g>')
    o.append(scratches(W, H, rnd, int(140 * scale + 40), scale))
    o.append(ragged_edges(W, H, scale))
    o.append(f'<rect width="{W}" height="{H}" filter="url(#grain)" opacity=".09"/>')
    return "".join(o)


# ------------------------------------------------------------------ HUD elements
def strip(x, y, spans, size, k="bold", align="left", padx=None, sp=None):
    """In-game notification line: crisp white rect, black uppercase text. spans=[(text, color)]"""
    sp = size * 0.015 if sp is None else sp
    padx = size * 0.26 if padx is None else padx
    w = sum(tw(k, s, size, sp) for s, _ in spans) + sp * (len(spans) - 1) + padx * 2
    h = size * 1.14
    x0 = x if align == "left" else x - w
    base = y + h / 2 + size * CAP / 2
    ts = "".join(f'<tspan fill="{c}">{s}</tspan>' for s, c in spans)
    return w, h, (f'<rect x="{x0:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" fill="#fff"/>'
                  f'<text class="{k}" x="{x0+padx:.1f}" y="{base:.1f}" font-size="{size}" letter-spacing="{sp:.2f}" fill="#000">{ts}</text>')


def stack(x, y, lines, gap, align="left"):
    """lines = [(spans, size, kwargs)] -> stacked strips, returns (svg, bottom_y, max_w)"""
    o, cy, mw = [], y, 0
    for spans, size, kw in lines:
        w, h, s = strip(x, cy, spans, size, align=align, **kw)
        o.append(s); cy += h + gap; mw = max(mw, w)
    return "".join(o), cy - gap, mw


def reticle(cx, cy, r, scale, plus=True):
    sw = 6 * scale
    g = 0.32  # gap half-angle (rad) where crosshair passes
    arcs = []
    for q in range(4):
        a0 = q * math.pi / 2 + g; a1 = (q + 1) * math.pi / 2 - g
        arcs.append(f'M{cx+r*math.cos(a0):.1f} {cy+r*math.sin(a0):.1f}A{r} {r} 0 0 1 {cx+r*math.cos(a1):.1f} {cy+r*math.sin(a1):.1f}')
    o = [f'<circle cx="{cx}" cy="{cy}" r="{r*1.02:.1f}" fill="#000" fill-opacity=".35"/>',
         f'<path d="{"".join(arcs)}" fill="none" stroke="#fff" stroke-width="{sw:.1f}" stroke-linecap="butt"/>',
         f'<circle cx="{cx}" cy="{cy}" r="{r*1.32:.1f}" fill="none" stroke="#fff" stroke-opacity=".35" stroke-width="{1.5*scale:.1f}"/>']
    if plus:
        a, t = r * 0.42, r * 0.16
        o.append(f'<path d="M{cx-t/2:.1f} {cy-a:.1f}h{t:.1f}v{a-t/2:.1f}h{a-t/2:.1f}v{t:.1f}h{-(a-t/2):.1f}v{a-t/2:.1f}h{-t:.1f}'
                 f'v{-(a-t/2):.1f}h{-(a-t/2):.1f}v{-t:.1f}h{a-t/2:.1f}z" fill="{ACC}" stroke="#fff" stroke-width="{2*scale:.1f}"/>')
    return "".join(o)


def tag(x, y, title, sub, scale):
    """Black name tag + small data lines (like the map's player marker label)."""
    s = 30 * scale
    w = tw("bold", title, s, 1) + 26 * scale
    o = [f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{s*1.25:.1f}" fill="#000" fill-opacity=".85"/>',
         T("bold", x + 13 * scale, y + s * 1.25 / 2 + s * CAP / 2, title, s, sp=1)]
    for i, line in enumerate(sub):
        o.append(T("semi", x + 6 * scale, y + s * 1.25 + (28 + i * 25) * scale, line, 21 * scale, sp=1.5 * scale, op=.85))
    return "".join(o)


def checkbox(x, y, s, checked=True, op=.75):
    o = f'<rect x="{x:.1f}" y="{y:.1f}" width="{s:.1f}" height="{s:.1f}" fill="none" stroke="#fff" stroke-opacity="{op}" stroke-width="{s*.11:.1f}"/>'
    if checked:
        i = s * .24
        o += (f'<path d="M{x+i:.1f} {y+i:.1f}L{x+s-i:.1f} {y+s-i:.1f}M{x+s-i:.1f} {y+i:.1f}L{x+i:.1f} {y+s-i:.1f}" '
              f'stroke="#fff" stroke-opacity="{op}" stroke-width="{s*.11:.1f}"/>')
    return o


FEATURES = ["HUD SCALING &amp; SAFE ZONE", "MISPLACED HUD BOXES FIXED", "MULTIPLAYER-SAFE"]
VERSION = "FOR THE CURRENT PC VERSION  ·  PATCH 838  ·  STEAM &amp; EPIC"
BUILD = "STEAM BUILD 9573671"


def subtitle_box(x, y, w, scale):
    hs, pad = 30 * scale, 22 * scale
    hh = pad * 1.6 + hs * 2.3
    return (f'<rect x="{x}" y="{y}" width="{w}" height="{hh:.1f}" fill="#000" fill-opacity=".72"/>' +
            T("semi", x + pad, y + pad + hs * .78, "HUD FIX &amp; QUALITY-OF-LIFE", hs, fill="#b4b6b0", sp=1) +
            T("semi", x + pad, y + pad + hs * 1.95, "FOR TOMB RAIDER (2013)", hs, fill="#b4b6b0", sp=1)), y + hh


def objective_panel(x, y, w, scale, features=FEATURES):
    """Black objective panel: grey header (subtitle) + checklist."""
    hs, fs = 30 * scale, 27 * scale
    pad = 22 * scale
    o = []
    hh = pad * 1.6 + hs * 2.3
    o.append(f'<rect x="{x}" y="{y}" width="{w}" height="{hh:.1f}" fill="#1b1c1b" fill-opacity=".82"/>')
    o.append(T("semi", x + pad, y + pad + hs * .78, "HUD FIX &amp; QUALITY-OF-LIFE", hs, fill="#a3a59f", sp=1))
    o.append(T("semi", x + pad, y + pad + hs * 1.95, "FOR TOMB RAIDER (2013)", hs, fill="#a3a59f", sp=1))
    by = y + hh
    lh = fs * 1.62
    bh = pad * 1.4 + lh * len(features)
    o.append(f'<rect x="{x}" y="{by:.1f}" width="{w}" height="{bh:.1f}" fill="#000" fill-opacity=".86"/>')
    for i, f in enumerate(features):
        ly = by + pad * .7 + lh * i + lh / 2
        o.append(checkbox(x + pad, ly - fs * .42, fs * .62, True, .8))
        o.append(T("bold", x + pad + fs * 1.05, ly + fs * CAP / 2, f, fs, sp=.6, op=.95))
    return "".join(o), by + bh


def button(x, y, label, size, scale, dim=False):
    w = tw("bold", label, size, 1) + 36 * scale
    h = size * 1.6
    return w, (f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" fill="#000" fill-opacity=".88"/>' +
               T("bold", x + w / 2, y + h / 2 + size * CAP / 2, label, size, fill="#fff", anchor="middle", sp=1, op=.45 if dim else .95))


def buttons_right(xr, y, labels, size, scale, gap):
    o, bx = [], xr
    for lab in reversed(labels):
        bx -= tw("bold", lab, size, 1) + 36 * scale
        o.append(button(bx, y, lab, size, scale)[1]); bx -= gap
    return "".join(o)


def emblem(cx, cy, S):
    """Logo mark: safe-zone corner brackets framing a white badge with 'H+' + crimson underline."""
    h = S / 2; L = S * .24; sw = S * .065
    br = "".join(f'<path d="M{cx+dx*h:.1f} {cy+dy*(h-L):.1f}V{cy+dy*h:.1f}H{cx+dx*(h-L):.1f}"/>'
                 for dx in (-1, 1) for dy in (-1, 1))
    o = [f'<g fill="none" stroke="#fff" stroke-width="{sw:.1f}" stroke-linecap="square">{br}</g>']
    b = S * .56; c = b * .16
    x0, y0 = cx - b / 2, cy - b / 2 - S * .02
    o.append(f'<path d="M{x0:.1f} {y0:.1f}H{x0+b:.1f}V{y0+b-c:.1f}L{x0+b-c:.1f} {y0+b:.1f}H{x0:.1f}Z" fill="#fff"/>')
    fs = b * .62
    o.append(T("bold", cx, y0 + b / 2 + fs * CAP / 2, "H+", fs, fill="#000", anchor="middle", sp=0))
    o.append(f'<rect x="{x0:.1f}" y="{y0+b+S*.05:.1f}" width="{b:.1f}" height="{S*.035:.1f}" fill="{ACC}"/>')
    return "".join(o)


def crosshair(W, H, cx, cy, gap, scale, x0=0):
    return (f'<g stroke="#fff" stroke-opacity=".22" stroke-width="{1.5*scale:.2f}">'
            f'<line x1="{x0}" y1="{cy}" x2="{cx-gap:.0f}" y2="{cy}"/><line x1="{cx+gap:.0f}" y1="{cy}" x2="{W}" y2="{cy}"/>'
            f'<line x1="{cx}" y1="0" x2="{cx}" y2="{cy-gap:.0f}"/><line x1="{cx}" y1="{cy+gap:.0f}" x2="{cx}" y2="{H}"/></g>')


DISCL = "Unofficial fan-made mod. Not affiliated with or endorsed by the developers or publishers of Tomb Raider."


def svg(W, H, seed, body):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">'
            f'<title>TR2013 HUD Fix+ (unofficial community mod)</title><defs>{css()}{defs(seed)}</defs>{body}</svg>')


def accent_tab(x, y, h, scale):
    return f'<rect x="{x-9*scale:.1f}" y="{y:.1f}" width="{6*scale:.1f}" height="{h:.1f}" fill="{ACC}"/>'


def title_block(x, y, s):
    """Name strips + version info. Returns (svg, bottom)."""
    o = []
    w1, h1, a = strip(x, y, [("TR2013", "#000")], 54 * s); o.append(a)
    y2 = y + h1 + 5 * s
    w2, h2, b = strip(x, y2, [("HUD FIX+", "#000")], 176 * s, padx=34 * s, sp=2 * s); o.append(b)
    o.append(accent_tab(x, y, h1 + 5 * s + h2, s))
    y3 = y2 + h2 + 5 * s
    w3, h3, c = strip(x, y3, [("[UNOFFICIAL COMMUNITY MOD]", ACC)], 40 * s); o.append(c)
    # version: narrow dark bar, white text
    vs = 25 * s
    y4 = y3 + h3 + 16 * s
    vw = tw("semi", VERSION, vs, 1.2 * s) + 32 * s; vh = vs * 1.55
    o.append(f'<rect x="{x:.1f}" y="{y4:.1f}" width="{vw:.1f}" height="{vh:.1f}" fill="#000" fill-opacity=".88"/>')
    o.append(T("semi", x + 16 * s, y4 + vh / 2 + vs * CAP / 2, VERSION, vs, sp=f"{1.2*s:.2f}"))
    y5 = y4 + vh + 26 * s
    o.append(T("semi", x + 16 * s, y5, BUILD, 19 * s, fill="#c9cbc4", op=.75, sp=f"{2.4*s:.2f}"))
    return "".join(o), y5


# ------------------------------------------------------------------ 1. Nexus
def nexus():
    W, H, s = 1920, 1080, 1.0
    ex, ey = 1450, 540
    bg = backdrop(W, H, 11, s)
    tb, bottom = title_block(150, 225, s)
    panel, _ = subtitle_box(150, bottom + 40, 640, s)
    body = (bg + crosshair(W, H, ex, ey, 200, s, 930) + emblem(ex, ey, 300) + tb + panel +
            T("med", W - 60, H - 24, DISCL, 17, anchor="end", op=.45, sp=.6))
    return svg(W, H, 11, body)


# ------------------------------------------------------------------ 2. GitHub
def github():
    W, H, s = 1280, 640, 0.62
    ex, ey = 1000, 320
    bg = backdrop(W, H, 23, s)
    tb, bottom = title_block(80, 116, s)
    panel, _ = subtitle_box(80, bottom + 26, 420, s)
    body = (bg + crosshair(W, H, ex, ey, 140, s, 580) + emblem(ex, ey, 210) + tb + panel +
            T("med", W - 40, H - 16, DISCL, 12, anchor="end", op=.45, sp=.4))
    return svg(W, H, 23, body)


# ------------------------------------------------------------------ 3. Icon
def icon():
    W = H = 512; s = 0.5
    bg = backdrop(W, H, 37, s)
    em = emblem(256, 186, 250)
    size = 92
    w1 = tw("bold", "HUD FIX+", size, 1) + 40
    _, h1, a = strip(256 - w1 / 2, 340, [("HUD FIX+", "#000")], size, padx=20, sp=1)
    return svg(W, H, 37, bg + em + a + accent_tab(256 - w1 / 2, 340, h1, 1.2))


# ------------------------------------------------------------------ 4. Before / After
def compare(before=None, after=None, caption="", seed=51):
    W, H, s = 1920, 1080, 1.0
    m, gap = 40, 24
    pw = (W - 2 * m - gap) / 2; ph = pw * 9 / 16; py = 210
    bg = backdrop(W, H, seed, s)
    _, hh, hdr = strip(m + 10, 70, [("TR2013 HUD FIX+", "#000")], 60, padx=18, sp=1)
    hdr += accent_tab(m + 10, 70, hh, s)
    _, _, unof = strip(W - m, 70 + 60 * 1.14 / 2 - 30 * 1.14 / 2, [("[UNOFFICIAL COMMUNITY MOD]", ACC)], 30, align="right")
    o = [bg, hdr, unof]
    for i, (href, lab) in enumerate(((before, "BEFORE"), (after, "AFTER"))):
        px = m + i * (pw + gap)
        o.append(f'<clipPath id="c{i}"><rect x="{px:.1f}" y="{py}" width="{pw:.1f}" height="{ph:.1f}"/></clipPath>')
        if href:
            o.append(f'<image x="{px:.1f}" y="{py}" width="{pw:.1f}" height="{ph:.1f}" href="{href}" preserveAspectRatio="xMidYMid slice" clip-path="url(#c{i})"/>')
        else:
            o.append(f'<rect x="{px:.1f}" y="{py}" width="{pw:.1f}" height="{ph:.1f}" fill="#3a3c38" fill-opacity=".7"/>' +
                     T("semi", px + pw / 2, py + ph / 2 + 10, "SCREENSHOT", 30, anchor="middle", op=.3, sp=6))
        o.append(f'<rect x="{px:.1f}" y="{py}" width="{pw:.1f}" height="{ph:.1f}" fill="none" stroke="#fff" stroke-opacity="{.9 if i else .35}" stroke-width="2"/>')
        if lab == "BEFORE":
            w = tw("bold", lab, 38, 2) + 40
            o.append(f'<rect x="{px:.1f}" y="{py+ph+14:.1f}" width="{w:.1f}" height="{38*1.25:.1f}" fill="#000" fill-opacity=".85"/>' +
                     T("bold", px + 20, py + ph + 14 + 38 * 1.25 / 2 + 38 * CAP / 2, lab, 38, fill="#9a9c96", sp=2))
            o.append(T("semi", px + w + 18, py + ph + 14 + 38 * 1.25 / 2 + 22 * CAP / 2, "VANILLA", 22, op=.45, sp=3))
        else:
            _, sh, st = strip(px, py + ph + 14, [("AFTER", "#000")], 38, sp=2)
            o.append(st + f'<rect x="{px:.1f}" y="{py+ph+14+sh:.1f}" width="{tw("bold","AFTER",38,2)+38*.52:.1f}" height="5" fill="{ACC}"/>')
            w = tw("bold", "AFTER", 38, 2) + 38 * .52
            o.append(T("semi", px + w + 18, py + ph + 14 + 38 * 1.14 / 2 + 22 * CAP / 2, "HUD FIX+", 22, fill="#fff", op=.8, sp=3))
    if caption:
        cap = caption.upper().replace("&", "&amp;")
        cw = tw("bold", cap, 34, 1) + 60
        cy = py + ph + 110
        o.append(f'<rect x="{W/2-cw/2:.1f}" y="{cy:.1f}" width="{cw:.1f}" height="{34*1.5:.1f}" fill="#000" fill-opacity=".8"/>' +
                 T("bold", W / 2, cy + 34 * 1.5 / 2 + 34 * CAP / 2, cap, 34, anchor="middle", sp=1))
    o.append(T("med", W - 40, H - 22, DISCL, 17, anchor="end", op=.45, sp=.6))
    return svg(W, H, seed, "".join(o))


# ------------------------------------------------------------------ 5. Single showcase
def showcase(img=None, caption="", seed=67):
    W, H, s = 1920, 1080, 1.0
    m = 40
    ph = 792; pw = ph * 16 / 9; px = (W - pw) / 2; py = 150
    bg = backdrop(W, H, seed, s)
    _, hh, hdr = strip(m + 10, 50, [("TR2013 HUD FIX+", "#000")], 60, padx=18, sp=1)
    hdr += accent_tab(m + 10, 50, hh, s)
    _, _, unof = strip(W - m, 50 + 60 * 1.14 / 2 - 30 * 1.14 / 2, [("[UNOFFICIAL COMMUNITY MOD]", ACC)], 30, align="right")
    o = [bg, hdr, unof, f'<clipPath id="c0"><rect x="{px:.1f}" y="{py}" width="{pw:.1f}" height="{ph:.1f}"/></clipPath>']
    if img:
        o.append(f'<image x="{px:.1f}" y="{py}" width="{pw:.1f}" height="{ph:.1f}" href="{img}" preserveAspectRatio="xMidYMid slice" clip-path="url(#c0)"/>')
    else:
        o.append(f'<rect x="{px:.1f}" y="{py}" width="{pw:.1f}" height="{ph:.1f}" fill="#3a3c38" fill-opacity=".7"/>' +
                 T("semi", px + pw / 2, py + ph / 2 + 10, "SCREENSHOT", 30, anchor="middle", op=.3, sp=6))
    o.append(f'<rect x="{px:.1f}" y="{py}" width="{pw:.1f}" height="{ph:.1f}" fill="none" stroke="#fff" stroke-opacity=".9" stroke-width="2"/>')
    if caption:
        cap = caption.upper().replace("&", "&amp;")
        cw = tw("bold", cap, 34, 1) + 60
        cy = py + ph + 26
        o.append(f'<rect x="{W/2-cw/2:.1f}" y="{cy:.1f}" width="{cw:.1f}" height="{34*1.5:.1f}" fill="#000" fill-opacity=".8"/>' +
                 f'<rect x="{W/2-cw/2:.1f}" y="{cy+34*1.5:.1f}" width="{cw:.1f}" height="4" fill="{ACC}"/>' +
                 T("bold", W / 2, cy + 34 * 1.5 / 2 + 34 * CAP / 2, cap, 34, anchor="middle", sp=1))
    o.append(T("med", W - 40, H - 14, DISCL, 17, anchor="end", op=.45, sp=.6))
    return svg(W, H, seed, "".join(o))


def data_uri(p):
    ext = os.path.splitext(p)[1].lower().lstrip(".")
    return f"data:image/{'jpeg' if ext in ('jpg','jpeg') else ext};base64," + base64.b64encode(open(p, "rb").read()).decode()


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    if len(sys.argv) > 1 and sys.argv[1] == "compare":
        _, _, b, a, cap, name = sys.argv
        open(os.path.join(OUT, name + ".svg"), "w", encoding="utf-8").write(compare(data_uri(b), data_uri(a), cap)); sys.exit()
    if len(sys.argv) > 1 and sys.argv[1] == "showcase":
        _, _, i, cap, name = sys.argv
        open(os.path.join(OUT, name + ".svg"), "w", encoding="utf-8").write(showcase(data_uri(i), cap)); sys.exit()
    for n, fn in (("nexus_1920x1080", nexus), ("github_1280x640", github), ("icon_512", icon)):
        open(os.path.join(OUT, f"tr2013-hudfix_{n}.svg"), "w", encoding="utf-8").write(fn())
    open(os.path.join(OUT, "tr2013-hudfix_compare_TEMPLATE.svg"), "w", encoding="utf-8").write(compare(caption="Example — what changed"))
    open(os.path.join(OUT, "tr2013-hudfix_showcase_TEMPLATE.svg"), "w", encoding="utf-8").write(showcase(caption="Example — showcase"))
    print("ok")
