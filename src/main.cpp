// Elvanto run sheet on a Sunton ESP32-8048S070 (7" 800x480 RGB), shown in portrait.
// Polls Elvanto every minute for the next service with a plan and shows it as a scrollable list.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <esp_lcd_panel_rgb.h>
#include <esp_lcd_panel_ops.h>
#include <vector>
#include <algorithm>
#include <HTTPUpdate.h>
#include <esp_ota_ops.h>
#if __has_include("secrets.h")
#include "secrets.h"  // local builds only: provides the config written to storage (see loadConfig)
#endif

// Set by CI from the release tag (e.g. "1.0.3"); empty for local builds, which any release replaces.
#ifndef FW_VERSION
#define FW_VERSION ""
#endif
static const char *OTA_REPO = "Asherslab/GSBC.ElvantoDisplay";
static const uint32_t OTA_CHECK_MS = 6 * 3600 * 1000UL;

// WiFi credentials and the server's address, read from /config.json (SD card, else internal flash).
static const char *DEFAULT_SERVER = "192.168.3.150";        // the on-site k3s node
static const char *DEFAULT_HOST = "elvanto-display.adp.au";  // its ingress host (LAN only)
struct Config { String ssid, pass, server, host; };
static Config conf;

// ---------- display ----------
//
// The panel is driven with no framebuffer: ESP-IDF's RGB driver streams from two small SRAM
// "bounce" buffers, and onBounce() refills them straight from the pre-rendered list at the
// current scroll offset. Scrolling costs nothing beyond what the panel reads anyway, and the
// offset is latched once per frame, so there is no tearing.

// Sprites are drawn in portrait (rotation 1) but stored in the panel's native landscape layout:
// portrait (x, y) lands at native (width-1-y, x), so each portrait column is one native row.
static LGFX_Sprite contentSpr[2], headerSpr[2], meas;
static int contentCur = 0, headerCur = 0;

static const int W = 480, H = 800, HEADER_H = 136;
static const int VIEW_H = H - HEADER_H;
static const int TEXT_X = 20;  // left edge of item text (clear of the live-item bar)

// Sprites are 4-bit palette images: these are palette indices, expanded to RGB565 by onBounce().
enum : uint8_t { C_BG, C_HEADER, C_TEXT, C_DIM, C_FAINT, C_ACCENT, C_SONG, C_RULE, C_HEADING, C_LIVE, C_ERR };
static const uint8_t PALETTE[][3] = {
  {14, 17, 22}, {22, 27, 34}, {236, 239, 244}, {150, 158, 170}, {80, 88, 100}, {255, 184, 64},
  {120, 190, 255}, {38, 44, 54}, {30, 36, 46}, {48, 40, 22}, {255, 120, 100},
};

// ---------- model ----------

struct Item {
  bool heading;
  String when;   // before | during | after
  String title;
  String song;   // verses note (or arrangement sequence) and key
  String desc;
  int durSec;
  time_t start;  // computed
};

struct Plan {
  String name;
  time_t start = 0;
  int totalSec = 0;
  std::vector<Item> items;
  String sig;  // change detection
};

static Plan plan;               // owned by the UI loop
static Plan incoming;           // filled by fetch task
static volatile bool hasIncoming = false;
static String status = "Connecting...";
static time_t lastOk = 0;
static SemaphoreHandle_t lock;

// ---------- server ----------
// The run sheet comes from the on-site server (server/ in this repo), which does the Elvanto work.
// Plain HTTP to the node's IP with the ingress Host header, so no DNS and no TLS here; Arduino's
// HTTPClient won't let the Host header be overridden, hence the hand-written request.

// Sends a request and leaves `c` positioned at the response body.
static bool serverRequest(WiFiClient &c, const char *method, const char *path, const String &body, int &status) {
  status = 0;
  if (!c.connect(conf.server.c_str(), 80, 5000)) return false;
  c.setTimeout(10000);
  c.printf("%s %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n", method, path, conf.host.c_str());
  if (body.length()) c.printf("Content-Type: application/json\r\nContent-Length: %u\r\n", (unsigned)body.length());
  c.print("\r\n");
  c.print(body);
  String line = c.readStringUntil('\n');  // "HTTP/1.1 200 OK"
  if (line.length() > 12) status = line.substring(9, 12).toInt();
  while (c.connected() || c.available()) {
    line = c.readStringUntil('\n');
    if (line.length() <= 1) break;  // blank line ends the headers
  }
  return status > 0;
}

// The server's /display JSON, which is also the saved copy's format.
static bool planFromJson(JsonDocument &d, Plan &p, time_t &fetched) {
  p = Plan();
  p.name = d["name"] | "";
  p.start = d["start"] | 0L;
  p.totalSec = d["total"] | 0;
  fetched = d["fetched"] | 0L;
  for (JsonObject o : d["items"].as<JsonArray>()) {
    Item x;
    x.heading = o["h"] | false;
    x.when = o["when"] | "during";
    x.title = o["title"] | "";
    x.song = o["song"] | "";
    x.desc = o["desc"] | "";
    x.durSec = o["dur"] | 0;
    x.start = o["start"] | 0L;
    p.items.push_back(x);
  }
  for (auto &x : p.items) p.sig += x.title + "|" + x.desc + "|" + x.song + "|" + x.durSec + "|" + x.when + "\n";
  p.sig += p.name + String((long)p.start);
  return !p.items.empty();
}

// Digits in the print code, from the server (0 until known: then 4-6 are accepted).
static volatile int pinLength = 0;

static bool fetchPlan(Plan &out, String &err) {
  WiFiClient c;
  int status;
  if (!serverRequest(c, "GET", "/display", "", status)) { err = "Server unreachable"; return false; }
  JsonDocument d;
  DeserializationError e = deserializeJson(d, c);
  c.stop();
  if (status != 200) { err = String("Server: ") + (d["error"] | "error"); return false; }
  if (e) { err = String("JSON ") + e.c_str(); return false; }
  pinLength = d["pin_length"] | 0;
  time_t fetched;
  if (!planFromJson(d, out, fetched)) { err = "Empty run sheet"; return false; }
  return true;
}

