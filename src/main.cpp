#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <Preferences.h>
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
static const uint16_t COL_XRP   = RGB565(220,  220, 220);   // light gray
static const uint16_t COL_BNB   = RGB565(243,  186, 47);    // binance yellow

struct CoinSnapshot {
  float price_usd;
  float change_24h_pct;
  bool  valid;
};

static const int COIN_COUNT = 5;
static const char *COIN_NAMES[COIN_COUNT] = {"BTC", "ETH", "SOL", "XRP", "BNB"};
static const char *COIN_IDS[COIN_COUNT]   = {"bitcoin", "ethereum", "solana", "ripple", "binancecoin"};
static const uint16_t COIN_COLORS[COIN_COUNT] = {COL_BTC, COL_ETH, COL_SOL, COL_XRP, COL_BNB};
static CoinSnapshot coins[COIN_COUNT];

static const uint32_t FETCH_INTERVAL_MS = 30000;
static const uint32_t RETRY_INTERVAL_MS = 5000;
static uint32_t next_fetch_at  = 0;
static uint32_t last_fetch_ok  = 0;
static uint32_t fetch_count    = 0;
static uint32_t fetch_failed   = 0;
static bool     grid_ready     = false;

enum Screen {
  SCREEN_LIST,
  SCREEN_DETAIL,
  SCREEN_GITHUB_DETAIL,
  SCREEN_ISSUES_DETAIL,
  SCREEN_WIFI_SCAN,
  SCREEN_WIFI_KEY,
};
static Screen current_screen = SCREEN_LIST;
static int    current_coin   = 0;

// --- On-screen WiFi setup ---
struct WifiNet { char ssid[33]; int rssi; bool open; };
static const int WIFI_LIST_MAX = 10;
static WifiNet wifi_list[WIFI_LIST_MAX];
static int     wifi_list_count = 0;
static bool    wifi_scan_busy  = false;

static char wifi_pick_ssid[33] = "";
static char wifi_pass_buf[64]  = "";
static int  wifi_pass_len      = 0;
static bool wifi_caps_on       = false;
static const char *wifi_status_msg = "";   // shown above keyboard for errors / hints

// Last-known good creds — populated by WiFiManager on initial boot, by the
// on-screen keyboard on successful connect. Used for silent loop reconnects.
static String saved_ssid;
static String saved_pass;

// Y bounds of the "Issues" section inside the GitHub detail screen, captured
// during render so the touch handler can tell when a tap lands on it.
static int gh_issues_band_top = -1;
static int gh_issues_band_bot = -1;

// Top-level pages on SCREEN_LIST — bump PAGE_COUNT to add more.
//   0 = GitHub box + weather/clock/Claude widgets
//   1 = CoinDeck full-screen (5 coins)
//   2 = WiFi info + on-demand setup portal
static const int PAGE_COUNT = 3;
static int current_page = 0;

static const int CHART_POINTS = 64;
struct ChartData {
  float    points[CHART_POINTS];
  int      count;
  uint32_t fetched_at;
};
static ChartData charts[COIN_COUNT] = {};
static const uint32_t CHART_TTL_MS = 5UL * 60UL * 1000UL;

static const char *TZ_BOSNIA = "CET-1CEST,M3.5.0/2,M10.5.0/3";

static bool format_time(char *out, size_t cap);
static void ensure_chart_fresh(int coin_idx);
static void draw_chart(int coin_idx);
static void draw_page_indicator_at(int y_center);

static const char *COINGECKO_URL =
    "https://api.coingecko.com/api/v3/simple/price"
    "?ids=bitcoin,ethereum,solana,ripple,binancecoin&vs_currencies=eur&include_24hr_change=true";

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

// Always 2 decimals; thousands separator only when there are thousands.
// No currency prefix — the EUR label lives in the page subtitle.
static void format_money(char *out, size_t cap, float v) {
  unsigned long ip = (unsigned long)v;
  unsigned int  fp = (unsigned int)((v - ip) * 100.0f + 0.5f);
  if (fp == 100) { ip++; fp = 0; }
  if (v < 1000.0f) {
    snprintf(out, cap, "%lu.%02u", ip, fp);
  } else if (v < 1000000.0f) {
    snprintf(out, cap, "%lu,%03lu.%02u", ip / 1000, ip % 1000, fp);
  } else {
    snprintf(out, cap, "%lu,%03lu,%03lu.%02u",
             ip / 1000000UL, (ip / 1000UL) % 1000UL, ip % 1000UL, fp);
  }
}

static void format_pct(char *out, size_t cap, float pct) {
  snprintf(out, cap, "%+.2f%%", pct);
}

static int16_t text_width(const char *s, uint8_t size) {
  return (int16_t)(strlen(s) * 6 * size);
}

static int draw_euro(int x, int y, uint8_t size, uint16_t color) {
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  gfx->setCursor(x, y);
  gfx->print("C");
  int bar_w = 5 * size;
  int bar_h = size >= 2 ? size / 2 : 1;
  gfx->fillRect(x - size / 2, y + 2 * size, bar_w, bar_h, color);
  gfx->fillRect(x - size / 2, y + 4 * size, bar_w, bar_h, color);
  return 6 * size + 2;
}

// --- Layout (480x320 landscape, split screen) ---
static const int16_t MARGIN     = 16;
static const int16_t ROW_HEIGHT = 70;
static const int16_t BTC_Y      = 60;
static const int16_t ETH_Y      = 140;
static const int16_t SOL_Y      = 220;
static const int16_t FOOTER_Y   = 300;
static const int16_t LEFT_W     = 320;   // GitHub gets more room; right widget stub narrows.
static const int16_t DIVIDER_X  = 320;
static const int16_t RIGHT_W    = LCD_W - DIVIDER_X;  // 160 px wide
static const int16_t CLAUDE_Y   = 140;   // start of Claude box in right column

// --- Weather (Open-Meteo for Graz, Austria — no API key required) ---
static const float GRAZ_LAT = 47.0708f;
static const float GRAZ_LON = 15.4395f;
static float    weather_temp_c    = 0;
static int      weather_code      = -1;   // -1 = unknown
static uint32_t weather_fetched_at = 0;
static const uint32_t WEATHER_TTL_MS = 10UL * 60UL * 1000UL;

