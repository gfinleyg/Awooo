"""Compares render_python.py's pictures with render_widgets.exe's, scene by scene: how many pixels differ (and by how
much), and a picture of each pair with their differences (PY_DIR/../diff/<scene>.png: Python on top, C++ below, then
the differing pixels in red).

    py -3.10 compare.py PY_DIR CPP_DIR [DIFF_DIR]"""

import os
import sys

from PIL import Image, ImageChops


def main():
    pyDir, cppDir = sys.argv[1], sys.argv[2]
    diffDir = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.dirname(os.path.abspath(pyDir)), "diff")
    os.makedirs(diffDir, exist_ok=True)
    for name in sorted(os.listdir(pyDir)):
        if not name.endswith(".png"):
            continue
        cppPath = os.path.join(cppDir, name)
        if not os.path.exists(cppPath):
            print(f"{name}: no C++ picture")
            continue
        a = Image.open(os.path.join(pyDir, name)).convert("RGBA")
        b = Image.open(cppPath).convert("RGBA")
        if a.size != b.size:
            print(f"{name}: sizes differ, {a.size} and {b.size}")
            b = b.resize(a.size)
        diff = ImageChops.difference(a, b)
        pixels = list(diff.getdata())
        changed = [max(p) for p in pixels if max(p) > 0]
        big = [c for c in changed if c > 48]
        print(f"{name}: {len(changed)} pixels differ ({100*len(changed)/len(pixels):.2f}%), "
              f"{len(big)} by more than 48, most {max(changed, default=0)}")
        mask = Image.eval(diff.convert("L"), lambda v: 255 if v > 0 else 0)
        red = Image.new("RGBA", a.size, (255, 0, 0, 255))
        marked = Image.composite(red, a.convert("RGBA"), mask)
        out = Image.new("RGBA", (a.width, a.height*3), (255, 255, 255, 255))
        out.paste(a, (0, 0))
        out.paste(b, (0, a.height))
        out.paste(marked, (0, 2*a.height))
        out.save(os.path.join(diffDir, name))


if __name__ == "__main__":
    main()