// ---------- saved copy ----------
// The last schedule is kept so a reboot shows it straight away, written only when it changes.
// Prefer the microSD card: it is on its own SPI bus, so writing it doesn't disturb the display.
// Without a card it goes to internal flash, whose writes stall PSRAM and need a panel resync.

static const char *PLAN_FILE = "/plan.json";
static fs::FS *store = nullptr;
static bool storeIsSd = false;
static void afterFlashWrite();

static bool haveFlashFs = false;

static void initStorage() {
  SPI.begin(12, 13, 11, 10);  // TF slot: SCK, MISO, MOSI, CS
  haveFlashFs = LittleFS.begin(true);
  if (SD.begin(10, SPI, 20000000)) {
    store = &SD;
    storeIsSd = true;
  } else if (haveFlashFs) {
    store = &LittleFS;
  }
  Serial.printf("storage: %s\n", storeIsSd ? "SD card" : store ? "internal flash" : "none");
}

// ---------- config ----------
// Released firmware carries no credentials: they live in /config.json on the SD card and in
// internal flash (so the card is optional). A local build with secrets.h writes that file.
// The Elvanto key is no longer needed here (the server has it); a config that still holds one
// is rewritten without it.

static const char *CONFIG_FILE = "/config.json";

static bool readConfig(fs::FS &fs, bool &hasOldKey) {
  hasOldKey = false;
  File f = fs.open(CONFIG_FILE, "r");
  if (!f) return false;
  JsonDocument d;
  bool ok = !deserializeJson(d, f);
  f.close();
  if (!ok) return false;
  conf.ssid = d["wifi_ssid"] | "";
  conf.pass = d["wifi_pass"] | "";
  conf.server = d["server"] | DEFAULT_SERVER;
  conf.host = d["host"] | DEFAULT_HOST;
  hasOldKey = d["elvanto_key"].is<const char *>();
  return conf.ssid.length() > 0;
}

static void writeConfig(fs::FS &fs) {
  JsonDocument d;
  d["wifi_ssid"] = conf.ssid;
  d["wifi_pass"] = conf.pass;
  d["server"] = conf.server;
  d["host"] = conf.host;
  File f = fs.open(CONFIG_FILE, "w");
  if (f) { serializeJson(d, f); f.close(); }
}

static bool loadConfig() {
  bool found = false;
  Config chosen;
  for (int i = 0; i < 2; i++) {
    bool sd = i == 0;
    if (sd ? !storeIsSd : !haveFlashFs) continue;
    fs::FS &fs = sd ? (fs::FS &)SD : (fs::FS &)LittleFS;
    bool hasOldKey;
    bool ok = readConfig(fs, hasOldKey);
#ifdef WIFI_SSID
    // Provision storage only where it differs, to avoid needless flash writes.
    Config want{WIFI_SSID, WIFI_PASS, DEFAULT_SERVER, DEFAULT_HOST};
    if (!ok || hasOldKey || conf.ssid != want.ssid || conf.pass != want.pass || conf.server != want.server || conf.host != want.host) {
      conf = want;
      writeConfig(fs);
      Serial.printf("config: written to %s\n", sd ? "SD" : "flash");
    }
    ok = true;
#else
    if (ok && hasOldKey) {
      writeConfig(fs);
      Serial.printf("config: removed old Elvanto key from %s\n", sd ? "SD" : "flash");
    }
#endif
    if (ok && !found) { found = true; chosen = conf; }
  }
  conf = chosen;
  return found;
}

static void savePlan(const Plan &p, time_t fetched) {
  JsonDocument doc;
  doc["name"] = p.name;
  doc["start"] = (long)p.start;
  doc["total"] = p.totalSec;
  doc["fetched"] = (long)fetched;
  JsonArray items = doc["items"].to<JsonArray>();
  for (auto &x : p.items) {
    JsonObject o = items.add<JsonObject>();
    o["h"] = x.heading;
    o["when"] = x.when;
    o["title"] = x.title;
    o["song"] = x.song;
    o["desc"] = x.desc;
    o["dur"] = x.durSec;
    o["start"] = (long)x.start;
  }
  if (!store) return;
  File f = store->open(PLAN_FILE, "w");
  if (!f) { Serial.println("save: open failed"); return; }
  serializeJson(doc, f);
  f.close();
  Serial.println("save: ok");
  if (!storeIsSd) afterFlashWrite();
}

static bool loadPlan(Plan &p, time_t &fetched) {
  if (!store) return false;
  File f = store->open(PLAN_FILE, "r");
  if (!f) return false;
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, f);
  f.close();
  if (e) { Serial.printf("load: %s\n", e.c_str()); return false; }
  return planFromJson(doc, p, fetched);
}

static void initPanel();

// Flash writes, WiFi start-up (radio calibration) and similar stalls can throw the panel stream
// out of step, and the driver doesn't always recover, leaving the image shifted sideways.
// Recreating it is a clean resync (one brief blink). Requested from any task; done in loop().
static volatile bool resyncWanted = false;

static void afterFlashWrite() {
  initPanel();
  Serial.println("panel recreated after flash write");
}

// ---------- updates ----------
// Pulls the latest GitHub release and installs it if its version is newer. The new firmware
// confirms itself (cancelling rollback) only after a successful Elvanto fetch; if it never gets
// that far, the next reset boots the previous version.

extern "C" bool verifyRollbackLater() { return true; }  // we confirm the app ourselves

static volatile bool otaMessage = false;
static String otaTag;

static long versionNumber(const char *v) {
  if (*v == 'v') v++;
  int a = 0, b = 0, c = 0;
  if (sscanf(v, "%d.%d.%d", &a, &b, &c) < 1) return 0;
  return a * 1000000L + b * 1000L + c;
}

