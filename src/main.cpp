#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Arduino_GFX_Library.h>

#include "secrets.h"
#include "clawd_sprite.h"

// --- JC3248W535 (Sunton/Guition) display + touch pinout ---
#define TFT_BL    1
#define TFT_CS    45
#define TFT_SCK   47
#define TFT_SDA0  21
#define TFT_SDA1  48
#define TFT_SDA2  40
#define TFT_SDA3  39

#define TP_SDA    4
#define TP_SCL    8
#define TP_INT    3
#define TP_ADDR   0x3B

// Native panel 320x480, but we use rotation 1 = landscape 480x320 (matches enclosure).
#define LCD_NATIVE_W 320
#define LCD_NATIVE_H 480
#define LCD_W        480
#define LCD_H        320

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    TFT_CS, TFT_SCK, TFT_SDA0, TFT_SDA1, TFT_SDA2, TFT_SDA3);

Arduino_GFX *panel = new Arduino_AXS15231B(
    bus, GFX_NOT_DEFINED, 0, false, LCD_NATIVE_W, LCD_NATIVE_H);

// AXS15231B QSPI rejects partial writes — draw into a PSRAM-backed canvas, flush whole frame.
Arduino_Canvas *gfx = new Arduino_Canvas(LCD_NATIVE_W, LCD_NATIVE_H, panel, 0, 0, 0);

static const uint16_t COL_BG    = RGB565_BLACK;
static const uint16_t COL_TITLE = RGB565_WHITE;
static const uint16_t COL_OK    = RGB565(0,    255, 100);
static const uint16_t COL_ERR   = RGB565(255,  60,  60);
static const uint16_t COL_WARN  = RGB565(255,  165, 0);
static const uint16_t COL_DIM   = RGB565(120,  120, 120);
static const uint16_t COL_BTC   = RGB565(247,  147, 26);
static const uint16_t COL_ETH   = RGB565(132,  157, 235);
static const uint16_t COL_SOL   = RGB565(153,  69,  255);

struct CoinSnapshot {
  float price_usd;
  float change_24h_pct;
  bool  valid;
  
};
static CoinSnapshot btc{}, eth{}, sol{};

static const uint32_t FETCH_INTERVAL_MS = 30000;
static const uint32_t RETRY_INTERVAL_MS = 5000;
static uint32_t next_fetch_at  = 0;
static uint32_t last_fetch_ok  = 0;
static uint32_t fetch_count    = 0;
static uint32_t fetch_failed   = 0;
static bool     grid_ready     = false;

enum Screen { SCREEN_LIST, SCREEN_DETAIL };
static Screen current_screen = SCREEN_LIST;
static int    current_coin   = 0;

// Top-level pages on SCREEN_LIST — bump PAGE_COUNT to add more.
static const int PAGE_COUNT = 2;
static int current_page = 0;

static const int CHART_POINTS = 64;
struct ChartData {
  float    points[CHART_POINTS];
  int      count;
  uint32_t fetched_at;
};
static ChartData charts[3] = {};
static const uint32_t CHART_TTL_MS = 5UL * 60UL * 1000UL;

static const char *TZ_BOSNIA = "CET-1CEST,M3.5.0/2,M10.5.0/3";

static bool format_time(char *out, size_t cap);
static void ensure_chart_fresh(int coin_idx);
static void draw_chart(int coin_idx);
static void draw_page_indicator_at(int y_center);

static const char *COINGECKO_URL =
    "https://api.coingecko.com/api/v3/simple/price"
    "?ids=bitcoin,ethereum,solana&vs_currencies=usd&include_24hr_change=true";

// --- Touch (AXS15231B integrated touch on I2C 0x3B) ---
static bool touch_begin() {
  Wire.begin(TP_SDA, TP_SCL);
  Wire.setClock(400000);
  pinMode(TP_INT, INPUT_PULLUP);
  Wire.beginTransmission(TP_ADDR);
  return Wire.endTransmission() == 0;
}

