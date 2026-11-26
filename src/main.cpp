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

// WiFi and Elvanto credentials, read from /config.json (SD card, else internal flash).
struct Config { String ssid, pass, key; };
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

static time_t utcToEpoch(const char *s) {
  // "YYYY-MM-DD HH:MM:SS" (UTC)
  int y, mo, d, h, mi, se;
  if (sscanf(s, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
  y -= mo <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = era * 146097L + (long)doe - 719468L;
  return (time_t)days * 86400 + h * 3600 + mi * 60 + se;
}

static int parseDur(const char *s) {
  int total = 0, part = 0;
  for (; s && *s; s++) {
    if (*s == ':') { total = total * 60 + part; part = 0; }
    else if (isdigit((unsigned char)*s)) part = part * 10 + (*s - '0');
  }
  // "MM:SS" or "HH:MM:SS"
  return total * 60 + part;
}

static String asciiOnly(const String &in) {
  String out;
  for (size_t i = 0; i < in.length(); i++) {
    unsigned char c = in[i];
    if (c < 0x80) { out += (char)c; continue; }
    // common UTF-8 punctuation
    if (c == 0xE2 && i + 2 < in.length()) {
      unsigned char b = in[i + 1], d = in[i + 2];
      if (b == 0x80 && (d == 0x98 || d == 0x99)) out += '\'';
      else if (b == 0x80 && (d == 0x9C || d == 0x9D)) out += '"';
      else if (b == 0x80 && (d == 0x93 || d == 0x94)) out += '-';
      else if (b == 0x80 && d == 0xA6) out += "...";
      i += 2;
      continue;
    }
    while (i + 1 < in.length() && (in[i + 1] & 0xC0) == 0x80) i++;
  }
  return out;
}

static String stripHtml(const char *html) {
  String s = html ? html : "";
  s.replace("</p>", "\n");
  s.replace("<br>", "\n");
  s.replace("<br />", "\n");
  s.replace("</li>", "\n");
  s.replace("<li>", "- ");
  String out;
  bool tag = false;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '<') tag = true;
    else if (c == '>') tag = false;
    else if (!tag) out += c;
  }
  out.replace("&nbsp;", " ");
  out.replace("&amp;", "&");
  out.replace("&quot;", "\"");
  out.replace("&#39;", "'");
  out.replace("&rsquo;", "'");
  out.replace("&lsquo;", "'");
  out.replace("&ldquo;", "\"");
  out.replace("&rdquo;", "\"");
  out.replace("&ndash;", "-");
  out.replace("&mdash;", "-");
  out.replace("&lt;", "<");
  out.replace("&gt;", ">");
  while (out.indexOf("\n\n") >= 0) out.replace("\n\n", "\n");
  out.trim();
  return asciiOnly(out);
}

// ---------- fetching ----------

static bool fetchPlan(Plan &out, String &err) {
  time_t now = time(nullptr);
  time_t from = now - 86400;
  struct tm g;
  gmtime_r(&from, &g);
  char date[16];
  strftime(date, sizeof date, "%Y-%m-%d", &g);
  char body[160];
  snprintf(body, sizeof body, "{\"page\":1,\"page_size\":10,\"start\":\"%s\",\"fields\":[\"plans\"]}", date);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);  // no chunked encoding, so we can stream-parse
  http.setTimeout(15000);
  if (!http.begin(client, "https://api.elvanto.com/v1/services/getAll.json")) { err = "HTTP begin failed"; return false; }
  http.setAuthorization(conf.key.c_str(), "x");
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  if (code != 200) { err = "HTTP " + String(code); http.end(); return false; }

  JsonDocument filter;
  filter["status"] = true;
  filter["error"]["message"] = true;
  JsonObject fs = filter["services"]["service"][0].to<JsonObject>();
  fs["name"] = true;
  fs["date"] = true;
  JsonObject fi = fs["plans"]["plan"][0]["items"]["item"][0].to<JsonObject>();
  fi["heading"] = true;
  fi["duration"] = true;
  fi["when"] = true;
  fi["title"] = true;
  fi["description"] = true;
  fi["song"]["arrangement"]["key_name"] = true;
  fi["song"]["arrangement"]["sequence"] = true;

  JsonDocument doc;
  DeserializationError de = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(24));
  http.end();
  if (de) { err = String("JSON ") + de.c_str(); return false; }
  if (strcmp(doc["status"] | "", "ok") != 0) { err = String("Elvanto: ") + (doc["error"]["message"] | "error"); return false; }

  // Services with a plan, in start order. Only start times are reliable, so a service stays up
  // until 2 h before the next one, or the end of its own (local) day, whichever is first.
  struct Cand { JsonObject s; time_t start; };
  std::vector<Cand> cands;
  for (JsonObject s : doc["services"]["service"].as<JsonArray>()) {
    JsonArray items = s["plans"]["plan"][0]["items"]["item"];
    if (items.isNull() || items.size() == 0) continue;
    cands.push_back({s, utcToEpoch(s["date"] | "")});
  }
  std::sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) { return a.start < b.start; });

  for (size_t ci = 0; ci < cands.size(); ci++) {
    time_t start = cands[ci].start;
    struct tm l;
    localtime_r(&start, &l);
    l.tm_mday += 1;
    l.tm_hour = l.tm_min = l.tm_sec = 0;
    time_t retire = mktime(&l);  // midnight after the service
    if (ci + 1 < cands.size()) retire = min(retire, cands[ci + 1].start - 2 * 3600);
    if (now >= retire) continue;

    JsonObject s = cands[ci].s;
    JsonArray items = s["plans"]["plan"][0]["items"]["item"];
    Plan p;
    p.name = asciiOnly(s["name"] | "");
    p.start = start;
    for (JsonObject it : items) {
      Item x;
      x.heading = (it["heading"].as<int>() == 1);
      x.when = it["when"] | "during";
      x.title = asciiOnly(it["title"] | "");
      x.desc = stripHtml(it["description"] | "");
      x.durSec = parseDur(it["duration"] | "");
      if (it["song"].is<JsonObject>()) {
        // Under a song: the item's note (usually the verses, e.g. "1 2 c 3"), falling back to
        // the arrangement's sequence, shortened ("Verse 1" -> "V1").
        String v = x.desc;
        x.desc = "";
        if (!v.length()) {
          for (const char *sec : it["song"]["arrangement"]["sequence"].as<JsonArray>()) {
            String part, num;
            for (const char *c = sec; *c; c++) {
              if (isdigit((unsigned char)*c)) num += *c;
              else if (isalpha((unsigned char)*c) && (c == sec || c[-1] == ' ' || c[-1] == '-')) part += (char)toupper(*c);
            }
            if (v.length()) v += " ";
            v += num.length() ? part + num : String(sec);
          }
        }
        String k = it["song"]["arrangement"]["key_name"] | "";
        if (k.length()) v += (v.length() ? "  -  Key " : "Key ") + k;
        x.song = v;
      }
      p.items.push_back(x);
    }
    // Times: "before" items count down to the start, the rest run from it.
    int beforeTotal = 0, during = 0;
    for (auto &x : p.items) if (x.when == "before") beforeTotal += x.durSec;
    int b = 0;
    for (auto &x : p.items) {
      if (x.when == "before") { x.start = start - beforeTotal + b; b += x.durSec; }
      else { x.start = start + during; during += x.durSec; }
    }
    p.totalSec = during;

    for (auto &x : p.items) p.sig += x.title + "|" + x.desc + "|" + x.song + "|" + x.durSec + "|" + x.when + "\n";
    p.sig += p.name + String((long)p.start);
    out = p;
    return true;
  }
  err = "No upcoming service with a plan";
  return false;
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
// Released firmware carries no secrets: credentials live in /config.json on the SD card and in
// internal flash (so the card is optional). A local build with secrets.h writes that file.