static void checkForUpdate() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(15000);
  String api = String("https://api.github.com/repos/") + OTA_REPO + "/releases/latest";
  if (!http.begin(client, api)) return;
  http.addHeader("User-Agent", "GSBC.ElvantoDisplay");
  http.addHeader("Accept", "application/vnd.github+json");
  int code = http.GET();
  if (code != 200) { Serial.printf("update: check HTTP %d\n", code); http.end(); return; }
  JsonDocument filter;
  filter["tag_name"] = true;
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["browser_download_url"] = true;
  JsonDocument d;
  DeserializationError e = deserializeJson(d, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (e) { Serial.printf("update: %s\n", e.c_str()); return; }

  const char *tag = d["tag_name"] | "";
  if (versionNumber(tag) <= versionNumber(FW_VERSION)) {
    Serial.printf("update: up to date (%s, latest %s)\n", *FW_VERSION ? FW_VERSION : "local build", tag);
    return;
  }
  String url;
  for (JsonObject a : d["assets"].as<JsonArray>())
    if (strcmp(a["name"] | "", "firmware.bin") == 0) url = a["browser_download_url"] | "";
  if (!url.length()) { Serial.printf("update: %s has no firmware.bin\n", tag); return; }

  Serial.printf("update: installing %s\n", tag);
  otaTag = tag;
  otaMessage = true;
  vTaskDelay(pdMS_TO_TICKS(300));  // let loop() show the message first

  WiFiClientSecure dl;
  dl.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);  // GitHub redirects to its CDN
  httpUpdate.rebootOnUpdate(true);
  t_httpUpdate_return r = httpUpdate.update(dl, url);
  // Only reached on failure (success reboots).
  Serial.printf("update: failed (%d) %s\n", r, httpUpdate.getLastErrorString().c_str());
  resyncWanted = true;  // the flash writes will have upset the panel
}

static void fetchTask(void *) {
  uint32_t lastUpdateCheck = 0;
  bool updateChecked = false;
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      xSemaphoreTake(lock, portMAX_DELAY);
      status = "WiFi reconnecting...";
      xSemaphoreGive(lock);
      WiFi.reconnect();
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }
    // On every boot (so a reboot updates) and every OTA_CHECK_MS after. A local (USB) build skips
    // the boot check, so testing one doesn't immediately replace it with the latest release.
    if (!updateChecked || millis() - lastUpdateCheck > OTA_CHECK_MS) {
      bool boot = !updateChecked;
      updateChecked = true;
      lastUpdateCheck = millis();
      if (boot && !*FW_VERSION) Serial.println("update: local build, skipping the boot check");
      else checkForUpdate();
    }
    Plan p;
    String err;
    bool ok = fetchPlan(p, err);
    xSemaphoreTake(lock, portMAX_DELAY);
    if (ok) {
      static bool first = true;
      if (first) {
        first = false;
        resyncWanted = true;                       // boot activity has settled
        esp_ota_mark_app_valid_cancel_rollback();  // this firmware works; keep it
      }
      incoming = p;
      hasIncoming = true;
      lastOk = time(nullptr);
      status = "";
    } else {
      status = err;
    }
    xSemaphoreGive(lock);
    Serial.printf("fetch: %s %s\n", ok ? "ok" : "FAIL", err.c_str());
    vTaskDelay(pdMS_TO_TICKS(ok ? 60000 : 5000));  // failures (often a truncated response) retry soon
  }
}

// ---------- layout ----------

static std::vector<String> wrap(const String &text, const lgfx::IFont *font, int maxW) {
  std::vector<String> lines;
  meas.setFont(font);
  int pos = 0;
  while (pos <= (int)text.length()) {
    int nl = text.indexOf('\n', pos);
    String para = text.substring(pos, nl < 0 ? text.length() : nl);
    pos = (nl < 0) ? text.length() + 1 : nl + 1;
    String line;
    int i = 0;
    while (i < (int)para.length()) {
      int sp = para.indexOf(' ', i);
      String word = para.substring(i, sp < 0 ? para.length() : sp);
      i = (sp < 0) ? para.length() : sp + 1;
      String trial = line.length() ? line + " " + word : word;
      if (meas.textWidth(trial) <= maxW || !line.length()) line = trial;
      else { lines.push_back(line); line = word; }
    }
    lines.push_back(line);
  }
  return lines;
}

static String fmtTime(time_t t, bool ampm = false) {
  struct tm l;
  localtime_r(&t, &l);
  char b[16];
  int h = l.tm_hour % 12; if (!h) h = 12;
  if (ampm) snprintf(b, sizeof b, "%d:%02d%s", h, l.tm_min, l.tm_hour < 12 ? "am" : "pm");
  else snprintf(b, sizeof b, "%d:%02d", h, l.tm_min);
  return b;
}

static std::vector<int> itemY;   // top of each item in content
static int contentH = 0;
static int liveIdx = -1;

static int currentIndex(time_t now) {
  for (int i = (int)plan.items.size() - 1; i >= 0; i--) {
    const Item &x = plan.items[i];
    if (x.heading) continue;
    if (now >= x.start && now < x.start + max(x.durSec, 60)) return i;
  }
  return -1;
}

// One pass to measure (s == nullptr), another to draw.
static int layout(LGFX_Sprite *s, time_t now) {
  const auto *fTitle = &fonts::FreeSansBold12pt7b;
  const auto *fSmall = &fonts::FreeSans9pt7b;
  const int textW = W - TEXT_X - 16;
  int y = 0;
  if (!s) itemY.clear();
  for (size_t i = 0; i < plan.items.size(); i++) {
    const Item &x = plan.items[i];
    if (!s) itemY.push_back(y);
    if (x.heading) {
      int h = 46;
      if (s) {
        s->fillRect(0, y, W, h, C_HEADING);
        s->setFont(fSmall);
        s->setTextColor(C_ACCENT);
        String t = x.title; t.toUpperCase();
        s->drawString(t, 16, y + 15);
      }
      y += h;
      continue;
    }
    auto titleLines = wrap(x.title, fTitle, textW);
    auto descLines = x.desc.length() ? wrap(x.desc, fSmall, textW) : std::vector<String>();
    auto songLines = x.song.length() ? wrap(x.song, fSmall, textW) : std::vector<String>();
    int h = 14 + titleLines.size() * 30 + songLines.size() * 22 + descLines.size() * 22 + 14;
    if (s) {
      bool live = ((int)i == liveIdx);
      bool past = liveIdx >= 0 && (int)i < liveIdx && x.when != "after";
      if (live) {
        s->fillRect(0, y, W, h, C_LIVE);
        s->fillRect(0, y, 7, h, C_ACCENT);
      }
      int ty = y + 14;
      s->setFont(fTitle);
      s->setTextColor(past ? C_FAINT : (live ? C_ACCENT : C_TEXT));
      for (auto &l : titleLines) { s->drawString(l, TEXT_X, ty); ty += 30; }
      s->setFont(fSmall);
      s->setTextColor(past ? C_FAINT : C_SONG);
      for (auto &l : songLines) { s->drawString(l, TEXT_X, ty); ty += 22; }
      s->setTextColor(past ? C_FAINT : C_DIM);
      for (auto &l : descLines) { s->drawString(l, TEXT_X, ty); ty += 22; }
      s->drawFastHLine(16, y + h - 1, W - 32, C_RULE);
    }
    y += h;
  }
  // Running firmware version, centred under the last item.
  if (s) {
    s->setFont(&fonts::FreeSans9pt7b);
    s->setTextColor(C_FAINT);
    s->setTextDatum(textdatum_t::top_center);
    s->drawString(*FW_VERSION ? String("Firmware v") + FW_VERSION : String("Firmware: local build"), W / 2, y + 16);
    s->setTextDatum(textdatum_t::top_left);
  }
  return y + 52;
}