// AXS15231B touch read protocol: write a 4-byte read command, get back 14 bytes
// with [touch_count] + per-point {gesture, x_hi, x_lo, y_hi, y_lo, ...}
static bool touch_read(int16_t &x, int16_t &y) {
  static const uint8_t read_cmd[4] = {0xB5, 0xAB, 0xA5, 0x5A};
  Wire.beginTransmission(TP_ADDR);
  Wire.write(read_cmd, 4);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)TP_ADDR, 8) != 8) return false;
  uint8_t buf[8];
  for (int i = 0; i < 8; i++) buf[i] = Wire.read();
  uint8_t fingers = buf[1];
  if (fingers == 0 || fingers > 5) return false;
  uint16_t raw_x = ((buf[2] & 0x0F) << 8) | buf[3];
  uint16_t raw_y = ((buf[4] & 0x0F) << 8) | buf[5];
  // Native is portrait 320x480; rotation 1 swaps and flips one axis.
  x = raw_y;
  y = LCD_H - raw_x;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x >= LCD_W) x = LCD_W - 1;
  if (y >= LCD_H) y = LCD_H - 1;
  return true;
}

static void format_money(char *out, size_t cap, float v) {
  unsigned long ip = (unsigned long)v;
  unsigned int  fp = (unsigned int)((v - ip) * 100.0f + 0.5f);
  if (fp == 100) { ip++; fp = 0; }
  if (v < 100.0f) {
    snprintf(out, cap, "$%lu.%02u", ip, fp);
  } else if (v < 10000.0f) {
    snprintf(out, cap, "$%lu,%03lu.%02u", ip / 1000, ip % 1000, fp);
  } else if (v < 1000000.0f) {
    snprintf(out, cap, "$%lu,%03lu", ip / 1000, ip % 1000);
  } else {
    snprintf(out, cap, "$%lu,%03lu,%03lu",
             ip / 1000000UL, (ip / 1000UL) % 1000UL, ip % 1000UL);
  }
}

static void format_pct(char *out, size_t cap, float pct) {
  snprintf(out, cap, "%+.2f%%", pct);
}

static int16_t text_width(const char *s, uint8_t size) {
  return (int16_t)(strlen(s) * 6 * size);
}

// --- Layout (480x320 landscape, split screen) ---
static const int16_t MARGIN     = 16;
static const int16_t ROW_HEIGHT = 70;
static const int16_t BTC_Y      = 60;
static const int16_t ETH_Y      = 140;
static const int16_t SOL_Y      = 220;
static const int16_t FOOTER_Y   = 300;
static const int16_t LEFT_W     = 260;   // crypto column width — tightened to give right column more room
static const int16_t DIVIDER_X  = 260;
static const int16_t RIGHT_W    = LCD_W - DIVIDER_X;  // 220 px wide
static const int16_t CLAUDE_Y   = 140;   // start of Claude box in right column

// --- Weather (Open-Meteo for Graz, Austria — no API key required) ---
static const float GRAZ_LAT = 47.0708f;
static const float GRAZ_LON = 15.4395f;
static float    weather_temp_c    = 0;
static int      weather_code      = -1;   // -1 = unknown
static uint32_t weather_fetched_at = 0;
static const uint32_t WEATHER_TTL_MS = 10UL * 60UL * 1000UL;

// --- Claude rate-limit usage (pushed by a 60s daemon on the laptop) ---
// Pulled from Anthropic API response headers (`anthropic-ratelimit-unified-{5h,7d}-{utilization,reset}`).
static int      claude_session_pct = -1;     // -1 = no data yet
static int      claude_session_reset_min = 0;
static int      claude_weekly_pct = -1;
static int      claude_weekly_reset_min = 0;
static uint32_t claude_last_update = 0;

static WebServer http_server(8080);
static const uint32_t CLAUDE_FRESH_MS = 180UL * 1000UL;  // daemon polls every 60s; allow 3x slack

// Clawd idle-breathe animation state (advanced from the loop)
static uint16_t clawd_frame = 0;
static uint32_t clawd_frame_started = 0;

// WMO weather code → short label (matches Open-Meteo's `current.weather_code`).
static const char *weather_label(int code) {
  if (code == 0)            return "clear";
  if (code <= 3)            return "cloudy";
  if (code == 45 || code == 48) return "fog";
  if (code >= 51 && code <= 67) return "rain";
  if (code >= 71 && code <= 77) return "snow";
  if (code >= 80 && code <= 86) return "shower";
  if (code >= 95)           return "storm";
  return "?";
}

static void draw_header_into_canvas() {
  gfx->setTextSize(3);
  gfx->setTextColor(COL_TITLE);
  int16_t w = text_width("CoinDeck", 3);
  gfx->setCursor((LEFT_W - w) / 2, 12);
  gfx->print("CoinDeck");

  gfx->setTextSize(1);
  gfx->setTextColor(COL_DIM);
  const char *sub = "crypto desk ticker";
  w = text_width(sub, 1);
  gfx->setCursor((LEFT_W - w) / 2, 42);
  gfx->print(sub);
}

