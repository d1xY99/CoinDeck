#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>

#include "secrets.h"

#define LCD_SDIO0   4
#define LCD_SDIO1   5
#define LCD_SDIO2   6
#define LCD_SDIO3   7
#define LCD_SCLK    11
#define LCD_CS      12
#define LCD_WIDTH   368
#define LCD_HEIGHT  448

#define IIC_SDA      15
#define IIC_SCL      14
#define XCA9554_ADDR 0x20
#define FT3168_ADDR  0x38
#define TP_INT       21

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

Arduino_SH8601 *gfx = new Arduino_SH8601(
    bus, GFX_NOT_DEFINED, 0, LCD_WIDTH, LCD_HEIGHT);

static const uint16_t COL_BG    = RGB565_BLACK;
static const uint16_t COL_TITLE = RGB565_WHITE;
static const uint16_t COL_OK    = RGB565(0,    255, 100);  // green
static const uint16_t COL_ERR   = RGB565(255,  60,  60);   // red
static const uint16_t COL_WARN  = RGB565(255,  165, 0);    // orange
static const uint16_t COL_DIM   = RGB565(120,  120, 120);  // dim gray
static const uint16_t COL_BTC   = RGB565(247,  147, 26);   // bitcoin orange
static const uint16_t COL_ETH   = RGB565(132,  157, 235);  // ethereum violet-blue
static const uint16_t COL_SOL   = RGB565(153,  69,  255);  // solana purple

// --- State ---
struct CoinSnapshot {
  float price_usd;
  float change_24h_pct;
  bool  valid;
};
static CoinSnapshot btc{}, eth{}, sol{};

static const uint32_t FETCH_INTERVAL_MS = 30000;
static const uint32_t RETRY_INTERVAL_MS = 5000;
static uint32_t next_fetch_at  = 0;
static uint32_t last_fetch_ok  = 0;       // millis() of last successful fetch
static uint32_t fetch_count    = 0;
static uint32_t fetch_failed   = 0;
static bool     grid_ready     = false;   // is the coin grid currently on screen?

enum Screen { SCREEN_LIST, SCREEN_DETAIL };
static Screen current_screen = SCREEN_LIST;
static int    current_coin   = 0;   // 0=BTC, 1=ETH, 2=SOL — used by SCREEN_DETAIL

static const char *COINGECKO_URL =
    "https://api.coingecko.com/api/v3/simple/price"
    "?ids=bitcoin,ethereum,solana&vs_currencies=usd&include_24hr_change=true";

static bool xca_write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(XCA9554_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool expander_reset_sequence() {
  Wire.beginTransmission(XCA9554_ADDR);
  if (Wire.endTransmission() != 0) return false;
  if (!xca_write(0x03, 0xF8)) return false;
  if (!xca_write(0x01, 0x00)) return false;
  delay(20);
  if (!xca_write(0x01, 0x07)) return false;
  delay(50);
  return true;
}

// --- FT3168 touch ---
static bool touch_begin() {
  Wire.beginTransmission(FT3168_ADDR);
  if (Wire.endTransmission() != 0) return false;
  // Reg 0xA5 = power mode; 0x01 = monitor (low-power, wake on touch).
  Wire.beginTransmission(FT3168_ADDR);
  Wire.write(0xA5);
  Wire.write(0x01);
  Wire.endTransmission();
  pinMode(TP_INT, INPUT_PULLUP);
  return true;
}

static bool touch_read(int16_t &x, int16_t &y) {
  Wire.beginTransmission(FT3168_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)FT3168_ADDR, 5) != 5) return false;
  uint8_t fingers = Wire.read();
  uint8_t xh = Wire.read();
  uint8_t xl = Wire.read();
  uint8_t yh = Wire.read();
  uint8_t yl = Wire.read();
  if (fingers == 0 || fingers > 5) return false;
  x = ((xh & 0x0F) << 8) | xl;
  y = ((yh & 0x0F) << 8) | yl;
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

// --- Drawing primitives ---
static void draw_header() {
  gfx->setTextSize(4);
  gfx->setTextColor(COL_TITLE);
  int16_t w = text_width("CoinDeck", 4);
  gfx->setCursor((LCD_WIDTH - w) / 2, 32);
  gfx->print("CoinDeck");

  gfx->setTextSize(1);
  gfx->setTextColor(COL_DIM);
  const char *sub = "crypto desk ticker";
  w = text_width(sub, 1);
  gfx->setCursor((LCD_WIDTH - w) / 2, 76);
  gfx->print(sub);
}

