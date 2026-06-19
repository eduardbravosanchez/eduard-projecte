#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include "config.h"
#include "version.h"
#include "globals.h"
#include "app_tasks.h" 

// ── INSTANCIACIÓ REAL DELS OBJECTES I VARIABLES GLOBALS ───────────────────────
RTC_DS3231       rtc;
TEA5767          radio;
Adafruit_ILI9341 tft(PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
Preferences      prefs;

volatile uint16_t  g_fmFreq      = FM_DEFAULT;
volatile bool      g_alarmFired  = false;
volatile bool      g_snoozed     = false;
volatile bool      g_forcesMono  = false;
volatile bool      g_isStereo    = false;
volatile bool      g_radioMuted  = true;
AlarmConfig        g_alarm       = { ALARM_HOUR_DEF, ALARM_MIN_DEF, false };
DateTime           g_now;

SemaphoreHandle_t  mutex_i2c;
SemaphoreHandle_t  sem_alarm;
QueueHandle_t      q_alarm_event;

// ═════════════════════════════════════════════════════════════════════════════
// SETUP
// ═════════════════════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);

    Serial.printf("\n=============================================\n");
    Serial.printf(" Projecte: D.05 Despertador FM Real (Modular)\n");
    Serial.printf(" Firmware Versió: %s\n", FIRMWARE_VERSION_STR);
    Serial.printf(" Data de compilació: %s - %s\n", FIRMWARE_BUILD_DATE, FIRMWARE_BUILD_TIME);
    Serial.printf("=============================================\n\n");

    // 1. Primitives FreeRTOS
    mutex_i2c     = xSemaphoreCreateMutex();
    sem_alarm     = xSemaphoreCreateBinary();
    q_alarm_event = xQueueCreate(4, sizeof(AlarmEvent));

    // 2. Pins i busos
    pinMode(PIN_BTN_FM_UP,   INPUT_PULLUP);
    pinMode(PIN_BTN_FM_DOWN, INPUT_PULLUP);
    pinMode(PIN_BTN_SET,     INPUT_PULLUP);
    pinMode(PIN_BTN_SNOOZE,  INPUT_PULLUP);
    pinMode(PIN_BTN_MONO_ST, INPUT_PULLUP);
    
    Wire.begin(PIN_SDA, PIN_SCL);
    SPI.begin(PIN_TFT_CLK, -1, PIN_TFT_MOSI, PIN_TFT_CS);

    // 3. Inicialitzar maquinari
    if (!rtc.begin()) {
        Serial.println("[ERROR] RTC no trobat");
    }
    if (rtc.lostPower()) {
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }

    tft.begin();
    tft.setRotation(1);
    tft.fillScreen(ILI9341_BLACK);
    tft.setTextColor(ILI9341_WHITE);
    tft.setTextSize(2);
    tft.setCursor(20, 100);
    tft.print("Iniciant...");

    radio.init();
    radio.setBandFrequency(RADIO_BAND_FM, FM_DEFAULT);
    radio.setVolume(15);
    radio.setMono(false);
    radio.setMute(true);

    // 4. Carregar NVS
    prefs.begin(NVS_NAMESPACE, false);
    g_alarm.hour    = prefs.getUChar (NVS_KEY_ALARM_H, ALARM_HOUR_DEF);
    g_alarm.minute  = prefs.getUChar (NVS_KEY_ALARM_M, ALARM_MIN_DEF);
    g_alarm.enabled = prefs.getBool  (NVS_KEY_ALARM_E, false);
    g_fmFreq        = prefs.getUShort(NVS_KEY_FREQ,    FM_DEFAULT);
    g_forcesMono    = prefs.getBool  (NVS_KEY_MONO,    false);
    prefs.end();

    applyFmFreq();
    applyMonoStereo();

    // 5. Crear les tasques FreeRTOS
    xTaskCreatePinnedToCore(rtc_task,   "rtc",   TASK_RTC_STACK,   NULL, TASK_RTC_PRIO,   NULL, TASK_RUNNING_CORE);
    xTaskCreatePinnedToCore(alarm_task, "alarm", TASK_ALARM_STACK, NULL, TASK_ALARM_PRIO, NULL, TASK_RUNNING_CORE);
    xTaskCreatePinnedToCore(ui_task,    "ui",    TASK_UI_STACK,    NULL, TASK_UI_PRIO,    NULL, TASK_RUNNING_CORE);

    Serial.println("[OK] Tasques actives i sistema llest.");
}

