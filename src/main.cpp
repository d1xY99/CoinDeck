#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>

#include "secrets.h"

// --- Waveshare ESP32-S3-Touch-AMOLED-1.8 pinout (from Waveshare demo pin_config.h) ---
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

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

Arduino_SH8601 *gfx = new Arduino_SH8601(
    bus, GFX_NOT_DEFINED, 0, LCD_WIDTH, LCD_HEIGHT);

static const uint16_t COL_BG    = RGB565_BLACK;
static const uint16_t COL_TITLE = RGB565_WHITE;
static const uint16_t COL_OK    = 0x07E0;
static const uint16_t COL_WARN  = 0xFD20;
static const uint16_t COL_ERR   = 0xF800;
static const uint16_t COL_DIM   = 0x8410;

struct CoinSnapshot {
  float price_usd;
  float change_24h_pct;
  bool  valid;
};
static CoinSnapshot btc{}, eth{}, sol{};

static const uint32_t FETCH_INTERVAL_MS = 30000;  // 30s — well under CoinGecko's free-tier rate limit
static const uint32_t RETRY_INTERVAL_MS = 5000;
static uint32_t next_fetch_at = 0;
static uint32_t fetch_count   = 0;
static uint32_t fetch_failed  = 0;

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

static void draw_header() {
  gfx->setTextSize(4);
  gfx->setTextColor(COL_TITLE);
  gfx->setCursor(88, 32);
  gfx->print("CoinDeck");

  gfx->setTextSize(1);
  gfx->setTextColor(COL_DIM);
  gfx->setCursor(116, 76);
  gfx->print("crypto desk ticker");
}

static void status_line(int y, uint16_t color, const char *text) {
  gfx->fillRect(0, y, LCD_WIDTH, 24, COL_BG);
  gfx->setTextSize(2);
  gfx->setTextColor(color);
  int16_t w = strlen(text) * 12;
  gfx->setCursor((LCD_WIDTH - w) / 2, y + 4);
  gfx->print(text);
}

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
  char buf[32];
  snprintf(buf, sizeof(buf), "IP %s", WiFi.localIP().toString().c_str());
  status_line(240, COL_DIM, buf);
  snprintf(buf, sizeof(buf), "RSSI %d dBm", (int)WiFi.RSSI());
  status_line(280, COL_DIM, buf);
  Serial.printf("WiFi OK. IP=%s, RSSI=%d dBm\n",
                WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
  return true;
}

static bool fetch_prices() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  // CoinGecko's /simple/price is public read-only data. setInsecure() skips cert
  // validation, which is fine here (no auth, no PII). We can pin the LE root CA
  // later if we want belt-and-suspenders MITM protection.
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, COINGECKO_URL)) {
    Serial.println("http.begin failed");
    return false;
  }
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
  Serial.println("=== CoinDeck step 4: CoinGecko fetch ===");

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

  draw_header();
  wifi_connect();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    status_line(200, COL_WARN, "wifi lost, retry");
    wifi_connect();
  }

  if ((int32_t)(millis() - next_fetch_at) >= 0) {
    bool ok = fetch_prices();
    fetch_count++;
    if (ok) {
      char buf[32];
      snprintf(buf, sizeof(buf), "fetch #%lu OK", (unsigned long)fetch_count);
      status_line(320, COL_OK, buf);
      next_fetch_at = millis() + FETCH_INTERVAL_MS;
    } else {
      fetch_failed++;
      char buf[32];
      snprintf(buf, sizeof(buf), "fetch fail (%lu)", (unsigned long)fetch_failed);
      status_line(320, COL_ERR, buf);
      next_fetch_at = millis() + RETRY_INTERVAL_MS;
    }
  }

  delay(100);
}