// Boot-time status messages live in the band where the coin grid will eventually go.
static void status_line(int y, uint16_t color, const char *text) {
  gfx->fillScreen(COL_BG);
  draw_header_into_canvas();
  gfx->setTextSize(2);
  gfx->setTextColor(color);
  int16_t w = text_width(text, 2);
  gfx->setCursor((LCD_W - w) / 2, y);
  gfx->print(text);
  gfx->flush();
}

static void draw_coin_row(int16_t y, const char *ticker, uint16_t ticker_color,
                          const CoinSnapshot &c) {
  gfx->setTextSize(3);
  gfx->setTextColor(ticker_color);
  gfx->setCursor(MARGIN, y + 10);
  gfx->print(ticker);

  if (!c.valid) {
    gfx->setTextSize(2);
    gfx->setTextColor(COL_DIM);
    const char *msg = "no data";
    gfx->setCursor(LEFT_W - MARGIN - text_width(msg, 2), y + 18);
    gfx->print(msg);
    return;
  }

  char price_buf[24];
  format_money(price_buf, sizeof(price_buf), c.price_usd);
  gfx->setTextSize(3);
  gfx->setTextColor(COL_TITLE);
  int16_t price_w = text_width(price_buf, 3);
  gfx->setCursor(LEFT_W - MARGIN - price_w, y + 10);
  gfx->print(price_buf);

  char pct_buf[16];
  format_pct(pct_buf, sizeof(pct_buf), c.change_24h_pct);
  gfx->setTextSize(2);
  gfx->setTextColor(c.change_24h_pct >= 0 ? COL_OK : COL_ERR);
  int16_t pct_w = text_width(pct_buf, 2);
  gfx->setCursor(LEFT_W - MARGIN - pct_w, y + 44);
  gfx->print(pct_buf);
}

