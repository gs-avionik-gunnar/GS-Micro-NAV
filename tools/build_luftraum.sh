#!/bin/sh
# Erzeugt maps/ovl_luftraum.vmap aus einer OpenAir-Datei (OpenFlightMaps, openair_ed.zip).
#
#   isolated/ed.seeyou.openair.txt  -> nur Deutschland          (empfohlen)
#   embedded/ed.seeyou.openair.txt  -> inkl. Nachbarlaender     (fuer Grenzflug)
#
# Aufruf:  tools/build_luftraum.sh /pfad/zu/isolated/ed.seeyou.openair.txt
# Danach ovl_luftraum.vmap ueber die Web-GUI hochladen (alte Datei wird ersetzt).
# Daten haengen am AIRAC-Zyklus (28 Tage) - regelmaessig neu erzeugen.
set -e
IN="${1:?OpenAir-Datei angeben}"
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/../maps/ovl_luftraum.vmap"
# tol = Vereinfachung in Metern (150: ~113 KB; 60: ~146 KB)
python3 "$HERE/geojson2vmap.py" convert -o "$OUT" -l "oa:$IN:tol=150"
python3 "$HERE/geojson2vmap.py" info "$OUT"