// --- Claude rate-limit usage (pulled from the Cloudflare Worker) ---
// Worker hits Anthropic /v1/messages with the user's OAuth and surfaces the
// `anthropic-ratelimit-unified-{5h,7d}-{utilization,reset}` headers.
static int      claude_session_pct = -1;     // -1 = no data yet
static int      claude_session_reset_min = 0;
static int      claude_weekly_pct = -1;
static int      claude_weekly_reset_min = 0;
static uint32_t claude_last_update = 0;

static const uint32_t CLAUDE_FRESH_MS = 180UL * 1000UL;  // pull every 60s; allow 3x slack

// Clawd idle-breathe animation state (advanced from the loop)
static uint16_t clawd_frame = 0;
static uint32_t clawd_frame_started = 0;

// GitHub dashboard state (pulled from the Cloudflare Worker, tools/coindeck-worker/)
static int      gh_open_prs        = -1;   // -1 = no data
static int      gh_review_requested = 0;
static int      gh_ci_passed       = 0;
static int      gh_ci_failed       = 0;
static int      gh_ci_pending      = 0;
static uint32_t gh_last_update     = 0;
static const uint32_t GH_FRESH_MS  = 5UL * 60UL * 1000UL;

struct GhPr { int num; char title[56]; };
static const int GH_PR_MAX = 5;
static GhPr gh_prs[GH_PR_MAX];
static int  gh_pr_count = 0;
static GhPr gh_reviews[GH_PR_MAX];
static int  gh_review_count = 0;
static int  gh_issues_assigned = 0;
static GhPr gh_issues[GH_PR_MAX];
static int  gh_issue_count = 0;

// Worker poll cadence — must match the cache TTLs on the Worker side.
static const uint32_t GH_POLL_MS      = 120UL * 1000UL;
static const uint32_t USAGE_POLL_MS   = 60UL  * 1000UL;
static const uint32_t REMOTE_RETRY_MS = 15UL  * 1000UL;
static uint32_t next_gh_poll_at    = 0;
static uint32_t next_usage_poll_at = 0;

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

