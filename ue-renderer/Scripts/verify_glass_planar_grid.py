"""Compare planar-grid ray lengths with independent clipping of the original unit boxes."""

import math
import random


def ray(start, end):
    length = math.dist(start, end)
    return tuple((b - a) / length for a, b in zip(start, end)), length


def clip(start, direction, distance, minimum, maximum):
    near, far = 0.0, distance
    for origin, slope, low, high in zip(start, direction, minimum, maximum):
        if abs(slope) < 1e-8:
            if not low <= origin < high:
                return 0.0, 0.0
        else:
            a, b = (low - origin) / slope, (high - origin) / slope
            near, far = max(near, min(a, b)), min(far, max(a, b))
    return near, far


def reference_cut(start, end, axes, minimum, cells):
    """Intersect each cube separately and multiply the reference transmission per channel."""
    direction, distance = ray(start, end)
    result = [1.0, 1.0, 1.0]
    for (x, y), (transmittance, dref) in cells.items():
        low = list(minimum)
        low[axes[0]] += x
        low[axes[1]] += y
        near, far = clip(start, direction, distance, low, [value + 1 for value in low])
        if far > near:
            for channel in range(3):
                result[channel] *= transmittance[channel] ** ((far - near) / dref)
    return result


def planar_cut(start, end, axes, minimum, size, cells):
    """Mirror McGlassPlanarDepth's near-to-far grid walk and optical depth accumulation."""
    direction, distance = ray(start, end)
    maximum = [value + 1 for value in minimum]
    for i, axis in enumerate(axes):
        maximum[axis] = minimum[axis] + size[i]
    near, far = clip(start, direction, distance, minimum, maximum)
    if far <= near:
        return [1.0, 1.0, 1.0]

    coordinate = [min(max(start[axis] - minimum[axis] + direction[axis] * near, 0), size[i])
                  for i, axis in enumerate(axes)]
    slope = [direction[axis] for axis in axes]
    step = [-1 if value < 0 else 1 for value in slope]
    cell = [math.ceil(p) - 1 if s < 0 else math.floor(p) for p, s in zip(coordinate, step)]
    cell = [min(max(p, 0), size[i] - 1) for i, p in enumerate(cell)]
    delta = [1 / abs(value) if abs(value) >= 1e-8 else math.inf for value in slope]
    boundary = [max(0, (cell[i] + max(step[i], 0) - coordinate[i]) / slope[i])
                if abs(slope[i]) >= 1e-8 else math.inf for i in range(2)]
    depth = [0.0, 0.0, 0.0]
    travelled, limit = 0.0, far - near
    for _ in range(32):
        exit_distance = min(limit, *boundary)
        thickness = exit_distance - travelled
        material = cells.get(tuple(cell))
        if thickness > 0 and material is not None:
            transmittance, dref = material
            for channel in range(3):
                if transmittance[channel] == 0:
                    depth[channel] = math.inf
                else:
                    depth[channel] += thickness * -math.log2(transmittance[channel]) / dref
        if exit_distance >= limit:
            break
        for i in range(2):
            if boundary[i] <= exit_distance:
                cell[i] += step[i]
                boundary[i] += delta[i]
        if any(not 0 <= cell[i] < size[i] for i in range(2)):
            break
        travelled = exit_distance
    return [2 ** -value for value in depth]


def main():
    colors = ((0.17, 0.63, 0.91), 0.5), ((0.09, 0.72, 0.44), 1.7), ((0.0, 0.36, 0.75), 2.0)
    size = 16, 16
    patterned = {(x, y): colors[(x + 2 * y) % len(colors)] for y in range(size[1]) for x in range(size[0])
                 if (x + y) % 4 != 3}
    sparse = {(0, 0): colors[0], (2, 1): colors[2], (3, 2): colors[1]}
    nonzero_colors = {(x, y): colors[(x + y) % 2] for y in range(size[1]) for x in range(size[0])}
    minimum = -11.0, 7.0, 4.0
    rng = random.Random(517321)
    count = 0

    for normal in range(3):
        axes = (normal + 1) % 3, (normal + 2) % 3

        def point(n, u, v):
            p = list(minimum)
            p[normal] += n
            p[axes[0]] += u
            p[axes[1]] += v
            return p

        fixed = (
            ("normal", point(-2, 0.5, 0.5), point(3, 0.5, 0.5), sparse),
            ("different reference depths", point(0.5, -1, 0.5), point(0.5, 5.5, 3.4), nonzero_colors),
            ("oblique slab", point(-2, -8, 4.5), point(3, 23, 11.4), patterned),
            ("grid corner ties", point(0.5, -1, -1), point(0.5, 17, 17), nonzero_colors),
            ("maximum crossings", point(0.5, -1, -0.7), point(0.5, 17, 17.2), nonzero_colors),
            ("interior boundary parallel", point(0.5, 1, -1), point(0.5, 1, 17), nonzero_colors),
            ("slab boundary parallel", point(0, -1, 1), point(0, 17, 1), patterned),
            ("upper slab boundary excluded", point(1, -1, 1), point(1, 17, 1), nonzero_colors),
            ("hole", point(-1, 1.5, 1.5), point(2, 1.5, 1.5), sparse),
            ("zero transmission", point(-1, 2.5, 1.5), point(2, 2.5, 1.5), sparse),
            ("ends inside a cell", point(0.1, 0.1, 0.1), point(0.3, 0.3, 0.3), sparse),
            ("ends at slab surface", point(-2, 0.5, 0.5), point(0, 0.5, 0.5), sparse),
            ("starts at grid boundary", point(0.5, 4, 4), point(0.5, 1.5, 1.5), nonzero_colors),
        )

        def verify(label, start, end, cells):
            nonlocal count
            expected = reference_cut(start, end, axes, minimum, cells)
            result = planar_cut(start, end, axes, minimum, size, cells)
            assert all(math.isclose(a, b, rel_tol=1e-10, abs_tol=1e-12) for a, b in zip(expected, result)), (
                label, normal, start, end, expected, result)
            count += 1

        for label, start, end, cells in fixed:
            verify(label, start, end, cells)
            verify(label + " reversed", end, start, cells)
        for _ in range(256):
            verify("seeded", point(rng.uniform(-2, 3), rng.uniform(-4, 20), rng.uniform(-4, 20)),
                   point(rng.uniform(-2, 3), rng.uniform(-4, 20), rng.uniform(-4, 20)), patterned)

    print(f"Planar volumetric filtering matched the independent box reference for {count} CPU rays.")


if __name__ == "__main__":
    main()
