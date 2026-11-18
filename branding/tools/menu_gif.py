"""TR2013 HUD Fix+ - settings-menu showcase (animated GIF + stills) from menu_preview.exe frames.

    build.bat preview
    build\\menu_preview.exe <frames_dir>
    python menu_gif.py <frames_dir> <out_dir>

Needs: pip install pillow fonttools brotli
Frames are the real overlay rendered by menu_preview.exe (same code, style and font as in game).
"""
import io, os, sys, glob
from PIL import Image, ImageDraw, ImageFont
from fontTools.ttLib import TTFont

HERE = os.path.dirname(os.path.abspath(__file__))
FONTS = os.path.join(HERE, "fonts")
BG = (30, 32, 28)
ACC = (158, 28, 58)
W, H = 1280, 800
MENU_BOX = (53, 53, 53 + 626, 53 + 741)   # menu union rectangle printed by menu_preview.exe

TABS = [
    ("01_fix", "FIX", "Choose which HUD bugs get fixed",
     ["Notifications back on screen", "Prompt and popup position", "Black stain: Move / Hide / Off",
      "Salvage counter back top-right"]),
    ("02_performance", "PERFORMANCE", "FPS limiter and live frame times",
     ["FPS limit and FPS overlay", "Frame time graph and 1% low", "Address space bar",
      "Low-memory warning"]),
    ("03_display", "DISPLAY", "Display options",
     ["Borderless window (experimental)", "Flip model and tearing", "Applied after a game restart"]),
    ("04_hotkeys", "HOTKEYS", "Every key can be rebound",
     ["Menu, fix toggle, FPS overlay", "Change / Clear in the menu", "Or key names in tr_hud_fix.ini"]),
    ("05_info", "INFO", "Live statistics",
     ["Corrected HUD draws per second", "Address space of the game", "Gameplay / menu detection"]),
]
KEYS = [("INSERT", "open / close menu"), ("F9", "fix on / off"), ("ESC", "close menu")]


def font(weight, size):
    f = TTFont(os.path.join(FONTS, f"barlow-semi-condensed-latin-{weight}-normal.woff2"))
    f.flavor = None
    b = io.BytesIO()
    f.save(b)
    b.seek(0)
    return ImageFont.truetype(b, size)


F_TITLE, F_HEAD, F_BODY, F_KEY, F_SMALL = font(700, 46), font(700, 34), font(500, 25), font(700, 24), font(500, 18)


def strip(d, x, y, text, f, fg=(0, 0, 0), bg=(240, 240, 235), padx=14, pady=6):
    l, t, r, b = d.textbbox((0, 0), text, font=f)
    w, h = r - l + 2 * padx, b - t + 2 * pady
    d.rectangle([x, y, x + w, y + h], fill=bg)
    d.text((x + padx - l, y + pady - t), text, font=f, fill=fg)
    return w, h


def frame(menu_img, tab_idx):
    key, name, head, bullets = TABS[tab_idx]
    im = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(im)
    im.paste(menu_img, (40, (H - menu_img.height) // 2))
    x0 = 720
    w, h = strip(d, x0, 52, "TR2013 HUD FIX+", F_TITLE)
    d.rectangle([x0 - 8, 52, x0 - 3, 52 + h], fill=ACC)
    strip(d, x0, 52 + h + 8, "[SETTINGS OVERLAY]", F_SMALL, fg=ACC)
    # tab progress
    y = 190
    tx = x0
    for i, t in enumerate(TABS):
        sel = i == tab_idx
        l, tt, r, b = d.textbbox((0, 0), t[1], font=F_SMALL)
        tw = r - l + 18
        d.rectangle([tx, y, tx + tw, y + 30], fill=(240, 240, 235) if sel else (44, 46, 42))
        d.text((tx + 9 - l, y + 15 - (b - tt) // 2 - tt), t[1], font=F_SMALL, fill=(0, 0, 0) if sel else (170, 172, 165))
        tx += tw + 6
    d.text((x0, 250), head.upper(), font=F_HEAD, fill=(245, 245, 240))
    by = 305
    for bl in bullets:
        d.rectangle([x0, by + 11, x0 + 7, by + 18], fill=ACC)
        d.text((x0 + 20, by), bl, font=F_BODY, fill=(215, 217, 210))
        by += 40
    # key legend
    ky = 600
    for k, desc in KEYS:
        kw, kh = strip(d, x0, ky, k, F_KEY, padx=12, pady=5)
        d.text((x0 + max(kw, 105) + 16, ky + kh / 2), desc, font=F_BODY, fill=(215, 217, 210), anchor="lm")
        ky += kh + 12
    d.text((x0, ky + 6), "Keys can be changed in the HOTKEYS tab.", font=F_SMALL, fill=(150, 152, 146))
    d.text((W - 30, H - 22), "Real in-game menu, rendered by the mod's own code. Values are sample data.",
           font=F_SMALL, fill=(110, 112, 106), anchor="rs")
    return im


def main(src, out):
    os.makedirs(out, exist_ok=True)
    frames, durs = [], []
    for i, (key, *_rest) in enumerate(TABS):
        files = sorted(glob.glob(os.path.join(src, key + "_*.bmp")))
        crops = [Image.open(p).convert("RGB").crop(MENU_BOX) for p in files]
        still = frame(crops[0], i)
        still.save(os.path.join(out, f"tr2013-hudfix_menu_{key[3:]}.png"))
        if len(crops) > 1:      # animated tab: frame-time graph moves
            for c in crops[1:]:
                frames.append(frame(c, i))
                durs.append(120)
        else:
            frames.append(still)
            durs.append(2600)
    gif = os.path.join(out, "tr2013-hudfix_menu.gif")
    pal = [f.quantize(colors=128, method=Image.Quantize.MEDIANCUT) for f in frames]
    pal[0].save(gif, save_all=True, append_images=pal[1:], duration=durs, loop=0, optimize=True, disposal=1)
    print(gif, os.path.getsize(gif) // 1024, "KB,", len(frames), "frames")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
