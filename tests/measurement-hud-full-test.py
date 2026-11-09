"""Find the complete rendered measurement text in a full DXVK HUD capture."""
import json
import struct
import sys
from pathlib import Path


def bitmap_mask(path):
    data = Path(path).read_bytes()
    assert data[:2] == b"BM", "expected real BMP capture"
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    planes, bits, compression = struct.unpack_from("<HHI", data, 26)
    assert width > 0 and height and (planes, bits, compression) == (1, 32, 0)
    rows = []
    for y in range(abs(height)):
        source_y = y if height < 0 else height - 1 - y
        row = data[offset + source_y * width * 4:offset + (source_y + 1) * width * 4]
        assert len(row) == width * 4
        # Ignore DIB alpha and low-intensity SDF fringe differences. The black
        # fixture leaves the text and any overlapping graph as foreground.
        rows.append(bytes(max(row[x:x + 3]) > 64 for x in range(0, len(row), 4)))
    return width, abs(height), rows


def locate(template, rows):
    anchor_index = max(range(len(template)), key=lambda i: sum(template[i]))
    anchor = template[anchor_index]
    for y in range(len(rows) - len(template) + 1):
        x = rows[y + anchor_index].find(anchor)
        while x >= 0:
            if all(row[x:x + len(expected)] == expected
                   for row, expected in zip(rows[y:y + len(template)], template)):
                return x, y
            x = rows[y + anchor_index].find(anchor, x + 1)
    return None


def main():
    _, _, reference = bitmap_mask(sys.argv[1])
    ink = [(x, y) for y, row in enumerate(reference) for x, value in enumerate(row) if value]
    assert ink, "reference measurement HUD must contain rendered text"
    left, right = min(x for x, _ in ink), max(x for x, _ in ink) + 1
    top, bottom = min(y for _, y in ink), max(y for _, y in ink) + 1
    assert bottom - top >= 110, "reference must contain all eight measurement lines"
    template = [row[left:right] for row in reference[top:bottom]]
    failed = False
    for path in sys.argv[2:]:
        width, height, rows = bitmap_mask(path)
        position = locate(template, rows)
        print(json.dumps({"capture": path, "width": width, "height": height,
                          "completeEightLineHud": position is not None,
                          "position": position, "textSize": [right - left, bottom - top]}))
        failed |= position is None
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