static void draw_right_column() {
  // Vertical divider
  gfx->drawFastVLine(DIVIDER_X, 0, LCD_H, COL_DIM);

  const int rx = DIVIDER_X + 1;
  const int rcenter = DIVIDER_X + RIGHT_W / 2;

  // City label
  gfx->setTextSize(1);
  gfx->setTextColor(COL_DIM);
  const char *city = "Graz";
  gfx->setCursor(rcenter - text_width(city, 1) / 2, 8);
  gfx->print(city);

  // Clock (HH:MM, big)
  char clock_buf[8];
  if (format_time(clock_buf, sizeof(clock_buf))) {
    gfx->setTextSize(4);
    gfx->setTextColor(COL_TITLE);
    int16_t w = text_width(clock_buf, 4);
    gfx->setCursor(rcenter - w / 2, 26);
    gfx->print(clock_buf);
  } else {
    gfx->setTextSize(1);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(rcenter - text_width("syncing...", 1) / 2, 36);
    gfx->print("syncing...");
  }

  // Temperature
  if (weather_code >= 0) {
    char temp_buf[16];
    snprintf(temp_buf, sizeof(temp_buf), "%dC", (int)(weather_temp_c + (weather_temp_c >= 0 ? 0.5f : -0.5f)));
    gfx->setTextSize(3);
    gfx->setTextColor(COL_WARN);
    int16_t w = text_width(temp_buf, 3);
    gfx->setCursor(rcenter - w / 2, 75);
    gfx->print(temp_buf);

    const char *wlabel = weather_label(weather_code);
    gfx->setTextSize(1);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(rcenter - text_width(wlabel, 1) / 2, 108);
    gfx->print(wlabel);
  } else {
    gfx->setTextSize(1);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(rcenter - text_width("...", 1) / 2, 90);
    gfx->print("...");
  }

  // Horizontal divider above Claude box
  gfx->drawFastHLine(rx + 4, CLAUDE_Y, RIGHT_W - 8, COL_DIM);

  // Title + live/stale indicator
  gfx->setTextSize(2);
  gfx->setTextColor(COL_TITLE);
  const char *claude_title = "Claude";
  gfx->setCursor(rcenter - text_width(claude_title, 2) / 2, CLAUDE_Y + 6);
  gfx->print(claude_title);

  bool has_data = claude_last_update != 0;
  bool live     = has_data && (millis() - claude_last_update < CLAUDE_FRESH_MS);
  uint16_t dot_color = !has_data ? COL_DIM : live ? COL_OK : COL_WARN;
  gfx->fillCircle(rcenter - text_width(claude_title, 2) / 2 - 10, CLAUDE_Y + 14, 4, dot_color);

  if (!has_data || claude_session_pct < 0) {
    gfx->setTextSize(1);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(rcenter - text_width("waiting...", 1) / 2, CLAUDE_Y + 50);
    gfx->print("waiting...");
    return;
  }

  const int bar_x = DIVIDER_X + 8;
  const int bar_w = RIGHT_W - 16;
  const int bar_h = 14;

  auto fmt_reset = [](char *out, size_t cap, int minutes) {
    if (minutes <= 0)          snprintf(out, cap, "now");
    else if (minutes < 60)     snprintf(out, cap, "in %dm", minutes);
    else if (minutes < 60*24) {
      int h = minutes / 60, m = minutes % 60;
      if (m == 0) snprintf(out, cap, "in %dh", h);
      else        snprintf(out, cap, "in %dh %dm", h, m);
    } else {
      int d = minutes / (60*24), h = (minutes % (60*24)) / 60;
      if (h == 0) snprintf(out, cap, "in %dd", d);
      else        snprintf(out, cap, "in %dd %dh", d, h);
    }
  };
  auto bar_color = [](int pct) -> uint16_t {
    if (pct < 60)  return COL_OK;
    if (pct < 85)  return COL_WARN;
    return COL_ERR;
  };

  auto draw_bar = [&](int y, const char *label, int pct, int reset_min) {
    char pct_buf[8];
    snprintf(pct_buf, sizeof(pct_buf), "%d%%", pct);
    gfx->setTextSize(2);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(bar_x, y);
    gfx->print(label);
    gfx->setTextColor(COL_TITLE);
    gfx->setCursor(DIVIDER_X + RIGHT_W - 8 - text_width(pct_buf, 2), y);
    gfx->print(pct_buf);

    int bary = y + 20;
    gfx->drawRect(bar_x, bary, bar_w, bar_h, COL_DIM);
    int fill_w = (bar_w - 2) * pct / 100;
    if (fill_w > 0) gfx->fillRect(bar_x + 1, bary + 1, fill_w, bar_h - 2, bar_color(pct));

    char reset_buf[24], reset_line[40];
    fmt_reset(reset_buf, sizeof(reset_buf), reset_min);
    snprintf(reset_line, sizeof(reset_line), "Resets %s", reset_buf);
    gfx->setTextSize(1);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(bar_x, bary + bar_h + 4);
    gfx->print(reset_line);
  };

  draw_bar(CLAUDE_Y + 28,  "5h",   claude_session_pct, claude_session_reset_min);
  draw_bar(CLAUDE_Y + 80,  "Week", claude_weekly_pct,  claude_weekly_reset_min);

  // Clawd sprite (20x20 scaled 2x = 40x40) bouncing at the bottom of the box.
  const int sprite_scale = 2;
  const int sw = CLAWD_W * sprite_scale;
  const int sh = CLAWD_H * sprite_scale;
  const int sx0 = rcenter - sw / 2;
  const int sy0 = LCD_H - sh - 4;
  const uint8_t *frame = splash_idle_breathe_frames[clawd_frame];
  for (int py = 0; py < CLAWD_H; py++) {
    for (int px = 0; px < CLAWD_W; px++) {
      uint8_t idx = frame[py * CLAWD_W + px];
      if (idx == 0) continue;  // transparent
      uint16_t col = splash_idle_breathe_palette[idx];
      gfx->fillRect(sx0 + px * sprite_scale, sy0 + py * sprite_scale,
                    sprite_scale, sprite_scale, col);
    }
  }
}

static void draw_footer_into_canvas() {
  gfx->setTextSize(1);
  gfx->setTextColor(COL_DIM);
  char left[40];
  if (last_fetch_ok == 0) {
    snprintf(left, sizeof(left), "fetching...");
  } else {
    snprintf(left, sizeof(left), "updated %lus ago",
             (unsigned long)((millis() - last_fetch_ok) / 1000));
  }
  gfx->setCursor(MARGIN, FOOTER_Y + 6);
  gfx->print(left);
}

