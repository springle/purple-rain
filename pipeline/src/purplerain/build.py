"""Purple Rain payload builder — one source per element, no fallbacks.

Solid = MRMS PrecipRate verbatim. Dither = HRRR sub-hourly max over the next
two hours. Vectors = ProbSevere motion on in-frame cells. Rail = KNYC station
obs + Open-Meteo UV/AQI. A source that fails simply goes dark and the health
byte says so; nothing substitutes.
"""

from __future__ import annotations

import gzip
import json
import math
import re
import sys
import tempfile
from datetime import datetime, timedelta, timezone

import requests

# map frame (must match watchapp masks.h / cartography/makemap.py)
LON0, LON1 = -74.115, -73.783
LAT0, LAT1 = 40.645, 40.855
GRID_W, GRID_H = 25, 21
NCELLS = GRID_W * GRID_H

# severity tiers, mm/h (0.05 / 1 / 2 inch per hour)
T_RAIN, T_HEAVY, T_SEVERE = 1.27, 25.4, 50.8


def level(mmh: float) -> int:
    """16-level log intensity for the wire: level = 2*log2(mmh+1), so the
    watch can interpolate smooth gradients instead of 4 flat tiers."""
    if mmh <= 0:
        return 0
    return max(0, min(15, round(2 * math.log2(mmh + 1))))

UA = {"User-Agent": "purple-rain/1.0 (pringle@hey.com)"}
MAX_VECS = 6


def cell_lonlat(i: int, j: int) -> tuple[float, float]:
    x, y = i * 8 + 4, j * 8 + 4
    lon = LON0 + (x / 200.0) * (LON1 - LON0)
    lat = LAT1 - (y / 168.0) * (LAT1 - LAT0)
    return lon, lat


def tier(mmh: float) -> int:
    if mmh >= T_SEVERE:
        return 3
    if mmh >= T_HEAVY:
        return 2
    if mmh >= T_RAIN:
        return 1
    return 0


# ---------- MRMS: the solid pixels ----------

def _get_retry(url: str, tries: int = 3, magic: bytes | None = None) -> bytes:
    """The MRMS server intermittently answers with an HTML error page; retry
    within the run (same source, same run — not a fallback) and verify the
    payload magic before trusting it."""
    import time

    last: Exception | None = None
    for i in range(tries):
        try:
            r = requests.get(url, headers=UA, timeout=60)
            r.raise_for_status()
            if magic and not r.content.startswith(magic):
                raise ValueError(f"bad magic, got {r.content[:8]!r}")
            return r.content
        except Exception as e:
            last = e
            time.sleep(4 * (i + 1))
    raise RuntimeError(f"{url} failed after {tries} tries: {last}")


def fetch_mrms() -> tuple[list[int] | None, float]:
    """Sample MRMS PrecipRate at every cell. Returns (tiers, age_minutes)."""
    import eccodes as ec

    base = "https://mrms.ncep.noaa.gov/2D/PrecipRate/"
    idx = _get_retry(base).decode(errors="replace")
    stamps = re.findall(r"MRMS_PrecipRate_00\.00_(\d{8}-\d{6})\.grib2\.gz", idx)
    if not stamps:
        return None, 1e9
    # the index sometimes advertises the newest frame before it's actually
    # downloadable — step back one 2-min frame rather than fail the run
    raw = None
    for ts in sorted(set(stamps), reverse=True)[:3]:
        try:
            raw = _get_retry(f"{base}MRMS_PrecipRate_00.00_{ts}.grib2.gz",
                             tries=2, magic=b"\x1f\x8b")
            break
        except Exception:
            continue
    if raw is None:
        return None, 1e9
    valid = datetime.strptime(ts, "%Y%m%d-%H%M%S").replace(tzinfo=timezone.utc)
    age_min = (datetime.now(timezone.utc) - valid).total_seconds() / 60
    with tempfile.NamedTemporaryFile(suffix=".grib2") as tf:
        tf.write(gzip.decompress(raw))
        tf.flush()
        with open(tf.name, "rb") as f:
            gid = ec.codes_grib_new_from_file(f)
            try:
                ni = ec.codes_get(gid, "Ni")
                lat1 = ec.codes_get(gid, "latitudeOfFirstGridPointInDegrees")
                lon1 = ec.codes_get(gid, "longitudeOfFirstGridPointInDegrees")
                di = ec.codes_get(gid, "iDirectionIncrementInDegrees")
                dj = ec.codes_get(gid, "jDirectionIncrementInDegrees")
                out = []
                for j in range(GRID_H):
                    for i in range(GRID_W):
                        lon, lat = cell_lonlat(i, j)
                        row = round((lat1 - lat) / dj)
                        col = round(((lon % 360) - lon1) / di)
                        v = ec.codes_get_double_element(gid, "values", row * ni + col)
                        out.append(level(v if v > 0 else 0.0))
            finally:
                ec.codes_release(gid)
    return out, age_min


