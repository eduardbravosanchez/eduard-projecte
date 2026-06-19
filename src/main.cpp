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
// SETUP - Inicialització del sistema
// ═════════════════════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);

    Serial.printf("\n=============================================\n");
    Serial.printf(" Projecte: D.05 Despertador FM Real (Arquitectura Neta)\n");
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

    // 4. Carregar NVS (memòria persistent)
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
    xTaskCreatePinnedToCore(task_rtc,   "rtc",   TASK_RTC_STACK,   NULL, TASK_RTC_PRIO,   NULL, TASK_RUNNING_CORE);
    xTaskCreatePinnedToCore(task_alarm, "alarm", TASK_ALARM_STACK, NULL, TASK_ALARM_PRIO, NULL, TASK_RUNNING_CORE);
    xTaskCreatePinnedToCore(task_ui,    "ui",    TASK_UI_STACK,    NULL, TASK_UI_PRIO,    NULL, TASK_RUNNING_CORE);

    Serial.println("[OK] Tasques actives i sistema llest.");
}

// Loop buit: el processament pesat el fa FreeRTOS
void loop() { vTaskDelay(portMAX_DELAY); }