static void publishContent(int next, int h);

// Allocate the off-screen content sprite (native layout, drawn in portrait).
static LGFX_Sprite &newContent(int h) {
  h = (h + 1) & ~1;  // even, so scroll offsets stay byte-aligned in the 4-bit rows
  int next = contentCur ^ 1;
  LGFX_Sprite &s = contentSpr[next];
  s.deleteSprite();
  s.setPsram(true);
  s.setColorDepth(lgfx::palette_4bit);
  if (!s.createSprite(h, W)) {
    Serial.printf("sprite alloc failed (%d px)\n", h);
    s.createSprite(VIEW_H, W);
  }
  s.setRotation(1);
  s.setTextDatum(textdatum_t::top_left);  // sprites are reused; message() leaves it centred
  // Clear in strips with a pause between: one ~0.5 MB fill hogs PSRAM long enough for the
  // display refill to miss its deadline, which shows as a glitch whenever the list is redrawn.
  for (int y = 0; y < s.height(); y += 64) {
    s.fillRect(0, y, W, 64, C_BG);
    delay(1);
  }
  return s;
}

static void rebuildContent(time_t now) {
  liveIdx = currentIndex(now);
  int h = max(layout(nullptr, now), VIEW_H);
  LGFX_Sprite &s = newContent(h);
  layout(&s, now);
  publishContent(contentCur ^ 1, s.height());
}

static void publishHeader(int next);

static void drawHeader(time_t now, const String &st) {
  int next = headerCur ^ 1;
  LGFX_Sprite &header = headerSpr[next];
  header.fillScreen(C_HEADER);
  header.setTextDatum(textdatum_t::top_left);
  if (plan.items.empty()) {
    header.setFont(&fonts::FreeSansBold18pt7b);
    header.setTextColor(C_TEXT);
    header.drawString("Run Sheet", 16, 18);
  } else {
    header.setFont(&fonts::FreeSansBold18pt7b);
    header.setTextColor(C_TEXT);
    String name = plan.name;
    meas.setFont(&fonts::FreeSansBold18pt7b);
    while (name.length() > 3 && meas.textWidth(name) > W - 130) name = name.substring(0, name.length() - 1);
    if (name != plan.name) name += ".";
    header.drawString(name, 16, 18);

    struct tm l;
    localtime_r(&plan.start, &l);
    char b[40];
    strftime(b, sizeof b, "%a %e %b", &l);
    String when = String(b) + "   " + fmtTime(plan.start, true);
    header.setFont(&fonts::FreeSans12pt7b);
    header.setTextColor(C_ACCENT);
    header.drawString(when, 16, 62);
  }

  // clock
  header.setFont(&fonts::FreeSansBold12pt7b);
  header.setTextColor(C_TEXT);
  header.setTextDatum(textdatum_t::top_right);
  header.drawString(fmtTime(now, true), W - 16, 22);
  header.setTextDatum(textdatum_t::top_left);

  header.setFont(&fonts::FreeSans9pt7b);
  String line;
  if (st.length()) { header.setTextColor(C_ERR); line = st; }
  else {
    header.setTextColor(C_DIM);
    int n = 0; for (auto &x : plan.items) if (!x.heading) n++;
    line = String(n) + " items";
    if (lastOk) line += "  -  updated " + fmtTime(lastOk, true);
  }
  header.drawString(line, 16, 100);

  // WiFi strength: a dot and three arcs, lit by RSSI (all dim when disconnected).
  meas.setFont(&fonts::FreeSans9pt7b);
  int wx = 16 + meas.textWidth(line) + 20, wy = 115;
  int rssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -127;
  int bars = rssi >= -60 ? 3 : rssi >= -70 ? 2 : rssi >= -80 ? 1 : 0;
  header.fillCircle(wx, wy, 2, bars ? C_TEXT : C_FAINT);
  for (int i = 1; i <= 3; i++)
    header.fillArc(wx, wy, 4 * i + 2, 4 * i, 225, 315, i <= bars ? C_TEXT : C_FAINT);
  // Printer icon (tap for the print screen), under the clock.
  int pxl = W - 54, pyt = 62;
  header.drawRect(pxl + 8, pyt, 22, 10, C_DIM);            // paper in
  header.fillRoundRect(pxl, pyt + 10, 38, 18, 4, C_DIM);   // body
  header.fillRect(pxl + 8, pyt + 22, 22, 14, C_TEXT);      // paper out
  header.drawFastHLine(0, HEADER_H - 1, W, C_RULE);
  publishHeader(next);
}

static void message(const String &a, const String &b = "") {
  LGFX_Sprite &s = newContent(VIEW_H);
  s.setTextDatum(textdatum_t::middle_center);
  s.setFont(&fonts::FreeSansBold12pt7b);
  s.setTextColor(C_TEXT);
  s.drawString(a, W / 2, VIEW_H / 2 - 16);
  s.setFont(&fonts::FreeSans9pt7b);
  s.setTextColor(C_DIM);
  s.drawString(b, W / 2, VIEW_H / 2 + 18);
  publishContent(contentCur ^ 1, VIEW_H);
}

// ---------- panel ----------