// Single-line coin row: ticker on the left, then price (white) + 24h % (green/red)
// on the right, all on one baseline so the row only needs ~24 px of height.
static void draw_coin_row(int16_t y, const char *ticker, uint16_t ticker_color,
                          const CoinSnapshot &c) {
  gfx->setTextSize(3);
  gfx->setTextColor(ticker_color);
  gfx->setCursor(MARGIN, y);
  gfx->print(ticker);

  if (!c.valid) {
    gfx->setTextSize(2);
    gfx->setTextColor(COL_DIM);
    const char *msg = "no data";
    gfx->setCursor(LCD_W - MARGIN - text_width(msg, 2), y + 4);
    gfx->print(msg);
    return;
  }

  char price_buf[24], pct_buf[16];
  format_money(price_buf, sizeof(price_buf), c.price_usd);
  format_pct(pct_buf, sizeof(pct_buf), c.change_24h_pct);

  int euro_w  = 6 * 3 + 2;                      // width that draw_euro reserves at size 3
  int price_w = text_width(price_buf, 3);
  int pct_w   = text_width(pct_buf, 2);
  const int gap = 14;
  int x_right = LCD_W - MARGIN;

  gfx->setTextSize(2);
  gfx->setTextColor(c.change_24h_pct >= 0 ? COL_OK : COL_ERR);
  gfx->setCursor(x_right - pct_w, y + 4);
  gfx->print(pct_buf);

  int price_x = x_right - pct_w - gap - price_w;
  gfx->setTextSize(3);
  gfx->setTextColor(COL_TITLE);
  gfx->setCursor(price_x, y);
  gfx->print(price_buf);

  draw_euro(price_x - euro_w, y, 3, COL_TITLE);
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

  // Temperature with a tiny circle for the degree sign (default font has no °).
  if (weather_code >= 0) {
    int temp = (int)(weather_temp_c + (weather_temp_c >= 0 ? 0.5f : -0.5f));
    char num_buf[8];
    snprintf(num_buf, sizeof(num_buf), "%d", temp);
    const int t_size  = 3;
    const int circle_r = 3;
    const int pad      = 2;
    int num_w   = text_width(num_buf, t_size);
    int c_w     = 6 * t_size;            // width of the 'C' glyph at this size
    int total_w = num_w + 2 * circle_r + pad + c_w;
    int sx      = rcenter - total_w / 2;

    gfx->setTextSize(t_size);
    gfx->setTextColor(COL_WARN);
    gfx->setCursor(sx, 75);
    gfx->print(num_buf);
    gfx->drawCircle(sx + num_w + circle_r, 75 + circle_r + 1, circle_r, COL_WARN);
    gfx->setCursor(sx + num_w + 2 * circle_r + pad, 75);
    gfx->print("C");

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

// Page 2: full-screen 5-coin grid (BTC, ETH, SOL, XRP, BNB). Single-line rows
// (ticker + price + %) so all five fit between header and footer.
static const int CRYPTO_ROW_H = 44;
static const int CRYPTO_ROW0  = 56;

static void draw_page_crypto() {
  gfx->fillScreen(COL_BG);

  gfx->setTextSize(3);
  gfx->setTextColor(COL_TITLE);
  const char *t = "CoinDeck";
  gfx->setCursor((LCD_W - text_width(t, 3)) / 2, 6);
  gfx->print(t);
  gfx->setTextSize(1);
  gfx->setTextColor(COL_DIM);
  const char *sub = "prices in EUR";
  gfx->setCursor((LCD_W - text_width(sub, 1)) / 2, 36);
  gfx->print(sub);

  for (int i = 0; i < COIN_COUNT; i++) {
    draw_coin_row(CRYPTO_ROW0 + i * CRYPTO_ROW_H,
                  COIN_NAMES[i], COIN_COLORS[i], coins[i]);
  }

  // Footer: "updated Xs ago" left, "HH:MM" right
  gfx->setTextSize(1);
  gfx->setTextColor(COL_DIM);
  char left[40];
  if (last_fetch_ok == 0) snprintf(left, sizeof(left), "fetching...");
  else snprintf(left, sizeof(left), "updated %lus ago",
                (unsigned long)((millis() - last_fetch_ok) / 1000));
  gfx->setCursor(MARGIN, LCD_H - 12);
  gfx->print(left);
  char clock_buf[8];
  if (format_time(clock_buf, sizeof(clock_buf))) {
    gfx->setCursor(LCD_W - MARGIN - text_width(clock_buf, 1), LCD_H - 12);
    gfx->print(clock_buf);
  }

  draw_page_indicator_at(LCD_H - 12);
  gfx->flush();
  grid_ready = true;
}

static const char *coin_name(int idx) {
  if (idx >= 0 && idx < COIN_COUNT) return COIN_NAMES[idx];
  return "?";
}
static uint16_t coin_color(int idx) {
  if (idx >= 0 && idx < COIN_COUNT) return COIN_COLORS[idx];
  return COL_DIM;
}
static const CoinSnapshot &coin_snap(int idx) {
  if (idx >= 0 && idx < COIN_COUNT) return coins[idx];
  return coins[0];
}

// --- Chart fetch + draw ---
static const int CHART_X0 = MARGIN;
static const int CHART_X1 = LCD_W - MARGIN;
static const int CHART_Y0 = 220;
static const int CHART_Y1 = 285;

static bool fetch_chart(int coin_idx) {
  if (coin_idx < 0 || coin_idx >= COIN_COUNT) return false;
  if (WiFi.status() != WL_CONNECTED) return false;
  const char *id = COIN_IDS[coin_idx];
  char url[160];
  snprintf(url, sizeof(url),
           "https://api.coingecko.com/api/v3/coins/%s/market_chart"
           "?vs_currency=eur&days=1", id);

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
    int euro_w = 6 * 4 + 2;
    int total  = euro_w + text_width(price_buf, 4);
    int px     = (LCD_W - total) / 2;
    draw_euro(px, 90, 4, COL_TITLE);
    gfx->setTextSize(4);
    gfx->setTextColor(COL_TITLE);
    gfx->setCursor(px + euro_w, 90);
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

// Strip Conventional-Commit style prefix: "feat(scope): title" -> "title".
static const char *strip_cc_prefix(const char *src) {
  const char *colon = strchr(src, ':');
  if (!colon) return src;
  const char *after = colon + 1;
  while (*after == ' ' || *after == '\t') after++;
  return *after ? after : src;
}

static void draw_page2() {
  gfx->fillScreen(COL_BG);
  gfx->drawFastVLine(DIVIDER_X, 0, LCD_H, COL_DIM);

  // Header
  gfx->setTextSize(2);
  gfx->setTextColor(COL_TITLE);
  const char *t = "GitHub";
  gfx->setCursor((LEFT_W - text_width(t, 2)) / 2, 12);
  gfx->print(t);

  bool has_data = gh_open_prs >= 0;
  bool live     = has_data && (millis() - gh_last_update < GH_FRESH_MS);
  uint16_t dot_color = !has_data ? COL_DIM : live ? COL_OK : COL_WARN;
  gfx->fillCircle(LEFT_W / 2 + text_width(t, 2) / 2 + 10, 20, 3, dot_color);

  // Right column = the weather/clock/Claude widgets that used to live on Page 1.
  draw_right_column();

  if (!has_data) {
    gfx->setTextSize(1);
    gfx->setTextColor(COL_DIM);
    const char *m = "waiting for daemon...";
    gfx->setCursor((LEFT_W - text_width(m, 1)) / 2, LCD_H / 2 - 4);
    gfx->print(m);
    draw_page_indicator_at(FOOTER_Y + 12);
    gfx->flush();
    grid_ready = true;
    return;
  }

  // Stacked sections: Open on top, Reviews below. Each: label + hline + rows.
  // Title size 2 (~12 px / char), max ~18 chars in the 260-px-wide column.
  auto draw_section = [&](int y_start, const char *label, int count,
                          uint16_t label_color, const GhPr *list,
                          int list_count, int max_rows) -> int {
    char hbuf[24];
    snprintf(hbuf, sizeof(hbuf), "%s (%d)", label, count);
    gfx->setTextSize(1);
    gfx->setTextColor(label_color);
    gfx->setCursor(12, y_start);
    gfx->print(hbuf);
    int y = y_start + 12;
    gfx->drawFastHLine(12, y, LEFT_W - 24, COL_DIM);
    y += 6;

    if (list_count == 0) {
      gfx->setTextColor(COL_DIM);
      gfx->setCursor(20, y + 2);
      gfx->print("-");
      return y + 20;
    }

    // setTextSize(1, 2) = same 6-px width as size 1 but 16-px tall — a "1.5x" feel.
    const int row_h = 18;
    int rows = list_count < max_rows ? list_count : max_rows;
    for (int i = 0; i < rows; i++) {
      char numbuf[8];
      snprintf(numbuf, sizeof(numbuf), "#%d", list[i].num);
      int num_w = text_width(numbuf, 1);
      gfx->setTextSize(1, 2);
      gfx->setTextColor(COL_DIM);
      gfx->setCursor(12, y + i * row_h);
      gfx->print(numbuf);

      const char *cleaned = strip_cc_prefix(list[i].title);
      int avail     = LEFT_W - 24 - num_w - 6;
      int max_chars = avail / 6;
      if (max_chars < 1) max_chars = 1;
      char title[64];
      if ((int)strlen(cleaned) > max_chars) {
        int keep = max_chars - 1;
        if (keep < 1) keep = 1;
        strncpy(title, cleaned, keep);
        title[keep] = '~';
        title[keep + 1] = 0;
      } else {
        strcpy(title, cleaned);
      }
      gfx->setTextColor(COL_TITLE);
      gfx->setCursor(12 + num_w + 6, y + i * row_h);
      gfx->print(title);
    }
    return y + rows * row_h + 4;
  };

  int y = 36;
  y = draw_section(y, "Open",    gh_open_prs,         COL_TITLE,
                   gh_prs,     gh_pr_count, 5);
  y += 4;
  draw_section(y, "Reviews", gh_review_requested,
               gh_review_requested > 0 ? COL_WARN : COL_DIM,
               gh_reviews, gh_review_count, 3);

  // CI line at bottom of left column
  int y_ci = FOOTER_Y - 6;
  gfx->setTextSize(1);
  char nbuf[8];
  snprintf(nbuf, sizeof(nbuf), "%d", gh_ci_passed);  int wp = text_width(nbuf, 1);
  snprintf(nbuf, sizeof(nbuf), "%d", gh_ci_failed);  int wf = text_width(nbuf, 1);
  snprintf(nbuf, sizeof(nbuf), "%d", gh_ci_pending); int wn = text_width(nbuf, 1);
  const char *s_pass = " ok  ", *s_fail = " fail  ", *s_pend = " pend";
  int total = wp + text_width(s_pass, 1) + wf + text_width(s_fail, 1) + wn + text_width(s_pend, 1);
  int x = (LEFT_W - total) / 2;
  gfx->setTextColor(COL_OK);  snprintf(nbuf, sizeof(nbuf), "%d", gh_ci_passed);
  gfx->setCursor(x, y_ci); gfx->print(nbuf); x += wp;
  gfx->setTextColor(COL_DIM); gfx->setCursor(x, y_ci); gfx->print(s_pass); x += text_width(s_pass, 1);
  gfx->setTextColor(COL_ERR); snprintf(nbuf, sizeof(nbuf), "%d", gh_ci_failed);
  gfx->setCursor(x, y_ci); gfx->print(nbuf); x += wf;
  gfx->setTextColor(COL_DIM); gfx->setCursor(x, y_ci); gfx->print(s_fail); x += text_width(s_fail, 1);
  gfx->setTextColor(COL_WARN);snprintf(nbuf, sizeof(nbuf), "%d", gh_ci_pending);
  gfx->setCursor(x, y_ci); gfx->print(nbuf); x += wn;
  gfx->setTextColor(COL_DIM); gfx->setCursor(x, y_ci); gfx->print(s_pend);

  draw_page_indicator_at(FOOTER_Y + 12);
  gfx->flush();
  grid_ready = true;
}

static void draw_github_detail() {
  gfx->fillScreen(COL_BG);

  // Small/condensed rendering — same trick we use elsewhere: 6 px wide, 16 px tall.
  auto draw_pr_row = [&](int y, const GhPr &pr) {
    char numbuf[8];
    snprintf(numbuf, sizeof(numbuf), "#%d", pr.num);
    int num_w = text_width(numbuf, 1);
    gfx->setTextSize(1, 2);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(MARGIN, y);
    gfx->print(numbuf);

    const char *cleaned = strip_cc_prefix(pr.title);
    int avail = LCD_W - MARGIN * 2 - num_w - 8;
    int max_chars = avail / 6;
    if (max_chars < 1) max_chars = 1;
    char title[80];
    if ((int)strlen(cleaned) > max_chars) {
      int keep = max_chars - 1;
      if (keep < 1) keep = 1;
      strncpy(title, cleaned, keep);
      title[keep] = '~';
      title[keep + 1] = 0;
    } else {
      strcpy(title, cleaned);
    }
    gfx->setTextColor(COL_TITLE);
    gfx->setCursor(MARGIN + num_w + 8, y);
    gfx->print(title);
  };

  auto draw_section = [&](int y, const char *label, int count, uint16_t label_color,
                          const GhPr *list, int list_count, int max_rows,
                          int *out_top = nullptr, int *out_bot = nullptr) -> int {
    int section_top = y;
    char hbuf[32];
    snprintf(hbuf, sizeof(hbuf), "%s (%d)", label, count);
    gfx->setTextSize(2);
    gfx->setTextColor(label_color);
    gfx->setCursor(MARGIN, y);
    gfx->print(hbuf);
    y += 18;
    gfx->drawFastHLine(MARGIN, y, LCD_W - MARGIN * 2, COL_DIM);
    y += 4;

    if (list_count == 0) {
      gfx->setTextSize(1, 2);
      gfx->setTextColor(COL_DIM);
      gfx->setCursor(MARGIN + 4, y);
      gfx->print("-");
      y += 18;
    } else {
      int rows = list_count < max_rows ? list_count : max_rows;
      const int row_h = 18;
      for (int i = 0; i < rows; i++) {
        draw_pr_row(y, list[i]);
        y += row_h;
      }
    }
    if (out_top) *out_top = section_top;
    if (out_bot) *out_bot = y;
    return y;
  };

  int y = 8;
  y = draw_section(y, "Open PRs", gh_open_prs, COL_TITLE,
                   gh_prs, gh_pr_count, 5);
  y += 6;
  y = draw_section(y, "Reviews", gh_review_requested,
                   gh_review_requested > 0 ? COL_WARN : COL_TITLE,
                   gh_reviews, gh_review_count, 3);
  y += 6;
  draw_section(y, "Issues", gh_issues_assigned,
               gh_issues_assigned > 0 ? COL_WARN : COL_TITLE,
               gh_issues, gh_issue_count, 3,
               &gh_issues_band_top, &gh_issues_band_bot);

  gfx->setTextSize(1);
  gfx->setTextColor(COL_DIM);
  const char *hint = "tap Issues for full list  -  tap elsewhere = back";
  gfx->setCursor((LCD_W - text_width(hint, 1)) / 2, LCD_H - 10);
  gfx->print(hint);

  gfx->flush();
}

static void draw_issues_detail() {
  gfx->fillScreen(COL_BG);

  gfx->setTextSize(3);
  gfx->setTextColor(COL_WARN);
  char hbuf[24];
  snprintf(hbuf, sizeof(hbuf), "Issues (%d)", gh_issues_assigned);
  gfx->setCursor(MARGIN, 8);
  gfx->print(hbuf);
  gfx->drawFastHLine(MARGIN, 38, LCD_W - 2 * MARGIN, COL_DIM);

  if (gh_issue_count == 0) {
    gfx->setTextSize(2);
    gfx->setTextColor(COL_DIM);
    const char *m = "no open issues assigned to you";
    gfx->setCursor((LCD_W - text_width(m, 2)) / 2, LCD_H / 2);
    gfx->print(m);
  } else {
    int y = 50;
    const int row_h = 28;
    for (int i = 0; i < gh_issue_count && y + row_h < LCD_H - 16; i++) {
      char numbuf[8];
      snprintf(numbuf, sizeof(numbuf), "#%d", gh_issues[i].num);
      int num_w = text_width(numbuf, 2);
      gfx->setTextSize(2);
      gfx->setTextColor(COL_DIM);
      gfx->setCursor(MARGIN, y);
      gfx->print(numbuf);

      const char *cleaned = strip_cc_prefix(gh_issues[i].title);
      int avail = LCD_W - 2 * MARGIN - num_w - 10;
      int max_chars = avail / 12;
      char title[64];
      if ((int)strlen(cleaned) > max_chars) {
        int keep = max_chars - 1;
        if (keep < 1) keep = 1;
        strncpy(title, cleaned, keep);
        title[keep] = '~';
        title[keep + 1] = 0;
      } else {
        strcpy(title, cleaned);
      }
      gfx->setTextColor(COL_TITLE);
      gfx->setCursor(MARGIN + num_w + 10, y);
      gfx->print(title);
      y += row_h;
    }
  }

  gfx->setTextSize(1);
  gfx->setTextColor(COL_DIM);
  const char *hint = "tap to go back";
  gfx->setCursor((LCD_W - text_width(hint, 1)) / 2, LCD_H - 10);
  gfx->print(hint);

  gfx->flush();
}

static void draw_page_wifi();
static void draw_wifi_scan();
static void draw_wifi_key();
static void ntp_begin();
static bool fetch_weather();
static void wifi_save_creds(const char *ssid, const char *pass);

static void redraw_current_screen() {
  if (current_screen == SCREEN_DETAIL)         { draw_detail();         return; }
  if (current_screen == SCREEN_GITHUB_DETAIL)  { draw_github_detail();  return; }
  if (current_screen == SCREEN_ISSUES_DETAIL)  { draw_issues_detail();  return; }
  if (current_screen == SCREEN_WIFI_SCAN)      { draw_wifi_scan();      return; }
  if (current_screen == SCREEN_WIFI_KEY)       { draw_wifi_key();       return; }
  if (current_page == 0)      draw_page2();
  else if (current_page == 1) draw_page_crypto();
  else                        draw_page_wifi();
}

static void draw_page_wifi() {
  gfx->fillScreen(COL_BG);

  gfx->setTextSize(3);
  gfx->setTextColor(COL_TITLE);
  const char *title = "WiFi";
  gfx->setCursor((LCD_W - text_width(title, 3)) / 2, 18);
  gfx->print(title);

  bool up = WiFi.status() == WL_CONNECTED;

  gfx->setTextSize(2);
  gfx->setTextColor(COL_DIM); gfx->setCursor(40, 75);  gfx->print("SSID:");
  gfx->setTextColor(up ? COL_OK : COL_ERR);
  gfx->setCursor(140, 75);
  gfx->print(up ? WiFi.SSID().c_str() : "not connected");

  if (up) {
    gfx->setTextColor(COL_DIM); gfx->setCursor(40, 110); gfx->print("IP:");
    gfx->setTextColor(COL_TITLE); gfx->setCursor(140, 110);
    gfx->print(WiFi.localIP().toString().c_str());

    gfx->setTextColor(COL_DIM); gfx->setCursor(40, 145); gfx->print("RSSI:");
    gfx->setTextColor(COL_TITLE); gfx->setCursor(140, 145);
    gfx->printf("%d dBm", (int)WiFi.RSSI());
  }

  const int bw = 360, bh = 56;
  const int bx = (LCD_W - bw) / 2, by = 205;
  gfx->fillRoundRect(bx, by, bw, bh, 10, COL_WARN);
  gfx->setTextColor(COL_BG);
  const char *btn = "Tap to switch WiFi";
  gfx->setCursor(bx + (bw - text_width(btn, 2)) / 2, by + 20);
  gfx->print(btn);

  draw_page_indicator_at(FOOTER_Y + 12);
  gfx->flush();
}

// --- On-screen WiFi setup: scan + soft keyboard ----------------------------

static void wifi_do_scan() {
  wifi_scan_busy  = true;
  wifi_list_count = 0;
  redraw_current_screen();

  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks();
  if (n < 0) n = 0;
  if (n > 64) n = 64;

  // Sort indices by RSSI desc.
  int idx[64];
  for (int i = 0; i < n; i++) idx[i] = i;
  for (int i = 0; i < n; i++)
    for (int j = i + 1; j < n; j++)
      if (WiFi.RSSI(idx[j]) > WiFi.RSSI(idx[i])) { int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }

  int taken = n < WIFI_LIST_MAX ? n : WIFI_LIST_MAX;
  for (int i = 0; i < taken; i++) {
    int k = idx[i];
    strncpy(wifi_list[i].ssid, WiFi.SSID(k).c_str(), sizeof(wifi_list[i].ssid) - 1);
    wifi_list[i].ssid[sizeof(wifi_list[i].ssid) - 1] = 0;
    wifi_list[i].rssi = WiFi.RSSI(k);
    wifi_list[i].open = WiFi.encryptionType(k) == WIFI_AUTH_OPEN;
  }
  wifi_list_count = taken;
  WiFi.scanDelete();
  wifi_scan_busy = false;
}

static void draw_wifi_scan() {
  gfx->fillScreen(COL_BG);

  gfx->setTextSize(2);
  gfx->setTextColor(COL_TITLE);
  gfx->setCursor(8, 8);
  gfx->print("Pick a network");

  gfx->setTextSize(1);
  gfx->setTextColor(COL_DIM);
  const char *back = "tap empty area to cancel";
  gfx->setCursor(LCD_W - text_width(back, 1) - 8, 14);
  gfx->print(back);

  if (wifi_scan_busy) {
    gfx->setTextSize(2);
    gfx->setTextColor(COL_WARN);
    const char *m = "Scanning...";
    gfx->setCursor((LCD_W - text_width(m, 2)) / 2, LCD_H / 2 - 8);
    gfx->print(m);
  } else if (wifi_list_count == 0) {
    gfx->setTextSize(2);
    gfx->setTextColor(COL_DIM);
    const char *m = "No networks found";
    gfx->setCursor((LCD_W - text_width(m, 2)) / 2, LCD_H / 2 - 8);
    gfx->print(m);
  } else {
    const int row_h = 26;
    int y = 40;
    for (int i = 0; i < wifi_list_count; i++) {
      char buf[34];
      strncpy(buf, wifi_list[i].ssid, sizeof(buf) - 1);
      buf[sizeof(buf) - 1] = 0;
      while (strlen(buf) > 0 && text_width(buf, 2) > LCD_W - 130) buf[strlen(buf) - 1] = 0;

      gfx->setTextSize(2);
      gfx->setTextColor(COL_TITLE);
      gfx->setCursor(8, y);
      gfx->print(buf);

      gfx->setTextSize(1);
      uint16_t col = wifi_list[i].rssi > -60 ? COL_OK
                   : wifi_list[i].rssi > -75 ? COL_WARN : COL_ERR;
      gfx->setTextColor(col);
      char rb[20];
      snprintf(rb, sizeof(rb), "%ddBm%s", wifi_list[i].rssi, wifi_list[i].open ? " open" : "");
      int w = text_width(rb, 1);
      gfx->setCursor(LCD_W - w - 8, y + 5);
      gfx->print(rb);

      y += row_h;
      if (y > LCD_H - row_h) break;
    }
  }
  gfx->flush();
}

static void draw_key(int x, int y, int w, int h, const char *label, uint16_t bg, uint16_t fg) {
  gfx->fillRoundRect(x + 1, y + 1, w - 2, h - 2, 4, bg);
  gfx->setTextSize(2);
  gfx->setTextColor(fg);
  int lw = text_width(label, 2);
  gfx->setCursor(x + (w - lw) / 2, y + (h - 16) / 2);
  gfx->print(label);
}

// Keyboard layout (480x320, rows below y=80):
//   Row 0 [80,128):  1234567890     (10 keys @ 48)
//   Row 1 [128,176): qwertyuiop
//   Row 2 [176,224): asdfghjkl      (9 keys, x offset 24)
//   Row 3 [224,272): SHIFT z x c v b n m DEL
//   Row 4 [272,320): CANCEL  SPACE  OK
static void draw_wifi_key() {
  gfx->fillScreen(COL_BG);

  gfx->setTextSize(2);
  gfx->setTextColor(COL_DIM);  gfx->setCursor(8, 6);  gfx->print("SSID:");
  gfx->setTextColor(COL_OK);   gfx->setCursor(80, 6); gfx->print(wifi_pick_ssid);

  gfx->setTextColor(COL_DIM);  gfx->setCursor(8, 34); gfx->print("Pass:");
  gfx->setCursor(80, 34);
  if (wifi_pass_len == 0) {
    gfx->setTextColor(COL_DIM); gfx->print("(tap keys)");
  } else {
    gfx->setTextColor(COL_TITLE);
    gfx->print(wifi_pass_buf);
  }

  if (wifi_status_msg[0]) {
    gfx->setTextSize(1);
    gfx->setTextColor(COL_WARN);
    gfx->setCursor(8, 60);
    gfx->print(wifi_status_msg);
  }

  const char *r0 = "1234567890";
  for (int i = 0; i < 10; i++) {
    char s[2] = { r0[i], 0 };
    draw_key(i * 48, 80, 48, 48, s, COL_DIM, COL_TITLE);
  }
  const char *r1 = "qwertyuiop";
  for (int i = 0; i < 10; i++) {
    char s[2] = { (char)(wifi_caps_on ? r1[i] - 32 : r1[i]), 0 };
    draw_key(i * 48, 128, 48, 48, s, COL_DIM, COL_TITLE);
  }
  const char *r2 = "asdfghjkl";
  for (int i = 0; i < 9; i++) {
    char s[2] = { (char)(wifi_caps_on ? r2[i] - 32 : r2[i]), 0 };
    draw_key(24 + i * 48, 176, 48, 48, s, COL_DIM, COL_TITLE);
  }
  draw_key(0, 224, 72, 48, "^", wifi_caps_on ? COL_WARN : COL_DIM, COL_TITLE);
  const char *r3 = "zxcvbnm";
  for (int i = 0; i < 7; i++) {
    char s[2] = { (char)(wifi_caps_on ? r3[i] - 32 : r3[i]), 0 };
    draw_key(72 + i * 48, 224, 48, 48, s, COL_DIM, COL_TITLE);
  }
  draw_key(408, 224, 72, 48, "DEL", COL_DIM, COL_TITLE);
  draw_key(0,   272, 96, 48, "CANCEL", COL_ERR, COL_TITLE);
  draw_key(96,  272, 288, 48, "space", COL_DIM, COL_TITLE);
  draw_key(384, 272, 96, 48, "OK",  COL_OK,  COL_BG);

  gfx->flush();
}

// Returns ASCII for letter/digit, or one of:
//   -2 SHIFT, -3 DEL, -4 SPACE, -5 OK, -6 CANCEL, -1 miss.
static int key_at(int x, int y) {
  if (y < 80 || y >= 320) return -1;
  if (y < 128) {
    int c = x / 48; if (c < 0 || c > 9) return -1;
    return "1234567890"[c];
  }
  if (y < 176) {
    int c = x / 48; if (c < 0 || c > 9) return -1;
    char k = "qwertyuiop"[c]; return wifi_caps_on ? k - 32 : k;
  }
  if (y < 224) {
    if (x < 24 || x >= 24 + 9 * 48) return -1;
    char k = "asdfghjkl"[(x - 24) / 48];
    return wifi_caps_on ? k - 32 : k;
  }
  if (y < 272) {
    if (x < 72)   return -2;
    if (x >= 408) return -3;
    int c = (x - 72) / 48; if (c < 0 || c > 6) return -1;
    char k = "zxcvbnm"[c]; return wifi_caps_on ? k - 32 : k;
  }
  if (x < 96)  return -6;
  if (x >= 384) return -5;
  return -4;
}

static void goto_wifi_scan() {
  current_screen = SCREEN_WIFI_SCAN;
  wifi_status_msg = "";
  wifi_do_scan();
  redraw_current_screen();
}

static void goto_wifi_key(const char *ssid) {
  strncpy(wifi_pick_ssid, ssid, sizeof(wifi_pick_ssid) - 1);
  wifi_pick_ssid[sizeof(wifi_pick_ssid) - 1] = 0;
  wifi_pass_buf[0] = 0;
  wifi_pass_len   = 0;
  wifi_caps_on    = false;
  wifi_status_msg = "";
  current_screen  = SCREEN_WIFI_KEY;
  redraw_current_screen();
}

static void wifi_try_connect_from_key() {
  wifi_status_msg = "Connecting...";
  redraw_current_screen();

  WiFi.disconnect(false, false);
  delay(150);
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_pick_ssid, wifi_pass_buf);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) delay(200);

  if (WiFi.status() == WL_CONNECTED) {
    saved_ssid = wifi_pick_ssid;
    saved_pass = wifi_pass_buf;
    wifi_save_creds(wifi_pick_ssid, wifi_pass_buf);
    Serial.printf("WiFi OK (on-screen). IP=%s SSID=%s\n",
                  WiFi.localIP().toString().c_str(), wifi_pick_ssid);
    ntp_begin();
    fetch_weather();
    current_screen = SCREEN_LIST;
    current_page   = 2;
    redraw_current_screen();
  } else {
    wifi_status_msg = "Failed. Wrong pass?";
    wifi_pass_len   = 0;
    wifi_pass_buf[0] = 0;
    redraw_current_screen();
  }
}

static void goto_list() {
  current_screen = SCREEN_LIST;
  redraw_current_screen();
}

static void goto_detail(int coin_idx) {
  current_coin = (coin_idx + COIN_COUNT) % COIN_COUNT;
  current_screen = SCREEN_DETAIL;
  redraw_current_screen();
}

static void goto_github_detail() {
  current_screen = SCREEN_GITHUB_DETAIL;
  redraw_current_screen();
}

static void goto_issues_detail() {
  current_screen = SCREEN_ISSUES_DETAIL;
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

// Persist WiFi creds in our own NVS namespace — independent of WiFiManager's
// storage so the same saved value is found whether the creds came from the
// portal or the on-screen soft keyboard.
static void wifi_save_creds(const char *ssid, const char *pass) {
  Preferences p;
  if (!p.begin("coindeck", false)) return;
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.end();
}

static bool wifi_load_creds(String &ssid, String &pass) {
  Preferences p;
  if (!p.begin("coindeck", true)) return false;
  ssid = p.getString("ssid", "");
  pass = p.getString("pass", "");
  p.end();
  return ssid.length() > 0;
}

static void wifi_portal_screen(WiFiManager *) {
  digitalWrite(TFT_BL, HIGH);
  gfx->fillScreen(COL_BG);
  gfx->setTextSize(3);
  gfx->setTextColor(COL_WARN);
  gfx->setCursor(20, 25);
  gfx->print("WiFi setup");

  gfx->setTextSize(2);
  gfx->setTextColor(COL_TITLE);
  gfx->setCursor(20, 85);
  gfx->print("1. Phone -> WiFi:");
  gfx->setTextColor(COL_OK);
  gfx->setCursor(20, 115);
  gfx->print("   CoinDeck-Setup");

  gfx->setTextColor(COL_TITLE);
  gfx->setCursor(20, 160);
  gfx->print("2. Open browser:");
  gfx->setTextColor(COL_OK);
  gfx->setCursor(20, 190);
  gfx->print("   http://192.168.4.1");
  gfx->setTextColor(COL_DIM);
  gfx->setCursor(20, 215);
  gfx->print("   (auto-opens on most");
  gfx->setCursor(20, 235);
  gfx->print("    Androids; iOS may");
  gfx->setCursor(20, 255);
  gfx->print("    need manual visit)");

  gfx->setTextSize(1);
  gfx->setCursor(20, 285);
  gfx->print("Times out after 3 min.");
  gfx->flush();
}

// allow_portal=true on first boot or when saved creds fail: opens AP captive
// portal. allow_portal=false in the loop reconnect path so a brief WiFi blip
// doesn't kick the user out of CoinDeck into setup mode.
static bool wifi_connect(bool allow_portal) {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  status_line(150, COL_WARN, "connecting wifi...");

  String s, p;
  if (saved_ssid.length() == 0) wifi_load_creds(saved_ssid, saved_pass);

  if (saved_ssid.length() > 0) {
    WiFi.begin(saved_ssid.c_str(), saved_pass.c_str());
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) delay(200);
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("WiFi OK. IP=%s SSID=%s RSSI=%d dBm\n",
                    WiFi.localIP().toString().c_str(),
                    WiFi.SSID().c_str(), (int)WiFi.RSSI());
      ntp_begin();
      return true;
    }
  }

  if (!allow_portal) return false;

  // No saved creds, or saved network out of range — drop into the on-screen
  // scan + keyboard. Loop drives the rest; nothing else in setup() depends on
  // WiFi being up.
  Serial.println("No usable saved WiFi — entering on-screen setup.");
  goto_wifi_scan();
  return false;
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

  for (int i = 0; i < COIN_COUNT; i++) {
    coins[i] = {
      doc[COIN_IDS[i]]["eur"]            | 0.0f,
      doc[COIN_IDS[i]]["eur_24h_change"] | 0.0f,
      true
    };
  }
  Serial.printf("BTC EUR %.2f %+6.2f%% | ETH EUR %.2f %+6.2f%% | SOL EUR %.2f %+6.2f%% | "
                "XRP EUR %.4f %+6.2f%% | BNB EUR %.2f %+6.2f%%\n",
                coins[0].price_usd, coins[0].change_24h_pct,
                coins[1].price_usd, coins[1].change_24h_pct,
                coins[2].price_usd, coins[2].change_24h_pct,
                coins[3].price_usd, coins[3].change_24h_pct,
                coins[4].price_usd, coins[4].change_24h_pct);
  return true;
}

