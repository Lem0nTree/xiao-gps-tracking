"""Render the bundled binary STL without requiring a CAD application."""

from pathlib import Path
import struct

import numpy as np
from PIL import Image, ImageDraw, ImageFilter


ROOT = Path(__file__).resolve().parent
STL_PATH = ROOT / "Seeed_GPS_Tracker.stl"
OUTPUT_PATH = ROOT / "xiao-gps-tracker-enclosure.png"
CANVAS = 1400


def load_binary_stl(path: Path) -> np.ndarray:
    data = path.read_bytes()
    triangle_count = struct.unpack_from("<I", data, 80)[0]
    expected_size = 84 + triangle_count * 50
    if len(data) != expected_size:
        raise ValueError("Expected a binary STL file")

    triangles = np.empty((triangle_count, 3, 3), dtype=np.float32)
    for index in range(triangle_count):
        offset = 84 + index * 50 + 12
        triangles[index] = np.frombuffer(data, dtype="<f4", count=9, offset=offset).reshape(3, 3)
    return triangles


def rotation_x(angle: float) -> np.ndarray:
    cosine, sine = np.cos(angle), np.sin(angle)
    return np.array(((1, 0, 0), (0, cosine, -sine), (0, sine, cosine)))


def rotation_z(angle: float) -> np.ndarray:
    cosine, sine = np.cos(angle), np.sin(angle)
    return np.array(((cosine, -sine, 0), (sine, cosine, 0), (0, 0, 1)))


triangles = load_binary_stl(STL_PATH)
triangles -= (triangles.min(axis=(0, 1)) + triangles.max(axis=(0, 1))) / 2
rotation = rotation_x(np.deg2rad(-61)) @ rotation_z(np.deg2rad(34))
rotated = triangles @ rotation.T

projected = rotated[:, :, :2]
minimum = projected.min(axis=(0, 1))
maximum = projected.max(axis=(0, 1))
scale = (CANVAS * 0.72) / max(maximum - minimum)
projected = projected * scale
projected[:, :, 0] += CANVAS / 2
projected[:, :, 1] = CANVAS - (projected[:, :, 1] + CANVAS / 2)

background = Image.new("RGBA", (CANVAS, CANVAS), (244, 248, 255, 255))
shadow = Image.new("RGBA", background.size, (0, 0, 0, 0))
shadow_draw = ImageDraw.Draw(shadow)
shadow_draw.ellipse((250, 1010, 1150, 1225), fill=(27, 49, 84, 75))
shadow = shadow.filter(ImageFilter.GaussianBlur(48))
background.alpha_composite(shadow)

draw = ImageDraw.Draw(background)
light = np.array((0.2, -0.35, 0.92))
light /= np.linalg.norm(light)
depth_order = np.argsort(rotated[:, :, 2].mean(axis=1))

for index in depth_order:
    face = rotated[index]
    normal = np.cross(face[1] - face[0], face[2] - face[0])
    length = np.linalg.norm(normal)
    if length == 0:
        continue
    normal /= length
    if normal[2] <= 0:
        continue
    intensity = 0.42 + 0.58 * max(0.0, float(np.dot(normal, light)))
    base = np.array((47, 111, 229))
    color = tuple(np.clip(base * intensity + 255 * (1 - intensity) * 0.14, 0, 255).astype(int))
    points = [tuple(point) for point in projected[index]]
    draw.polygon(points, fill=color + (255,))

background.save(OUTPUT_PATH, optimize=True)
print(OUTPUT_PATH)