// What the bounce callback shows. The loop writes `pending`; the ISR copies it to `active`
// at the start of each frame, so a frame never mixes two scroll positions.
struct Frame {
  const uint8_t *content = nullptr;  // 4-bit native rows, `stride` bytes each
  int stride = 0;
  int srcX = 0;                      // native column (pixel) shown at the top of the view
  const uint8_t *header = nullptr;   // 4-bit native rows, `hstride` bytes each
  int hstride = 0;
  int bar0 = -1, bar1 = -1;          // scrollbar span, native columns
};
static Frame pending, active;
static portMUX_TYPE frameMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t frameCount = 0;
static uint16_t barColor;
static uint16_t DRAM_ATTR pal565[16];  // byte-swapped RGB565 (the data pins are byte-swapped)
static uint32_t DRAM_ATTR lut2[256];   // one 4-bit byte -> two pixels, first pixel in the high nibble

static void buildLut() {
  for (int i = 0; i < 16; i++) {
    uint16_t c = i < (int)(sizeof PALETTE / 3) ? lgfx::color565(PALETTE[i][0], PALETTE[i][1], PALETTE[i][2]) : 0;
    pal565[i] = (c >> 8) | (c << 8);
  }
  for (int b = 0; b < 256; b++) lut2[b] = pal565[b >> 4] | (uint32_t)pal565[b & 15] << 16;
  barColor = pal565[C_FAINT];
}

// Expand n (even) 4-bit pixels starting at pixel x0 of `row` into dst (4-byte aligned).
static inline void IRAM_ATTR expand4(uint16_t *dst, const uint8_t *row, int x0, int n) {
  uint32_t *d = (uint32_t *)dst;
  const uint8_t *p = row + (x0 >> 1);
  n >>= 1;
  if (!(x0 & 1)) {
    // Byte loads to reach 4-byte alignment, then whole words (4 bytes = 8 pixels) at a time.
    while (n && ((uintptr_t)p & 3)) { *d++ = lut2[*p++]; n--; }
    const uint32_t *w = (const uint32_t *)p;
    for (; n >= 4; n -= 4, d += 4) {
      uint32_t v = *w++;
      d[0] = lut2[v & 0xFF];
      d[1] = lut2[(v >> 8) & 0xFF];
      d[2] = lut2[(v >> 16) & 0xFF];
      d[3] = lut2[v >> 24];
    }
    p = (const uint8_t *)w;
    while (n--) *d++ = lut2[*p++];
  } else {  // not reached while scroll offsets stay even
    for (int i = 0; i < n; i++) d[i] = lut2[(uint8_t)(p[i] << 4 | p[i + 1] >> 4)];
  }
}
static esp_lcd_panel_handle_t panel = nullptr;
// 480 / 24 = 20 chunks. Each refill (~400 us) must beat 24 lines of scan-out (~760 us), and at
// every frame start the driver fills two of them inside the vertical blanking (see porches).
static const int BOUNCE_ROWS = 24;

// Refill timing, reported with the frame rate (each refill must beat BOUNCE_ROWS lines of scan-out).
static volatile uint32_t dbgMaxCycles = 0, dbgSumCycles = 0, dbgCalls = 0;

static bool IRAM_ATTR onBounceImpl(void *buf, int pos_px, int len_bytes);
static bool IRAM_ATTR onBounce(esp_lcd_panel_handle_t, void *buf, int pos_px, int len_bytes, void *) {
  uint32_t c0 = esp_cpu_get_cycle_count();
  bool r = onBounceImpl(buf, pos_px, len_bytes);
  uint32_t c = esp_cpu_get_cycle_count() - c0;
  if (c > dbgMaxCycles) dbgMaxCycles = c;
  dbgSumCycles += c;
  dbgCalls++;
  return r;
}

static bool IRAM_ATTR onBounceImpl(void *buf, int pos_px, int len_bytes) {
  if (pos_px == 0) {
    portENTER_CRITICAL_ISR(&frameMux);
    active = pending;
    portEXIT_CRITICAL_ISR(&frameMux);
    frameCount++;
  }
  uint16_t *dst = (uint16_t *)buf;
  int row = pos_px / H, rows = len_bytes / (H * 2);
  for (int r = 0; r < rows; r++, row++, dst += H) {
    if (active.content) expand4(dst, active.content + row * active.stride, active.srcX, VIEW_H);
    else memset(dst, 0, VIEW_H * 2);
    if (row >= W - 5 && row < W - 2)
      for (int x = active.bar0; x >= 0 && x < active.bar1; x++) dst[x] = barColor;
    if (active.header) expand4(dst + VIEW_H, active.header + row * active.hstride, 0, HEADER_H);
    else memset(dst + VIEW_H, 0, HEADER_H * 2);
  }
  return false;
}

static void waitFrame() {
  uint32_t f = frameCount, t0 = millis();
  while (frameCount - f < 2 && millis() - t0 < 200) delay(1);
}

static void initPanel() {
  // Data pins B0..B4, G0..G5, R0..R4. LovyanGFX sprites store RGB565 byte-swapped, so the two
  // bytes' pins are swapped here (i ^ 8) instead of swapping every pixel.
  static const int pins[16] = {15, 7, 6, 5, 4, 9, 46, 3, 8, 16, 1, 14, 21, 47, 48, 45};
  esp_lcd_rgb_panel_config_t cfg = {};
  cfg.clk_src = LCD_CLK_SRC_DEFAULT;
  cfg.timings.pclk_hz = 27000000;  // ~59 Hz refresh; below ~55 Hz the panel visibly flickers
  cfg.timings.h_res = H;
  cfg.timings.v_res = W;
  cfg.timings.hsync_front_porch = 8;
  cfg.timings.hsync_pulse_width = 2;
  cfg.timings.hsync_back_porch = 43;
  // At each frame start the driver prefills two bounce buffers (~0.8 ms, slower while booting)
  // after the vsync pulse, so the back porch must outlast that (45 lines = ~1.4 ms); too short and
  // the frame starts a chunk late (image shifted and wrapped).
  cfg.timings.vsync_front_porch = 8;
  cfg.timings.vsync_pulse_width = 2;
  cfg.timings.vsync_back_porch = 45;
  cfg.timings.flags.hsync_idle_low = 1;
  cfg.timings.flags.vsync_idle_low = 1;
  cfg.timings.flags.pclk_active_neg = 1;
  cfg.data_width = 16;
  cfg.bits_per_pixel = 16;
  cfg.bounce_buffer_size_px = H * BOUNCE_ROWS;
  cfg.hsync_gpio_num = 39;
  cfg.vsync_gpio_num = 40;
  cfg.de_gpio_num = 41;
  cfg.pclk_gpio_num = 42;
  cfg.disp_gpio_num = -1;
  for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = pins[i ^ 8];
  cfg.flags.no_fb = 1;

  if (panel) esp_lcd_panel_del(panel);
  ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &panel));
  esp_lcd_rgb_panel_event_callbacks_t cbs = {};
  cbs.on_bounce_empty = onBounce;
  ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(panel, &cbs, nullptr));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

  pinMode(2, OUTPUT);
  digitalWrite(2, HIGH);  // backlight fully on: no PWM, no flicker
}

