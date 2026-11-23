"""Check SectionGlass recordings exported by the Fabric JAR contract."""

import argparse
from dataclasses import dataclass
import json
import math
from pathlib import Path
import struct

from bridge_v27_replay import frames

DEFAULT_CONTRACTS = Path(__file__).resolve().parents[2] / "fabric-bridge/build/tmp/verifySectionGlass"
HEADER = struct.Struct(">I3i4H")
MATERIAL = struct.Struct(">4f")
BOX = struct.Struct(">2H6f")
CASES = ("clear", "blue", "green", "green-stacked", "green-pane", "green-rect-half", "green-rect-full",
         "green-sun", "multi-light", "multipart", "transaction")


@dataclass(frozen=True)
class GlassBox:
    minimum: tuple
    maximum: tuple
    transmittance: tuple
    reference_thickness: float


@dataclass(frozen=True)
class GlassUpdate:
    scene: int
    section: tuple
    parts: tuple
    boxes: tuple


def require(valid, description):
    if not valid:
        raise ValueError(description)


def decode(data):
    """Read a complete set of TCP frames written by the Java encoder."""
    boxes, parts = [], []
    group = None
    for kind, body in frames(data):
        require(kind == 40 and len(body) >= HEADER.size, "expected SectionGlass with a complete header")
        scene, sx, sy, sz, index, total, materials, count = HEADER.unpack_from(body)
        key = (scene, (sx, sy, sz), total)
        if group is None:
            group = key
        require(group == key and 1 <= total <= 256 and index == len(parts) < total, "part header mismatch")
        require(0 <= materials <= 4096 and 0 <= count <= 4096, "part record count out of range")
        require((count and materials) or (index == 0 and total == 1 and materials == count == 0),
                "only an empty whole replacement may have zero records")
        first_box = HEADER.size + materials * MATERIAL.size
        require(len(body) == first_box + count * BOX.size, "part length does not match record counts")
        table = tuple(MATERIAL.iter_unpack(body[HEADER.size:first_box]))
        require(all(all(0.0 <= value <= 1.0 for value in row[:3]) and math.isfinite(row[3]) and row[3] > 0
                    for row in table), "invalid filter material")
        for pos, material, x0, y0, z0, x1, y1, z1 in BOX.iter_unpack(body[first_box:]):
            require(pos < 4096 and material < materials, "invalid local position or material index")
            local_min, local_max = (x0, y0, z0), (x1, y1, z1)
            require(all(math.isfinite(a) and math.isfinite(b) and a < b for a, b in zip(local_min, local_max)),
                    "invalid filter box bounds")
            origin = (16 * sx + (pos & 15), 16 * sy + ((pos >> 4) & 15), 16 * sz + (pos >> 8))
            row = table[material]
            boxes.append(GlassBox(tuple(a + b for a, b in zip(origin, local_min)),
                                  tuple(a + b for a, b in zip(origin, local_max)), row[:3], row[3]))
        parts.append((materials, count))
    require(group is not None and len(parts) == group[2], "incomplete SectionGlass part group")
    return GlassUpdate(group[0], group[1], tuple(parts), tuple(boxes))


def ray_transmittance(boxes, start, end):
    delta = tuple(b - a for a, b in zip(start, end))
    length = math.sqrt(sum(v * v for v in delta))
    result = [1.0, 1.0, 1.0]
    for box in boxes:
        enter, leave = 0.0, 1.0
        for p, d, low, high in zip(start, delta, box.minimum, box.maximum):
            if abs(d) < 1e-12:
                if p < low or p >= high:
                    enter = leave
                    break
                continue
            a, b = (low - p) / d, (high - p) / d
            enter, leave = max(enter, min(a, b)), min(leave, max(a, b))
        if leave > enter:
            exponent = (leave - enter) * length / box.reference_thickness
            for channel, value in enumerate(box.transmittance):
                result[channel] *= value ** exponent
    return result


def verify(contracts):
    decoded = {name: decode((contracts / f"{name}.bin").read_bytes()) for name in CASES}
    require(all(u.scene == 1 and u.section == (0, 0, 0) for u in decoded.values()), "fixture coordinates")
    counts = (0, 1, 1, 2, 1, 1, 2, 1, 110, 4097, 4097)
    require(tuple(len(update.boxes) for update in decoded.values()) == counts, "unexpected fixture box counts")
    require(decoded["multipart"].parts == ((1, 4096), (1, 1)), "material table or part limit")
    extended = decoded["multipart"].boxes[-1]
    require(extended.minimum == (-0.25, 0, 0) and extended.maximum == (0, 1, 1.25)
            and extended.transmittance == (0, 1, 0.5) and extended.reference_thickness == 0.125,
            "out-of-cell box or valid zero transmittance was lost")
    blue = decoded["blue"].boxes[0]
    green = decoded["green"].boxes[0]
    pane = decoded["green-pane"].boxes[0]
    require(blue.minimum == green.minimum == (8, 4, 10) and blue.maximum == green.maximum == (9, 5, 11),
            "wrong block coordinates")
    require(pane.minimum == (8, 4, 10 + 7 / 16) and pane.maximum == (9, 5, 10 + 9 / 16)
            and pane.transmittance == green.transmittance, "connected pane thickness or material mismatch")
    require(decoded["green-rect-full"].boxes == decoded["green-rect-half"].boxes + (pane,),
            "full rectangular blocker must fill the adjoining pane volume")
    transaction = decoded["transaction"]
    require(transaction.parts == ((1, 4096), (1, 1)) and transaction.boxes[0] == green
            and all(all(a >= 1 and b <= 2 for a, b in zip(box.minimum, box.maximum))
                    for box in transaction.boxes[1:]), "transaction must place its only intersecting box in part zero")
    start, end = (8.5, 4.5, 8.25), (8.5, 4.5, 13.5)
    measured = {key: ray_transmittance(decoded[key].boxes, start, end)
                for key in ("blue", "green", "green-stacked", "green-pane", "clear")}
    expected = {"blue": blue.transmittance, "green": green.transmittance,
                "green-stacked": tuple(t ** 2 for t in green.transmittance),
                "green-pane": tuple(t ** 0.125 for t in green.transmittance), "clear": (1, 1, 1)}
    require(all(all(math.isclose(a, b) for a, b in zip(measured[key], value)) for key, value in expected.items()),
            "serialized lengths disagree with analytical light-through-glass reference")
    return {"box_counts": dict(zip(CASES, counts)), "transmittance": measured}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--contracts", type=Path, default=DEFAULT_CONTRACTS)
    options = parser.parse_args()
    print(json.dumps(verify(options.contracts), indent=2))