# ---------- HRRR sub-hourly: the dithered pixels ----------

def fetch_hrrr() -> list[int] | None:
    """Max PRATE over f01+f02 15-min steps (~next 2 h), tiered per cell."""
    import eccodes as ec

    now = datetime.now(timezone.utc)
    for back in (1, 2, 3):
        cyc = (now - timedelta(hours=back)).replace(minute=0, second=0, microsecond=0)
        peak = [0.0] * NCELLS
        got = 0
        for fxx in (1, 2):
            url = (
                "https://nomads.ncep.noaa.gov/cgi-bin/filter_hrrr_sub.pl"
                f"?file=hrrr.t{cyc.hour:02d}z.wrfsubhf{fxx:02d}.grib2"
                "&var_PRATE=on&subregion="
                "&leftlon=-74.25&rightlon=-73.65&toplat=40.95&bottomlat=40.55"
                f"&dir=%2Fhrrr.{cyc:%Y%m%d}%2Fconus"
            )
            r = requests.get(url, timeout=45)
            if r.status_code != 200 or len(r.content) < 500:
                break
            with tempfile.NamedTemporaryFile(suffix=".grib2") as tf:
                tf.write(r.content)
                tf.flush()
                with open(tf.name, "rb") as f:
                    while True:
                        gid = ec.codes_grib_new_from_file(f)
                        if gid is None:
                            break
                        try:
                            for j in range(GRID_H):
                                for i in range(GRID_W):
                                    lon, lat = cell_lonlat(i, j)
                                    # HRRR is 3 km on a Lambert grid; interpolate
                                    # the 4 nearest points (inverse-distance) so
                                    # the dither renders the model field itself,
                                    # not projection-resampling staircase
                                    pts = ec.codes_grib_find_nearest(gid, lat, lon, False, 4)
                                    wsum = vsum = 0.0
                                    for n in pts:
                                        w = 1.0 / max(n.distance, 0.01)
                                        wsum += w
                                        vsum += w * max(0.0, n.value)
                                    mmh = (vsum / wsum) * 3600.0
                                    k = j * GRID_W + i
                                    peak[k] = max(peak[k], mmh)
                        finally:
                            ec.codes_release(gid)
            got += 1
        if got == 2:
            return [level(v) for v in peak]
    return None


# ---------- ProbSevere: the vectors ----------

def fetch_vectors() -> list[list[int]]:
    base = "https://mrms.ncep.noaa.gov/ProbSevere/PROBSEVERE/"
    idx = _get_retry(base).decode(errors="replace")
    names = re.findall(r"MRMS_PROBSEVERE_\d{8}_\d{6}\.json", idx)
    if not names:
        return []
    data = json.loads(_get_retry(base + max(names)))
    vecs = []
    for feat in data.get("features", []):
        try:
            ring = feat["geometry"]["coordinates"][0]
            if ring and isinstance(ring[0][0], list):  # multipolygon nesting
                ring = ring[0]
            lon = sum(p[0] for p in ring) / len(ring)
            lat = sum(p[1] for p in ring) / len(ring)
        except (KeyError, IndexError, TypeError, ZeroDivisionError):
            continue
        if not (LON0 <= lon <= LON1 and LAT0 <= lat <= LAT1):
            continue
        props = feat.get("properties", {})
        east = float(props.get("MOTION_EAST", 0))
        south = float(props.get("MOTION_SOUTH", 0))
        prob = float(props.get("PS", props.get("PROB", 0)))
        x = round((lon - LON0) / (LON1 - LON0) * 200)
        y = round((LAT1 - lat) / (LAT1 - LAT0) * 168)
        dx = max(-20, min(20, round(east)))
        dy = max(-20, min(20, round(south)))
        vecs.append((prob, [x, y, dx, dy]))
    vecs.sort(key=lambda v: -v[0])
    return [v[1] for v in vecs[:MAX_VECS]]


# ---------- rail: KNYC obs + Open-Meteo UV/AQI ----------