void loop() { vTaskDelay(portMAX_DELAY); }

// ═════════════════════════════════════════════════════════════════════════════
// LOGICA DE DRIVERS INTERNS (Últim pas pendent d'extreure)
// ═════════════════════════════════════════════════════════════════════════════
void tea_write(uint16_t freq10kHz, bool searchMode, bool searchUp, uint8_t ssl) {
    uint32_t freqKhz = (uint32_t)freq10kHz * 10;
    uint16_t pll     = (uint16_t)((4.0f * (float)(freqKhz + 225)) / 32.768f);
    uint8_t b[5];
    b[0] = (searchMode ? 0x40 : 0x00) | (uint8_t)((pll >> 8) & 0x3F);
    b[1] = (uint8_t)(pll & 0xFF);
    b[2] = (searchUp    ? 0x80 : 0x00) | ssl | 0x10 | (g_forcesMono ? 0x08 : 0x00);
    b[3] = 0x10 | 0x04 | 0x02;
    b[4] = 0x00;

    Wire.beginTransmission(TEA5767_I2C_ADDR);
    for (int i = 0; i < 5; i++) Wire.write(b[i]);
    Wire.endTransmission();
}

uint16_t tea_seek_once(bool up, uint16_t fromFreq) {
    tea_write(fromFreq, true, up, TEA_SSL);
    uint8_t resp[5] = {0};
    uint32_t t0 = millis();

    while (millis() - t0 < 5000) {
        vTaskDelay(pdMS_TO_TICKS(50));
        Wire.requestFrom(TEA5767_I2C_ADDR, 5);
        for (int i = 0; i < 5; i++) resp[i] = Wire.available() ? Wire.read() : 0;

        bool rf  = (resp[0] >> 7) & 0x01;
        bool blf = (resp[0] >> 6) & 0x01;

        if (blf) return 0;
        if (rf) {
            uint16_t pllFound = ((uint16_t)(resp[0] & 0x3F) << 8) | resp[1];
            uint32_t freqKhz  = (uint32_t)((float)pllFound * 32.768f / 4.0f) - 225;
            uint16_t found    = (uint16_t)(freqKhz / 10);
            if (found < FM_MIN) found = FM_MIN;
            if (found > FM_MAX) found = FM_MAX;
            return found;
        }
    }
    return 0;
}

void seekAndUpdate(bool up) {
    drawSeeking();
    uint16_t startFreq = g_fmFreq;
    uint16_t next = up ? startFreq + 10 : startFreq - 10;
    if (next > FM_MAX) next = FM_MIN;
    if (next < FM_MIN) next = FM_MAX;

    uint16_t found = 0;
    if (xSemaphoreTake(mutex_i2c, pdMS_TO_TICKS(500)) == pdTRUE) {
        found = tea_seek_once(up, next);
        if (found == 0) {
            found = tea_seek_once(up, up ? FM_MIN : FM_MAX);
        }
        xSemaphoreGive(mutex_i2c);
    }

    if (found != 0) {
        g_fmFreq = found;
        prefs.begin(NVS_NAMESPACE, false);
        prefs.putUShort(NVS_KEY_FREQ, g_fmFreq);
        prefs.end();
        Serial.printf("[SEEK] Emissora trobada: %d.%02d MHz\n", g_fmFreq/100, g_fmFreq%100);
    } else {
        Serial.println("[SEEK] No s'ha trobat cap senyal vàlid");
    }
    drawFreq();
}

void applyFmFreq() {
    if (xSemaphoreTake(mutex_i2c, pdMS_TO_TICKS(200)) == pdTRUE) {
        tea_write(g_fmFreq, false, true, TEA_SSL);
        xSemaphoreGive(mutex_i2c);
    }
}

void applyMonoStereo() {
    if (xSemaphoreTake(mutex_i2c, pdMS_TO_TICKS(200)) == pdTRUE) {
        tea_write(g_fmFreq, false, true, TEA_SSL);
        xSemaphoreGive(mutex_i2c);
    }
}
