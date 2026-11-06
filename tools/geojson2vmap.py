#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
geojson2vmap.py - wandelt GeoJSON in das kompakte .vmap-Format fuer den
M5Dial "Moving Map" Sketch um (nur Standardbibliothek + optional Pillow
fuer Vorschau/Simulation).

Unterbefehle
------------
  convert   GeoJSON-Layer -> .vmap  (Clip auf Bounding-Box, Vereinfachung)
  info      Kopfdaten / Statistik einer .vmap anzeigen
  preview   .vmap als PNG-Uebersicht rendern (Pillow)
  sim       Bildschirmausschnitt des M5Dial nachrechnen (gleiche Mathematik
            wie im Sketch) -> prueft Projektion und Dateiformat

Layer-Angabe fuer "convert":   -l TYP:DATEI[:OPTION[:OPTION...]]
  TYP     Zahl, siehe Tabelle unten
  OPTION  key=wert|wert   Property ist eines der Werte
          key!=wert       Property ist keiner der Werte
          key>=zahl  key<=zahl  key>zahl  key<zahl
          tol=METER       Vereinfachungstoleranz fuer diesen Layer
          name=FELD       Property, aus dem der Ortsname gelesen wird (Orte)
  (Pfade bitte ohne ':' angeben, unter Windows mit '/' statt '\\')

Typen (gleiche Nummern wie im Sketch)
  0 Landesgrenze/Hauptumriss   1 Regionen/Bundeslaender   2 Nachbarlaender
  3 Fluesse/Gewaesser          4 Autobahn                 5 Hauptstrasse
  6 Bahn                       7 Kueste
  10 Grossstadt  11 Stadt  12 Ort            (Punkte mit Namen)
  13 Verkehrsflughafen  14 Flugplatz/Landeplatz   (Punkte, Name = ICAO)
  20 CTR  21 TMA/CTA (Klasse A-D)  22 RMZ/TMZ  23 Beschraenkung (R)
  24 Gefahrengebiet (Q/D)  25 Verbotszone (P)  26 TRA/Segelflug/Wellenfenster
  27 Luftraum E (OpenAir 'AC E')
     (Luftraeume = geschlossene Polygone, Name = "LABEL|UNTERGRENZE|OBERGRENZE")

Luftraeume aus OpenAir (z.B. OpenFlightMaps, openair_ed.zip -> *.seeyou.openair.txt):
  -l oa:ed.seeyou.openair.txt:tol=150          (optional :skip=W|G  Klassen weglassen)