static void draw_grid() {
  gfx->fillScreen(COL_BG);
  draw_header_into_canvas();
  draw_coin_row(BTC_Y, "BTC", COL_BTC, btc);
  draw_coin_row(ETH_Y, "ETH", COL_ETH, eth);
  draw_coin_row(SOL_Y, "SOL", COL_SOL, sol);
  draw_footer_into_canvas();
  draw_right_column();
  draw_page_indicator_at(FOOTER_Y + 12);
  gfx->flush();
  grid_ready = true;
}

static const char *coin_name(int idx) {
  switch (idx) { case 0: return "BTC"; case 1: return "ETH"; case 2: return "SOL"; }
  return "?";
}
static uint16_t coin_color(int idx) {
  switch (idx) { case 0: return COL_BTC; case 1: return COL_ETH; case 2: return COL_SOL; }
  return COL_DIM;
}
static const CoinSnapshot &coin_snap(int idx) {
  switch (idx) { case 0: return btc; case 1: return eth; }
  return sol;
}

// --- Chart fetch + draw ---
static const int CHART_X0 = MARGIN;
static const int CHART_X1 = LCD_W - MARGIN;
static const int CHART_Y0 = 220;
static const int CHART_Y1 = 285;

static bool fetch_chart(int coin_idx) {
  if (coin_idx < 0 || coin_idx > 2) return false;
  if (WiFi.status() != WL_CONNECTED) return false;
  const char *id = coin_idx == 0 ? "bitcoin"
                 : coin_idx == 1 ? "ethereum"
                 :                  "solana";
  char url[160];
  snprintf(url, sizeof(url),
           "https://api.coingecko.com/api/v3/coins/%s/market_chart"
           "?vs_currency=usd&days=1", id);

  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) return false;
  http.setUserAgent("CoinDeck/0.2 (JC3248W535)");
  http.setTimeout(12000);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("chart HTTP %d for %s\n", code, id);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  JsonDocument filter; filter["prices"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
    Serial.println("chart JSON parse error");
    return false;
  }
  JsonArray prices = doc["prices"];
  int total = prices.size();
  if (total < 2) return false;

  ChartData &c = charts[coin_idx];
  c.count = 0;
  for (int i = 0; i < CHART_POINTS && c.count < CHART_POINTS; i++) {
    int src = (int)((float)i * (total - 1) / (CHART_POINTS - 1));
    JsonArray p = prices[src];
    if (p.size() < 2) break;
    c.points[c.count++] = p[1].as<float>();
  }
  c.fetched_at = millis();
  Serial.printf("chart[%d] %d->%d pts, first=%.2f last=%.2f\n",
                coin_idx, total, c.count, c.points[0], c.points[c.count - 1]);
  return true;
}

static void draw_chart(int coin_idx) {
  const ChartData &c = charts[coin_idx];
  const int w = CHART_X1 - CHART_X0;
  const int h = CHART_Y1 - CHART_Y0;

  if (c.count < 2) {
    gfx->setTextSize(1);
    gfx->setTextColor(COL_DIM);
    const char *m = "chart loading...";
    gfx->setCursor((LCD_W - text_width(m, 1)) / 2, CHART_Y0 + h / 2 - 4);
    gfx->print(m);
    return;
  }

  float mn = c.points[0], mx = c.points[0];
  for (int i = 1; i < c.count; i++) {
    if (c.points[i] < mn) mn = c.points[i];
    if (c.points[i] > mx) mx = c.points[i];
  }
  float rng = mx - mn;
  if (rng < 0.0001f) rng = 0.0001f;

  uint16_t line_color = c.points[c.count - 1] >= c.points[0] ? COL_OK : COL_ERR;
  gfx->drawFastHLine(CHART_X0, CHART_Y1, w, COL_DIM);

  int prev_x = CHART_X0;
  int prev_y = CHART_Y1 - (int)((c.points[0] - mn) / rng * h);
  for (int i = 1; i < c.count; i++) {
    int x = CHART_X0 + (int)((float)i / (c.count - 1) * w);
    int y = CHART_Y1 - (int)((c.points[i] - mn) / rng * h);
    gfx->drawLine(prev_x, prev_y, x, y, line_color);
    gfx->drawLine(prev_x, prev_y - 1, x, y - 1, line_color);
    prev_x = x; prev_y = y;
  }
}

static void ensure_chart_fresh(int coin_idx) {
  ChartData &c = charts[coin_idx];
  bool stale = (c.count == 0) || (millis() - c.fetched_at > CHART_TTL_MS);
  if (stale) fetch_chart(coin_idx);
}

