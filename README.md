# CoinDeck

Crypto desk ticker (BTC / ETH / SOL) za Waveshare ESP32-S3-Touch-AMOLED 1.8".

## Hardver

- Waveshare ESP32-S3-Touch-AMOLED-1.8 (N16R8: 16MB flash, 8MB octal PSRAM)
- 1.8" AMOLED 368×448, SH8601 driver (QSPI)
- Touch FT3168 (I2C)
- IMU QMI8658
- 3.7V Li-Po opcionalno

## Setup

1. Instaliraj **PlatformIO IDE** ekstenziju u VSCode-u
   (Extensions → traži "PlatformIO IDE" → Install). Reload prozor kad završi.
2. Otvori folder `CoinDeck/` u VSCode-u (File → Open Folder).
3. PlatformIO će automatski preuzeti toolchain i dependencije pri prvom buildu.

## Flash + Serial

USB-C kabal direktno u ploču (ima native USB, ne treba poseban USB-UART).

- **Build:** PlatformIO traka (donja) → ikona ✓
- **Upload:** ikona → (strelica)
- **Serial Monitor:** ikona 🔌 (115200 baud)

Ako upload ne uspije iz prve: drži `BOOT` taster, kratko pritisni `RESET`, pusti `BOOT`, pa probaj upload ponovo (ESP32-S3 download mode).

## Roadmap

- [x] Korak 0 — projekat skeleton
- [x] Korak 1 — smoke test (Serial print, PSRAM check)
- [x] Korak 2 — SH8601 display "Hello World" (Arduino_GFX)
- [x] Korak 3 — WiFi konekcija
- [x] Korak 4 — CoinGecko API fetch
- [ ] Korak 5 — LVGL UI prvi screen
- [ ] Korak 6 — Touch + swipe između ekrana
- [ ] Korak 7 — Polish (fontovi, glow, mini chart)