Flugplaetze: als Quelle geht auch eine OurAirports-CSV (airports.csv,
https://ourairports.com/data/), z.B.
  -l 13:airports.csv:iso_country=DE:type=large_airport|medium_airport:name=ident
  -l 14:airports.csv:iso_country=DE:type=small_airport:name=ident

Dateiformat .vmap v1 (little-endian)
  Header 32 Byte: "VMAP", u8 version(=1), u8 flags, u16 reserviert,
                  u32 Anzahl Records, i32 minLat, minLon, maxLat, maxLon
                  (Einheit 1e-5 Grad), u32 Erstelldatum JJJJMMTT (0 = unbekannt)
  Record: u8 typ, u8 namelen, u16 npts, i32 minLat,minLon,maxLat,maxLon,
          danach <namelen> Bytes ASCII-Name, danach npts * (i32 lat, i32 lon)
  Die Records werden in Dateireihenfolge gezeichnet (unten -> oben).
"""
import argparse
import json
import math
import os
import re
import struct
import sys
import unicodedata
import datetime

MAGIC = b"VMAP"
VERSION = 1
HDR = struct.Struct("<4sBBHI4iI")    # 32 Byte, letztes Feld: Erstelldatum JJJJMMTT (0 = unbekannt)
REC = struct.Struct("<BBH4i")        # 20 Byte
PT = struct.Struct("<ii")
assert HDR.size == 32 and REC.size == 20

(T_BORDER, T_REGION, T_NEIGHBOR, T_WATER, T_MOTORWAY, T_ROAD, T_RAIL,
 T_COAST) = range(8)
T_PLACE_L, T_PLACE_M, T_PLACE_S = 10, 11, 12
T_AIRPORT_L, T_AIRPORT_S = 13, 14
(T_A_CTR, T_A_TMA, T_A_RMZ, T_A_R, T_A_Q, T_A_P, T_A_TRA, T_A_E) = range(20, 28)
DRAW_ORDER = [T_NEIGHBOR, T_COAST, T_REGION, T_WATER, T_RAIL, T_ROAD,
              T_MOTORWAY, T_A_E, T_A_TRA, T_A_TMA, T_A_RMZ, T_A_CTR, T_A_Q, T_A_R, T_A_P,
              T_BORDER, T_PLACE_S, T_PLACE_M, T_PLACE_L,
              T_AIRPORT_S, T_AIRPORT_L]

# Farben wie im Sketch (RGB)
BG = (8, 12, 20)
COLORS = {
    T_BORDER: (235, 235, 235), T_REGION: (95, 105, 120),
    T_NEIGHBOR: (62, 72, 88), T_WATER: (45, 115, 225),
    T_MOTORWAY: (240, 140, 30), T_ROAD: (170, 145, 60),
    T_RAIL: (110, 110, 110), T_COAST: (70, 130, 180),
    T_PLACE_L: (255, 255, 255), T_PLACE_M: (230, 230, 200),
    T_PLACE_S: (190, 190, 160),
    T_AIRPORT_L: (230, 90, 230), T_AIRPORT_S: (230, 90, 230),
    T_A_CTR: (60, 140, 255), T_A_TMA: (70, 170, 200), T_A_RMZ: (80, 190, 110),
    T_A_R: (235, 70, 70), T_A_Q: (250, 200, 60), T_A_P: (255, 40, 40),
    T_A_TRA: (120, 150, 120), T_A_E: (160, 120, 255),
}

EXONYMS = {
    "Munich": "Muenchen", "Cologne": "Koeln", "Nuremberg": "Nuernberg",
    "Hanover": "Hannover", "Brunswick": "Braunschweig", "Prague": "Prag",
    "Vienna": "Wien", "Copenhagen": "Kopenhagen", "Warsaw": "Warschau",
    "Zurich": "Zuerich", "Geneva": "Genf", "Luxembourg": "Luxemburg",
    "Brussels": "Bruessel", "Strasbourg": "Strassburg",
    "Prague ": "Prag",
}


def ascii_de(s):
    s = EXONYMS.get(s, s)
    for a, b in (("ä", "ae"), ("ö", "oe"), ("ü", "ue"), ("Ä", "Ae"),
                 ("Ö", "Oe"), ("Ü", "Ue"), ("ß", "ss"), ("ø", "oe"),
                 ("Ø", "Oe"), ("å", "aa"), ("Å", "Aa"), ("æ", "ae"),
                 ("ł", "l"), ("Ł", "L"), ("đ", "d")):
        s = s.replace(a, b)
    s = unicodedata.normalize("NFKD", s)
    s = "".join(ch for ch in s if ord(ch) < 128)
    return s.strip()[:31]


# ---------------------------------------------------------------- Geometrie
def iter_lines(geom):
    t, c = geom["type"], geom["coordinates"]
    if t == "LineString":
        yield c
    elif t == "MultiLineString":
        yield from c
    elif t == "Polygon":
        yield from c
    elif t == "MultiPolygon":
        for poly in c:
            yield from poly


def iter_points(geom):
    t, c = geom["type"], geom["coordinates"]
    if t == "Point":
        yield c
    elif t == "MultiPoint":
        yield from c


def clip_runs(pts, bb):
    """Teilt eine Linie in Stuecke, die die Bounding-Box beruehren."""
    if bb is None:
        return [pts]
    x0, y0, x1, y1 = bb
    inside = [x0 <= p[0] <= x1 and y0 <= p[1] <= y1 for p in pts]
    n = len(pts)
    runs, cur = [], []
    for i, p in enumerate(pts):
        keep = inside[i] or (i > 0 and inside[i - 1]) or \
            (i < n - 1 and inside[i + 1])
        if keep:
            cur.append(p)
        elif cur:
            runs.append(cur)
            cur = []
    if cur:
        runs.append(cur)
    return [r for r in runs if len(r) >= 2]


def rdp(pts, tol):
    """Douglas-Peucker (iterativ). pts: Liste (x_m, y_m)."""
    n = len(pts)
    if n < 3:
        return list(range(n))
    keep = [False] * n
    keep[0] = keep[-1] = True
    stack = [(0, n - 1)]
    while stack:
        a, b = stack.pop()
        ax, ay = pts[a]
        dx, dy = pts[b][0] - ax, pts[b][1] - ay
        l2 = dx * dx + dy * dy
        dmax, idx = -1.0, -1
        for i in range(a + 1, b):
            px, py = pts[i]
            if l2 == 0:
                d = math.hypot(px - ax, py - ay)
            else:
                t = ((px - ax) * dx + (py - ay) * dy) / l2
                t = 0.0 if t < 0 else 1.0 if t > 1 else t
                d = math.hypot(px - (ax + t * dx), py - (ay + t * dy))
            if d > dmax:
                dmax, idx = d, i
        if dmax > tol:
            keep[idx] = True
            stack.append((a, idx))
            stack.append((idx, b))
    return [i for i, k in enumerate(keep) if k]


def simplify(pts, tol_m, lat_ref):
    if tol_m <= 0 or len(pts) < 3:
        return pts
    cx = math.cos(math.radians(lat_ref)) * 111320.0
    proj = [(p[0] * cx, p[1] * 110574.0) for p in pts]
    return [pts[i] for i in rdp(proj, tol_m)]


def ring_simplify(pts, tol_m, lat_ref):
    """Vereinfacht einen geschlossenen Ring (Anfang bleibt Ende)."""
    if pts[0] == pts[-1]:
        pts = pts[:-1]
    if tol_m <= 0 or len(pts) < 6:
        return pts + [pts[0]]
    cx = math.cos(math.radians(lat_ref)) * 111320.0
    x0, y0 = pts[0]
    far = max(range(len(pts)), key=lambda i: ((pts[i][0] - x0) * cx) ** 2 +
                                              ((pts[i][1] - y0) * 110574.0) ** 2)
    a = simplify(pts[:far + 1], tol_m, lat_ref)
    b = simplify(pts[far:] + [pts[0]], tol_m, lat_ref)
    return a[:-1] + b


# ---------------------------------------------------------------- OpenAir
def _oa_coord(tok, hemi):
    f = [float(x) for x in tok.split(":")]
    v = f[0] + (f[1] / 60 if len(f) > 1 else 0) + (f[2] / 3600 if len(f) > 2 else 0)
    return -v if hemi.upper() in ("S", "W") else v


def _oa_latlon(txt):
    t = txt.replace(",", " ").split()
    return _oa_coord(t[2], t[3]), _oa_coord(t[0], t[1])       # (lon, lat)


def oa_limit(txt):
    u = txt.strip().upper().replace(" ", "")
    if u in ("GND", "SFC", "GROUND", "SURFACE"):
        return "GND"
    if u in ("UNL", "UNLTD", "UNLIMITED"):
        return "UNL"
    m = re.match(r"FL(\d+)$", u)
    if m:
        return "FL" + m.group(1)
    m = re.match(r"(\d+)(?:FT)?(MSL|AMSL)$", u)
    if m:
        return str(int(m.group(1)))
    m = re.match(r"(\d+)(?:FT)?(AGL|GND|AGND)$", u)
    if m:
        n = int(m.group(1))
        return "GND" if n == 0 else "%dAGL" % n
    return re.sub(r"[^A-Z0-9]", "", u)[:8] or "?"


def oa_type(ac, ay):
    ay = (ay or "").upper()
    ac = ac.upper()
    if ay == "CTR":
        return T_A_CTR
    if ay in ("RMZ", "TMZ") or ac == "TMZ":
        return T_A_RMZ
    return {"R": T_A_R, "Q": T_A_Q, "P": T_A_P, "W": T_A_TRA,
            "G": T_A_TRA, "E": T_A_E}.get(ac, T_A_TMA)


def load_openair(fn, skip=()):
    """OpenAir -> [(typ, "LABEL|von|bis", [(lon, lat), ...])]"""
    out, cur, warn = [], None, 0
    center, circle_r = None, None

    def flush():
        nonlocal cur
        if cur and len(cur["pts"]) >= 3 and cur["ac"] not in skip:
            lab = re.sub(r"\b\d{3}\.\d{2,3}\b", "", cur.get("an", ""))
            lab = ascii_de(" ".join(lab.replace("|", " ").split()))[:24].strip()
            nm = "%s|%s|%s" % (lab or "?", oa_limit(cur.get("al", "GND")),
                              oa_limit(cur.get("ah", "UNL")))
            out.append((oa_type(cur["ac"], cur.get("ay")), nm, cur["pts"]))
        cur = None

    with open(fn, encoding="utf-8", errors="replace") as f:
        for raw in f:
            l = raw.strip()
            if not l or l[0] == "*":
                continue
            k, _, v = l.partition(" ")
            k = k.upper()
            if k == "AC":
                flush()
                cur = {"ac": v.strip(), "pts": []}
                center = circle_r = None
            elif cur is None:
                continue
            elif k in ("AN", "AY", "AH", "AL"):
                cur[k.lower()] = v.strip()
            elif k == "DP":
                cur["pts"].append(_oa_latlon(v))
            elif k == "V" and v.strip().upper().startswith("X="):
                center = _oa_latlon(v.split("=", 1)[1])
            elif k == "DC" and center:
                r_nm = float(v)
                for a in range(0, 360, 5):
                    ang = math.radians(a)
                    cur["pts"].append((center[0] + r_nm * 1852 * math.sin(ang) /
                                       (111320.0 * math.cos(math.radians(center[1]))),
                                       center[1] + r_nm * 1852 * math.cos(ang) / 110574.0))
            elif k in ("DA", "DB"):
                warn += 1
    flush()
    if warn:
        print("  Warnung: %d Bogen-Befehle (DA/DB) werden nicht unterstuetzt" % warn)
    return out


# ---------------------------------------------------------------- Filter
def parse_filter(s):
    for op in (">=", "<=", "!=", "=", ">", "<"):
        i = s.find(op)
        if i > 0:
            return s[:i], op, s[i + len(op):]
    raise SystemExit("Ungueltiger Filter: %r" % s)


def match(props, flt):
    for key, op, val in flt:
        v = props.get(key)
        if op in ("=", "!="):
            hit = str(v) in val.split("|")
            if hit != (op == "="):
                return False
        else:
            try:
                x, y = float(v), float(val)
            except (TypeError, ValueError):
                return False
            if not {">=": x >= y, "<=": x <= y, ">": x > y, "<": x < y}[op]:
                return False
    return True


def load_features(fn):
    """GeoJSON oder OurAirports-CSV -> Liste von GeoJSON-Features."""
    with open(fn, encoding="utf-8-sig", newline="") as f:
        if fn.lower().endswith(".csv"):
            import csv
            feats = []
            for row in csv.DictReader(f):
                try:
                    lat = float(row["latitude_deg"])
                    lon = float(row["longitude_deg"])
                except (KeyError, ValueError):
                    continue
                feats.append({"properties": row, "geometry":
                              {"type": "Point", "coordinates": [lon, lat]}})
            return feats
        return json.load(f)["features"]


# ---------------------------------------------------------------- convert
def cmd_convert(args):
    bb = tuple(args.bbox) if args.bbox else None
    lat_ref = (bb[1] + bb[3]) / 2 if bb else 51.0
    records = []
    for spec in args.layer:
        parts = spec.split(":")
        if len(parts) > 2 and len(parts[1]) == 1 and parts[2][:1] in "\\/":
            parts[1:3] = [parts[1] + ":" + parts[2]]       # Windows-Laufwerk
        typ, fn = (parts[0] if parts[0] == "oa" else int(parts[0])), parts[1]
        tol, namefield, flt, skip = args.tol, None, [], set()
        for o in parts[2:]:
            if o.startswith("skip="):
                skip = set(o[5:].split("|"))
            elif o.startswith("tol="):
                tol = float(o[4:])
            elif o.startswith("name="):
                namefield = o[5:]
            else:
                flt.append(parse_filter(o))
        if typ == "oa":
            n_in = n_out = n_rec = 0
            for t, nm, pts in load_openair(fn, skip):
                xs = [p[0] for p in pts]
                ys = [p[1] for p in pts]
                if bb and (max(xs) < bb[0] or min(xs) > bb[2] or
                           max(ys) < bb[1] or min(ys) > bb[3]):
                    continue
                n_in += len(pts)
                q = []
                for x, y in ring_simplify(pts, tol, lat_ref):
                    p = (int(round(y * 1e5)), int(round(x * 1e5)))
                    if not q or q[-1] != p:
                        q.append(p)
                if q[0] != q[-1]:
                    q.append(q[0])
                if len(q) < 4:
                    continue
                records.append((t, nm, q))
                n_out += len(q)
                n_rec += 1
            print("  OpenAir %-28s %5d Luftraeume, Punkte %6d -> %6d"
                  % (os.path.basename(fn), n_rec, n_in, n_out))
            continue
        feats = load_features(fn)
        nf = npts_in = npts_out = nrec = 0
        for ft in feats:
            geom = ft.get("geometry")
            if not geom or not match(ft.get("properties") or {}, flt):
                continue
            nf += 1
            if typ >= 10:                                   # Punkte / Orte
                for x, y in iter_points(geom):
                    if bb and not (bb[0] <= x <= bb[2] and bb[1] <= y <= bb[3]):
                        continue
                    pr = ft["properties"]
                    nm = ascii_de(str(pr.get(namefield or "name") or ""))
                    p = (int(round(y * 1e5)), int(round(x * 1e5)))
                    records.append((typ, nm, [p]))
                    nrec += 1
                continue
            for line in iter_lines(geom):
                line = [(p[0], p[1]) for p in line]
                npts_in += len(line)
                for run in clip_runs(line, bb):
                    run = simplify(run, tol, lat_ref)
                    q = []
                    for x, y in run:
                        p = (int(round(y * 1e5)), int(round(x * 1e5)))
                        if not q or q[-1] != p:
                            q.append(p)
                    for k in range(0, len(q), 60000):       # u16 npts
                        chunk = q[max(0, k - 1):k + 60000]
                        if len(chunk) >= 2:
                            records.append((typ, "", chunk))
                            npts_out += len(chunk)
                            nrec += 1
        print("  Typ %2d %-34s %5d Features -> %5d Records, Punkte %6d -> %6d"
              % (typ, os.path.basename(fn), nf, nrec, npts_in, npts_out))

    order = {t: i for i, t in enumerate(DRAW_ORDER)}
    records.sort(key=lambda r: order.get(r[0], 99))         # stabil
    if not records:
        raise SystemExit("Keine Daten - Filter/BBox pruefen.")
    lats = [p[0] for r in records for p in r[2]]
    lons = [p[1] for r in records for p in r[2]]
    out = bytearray(HDR.pack(MAGIC, VERSION, 0, 0, len(records),
                             min(lats), min(lons), max(lats), max(lons), args.date))
    for typ, nm, pts in records:
        nmb = nm.encode("ascii")
        la = [p[0] for p in pts]
        lo = [p[1] for p in pts]
        out += REC.pack(typ, len(nmb), len(pts), min(la), min(lo),
                        max(la), max(lo))
        out += nmb
        for p in pts:
            out += PT.pack(*p)
    with open(args.output, "wb") as f:
        f.write(out)
    print("-> %s: %d Records, %d Punkte, %.1f KB, Stand %s"
          % (args.output, len(records), len(lats), len(out) / 1024, fmt_date(args.date)))


def fmt_date(d):
    return "%02d.%02d.%04d" % (d % 100, d // 100 % 100, d // 10000) if d else "unbekannt"


def vmap_date(path):
    with open(path, "rb") as f:
        return HDR.unpack(f.read(HDR.size))[-1]


# ---------------------------------------------------------------- lesen
def read_vmap(path):
    data = open(path, "rb").read()
    magic, ver, flags, _, n, a, b, c, d, _date = HDR.unpack_from(data, 0)
    if magic != MAGIC or ver != VERSION:
        raise SystemExit("Keine gueltige .vmap (Magic/Version)")
    off, recs = HDR.size, []
    for _ in range(n):
        t, nl, npts, *_bb = REC.unpack_from(data, off)
        off += REC.size
        name = data[off:off + nl].decode("ascii")
        off += nl
        pts = list(PT.iter_unpack(data[off:off + 8 * npts]))
        off += 8 * npts
        recs.append((t, name, pts))
    if off != len(data):
        raise SystemExit("Dateilaenge passt nicht zu den Records "
                         "(%d != %d)" % (off, len(data)))
    return (a, b, c, d), recs, len(data)


def cmd_info(args):
    bbx, recs, size = read_vmap(args.vmap)
    print("%s: %.1f KB, %d Records" % (args.vmap, size / 1024, len(recs)))
    print("Erstellt am: %s" % fmt_date(vmap_date(args.vmap)))
    print("BBox lat %.3f..%.3f  lon %.3f..%.3f"
          % (bbx[0] / 1e5, bbx[2] / 1e5, bbx[1] / 1e5, bbx[3] / 1e5))
    stat = {}
    for t, _, pts in recs:
        s = stat.setdefault(t, [0, 0])
        s[0] += 1
        s[1] += len(pts)
    for t in sorted(stat):
        print("  Typ %2d: %5d Records, %6d Punkte" % (t, *stat[t]))


def _pil():
    try:
        from PIL import Image, ImageDraw, ImageFont
        return Image, ImageDraw, ImageFont
    except ImportError:
        raise SystemExit("Fuer preview/sim wird Pillow benoetigt: "
                         "pip install pillow")


def _font(ImageFont, size):
    try:
        return ImageFont.load_default(size)
    except Exception:
        return ImageFont.load_default()


def cmd_preview(args):
    Image, ImageDraw, ImageFont = _pil()
    (la0, lo0, la1, lo1), recs, _ = read_vmap(args.vmap)
    la0, la1, lo0, lo1 = la0 / 1e5, la1 / 1e5, lo0 / 1e5, lo1 / 1e5
    lat0 = (la0 + la1) / 2
    c = math.cos(math.radians(lat0))
    w_m, h_m = (lo1 - lo0) * c, (la1 - la0)
    s = args.size / max(w_m, h_m)
    W, H = int(w_m * s) + 20, int(h_m * s) + 20
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    font = _font(ImageFont, max(11, args.size // 70))

    def xy(p):
        return (10 + (p[1] / 1e5 - lo0) * c * s, H - 10 - (p[0] / 1e5 - la0) * s)

    for t, name, pts in recs:
        col = COLORS.get(t, (255, 0, 255))
        if 10 <= t <= 14:
            x, y = xy(pts[0])
            r = {10: 4, 11: 3, 12: 2, 13: 4, 14: 3}.get(t, 2)
            d.ellipse((x - r, y - r, x + r, y + r), fill=col)
            d.text((x + r + 2, y - 7), name, fill=col, font=font)
        else:
            d.line([xy(p) for p in pts], fill=col,
                   width=2 if t == T_BORDER else 1)
    img.save(args.png)
    print("->", args.png, img.size)


def cmd_sim(args):
    """Rechnet exakt wie der Sketch: Mercator, 2. Ordnung um den Mittelpunkt."""
    Image, ImageDraw, ImageFont = _pil()
    _, recs, _ = read_vmap(args.vmap)
    lat0, lon0, mpp = args.lat, args.lon, args.mpp
    la0i, lo0i = int(round(lat0 * 1e5)), int(round(lon0 * 1e5))
    deg = math.pi / 180.0 * 1e-5
    c = math.cos(math.radians(lat0))
    tn = math.tan(math.radians(lat0))
    k = 6378137.0 * c / mpp
    psi = math.radians(args.heading or 0.0)
    cp, sp = math.cos(psi), math.sin(psi)
    cx, cy = 120.0, 150.0 if args.heading is not None else 120.0
    S = args.scale
    img = Image.new("RGB", (240 * S, 240 * S), BG)
    d = ImageDraw.Draw(img)
    font = _font(ImageFont, 11 * S)

    def prj(p):
        dlat = (p[0] - la0i) * deg
        dlon = (p[1] - lo0i) * deg
        east = dlon * k
        north = (dlat / c + dlat * dlat * tn / (2 * c)) * k
        u = east * cp - north * sp
        v = east * sp + north * cp
        return ((cx + u) * S, (cy - v) * S)

    air_max = {20: 1500, 21: 1000, 22: 700, 23: 2000, 24: 1000, 25: 2000, 26: 150, 27: 1000}
    for t, name, pts in recs:
        col = COLORS.get(t, (255, 0, 255))
        if t in air_max and mpp > air_max[t]:
            continue
        if 10 <= t <= 14:
            if ((t == T_PLACE_M and mpp > 1000) or (t == T_PLACE_S and mpp > 250)
                    or (t == T_AIRPORT_L and mpp > 2000) or (t == T_AIRPORT_S and mpp > 250)):
                continue
            x, y = prj(pts[0])
            d.ellipse((x - 2 * S, y - 2 * S, x + 2 * S, y + 2 * S), fill=col)
            d.text((x + 4 * S, y - 6 * S), name, fill=col, font=font)
        else:
            d.line([prj(p) for p in pts], fill=col, width=S)
    # Eigene Position
    d.polygon([(cx * S, (cy - 10) * S), ((cx - 6) * S, (cy + 8) * S),
               (cx * S, (cy + 4) * S), ((cx + 6) * S, (cy + 8) * S)],
              fill=(255, 40, 80), outline=(255, 255, 255))
    mask = Image.new("L", img.size, 0)
    ImageDraw.Draw(mask).ellipse((0, 0, 240 * S - 1, 240 * S - 1), fill=255)
    out = Image.new("RGB", img.size, (0, 0, 0))
    out.paste(img, (0, 0), mask)
    out.save(args.png)
    print("->", args.png)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("convert", help="GeoJSON -> .vmap")
    c.add_argument("-o", "--output", required=True)
    c.add_argument("-l", "--layer", action="append", default=[],
                   help="TYP:DATEI[:OPTION...] (mehrfach)")
    c.add_argument("--bbox", nargs=4, type=float,
                   metavar=("MINLON", "MINLAT", "MAXLON", "MAXLAT"))
    c.add_argument("--date", type=int,
                   default=int(datetime.date.today().strftime("%Y%m%d")),
                   help="Erstelldatum JJJJMMTT im Header (Standard: heute)")
    c.add_argument("--tol", type=float, default=1000.0,
                   help="Standard-Vereinfachung in Metern (1000)")
    c.set_defaults(fn=cmd_convert)
    i = sub.add_parser("info", help=".vmap pruefen")
    i.add_argument("vmap")
    i.set_defaults(fn=cmd_info)
    p = sub.add_parser("preview", help="PNG-Uebersicht")
    p.add_argument("vmap")
    p.add_argument("png")
    p.add_argument("--size", type=int, default=1000)
    p.set_defaults(fn=cmd_preview)
    s = sub.add_parser("sim", help="M5Dial-Bildschirm simulieren")
    s.add_argument("vmap")
    s.add_argument("png")
    s.add_argument("lat", type=float)
    s.add_argument("lon", type=float)
    s.add_argument("mpp", type=float, help="Meter pro Pixel")
    s.add_argument("--heading", type=float, default=None,
                   help="Kurs in Grad (Kurs-oben-Ansicht)")
    s.add_argument("--scale", type=int, default=3)
    s.set_defaults(fn=cmd_sim)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