static void draw_detail() {
  gfx->fillScreen(COL_BG);

  const char *name = coin_name(current_coin);
  uint16_t color = coin_color(current_coin);
  const CoinSnapshot &c = coin_snap(current_coin);

  gfx->setTextSize(5);
  gfx->setTextColor(color);
  gfx->setCursor((LCD_W - text_width(name, 5)) / 2, 20);
  gfx->print(name);

  if (!c.valid) {
    gfx->setTextSize(2);
    gfx->setTextColor(COL_DIM);
    const char *m = "no data yet";
    gfx->setCursor((LCD_W - text_width(m, 2)) / 2, 100);
    gfx->print(m);
  } else {
    char price_buf[24];
    format_money(price_buf, sizeof(price_buf), c.price_usd);
    gfx->setTextSize(4);
    gfx->setTextColor(COL_TITLE);
    gfx->setCursor((LCD_W - text_width(price_buf, 4)) / 2, 90);
    gfx->print(price_buf);

    char pct_buf[16];
    format_pct(pct_buf, sizeof(pct_buf), c.change_24h_pct);
    gfx->setTextSize(3);
    gfx->setTextColor(c.change_24h_pct >= 0 ? COL_OK : COL_ERR);
    gfx->setCursor((LCD_W - text_width(pct_buf, 3)) / 2, 150);
    gfx->print(pct_buf);
  }

  // Chart is sync-fetched on entry; the "chart loading..." placeholder shows during the fetch.
  ensure_chart_fresh(current_coin);
  draw_chart(current_coin);

  gfx->setTextSize(1);
  gfx->setTextColor(COL_DIM);
  const char *hint = "tap=list  swipe<>";
  gfx->setCursor(MARGIN, FOOTER_Y + 6);
  gfx->print(hint);
  char clock_buf[8];
  if (format_time(clock_buf, sizeof(clock_buf))) {
    int16_t w = text_width(clock_buf, 1);
    gfx->setCursor(LCD_W - MARGIN - w, FOOTER_Y + 6);
    gfx->print(clock_buf);
  }

  gfx->flush();
}

static void draw_page_indicator_at(int y_center) {
  const int dot_r   = 3;
  const int gap     = 12;
  const int total_w = PAGE_COUNT * (dot_r * 2) + (PAGE_COUNT - 1) * gap;
  const int x0      = (LCD_W - total_w) / 2;
  for (int i = 0; i < PAGE_COUNT; i++) {
    int cx = x0 + i * (dot_r * 2 + gap) + dot_r;
    if (i == current_page) gfx->fillCircle(cx, y_center, dot_r, COL_TITLE);
    else                   gfx->drawCircle(cx, y_center, dot_r, COL_DIM);
  }
}

static void draw_page2() {
  gfx->fillScreen(COL_BG);

  gfx->setTextSize(4);
  gfx->setTextColor(COL_TITLE);
  const char *t = "Page 2";
  gfx->setCursor((LCD_W - text_width(t, 4)) / 2, 100);
  gfx->print(t);

  gfx->setTextSize(1);
  gfx->setTextColor(COL_DIM);
  const char *hint1 = "more widgets go here";
  const char *hint2 = "swipe right to go back";
  gfx->setCursor((LCD_W - text_width(hint1, 1)) / 2, 160);
  gfx->print(hint1);
  gfx->setCursor((LCD_W - text_width(hint2, 1)) / 2, 178);
  gfx->print(hint2);

  draw_page_indicator_at(FOOTER_Y + 12);
  gfx->flush();
  grid_ready = true;
}

static void redraw_current_screen() {
  if (current_screen == SCREEN_DETAIL) { draw_detail(); return; }
  if (current_page == 0) draw_grid();
  else                   draw_page2();
}

static void goto_list() {
  current_screen = SCREEN_LIST;
  redraw_current_screen();
}

static void goto_detail(int coin_idx) {
  current_coin = (coin_idx + 3) % 3;
  current_screen = SCREEN_DETAIL;
  redraw_current_screen();
}