static const char *CONFIG_FILE = "/config.json";

static bool readConfig(fs::FS &fs) {
  File f = fs.open(CONFIG_FILE, "r");
  if (!f) return false;
  JsonDocument d;
  bool ok = !deserializeJson(d, f);
  f.close();
  if (!ok) return false;
  conf.ssid = d["wifi_ssid"] | "";
  conf.pass = d["wifi_pass"] | "";
  conf.key = d["elvanto_key"] | "";
  return conf.ssid.length() && conf.key.length();
}

static void writeConfig(fs::FS &fs) {
  JsonDocument d;
  d["wifi_ssid"] = conf.ssid;
  d["wifi_pass"] = conf.pass;
  d["elvanto_key"] = conf.key;
  File f = fs.open(CONFIG_FILE, "w");
  if (f) { serializeJson(d, f); f.close(); }
}

static bool loadConfig() {
#ifdef WIFI_SSID
  conf = {WIFI_SSID, WIFI_PASS, ELVANTO_KEY};
  Config want = conf;
  // Provision storage only where it differs, to avoid needless flash writes.
  if (storeIsSd && !(readConfig(SD) && conf.ssid == want.ssid && conf.pass == want.pass && conf.key == want.key)) {
    conf = want; writeConfig(SD); Serial.println("config: written to SD");
  }
  if (haveFlashFs && !(readConfig(LittleFS) && conf.ssid == want.ssid && conf.pass == want.pass && conf.key == want.key)) {
    conf = want; writeConfig(LittleFS); Serial.println("config: written to flash");
  }
  conf = want;
  return true;
#else
  if (storeIsSd && readConfig(SD)) return true;
  return haveFlashFs && readConfig(LittleFS);
#endif
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
  p = Plan();
  p.name = doc["name"] | "";
  p.start = doc["start"] | 0L;
  p.totalSec = doc["total"] | 0;
  fetched = doc["fetched"] | 0L;
  for (JsonObject o : doc["items"].as<JsonArray>()) {
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
  // Same signature as fetchPlan() builds, so an unchanged fetch is not saved again.
  for (auto &x : p.items) p.sig += x.title + "|" + x.desc + "|" + x.song + "|" + x.durSec + "|" + x.when + "\n";
  p.sig += p.name + String((long)p.start);
  return !p.items.empty();
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
    // On every boot (so a reboot updates) and every OTA_CHECK_MS after.
    if (!updateChecked || millis() - lastUpdateCheck > OTA_CHECK_MS) {
      updateChecked = true;
      lastUpdateCheck = millis();
      checkForUpdate();
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
  return y + 40;  // a little space after the last item
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
  s.fillScreen(C_BG);
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
  if (!cached) message("Loading from Elvanto", "");

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
    if (ms - lastHeaderMs > 1000) { lastHeaderMs = ms; if (st.length()) message("Waiting for Elvanto", st); }
    delay(30);
    return;
  }

  // Re-evaluate the live item once a minute.
  if (now / 60 != lastMinute) {
    lastMinute = now / 60;
    if (haveContent && currentIndex(now) != liveIdx) rebuild = true;
  }
  if (rebuild || !haveContent) {
    rebuildContent(now);
    haveContent = true;
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

  updateScroll(dt);
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