// ---------- touch (GT911) ----------

static uint8_t gtAddr = 0;

static bool gtWrite(uint16_t reg, uint8_t v) {
  Wire.beginTransmission(gtAddr);
  Wire.write(reg >> 8); Wire.write(reg & 0xFF); Wire.write(v);
  return Wire.endTransmission() == 0;
}

static bool gtRead(uint16_t reg, uint8_t *buf, int n) {
  Wire.beginTransmission(gtAddr);
  Wire.write(reg >> 8); Wire.write(reg & 0xFF);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)gtAddr, n) != n) return false;
  for (int i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

static void initTouch() {
  pinMode(38, OUTPUT);
  digitalWrite(38, LOW); delay(10);
  digitalWrite(38, HIGH); delay(60);
  Wire.begin(19, 20, 400000);
  for (uint8_t a : {0x5D, 0x14}) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { gtAddr = a; break; }
  }
  Serial.printf("GT911 %s 0x%02X\n", gtAddr ? "found at" : "not found", gtAddr);
}

// Returns true while touched; *x/*y in native panel coordinates.
static bool readTouch(int *x, int *y) {
  static bool down = false;
  static int lx, ly;
  if (!gtAddr) return false;
  uint8_t st;
  if (!gtRead(0x814E, &st, 1)) return down;
  if (st & 0x80) {  // new report ready
    int n = st & 0x0F;
    if (n) {
      uint8_t p[4];
      if (gtRead(0x8150, p, 4)) { lx = p[0] | p[1] << 8; ly = p[2] | p[3] << 8; }
    }
    down = n > 0;
    gtWrite(0x814E, 0);
  }
  *x = lx; *y = ly;
  return down;
}

// Touch is polled on core 0 so its I2C interrupts never delay the display refill on core 1.
static volatile bool touchDown = false;
static volatile int touchX = 0, touchY = 0;

