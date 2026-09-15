# Generates the MUI2 welcome/finish page bitmap (164x314, 24-bit BMP) for the
# NSIS installer, using the app icon + product branding. The bundled NSIS at
# .../nsis/Contrib/Graphics/Wizard is empty, so a branded image is supplied.
import os

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
W, H = 164, 314

# Dark vertical gradient background (matches the app's dark theme).
top = (54, 60, 72)
bot = (30, 32, 38)
img = Image.new('RGB', (W, H))
d = ImageDraw.Draw(img)
for y in range(H):
    t = y / (H - 1)
    r = int(top[0] + (bot[0] - top[0]) * t)
    g = int(top[1] + (bot[1] - top[1]) * t)
    b = int(top[2] + (bot[2] - top[2]) * t)
    d.line([(0, y), (W, y)], fill=(r, g, b))

# App icon scaled to fit the upper area.
icon = Image.open(os.path.join(HERE, '..', 'resources', 'icons', 'app_256.png')).convert('RGBA')
icon = icon.resize((150, 150), Image.LANCZOS)
img.paste(icon, ((W - 150) // 2, 26), icon)

def font(size, bold=True):
    for f in ('C:/Windows/Fonts/msyhbd.ttc' if bold else 'C:/Windows/Fonts/msyh.ttc',
              'C:/Windows/Fonts/arialbd.ttf', 'C:/Windows/Fonts/arial.ttf'):
        if os.path.exists(f):
            return ImageFont.truetype(f, size)
    return ImageFont.load_default()

d.text((W // 2, 198), 'Silver Halide', font=font(17), fill=(232, 234, 240), anchor='mm')
d.text((W // 2, 268), 'v2.11.0  \u00a9ASE-128 2026', font=font(12, bold=False), fill=(150, 156, 168), anchor='mm')

out = os.path.join(HERE, 'welcome.bmp')
img.save(out, 'BMP')
print('wrote', out)
