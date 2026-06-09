# Pinout — D.05 Despertador amb Ràdio FM Real

**Eduard Bravo Sánchez · Processadors Digitals · UPC 2025/2026**  
**MCU:** ESP32-S3 DevKitC-1

---

## TEA5767 — Sintonitzador FM (bus I2C)

| TEA5767 | ESP32-S3 |
|---------|----------|
| VCC | 3.3V |
| GND | GND |
| SDA | GPIO 8 |
| SCL | GPIO 9 |

---

## DS3231 — Rellotge en temps real (bus I2C, mateix bus que TEA5767)

| DS3231 | ESP32-S3 |
|--------|----------|
| VCC | 3.3V |
| GND | GND |
| SDA | GPIO 8 |
| SCL | GPIO 9 |

> TEA5767 (adreça I2C: 0x60) i DS3231 (adreça I2C: 0x68) comparteixen
> les mateixes línies SDA i SCL connectades en paral·lel.

---

## ILI9341 — Display TFT 2.4" (bus SPI)

| TFT | ESP32-S3 |
|-----|----------|
| VCC | 3.3V |
| GND | GND |
| CS | GPIO 10 |
| RESET | GPIO 14 |
| DC | GPIO 13 |
| MOSI | GPIO 11 |
| SCK | GPIO 12 |
| LED | 3.3V |

> Els 6 pins de tàctil (T_CLK, T_CS, T_DIN, T_DO, T_IRQ, SCK touch)
> deixar-los lliures, no connectar res.

---

## Polsadors

| Polsador | Pin A | Pin B | Funció |
|----------|-------|-------|--------|
| FM+ | GPIO 1 | GND | Seek FM cap amunt |
| FM− | GPIO 2 | GND | Seek FM cap avall |
| SET | GPIO 3 | GND | Ajust alarma (curt) / Ajust hora rellotge (≥3s) |
| SNOOZE | GPIO 4 | GND | Snooze 5 min (1r pols) / Apagar alarma (2n pols) |
| MONO/ST | GPIO 5 | GND | Alternar mono forçat ↔ estèreo auto |

> Tots els polsadors usen `INPUT_PULLUP` intern de l'ESP32-S3.
> No calen resistències pull-up externes ni condensadors.

---

## Àudio

| Component | Connexió |
|-----------|----------|
| Auriculars / altaveu actiu | Jack 3.5mm sortida directa del TEA5767 |

---

## Resum complet de GPIOs

| GPIO | Component | Funció |
|------|-----------|--------|
| GPIO 1 | Polsador FM+ | Input digital — seek FM amunt |
| GPIO 2 | Polsador FM− | Input digital — seek FM avall |
| GPIO 3 | Polsador SET | Input digital — ajust alarma / rellotge |
| GPIO 4 | Polsador SNOOZE | Input digital — snooze / apagar alarma |
| GPIO 5 | Polsador MONO/ST | Input digital — mono ↔ estèreo |
| GPIO 8 | TEA5767 + DS3231 | I2C SDA |
| GPIO 9 | TEA5767 + DS3231 | I2C SCL |
| GPIO 10 | ILI9341 | SPI CS |
| GPIO 11 | ILI9341 | SPI MOSI |
| GPIO 12 | ILI9341 | SPI SCK |
| GPIO 13 | ILI9341 | SPI DC |
| GPIO 14 | ILI9341 | SPI RESET |

---

## Diagrama ràpid de connexions

```
ESP32-S3 DevKitC-1
│
├── GPIO1  ──── SW1 (FM+)     ──── GND
├── GPIO2  ──── SW2 (FM−)     ──── GND
├── GPIO3  ──── SW3 (SET)     ──── GND
├── GPIO4  ──── SW4 (SNOOZE)  ──── GND
├── GPIO5  ──── SW5 (MONO/ST) ──── GND
│
├── GPIO8  (SDA) ──┬──── TEA5767 SDA
│                  └──── DS3231  SDA
│
├── GPIO9  (SCL) ──┬──── TEA5767 SCL
│                  └──── DS3231  SCL
│
├── GPIO10 ──── ILI9341 CS
├── GPIO11 ──── ILI9341 MOSI
├── GPIO12 ──── ILI9341 SCK
├── GPIO13 ──── ILI9341 DC
├── GPIO14 ──── ILI9341 RESET
│
├── 3.3V ──────┬──── TEA5767 VCC
│              ├──── DS3231  VCC
│              ├──── ILI9341 VCC
│              └──── ILI9341 LED
│
└── GND ───────┬──── TEA5767 GND
               ├──── DS3231  GND
               └──── ILI9341 GND
```

---

## Definicions al codi (`include/config.h`)

```cpp
// ── I2C (TEA5767 + DS3231) ────────────────────────────────────────────────
#define PIN_SDA          8
#define PIN_SCL          9

// ── SPI TFT ILI9341 ───────────────────────────────────────────────────────
#define PIN_TFT_CS      10
#define PIN_TFT_RST     14
#define PIN_TFT_DC      13
#define PIN_TFT_MOSI    11
#define PIN_TFT_CLK     12

// ── Polsadors (INPUT_PULLUP intern, actius a LOW) ─────────────────────────
#define PIN_BTN_FM_UP    1
#define PIN_BTN_FM_DOWN  2
#define PIN_BTN_SET      3
#define PIN_BTN_SNOOZE   4
#define PIN_BTN_MONO_ST  5
```