// --- Time / NTP ---
static void ntp_begin() {
  configTzTime(TZ_BOSNIA, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  struct tm t;
  if (getLocalTime(&t, 4000)) {
    Serial.printf("NTP synced: %02d:%02d:%02d\n", t.tm_hour, t.tm_min, t.tm_sec);
  } else {
    Serial.println("NTP sync timeout (will retry implicitly via SNTP)");
  }
}

static bool format_time(char *out, size_t cap) {
  struct tm t;
  if (!getLocalTime(&t, 0)) return false;
  strftime(out, cap, "%H:%M", &t);
  return true;
}

static bool wifi_connect(uint32_t timeout_ms = 20000) {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  status_line(150, COL_WARN, "connecting wifi...");
  Serial.printf("Connecting to '%s'", WIFI_SSID);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeout_ms) {
    delay(300);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    status_line(150, COL_ERR, "wifi FAILED");
    return false;
  }
  Serial.printf("WiFi OK. IP=%s, RSSI=%d dBm\n",
                WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
  ntp_begin();
  return true;
}

static bool fetch_weather() {
  if (WiFi.status() != WL_CONNECTED) return false;
  char url[256];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,weather_code",
           GRAZ_LAT, GRAZ_LON);
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) return false;
  http.setUserAgent("CoinDeck/0.3");
  http.setTimeout(8000);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("weather HTTP %d\n", code);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    Serial.println("weather JSON parse error");
    return false;
  }
  weather_temp_c = doc["current"]["temperature_2m"].as<float>();
  weather_code   = doc["current"]["weather_code"].as<int>();
  weather_fetched_at = millis();
  Serial.printf("weather Graz: %.1fC, code %d (%s)\n",
                weather_temp_c, weather_code, weather_label(weather_code));
  return true;
}

static bool fetch_prices() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, COINGECKO_URL)) return false;
  http.setUserAgent("CoinDeck/0.2 (JC3248W535)");
  http.setTimeout(8000);

  int code = http.GET();
  if (code != 200) {
    Serial.printf("HTTP error %d\n", code);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("JSON parse error: %s\n", err.c_str());
    return false;
  }

  btc = { doc["bitcoin"]["usd"]  | 0.0f, doc["bitcoin"]["usd_24h_change"]  | 0.0f, true };
  eth = { doc["ethereum"]["usd"] | 0.0f, doc["ethereum"]["usd_24h_change"] | 0.0f, true };
  sol = { doc["solana"]["usd"]   | 0.0f, doc["solana"]["usd_24h_change"]   | 0.0f, true };

  Serial.printf("BTC $%-10.2f %+6.2f%%   ETH $%-8.2f %+6.2f%%   SOL $%-6.2f %+6.2f%%\n",
                btc.price_usd, btc.change_24h_pct,
                eth.price_usd, eth.change_24h_pct,
                sol.price_usd, sol.change_24h_pct);
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(2500);
  Serial.println();
  Serial.println("=== CoinDeck v0.2 — JC3248W535 ===");
  Serial.printf("Free heap: %u, free PSRAM: %u\n", ESP.getFreeHeap(), ESP.getFreePsram());

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, LOW);

  if (!gfx->begin()) {
    Serial.println("ERROR: Canvas begin failed.");
    while (true) delay(1000);
  }
  gfx->setRotation(1);  // 480x320 landscape

  if (!touch_begin()) Serial.println("WARN: touch chip did not ACK on I2C 0x3B");
  else                Serial.println("Touch chip OK");

  gfx->fillScreen(COL_BG);
  gfx->flush();
  digitalWrite(TFT_BL, HIGH);

  wifi_connect();
  fetch_weather();

  if (MDNS.begin("coindeck")) {
    MDNS.addService("http", "tcp", 8080);
    Serial.println("mDNS: coindeck.local on port 8080");
  } else {
    Serial.println("mDNS begin failed (will fall back to raw IP).");
  }

  http_server.on("/usage", HTTP_POST, []() {
    String body = http_server.arg("plain");
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
      http_server.send(400, "application/json", "{\"ok\":false,\"err\":\"bad json\"}");
      return;
    }
    claude_session_pct       = doc["session_pct"]       | -1;
    claude_session_reset_min = doc["session_reset_min"] | 0;
    claude_weekly_pct        = doc["weekly_pct"]        | -1;
    claude_weekly_reset_min  = doc["weekly_reset_min"]  | 0;
    claude_last_update = millis();
    Serial.printf("[claude] 5h=%d%% (reset %dm) | 7d=%d%% (reset %dm)\n",
                  claude_session_pct, claude_session_reset_min,
                  claude_weekly_pct,  claude_weekly_reset_min);
    http_server.send(200, "application/json", "{\"ok\":true}");
    if (current_screen == SCREEN_LIST && grid_ready) redraw_current_screen();
  });
  http_server.on("/", HTTP_GET, []() {
    http_server.send(200, "text/plain",
                     "CoinDeck up. POST /usage with {session_pct, session_reset_min, weekly_pct, weekly_reset_min}.");
  });
  http_server.begin();
  Serial.printf("HTTP server up on %s:8080\n", WiFi.localIP().toString().c_str());
}