static bool fetch_worker_json(const char *path, JsonDocument &doc) {
  if (WiFi.status() != WL_CONNECTED) return false;
  char url[256];
  snprintf(url, sizeof(url), "%s%s", WORKER_BASE_URL, path);
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) return false;
  http.setUserAgent("CoinDeck/0.3");
  http.setTimeout(12000);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("worker %s HTTP %d\n", path, code);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("worker %s JSON: %s\n", path, err.c_str());
    return false;
  }
  return true;
}

static bool fetch_usage() {
  JsonDocument doc;
  if (!fetch_worker_json("/usage", doc)) return false;
  claude_session_pct       = doc["session_pct"]       | -1;
  claude_session_reset_min = doc["session_reset_min"] | 0;
  claude_weekly_pct        = doc["weekly_pct"]        | -1;
  claude_weekly_reset_min  = doc["weekly_reset_min"]  | 0;
  claude_last_update = millis();
  Serial.printf("[claude] 5h=%d%% (reset %dm) | 7d=%d%% (reset %dm)\n",
                claude_session_pct, claude_session_reset_min,
                claude_weekly_pct,  claude_weekly_reset_min);
  if (current_screen == SCREEN_LIST && grid_ready) redraw_current_screen();
  return true;
}

static bool fetch_github() {
  JsonDocument doc;
  if (!fetch_worker_json("/github", doc)) return false;
  gh_open_prs         = doc["open_prs"]         | -1;
  gh_review_requested = doc["review_requested"] | 0;
  gh_issues_assigned  = doc["issues_assigned"]  | 0;
  gh_ci_passed        = doc["ci_passed"]        | 0;
  gh_ci_failed        = doc["ci_failed"]        | 0;
  gh_ci_pending       = doc["ci_pending"]       | 0;
  gh_last_update      = millis();

  auto copy_list = [](JsonArray arr, GhPr *dst, int &count) {
    count = 0;
    for (JsonObject pr : arr) {
      if (count >= GH_PR_MAX) break;
      dst[count].num = pr["n"] | 0;
      const char *t = pr["t"] | "";
      strncpy(dst[count].title, t, sizeof(dst[count].title) - 1);
      dst[count].title[sizeof(dst[count].title) - 1] = 0;
      count++;
    }
  };
  copy_list(doc["prs"].as<JsonArray>(),     gh_prs,     gh_pr_count);
  copy_list(doc["reviews"].as<JsonArray>(), gh_reviews, gh_review_count);
  copy_list(doc["issues"].as<JsonArray>(),  gh_issues,  gh_issue_count);

  Serial.printf("[github] PRs=%d/%d  reviews=%d/%d  issues=%d/%d  ci=%dP/%dF/%d~\n",
                gh_open_prs, gh_pr_count, gh_review_requested, gh_review_count,
                gh_issues_assigned, gh_issue_count,
                gh_ci_passed, gh_ci_failed, gh_ci_pending);
  if ((current_screen == SCREEN_LIST && current_page == 0) ||
       current_screen == SCREEN_GITHUB_DETAIL ||
       current_screen == SCREEN_ISSUES_DETAIL) {
    redraw_current_screen();
  }
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

  wifi_connect(true);
  fetch_weather();
}

