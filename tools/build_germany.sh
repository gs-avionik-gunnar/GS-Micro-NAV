#!/bin/sh
# Erzeugt maps/deutschland_grob.vmap aus Natural-Earth-GeoJSON (Public Domain).
#
# Quelle: https://github.com/nvkelso/natural-earth-vector  (Ordner geojson/)
# Benoetigt dort (1:10m):
#   ne_10m_admin_0_countries.geojson
#   ne_10m_admin_1_states_provinces_lines.geojson
#   ne_10m_coastline.geojson
#   ne_10m_rivers_lake_centerlines.geojson
#   ne_10m_roads.geojson
#   ne_10m_populated_places_simple.geojson
#
# Aufruf:  tools/build_germany.sh /pfad/zu/natural-earth-vector/geojson [airports.csv]
set -e
NE="${1:?Pfad zum Natural-Earth-geojson-Ordner angeben}"
HERE="$(cd "$(dirname "$0")" && pwd)"
# Flugplaetze: Standard = data/flugplaetze_de.csv (aus OurAirports, tools/filter_ourairports.py).
# Eigene Liste: CSV als 2. Argument angeben.
AP="${2:-$HERE/../data/flugplaetze_de.csv}"
OUT="$HERE/../maps/deutschland_grob_flugplaetze.vmap"

python3 "$HERE/geojson2vmap.py" convert -o "$OUT" \
  --bbox 5.0 46.8 16.5 55.8 --tol 1000 \
  -l "2:$NE/ne_10m_admin_0_countries.geojson:ADM0_A3!=DEU:tol=1500" \
  -l "7:$NE/ne_10m_coastline.geojson:tol=1500" \
  -l "1:$NE/ne_10m_admin_1_states_provinces_lines.geojson:ADM0_A3=DEU:tol=800" \
  -l "3:$NE/ne_10m_rivers_lake_centerlines.geojson:tol=1000" \
  -l "5:$NE/ne_10m_roads.geojson:type=Secondary Highway:tol=2000" \
  -l "4:$NE/ne_10m_roads.geojson:type=Major Highway:tol=1200" \
  -l "0:$NE/ne_10m_admin_0_countries.geojson:ADM0_A3=DEU:tol=600" \
  -l "12:$NE/ne_10m_populated_places_simple.geojson:adm0name=Germany:pop_max>=60000:pop_max<150000" \
  -l "11:$NE/ne_10m_populated_places_simple.geojson:adm0name=Germany:pop_max>=150000:pop_max<500000" \
  -l "11:$NE/ne_10m_populated_places_simple.geojson:adm0name!=Germany:featurecla=Admin-0 capital" \
  -l "10:$NE/ne_10m_populated_places_simple.geojson:adm0name=Germany:pop_max>=500000" \
  -l "14:$AP:iso_country=DE:type=small_airport:name=ident" \
  -l "13:$AP:iso_country=DE:type=large_airport|medium_airport:name=ident"

python3 "$HERE/geojson2vmap.py" info "$OUT"