static void status_line(int y, uint16_t color, const char *text) {
  gfx->fillRect(0, y, LCD_WIDTH, 24, COL_BG);
  gfx->setTextSize(2);
  gfx->setTextColor(color);
  int16_t w = text_width(text, 2);
  gfx->setCursor((LCD_WIDTH - w) / 2, y + 4);
  gfx->print(text);
}

// Layout:
//   row top = base_y
//   y+15..47:  ticker (left, color)  + price (right, white) — both at size 4
//   y+55..71:  24h change at size 2, right-aligned, green/red
static const int16_t MARGIN     = 16;
static const int16_t ROW_HEIGHT = 90;
static const int16_t BTC_Y      = 100;
static const int16_t ETH_Y      = 200;
static const int16_t SOL_Y      = 300;
static const int16_t FOOTER_Y   = 410;

static void draw_coin_row(int16_t y, const char *ticker, uint16_t ticker_color,
                          const CoinSnapshot &c) {
  // Clear the entire row first so old digits don't ghost when the price gets shorter.
  gfx->fillRect(0, y, LCD_WIDTH, ROW_HEIGHT, COL_BG);

  // Ticker on the left.
  gfx->setTextSize(4);
  gfx->setTextColor(ticker_color);
  gfx->setCursor(MARGIN, y + 15);
  gfx->print(ticker);

  if (!c.valid) {
    gfx->setTextSize(2);
    gfx->setTextColor(COL_DIM);
    const char *msg = "no data";
    gfx->setCursor(LCD_WIDTH - MARGIN - text_width(msg, 2), y + 23);
    gfx->print(msg);
    return;
  }

  // Price on the right, white.
  char price_buf[24];
  format_money(price_buf, sizeof(price_buf), c.price_usd);
  gfx->setTextSize(4);
  gfx->setTextColor(COL_TITLE);
  int16_t price_w = text_width(price_buf, 4);
  gfx->setCursor(LCD_WIDTH - MARGIN - price_w, y + 15);
  gfx->print(price_buf);

  // 24h percent change, right-aligned below the price.
  char pct_buf[16];
  format_pct(pct_buf, sizeof(pct_buf), c.change_24h_pct);
  gfx->setTextSize(2);
  gfx->setTextColor(c.change_24h_pct >= 0 ? COL_OK : COL_ERR);
  int16_t pct_w = text_width(pct_buf, 2);
  gfx->setCursor(LCD_WIDTH - MARGIN - pct_w, y + 55);
  gfx->print(pct_buf);
}

static void draw_footer() {
  gfx->fillRect(0, FOOTER_Y, LCD_WIDTH, 20, COL_BG);

  char buf[40];
  if (last_fetch_ok == 0) {
    snprintf(buf, sizeof(buf), "fetching...");
  } else {
    uint32_t age_s = (millis() - last_fetch_ok) / 1000;
    snprintf(buf, sizeof(buf), "updated %lus ago", (unsigned long)age_s);
  }
  gfx->setTextSize(1);
  gfx->setTextColor(COL_DIM);
  gfx->setCursor((LCD_WIDTH - text_width(buf, 1)) / 2, FOOTER_Y + 4);
  gfx->print(buf);
}

