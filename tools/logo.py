# Draws logo.png: a small flame graph and a stopwatch. python tools/logo.py
from PIL import Image, ImageDraw

S = 512
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
d.rounded_rectangle((16, 16, S - 16, S - 16), radius=96, fill=(28, 32, 48, 255))
green, red, amber, blue = (120, 200, 120), (240, 110, 110), (255, 190, 80), (86, 156, 255)
bars = [
    (64, 380, 448, 436, blue),
    (64, 316, 300, 372, green), (308, 316, 448, 372, amber),
    (64, 252, 200, 308, green), (208, 252, 300, 308, red), (308, 252, 400, 308, amber),
    (64, 188, 150, 244, green), (208, 188, 280, 244, red),
    (84, 124, 140, 180, green),
]
for x0, y0, x1, y1, color in bars:
    d.rounded_rectangle((x0, y0, x1, y1), radius=10, fill=color + (255,))
white = (235, 235, 245, 255)
d.ellipse((322, 72, 454, 204), outline=white, width=14)
d.line((388, 138, 388, 100), fill=white, width=12)
d.line((388, 138, 416, 156), fill=white, width=12)
d.rectangle((376, 46, 400, 66), fill=white)
img.save("logo.png")
