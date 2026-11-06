# GS-Micro-NAV

**Micro-NAV** ist eine Vektor-Moving-Map für den **M5Dial** (M5Stack StampS3, ESP32-S3, 8 MB Flash) von **GS-Avionik** ([gs-avionik.de](https://gs-avionik.de)).
Die Position kommt per WLAN als NMEA-Datenstrom von einem Host (z. B. GPS-/ADS-B-/FLARM-Empfänger); die Karten liegen als kompakte Vektordaten im Flash des Geräts und werden über eine Web-GUI hochgeladen.

> ⚠️ **Nur Lagebild!** Micro-NAV ist **kein** zugelassenes Navigationsgerät. Karten-, Luftraum- und Traffic-Daten dürfen nicht zur Navigation, Freigabe oder Kollisionsvermeidung verwendet werden. Luftraumdaten sind nur so aktuell wie der Zeitpunkt der Konvertierung (siehe „Karten-Stand“).

## Funktionen

- **Moving Map** auf dem 240×240-Rundumdisplay: Zoom per Drehrad, Nord oben oder Kurs oben, Entfernungsmaßstab
- **Position per WLAN**: TCP-Client auf `192.168.1.1:2000` (einstellbar), NMEA 0183 (`RMC`, `GGA`, mit Prüfsumme), z. B. von **Stratux** oder **GS-Traffic2WIFI** (siehe [NMEA-Datenquellen](#nmea-datenquellen))
- **Vektorkarten** (`.vmap`) im Flash (LittleFS): Grenzen, Küste, Flüsse, Autobahnen, Orte, Flugplätze mit ICAO-Kennung
- **Overlays** (`ovl_*.vmap`) werden über die Basiskarte gelegt, z. B. Flugplätze und Lufträume
- **Lufträume** (CTR, TMA/CTA, RMZ/TMZ, R, Q/D, P, TRA/Segelflug, Klasse E) mit Filter nach Typ und Untergrenze
- **Traffic** aus `$PFLAA`-Sätzen (FLARM-Datenport, z. B. von **Stratux** oder **GS-Traffic2WIFI**) als Symbole mit relativer Höhe
- **Direct-To**: ICAO-Kennung per Rad wählen → magentafarbene Linie zum Platz
- **Touch**: Karte verschieben, Tippen = zurück zur eigenen Position
- **Einheiten** km/h oder kts
- **AP-Modus** mit **Web-GUI**: Karten hochladen/löschen, WLAN-Client einstellen, **Firmware-Update (OTA)** und ein **Karten-Konverter**, der Rohdaten direkt im Browser in `.vmap` umwandelt
- **Karten-Stand**: Das Erstelldatum jeder Karte ist in der Datei gespeichert und wird im Gerätemenü und in der Web-GUI angezeigt

## Hardware

| | |
|---|---|
| Gerät | M5Stack **M5Dial** (StampS3 / ESP32-S3) |
| Flash | **8 MB** (Pflicht, wegen OTA-Partitionen) |
| GPS-Host | beliebiger Host, der NMEA 0183 per TCP bereitstellt (Standard `192.168.1.1:2000`), z. B. Stratux oder GS-Traffic2WIFI |

## Installation

### 1. Arduino-IDE vorbereiten

- Arduino-IDE 2.x
- Boardverwalter: **esp32** von Espressif, Version 3.x
- Bibliothek: **M5Dial** (zieht M5Unified und M5GFX nach)
- Board: **M5Dial** (alternativ *M5Stack StampS3*)
- **Flash Size: 8MB**
- Der Sketch-Ordner muss **`GS-Micro-NAV`** heißen (wie die `.ino`). Die mitgelieferte `partitions.csv` im Sketch-Ordner wird automatisch verwendet.

### 2. Erstes Flashen per USB

Die Partitionstabelle (`partitions.csv`) enthält zwei Programm-Slots für OTA-Updates (je 2,5 MB) und ca. 2,9 MB für Karten:

| Name | Typ | Offset | Größe |
|---|---|---|---|
| nvs | data | 0x9000 | 0x5000 |
| otadata | data | 0xE000 | 0x2000 |
| app0 (ota_0) | app | 0x10000 | 0x280000 |
| app1 (ota_1) | app | 0x290000 | 0x280000 |
| spiffs (LittleFS) | data | 0x510000 | 0x2F0000 |

Beim **ersten** Flashen mit dieser Tabelle: *Tools → Erase All Flash Before Sketch Upload → Enabled*. Dabei werden Karten und Einstellungen einmalig gelöscht. Danach kann die Option wieder aus.

Die Tabellen `partitions_4MB_ohneOTA.csv.txt` und `partitions_8MB_ohneOTA.csv.txt` sind Alternativen ohne Web-Update für 4-MB- bzw. 8-MB-Module (zum Verwenden in `partitions.csv` umbenennen).

### 3. Firmware-Update über die Web-GUI

1. In der Arduino-IDE: *Sketch → Kompilierte Binärdatei exportieren*
2. Am M5Dial im Menü auf **WLAN → AP** wechseln, mit dem WLAN des Geräts verbinden (siehe unten), Web-GUI öffnen
3. Im Abschnitt *Firmware aktualisieren* die Datei **`GS-Micro-NAV.ino.bin`** wählen und installieren. Nicht die Dateien `*.merged.bin`, `*.bootloader.bin` oder `*.partitions.bin`; diese werden abgelehnt.
4. Das Gerät startet danach selbst neu. Währenddessen nicht ausschalten.

## Bedienung

| Aktion | Wirkung |
|---|---|
| Drehen (Karte) | Zoom |
| Klick (Karte) | Menü öffnen |
| Halten (Karte) | Nord oben ↔ Kurs oben |
| Touch ziehen | Karte verschieben |
| Touch tippen | zurück zur eigenen Position |
| Drehen / Klick (Menü) | Auswahl / Eintrag aktivieren |
| Halten (Menü) | zurück zur Karte |

**Menü:** Zurück · Karte (Basiskarte wählen) · Ansicht (Nord/Kurs oben) · Einheit (km/h / kts) · Direct-To · Traffic (an/aus) · LR-Höhe (Luftraum-Untergrenze) · LR-Typen (Lufträume einzeln ein-/ausblenden) · WLAN (Client ↔ AP) · Info · **Karten-Stand** · **AP-Zugang**

## WLAN-Modi

**Client-Modus** (Standard): Das Gerät verbindet sich mit dem WLAN des GPS-Hosts und liest NMEA per TCP. Voreinstellungen: SSID `GPS-Host`, Passwort `geheim123`, Host `192.168.1.1`, Port `2000`. Änderbar in der Web-GUI.

### NMEA-Datenquellen

Micro-NAV verbindet sich als TCP-Client mit einem Host, der NMEA 0183 liefert (Position über `$..RMC` und `$..GGA`, Traffic über `$PFLAA`). Beispiele für geeignete Quellen:

| Quelle | Verwendung |
|---|---|
| [**Stratux**](https://github.com/b3nn0/stratux) (Open-Source-ADS-B-Empfänger auf Raspberry-Pi-Basis, Europe Edition) | Stratux spannt ein eigenes WLAN auf und stellt neben GDL90 (UDP 4000) auch **FLARM-NMEA über TCP-Port 2000** bereit. SSID, Passwort und Host (Stratux-Adresse, je nach Einstellung z. B. `192.168.1.1` oder `192.168.10.1`) in der Web-GUI unter *WLAN-Client* eintragen. |
| **GS-Traffic2WIFI** ([GS-Avionik](https://gs-avionik.de)) | Liefert Position und Traffic per WLAN als NMEA/`$PFLAA`. SSID, Passwort, Host und Port in der Web-GUI unter *WLAN-Client* eintragen. |
| andere | jeder Host, der NMEA 0183 per TCP bereitstellt (GPS-WLAN-Gateways, FLARM-/ADS-B-Empfänger mit NMEA-TCP-Ausgabe, Simulatoren) |

Ohne `$PFLAA`-Sätze funktioniert die Moving Map trotzdem; es werden dann nur keine Verkehrssymbole angezeigt (Menü → *Traffic* kann abgeschaltet werden).

**AP-Modus** (Menü → *WLAN → AP*): Das Gerät spannt ein eigenes WLAN auf.

- **SSID:** `GS-MN-XX`, wobei `XX` die letzten zwei Hex-Ziffern der MAC-Adresse sind (aus `ESP.getEfuseMac()`)
- **Passwort:** die MAC-Adresse als 12 Hex-Zeichen, z. B. `AABBCCDDEEF1` (mit Doppelpunkten über `AP_PASS_COLONS 1` im Sketch)
- Beides steht auf dem AP-Bildschirm, im Menü *AP-Zugang* (mit QR-Code zum Verbinden) und auf der Info-Seite
- **Web-GUI:** `http://192.168.4.1`

Zurück in den Client-Modus: Menü → *WLAN → Client*.

## Karten erstellen

### Das `.vmap`-Format

Kompaktes Binärformat (little-endian), Koordinaten in 1e-5 Grad:

```
Header (32 Byte): "VMAP" | u8 Version (=1) | u8 Flags | u16 reserviert | u32 Anzahl Records |
                  i32 minLat, minLon, maxLat, maxLon | u32 Erstelldatum (JJJJMMTT, 0 = unbekannt)
Record:           u8 Typ | u8 Namenslänge | u16 Anzahl Punkte | i32 minLat, minLon, maxLat, maxLon |
                  <Name, ASCII> | Anzahl × (i32 lat, i32 lon)
```

Die Records werden in Dateireihenfolge gezeichnet (unten → oben).

| Typ | Bedeutung | Typ | Bedeutung |
|---|---|---|---|
| 0 | Landesgrenze | 13 | Verkehrsflughafen (Name = ICAO) |
| 1 | Regionen / Bundesländer | 14 | Flugplatz (Name = ICAO) |
| 2 | Nachbarländer | 20 | CTR |
| 3 | Flüsse / Gewässer | 21 | TMA / CTA (Klassen A–D) |
| 4 | Autobahn | 22 | RMZ / TMZ |
| 5 | Hauptstraße | 23 | Beschränkungsgebiet (R) |
| 6 | Bahn | 24 | Gefahrengebiet (Q/D) |
| 7 | Küste | 25 | Verbotszone (P) |
| 10 / 11 / 12 | Großstadt / Stadt / Ort | 26 | TRA / Segelflug / Wellengebiet |
| | | 27 | Luftraum E |

Luftraum-Records tragen den Namen `LABEL|Untergrenze|Obergrenze`.
Dateien mit dem Präfix **`ovl_`** werden als Overlay über die gewählte Basiskarte gelegt (z. B. `ovl_luftraum.vmap`, `ovl_flugplaetze.vmap`). Alle anderen `.vmap`-Dateien sind wählbare Basiskarten.

### Der Konverter

Der Konverter läuft **komplett im Browser**, ohne zusätzliche Software und ohne Internetverbindung (nur die Quelldateien müssen vorliegen). Es gibt ihn zweimal:

- **in der Web-GUI des Geräts** (AP-Modus) im Abschnitt *Karten selbst erzeugen (Konverter)*: Die fertige Datei kann direkt auf das Gerät hochgeladen werden.
- als **eigenständige Datei** [`tools/vmap-konverter.html`](tools/vmap-konverter.html): im Browser öffnen, konvertieren, *Herunterladen*, danach über die Web-GUI hochladen.

**Bedienung**

1. Quelldateien wählen (mehrere gleichzeitig möglich): GeoJSON (`.geojson`/`.json`), `airports.csv`, OpenAir-`.txt` oder eine `.zip` (z. B. von OpenFlightMaps).
2. Für bekannte Quellen werden die Ebenen **automatisch** mit sinnvollen Filtern und Toleranzen angelegt. Pro Ebene lassen sich Typ, Filter, Namensfeld und Toleranz ändern oder die Ebene abschalten.
3. Optional: **BBox** (W S O N in Grad; Standard `5.0 46.8 16.5 55.8` = Deutschland), **Standard-Toleranz** und Dateiname.
4. **Konvertieren** zeigt Log und Vorschau. Danach **Herunterladen** oder **Auf das Gerät hochladen**.
5. Beim Konvertieren wird das **aktuelle Datum** in die Datei geschrieben.

**Filtersyntax** (Feld *Filter*, mehrere Bedingungen mit `:` getrennt):

| Schreibweise | Bedeutung |
|---|---|
| `key=a\|b` | Property ist einer der Werte |
| `key!=a\|b` | Property ist keiner der Werte |
| `key>=zahl`, `key<=zahl`, `key>zahl`, `key<zahl` | numerischer Vergleich |

Beispiel: `adm0name=Germany:pop_max>=150000:pop_max<500000`

**Toleranz** (Meter) bestimmt, wie stark Linien vereinfacht werden (Douglas-Peucker). Größere Werte ergeben kleinere Dateien. Faustwerte: Küste/Flüsse ca. 1000–1500, Luftraum 150 (ca. 113 KB für Deutschland).

### Quelldaten und Bezugsquellen

| Daten | Quelle | Lizenz |
|---|---|---|
| Grenzen, Küste, Flüsse, Straßen, Orte | [Natural Earth](https://www.naturalearthdata.com/), GeoJSON-Dateien im Ordner `geojson/` von [nvkelso/natural-earth-vector](https://github.com/nvkelso/natural-earth-vector/tree/master/geojson) (Maßstab 1:10m) | Public Domain |
| Flugplätze mit ICAO-Kennung | [OurAirports](https://ourairports.com/data/), Datei `airports.csv` | Public Domain |
| Lufträume (OpenAir) | [open flightmaps](https://www.openflightmaps.org/), OpenAir-Daten Deutschland (`openair_ed.zip`) aus dem Download-Bereich | CC BY-NC-SA, nur nicht-kommerziell, Namensnennung, Weitergabe unter gleichen Bedingungen |

Die Lufträume von open flightmaps hängen am **AIRAC-Zyklus (28 Tage)**. Für ein aktuelles Lagebild regelmäßig neu laden und konvertieren. Das Konvertierdatum steht im Gerätemenü unter *Karten-Stand*.

### Anleitung 1: Basiskarte Deutschland (grob) mit Flugplätzen

Benötigte Dateien aus dem Natural-Earth-GeoJSON-Ordner (jeweils *Raw* herunterladen):

- `ne_10m_admin_0_countries.geojson`
- `ne_10m_coastline.geojson`
- `ne_10m_admin_1_states_provinces_lines.geojson`
- `ne_10m_rivers_lake_centerlines.geojson`
- `ne_10m_roads.geojson` (groß, auf dem Smartphone langsam; am Rechner konvertieren)
- `ne_10m_populated_places_simple.geojson`
- optional `airports.csv` von OurAirports

Alle Dateien im Konverter auswählen. Die Ebenen entstehen automatisch:

| Datei | Ebenen |
|---|---|
| admin_0_countries | Nachbarländer (Typ 2, `ADM0_A3!=DEU`) · Landesgrenze (Typ 0, `ADM0_A3=DEU`) |
| coastline | Küste (Typ 7) |
| admin_1_states_provinces_lines | Bundesländer (Typ 1, `ADM0_A3=DEU`) |
| rivers_lake_centerlines | Flüsse (Typ 3) |
| roads | Hauptstraßen (Typ 5, `type=Secondary Highway`) · Autobahnen (Typ 4, `type=Major Highway`) |
| populated_places_simple | Orte (Typen 12/11/10 nach Einwohnerzahl, dazu Hauptstädte der Nachbarländer) |
| airports.csv | Verkehrsflughäfen (Typ 13, `large_airport`/`medium_airport`) · Flugplätze (Typ 14, `small_airport`), jeweils `iso_country=DE`, Name = `ident` |

Dateiname z. B. `deutschland_grob.vmap` → Konvertieren → Hochladen (ca. 100 bis 115 KB). Die Datei wird beim ersten Upload automatisch als aktive Karte gewählt, sonst im Menü unter *Karte*.

### Anleitung 2: Flugplatz-Overlay

1. `airports.csv` von [ourairports.com/data](https://ourairports.com/data/) herunterladen.
2. Nur diese Datei im Konverter wählen. Es entstehen zwei Ebenen (Verkehrsflughäfen und Flugplätze in Deutschland).
3. Dateiname `ovl_flugplaetze.vmap` (wird automatisch vorgeschlagen) → Konvertieren → Hochladen (ca. 15 KB, rund 460 Plätze).

Flugplätze aus den Overlays erscheinen auch in der **Direct-To**-Auswahl. Für andere Länder in der Ebene den Filter `iso_country=DE` anpassen (z. B. `AT`, `CH`) und die BBox erweitern.

### Anleitung 3: Luftraum-Overlay

1. Bei [open flightmaps](https://www.openflightmaps.org/) die OpenAir-Daten für Deutschland (`openair_ed.zip`) herunterladen. Die ZIP kann **ungepackt direkt** im Konverter gewählt werden.
2. Aus der ZIP wird die Variante `isolated/ed.seeyou.openair.txt` (nur Deutschland) verwendet. Die Variante `embedded` (inkl. Nachbarländer) wird zwar als Ebene angelegt, ist aber standardmäßig deaktiviert; zum Verwenden aktivieren und die `isolated`-Ebene deaktivieren.
3. Dateiname `ovl_luftraum.vmap` (wird automatisch vorgeschlagen) → Konvertieren → Hochladen (ca. 113 KB, rund 700 Lufträume).

Hinweise:

- Unterstützt werden Polygone (`DP`) und Kreise (`V X=` + `DC`). Kreisbögen (`DA`/`DB`) werden nicht unterstützt; der Konverter zeigt dazu eine Warnung.
- Die Datei enthält nur **Luftraumflächen**, keine Flugplätze. Diese kommen aus dem Overlay aus Anleitung 2.
- Typzuordnung: `AY CTR` → 20 · `AC C/D` → 21 · `AY RMZ/TMZ` → 22 · `AC R` → 23 · `AC Q` → 24 · `AC P` → 25 · `AC W/G` → 26 · `AC E` → 27.

### Speicherbedarf

Der Flash bietet ca. 2,9 MB für Karten. Zur Orientierung: Basiskarte Deutschland grob ≈ 100 KB, Flugplätze ≈ 15 KB, Lufträume ≈ 115 KB. Der belegte und freie Speicher wird in der Web-GUI und auf dem Gerät (Info) angezeigt.

## Projektstruktur

```
GS-Micro-NAV/
├── GS-Micro-NAV.ino                 Sketch (inkl. Web-GUI und eingebettetem Konverter)
├── partitions.csv                   Partitionstabelle 8 MB mit OTA
└── tools/
    ├── vmap-konverter.html          Konverter als eigenständige Seite (erzeugt)
    ├── build_web.py                 Fügt tools/web/* in Sketch und Konverter-Seite ein
    └── web/                         Quelltext des Konverters (konverter.js, konverter_ui.js, konverter.part.html)
```

### Konverter weiterentwickeln

Der Konverter-Quelltext liegt in `tools/web/`. Nach Änderungen `python3 tools/build_web.py` ausführen. Das Skript schreibt den Code in die Web-GUI des Sketches (zwischen den Markierungen `KONV-HTML-*` und `KONV-JS-*`) und erzeugt `tools/vmap-konverter.html` neu. Für den Betrieb des Geräts oder das Konvertieren wird Python **nicht** benötigt.

## Lizenz und Datenquellen

Der Quellcode steht unter der **MIT-Lizenz** (siehe [`LICENSE`](LICENSE)). Für die Kartendaten gelten die Lizenzen der Quellen (siehe Tabelle oben). Natural Earth und OurAirports sind gemeinfrei. Die Luftraumdaten von open flightmaps stehen unter **CC BY-NC-SA** und dürfen nur nicht-kommerziell verwendet und weitergegeben werden. Bei Weitergabe daraus erzeugter `.vmap`-Dateien ist open flightmaps zu nennen.

---

© GS-Avionik · [gs-avionik.de](https://gs-avionik.de)