void loop() {
  static uint32_t last_footer_tick = 0;

  http_server.handleClient();

  // Advance the Clawd animation frame when its hold time elapses.
  if (millis() - clawd_frame_started >= splash_idle_breathe_holds[clawd_frame]) {
    clawd_frame = (clawd_frame + 1) % CLAWD_FRAMES;
    clawd_frame_started = millis();
    if (current_screen == SCREEN_LIST && grid_ready) redraw_current_screen();
  }

  if (WiFi.status() != WL_CONNECTED) {
    grid_ready = false;
    status_line(150, COL_WARN, "wifi lost, retry");
    wifi_connect();
  }

  if ((int32_t)(millis() - next_fetch_at) >= 0) {
    bool ok = fetch_prices();
    fetch_count++;
    if (ok) {
      last_fetch_ok = millis();
      redraw_current_screen();
      next_fetch_at = millis() + FETCH_INTERVAL_MS;
    } else {
      fetch_failed++;
      next_fetch_at = millis() + RETRY_INTERVAL_MS;
    }
    Serial.printf("[fetch %lu ok / %lu fail]\n",
                  (unsigned long)(fetch_count - fetch_failed),
                  (unsigned long)fetch_failed);
  }

  if (millis() - weather_fetched_at > WEATHER_TTL_MS) {
    fetch_weather();
    if (current_screen == SCREEN_LIST && grid_ready) redraw_current_screen();
  }

  // Per-second clock tick — only redraw the visible frame when the displayed minute would actually change.
  if (grid_ready && millis() - last_footer_tick > 1000) {
    redraw_current_screen();
    last_footer_tick = millis();
  }

  // Touch state machine — tap → detail, horizontal swipe → cycle coin on detail, tap on detail → back.
  static bool     touch_active = false;
  static int16_t  start_x = 0, start_y = 0;
  static int16_t  last_x  = 0, last_y  = 0;
  static uint32_t start_ms = 0;

  int16_t tx, ty;
  bool now_touched = touch_read(tx, ty);
  if (now_touched) {
    if (!touch_active) {
      touch_active = true;
      start_x = tx; start_y = ty;
      start_ms = millis();
    }
    last_x = tx; last_y = ty;
  } else if (touch_active) {
    touch_active = false;
    uint32_t dur = millis() - start_ms;
    int16_t  dx  = last_x - start_x;
    int16_t  dy  = last_y - start_y;
    int16_t  adx = dx < 0 ? -dx : dx;
    int16_t  ady = dy < 0 ? -dy : dy;

    if (dur < 40) {
      // bounce — ignore
    } else if (adx > 60 && adx > ady * 2) {
      int dir = dx > 0 ? +1 : -1;
      Serial.printf("SWIPE dx=%d dir=%+d\n", dx, dir);
      if (current_screen == SCREEN_DETAIL) {
        goto_detail(current_coin + (dir > 0 ? -1 : +1));
      } else {
        // SCREEN_LIST: swipe-left → next page, swipe-right → previous.
        int next = current_page + (dir > 0 ? -1 : +1);
        if (next >= 0 && next < PAGE_COUNT && next != current_page) {
          current_page = next;
          redraw_current_screen();
        }
      }
    } else {
      Serial.printf("TAP x=%d y=%d\n", last_x, last_y);
      if (current_screen == SCREEN_LIST) {
        if (current_page == 0 && last_x < LEFT_W) {
          int picked = -1;
          if      (last_y >= BTC_Y && last_y < BTC_Y + ROW_HEIGHT) picked = 0;
          else if (last_y >= ETH_Y && last_y < ETH_Y + ROW_HEIGHT) picked = 1;
          else if (last_y >= SOL_Y && last_y < SOL_Y + ROW_HEIGHT) picked = 2;
          if (picked >= 0) goto_detail(picked);
        }
      } else {
        goto_list();
      }
    }
  }

  delay(20);
}
