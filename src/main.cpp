#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
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
#define XCA9554_ADDR 0x20  // I2C GPIO expander — gates display + touch reset/power

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

Arduino_SH8601 *gfx = new Arduino_SH8601(
    bus, GFX_NOT_DEFINED, 0, LCD_WIDTH, LCD_HEIGHT);

// Accent colors used across the UI.
static const uint16_t COL_BG       = RGB565_BLACK;
static const uint16_t COL_TITLE    = RGB565_WHITE;
static const uint16_t COL_OK       = 0x07E0;  // green
static const uint16_t COL_WARN     = 0xFD20;  // orange
static const uint16_t COL_ERR      = 0xF800;  // red
static const uint16_t COL_DIM      = 0x8410;  // gray

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

// Draw the static "CoinDeck" header at the top so every screen reuses it.
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

// Repaint a horizontal status band (clears the row first so old text doesn't bleed).
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
  uint8_t dots = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeout_ms) {
    delay(300);
    Serial.print('.');
    char buf[24];
    snprintf(buf, sizeof(buf), "connecting%.*s", (dots % 4) + 1, "....");
    status_line(200, COL_WARN, buf);
    dots++;
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    status_line(200, COL_ERR, "wifi FAILED");
    Serial.println("WiFi connect timed out.");
    return false;
  }

  status_line(200, COL_OK, "wifi OK");

  char ipbuf[24];
  snprintf(ipbuf, sizeof(ipbuf), "IP %s", WiFi.localIP().toString().c_str());
  status_line(240, COL_DIM, ipbuf);

  char rssibuf[24];
  snprintf(rssibuf, sizeof(rssibuf), "RSSI %d dBm", (int)WiFi.RSSI());
  status_line(280, COL_DIM, rssibuf);

  Serial.printf("WiFi OK. IP=%s, RSSI=%d dBm\n",
                WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println();
  Serial.println("=== CoinDeck step 3: WiFi ===");

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
  static uint32_t t = 0;
  // Reconnect if we drop the network.
  if (WiFi.status() != WL_CONNECTED) {
    status_line(200, COL_WARN, "wifi lost, retry");
    wifi_connect();
  }
  Serial.printf("[%lu] uptime=%lus, rssi=%d, heap=%u\n",
                t++, millis() / 1000, (int)WiFi.RSSI(), ESP.getFreeHeap());
  delay(5000);
}
