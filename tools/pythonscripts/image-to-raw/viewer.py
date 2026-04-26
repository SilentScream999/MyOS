import numpy as np
from PIL import Image

filename = "output_bgra.raw"

# 🔥 SET THESE FROM CONVERTER OUTPUT
width = 1920
height = 1080

FLIP_VERTICAL = False  # toggle if upside-down

# =============================
# LOAD RAW
# =============================
with open(filename, "rb") as f:
    raw = f.read()

print("Raw size:", len(raw))
print("Expected:", width * height * 4)

if len(raw) != width * height * 4:
    raise ValueError("Size mismatch — wrong width/height or bad file")

# =============================
# PROCESS
# =============================
data = np.frombuffer(raw, dtype=np.uint8)
data = data.reshape((height, width, 4))

# BGRA → RGBA
data = data[:, :, [2, 1, 0, 3]]

if FLIP_VERTICAL:
    data = np.flipud(data)

# =============================
# DISPLAY
# =============================
img = Image.fromarray(data, "RGBA")
img.show()

img.save("preview.png")
print("Saved preview.png")
