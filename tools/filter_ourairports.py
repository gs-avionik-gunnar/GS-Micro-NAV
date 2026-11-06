#!/usr/bin/env python3
"""Filtert die OurAirports-airports.csv auf deutsche Flugplaetze mit ICAO-Kennung.

  python3 tools/filter_ourairports.py airports.csv -o data/flugplaetze_de.csv

Behalten: iso_country=DE, Typ large/medium/small_airport, Kennung ED?? oder ET??
Weggelassen: Hubschrauberlandeplaetze, geschlossene Plaetze, Wasser-/Ballonplaetze
und Plaetze ohne ICAO-Kennung (Kennung wie DE-0123), weil sie auf der Karte
nur als unverstaendliche Beschriftung erscheinen wuerden.
"""
import argparse
import csv
import re

ap = argparse.ArgumentParser()
ap.add_argument("csv")
ap.add_argument("-o", "--output", required=True)
a = ap.parse_args()
keep, n_all = [], 0
with open(a.csv, encoding="utf-8-sig", newline="") as f:
    for r in csv.DictReader(f):
        if r["iso_country"] != "DE":
            continue
        n_all += 1
        if r["type"] in ("large_airport", "medium_airport", "small_airport") \
                and re.fullmatch(r"E[DT][A-Z]{2}", r["ident"]):
            keep.append(r)
keep.sort(key=lambda r: r["ident"])
with open(a.output, "w", encoding="utf-8", newline="") as f:
    w = csv.writer(f)
    w.writerow(["ident", "type", "name", "latitude_deg", "longitude_deg", "iso_country"])
    for r in keep:
        w.writerow([r["ident"], r["type"], r["name"], r["latitude_deg"],
                    r["longitude_deg"], "DE"])
print("%d deutsche Eintraege -> %d Flugplaetze mit ICAO-Kennung (%s)"
      % (n_all, len(keep), a.output))
