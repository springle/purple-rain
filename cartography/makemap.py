"""Rasterize the NY-harbor counties into a 200x168 1-bit land mask at watch scale.

Census cb_2023 500k boundaries are shoreline-clipped, so the union of the
county polygons IS the iconic coastline. Supersample 4x, majority-vote down.
Outputs: preview PNG (3x, palette colors) + packed hex masks for the artifact.
"""

import math

import shapefile
from PIL import Image, ImageDraw

# frame: ~140 m/px, north-up. Manhattan bleeds off the top on purpose.
LON0, LON1 = -74.115, -73.783
LAT0, LAT1 = 40.645, 40.855
W, H, SS = 200, 168, 4

WANT = {
    "34017",  # Hudson NJ (JC, Hoboken)
    "34003",  # Bergen NJ (Palisades, Fort Lee)
    "34013",  # Essex NJ (Newark edge)
    "34039",  # Union NJ (bottom-left sliver)
    "36061",  # Manhattan
    "36047",  # Brooklyn
    "36081",  # Queens
    "36005",  # Bronx
    "36085",  # Staten Island tip
}


def xy(lon, lat, s=SS):
    return ((lon - LON0) / (LON1 - LON0) * W * s, (LAT1 - lat) / (LAT1 - LAT0) * H * s)


img = Image.new("1", (W * SS, H * SS), 0)
d = ImageDraw.Draw(img)
sf = shapefile.Reader("cb_2023_us_county_500k")
fields = [f[0] for f in sf.fields[1:]]
gi = fields.index("GEOID")
for sr in sf.iterShapeRecords():
    if sr.record[gi] not in WANT:
        continue
    shp = sr.shape
    parts = list(shp.parts) + [len(shp.points)]
    for i in range(len(parts) - 1):
        ring = [xy(x, y) for x, y in shp.points[parts[i] : parts[i + 1]]]
        if len(ring) >= 3:
            d.polygon(ring, fill=1)

# county legal boundaries run mid-river: erase TIGER area-water to get the
# true shoreline (skip ponds < 0.1 km^2 so parks don't get moth-eaten)
for fips in sorted(WANT):
    aw = shapefile.Reader(f"tl_2023_{fips}_areawater")
    awf = [f[0] for f in aw.fields[1:]]
    ai = awf.index("AWATER")
    for sr in aw.iterShapeRecords():
        if sr.record[ai] < 150000:
            continue
        shp = sr.shape
        parts = list(shp.parts) + [len(shp.points)]
        for i in range(len(parts) - 1):
            ring = [xy(x, y) for x, y in shp.points[parts[i] : parts[i + 1]]]
            if len(ring) >= 3:
                d.polygon(ring, fill=0)
                # dilate ~1 final px: closes hairline seams where NY and NJ
                # water polygons meet mid-river (else a ghost strip of "land")
                d.line(ring + [ring[0]], fill=0, width=4)

px = img.load()
land = [[0] * W for _ in range(H)]
for y in range(H):
    for x in range(W):
        s = sum(px[x * SS + i, y * SS + j] for i in range(SS) for j in range(SS))
        land[y][x] = 1 if s >= SS * SS // 2 else 0

# hand-add islands the 500k generalization drops
def blob(lon, lat, w=2, h=2):
    cx, cy = xy(lon, lat, 1)
    for j in range(int(cy), int(cy) + h):
        for i in range(int(cx), int(cx) + w):
            if 0 <= i < W and 0 <= j < H:
                land[j][i] = 1

blob(-74.0446, 40.6892, 2, 2)  # Liberty
blob(-74.0396, 40.6995, 2, 2)  # Ellis
blob(-74.0180, 40.6900, 3, 3)  # Governors
for t in range(11):             # Roosevelt Island sliver
    lat = 40.7475 + t * (40.7760 - 40.7475) / 10
    lon = -73.9545 + t * (-73.9420 + 73.9545) / 10
    cx, cy = xy(lon, lat, 1)
    if 0 <= int(cx) < W and 0 <= int(cy) < H:
        land[int(cy)][int(cx)] = 1

# prune land speckle in the water: keep big landmasses and real islands only
# (Liberty, Ellis, Governors, Roosevelt) — pier fragments and slivers go
KEEP_BOXES = [(40, 126, 47, 135), (42, 121, 49, 128), (55, 128, 64, 137), (94, 58, 104, 92)]
seen = [[False] * W for _ in range(H)]
for y0 in range(H):
    for x0 in range(W):
        if not land[y0][x0] or seen[y0][x0]:
            continue
        stack, comp = [(x0, y0)], []
        seen[y0][x0] = True
        while stack:
            x, y = stack.pop()
            comp.append((x, y))
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx, ny = x + dx, y + dy
                if 0 <= nx < W and 0 <= ny < H and land[ny][nx] and not seen[ny][nx]:
                    seen[ny][nx] = True
                    stack.append((nx, ny))
        keep = len(comp) >= 40 or any(
            bx0 <= cx <= bx1 and by0 <= cy <= by1
            for cx, cy in comp
            for bx0, by0, bx1, by1 in KEEP_BOXES
        )
        if not keep:
            for cx, cy in comp:
                land[cy][cx] = 0

coast = [[0] * W for _ in range(H)]
for y in range(H):
    for x in range(W):
        if not land[y][x]:
            continue
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nx, ny = x + dx, y + dy
            if 0 <= nx < W and 0 <= ny < H and not land[ny][nx]:
                coast[y][x] = 1
                break

# ---- preview PNG ----
SC = 3
pv = Image.new("RGB", (W * SC, H * SC), (0, 0, 0))
pd = ImageDraw.Draw(pv)
for y in range(H):
    for x in range(W):
        c = None
        if coast[y][x]:
            c = (170, 170, 170)
        elif land[y][x]:
            c = (60, 60, 60) if (x + y) % 2 == 0 else (20, 20, 20)
        if c:
            pd.rectangle([x * SC, y * SC, x * SC + SC - 1, y * SC + SC - 1], fill=c)

# ---- packed hex masks for the artifact ----
def pack(mask):
    out = []
    for y in range(H):
        bits = 0
        for x in range(W):
            bits = (bits << 1) | mask[y][x]
        out.append(format(bits, "050x"))
    return "".join(out)

with open("mask.js", "w") as f:
    f.write("const MAP_W=200, MAP_H=168;\n")
    f.write("const LAND_HEX='%s';\n" % pack(land))
    f.write("const COAST_HEX='%s';\n" % pack(coast))
land_n = sum(map(sum, land))
print("land px:", land_n, " coast px:", sum(map(sum, coast)))
