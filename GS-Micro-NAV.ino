/*
  Micro-NAV 1.0Beta1  -  GS-Avionik (gs-avionik.de)
  Lizenz: MIT (siehe LICENSE)
  =================================================
  (Moving Map fuer den M5Dial)
  Vektor-Moving-Map auf dem M5Dial (StampS3, ESP32-S3, 8 MB Flash).

  - Position per WLAN: TCP-Client auf 192.168.1.1:2000, NMEA 0183
    ($..RMC fuer Position/Kurs/Speed, $..GGA fuer Satelliten/Hoehe)
  - Bedienung: Drehen = Zoom (Karte) bzw. Auswahl (Menue)
               Klick  = Menue oeffnen / Eintrag waehlen
               Halten = in der Karte Nord-oben <-> Kurs-oben umschalten
               Touch  = Karte ziehen: verschieben; Tippen: zurueck zur eigenen Position
  - Direct-To: Menue > Direct-To, ICAO-Kennung per Rad waehlen -> magenta Linie zum Platz
    (Plaetze kommen aus den geladenen Karten, z.B. ovl_flugplaetze.vmap)
  - Menue: Zurueck | Karte | Ansicht | Einheit | Direct-To | Traffic | LR-Hoehe | LR-Typen |
           WLAN: Client <-> AP | Info | Karten-Stand | AP-Zugang
  - Overlay-Karten: Dateien mit Namen ovl_*.vmap (z.B. ovl_luftraum.vmap) werden ueber
    die gewaehlte Karte gelegt. Luftraeume (Typ 20..26) lassen sich im Menue nach
    Untergrenze filtern. Nur Lagebild - NICHT zur Navigation / Freigabe verwenden!
  - Traffic: $PFLAA-Saetze (FLARM-Datenport, auch von vielen ADS-B-Empfaengern) werden als
    Flugzeugsymbole mit relativer Hoehe (in 100 ft) gezeigt. Kein Kollisionswarnsystem!
  - AP-Modus: M5Dial spannt ein WLAN auf (WPA2), Web-GUI unter
    http://192.168.4.1 zum Hochladen/Loeschen von .vmap-Karten und zum
    Einstellen von SSID/Passwort/Host/Port des GPS-Hosts und fuer das Firmware-Update
    (Datei GS-Micro-NAV.ino.bin, siehe unten).
    AP-Zugang: SSID GS-MN-XX (XX = letztes MAC-Byte), Passwort = MAC (12 Hex-Zeichen), beides im Menue
    unter "AP-Zugang" und auf dem AP-Bildschirm. Menue "Karten-Stand": Erstelldatum der Karten.
    Konverter in der Web-GUI: GeoJSON / airports.csv / OpenAir (.txt/.zip) -> .vmap direkt im Browser
    (Quelltext: tools/web/, Einbau: python3 tools/build_web.py).
  - Karten liegen im Flash (LittleFS). Format und Konverter: siehe README.md

  Arduino-IDE (2.x) / Einstellungen
  ---------------------------------
  Boardverwalter : "esp32" von Espressif (3.x)
  Board          : M5Dial (falls nicht vorhanden: M5Stack StampS3)
  Flash Size     : 8MB (Pflicht fuer das Firmware-Update ueber die Web-GUI)
  Partition      : partitions.csv im Sketch-Ordner wird automatisch benutzt:
                   2 x 2,5 MB Programm (OTA) + ca. 2,9 MB Karten. Beim ERSTEN Flashen mit
                   dieser Tabelle per USB: Tools -> Erase All Flash Before Sketch Upload = Enabled
                   (loescht Karten und Einstellungen einmalig).
  Firmware-Update: Sketch -> "Kompilierte Binaerdatei exportieren" -> Datei
                   GS-Micro-NAV.ino.bin (NICHT die .merged.bin/.bootloader.bin/.partitions.bin)
                   im AP-Modus ueber die Web-GUI hochladen, danach startet das Geraet neu.
  Bibliotheken   : M5Dial (zieht M5Unified + M5GFX nach)
  Sketch-Ordner  : muss "GS-Micro-NAV" heissen (wie diese .ino)

  Hinweis zum Encoder: Die M5Dial-Bibliothek zaehlt in Viertelschritten.
  Springt die Auswahl pro Rastung um 2 oder 4 Eintraege, ENC_COUNTS_PER_DETENT
  unten anpassen (Standard 4).
*/

#include <M5Dial.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Update.h>
#include <Preferences.h>
#include <vector>
#include <algorithm>
#include <math.h>

// ============================== Einstellungen ==============================
// Standardwerte fuer den WLAN-Client. Spaeter in der Web-GUI aenderbar
// (werden dann im NVS gespeichert und ueberschreiben diese Werte).
#define DEFAULT_STA_SSID "GPS-Host"
#define DEFAULT_STA_PASS "geheim123"   // leer lassen fuer offenes WLAN
#define DEFAULT_HOST     "192.168.1.1"
#define DEFAULT_PORT     2000

// Access-Point-Modus (Passwort mindestens 8 Zeichen!)
// AP-Zugang: SSID "GS-MN-XX" (XX = letztes Byte der MAC), Passwort = MAC (12 Hex-Zeichen)
#define AP_PASS_COLONS 0     // 1 = Passwort mit Doppelpunkten (AA:BB:CC:DD:EE:FF)

#define ENC_COUNTS_PER_DETENT 4   // Encoder-Zaehlschritte pro Rastung
#define CLICK_SOUND           1   // 1 = leises Klicken beim Drehen
#define DISPLAY_BRIGHTNESS    180
#define MENU_TIMEOUT_MS       20000

// Produktangaben (Bootscreen, Info-Seite, Web-GUI)
#define APP_VENDOR  "GS-Avionik"
#define APP_NAME    "Micro-NAV"
#define APP_VERSION "1.0Beta1"
#define APP_URL     "gs-avionik.de"
// ===========================================================================

#define UPLOAD_TMP "/upload.tmp"
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

// ------------------------------- Dateiformat -------------------------------
#pragma pack(push, 1)
struct VmapHeader {
  char     magic[4];       // "VMAP"
  uint8_t  version;        // 1
  uint8_t  flags;
  uint16_t reserved;
  uint32_t numRecords;
  int32_t  minLat, minLon, maxLat, maxLon;   // 1e-5 Grad
  uint32_t buildDate;      // Erstelldatum JJJJMMTT, 0 = unbekannt
};
struct VmapRecord {
  uint8_t  type;
  uint8_t  nameLen;
  uint16_t npts;
  int32_t  minLat, minLon, maxLat, maxLon;
};
#pragma pack(pop)
static_assert(sizeof(VmapHeader) == 32, "VmapHeader");
static_assert(sizeof(VmapRecord) == 20, "VmapRecord");

// Typen: 0 Grenze, 1 Region, 2 Nachbar, 3 Wasser, 4 Autobahn, 5 Strasse,
//        6 Bahn, 7 Kueste, 10/11/12 Orte (gross/mittel/klein),
//        13 Verkehrsflughafen, 14 Flugplatz (Name = ICAO)
struct LineStyle { uint16_t color; float maxMpp; bool thick; };
static const LineStyle LINE_STYLE[8] = {
  { RGB565(235, 235, 235), 5000, true  },   // 0 Landesgrenze
  { RGB565(95, 105, 120),  5000, false },   // 1 Bundeslaender
  { RGB565(62, 72, 88),    5000, false },   // 2 Nachbarlaender
  { RGB565(45, 115, 225),  5000, false },   // 3 Fluesse
  { RGB565(240, 140, 30),  5000, false },   // 4 Autobahn
  { RGB565(170, 145, 60),  700,  false },   // 5 Hauptstrasse
  { RGB565(110, 110, 110), 300,  false },   // 6 Bahn
  { RGB565(70, 130, 180),  5000, false },   // 7 Kueste
};
static const uint16_t PLACE_COLOR[3]  = { RGB565(255, 255, 255), RGB565(230, 230, 200), RGB565(190, 190, 160) };
static const float    PLACE_MAXMPP[3] = { 5000, 1000, 250 };
static const uint16_t C_AIRPORT       = RGB565(230, 90, 230);
static const float    AIRPORT_MAXMPP[2]   = { 3500, 700 };   // Symbol: Typ 13 / 14
static const float    AIRPORT_LABELMPP[2] = { 1000, 250 };   // ICAO-Kennung dazu

// Luftraeume: Typ 20..27 = geschlossene Polygone, Name "LABEL|von|bis"
// 20 CTR, 21 TMA/CTA, 22 RMZ/TMZ, 23 Beschraenkung (R), 24 Gefahr/Fallschirm (Q),
// 25 Verbot (P), 26 TRA/Segelflug/Wellenfenster, 27 Luftraum E
static const LineStyle AS_STYLE[8] = {
  { RGB565(60, 140, 255),  1500, true  },
  { RGB565(70, 170, 200),  1000, false },
  { RGB565(80, 190, 110),  700,  false },
  { RGB565(235, 70, 70),   2000, false },
  { RGB565(250, 200, 60),  1000, false },
  { RGB565(255, 40, 40),   2000, true  },
  { RGB565(120, 150, 120), 150,  false },
  { RGB565(160, 120, 255), 1000, false },   // 27 Luftraum E
};
#define AS_LABEL_MAXMPP 150          // Namen/Hoehen nur bei starkem Zoom
// Luftraum-Filter: zeigt Raeume, deren Untergrenze <= Schwelle (in 100 ft) liegt
static const int         AS_LEVEL_N = 6;
static const int         AS_LEVEL_FT100[AS_LEVEL_N] = { -1, 30, 50, 100, 195, 9999 };
static const char *const AS_LEVEL_TXT[AS_LEVEL_N]   = { "aus", "bis 3000 ft", "bis 5000 ft", "bis FL100", "bis FL195", "alle" };
static const char *const AS_TYPE_NAME[9] = { "CTR", "TMA / CTA", "RMZ / TMZ", "Beschr. (R)", "Gefahr/Fallsch.", "Verbot (P)", "TRA/Segelflug", "Luftraum E", "Zurueck" };

static const uint16_t C_BG       = RGB565(8, 12, 20);
static const uint16_t C_BLACK    = RGB565(0, 0, 0);
static const uint16_t C_WHITE    = RGB565(255, 255, 255);
static const uint16_t C_GREY     = RGB565(140, 150, 165);
static const uint16_t C_MARK     = RGB565(255, 40, 80);
static const uint16_t C_NOFIX    = RGB565(150, 150, 150);
static const uint16_t C_GREEN    = RGB565(60, 220, 100);
static const uint16_t C_YELLOW   = RGB565(250, 210, 50);
static const uint16_t C_ORANGE   = RGB565(250, 140, 40);
static const uint16_t C_RED      = RGB565(240, 70, 70);
static const uint16_t C_SELBG    = RGB565(30, 70, 140);
static const uint16_t C_TRAFFIC  = RGB565(140, 255, 255);

// Massstaebe in Metern pro Pixel (Rad im Uhrzeigersinn = hineinzoomen)
static const float ZOOM_MPP[] = { 5, 10, 25, 50, 100, 250, 500, 1000, 2000, 3500 };
static const int   ZOOM_N     = sizeof(ZOOM_MPP) / sizeof(ZOOM_MPP[0]);

// ------------------------------ Zustandsdaten ------------------------------
struct AppConfig {
  String   staSsid, staPass, host, map, dct;
  uint16_t port = DEFAULT_PORT;
  uint8_t  zoom = 9;
  uint8_t  headingUp = 0;
  uint8_t  traffic = 1;               // Traffic anzeigen
  uint8_t  units = 0;                 // Geschwindigkeit: 0 = km/h, 1 = kts
  uint8_t  asLevel = 3;               // Luftraum-Filter (Index AS_LEVEL_*)
  uint8_t  asMask = 0xFF;             // Bit i = Luftraumtyp 20+i anzeigen
  double   lastLat = 51.1657, lastLon = 10.4515;
};

struct GpsData {
  double   lat = 0, lon = 0;
  float    speedKn = 0, course = 0, alt = 0;
  bool     fix = false, courseValid = false;
  uint8_t  sats = 0;
  uint32_t lastAnyMs = 0, lastPosMs = 0, lastRmcMs = 0, seq = 0;
};

#define MAX_TRAF           24
#define TRAFFIC_TIMEOUT_MS 10000     // Ziel verschwindet nach 10 s ohne Update
struct Traffic {
  char     id[10];
  float    north, east, vert;        // relativ zur eigenen Position, Meter
  float    track, gs, climb;         // Grad, m/s, m/s
  uint8_t  alarm, type;
  bool     trackOk;
  uint32_t lastMs;                   // 0 = Slot frei
};

struct View {
  int32_t lat0 = 0, lon0 = 0;
  float   mpp = 1, k = 1, invC = 1, tOver2C = 0, cosP = 1, sinP = 0, cx = 120, cy = 120;
  int32_t vMinLat = 0, vMaxLat = 0, vMinLon = 0, vMaxLon = 0;
};

enum WifiModeE { MODE_CLIENT, MODE_AP };
enum UiState   { UI_MAP, UI_MENU, UI_INFO, UI_ASTYPES, UI_DCT };

#define MENU_COUNT 12  // 0 Zurueck, 1 Karte, 2 Ansicht, 3 Einheit, 4 Direct-To, 5 Traffic, 6 LR-Hoehe, 7 LR-Typen, 8 WLAN, 9 Info, 10 Karten-Stand, 11 AP-Zugang

M5Canvas    canvas;
Preferences prefs;
WebServer   server(80);
AppConfig   cfg;
View        V;

GpsData           g_gps;
SemaphoreHandle_t g_gpsMutex = nullptr;
volatile bool     g_staWanted = false, g_netIdle = true, g_tcpUp = false;
Traffic           g_traf[MAX_TRAF];            // geschuetzt durch g_gpsMutex
volatile uint32_t g_trafSeq = 0;
uint32_t          g_lastTrafSeq = 0, g_lastAlarmBeep = 0;
int               g_trafCount = 0;

volatile WifiModeE wifiMode = MODE_CLIENT;
UiState  ui = UI_MAP;
int      menuSel = 0, asSel = 0;
bool     g_uiDirty = true, g_mapDirty = true, g_apDirty = true;
bool     g_panned = false;             // Karte per Touch verschoben
double   g_panLat = 0, g_panLon = 0;   // Kartenmitte im verschobenen Zustand

// Direct-To: Flugplaetze aus den Karten (Typ 13/14, Name = ICAO), Ziel aus cfg.dct
struct Apt { char id[5]; int32_t lat, lon; };
std::vector<Apt> g_apts;
bool     g_dctOk = false;                // Ziel gesetzt und Koordinaten bekannt
int32_t  g_dctLat = 0, g_dctLon = 0;
int      dctStep = 0, dctIdx = 0, dctN = 0, dctMin = 0;
char     dctPrefix[5] = "";
char     dctCand[28];
bool     g_mapOk = false, g_uploading = false, s_serverRunning = false;
size_t   g_mapSize = 0;
float    s_heading = 0;
uint32_t g_lastDraw = 0, g_lastSeq = 0xFFFFFFFF, g_lastInputMs = 0;
uint32_t g_zoomSaveAt = 0, g_lastPosSave = 0;
double   g_savedLat = 0, g_savedLon = 0;
long     g_encLast = 0;
int      g_encAccum = 0;
String   g_notice;                // kurze Meldung (z.B. WLAN-Wechsel)
std::vector<String> g_overlays;   // ovl_*.vmap, werden ueber die Karte gelegt

// Upload-Status
File   s_upFile;
bool   s_upErr = false;
String s_upName, s_upMsg;

// ------------------------------ Hilfsfunktionen ----------------------------
String fitStr(const String &s, size_t n) {
  if (s.length() <= n) return s;
  return s.substring(0, n - 1) + "~";
}

String g_apSsid, g_apPass;