static void touchTask(void *) {
  initTouch();  // installs the I2C driver (and its interrupt) on this core
  for (;;) {
    int x, y;
    bool d = readTouch(&x, &y);
    touchX = x;
    touchY = y;
    touchDown = d;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ---------- scrolling ----------

static float scrollY = 0, velocity = 0;
static bool dragging = false;
static int dragLastY = 0;

static int maxScroll() { return max(0, contentH - VIEW_H); }

static void publishContent(int next, int h) {
  contentCur = next;
  contentH = h;
  scrollY = constrain(scrollY, 0.0f, (float)maxScroll());
  portENTER_CRITICAL(&frameMux);
  pending.content = (const uint8_t *)contentSpr[next].getBuffer();
  pending.stride = contentSpr[next].bufferLength() / W;
  pending.srcX = contentH - VIEW_H - ((int)scrollY & ~1);  // must match the new stride in the same frame
  pending.bar0 = pending.bar1 = -1;
  portEXIT_CRITICAL(&frameMux);
  waitFrame();  // the old sprite may be reused after this
}

static void publishHeader(int next) {
  portENTER_CRITICAL(&frameMux);
  pending.header = (const uint8_t *)headerSpr[next].getBuffer();
  pending.hstride = headerSpr[next].bufferLength() / W;
  portEXIT_CRITICAL(&frameMux);
  headerCur = next;
}

static void publishScroll() {
  // Portrait row HEADER_H + v shows content row scrollY + v, which is native column
  // (contentH - 1 - scrollY - v); the view's native columns run 0..VIEW_H-1 bottom-up.
  int s = (int)scrollY & ~1;  // even: whole bytes of the 4-bit rows (fast path in expand4)
  int bar0 = -1, bar1 = -1;
  if (maxScroll() > 0) {
    int barH = max(40, VIEW_H * VIEW_H / contentH);
    int barY = (int)((VIEW_H - barH) * (scrollY / maxScroll()));
    bar0 = VIEW_H - barY - barH;
    bar1 = VIEW_H - barY;
  }
  portENTER_CRITICAL(&frameMux);
  pending.srcX = contentH - VIEW_H - s;
  pending.bar0 = bar0;
  pending.bar1 = bar1;
  portEXIT_CRITICAL(&frameMux);
}

static void updateScroll(float dt) {
  bool touched = touchDown;
  int py = H - 1 - touchX;  // portrait y
  if (touched) {
    if (!dragging) { dragging = true; dragLastY = py; velocity = 0; }
    int dy = py - dragLastY;
    scrollY -= dy;
    if (dt > 0) velocity = 0.5f * velocity + 0.5f * (-dy / dt);  // smoothed for flings
    dragLastY = py;
  } else if (dragging) {
    dragging = false;
  } else if (fabsf(velocity) > 5) {
    scrollY += velocity * dt;
    velocity *= powf(0.05f, dt);  // fling decay
  } else {
    velocity = 0;
  }
  scrollY = constrain(scrollY, 0.0f, (float)maxScroll());
}

// ---------- print ----------
// Tap the printer icon in the header for the print screen: copies, a 4-6 digit code on a keypad,
// then Print. The server checks the code (with a lockout) and sends the PDF to the printer.

// Portrait x from the touch panel's native y. If taps land mirrored left-to-right, use (W - 1 - y).
static inline int touchPortraitX(int y) { return y; }

// A tap: short press that barely moved. Reports where it started, in portrait screen coordinates.
static bool pollTap(int &tx, int &ty) {
  static bool was = false;
  static int sx, sy, lx, ly;
  static uint32_t t0;
  bool down = touchDown;
  int x = touchPortraitX(touchY), y = H - 1 - touchX;
  if (down && !was) { sx = lx = x; sy = ly = y; t0 = millis(); }
  if (down) { lx = x; ly = y; }
  bool tap = !down && was && millis() - t0 < 800 && abs(lx - sx) < 24 && abs(ly - sy) < 24;
  if (tap) { tx = sx; ty = sy; }
  was = down;
  return tap;
}

struct Btn {
  int x, y, w, h;
  bool hit(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};
// In content coordinates (portrait, 0 = top of the area below the header).
static const Btn B_MINUS{170, 92, 64, 56}, B_PLUS{346, 92, 64, 56};
static const Btn B_CANCEL{20, 590, 210, 62}, B_PRINT{250, 590, 210, 62};
static const char *KEYS = "123456789C0<";  // C = clear, < = delete
static Btn keyBtn(int i) { return {60 + (i % 3) * 124, 262 + (i / 3) * 80, 112, 70}; }

static bool printMode = false;
static String pinEntry, printMsg;
static uint8_t printMsgColor = C_DIM;
static int printCopies = 1;
static uint32_t printIdleMs = 0, printCloseAt = 0;
static float savedScroll = 0;

static int pinDigits() { return pinLength >= 4 && pinLength <= 6 ? pinLength : 6; }
static bool pinReady() { return pinLength >= 4 && pinLength <= 6 ? (int)pinEntry.length() == pinLength : pinEntry.length() >= 4; }

static void button(LGFX_Sprite &s, const Btn &b, const char *label, uint8_t bg, uint8_t fg, const lgfx::IFont *font) {
  s.fillRoundRect(b.x, b.y, b.w, b.h, 10, bg);
  s.setFont(font);
  s.setTextColor(fg);
  s.setTextDatum(textdatum_t::middle_center);
  s.drawString(label, b.x + b.w / 2, b.y + b.h / 2);
}

static void drawPrintScreen() {
  LGFX_Sprite &s = newContent(VIEW_H);
  s.setTextDatum(textdatum_t::top_left);
  s.setFont(&fonts::FreeSansBold12pt7b);
  s.setTextColor(C_TEXT);
  s.drawString("Print run sheet", 20, 22);
  s.setFont(&fonts::FreeSans9pt7b);
  s.setTextColor(C_DIM);
  s.drawString(plan.name + ", 3 to a page", 20, 56);

  s.setFont(&fonts::FreeSansBold12pt7b);
  s.setTextColor(C_TEXT);
  s.setTextDatum(textdatum_t::middle_left);
  s.drawString("Pages", 20, B_MINUS.y + B_MINUS.h / 2);
  button(s, B_MINUS, "-", C_HEADING, printCopies > 1 ? C_TEXT : C_FAINT, &fonts::FreeSansBold18pt7b);
  button(s, B_PLUS, "+", C_HEADING, printCopies < 20 ? C_TEXT : C_FAINT, &fonts::FreeSansBold18pt7b);
  s.setFont(&fonts::FreeSansBold18pt7b);
  s.setTextDatum(textdatum_t::middle_center);
  s.drawString(String(printCopies), (B_MINUS.x + B_MINUS.w + B_PLUS.x) / 2, B_MINUS.y + B_MINUS.h / 2);

  // Code: a box per digit (as many as the server's code has), a dot per digit entered.
  int boxes = pinDigits(), x0 = (W - (boxes * 54 - 10)) / 2;
  for (int i = 0; i < boxes; i++) {
    int x = x0 + i * 54, y = 170;
    s.drawRoundRect(x, y, 44, 44, 8, i < (int)pinEntry.length() ? C_ACCENT : C_RULE);
    if (i < (int)pinEntry.length()) s.fillCircle(x + 22, y + 22, 7, C_TEXT);
  }
  s.setFont(&fonts::FreeSans9pt7b);
  s.setTextColor(printMsgColor);
  s.drawString(printMsg.length() ? printMsg : String("Enter the print code"), W / 2, 236);

  for (int i = 0; i < 12; i++) {
    char k = KEYS[i];
    const char *label = k == 'C' ? "Clear" : k == '<' ? "Del" : nullptr;
    char d[2] = {k, 0};
    button(s, keyBtn(i), label ? label : d, C_HEADING, C_TEXT, label ? &fonts::FreeSans12pt7b : &fonts::FreeSansBold18pt7b);
  }
  button(s, B_CANCEL, "Cancel", C_HEADING, C_TEXT, &fonts::FreeSansBold12pt7b);
  bool ready = pinReady();
  button(s, B_PRINT, "Print", ready ? C_ACCENT : C_HEADING, ready ? C_BG : C_FAINT, &fonts::FreeSansBold12pt7b);
  s.setTextDatum(textdatum_t::top_left);
  publishContent(contentCur ^ 1, VIEW_H);
}

static void openPrint() {
  printMode = true;
  pinEntry = "";
  printMsg = "";
  printMsgColor = C_DIM;
  printCopies = 1;
  printCloseAt = 0;
  printIdleMs = millis();
  savedScroll = scrollY;
  scrollY = 0;
  velocity = 0;
  drawPrintScreen();
}

static void closePrint(time_t now) {
  printMode = false;
  pinEntry = "";  // don't keep the code around
  scrollY = savedScroll;
  rebuildContent(now);
}

static void sendPrint() {
  printMsg = "Sending...";
  printMsgColor = C_DIM;
  drawPrintScreen();
  JsonDocument req;
  req["pin"] = pinEntry;
  req["copies"] = printCopies;
  String body;
  serializeJson(req, body);
  WiFiClient c;
  int status;
  JsonDocument res;
  bool sent = serverRequest(c, "POST", "/print", body, status);
  if (sent) deserializeJson(res, c);
  c.stop();
  bool ok = sent && status == 200 && (res["ok"] | false);
  printMsg = sent ? String(res["message"] | (ok ? "Printing" : "Print failed")) : String("Server unreachable");
  printMsgColor = ok ? C_ACCENT : C_ERR;
  pinEntry = "";
  if (ok) printCloseAt = millis() + 3000;
  Serial.printf("print: %s\n", printMsg.c_str());
  drawPrintScreen();
}

// Handles a tap on the print screen (content coordinates). Returns true if it should close.
static bool printTap(int px, int py) {
  printIdleMs = millis();
  if (printCloseAt) return false;
  if (B_CANCEL.hit(px, py)) return true;
  if (B_MINUS.hit(px, py) && printCopies > 1) printCopies--;
  else if (B_PLUS.hit(px, py) && printCopies < 20) printCopies++;
  else if (B_PRINT.hit(px, py)) {
    if (pinReady()) { sendPrint(); return false; }
  } else {
    for (int i = 0; i < 12; i++) {
      if (!keyBtn(i).hit(px, py)) continue;
      char k = KEYS[i];
      if (k == 'C') pinEntry = "";
      else if (k == '<') { if (pinEntry.length()) pinEntry.remove(pinEntry.length() - 1); }
      else if ((int)pinEntry.length() < pinDigits()) pinEntry += k;
      printMsg = "";
      printMsgColor = C_DIM;
    }
  }
  drawPrintScreen();
  return false;
}

// ---------- main ----------

void setup() {
  Serial.begin(115200);
  Serial.printf("PSRAM %u free %u, heap free %u\n", ESP.getPsramSize(), ESP.getFreePsram(), ESP.getFreeHeap());

  buildLut();

  for (auto &h : headerSpr) {
    h.setPsram(true);  // internal RAM is needed for bounce buffers and TLS
    h.setColorDepth(lgfx::palette_4bit);
    h.createSprite(HEADER_H, W);
    h.setRotation(1);
  }

  initPanel();
  Serial.printf("heap free after panel %u\n", ESP.getFreeHeap());
  xTaskCreatePinnedToCore(touchTask, "touch", 4096, nullptr, 2, nullptr, 0);

  // Show the saved schedule (if any) while WiFi and the first fetch happen.
  bool cached = false;
  initStorage();
  if (!loadConfig()) {
    message("No configuration", "Put config.json on the SD card");
    Serial.println("config: none found");
    for (;;) delay(1000);
  }
  Serial.printf("firmware %s\n", *FW_VERSION ? FW_VERSION : "local build");
  time_t fetched = 0;
  if (loadPlan(plan, fetched)) {
    cached = true;
    lastOk = fetched;
    status = "Saved copy - updating...";
    rebuildContent(time(nullptr));
    Serial.printf("load: %d items\n", (int)plan.items.size());
  }
  drawHeader(time(nullptr), cached ? status : "");

  if (!cached) message("Connecting to WiFi", conf.ssid);
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) { resyncWanted = true; }, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.persistent(false);  // credentials come from config.json; don't write them to flash (see afterFlashWrite)
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(conf.ssid.c_str(), conf.pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000) delay(200);
  if (WiFi.status() != WL_CONNECTED) { if (!cached) message("WiFi not connected", "Retrying in background..."); }
  else Serial.printf("WiFi ok %s\n", WiFi.localIP().toString().c_str());

  configTzTime("AEST-10", "pool.ntp.org", "time.google.com");
  if (!cached) message("Syncing clock", "");
  t0 = millis();
  while (time(nullptr) < 1700000000 && millis() - t0 < 20000) delay(200);
  if (!cached) message("Loading run sheet", "");

  lock = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(fetchTask, "fetch", 16384, nullptr, 1, nullptr, 0);
}

void loop() {
  static uint32_t lastMs = millis(), lastHeaderMs = 0;
  static time_t lastMinute = 0;
  static bool haveContent = false;
  uint32_t ms = millis();
  float dt = (ms - lastMs) / 1000.0f;
  lastMs = ms;
  time_t now = time(nullptr);

  String st;
  bool rebuild = false;
  xSemaphoreTake(lock, portMAX_DELAY);
  if (hasIncoming) {
    hasIncoming = false;
    if (incoming.sig != plan.sig) { plan = incoming; rebuild = true; savePlan(plan, lastOk); }
  }
  st = status;
  xSemaphoreGive(lock);

  if (!haveContent && plan.items.empty()) {
    if (ms - lastHeaderMs > 1000) { lastHeaderMs = ms; if (st.length()) message("Waiting for server", st); }
    delay(30);
    return;
  }

  // Re-evaluate the live item once a minute.
  if (now / 60 != lastMinute) {
    lastMinute = now / 60;
    if (haveContent && currentIndex(now) != liveIdx) rebuild = true;
  }
  // While the print screen is up, list changes wait until it closes (closePrint redraws).
  static bool rebuildLater = false;
  if (printMode && rebuild) { rebuildLater = true; rebuild = false; }
  if (!printMode && (rebuild || rebuildLater || !haveContent)) {
    rebuildContent(now);
    haveContent = true;
    rebuildLater = false;
    lastHeaderMs = 0;
  }

  if (ms - lastHeaderMs > 1000) { lastHeaderMs = ms; drawHeader(now, st); }

  if (otaMessage) {
    otaMessage = false;
    message("Updating firmware", "Installing " + otaTag + ", restarting shortly");
  }

  if (resyncWanted) {
    resyncWanted = false;
    initPanel();
    Serial.println("panel recreated (resync)");
  }

  int tx, ty;
  bool tap = pollTap(tx, ty);
  if (printMode) {
    bool close = tap && ty >= HEADER_H && printTap(tx, ty - HEADER_H);
    if (close || millis() - printIdleMs > 60000 || (printCloseAt && millis() > printCloseAt)) {
      closePrint(now);
      rebuildLater = false;
    }
  } else {
    if (tap && ty < HEADER_H && tx >= W - 80) openPrint();
    else updateScroll(dt);
  }
  publishScroll();

  static uint32_t statMs = 0, statFrames = 0;
  if (ms - statMs > 5000) {
    uint32_t calls = dbgCalls;
    Serial.printf("panel %.1f fps  bounce calls %u  avg %u us  max %u us\n",
                  (frameCount - statFrames) * 1000.0f / (ms - statMs), calls,
                  calls ? dbgSumCycles / calls / 240 : 0, dbgMaxCycles / 240);
    dbgMaxCycles = dbgSumCycles = dbgCalls = 0;
    statFrames = frameCount;
    statMs = ms;
  }

  delay(5);  // touch reports arrive every ~10 ms
}