def c_to_f(c: float) -> int:
    return round(c * 9 / 5 + 32)


def compass(deg: float) -> str:
    pts = ["N", "NE", "E", "SE", "S", "SW", "W", "NW"]
    return pts[round(deg / 45) % 8]


def _station(sid: str) -> dict:
    r = requests.get(
        f"https://api.weather.gov/stations/{sid}/observations/latest",
        headers=UA, timeout=30,
    )
    p = r.json()["properties"]
    return {k: p.get(k, {}).get("value") for k in
            ("temperature", "dewpoint", "windDirection", "windSpeed", "windGust")}


def fetch_obs() -> dict:
    """Temp/dew: KNYC (the official NYC thermometer). Wind/gust: KLGA — the
    Central Park anemometer sits under the tree canopy and reads ~0 while
    every airport around it reads real wind."""
    out = {}
    nyc = _station("KNYC")
    if nyc["temperature"] is not None:
        out["temp"] = c_to_f(nyc["temperature"])
    if nyc["dewpoint"] is not None:
        out["dew"] = c_to_f(nyc["dewpoint"])

    lga = _station("KLGA")
    wind = ""
    if lga["windSpeed"] is not None:
        mph = round(lga["windSpeed"] * 0.621)
        if mph == 0:
            wind = "CALM"
        elif lga["windDirection"] is not None:
            wind = f"{compass(lga['windDirection'])}{mph}"
        else:
            wind = f"{mph}"
        if lga["windGust"]:
            wind += f" G{round(lga['windGust'] * 0.621)}"
    out["wind"] = wind
    return out


def fetch_uv_aqi() -> dict:
    out = {}
    lat, lon = 40.72, -73.99
    try:
        r = requests.get(
            "https://api.open-meteo.com/v1/forecast",
            params={"latitude": lat, "longitude": lon, "hourly": "uv_index",
                    "forecast_days": 1, "timezone": "America/New_York"},
            timeout=30,
        ).json()
        hour = datetime.now().hour
        out["uv"] = round(r["hourly"]["uv_index"][hour])
    except Exception:
        pass
    try:
        r = requests.get(
            "https://air-quality-api.open-meteo.com/v1/air-quality",
            params={"latitude": lat, "longitude": lon, "hourly": "us_aqi",
                    "forecast_days": 1, "timezone": "America/New_York"},
            timeout=30,
        ).json()
        hour = datetime.now().hour
        out["aqi"] = round(r["hourly"]["us_aqi"][hour])
    except Exception:
        pass
    return out


# ---------- assembly ----------

def main() -> int:
    out_path = sys.argv[1] if len(sys.argv) > 1 else "/dev/stdout"

    try:
        now_tiers, mrms_age = fetch_mrms()
    except Exception as e:
        print(f"mrms failed: {e}", file=sys.stderr)
        now_tiers, mrms_age = None, 1e9
    try:
        fut_tiers = fetch_hrrr()
    except Exception as e:
        print(f"hrrr failed: {e}", file=sys.stderr)
        fut_tiers = None
    try:
        vecs = fetch_vectors()
    except Exception as e:
        print(f"probsevere failed: {e}", file=sys.stderr)
        vecs = []
    rail = {}
    try:
        rail.update(fetch_obs())
    except Exception as e:
        print(f"obs failed: {e}", file=sys.stderr)
    rail.update(fetch_uv_aqi())

    mrms_ok = now_tiers is not None and mrms_age < 45
    health = 0 if (mrms_ok and mrms_age < 10 and fut_tiers is not None) else (1 if mrms_ok else 2)

    now_t = now_tiers or [0] * NCELLS
    fut_t = fut_tiers or [0] * NCELLS
    cells = "".join(f"{(n | (f << 4)):02x}" for n, f in zip(now_t, fut_t))

    payload = {
        "v": 2,
        "gen": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "cells": cells,
        "vecs": vecs,
        "wind": rail.get("wind", ""),
        "health": health,
    }
    for k in ("temp", "dew", "uv", "aqi"):
        if k in rail:
            payload[k] = rail[k]

    with open(out_path, "w") as f:
        json.dump(payload, f, separators=(",", ":"))
    wet = sum(1 for c in now_t if c >= 2) + sum(1 for c in fut_t if c >= 2)
    print(
        f"built: health={health} mrms_age={mrms_age:.0f}m wet_cells={wet} "
        f"vecs={len(vecs)} rail={ {k: v for k, v in rail.items()} }",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