void loop() {
  static uint32_t last_footer_tick = 0;

  // Advance the Clawd animation frame when its hold time elapses.
  if (millis() - clawd_frame_started >= splash_idle_breathe_holds[clawd_frame]) {
    clawd_frame = (clawd_frame + 1) % CLAWD_FRAMES;
    clawd_frame_started = millis();
    if (current_screen == SCREEN_LIST && grid_ready) redraw_current_screen();
  }

  if (WiFi.status() != WL_CONNECTED
      && current_screen != SCREEN_WIFI_SCAN
      && current_screen != SCREEN_WIFI_KEY) {
    grid_ready = false;
    status_line(150, COL_WARN, "wifi lost, retry");
    wifi_connect(false);
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

  if ((int32_t)(millis() - next_gh_poll_at) >= 0) {
    next_gh_poll_at = millis() + (fetch_github() ? GH_POLL_MS : REMOTE_RETRY_MS);
  }
  if ((int32_t)(millis() - next_usage_poll_at) >= 0) {
    next_usage_poll_at = millis() + (fetch_usage() ? USAGE_POLL_MS : REMOTE_RETRY_MS);
  }

  // Per-second clock tick — only redraw the visible frame when the displayed minute would actually change.
  // Skip during the WiFi setup screens so the keyboard / scan list doesn't repaint mid-tap.
  if (grid_ready && current_screen != SCREEN_WIFI_SCAN && current_screen != SCREEN_WIFI_KEY
      && millis() - last_footer_tick > 1000) {
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
      } else if (current_screen == SCREEN_LIST) {
        // swipe-left → next page, swipe-right → previous
        int next = current_page + (dir > 0 ? -1 : +1);
        if (next >= 0 && next < PAGE_COUNT && next != current_page) {
          current_page = next;
          redraw_current_screen();
        }
      }
      // swipe on SCREEN_GITHUB_DETAIL: ignore for now
    } else {
      Serial.printf("TAP x=%d y=%d\n", last_x, last_y);
      if (current_screen == SCREEN_LIST) {
        if (current_page == 1) {
          // Page 1 crypto: tap a coin row to open detail.
          for (int i = 0; i < COIN_COUNT; i++) {
            int top = CRYPTO_ROW0 + i * CRYPTO_ROW_H;
            if (last_y >= top && last_y < top + CRYPTO_ROW_H) {
              goto_detail(i);
              break;
            }
          }
        } else if (current_page == 0 && last_x < LEFT_W) {
          // Page 0 GitHub box (anywhere in left column) → full-screen detail.
          goto_github_detail();
        } else if (current_page == 2) {
          goto_wifi_scan();
        }
      } else if (current_screen == SCREEN_WIFI_SCAN) {
        const int row_h = 26, start_y = 40;
        int idx = (last_y - start_y) / row_h;
        if (!wifi_scan_busy && wifi_list_count > 0
            && last_y >= start_y && idx >= 0 && idx < wifi_list_count) {
          goto_wifi_key(wifi_list[idx].ssid);
        } else if (!wifi_scan_busy) {
          current_screen = SCREEN_LIST; current_page = 2; redraw_current_screen();
        }
      } else if (current_screen == SCREEN_WIFI_KEY) {
        int k = key_at(last_x, last_y);
        if      (k == -6) { current_screen = SCREEN_LIST; current_page = 2; redraw_current_screen(); }
        else if (k == -5) { wifi_try_connect_from_key(); }
        else if (k == -4) {
          if (wifi_pass_len < (int)sizeof(wifi_pass_buf) - 1) {
            wifi_pass_buf[wifi_pass_len++] = ' ';
            wifi_pass_buf[wifi_pass_len]   = 0;
            redraw_current_screen();
          }
        } else if (k == -3) {
          if (wifi_pass_len > 0) { wifi_pass_buf[--wifi_pass_len] = 0; redraw_current_screen(); }
        } else if (k == -2) {
          wifi_caps_on = !wifi_caps_on; redraw_current_screen();
        } else if (k > 0) {
          if (wifi_pass_len < (int)sizeof(wifi_pass_buf) - 1) {
            wifi_pass_buf[wifi_pass_len++] = (char)k;
            wifi_pass_buf[wifi_pass_len]   = 0;
            redraw_current_screen();
          }
        }
      } else if (current_screen == SCREEN_GITHUB_DETAIL) {
        // Tap in the Issues band opens issues-only page; tap elsewhere goes back.
        if (gh_issues_band_top >= 0 && last_y >= gh_issues_band_top &&
            last_y <= gh_issues_band_bot) {
          goto_issues_detail();
        } else {
          goto_list();
        }
      } else if (current_screen == SCREEN_ISSUES_DETAIL) {
        goto_github_detail();
      } else {
        goto_list();
      }
    }
  }

  delay(20);
}