static void draw_grid() {
  // Erase everything below the header in one shot.
  gfx->fillRect(0, 90, LCD_WIDTH, LCD_HEIGHT - 90, COL_BG);
  draw_coin_row(BTC_Y, "BTC", COL_BTC, btc);
  draw_coin_row(ETH_Y, "ETH", COL_ETH, eth);
  draw_coin_row(SOL_Y, "SOL", COL_SOL, sol);
  draw_footer();
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

static void draw_detail() {
  gfx->fillRect(0, 90, LCD_WIDTH, LCD_HEIGHT - 90, COL_BG);

  const char *name = coin_name(current_coin);
  uint16_t color = coin_color(current_coin);
  const CoinSnapshot &c = coin_snap(current_coin);

  gfx->setTextSize(8);
  gfx->setTextColor(color);
  gfx->setCursor((LCD_WIDTH - text_width(name, 8)) / 2, 100);
  gfx->print(name);

  if (!c.valid) {
    gfx->setTextSize(2);
    gfx->setTextColor(COL_DIM);
    const char *m = "no data yet";
    gfx->setCursor((LCD_WIDTH - text_width(m, 2)) / 2, 220);
    gfx->print(m);
  } else {
    char price_buf[24];
    format_money(price_buf, sizeof(price_buf), c.price_usd);
    gfx->setTextSize(5);
    gfx->setTextColor(COL_TITLE);
    gfx->setCursor((LCD_WIDTH - text_width(price_buf, 5)) / 2, 220);
    gfx->print(price_buf);

    char pct_buf[16];
    format_pct(pct_buf, sizeof(pct_buf), c.change_24h_pct);
    gfx->setTextSize(4);
    gfx->setTextColor(c.change_24h_pct >= 0 ? COL_OK : COL_ERR);
    gfx->setCursor((LCD_WIDTH - text_width(pct_buf, 4)) / 2, 300);
    gfx->print(pct_buf);
  }

  gfx->setTextSize(1);
  gfx->setTextColor(COL_DIM);
  const char *hint = "tap = list   swipe < > = coin";
  gfx->setCursor((LCD_WIDTH - text_width(hint, 1)) / 2, 410);
  gfx->print(hint);
}

static void redraw_current_screen() {
  if (current_screen == SCREEN_LIST) draw_grid();
  else                                draw_detail();
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

// --- WiFi ---
static bool wifi_connect(uint32_t timeout_ms = 20000) {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  status_line(200, COL_WARN, "connecting wifi...");
  Serial.printf("Connecting to '%s'", WIFI_SSID);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeout_ms) {
    delay(300);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    status_line(200, COL_ERR, "wifi FAILED");
    return false;
  }

  status_line(200, COL_OK, "wifi OK");
  char buf[40];
  snprintf(buf, sizeof(buf), "IP %s", WiFi.localIP().toString().c_str());
  status_line(240, COL_DIM, buf);
  snprintf(buf, sizeof(buf), "RSSI %d dBm", (int)WiFi.RSSI());
  status_line(280, COL_DIM, buf);
  Serial.printf("WiFi OK. IP=%s, RSSI=%d dBm\n",
                WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
  return true;
}

// --- CoinGecko fetch ---
static bool fetch_prices() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();  // public price data, no auth — pin LE root CA later if we care
  HTTPClient http;
  if (!http.begin(client, COINGECKO_URL)) return false;
  http.setUserAgent("CoinDeck/0.1 (ESP32-S3)");
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
  delay(2000);
  Serial.println();
  Serial.println("=== CoinDeck step 5: price grid ===");

  Wire.begin(IIC_SDA, IIC_SCL);
  Wire.setClock(400000);

  if (!expander_reset_sequence()) {
    Serial.println("ERROR: XCA9554 not found at 0x20.");
    while (true) delay(1000);
  }
  if (!gfx->begin()) {
    Serial.println("ERROR: SH8601 init failed.");
    while (true) delay(1000);
  }

  gfx->fillScreen(COL_BG);
  for (int b = 0; b <= 255; b += 4) { gfx->setBrightness(b); delay(8); }

  if (!touch_begin()) {
    Serial.println("WARN: FT3168 not responding on I2C 0x38.");
  } else {
    Serial.println("FT3168 init OK");
  }

  draw_header();
  wifi_connect();
}

void loop() {
  static uint32_t last_footer_tick = 0;

  if (WiFi.status() != WL_CONNECTED) {
    grid_ready = false;
    status_line(200, COL_WARN, "wifi lost, retry");
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
      if (!grid_ready) status_line(320, COL_ERR, "fetch failed");
      next_fetch_at = millis() + RETRY_INTERVAL_MS;
    }
    Serial.printf("[fetch %lu ok / %lu fail]\n",
                  (unsigned long)(fetch_count - fetch_failed),
                  (unsigned long)fetch_failed);
  }

  if (current_screen == SCREEN_LIST && grid_ready &&
      millis() - last_footer_tick > 1000) {
    draw_footer();
    last_footer_tick = millis();
  }

  // Touch state machine. Track press-down → release; classify as tap or
  // horizontal swipe based on distance and direction.
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
      // ignore — too short, almost certainly a noise/contact bounce
    } else if (adx > 60 && adx > ady * 2) {
      int dir = dx > 0 ? +1 : -1;
      Serial.printf("SWIPE dx=%d dir=%+d\n", dx, dir);
      if (current_screen == SCREEN_DETAIL) {
        goto_detail(current_coin + (dir > 0 ? -1 : +1));  // swipe-left = next coin
      }
    } else {
      Serial.printf("TAP x=%d y=%d\n", last_x, last_y);
      if (current_screen == SCREEN_LIST) {
        int picked = -1;
        if      (last_y >= BTC_Y && last_y < BTC_Y + ROW_HEIGHT) picked = 0;
        else if (last_y >= ETH_Y && last_y < ETH_Y + ROW_HEIGHT) picked = 1;
        else if (last_y >= SOL_Y && last_y < SOL_Y + ROW_HEIGHT) picked = 2;
        if (picked >= 0) goto_detail(picked);
      } else {
        goto_list();
      }
    }
  }

  delay(20);
}