// ESP.getEfuseMac(): erstes MAC-Byte steht im niederwertigsten Byte
void initApCreds() {
  uint64_t m = ESP.getEfuseMac();
  uint8_t b[6];
  for (int i = 0; i < 6; i++) b[i] = (uint8_t)(m >> (8 * i));
  char buf[20];
#if AP_PASS_COLONS
  snprintf(buf, sizeof buf, "%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
#else
  snprintf(buf, sizeof buf, "%02X%02X%02X%02X%02X%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
#endif
  g_apPass = buf;
  snprintf(buf, sizeof buf, "GS-MN-%02X", b[5]);
  g_apSsid = buf;
}

uint32_t vmapDate(const String &name) {
  File f = LittleFS.open("/" + name, "r");
  if (!f) return 0;
  VmapHeader h;
  if (f.read((uint8_t *)&h, sizeof h) != sizeof h || memcmp(h.magic, "VMAP", 4) != 0) return 0;
  return h.buildDate;
}

String fmtDate(uint32_t d, bool shortYear = true) {
  if (!d) return "-";
  char b[12];
  if (shortYear) snprintf(b, sizeof b, "%02u.%02u.%02u", (unsigned)(d % 100), (unsigned)(d / 100 % 100), (unsigned)(d / 10000 % 100));
  else snprintf(b, sizeof b, "%02u.%02u.%04u", (unsigned)(d % 100), (unsigned)(d / 100 % 100), (unsigned)(d / 10000));
  return b;
}

String jsonEsc(const String &s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

String sanitizeName(String n) {
  int s1 = n.lastIndexOf('/'), s2 = n.lastIndexOf('\\');
  int s = s1 > s2 ? s1 : s2;
  if (s >= 0) n = n.substring(s + 1);
  String o;
  for (size_t i = 0; i < n.length() && o.length() < 31; i++) {
    char c = n[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    o += ok ? c : '_';
  }
  String low = o;
  low.toLowerCase();
  if (!low.endsWith(".vmap") || low.length() < 6) return "";
  return o;
}

void beep() {
#if CLICK_SOUND
  M5Dial.Speaker.tone(5000, 6);
#endif
}

void markAllDirty() { g_uiDirty = g_mapDirty = g_apDirty = true; }

// -------------------------------- Konfiguration -----------------------------
void loadConfig() {
  prefs.begin("m5map", false);
  cfg.staSsid   = prefs.getString("ssid", DEFAULT_STA_SSID);
  cfg.staPass   = prefs.getString("pass", DEFAULT_STA_PASS);
  cfg.host      = prefs.getString("host", DEFAULT_HOST);
  cfg.port      = prefs.getUShort("port", DEFAULT_PORT);
  cfg.zoom      = prefs.getUChar("zoom", 9);
  cfg.headingUp = prefs.getUChar("hup", 0);
  cfg.asLevel   = prefs.getUChar("asl", 3);
  cfg.traffic   = prefs.getUChar("trf", 1);
  cfg.units     = prefs.getUChar("spd", 0) ? 1 : 0;
  if (cfg.asLevel >= AS_LEVEL_N) cfg.asLevel = 3;
  cfg.asMask    = prefs.getUChar("asm", 0xFF);
  cfg.map       = prefs.getString("map", "");
  cfg.dct       = prefs.getString("dct", "");
  cfg.lastLat   = prefs.getDouble("lat", 51.1657);
  cfg.lastLon   = prefs.getDouble("lon", 10.4515);
  if (cfg.zoom >= ZOOM_N) cfg.zoom = ZOOM_N - 1;
  g_savedLat = cfg.lastLat;
  g_savedLon = cfg.lastLon;
}

void savePosition() {
  prefs.putDouble("lat", cfg.lastLat);
  prefs.putDouble("lon", cfg.lastLon);
  g_savedLat = cfg.lastLat;
  g_savedLon = cfg.lastLon;
}

// ----------------------------------- Karten ----------------------------------
std::vector<String> listMaps() {
  std::vector<String> v;
  File root = LittleFS.open("/");
  if (!root) return v;
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    if (f.isDirectory()) continue;
    String n = f.name();
    int sl = n.lastIndexOf('/');
    if (sl >= 0) n = n.substring(sl + 1);
    if (n.endsWith(".vmap") && !n.startsWith("ovl_")) v.push_back(n);
  }
  return v;
}

void refreshOverlays() {
  g_overlays.clear();
  File root = LittleFS.open("/");
  if (!root) return;
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    if (f.isDirectory()) continue;
    String n = f.name();
    int sl = n.lastIndexOf('/');
    if (sl >= 0) n = n.substring(sl + 1);
    if (n.endsWith(".vmap") && n.startsWith("ovl_")) g_overlays.push_back(n);
  }
}

bool openMapMeta(const String &name) {
  g_mapOk = false;
  if (!name.length()) return false;
  File f = LittleFS.open("/" + name, "r");
  if (!f) return false;
  g_mapSize = f.size();
  VmapHeader h;
  if (f.read((uint8_t *)&h, sizeof h) != sizeof h) return false;
  if (memcmp(h.magic, "VMAP", 4) != 0 || h.version != 1) return false;
  g_mapOk = true;
  return true;
}

static void scanAirports(const String &path) {
  File f = LittleFS.open(path, "r");
  if (!f) return;
  VmapHeader h;
  if (f.read((uint8_t *)&h, sizeof h) != sizeof h) return;
  for (uint32_t i = 0; i < h.numRecords; i++) {
    VmapRecord r;
    if (f.read((uint8_t *)&r, sizeof r) != sizeof r) break;
    size_t base = f.position();
    if ((r.type == 13 || r.type == 14) && r.nameLen == 4 && r.npts >= 1) {
      Apt a;
      f.read((uint8_t *)a.id, 4);
      a.id[4] = 0;
      a.lat = r.minLat;
      a.lon = r.minLon;
      g_apts.push_back(a);
    }
    f.seek(base + (size_t)r.nameLen + (size_t)r.npts * 8);
  }
}

void loadAirports() {
  g_apts.clear();
  if (cfg.map.length()) scanAirports("/" + cfg.map);
  for (auto &ov : g_overlays) scanAirports("/" + ov);
  std::sort(g_apts.begin(), g_apts.end(), [](const Apt &a, const Apt &b) { return strcmp(a.id, b.id) < 0; });
  g_apts.erase(std::unique(g_apts.begin(), g_apts.end(), [](const Apt &a, const Apt &b) { return !strcmp(a.id, b.id); }), g_apts.end());
  g_dctOk = false;
  if (cfg.dct.length())
    for (auto &a : g_apts)
      if (cfg.dct == a.id) { g_dctOk = true; g_dctLat = a.lat; g_dctLon = a.lon; }
}

void mapReload() {
  openMapMeta(cfg.map);
  refreshOverlays();
  loadAirports();
  g_mapDirty = g_uiDirty = true;
}

// waehlt eine vorhandene Karte, falls die gespeicherte fehlt
void ensureMapSelected() {
  auto v = listMaps();
  bool found = false;
  for (auto &n : v) if (n == cfg.map) found = true;
  if (!found) {
    cfg.map = v.empty() ? "" : v[0];
    prefs.putString("map", cfg.map);
  }
  mapReload();
}

void cycleMap() {
  auto v = listMaps();
  if (v.empty()) { cfg.map = ""; mapReload(); return; }
  int idx = -1;
  for (size_t i = 0; i < v.size(); i++) if (v[i] == cfg.map) idx = (int)i;
  idx = (idx + 1) % (int)v.size();
  cfg.map = v[idx];
  prefs.putString("map", cfg.map);
  mapReload();
}

// prueft Header und Record-Kette einer Datei (erkennt abgebrochene Uploads)
bool validateVmap(const char *path, String &err) {
  File f = LittleFS.open(path, "r");
  if (!f) { err = "Datei nicht lesbar"; return false; }
  size_t size = f.size();
  VmapHeader h;
  if (size < sizeof h || f.read((uint8_t *)&h, sizeof h) != sizeof h) { err = "zu kurz"; return false; }
  if (memcmp(h.magic, "VMAP", 4) != 0 || h.version != 1) { err = "kein VMAP v1"; return false; }
  size_t pos = sizeof h;
  for (uint32_t i = 0; i < h.numRecords; i++) {
    VmapRecord r;
    if (pos + sizeof r > size) { err = "Records unvollstaendig"; return false; }
    f.seek(pos);
    if (f.read((uint8_t *)&r, sizeof r) != sizeof r) { err = "Lesefehler"; return false; }
    pos += sizeof r + r.nameLen + (size_t)r.npts * 8;
    if (pos > size) { err = "Datei abgeschnitten"; return false; }
  }
  if (pos != size) { err = "Laenge passt nicht"; return false; }
  return true;
}

// ----------------------------------- NMEA ------------------------------------
double nmeaCoord(const char *v, char hemi) {
  if (!v[0]) return NAN;
  double x = atof(v);
  int deg = (int)(x / 100.0);
  double d = deg + (x - deg * 100.0) / 60.0;
  return (hemi == 'S' || hemi == 'W') ? -d : d;
}

// $PFLAA,alarm,N,E,vert,idType,id,track,turn,gs,climb,type   (Meter, m/s)
static void handlePflaa(char **f, int n, uint32_t now) {
  if (n < 12 || !f[6][0]) return;
  Traffic t;
  memset(&t, 0, sizeof t);
  strlcpy(t.id, f[6], sizeof t.id);
  t.alarm   = (uint8_t)atoi(f[1]);
  t.north   = (float)atof(f[2]);
  t.east    = (float)atof(f[3]);
  t.vert    = (float)atof(f[4]);
  t.trackOk = f[7][0] != 0;
  t.track   = (float)atof(f[7]);
  t.gs      = (float)atof(f[9]);
  t.climb   = (float)atof(f[10]);
  t.type    = (uint8_t)strtol(f[11], nullptr, 16);
  t.lastMs  = now;
  xSemaphoreTake(g_gpsMutex, portMAX_DELAY);
  int slot = -1, oldest = 0;
  for (int i = 0; i < MAX_TRAF; i++) {
    if (g_traf[i].lastMs && !strcmp(g_traf[i].id, t.id)) { slot = i; break; }
    if (slot < 0 && (!g_traf[i].lastMs || now - g_traf[i].lastMs > TRAFFIC_TIMEOUT_MS)) slot = i;
    if (g_traf[i].lastMs < g_traf[oldest].lastMs) oldest = i;
  }
  if (slot < 0) slot = oldest;
  g_traf[slot] = t;
  g_trafSeq++;
  g_gps.lastAnyMs = now;
  xSemaphoreGive(g_gpsMutex);
}

void parseNmea(char *s) {
  if (s[0] != '$') return;
  char *star = strchr(s, '*');
  if (star) {
    uint8_t cs = 0;
    for (char *p = s + 1; p < star; ++p) cs ^= (uint8_t)*p;
    if (cs != (uint8_t)strtol(star + 1, nullptr, 16)) return;   // Pruefsumme falsch
    *star = 0;
  }
  char *f[24];
  int n = 0;
  char *p = s + 1;
  f[n++] = p;
  while (*p && n < 24) {
    if (*p == ',') { *p = 0; f[n++] = p + 1; }
    p++;
  }
  if (strlen(f[0]) < 5) return;
  if (!strcmp(f[0], "PFLAA")) {                // Traffic (herstellerunabhaengig, FLARM-Format)
    if (cfg.traffic) handlePflaa(f, n, millis());
    return;
  }
  const char *type = f[0] + 2;           // Talker-ID (GP, GN, GL, ...) ueberspringen
  uint32_t now = millis();

  if (!strncmp(type, "RMC", 3) && n >= 9) {
    bool valid = f[2][0] == 'A';
    double lat = nmeaCoord(f[3], f[4][0]);
    double lon = nmeaCoord(f[5], f[6][0]);
    xSemaphoreTake(g_gpsMutex, portMAX_DELAY);
    g_gps.lastAnyMs = now;
    if (valid && !isnan(lat) && !isnan(lon)) {
      g_gps.lat = lat;
      g_gps.lon = lon;
      g_gps.fix = true;
      g_gps.lastPosMs = g_gps.lastRmcMs = now;
      g_gps.speedKn = (float)atof(f[7]);
      if (f[8][0]) { g_gps.course = (float)atof(f[8]); g_gps.courseValid = true; }
      else g_gps.courseValid = false;
    } else {
      g_gps.fix = false;
    }
    g_gps.seq++;
    xSemaphoreGive(g_gpsMutex);
  } else if (!strncmp(type, "GGA", 3) && n >= 10) {
    int q = atoi(f[6]);
    double lat = nmeaCoord(f[2], f[3][0]);
    double lon = nmeaCoord(f[4], f[5][0]);
    xSemaphoreTake(g_gpsMutex, portMAX_DELAY);
    g_gps.lastAnyMs = now;
    g_gps.sats = (uint8_t)atoi(f[7]);
    g_gps.alt = (float)atof(f[9]);
    if (now - g_gps.lastRmcMs > 3000) {        // nur Position aus GGA, wenn kein RMC kommt
      if (q > 0 && !isnan(lat) && !isnan(lon)) {
        g_gps.lat = lat;
        g_gps.lon = lon;
        g_gps.fix = true;
        g_gps.lastPosMs = now;
      } else {
        g_gps.fix = false;
      }
      g_gps.seq++;
    }
    xSemaphoreGive(g_gpsMutex);
  }
}

GpsData getGps() {
  xSemaphoreTake(g_gpsMutex, portMAX_DELAY);
  GpsData c = g_gps;
  xSemaphoreGive(g_gpsMutex);
  return c;
}

// TCP-Client in eigenem Task (Core 0), damit Verbindungsversuche die UI nicht blockieren
void netTask(void *) {
  WiFiClient client;
  char line[160];
  size_t len = 0;
  uint8_t buf[128];
  uint32_t lastTry = 0, lastRx = 0;
  for (;;) {
    if (!g_staWanted) {
      client.stop();
      g_tcpUp = false;
      g_netIdle = true;
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    g_netIdle = false;
    if (WiFi.status() != WL_CONNECTED) {
      client.stop();
      g_tcpUp = false;
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    if (!client.connected()) {
      g_tcpUp = false;
      uint32_t now = millis();
      if (now - lastTry >= 2500) {
        lastTry = now;
        char host[64];
        strlcpy(host, cfg.host.c_str(), sizeof host);
        if (client.connect(host, cfg.port, 1500)) {
          client.setNoDelay(true);
          len = 0;
          lastRx = millis();
          g_tcpUp = true;
        }
      }
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    g_tcpUp = true;
    int avail = client.available();
    if (avail <= 0) {
      if (millis() - lastRx > 15000) client.stop();   // Verbindung ohne Daten -> neu aufbauen
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    int n = client.read(buf, avail > (int)sizeof buf ? sizeof buf : avail);
    if (n <= 0) continue;
    lastRx = millis();
    for (int i = 0; i < n; i++) {
      char c = (char)buf[i];
      if (c == '\n' || c == '\r') {
        if (len) { line[len] = 0; parseNmea(line); len = 0; }
      } else if (len < sizeof line - 1) {
        line[len++] = c;
      } else {
        len = 0;
      }
    }
  }
}

// --------------------------------- WLAN-Modi ---------------------------------
void stopNetTask() {
  g_staWanted = false;
  uint32_t t = millis();
  while (!g_netIdle && millis() - t < 3000) delay(10);
}

void startClientMode() {
  if (s_serverRunning) { server.stop(); s_serverRunning = false; }
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(200);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.begin(cfg.staSsid.c_str(), cfg.staPass.length() ? cfg.staPass.c_str() : nullptr);
  wifiMode = MODE_CLIENT;
  g_staWanted = true;
  markAllDirty();
}

void startApMode() {
  stopNetTask();
  savePosition();
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
  delay(200);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(g_apSsid.c_str(), g_apPass.c_str());
  delay(300);
  server.begin();
  s_serverRunning = true;
  wifiMode = MODE_AP;
  markAllDirty();
}

// ----------------------------- Karten-Rendering -----------------------------
static const float DEG_E5_TO_RAD = (float)(M_PI / 180.0 * 1e-5);
static const float DEG2RAD = (float)(M_PI / 180.0);

void setupView(double lat, double lon, float mpp, float headingDeg) {
  V.lat0 = (int32_t)lround(lat * 1e5);
  V.lon0 = (int32_t)lround(lon * 1e5);
  float phi = (float)(lat * M_PI / 180.0);
  float c = cosf(phi), t = tanf(phi);
  V.mpp = mpp;
  V.k = 6378137.0f * c / mpp;             // Pixel pro Mercator-Einheit im Zentrum
  V.invC = 1.0f / c;
  V.tOver2C = t / (2.0f * c);
  float psi = headingDeg * DEG2RAD;
  V.cosP = cosf(psi);
  V.sinP = sinf(psi);
  V.cx = 120;
  V.cy = cfg.headingUp ? 150 : 120;
  float rm = 230.0f * 1.3f * mpp;         // Sichtradius in Metern (mit Reserve)
  int32_t dLat = (int32_t)(rm / 1.11195f);
  int32_t dLon = (int32_t)(rm / (1.11195f * c));
  V.vMinLat = V.lat0 - dLat;
  V.vMaxLat = V.lat0 + dLat;
  V.vMinLon = V.lon0 - dLon;
  V.vMaxLon = V.lon0 + dLon;
}

// Mercator um den Mittelpunkt, Reihenentwicklung 2. Ordnung (Fehler < 0,1 % bei 400 km)
static inline void project(int32_t lat, int32_t lon, float &px, float &py) {
  float dlat = (float)(lat - V.lat0) * DEG_E5_TO_RAD;
  float dlon = (float)(lon - V.lon0) * DEG_E5_TO_RAD;
  float east = dlon * V.k;
  float north = (dlat * V.invC + dlat * dlat * V.tOver2C) * V.k;
  float u = east * V.cosP - north * V.sinP;
  float v = east * V.sinP + north * V.cosP;
  px = V.cx + u;
  py = V.cy - v;
}

// Liang-Barsky auf den 240x240-Sprite
static bool clipLine(float &x0, float &y0, float &x1, float &y1) {
  float dx = x1 - x0, dy = y1 - y0, t0 = 0, t1 = 1;
  const float p[4] = { -dx, dx, -dy, dy };
  const float q[4] = { x0, 239 - x0, y0, 239 - y0 };
  for (int i = 0; i < 4; i++) {
    if (p[i] == 0) {
      if (q[i] < 0) return false;
    } else {
      float r = q[i] / p[i];
      if (p[i] < 0) { if (r > t1) return false; if (r > t0) t0 = r; }
      else          { if (r < t0) return false; if (r < t1) t1 = r; }
    }
  }
  float nx0 = x0 + t0 * dx, ny0 = y0 + t0 * dy, nx1 = x0 + t1 * dx, ny1 = y0 + t1 * dy;
  x0 = nx0; y0 = ny0; x1 = nx1; y1 = ny1;
  return true;
}

static void drawRecordLines(File &f, const VmapRecord &r, const LineStyle &st) {
  int32_t buf[80];                         // 40 Punkte
  uint32_t remaining = r.npts;
  float px0 = 0, py0 = 0;
  bool have = false;
  while (remaining) {
    uint32_t n = remaining > 40 ? 40 : remaining;
    if (f.read((uint8_t *)buf, n * 8) != n * 8) return;
    for (uint32_t i = 0; i < n; i++) {
      float px, py;
      project(buf[2 * i], buf[2 * i + 1], px, py);
      if (have) {
        float x0 = px0, y0 = py0, x1 = px, y1 = py;
        if (clipLine(x0, y0, x1, y1)) {
          int ax = (int)lroundf(x0), ay = (int)lroundf(y0), bx = (int)lroundf(x1), by = (int)lroundf(y1);
          canvas.drawLine(ax, ay, bx, by, st.color);
          if (st.thick) {
            if (fabsf(x1 - x0) > fabsf(y1 - y0)) canvas.drawLine(ax, ay + 1, bx, by + 1, st.color);
            else                                 canvas.drawLine(ax + 1, ay, bx + 1, by, st.color);
          }
        }
      }
      px0 = px; py0 = py; have = true;
    }
    remaining -= n;
  }
}

static void drawPlace(const VmapRecord &r, const char *name) {
  float px, py;
  project(r.minLat, r.minLon, px, py);
  if (px < -40 || px > 280 || py < -12 || py > 252) return;
  int x = (int)lroundf(px), y = (int)lroundf(py);
  if (r.type >= 13) {                       // Flugplatz: Ring mit Kreuz + ICAO
    int rad = (r.type == 13) ? 5 : 3;
    canvas.drawCircle(x, y, rad, C_AIRPORT);
    canvas.drawLine(x - rad - 2, y, x + rad + 2, y, C_AIRPORT);
    canvas.drawLine(x, y - rad - 2, x, y + rad + 2, C_AIRPORT);
    if (V.mpp <= AIRPORT_LABELMPP[r.type - 13]) {
      canvas.setTextColor(C_AIRPORT);
      canvas.drawString(name, x + rad + 4, y - 7);
    }
    return;
  }
  int idx = r.type - 10;
  uint16_t col = PLACE_COLOR[idx];
  int rad = (idx == 0) ? 3 : (idx == 1 ? 2 : 1);
  canvas.fillCircle(x, y, rad, col);
  canvas.setTextColor(col);
  canvas.drawString(name, x + rad + 3, y - 7);
}

// "GND", "FL100", "3500", "1500AGL", "UNL" -> in 100 ft
static int limitToFt100(const char *s) {
  if (!strncmp(s, "GND", 3)) return 0;
  if (!strncmp(s, "UNL", 3)) return 9999;
  if (!strncmp(s, "FL", 2))  return atoi(s + 2);
  return (atoi(s) + 50) / 100;
}

// zerlegt "LABEL|von|bis" an Ort und Stelle
static void parseAsName(char *nm, int &lo, int &hi, const char *&label) {
  lo = 0; hi = 9999; label = nm;
  char *p2 = strrchr(nm, '|');
  if (!p2) return;
  *p2 = 0; hi = limitToFt100(p2 + 1);
  char *p1 = strrchr(nm, '|');
  if (!p1) return;
  *p1 = 0; lo = limitToFt100(p1 + 1);
}

static void fmtLimit(char *o, size_t n, int v) {
  if (v <= 0)         snprintf(o, n, "GND");
  else if (v >= 9999) snprintf(o, n, "UNL");
  else if (v >= 100)  snprintf(o, n, "FL%d", v);
  else                snprintf(o, n, "%d00", v);
}

static void drawAsLabel(const VmapRecord &r, const char *label, int lo, int hi, uint16_t col) {
  float px, py;
  project((r.minLat + r.maxLat) / 2, (r.minLon + r.maxLon) / 2, px, py);
  if (px < 35 || px > 205 || py < 35 || py > 205) return;
  char a[12], b[12], lim[28], lab[16];
  fmtLimit(a, sizeof a, lo);
  fmtLimit(b, sizeof b, hi);
  snprintf(lim, sizeof lim, "%s-%s", a, b);
  strlcpy(lab, label, sizeof lab);
  canvas.setFont(&fonts::Font0);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(col);
  canvas.drawString(lab, (int)lroundf(px), (int)lroundf(py) - 5);
  canvas.drawString(lim, (int)lroundf(px), (int)lroundf(py) + 5);
  canvas.setFont(&fonts::Font2);          // Zustand fuer Orte/Flugplaetze wiederherstellen
  canvas.setTextDatum(top_left);
}

static bool renderMapFile(const String &path) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  VmapHeader h;
  if (f.read((uint8_t *)&h, sizeof h) != sizeof h) return false;
  canvas.setFont(&fonts::Font2);
  canvas.setTextDatum(top_left);
  for (uint32_t i = 0; i < h.numRecords; i++) {
    VmapRecord r;
    if (f.read((uint8_t *)&r, sizeof r) != sizeof r) break;
    size_t base = f.position();
    size_t dataBytes = (size_t)r.nameLen + (size_t)r.npts * 8;
    bool visible = false;
    if (r.type < 8)                visible = V.mpp <= LINE_STYLE[r.type].maxMpp;
    else if (r.type >= 10 && r.type <= 12) visible = V.mpp <= PLACE_MAXMPP[r.type - 10];
    else if (r.type == 13 || r.type == 14)  visible = V.mpp <= AIRPORT_MAXMPP[r.type - 13];
    else if (r.type >= 20 && r.type <= 27)  visible = cfg.asLevel > 0 && (cfg.asMask & (1 << (r.type - 20))) && V.mpp <= AS_STYLE[r.type - 20].maxMpp;
    if (visible && (r.maxLat < V.vMinLat || r.minLat > V.vMaxLat || r.maxLon < V.vMinLon || r.minLon > V.vMaxLon))
      visible = false;
    if (!visible) { f.seek(base + dataBytes); continue; }
    if (r.type >= 20 && r.type <= 27) {                       // Luftraum
      char nm[64];
      uint8_t nl = r.nameLen > 63 ? 63 : r.nameLen;
      f.read((uint8_t *)nm, nl);
      nm[nl] = 0;
      int lo, hi;
      const char *label;
      parseAsName(nm, lo, hi, label);
      if (lo > AS_LEVEL_FT100[cfg.asLevel]) { f.seek(base + dataBytes); continue; }
      const LineStyle &st = AS_STYLE[r.type - 20];
      f.seek(base + r.nameLen);
      drawRecordLines(f, r, st);
      if (V.mpp <= AS_LABEL_MAXMPP) drawAsLabel(r, label, lo, hi, st.color);
      continue;
    }
    if (r.type >= 10) {
      char nm[32];
      uint8_t nl = r.nameLen > 31 ? 31 : r.nameLen;
      f.read((uint8_t *)nm, nl);
      nm[nl] = 0;
      drawPlace(r, nm);
      f.seek(base + dataBytes);
    } else {
      f.seek(base + r.nameLen);
      drawRecordLines(f, r, LINE_STYLE[r.type]);
    }
  }
  return true;
}

// Grosskreis: Entfernung in NM und Peilung (rechtweisend) von (lat1,lon1) nach (lat2,lon2)
static void dctCalc(double lat1, double lon1, double lat2, double lon2, float &nm, float &brg) {
  double p1 = lat1 * M_PI / 180.0, p2 = lat2 * M_PI / 180.0, dl = (lon2 - lon1) * M_PI / 180.0;
  double sa = sin((p2 - p1) / 2), sb = sin(dl / 2);
  double a = sa * sa + cos(p1) * cos(p2) * sb * sb;
  nm = (float)(2.0 * asin(sqrt(a)) * 6371000.0 / 1852.0);
  double y = sin(dl) * cos(p2), x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
  brg = (float)fmod(atan2(y, x) * 180.0 / M_PI + 360.0, 360.0);
}

static const uint16_t C_DCT = RGB565(255, 0, 255);

static void drawDctLine(float ox, float oy) {
  float tx, ty;
  project(g_dctLat, g_dctLon, tx, ty);
  float x0 = ox, y0 = oy, x1 = tx, y1 = ty;
  if (clipLine(x0, y0, x1, y1)) {
    int ax = (int)lroundf(x0), ay = (int)lroundf(y0), bx = (int)lroundf(x1), by = (int)lroundf(y1);
    canvas.drawLine(ax, ay, bx, by, C_DCT);
    if (fabsf(x1 - x0) > fabsf(y1 - y0)) canvas.drawLine(ax, ay + 1, bx, by + 1, C_DCT);
    else                                 canvas.drawLine(ax + 1, ay, bx + 1, by, C_DCT);
  }
  if (tx >= 0 && tx < 240 && ty >= 0 && ty < 240) {                 // Zielmarke
    canvas.drawCircle((int)lroundf(tx), (int)lroundf(ty), 8, C_DCT);
    canvas.drawCircle((int)lroundf(tx), (int)lroundf(ty), 9, C_DCT);
  }
}

static void drawDctInfo(float nm, float brg, const GpsData &g, bool fixOk) {
  char b[48], d[12];
  if (nm >= 100) snprintf(d, sizeof d, "%.0f", nm); else snprintf(d, sizeof d, "%.1f", nm);
  int hdg = ((int)lroundf(brg)) % 360;
  if (fixOk && g.speedKn >= 20.0f)
    snprintf(b, sizeof b, "%s %03d %sNM ETE %dm", cfg.dct.c_str(), hdg, d, (int)(nm / g.speedKn * 60.0f + 0.5f));
  else
    snprintf(b, sizeof b, "%s %03d %sNM", cfg.dct.c_str(), hdg, d);
  canvas.setFont(&fonts::Font0);
  canvas.setTextDatum(middle_center);
  canvas.fillRoundRect(36, 66, 168, 16, 6, C_BLACK);
  canvas.setTextColor(C_DCT);
  canvas.drawString(b, 120, 74);
}

static void drawTraffic(uint32_t now, float ox, float oy) {
  if (!cfg.traffic) { g_trafCount = 0; return; }
  Traffic t[MAX_TRAF];
  xSemaphoreTake(g_gpsMutex, portMAX_DELAY);
  memcpy(t, g_traf, sizeof t);
  xSemaphoreGive(g_gpsMutex);
  float psi = cfg.headingUp ? s_heading : 0.0f;
  int cnt = 0;
  bool alarmNow = false;
  canvas.setFont(&fonts::Font0);
  canvas.setTextDatum(top_left);
  for (int i = 0; i < MAX_TRAF; i++) {
    const Traffic &a = t[i];
    if (!a.lastMs || now - a.lastMs > TRAFFIC_TIMEOUT_MS) continue;
    cnt++;
    float u = (a.east * V.cosP - a.north * V.sinP) / V.mpp;
    float v = (a.east * V.sinP + a.north * V.cosP) / V.mpp;
    int x = (int)lroundf(ox + u), y = (int)lroundf(oy - v);
    if (x < -10 || x > 250 || y < -10 || y > 250) continue;
    uint16_t col = a.alarm >= 2 ? C_RED : (a.alarm == 1 ? C_ORANGE : C_TRAFFIC);
    if (a.alarm >= 2) {
      alarmNow = true;
      canvas.drawCircle(x, y, 11, C_RED);
      canvas.drawCircle(x, y, 12, C_RED);
    }
    if (a.trackOk) {
      float ang = (a.track - psi) * DEG2RAD;
      float ca = cosf(ang), sa = sinf(ang);
      const float pt[4][2] = { { 0, -8 }, { -5, 6 }, { 0, 3 }, { 5, 6 } };
      int X[4], Y[4];
      for (int k = 0; k < 4; k++) {
        X[k] = (int)lroundf(x + pt[k][0] * ca - pt[k][1] * sa);
        Y[k] = (int)lroundf(y + pt[k][0] * sa + pt[k][1] * ca);
      }
      canvas.fillTriangle(X[0], Y[0], X[1], Y[1], X[2], Y[2], col);
      canvas.fillTriangle(X[0], Y[0], X[2], Y[2], X[3], Y[3], col);
      for (int k = 0; k < 4; k++) canvas.drawLine(X[k], Y[k], X[(k + 1) % 4], Y[(k + 1) % 4], C_BLACK);
    } else {
      canvas.fillCircle(x, y, 4, col);          // Richtung unbekannt
    }
    if (V.mpp <= 400) {                         // relative Hoehe in 100 ft, ^/v = steigt/sinkt
      char lb[12];
      int h = (int)lroundf(a.vert * 3.28084f / 100.0f);
      snprintf(lb, sizeof lb, "%+d%s", h, a.climb > 1.0f ? "^" : (a.climb < -1.0f ? "v" : ""));
      canvas.setTextColor(col);
      canvas.drawString(lb, x + 8, y - 4);
    }
  }
  g_trafCount = cnt;
  if (alarmNow && now - g_lastAlarmBeep > 4000) {
    g_lastAlarmBeep = now;
    M5Dial.Speaker.tone(2500, 150);
  }
}

static void drawMarker(bool fixOk, float mx, float my) {
  float ang = cfg.headingUp ? 0.0f : s_heading * DEG2RAD;
  float ca = cosf(ang), sa = sinf(ang);
  const float pt[4][2] = { { 0, -12 }, { -8, 9 }, { 0, 4 }, { 8, 9 } };
  int X[4], Y[4];
  for (int i = 0; i < 4; i++) {
    X[i] = (int)lroundf(mx + pt[i][0] * ca - pt[i][1] * sa);
    Y[i] = (int)lroundf(my + pt[i][0] * sa + pt[i][1] * ca);
  }
  uint16_t col = fixOk ? C_MARK : C_NOFIX;
  canvas.fillTriangle(X[0], Y[0], X[1], Y[1], X[2], Y[2], col);
  canvas.fillTriangle(X[0], Y[0], X[2], Y[2], X[3], Y[3], col);
  for (int i = 0; i < 4; i++) canvas.drawLine(X[i], Y[i], X[(i + 1) % 4], Y[(i + 1) % 4], C_WHITE);
  if (!fixOk) canvas.drawCircle((int)mx, (int)my, 16, C_NOFIX);
}

static void drawScaleBar(float mpp) {
  float target = mpp * 60.0f;
  float p = powf(10.0f, floorf(log10f(target)));
  float nice = p;
  if (target >= 5 * p) nice = 5 * p;
  else if (target >= 2 * p) nice = 2 * p;
  int px = (int)(nice / mpp + 0.5f);
  char lab[16];
  if (nice >= 1000) snprintf(lab, sizeof lab, "%g km", nice / 1000.0f);
  else snprintf(lab, sizeof lab, "%d m", (int)nice);
  canvas.setTextColor(C_WHITE);
  canvas.setFont(&fonts::Font2);
  canvas.setTextDatum(middle_center);
  canvas.drawString(lab, 120, 200);
  int x0 = 120 - px / 2, x1 = x0 + px;
  canvas.drawLine(x0, 215, x1, 215, C_WHITE);
  canvas.drawLine(x0, 211, x0, 219, C_WHITE);
  canvas.drawLine(x1, 211, x1, 219, C_WHITE);
}

static void drawHud(const GpsData &g, bool fixOk, uint32_t now) {
  // Geschwindigkeit oben
  canvas.fillRoundRect(55, 12, 130, 32, 10, C_BLACK);
  char sp[24];
  const char *un = cfg.units ? "kts" : "km/h";
  if (fixOk) snprintf(sp, sizeof sp, "%d %s", (int)lroundf(cfg.units ? g.speedKn : g.speedKn * 1.852f), un);
  else       snprintf(sp, sizeof sp, "-- %s", un);
  canvas.setFont(&fonts::Font4);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(C_WHITE);
  canvas.drawString(sp, 120, 29);

  // Status + Massstab unten
  canvas.fillRoundRect(64, 172, 112, 52, 10, C_BLACK);
  const char *st;
  uint16_t sc;
  char buf[24];
  if (WiFi.status() != WL_CONNECTED)      { st = "kein WLAN";    sc = C_RED; }
  else if (!g_tcpUp)                      { st = "kein TCP";     sc = C_ORANGE; }
  else if (now - g.lastAnyMs > 5000)      { st = "keine Daten";  sc = C_ORANGE; }
  else if (!fixOk)                        { st = "kein Fix";     sc = C_YELLOW; }
  else {
    if (cfg.traffic && g_trafCount > 0) snprintf(buf, sizeof buf, "%u Sat  T%d", (unsigned)g.sats, g_trafCount);
    else snprintf(buf, sizeof buf, "%u Sat", (unsigned)g.sats);
    st = buf; sc = C_GREEN;
  }
  canvas.setFont(&fonts::Font2);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(sc);
  canvas.drawString(st, 120, 184);
  drawScaleBar(V.mpp);

  // Nordmarke im Kurs-oben-Modus
  if (cfg.headingUp) {
    float a = s_heading * DEG2RAD;
    int nx = (int)lroundf(120 - 106 * sinf(a));
    int ny = (int)lroundf(120 - 106 * cosf(a));
    canvas.fillCircle(nx, ny, 9, C_RED);
    canvas.setTextColor(C_WHITE);
    canvas.drawString("N", nx, ny);
  }
}

// Fadenkreuz + Koordinaten der Kartenmitte, solange die Karte verschoben ist
static void drawPanInfo(double lat, double lon) {
  int cx = (int)V.cx, cy = (int)V.cy;
  canvas.drawLine(cx - 8, cy, cx + 8, cy, C_YELLOW);
  canvas.drawLine(cx, cy - 8, cx, cy + 8, C_YELLOW);
  char b[40];
  snprintf(b, sizeof b, "%.3f%c  %.3f%c", fabs(lat), lat >= 0 ? 'N' : 'S', fabs(lon), lon >= 0 ? 'E' : 'W');
  canvas.setFont(&fonts::Font0);
  canvas.setTextDatum(middle_center);
  canvas.fillRoundRect(48, 48, 144, 16, 6, C_BLACK);
  canvas.setTextColor(C_YELLOW);
  canvas.drawString(b, 120, 56);
  canvas.fillRoundRect(52, 152, 136, 16, 6, C_BLACK);
  canvas.setTextColor(C_GREY);
  canvas.drawString("Tippen = zur Position", 120, 160);
}

void drawMapScreen(const GpsData &g) {
  uint32_t now = millis();
  bool fixOk = g.fix && (now - g.lastPosMs < 5000);
  double cLat = cfg.lastLat, cLon = cfg.lastLon;
  if (fixOk) {
    cLat = g.lat;
    cLon = g.lon;
    cfg.lastLat = cLat;
    cfg.lastLon = cLon;
    if (g.courseValid && g.speedKn >= 1.0f) s_heading = g.course;
  }
  double ownLat = cLat, ownLon = cLon;
  if (g_panned) { cLat = g_panLat; cLon = g_panLon; }
  setupView(cLat, cLon, ZOOM_MPP[cfg.zoom], cfg.headingUp ? s_heading : 0.0f);
  float ox = V.cx, oy = V.cy;                 // Bildschirmposition des eigenen Flugzeugs
  if (g_panned) project((int32_t)lround(ownLat * 1e5), (int32_t)lround(ownLon * 1e5), ox, oy);
  canvas.fillScreen(C_BG);
  bool drawn = g_mapOk && renderMapFile("/" + cfg.map);
  for (auto &ov : g_overlays) renderMapFile("/" + ov);
  if (!drawn) {
    canvas.setTextDatum(middle_center);
    canvas.setFont(&fonts::Font4);
    canvas.setTextColor(C_GREEN);
    canvas.drawString("Keine Karte", 120, 88);
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(C_GREY);
    canvas.drawString("Menue > WLAN -> AP", 120, 114);
    canvas.drawString("und .vmap hochladen", 120, 132);
  }
  float dNm = 0, dBrg = 0;
  bool dctOn = g_dctOk && cfg.dct.length();
  if (dctOn) {
    dctCalc(ownLat, ownLon, g_dctLat * 1e-5, g_dctLon * 1e-5, dNm, dBrg);
    drawDctLine(ox, oy);
  }
  drawTraffic(now, ox, oy);
  drawMarker(fixOk, ox, oy);
  drawHud(g, fixOk, now);
  if (dctOn) drawDctInfo(dNm, dBrg, g, fixOk);
  if (g_panned) drawPanInfo(cLat, cLon);
  canvas.pushSprite(&M5Dial.Display, 0, 0);
  g_mapDirty = false;
  g_lastSeq = g.seq;
  g_lastTrafSeq = g_trafSeq;
  g_lastDraw = now;
}

// ------------------------------- Menue / Screens ----------------------------
String menuLabel(int i) {
  switch (i) {
    case 0: return "Zurueck";
    case 1: return "Karte";
    case 2: return "Ansicht";
    case 3: return "Einheit";
    case 4: return "Direct-To";
    case 5: return "Traffic";
    case 6: return "LR-Hoehe";
    case 7: return "LR-Typen";
    case 8: return wifiMode == MODE_CLIENT ? "WLAN -> AP" : "WLAN -> Client";
    case 9: return "Info";
    case 10: return "Karten-Stand";
    default: return "AP-Zugang";
  }
}

String menuValue(int i) {
  switch (i) {
    case 0: return "zur Karte";
    case 1: {
      if (!cfg.map.length()) return "keine Karte";
      String n = cfg.map;
      n.replace(".vmap", "");
      return fitStr(n, 20);
    }
    case 2: return cfg.headingUp ? "Kurs oben" : "Nord oben";
    case 3: return cfg.units ? "Knoten (kts)" : "km/h";
    case 4: return cfg.dct.length() ? (g_dctOk ? cfg.dct : cfg.dct + " (?)") : String("kein Ziel");
    case 5: return cfg.traffic ? "an (PFLAA)" : "aus";
    case 6: return String(AS_LEVEL_TXT[cfg.asLevel]);
    case 7: {
      int n = 0;
      for (int k = 0; k < 8; k++) if (cfg.asMask & (1 << k)) n++;
      return String(n) + " von 8 aktiv";
    }
    case 8: return wifiMode == MODE_CLIENT ? "jetzt: Client (GPS)" : "jetzt: Access Point";
    case 9: return "IP, Speicher, Status";
    case 10: return "Datum der Karten";
    default: return g_apSsid;
  }
}

void drawMenu() {
  canvas.fillScreen(C_BG);
  canvas.setTextDatum(middle_center);
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(C_GREY);
  canvas.drawString("MENUE", 120, 24);
  canvas.setTextColor(RGB565(110, 120, 135));
  canvas.drawString(menuLabel((menuSel + MENU_COUNT - 1) % MENU_COUNT), 120, 70);
  canvas.drawString(menuLabel((menuSel + 1) % MENU_COUNT), 120, 176);
  canvas.fillRoundRect(14, 92, 212, 58, 12, C_SELBG);
  canvas.setFont(&fonts::Font4);
  canvas.setTextColor(C_WHITE);
  canvas.drawString(menuLabel(menuSel), 120, 111);
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(RGB565(170, 200, 255));
  canvas.drawString(menuValue(menuSel), 120, 136);
  canvas.setTextColor(C_GREY);
  canvas.drawString("Drehen / Klick", 120, 214);
  if (g_notice.length()) {
    canvas.fillRoundRect(30, 96, 180, 48, 10, C_BLACK);
    canvas.setTextColor(C_GREEN);
    canvas.setFont(&fonts::Font2);
    canvas.drawString(g_notice, 120, 120);
  }
  canvas.pushSprite(&M5Dial.Display, 0, 0);
  g_uiDirty = false;
}

void drawAsTypes() {
  canvas.fillScreen(C_BG);
  canvas.setFont(&fonts::Font2);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(C_GREY);
  canvas.drawString("LUFTRAUM-TYPEN", 120, 22);
  for (int off = -2; off <= 2; off++) {
    int i = ((asSel + off) % 9 + 9) % 9;
    int y = 120 + off * 34;
    if (off == 0) canvas.fillRoundRect(18, y - 16, 204, 32, 8, C_SELBG);
    canvas.setTextDatum(middle_left);
    if (i < 8) {
      bool on = cfg.asMask & (1 << i);
      uint16_t col = AS_STYLE[i].color;
      canvas.fillRect(32, y - 5, 10, 10, on ? col : C_BG);
      canvas.drawRect(32, y - 5, 10, 10, col);
      canvas.setTextColor(off == 0 ? C_WHITE : (on ? C_GREY : RGB565(80, 88, 100)));
      canvas.drawString(AS_TYPE_NAME[i], 50, y);
      canvas.setTextDatum(middle_right);
      canvas.drawString(on ? "AN" : "AUS", 208, y);
    } else {
      canvas.setTextColor(off == 0 ? C_WHITE : C_GREY);
      canvas.drawString(AS_TYPE_NAME[8], 50, y);
    }
  }
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(C_GREY);
  canvas.drawString("Klick = umschalten", 120, 224);
  canvas.pushSprite(&M5Dial.Display, 0, 0);
  g_uiDirty = false;
}

static void buildDctCandsOnce() {
  dctN = 0;
  if (dctStep == 0 && cfg.dct.length()) dctCand[dctN++] = '-';          // Ziel loeschen
  char last = 0;
  for (auto &a : g_apts) {
    if (strncmp(a.id, dctPrefix, dctStep) != 0) continue;
    if (a.id[dctStep] != last && dctN < 27) { last = a.id[dctStep]; dctCand[dctN++] = last; }
  }
  dctIdx = (dctN > 1 && dctCand[0] == '-') ? 1 : 0;
}

// Schritte mit nur einer Moeglichkeit (z.B. immer 'E' am Anfang) werden uebersprungen
static void buildDctCands() {
  buildDctCandsOnce();
  while (dctN == 1 && dctCand[0] != '-' && dctStep < 3) {
    dctPrefix[dctStep] = dctCand[0];
    dctStep++;
    dctPrefix[dctStep] = 0;
    buildDctCandsOnce();
  }
}

static void dctComplete(const Apt &a) {
  cfg.dct = String(a.id);
  g_dctOk = true;
  g_dctLat = a.lat;
  g_dctLon = a.lon;
  prefs.putString("dct", cfg.dct);
  ui = UI_MAP;
  markAllDirty();
}

static void dctClick() {
  if (!dctN) { ui = UI_MENU; g_uiDirty = true; return; }
  char c = dctCand[dctIdx];
  if (c == '-') {
    cfg.dct = "";
    g_dctOk = false;
    prefs.putString("dct", "");
    ui = UI_MAP;
    markAllDirty();
    return;
  }
  dctPrefix[dctStep] = c;
  dctPrefix[dctStep + 1] = 0;
  dctStep++;
  int matches = 0;
  const Apt *only = nullptr;
  for (auto &a : g_apts) if (!strncmp(a.id, dctPrefix, dctStep)) { matches++; only = &a; }
  if (only && (dctStep == 4 || matches == 1)) { dctComplete(*only); return; }   // eindeutig -> fertig
  buildDctCands();
  g_uiDirty = true;
}

void drawDct() {
  canvas.fillScreen(C_BG);
  canvas.setFont(&fonts::Font2);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(C_GREY);
  canvas.drawString("DIRECT-TO", 120, 22);
  if (g_apts.empty() || !dctN) {
    canvas.setTextColor(C_GREEN);
    canvas.drawString("Keine Flugplaetze", 120, 100);
    canvas.setTextColor(C_GREY);
    canvas.drawString("ovl_flugplaetze.vmap", 120, 124);
    canvas.drawString("hochladen", 120, 142);
    canvas.pushSprite(&M5Dial.Display, 0, 0);
    g_uiDirty = false;
    return;
  }
  char cur = dctCand[dctIdx];
  canvas.setFont(&fonts::Font4);
  for (int i = 0; i < 4; i++) {
    int x = 120 + (int)((i - 1.5f) * 30);
    char b[2] = { '_', 0 };
    uint16_t col = C_GREY;
    if (i < dctStep)       { b[0] = dctPrefix[i]; col = C_WHITE; }
    else if (i == dctStep) { b[0] = cur; col = C_GREEN; canvas.fillRoundRect(x - 14, 78, 28, 40, 6, C_SELBG); }
    canvas.setTextColor(col);
    canvas.drawString(b, x, 98);
  }
  canvas.setFont(&fonts::Font2);
  for (int off = -3; off <= 3; off++) {                       // Kandidaten-Zeile
    int k = dctIdx + off;
    if (k < 0 || k >= dctN) continue;
    char b[2] = { dctCand[k], 0 };
    canvas.setTextColor(off == 0 ? C_GREEN : C_GREY);
    canvas.drawString(b, 120 + off * 26, 146);
  }
  char info[40] = "";
  if (cur == '-') {
    snprintf(info, sizeof info, "Ziel %s loeschen", cfg.dct.c_str());
  } else {
    int n = 0;
    const Apt *one = nullptr;
    for (auto &a : g_apts)
      if (!strncmp(a.id, dctPrefix, dctStep) && a.id[dctStep] == cur) { n++; one = &a; }
    if (n == 1 && one) {
      float nm, brg;
      dctCalc(cfg.lastLat, cfg.lastLon, one->lat * 1e-5, one->lon * 1e-5, nm, brg);
      snprintf(info, sizeof info, "%s  %.0f NM  %03d", one->id, nm, ((int)lroundf(brg)) % 360);
    } else {
      snprintf(info, sizeof info, "%d Plaetze", n);
    }
  }
  canvas.setTextColor(C_WHITE);
  canvas.drawString(info, 120, 182);
  canvas.setTextColor(C_GREY);
  canvas.drawString("Klick=weiter  Halten=zurueck", 120, 218);
  canvas.pushSprite(&M5Dial.Display, 0, 0);
  g_uiDirty = false;
}

static uint8_t infoPage = 0;     // 0 Info, 1 Karten-Stand, 2 AP-Zugang

static void drawInfoHeader(const char *title, const char *sub) {
  canvas.fillScreen(C_BG);
  canvas.setTextDatum(middle_center);
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(C_GREEN);
  canvas.drawString(title, 120, 18);
  canvas.setTextColor(C_GREY);
  canvas.drawString(sub, 120, 35);
}

// Karten-Stand: Erstelldatum jeder .vmap im Speicher
static void drawMapDates() {
  drawInfoHeader("KARTEN-STAND", "Erstellt am");
  auto v = listMaps();
  for (auto &o : g_overlays) v.push_back(o);
  canvas.setFont(&fonts::Font2);
  int y = 62;
  for (size_t i = 0; i < v.size() && i < 8; i++, y += 20) {
    String n = v[i];
    n.replace(".vmap", "");
    bool act = (v[i] == cfg.map);
    canvas.setTextColor(act ? C_GREEN : C_WHITE);
    canvas.drawString(fitStr(n, 13) + " " + fmtDate(vmapDate(v[i])), 120, y);
  }
  if (v.empty()) { canvas.setTextColor(C_GREY); canvas.drawString("keine Karten", 120, 110); }
  canvas.setTextColor(C_GREY);
  canvas.drawString("Klick = zurueck", 120, 222);
  canvas.pushSprite(&M5Dial.Display, 0, 0);
  g_uiDirty = false;
}

// AP-Zugang: SSID, Passwort (= MAC), QR-Code
static void drawApCreds() {
  drawInfoHeader("AP-ZUGANG", "WLAN beim Wechsel auf AP");
  String pwq = g_apPass;
  pwq.replace(":", "\\:");
  canvas.qrcode(String("WIFI:T:WPA;S:") + g_apSsid + ";P:" + pwq + ";;", 70, 46, 100);
  canvas.setTextColor(C_WHITE);
  canvas.drawString("SSID: " + g_apSsid, 120, 162);
  canvas.drawString("PW: " + g_apPass, 120, 182);
  canvas.setTextColor(C_GREY);
  canvas.drawString("Klick = zurueck", 120, 222);
  canvas.pushSprite(&M5Dial.Display, 0, 0);
  g_uiDirty = false;
}

void drawInfo() {
  if (infoPage == 1) { drawMapDates(); return; }
  if (infoPage == 2) { drawApCreds(); return; }
  canvas.fillScreen(C_BG);
  canvas.setTextDatum(middle_center);
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(C_GREEN);
  canvas.drawString(APP_NAME, 120, 18);
  canvas.setTextColor(C_GREY);
  canvas.drawString("Version " APP_VERSION, 120, 35);
  canvas.setTextColor(C_WHITE);
  String l[8];
  if (wifiMode == MODE_CLIENT) {
    l[0] = "STA: " + fitStr(cfg.staSsid, 16);
    l[1] = "IP: " + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("-"));
    l[2] = fitStr(cfg.host + ":" + String(cfg.port), 21);
    l[3] = g_tcpUp ? "TCP: verbunden" : "TCP: getrennt";
  } else {
    l[0] = "AP: " + fitStr(g_apSsid, 17);
    l[1] = "PW: " + g_apPass;
    l[2] = "IP: " + WiFi.softAPIP().toString() + " (" + String(WiFi.softAPgetStationNum()) + ")";
    l[3] = "";
  }
  l[4] = "Karte: " + (cfg.map.length() ? fitStr(cfg.map, 14) : String("-"));
  l[5] = "Flash frei: " + String((LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024) + " KB";
  l[6] = "RAM frei: " + String(ESP.getFreeHeap() / 1024) + " KB";
  l[7] = "Overlays: " + String((int)g_overlays.size());
  for (int i = 0; i < 8; i++) canvas.drawString(l[i], 120, 56 + i * 17);
  canvas.setTextColor(C_TRAFFIC);
  canvas.drawString(APP_URL, 120, 195);
  canvas.setTextColor(C_GREY);
  canvas.drawString("Klick = zurueck", 120, 216);
  canvas.pushSprite(&M5Dial.Display, 0, 0);
  g_uiDirty = false;
}

void drawApScreen() {
  canvas.fillScreen(C_BG);
  canvas.setTextDatum(middle_center);
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(g_uploading ? C_ORANGE : C_GREEN);
  canvas.drawString(g_uploading ? "UPLOAD LAEUFT..." : "ACCESS POINT", 120, 18);
  String pwq = g_apPass;
  pwq.replace(":", "\\:");
  String qr = String("WIFI:T:WPA;S:") + g_apSsid + ";P:" + pwq + ";;";
  canvas.qrcode(qr, 70, 30, 100);
  canvas.setTextColor(C_WHITE);
  canvas.drawString(String("SSID: ") + g_apSsid, 120, 146);
  canvas.drawString(String("PW: ") + g_apPass, 120, 163);
  canvas.setTextColor(C_GREEN);
  canvas.drawString("http://" + WiFi.softAPIP().toString(), 120, 182);
  canvas.setTextColor(C_GREY);
  auto maps = listMaps();
  String s2 = String(maps.size()) + " Karte(n), " + String((LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024) + " KB frei";
  canvas.drawString(fitStr(s2, 22), 120, 202);
  canvas.drawString("Klick = Menue", 120, 220);
  canvas.pushSprite(&M5Dial.Display, 0, 0);
  g_apDirty = false;
  g_lastDraw = millis();
}

// ----------------------------------- Web-GUI ---------------------------------
static const char PAGE[] PROGMEM = R"rawliteral(<!doctype html>
<html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Micro-NAV Karten</title>
<style>
body{font-family:system-ui,sans-serif;background:#0b1018;color:#e6e9ef;margin:0 auto;padding:16px;max-width:640px}
h1{font-size:20px}h2{font-size:15px;margin:22px 0 8px;color:#9fb0c8}
.card{background:#131b27;border-radius:10px;padding:12px}
table{width:100%;border-collapse:collapse}td{padding:7px 4px;border-bottom:1px solid #223}
button{background:#2d6cdf;color:#fff;border:0;border-radius:6px;padding:7px 12px;font-size:14px}
button.d{background:#8a2c2c}button.s{background:#2a7d46}
input{background:#0b1018;color:#e6e9ef;border:1px solid #334;border-radius:6px;padding:8px;width:100%;box-sizing:border-box;margin:3px 0 8px}
#barw{background:#223;border-radius:5px;height:10px;margin-top:8px}#bar{background:#2d6cdf;height:10px;border-radius:5px;width:0}
progress{width:100%;margin:8px 0}small{color:#8fa0b8}
</style></head><body>
<h1>Micro-NAV &ndash; Karten-Manager <small>v)rawliteral" APP_VERSION R"rawliteral(</small></h1>
<div class="card"><div id="fs">Lade ...</div><div id="barw"><div id="bar"></div></div></div>
<h2>Karten auf dem Ger&auml;t</h2>
<div class="card"><table id="files"></table></div>
<h2>Karte hochladen (.vmap)</h2>
<div class="card"><input type="file" id="file" accept=".vmap" multiple>
<progress id="prog" value="0" max="100"></progress>
<button onclick="upload()">Hochladen</button><div id="msg"></div></div>
<h2>Karten selbst erzeugen (Konverter)</h2>
<!--KONV-HTML-BEGIN-->
<style>
#kv .kl{border-top:1px solid #223;padding:8px 0}#kv .kr{display:flex;gap:6px;flex-wrap:wrap;align-items:center}
#kv .kr>*{flex:1 1 100px;min-width:0}#kv select{background:#0b1018;color:#e6e9ef;border:1px solid #334;border-radius:6px;padding:8px}
#kv .kr input[type=checkbox]{flex:0 0 auto;width:auto;margin:0}#kv .kn{flex:2 1 160px;font-size:13px;word-break:break-all}
#kv pre{background:#0b1018;border-radius:6px;padding:8px;font-size:12px;white-space:pre-wrap;max-height:220px;overflow:auto}
#kprev{width:100%;background:#080c14;border-radius:6px;margin-top:8px}
</style>
<div class="card" id="kv">
<small>Quelldaten w&auml;hlen: <b>GeoJSON</b> (z.&nbsp;B. Natural Earth 10m), <b>airports.csv</b> (ourairports.com/data), <b>OpenAir-.txt</b> oder die <b>.zip</b> von OpenFlightMaps. Die Umwandlung l&auml;uft komplett im Browser, auch ohne Internet &ndash; nur die Quelldateien m&uuml;ssen vorliegen. Passende Voreinstellungen werden automatisch gesetzt (Anleitung: README.md).</small>
<input type="file" id="kfiles" multiple accept=".geojson,.json,.csv,.txt,.zip">
<div id="klayers"></div>
<div class="kr"><span>BBox <small>(W S O N, leer = alles)</small><input id="kbb" value="5.0 46.8 16.5 55.8"></span>
<span>Standard-Toleranz <small>(m)</small><input id="ktol" type="number" value="1000" min="0"></span>
<span>Dateiname<input id="kname" value="karte.vmap"></span></div>
<button onclick="kConvert()">Konvertieren</button> <button class="s" id="kdl" style="display:none">Herunterladen</button> <button class="s" id="kup" style="display:none">Auf das Ger&auml;t hochladen</button>
<progress id="kprog" value="0" max="100" style="display:none"></progress>
<pre id="klog" style="display:none"></pre>
<canvas id="kprev" width="480" height="480" style="display:none"></canvas>
</div>
<!--KONV-HTML-END-->
<h2>Firmware aktualisieren</h2>
<div class="card"><input type="file" id="fwfile" accept=".bin">
<progress id="fwprog" value="0" max="100"></progress>
<button class="d" onclick="fw()">Firmware installieren</button> <span id="fwmsg"></span>
<p><small>Nur die Datei <b>GS-Micro-NAV.ino.bin</b> (Arduino: Sketch &rarr; Kompilierte Bin&auml;rdatei exportieren), nicht .merged/.bootloader/.partitions. W&auml;hrend des Updates das Ger&auml;t nicht ausschalten.</small></p></div>
<h2>WLAN-Client (GPS-Host)</h2>
<div class="card"><form onsubmit="return save(event)">
SSID<input id="ssid" maxlength="32">
Passwort <small>(leer = unver&auml;ndert, "-" = l&ouml;schen)</small><input id="pass" type="password" maxlength="63">
Host (NMEA-TCP)<input id="host" maxlength="63">
Port<input id="port" type="number" min="1" max="65535">
<button type="submit">Speichern</button> <span id="smsg"></span></form></div>
<p><small>Dateien <b>ovl_*.vmap</b> (z.&nbsp;B. ovl_luftraum.vmap) werden als Overlay &uuml;ber die aktive Karte gelegt. Luftraumdaten sind nur ein Lagebild &ndash; nicht zur Navigation verwenden!</small></p>
<p><small>GS-Avionik &middot; gs-avionik.de</small></p>
<p><small>Karten erzeugen: Konverter oben (Anleitung: README.md auf GitHub); das Erstelldatum steht in jeder Datei. Zur&uuml;ck in den Client-Modus: Men&uuml; am M5Dial &rarr; &bdquo;WLAN &rarr; Client&ldquo;.</small></p>
<script>
const $=s=>document.querySelector(s);
const fd=d=>String(d%100).padStart(2,'0')+'.'+String(Math.floor(d/100)%100).padStart(2,'0')+'.'+Math.floor(d/10000);
const fmt=n=>n>=1048576?(n/1048576).toFixed(2)+' MB':(n/1024).toFixed(1)+' KB';
async function post(u,o){const r=await fetch(u,{method:'POST',body:new URLSearchParams(o)});return r.text();}
async function load(){
 const s=await(await fetch('/api/status')).json();
 $('#fs').textContent='Speicher: '+fmt(s.used)+' von '+fmt(s.total)+' belegt';
 $('#bar').style.width=(100*s.used/s.total)+'%';
 const t=$('#files');t.innerHTML='';
 if(!s.files.length)t.innerHTML='<tr><td>Keine Karten vorhanden</td></tr>';
 s.files.forEach(f=>{
  const tr=t.insertRow();
  const ov=f.name.startsWith('ovl_');
  tr.insertCell().textContent=(f.name==s.active?'▶ ':'')+f.name+(ov?' (Overlay)':'');
  tr.insertCell().textContent=fmt(f.size)+(f.date?' \u00b7 Stand '+fd(f.date):'');
  const c=tr.insertCell();c.style.textAlign='right';
  const a=document.createElement('button');a.textContent='Aktiv';a.className='s';
  a.onclick=async()=>{await post('/select',{name:f.name});load();};
  const d=document.createElement('button');d.textContent='Löschen';d.className='d';d.style.marginLeft='6px';
  d.onclick=async()=>{if(confirm(f.name+' löschen?')){await post('/delete',{name:f.name});load();}};
  if(ov)c.append(d);else c.append(a,d);
 });
 $('#ssid').value=s.ssid;$('#host').value=s.host;$('#port').value=s.port;
 $('#pass').placeholder=s.hasPass?'(gespeichert)':'(leer = offenes WLAN)';
}
function upload(){
 const fl=$('#file').files;if(!fl.length){$('#msg').textContent='Bitte Datei wählen';return;}
 let i=0;
 const next=()=>{
  if(i>=fl.length){load();return;}
  const f=fl[i++],fd=new FormData();fd.append('file',f,f.name);
  const x=new XMLHttpRequest();x.open('POST','/upload');
  x.upload.onprogress=e=>{if(e.lengthComputable)$('#prog').value=e.loaded/e.total*100;};
  x.onload=()=>{$('#msg').textContent=f.name+': '+x.responseText;next();};
  x.onerror=()=>{$('#msg').textContent=f.name+': Verbindungsfehler';};
  x.send(fd);
 };
 next();
}
function fw(){
 const f=$('#fwfile').files[0];if(!f){$('#fwmsg').textContent='Bitte .bin w\u00e4hlen';return;}
 if(!confirm('Firmware '+f.name+' ('+fmt(f.size)+') installieren? Das Ger\u00e4t startet danach neu.'))return;
 const fd=new FormData();fd.append('file',f,f.name);
 const x=new XMLHttpRequest();x.open('POST','/update');
 x.upload.onprogress=e=>{if(e.lengthComputable)$('#fwprog').value=e.loaded/e.total*100;};
 x.onload=()=>{$('#fwmsg').textContent=x.responseText;if(x.status==200)setTimeout(()=>location.reload(),15000);};
 x.onerror=()=>{$('#fwmsg').textContent='Verbindung beendet (Neustart?) - Seite in 15 s neu laden';setTimeout(()=>location.reload(),15000);};
 x.send(fd);
}
/*KONV-JS-BEGIN*/
/* ===== vmap-Konverter (laeuft komplett im Browser; Format siehe README.md) ===== */
const VM_ORDER=[2,7,1,3,6,5,4,27,26,21,22,20,24,23,25,0,12,11,10,14,13];
const VM_COL={0:'#ebebeb',1:'#5f6978',2:'#3e4858',3:'#2d73e1',4:'#f08c1e',5:'#aa913c',6:'#6e6e6e',7:'#4682b4',10:'#ffffff',11:'#e6e6c8',12:'#bebea0',13:'#e65ae6',14:'#e65ae6',20:'#3c8cff',21:'#46aac8',22:'#50be6e',23:'#eb4646',24:'#fac83c',25:'#ff2828',26:'#789678',27:'#a078ff'};
const VM_TYPES=[[0,'0 Landesgrenze'],[1,'1 Regionen/Bundesländer'],[2,'2 Nachbarländer'],[3,'3 Flüsse/Gewässer'],[4,'4 Autobahn'],[5,'5 Hauptstraße'],[6,'6 Bahn'],[7,'7 Küste'],[10,'10 Großstadt'],[11,'11 Stadt'],[12,'12 Ort'],[13,'13 Verkehrsflughafen'],[14,'14 Flugplatz'],[20,'20 CTR'],[21,'21 TMA/CTA'],[22,'22 RMZ/TMZ'],[23,'23 Beschränkung R'],[24,'24 Gefahrengebiet Q/D'],[25,'25 Verbotszone P'],[26,'26 TRA/Segelflug'],[27,'27 Luftraum E'],['oa','OpenAir (Lufträume)']];
const VM_EXO={"Munich":"Muenchen","Cologne":"Koeln","Nuremberg":"Nuernberg","Hanover":"Hannover","Brunswick":"Braunschweig","Prague":"Prag","Vienna":"Wien","Copenhagen":"Kopenhagen","Warsaw":"Warschau","Zurich":"Zuerich","Geneva":"Genf","Luxembourg":"Luxemburg","Brussels":"Bruessel","Strasbourg":"Strassburg","Prague ":"Prag"};
const VM_REPL=[["ä","ae"],["ö","oe"],["ü","ue"],["Ä","Ae"],["Ö","Oe"],["Ü","Ue"],["ß","ss"],["ø","oe"],["Ø","Oe"],["å","aa"],["Å","Aa"],["æ","ae"],["ł","l"],["Ł","L"],["đ","d"]];

/* Python-round (Banker's rounding) */
function vmRnd(x){const r=Math.round(x);return(Math.abs(x%1)===0.5&&r%2!==0)?r-1:r;}
function vmToday(){const d=new Date();return d.getFullYear()*10000+(d.getMonth()+1)*100+d.getDate();}
function vmFmtDate(d){return d?String(d%100).padStart(2,'0')+'.'+String(Math.floor(d/100)%100).padStart(2,'0')+'.'+Math.floor(d/10000):'unbekannt';}
function asciiDe(s){
 if(VM_EXO[s])s=VM_EXO[s];
 for(const[a,b]of VM_REPL)s=s.split(a).join(b);
 s=s.normalize('NFKD').replace(/[^\x00-\x7f]/g,'');
 return s.trim().slice(0,31);
}
/* ---- Geometrie ---- */
function vmLines(g){const t=g.type,c=g.coordinates;
 if(t==='LineString')return[c];if(t==='MultiLineString')return c;if(t==='Polygon')return c;
 if(t==='MultiPolygon'){const o=[];for(const p of c)for(const r of p)o.push(r);return o;}return[];}
function vmPoints(g){const t=g.type,c=g.coordinates;if(t==='Point')return[c];if(t==='MultiPoint')return c;return[];}
function clipRuns(pts,bb){
 if(!bb)return[pts];
 const[x0,y0,x1,y1]=bb,n=pts.length;
 const ins=pts.map(p=>x0<=p[0]&&p[0]<=x1&&y0<=p[1]&&p[1]<=y1);
 const runs=[];let cur=[];
 for(let i=0;i<n;i++){
  const keep=ins[i]||(i>0&&ins[i-1])||(i<n-1&&ins[i+1]);
  if(keep)cur.push(pts[i]);else if(cur.length){runs.push(cur);cur=[];}
 }
 if(cur.length)runs.push(cur);
 return runs.filter(r=>r.length>=2);
}
function rdp(pts,tol){
 const n=pts.length;if(n<3)return pts.map((_,i)=>i);
 const keep=new Array(n).fill(false);keep[0]=keep[n-1]=true;
 const st=[[0,n-1]];
 while(st.length){
  const[a,b]=st.pop();const ax=pts[a][0],ay=pts[a][1];
  const dx=pts[b][0]-ax,dy=pts[b][1]-ay,l2=dx*dx+dy*dy;
  let dmax=-1,idx=-1;
  for(let i=a+1;i<b;i++){
   const px=pts[i][0],py=pts[i][1];let d;
   if(l2===0)d=Math.hypot(px-ax,py-ay);
   else{let t=((px-ax)*dx+(py-ay)*dy)/l2;t=t<0?0:t>1?1:t;d=Math.hypot(px-(ax+t*dx),py-(ay+t*dy));}
   if(d>dmax){dmax=d;idx=i;}
  }
  if(dmax>tol){keep[idx]=true;st.push([a,idx]);st.push([idx,b]);}
 }
 const o=[];keep.forEach((k,i)=>{if(k)o.push(i);});return o;
}
function simplify(pts,tol,latRef){
 if(tol<=0||pts.length<3)return pts;
 const cx=Math.cos(latRef*Math.PI/180)*111320.0;
 return rdp(pts.map(p=>[p[0]*cx,p[1]*110574.0]),tol).map(i=>pts[i]);
}
function ringSimplify(pts,tol,latRef){
 const f=pts[0],l=pts[pts.length-1];
 if(f[0]===l[0]&&f[1]===l[1])pts=pts.slice(0,-1);
 if(tol<=0||pts.length<6)return pts.concat([pts[0]]);
 const cx=Math.cos(latRef*Math.PI/180)*111320.0,x0=pts[0][0],y0=pts[0][1];
 let far=0,best=-1;
 for(let i=0;i<pts.length;i++){const d=((pts[i][0]-x0)*cx)**2+((pts[i][1]-y0)*110574.0)**2;if(d>best){best=d;far=i;}}
 const a=simplify(pts.slice(0,far+1),tol,latRef);
 const b=simplify(pts.slice(far).concat([pts[0]]),tol,latRef);
 return a.slice(0,-1).concat(b);
}
/* ---- OpenAir ---- */
function oaCoord(tok,h){const f=tok.split(':').map(Number);
 const v=f[0]+(f.length>1?f[1]/60:0)+(f.length>2?f[2]/3600:0);
 return('SW'.includes(h.toUpperCase())?-v:v);}
function oaLatLon(txt){const t=txt.replace(/,/g,' ').trim().split(/\s+/);return[oaCoord(t[2],t[3]),oaCoord(t[0],t[1])];}
function oaLimit(txt){
 const u=txt.trim().toUpperCase().replace(/ /g,'');let m;
 if(['GND','SFC','GROUND','SURFACE'].includes(u))return'GND';
 if(['UNL','UNLTD','UNLIMITED'].includes(u))return'UNL';
 if((m=/^FL(\d+)$/.exec(u)))return'FL'+m[1];
 if((m=/^(\d+)(?:FT)?(MSL|AMSL)$/.exec(u)))return String(parseInt(m[1],10));
 if((m=/^(\d+)(?:FT)?(AGL|GND|AGND)$/.exec(u))){const n=parseInt(m[1],10);return n===0?'GND':n+'AGL';}
 return u.replace(/[^A-Z0-9]/g,'').slice(0,8)||'?';
}
function oaType(ac,ay){ay=(ay||'').toUpperCase();ac=ac.toUpperCase();
 if(ay==='CTR')return 20;if(ay==='RMZ'||ay==='TMZ'||ac==='TMZ')return 22;
 const m={R:23,Q:24,P:25,W:26,G:26,E:27};return m[ac]!==undefined?m[ac]:21;}
function loadOpenAir(text,skip){
 const out=[];let cur=null,center=null,warn=0;
 const flush=()=>{
  if(cur&&cur.pts.length>=3&&!skip.has(cur.ac)){
   let lab=(cur.an||'').replace(/\b\d{3}\.\d{2,3}\b/g,'');
   lab=asciiDe(lab.replace(/\|/g,' ').split(/\s+/).filter(Boolean).join(' ')).slice(0,24).trim();
   out.push([oaType(cur.ac,cur.ay),(lab||'?')+'|'+oaLimit(cur.al||'GND')+'|'+oaLimit(cur.ah||'UNL'),cur.pts]);
  }
  cur=null;
 };
 for(const raw of text.split(/\r?\n/)){
  const l=raw.trim();if(!l||l[0]==='*')continue;
  const sp=l.indexOf(' ');const k=(sp<0?l:l.slice(0,sp)).toUpperCase(),v=sp<0?'':l.slice(sp+1);
  if(k==='AC'){flush();cur={ac:v.trim(),pts:[]};center=null;}
  else if(!cur)continue;
  else if(k==='AN'||k==='AY'||k==='AH'||k==='AL')cur[k.toLowerCase()]=v.trim();
  else if(k==='DP')cur.pts.push(oaLatLon(v));
  else if(k==='V'&&v.trim().toUpperCase().startsWith('X='))center=oaLatLon(v.split('=').slice(1).join('='));
  else if(k==='DC'&&center){const r=parseFloat(v);
   for(let a=0;a<360;a+=5){const g=a*Math.PI/180;
    cur.pts.push([center[0]+r*1852*Math.sin(g)/(111320.0*Math.cos(center[1]*Math.PI/180)),center[1]+r*1852*Math.cos(g)/110574.0]);}}
  else if(k==='DA'||k==='DB')warn++;
 }
 flush();
 return{items:out,warn};
}
/* ---- Filter ---- */
function parseFilter(s){
 for(const op of['>=','<=','!=','=','>','<']){const i=s.indexOf(op);if(i>0)return[s.slice(0,i),op,s.slice(i+op.length)];}
 throw new Error('Ungültiger Filter: '+s);
}
function matchProps(pr,flt){
 for(const[key,op,val]of flt){
  const v=pr[key];
  if(op==='='||op==='!='){const hit=val.split('|').includes(String(v));if(hit!==(op==='='))return false;}
  else{const x=(v===null||v===undefined||v==='')?NaN:Number(v),y=Number(val);
   if(isNaN(x)||isNaN(y))return false;
   const r={'>=':x>=y,'<=':x<=y,'>':x>y,'<':x<y}[op];if(!r)return false;}
 }
 return true;
}
/* ---- Quellen laden ---- */
function parseCSV(t){
 if(t.charCodeAt(0)===0xFEFF)t=t.slice(1);
 const rows=[];let r=[],f='',q=false;
 for(let i=0;i<t.length;i++){const c=t[i];
  if(q){if(c==='"'){if(t[i+1]==='"'){f+='"';i++;}else q=false;}else f+=c;}
  else if(c==='"')q=true;else if(c===','){r.push(f);f='';}
  else if(c==='\n'){r.push(f);rows.push(r);r=[];f='';}
  else if(c!=='\r')f+=c;}
 if(f!==''||r.length){r.push(f);rows.push(r);}
 return rows;
}
function csvFeatures(text){
 const rows=parseCSV(text),h=rows[0]||[],ia=h.indexOf('latitude_deg'),io=h.indexOf('longitude_deg');
 if(ia<0||io<0)throw new Error('CSV ohne Spalten latitude_deg/longitude_deg (OurAirports airports.csv erwartet)');
 const feats=[];
 for(let i=1;i<rows.length;i++){const r=rows[i],lat=parseFloat(r[ia]),lon=parseFloat(r[io]);
  if(isNaN(lat)||isNaN(lon))continue;
  const pr={};for(let k=0;k<h.length;k++)pr[h[k]]=r[k];
  feats.push({properties:pr,geometry:{type:'Point',coordinates:[lon,lat]}});}
 return feats;
}
function geoFeatures(text){
 const j=JSON.parse(text.charCodeAt(0)===0xFEFF?text.slice(1):text);
 if(j.features)return j.features;if(j.geometry)return[j];if(j.type&&j.coordinates)return[{properties:{},geometry:j}];
 throw new Error('Kein GeoJSON');
}
/* Quelle aus Text erzeugen: {name,kind:'geo'|'csv'|'oa',feats|text} */
function vmSource(name,text){
 const n=name.toLowerCase();
 if(n.endsWith('.csv'))return{name,kind:'csv',feats:csvFeatures(text)};
 if(n.endsWith('.json')||n.endsWith('.geojson'))return{name,kind:'geo',feats:geoFeatures(text)};
 if(/^AC\s/m.test(text))return{name,kind:'oa',text};
 return null;
}
/* Voreingestellte Ebenen je nach Quelldatei (Natural Earth, OurAirports, OpenAir) */
function vmPresets(src){
 const b=src.name.toLowerCase().split('/').pop();
 if(src.kind==='oa')return[{typ:'oa',flt:'',tol:150}];
 if(src.kind==='csv'){const nf='ident';
  return[{typ:13,flt:'iso_country=DE:type=large_airport|medium_airport',nameField:nf},{typ:14,flt:'iso_country=DE:type=small_airport',nameField:nf}];}
 if(b.includes('admin_0_countries'))return[{typ:2,flt:'ADM0_A3!=DEU',tol:1500},{typ:0,flt:'ADM0_A3=DEU',tol:600}];
 if(b.includes('coastline'))return[{typ:7,tol:1500}];
 if(b.includes('admin_1_states'))return[{typ:1,flt:'ADM0_A3=DEU',tol:800}];
 if(b.includes('rivers_lake'))return[{typ:3,tol:1000}];
 if(b.includes('_roads'))return[{typ:5,flt:'type=Secondary Highway',tol:2000},{typ:4,flt:'type=Major Highway',tol:1200}];
 if(b.includes('populated_places'))return[{typ:12,flt:'adm0name=Germany:pop_max>=60000:pop_max<150000'},{typ:11,flt:'adm0name=Germany:pop_max>=150000:pop_max<500000'},{typ:11,flt:'adm0name!=Germany:featurecla=Admin-0 capital'},{typ:10,flt:'adm0name=Germany:pop_max>=500000'}];
 const g=src.feats.find(f=>f.geometry);
 return[{typ:g&&/Point/.test(g.geometry.type)?12:5}];
}
/* ---- Konvertierung ---- */
const tick=()=>new Promise(r=>setTimeout(r,0));
async function vmConvert(layers,o){
 const bb=o.bbox||null,latRef=bb?(bb[1]+bb[3])/2:51.0,log=[],recs=[];
 const pt=(x,y)=>[vmRnd(y*1e5),vmRnd(x*1e5)];
 for(const L of layers){
  await tick();
  const tol=(L.tol===''||L.tol===undefined||L.tol===null||isNaN(Number(L.tol)))?o.tol:Number(L.tol);
  const nm0=L.src.name.split('/').pop();
  if(L.typ==='oa'){
   const skip=new Set((L.skip||'').split('|').map(s=>s.trim()).filter(Boolean));
   const oa=loadOpenAir(L.src.text,skip);
   let nIn=0,nOut=0,nRec=0;
   for(const[t,nm,pts]of oa.items){
    const xs=pts.map(p=>p[0]),ys=pts.map(p=>p[1]);
    if(bb&&(Math.max(...xs)<bb[0]||Math.min(...xs)>bb[2]||Math.max(...ys)<bb[1]||Math.min(...ys)>bb[3]))continue;
    nIn+=pts.length;const q=[];
    for(const[x,y]of ringSimplify(pts,tol,latRef)){const p=pt(x,y);const l=q[q.length-1];if(!l||l[0]!==p[0]||l[1]!==p[1])q.push(p);}
    if(q[0][0]!==q[q.length-1][0]||q[0][1]!==q[q.length-1][1])q.push(q[0]);
    if(q.length<4)continue;
    recs.push([t,nm,q]);nOut+=q.length;nRec++;
   }
   log.push('OpenAir '+nm0+': '+nRec+' Lufträume, Punkte '+nIn+' -> '+nOut);
   if(oa.warn)log.push('  Warnung: '+oa.warn+' Bogen-Befehle (DA/DB) werden nicht unterstützt');
   continue;
  }
  const flt=(L.flt||'').split(':').map(s=>s.trim()).filter(Boolean).map(parseFilter);
  const typ=Number(L.typ);let nf=0,nIn=0,nOut=0,nRec=0;
  for(const ft of L.src.feats){
   const g=ft.geometry;if(!g||!matchProps(ft.properties||{},flt))continue;nf++;
   if(typ>=10){
    for(const[x,y]of vmPoints(g)){
     if(bb&&!(bb[0]<=x&&x<=bb[2]&&bb[1]<=y&&y<=bb[3]))continue;
     const v=(ft.properties||{})[L.nameField||'name'];
     recs.push([typ,asciiDe(String(v||'')),[pt(x,y)]]);nRec++;
    }
    continue;
   }
   for(const line0 of vmLines(g)){
    const line=line0.map(p=>[p[0],p[1]]);nIn+=line.length;
    for(let run of clipRuns(line,bb)){
     run=simplify(run,tol,latRef);const q=[];
     for(const[x,y]of run){const p=pt(x,y);const l=q[q.length-1];if(!l||l[0]!==p[0]||l[1]!==p[1])q.push(p);}
     for(let k=0;k<q.length;k+=60000){
      const ch=q.slice(Math.max(0,k-1),k+60000);
      if(ch.length>=2){recs.push([typ,'',ch]);nOut+=ch.length;nRec++;}
     }
    }
   }
  }
  log.push('Typ '+typ+' '+nm0+': '+nf+' Features -> '+nRec+' Records'+(typ<10?', Punkte '+nIn+' -> '+nOut:''));
 }
 const ord={};VM_ORDER.forEach((t,i)=>ord[t]=i);
 recs.sort((a,b)=>(ord[a[0]]!==undefined?ord[a[0]]:99)-(ord[b[0]]!==undefined?ord[b[0]]:99));
 if(!recs.length)throw new Error('Keine Daten - Filter/BBox prüfen.');
 let la0=1e9,lo0=1e9,la1=-1e9,lo1=-1e9,np=0,size=32;
 const enc=new TextEncoder(),names=recs.map(r=>enc.encode(r[1]));
 recs.forEach((r,i)=>{size+=20+names[i].length+8*r[2].length;np+=r[2].length;
  for(const p of r[2]){if(p[0]<la0)la0=p[0];if(p[0]>la1)la1=p[0];if(p[1]<lo0)lo0=p[1];if(p[1]>lo1)lo1=p[1];}});
 const buf=new ArrayBuffer(size),dv=new DataView(buf),u8=new Uint8Array(buf);
 u8.set([0x56,0x4d,0x41,0x50,1,0,0,0]);
 dv.setUint32(8,recs.length,true);dv.setInt32(12,la0,true);dv.setInt32(16,lo0,true);dv.setInt32(20,la1,true);dv.setInt32(24,lo1,true);
 const date=o.date||vmToday();dv.setUint32(28,date,true);
 let off=32;
 recs.forEach((r,i)=>{
  let a=1e9,b=1e9,c=-1e9,d=-1e9;
  for(const p of r[2]){if(p[0]<a)a=p[0];if(p[0]>c)c=p[0];if(p[1]<b)b=p[1];if(p[1]>d)d=p[1];}
  dv.setUint8(off,r[0]);dv.setUint8(off+1,names[i].length);dv.setUint16(off+2,r[2].length,true);
  dv.setInt32(off+4,a,true);dv.setInt32(off+8,b,true);dv.setInt32(off+12,c,true);dv.setInt32(off+16,d,true);
  off+=20;u8.set(names[i],off);off+=names[i].length;
  for(const p of r[2]){dv.setInt32(off,p[0],true);dv.setInt32(off+4,p[1],true);off+=8;}
 });
 log.push('=> '+recs.length+' Records, '+np+' Punkte, '+(size/1024).toFixed(1)+' KB, Stand '+vmFmtDate(date));
 return{buf,log,recs:recs.length,pts:np,size,date};
}
/* ---- .vmap lesen (Vorschau) ---- */
function vmRead(buf){
 const dv=new DataView(buf),n=dv.getUint32(8,true);let off=32;const recs=[];
 for(let i=0;i<n;i++){const t=dv.getUint8(off),nl=dv.getUint8(off+1),np=dv.getUint16(off+2,true);off+=20+nl;
  const pts=[];for(let k=0;k<np;k++){pts.push([dv.getInt32(off,true),dv.getInt32(off+4,true)]);off+=8;}recs.push([t,pts]);}
 return{bb:[dv.getInt32(12,true),dv.getInt32(16,true),dv.getInt32(20,true),dv.getInt32(24,true)],date:dv.getUint32(28,true),recs};
}
/* ---- ZIP lesen (Deflate ueber DecompressionStream) ---- */
async function zipEntries(buf){
 const u8=new Uint8Array(buf),dv=new DataView(buf);let e=u8.length-22;
 while(e>=0&&dv.getUint32(e,true)!==0x06054b50)e--;
 if(e<0)throw new Error('Keine gültige ZIP-Datei');
 const n=dv.getUint16(e+10,true);let p=dv.getUint32(e+16,true);const out=[],dec=new TextDecoder();
 for(let i=0;i<n;i++){
  if(dv.getUint32(p,true)!==0x02014b50)break;
  const method=dv.getUint16(p+10,true),cs=dv.getUint32(p+20,true),nl=dv.getUint16(p+28,true),el=dv.getUint16(p+30,true),cl=dv.getUint16(p+32,true),lh=dv.getUint32(p+42,true);
  const name=dec.decode(u8.subarray(p+46,p+46+nl));p+=46+nl+el+cl;
  if(name.endsWith('/'))continue;
  out.push({name,text:async()=>{
   const st=lh+30+dv.getUint16(lh+26,true)+dv.getUint16(lh+28,true),data=u8.subarray(st,st+cs);
   if(method===0)return dec.decode(data);
   if(method!==8)throw new Error('ZIP-Methode '+method+' nicht unterstützt');
   if(typeof DecompressionStream==='undefined')throw new Error('Browser kann ZIP nicht entpacken - bitte Datei einzeln wählen');
   const s=new Blob([data]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
   return dec.decode(await new Response(s).arrayBuffer());}});
 }
 return out;
}
if(typeof module!=='undefined')module.exports={vmFmtDate,vmConvert,vmSource,vmPresets,vmRead,zipEntries,parseCSV,asciiDe,loadOpenAir};

/* ===== Konverter-Oberflaeche ===== */
const KSRC=[],KL=[];let KOUT=null,KNAMEUSER=false;
const kEl=id=>document.getElementById(id);
function kLog(t){const p=kEl('klog');p.style.display='block';p.textContent=t;}
async function kAdd(files){
 const msgs=[];
 for(const f of files){
  try{
   const n=f.name.toLowerCase();let list=[];
   if(n.endsWith('.zip')){
    const es=await zipEntries(await f.arrayBuffer());
    const cand=es.filter(e=>/\.(txt|csv|json|geojson)$/i.test(e.name));
    const seeyou=cand.some(e=>/seeyou/i.test(e.name));
    for(const e of cand){
     if(/\.txt$/i.test(e.name)&&seeyou&&!/seeyou/i.test(e.name))continue;
     list.push({name:f.name+'/'+e.name,text:await e.text(),off:/embedded/i.test(e.name)});
    }
   }else list.push({name:f.name,text:await f.text()});
   for(const it of list){
    const s=vmSource(it.name,it.text);
    if(!s){msgs.push(it.name+': Format nicht erkannt');continue;}
    KSRC.push(s);
    for(const p of vmPresets(s))KL.push(Object.assign({src:s,on:!it.off,flt:'',nameField:'',tol:'',skip:''},p));
    msgs.push(it.name+': '+(s.kind==='oa'?'OpenAir':s.feats.length+' Einträge')+(it.off?' (Embedded-Variante, standardmäßig aus)':''));
   }
  }catch(e){msgs.push(f.name+': '+e.message);}
 }
 kLog(msgs.join('\n'));kRender();
}
function kAuto(){
 if(KNAMEUSER)return;
 const on=KL.filter(l=>l.on);
 const oa=on.length&&on.every(l=>l.typ==='oa'),ap=on.length&&on.every(l=>l.src.kind==='csv');
 kEl('kname').value=oa?'ovl_luftraum.vmap':ap?'ovl_flugplaetze.vmap':'karte.vmap';
}
function kRender(){
 const box=kEl('klayers');box.innerHTML='';
 KL.forEach((L,i)=>{
  const d=document.createElement('div');d.className='kl';
  const r1=document.createElement('div');r1.className='kr';
  const cb=document.createElement('input');cb.type='checkbox';cb.checked=L.on;cb.onchange=()=>{L.on=cb.checked;kAuto();};
  const nm=document.createElement('span');nm.className='kn';nm.textContent=L.src.name.split('/').pop();nm.title=L.src.name;
  const sel=document.createElement('select');
  VM_TYPES.forEach(([v,t])=>{const o=document.createElement('option');o.value=v;o.textContent=t;if(String(v)===String(L.typ))o.selected=true;sel.append(o);});
  sel.onchange=()=>{L.typ=sel.value==='oa'?'oa':Number(sel.value);kAuto();kRender();};
  const x=document.createElement('button');x.textContent='✕';x.className='d';x.style.flex='0 0 auto';x.onclick=()=>{KL.splice(i,1);kAuto();kRender();};
  r1.append(cb,nm,sel,x);d.append(r1);
  const r2=document.createElement('div');r2.className='kr';
  const mk=(ph,key,type,cls)=>{const e=document.createElement('input');e.placeholder=ph;e.title=ph;e.value=L[key]===undefined?'':L[key];if(type)e.type=type;
   e.oninput=()=>{L[key]=e.value;};return e;};
  if(L.typ==='oa'){r2.append(mk('Klassen weglassen, z.B. W|G','skip'),mk('Toleranz m','tol','number'));}
  else{const f=mk('Filter: key=wert|wert:key>=zahl','flt');f.style.flex='3 1 220px';r2.append(f);
   if(Number(L.typ)>=10)r2.append(mk('Namensfeld (z.B. name, ident)','nameField'));
   else r2.append(mk('Toleranz m','tol','number'));}
  d.append(r2);box.append(d);
 });
 kAuto();
}
function kBB(){const t=kEl('kbb').value.trim();if(!t)return null;
 const b=t.split(/[\s,;]+/).map(Number);if(b.length!==4||b.some(isNaN))throw new Error('BBox: vier Zahlen W S O N');return b;}
async function kConvert(){
 try{
  const L=KL.filter(l=>l.on);if(!L.length)throw new Error('Keine Ebene aktiv - bitte zuerst Quelldateien wählen.');
  kLog('Konvertiere ...');
  const r=await vmConvert(L,{bbox:kBB(),tol:Number(kEl('ktol').value)||0});
  let nm=kEl('kname').value.trim()||'karte.vmap';if(!/\.vmap$/i.test(nm))nm+='.vmap';
  KOUT={blob:new Blob([r.buf],{type:'application/octet-stream'}),name:nm,buf:r.buf};
  kLog(r.log.join('\n')+'\n\nDatei: '+nm+' ('+fmt(r.size)+')'+(r.size>1.5*1048576?'\nHinweis: große Datei - Toleranz erhöhen oder BBox verkleinern.':''));
  kEl('kdl').style.display=kEl('kup').style.display='inline-block';
  kEl('kdl').onclick=()=>{const a=document.createElement('a');a.href=URL.createObjectURL(KOUT.blob);a.download=KOUT.name;a.click();};
  kEl('kup').onclick=kUpload;kPreview(r.buf);
 }catch(e){kLog('Fehler: '+e.message);}
}
function kUpload(){
 if(!KOUT)return;const fd=new FormData();fd.append('file',KOUT.blob,KOUT.name);
 const p=kEl('kprog');p.style.display='block';p.value=0;
 const x=new XMLHttpRequest();x.open('POST','/upload');
 x.upload.onprogress=e=>{if(e.lengthComputable)p.value=e.loaded/e.total*100;};
 x.onload=()=>{kLog(KOUT.name+': '+x.responseText);if(typeof load==='function')load();};
 x.onerror=()=>kLog('Verbindungsfehler - Seite muss vom M5Dial (AP-Modus) geladen sein.');
 x.send(fd);
}
function kPreview(buf){
 const c=kEl('kprev'),g=c.getContext('2d'),m=vmRead(buf);c.style.display='block';
 g.fillStyle='#080c14';g.fillRect(0,0,c.width,c.height);
 const[la0,lo0,la1,lo1]=m.bb,mid=(la0+la1)/2,k=Math.cos(mid/1e5*Math.PI/180);
 const w=(lo1-lo0)*k||1,h=(la1-la0)||1,s=Math.min((c.width-10)/w,(c.height-10)/h);
 const X=p=>5+(p[1]-lo0)*k*s,Y=p=>c.height-5-(p[0]-la0)*s;
 for(const[t,pts]of m.recs){
  g.strokeStyle=g.fillStyle=VM_COL[t]||'#fff';
  if(t>=10&&t<=14){const r=t>=13?2.5:1.5;g.beginPath();g.arc(X(pts[0]),Y(pts[0]),r,0,6.3);g.fill();continue;}
  g.globalAlpha=t>=20?0.8:1;g.beginPath();pts.forEach((p,i)=>i?g.lineTo(X(p),Y(p)):g.moveTo(X(p),Y(p)));g.stroke();g.globalAlpha=1;
 }
}
document.addEventListener('DOMContentLoaded',()=>{const f=kEl('kfiles');if(f)f.onchange=()=>{kAdd([...f.files]);f.value='';};
 kEl('kname').oninput=()=>{KNAMEUSER=true;};});

/*KONV-JS-END*/
async function save(e){e.preventDefault();
 $('#smsg').textContent=await post('/settings',{ssid:$('#ssid').value,pass:$('#pass').value,host:$('#host').value,port:$('#port').value});
 $('#pass').value='';load();return false;}
load();
</script></body></html>)rawliteral";

void handleRoot() { server.send_P(200, "text/html; charset=utf-8", PAGE); }

void handleStatus() {
  String j;
  j.reserve(768);
  j += "{\"total\":" + String((uint32_t)LittleFS.totalBytes());
  j += ",\"used\":" + String((uint32_t)LittleFS.usedBytes());
  j += ",\"active\":\"" + jsonEsc(cfg.map) + "\"";
  j += ",\"ssid\":\"" + jsonEsc(cfg.staSsid) + "\"";
  j += ",\"host\":\"" + jsonEsc(cfg.host) + "\"";
  j += ",\"port\":" + String(cfg.port);
  j += String(",\"hasPass\":") + (cfg.staPass.length() ? "true" : "false");
  j += ",\"files\":[";
  File root = LittleFS.open("/");
  bool first = true;
  if (root) {
    for (File f = root.openNextFile(); f; f = root.openNextFile()) {
      if (f.isDirectory()) continue;
      String n = f.name();
      int sl = n.lastIndexOf('/');
      if (sl >= 0) n = n.substring(sl + 1);
      if (!n.endsWith(".vmap")) continue;
      if (!first) j += ",";
      first = false;
      j += "{\"name\":\"" + jsonEsc(n) + "\",\"size\":" + String((uint32_t)f.size()) + ",\"date\":" + String(vmapDate(n)) + "}";
    }
  }
  j += "]}";
  server.send(200, "application/json", j);
}

void handleUploadData() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    s_upErr = false;
    s_upMsg = "";
    s_upName = sanitizeName(up.filename);
    if (!s_upName.length()) {
      s_upErr = true;
      s_upMsg = "Dateiname muss auf .vmap enden";
      return;
    }
    LittleFS.remove(UPLOAD_TMP);
    s_upFile = LittleFS.open(UPLOAD_TMP, "w");
    if (!s_upFile) { s_upErr = true; s_upMsg = "Speicher nicht beschreibbar"; }
    g_uploading = true;
    drawApScreen();
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!s_upErr && s_upFile) {
      if (s_upFile.write(up.buf, up.currentSize) != up.currentSize) {
        s_upErr = true;
        s_upMsg = "Speicher voll";
        s_upFile.close();
        LittleFS.remove(UPLOAD_TMP);
      }
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (s_upFile) s_upFile.close();
    if (!s_upErr) {
      String e;
      if (!validateVmap(UPLOAD_TMP, e)) {
        s_upErr = true;
        s_upMsg = "Ungueltige .vmap: " + e;
        LittleFS.remove(UPLOAD_TMP);
      } else {
        String dst = "/" + s_upName;
        LittleFS.remove(dst);
        if (!LittleFS.rename(UPLOAD_TMP, dst)) {
          s_upErr = true;
          s_upMsg = "Umbenennen fehlgeschlagen";
        }
      }
    }
    g_uploading = false;
    g_apDirty = true;
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (s_upFile) s_upFile.close();
    LittleFS.remove(UPLOAD_TMP);
    s_upErr = true;
    s_upMsg = "Upload abgebrochen";
    g_uploading = false;
    g_apDirty = true;
  }
}

void handleUploadDone() {
  if (s_upErr) {
    server.send(400, "text/plain", s_upMsg.length() ? s_upMsg : String("Fehler"));
    return;
  }
  if (s_upName.startsWith("ovl_")) {                // Overlay: nur neu einlesen
    mapReload();
  } else if (!cfg.map.length() || s_upName == cfg.map) {   // erste Karte oder aktive Karte ersetzt
    cfg.map = s_upName;
    prefs.putString("map", cfg.map);
    mapReload();
  }
  server.send(200, "text/plain", "OK, gespeichert");
}

// ------------------------- Firmware-Update (OTA ueber die Web-GUI) -------------------------
static bool   s_fwErr = false;
static String s_fwMsg;
static uint32_t s_fwShown = 0;

static void drawFwScreen(const char *title, const String &line, uint16_t col) {
  canvas.fillScreen(C_BG);
  canvas.setFont(&fonts::Font2);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(col);
  canvas.drawString(title, 120, 80);
  canvas.setTextColor(C_WHITE);
  canvas.drawString(line, 120, 118);
  canvas.setTextColor(C_GREY);
  canvas.drawString("Nicht ausschalten!", 120, 156);
  canvas.pushSprite(&M5Dial.Display, 0, 0);
}

void handleUpdateData() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    s_fwErr = false;
    s_fwMsg = "";
    s_fwShown = 0;
    String n = up.filename;
    n.toLowerCase();
    if (!n.endsWith(".bin")) { s_fwErr = true; s_fwMsg = "Datei muss auf .bin enden"; return; }
    if (n.indexOf("merged") >= 0 || n.indexOf("bootloader") >= 0 || n.indexOf("partitions") >= 0) {
      s_fwErr = true;
      s_fwMsg = "Falsche Datei - bitte GS-Micro-NAV.ino.bin verwenden (nicht merged/bootloader/partitions)";
      return;
    }
    g_uploading = true;
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      s_fwErr = true;
      s_fwMsg = String("Update nicht moeglich: ") + Update.errorString() + " (Partitionstabelle mit OTA noetig)";
      return;
    }
    drawFwScreen("FIRMWARE-UPDATE", "0 KB", C_ORANGE);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (s_fwErr) return;
    if (Update.write(up.buf, up.currentSize) != up.currentSize) {
      s_fwErr = true;
      s_fwMsg = String("Schreibfehler: ") + Update.errorString();
      Update.abort();
      return;
    }
    if (up.totalSize - s_fwShown >= 32768) {
      s_fwShown = up.totalSize;
      drawFwScreen("FIRMWARE-UPDATE", String((unsigned)(up.totalSize / 1024)) + " KB", C_ORANGE);
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (!s_fwErr && !Update.end(true)) {
      s_fwErr = true;
      s_fwMsg = String("Update fehlgeschlagen: ") + Update.errorString();
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    s_fwErr = true;
    s_fwMsg = "Upload abgebrochen";
  }
}

void handleUpdateDone() {
  g_uploading = false;
  g_apDirty = true;
  if (s_fwErr) {
    server.send(400, "text/plain", s_fwMsg.length() ? s_fwMsg : String("Fehler"));
    return;
  }
  server.send(200, "text/plain", "OK, Firmware installiert - Neustart ...");
  drawFwScreen("FIRMWARE OK", "Neustart ...", C_GREEN);
  delay(800);
  ESP.restart();
}

void handleSelect() {
  String n = server.arg("name");
  if (!n.length() || sanitizeName(n) != n || n.startsWith("ovl_") || !LittleFS.exists("/" + n)) { server.send(400, "text/plain", "ungueltig (Overlay?)"); return; }
  cfg.map = n;
  prefs.putString("map", cfg.map);
  mapReload();
  server.send(200, "text/plain", "OK");
}

void handleDelete() {
  String n = server.arg("name");
  if (!n.length() || sanitizeName(n) != n) { server.send(400, "text/plain", "ungueltig"); return; }
  LittleFS.remove("/" + n);
  if (n == cfg.map) ensureMapSelected();
  refreshOverlays();
  loadAirports();
  g_mapDirty = true;
  g_apDirty = true;
  server.send(200, "text/plain", "OK");
}

void handleSettings() {
  String ssid = server.arg("ssid"), pass = server.arg("pass"), host = server.arg("host");
  long port = server.arg("port").toInt();
  ssid.trim();
  host.trim();
  if (ssid.length() < 1 || ssid.length() > 32 || host.length() < 1 || host.length() > 63 || port < 1 || port > 65535) {
    server.send(200, "text/plain", "Ungueltige Eingabe");
    return;
  }
  if (pass == "-") pass = "";
  else if (pass.length() == 0) pass = cfg.staPass;           // unveraendert lassen
  else if (pass.length() < 8 || pass.length() > 63) {
    server.send(200, "text/plain", "Passwort: 8-63 Zeichen");
    return;
  }
  cfg.staSsid = ssid;
  cfg.staPass = pass;
  cfg.host = host;
  cfg.port = (uint16_t)port;
  prefs.putString("ssid", cfg.staSsid);
  prefs.putString("pass", cfg.staPass);
  prefs.putString("host", cfg.host);
  prefs.putUShort("port", cfg.port);
  server.send(200, "text/plain", "Gespeichert (gilt beim Wechsel in den Client-Modus)");
}

void setupWebRoutes() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/upload", HTTP_POST, handleUploadDone, handleUploadData);
  server.on("/select", HTTP_POST, handleSelect);
  server.on("/delete", HTTP_POST, handleDelete);
  server.on("/settings", HTTP_POST, handleSettings);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateData);
  server.onNotFound([]() { server.send(404, "text/plain", "404"); });
}

// -------------------------------- Eingabe / UI -------------------------------
int readEncoderSteps() {
  long p = M5Dial.Encoder.read();
  g_encAccum += (int)(p - g_encLast);
  g_encLast = p;
  int steps = g_encAccum / ENC_COUNTS_PER_DETENT;
  g_encAccum -= steps * ENC_COUNTS_PER_DETENT;
  return steps;
}

// Karte um (dx,dy) Bildschirmpixel mit dem Finger ziehen
void panBy(int dx, int dy) {
  if (!g_panned) { g_panned = true; g_panLat = cfg.lastLat; g_panLon = cfg.lastLon; }
  float mpp = ZOOM_MPP[cfg.zoom];
  float u = (float)-dx, v = (float)dy;         // Mittelpunkt wandert gegen die Fingerbewegung
  float east  = (u * V.cosP + v * V.sinP) * mpp;
  float north = (-u * V.sinP + v * V.cosP) * mpp;
  g_panLat += north / 111195.0;
  if (g_panLat > 85.0) g_panLat = 85.0;
  if (g_panLat < -85.0) g_panLat = -85.0;
  g_panLon += east / (111195.0 * cos(g_panLat * M_PI / 180.0));
  if (g_panLon > 180.0) g_panLon -= 360.0;
  if (g_panLon < -180.0) g_panLon += 360.0;
  g_mapDirty = true;
}

void handleTouch() {
  static bool touching = false, dragged = false;
  static int sx = 0, sy = 0, lx = 0, ly = 0;
  auto td = M5Dial.Touch.getDetail();
  if (ui != UI_MAP || wifiMode != MODE_CLIENT) { touching = false; return; }
  if (td.isPressed()) g_lastInputMs = millis();
  if (td.wasPressed()) {
    touching = true; dragged = false;
    sx = lx = td.x; sy = ly = td.y;
  } else if (touching && td.isPressed()) {
    if (!dragged && (abs(td.x - sx) > 8 || abs(td.y - sy) > 8)) dragged = true;
    if (dragged) {
      int dx = td.x - lx, dy = td.y - ly;
      if (dx || dy) panBy(dx, dy);
      lx = td.x; ly = td.y;
    }
  }
  if (touching && td.wasReleased()) {
    touching = false;
    if (!dragged && g_panned) { g_panned = false; g_mapDirty = true; }   // Tippen = zurueck
  }
}

void showNotice(const char *msg) {
  g_notice = msg;
  drawMenu();
}

void activateMenuItem() {
  switch (menuSel) {
    case 0:
      ui = UI_MAP;
      markAllDirty();
      break;
    case 1:
      cycleMap();
      break;
    case 2:
      cfg.headingUp = cfg.headingUp ? 0 : 1;
      prefs.putUChar("hup", cfg.headingUp);
      g_mapDirty = g_uiDirty = true;
      break;
    case 3:
      cfg.units = cfg.units ? 0 : 1;
      prefs.putUChar("spd", cfg.units);
      g_mapDirty = g_uiDirty = true;
      break;
    case 4:
      ui = UI_DCT;
      dctStep = 0;
      dctPrefix[0] = 0;
      buildDctCands();
      dctMin = dctStep;                        // bis hierhin kein Zurueck (automatisch gewaehlt)
      g_uiDirty = true;
      break;
    case 5:
      cfg.traffic = cfg.traffic ? 0 : 1;
      prefs.putUChar("trf", cfg.traffic);
      g_mapDirty = g_uiDirty = true;
      break;
    case 6:
      cfg.asLevel = (cfg.asLevel + 1) % AS_LEVEL_N;
      prefs.putUChar("asl", cfg.asLevel);
      g_mapDirty = g_uiDirty = true;
      break;
    case 7:
      ui = UI_ASTYPES;
      asSel = 0;
      g_uiDirty = true;
      break;
    case 8:
      showNotice(wifiMode == MODE_CLIENT ? "Starte Access Point..." : "Starte Client...");
      if (wifiMode == MODE_CLIENT) startApMode();
      else startClientMode();
      g_notice = "";
      ui = UI_MAP;
      markAllDirty();
      break;
    case 9:
    case 10:
    case 11:
      infoPage = (uint8_t)(menuSel - 9);
      ui = UI_INFO;
      g_uiDirty = true;
      break;
  }
}

void handleInput() {
  handleTouch();
  int steps = readEncoderSteps();
  bool click = M5Dial.BtnA.wasClicked();
  bool hold = M5Dial.BtnA.wasHold();
  if (steps || click || hold) g_lastInputMs = millis();
  if (steps) beep();

  switch (ui) {
    case UI_MAP:
      if (wifiMode == MODE_CLIENT && steps) {
        int z = (int)cfg.zoom - steps;           // im Uhrzeigersinn = naeher ran
        if (z < 0) z = 0;
        if (z >= ZOOM_N) z = ZOOM_N - 1;
        if (z != cfg.zoom) {
          cfg.zoom = (uint8_t)z;
          g_zoomSaveAt = millis() + 2000;
          g_mapDirty = true;
        }
      }
      if (hold && wifiMode == MODE_CLIENT) {
        cfg.headingUp = cfg.headingUp ? 0 : 1;
        prefs.putUChar("hup", cfg.headingUp);
        g_mapDirty = true;
      } else if (click) {
        ui = UI_MENU;
        menuSel = 0;
        g_uiDirty = true;
      }
      break;
    case UI_MENU:
      if (steps) {
        menuSel = ((menuSel + steps) % MENU_COUNT + MENU_COUNT) % MENU_COUNT;
        g_uiDirty = true;
      }
      if (hold) { ui = UI_MAP; markAllDirty(); }
      else if (click) activateMenuItem();
      break;
    case UI_INFO:
      if (click || hold) { ui = UI_MENU; g_uiDirty = true; }
      break;
    case UI_DCT:
      if (steps && dctN) { dctIdx = ((dctIdx + steps) % dctN + dctN) % dctN; g_uiDirty = true; }
      if (hold) {
        if (dctStep > dctMin) { dctStep--; dctPrefix[dctStep] = 0; buildDctCands(); }
        else ui = UI_MENU;
        g_uiDirty = true;
      } else if (click) dctClick();
      break;
    case UI_ASTYPES:
      if (steps) {
        asSel = ((asSel + steps) % 9 + 9) % 9;
        g_uiDirty = true;
      }
      if (hold || (click && asSel == 8)) { ui = UI_MENU; g_uiDirty = true; }
      else if (click) {
        cfg.asMask ^= (uint8_t)(1 << asSel);
        prefs.putUChar("asm", cfg.asMask);
        g_mapDirty = g_uiDirty = true;
      }
      break;
  }

  if (ui != UI_MAP && millis() - g_lastInputMs > MENU_TIMEOUT_MS) {   // Menue schliesst sich selbst
    ui = UI_MAP;
    markAllDirty();
  }
}

void renderUi() {
  uint32_t now = millis();
  switch (ui) {
    case UI_MAP:
      if (wifiMode == MODE_AP) {
        if (g_apDirty || now - g_lastDraw > 2000) drawApScreen();
      } else {
        GpsData g = getGps();
        bool need = g_mapDirty || g.seq != g_lastSeq || g_trafSeq != g_lastTrafSeq || now - g_lastDraw > 1000;
        if (need && now - g_lastDraw >= 120) drawMapScreen(g);
      }
      break;
    case UI_MENU:
      if (g_uiDirty) drawMenu();
      break;
    case UI_INFO:
      if (g_uiDirty || now - g_lastDraw > 1500) { drawInfo(); g_lastDraw = now; }
      break;
    case UI_ASTYPES:
      if (g_uiDirty) drawAsTypes();
      break;
    case UI_DCT:
      if (g_uiDirty) drawDct();
      break;
  }
}

void housekeeping() {
  uint32_t now = millis();
  if (g_zoomSaveAt && now > g_zoomSaveAt) {
    prefs.putUChar("zoom", cfg.zoom);
    g_zoomSaveAt = 0;
  }
  if (now - g_lastPosSave > 60000) {            // letzte Position gelegentlich sichern
    g_lastPosSave = now;
    if (fabs(cfg.lastLat - g_savedLat) > 0.01 || fabs(cfg.lastLon - g_savedLon) > 0.01) savePosition();
  }
}

// ------------------------------------ Setup ----------------------------------
static void drawBoot() {
  canvas.fillScreen(C_BG);
  canvas.setTextDatum(middle_center);
  canvas.setFont(&fonts::FreeSansBold18pt7b);
  canvas.setTextColor(C_GREEN);
  canvas.drawString(APP_VENDOR, 120, 84);
  canvas.setTextColor(C_WHITE);
  canvas.drawString(APP_NAME, 120, 122);
  canvas.drawLine(60, 150, 180, 150, C_GREY);
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(C_GREY);
  canvas.drawString("Version " APP_VERSION, 120, 170);
  canvas.pushSprite(&M5Dial.Display, 0, 0);
}

void setup() {
  auto mcfg = M5.config();
  M5Dial.begin(mcfg, true, false);
  Serial.begin(115200);
  initApCreds();
  M5Dial.Display.setBrightness(DISPLAY_BRIGHTNESS);
  M5Dial.Speaker.setVolume(60);

  canvas.setColorDepth(16);
  if (!canvas.createSprite(240, 240)) {          // Notfall: 8 Bit Farbtiefe
    canvas.setColorDepth(8);
    canvas.createSprite(240, 240);
  }
  uint32_t bootT0 = millis();
  drawBoot();                                    // Bootscreen waehrend der Initialisierung
  Serial.println(APP_NAME " " APP_VERSION " - " APP_VENDOR);

  g_gpsMutex = xSemaphoreCreateMutex();
  WiFi.persistent(false);

  if (!LittleFS.begin(true)) Serial.println("LittleFS Fehler");
  loadConfig();
  ensureMapSelected();
  setupWebRoutes();

  g_encLast = M5Dial.Encoder.read();
  xTaskCreatePinnedToCore(netTask, "nmea", 8192, nullptr, 1, nullptr, 0);
  startClientMode();
  while (millis() - bootT0 < 2500) delay(10);    // Bootscreen mindestens 2,5 s zeigen
  g_lastInputMs = millis();
  markAllDirty();
}

void loop() {
  M5Dial.update();
  handleInput();
  if (wifiMode == MODE_AP) server.handleClient();
  renderUi();
  housekeeping();
  delay(2);
}
