/*
// ─────────────────────────────────────────────────────────────────────────────
// D.05 — Despertador con Radio FM Real (Versión Auriculares)
// Eduard Bravo Sánchez · Procesadores Digitales · UPC 2025/2026
//
// Hardware:
//   ESP32-S3        — MCU principal
//   TEA5767         — Sintonizador FM (I2C: SDA=GPIO8, SCL=GPIO9) - Salida Jack directa
//   DS3231          — RTC (I2C compartido con TEA5767)
//   ILI9341 2.4"    — Display TFT no táctil (SPI: MOSI=11, CLK=12, CS=10, DC=13, RST=14)
//   BTN_FM_UP       — GPIO 1  — Subir frecuencia FM (+0.1 MHz)
//   BTN_FM_DOWN     — GPIO 2  — Bajar frecuencia FM (-0.1 MHz)
//   BTN_SET         — GPIO 3  — Entrar/confirmar modo ajuste alarma
//   BTN_SNOOZE      — GPIO 4  — Snooze / apagar alarma activa
//
// Buses usados: I2C (TEA5767 + DS3231), SPI (ILI9341), GPIO (botones)
// FreeRTOS: tareas rtc_task, alarm_task, ui_task + mutex I2C + queue + semáforo
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <RTClib.h>
#include <Radio.h>
#include <TEA5767.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>

// ── Pines ─────────────────────────────────────────────────────────────────────
#define PIN_SDA         8
#define PIN_SCL         9
#define PIN_TFT_MOSI   11
#define PIN_TFT_CLK    12
#define PIN_TFT_CS     10
#define PIN_TFT_DC     13
#define PIN_TFT_RST    14
#define PIN_BTN_FM_UP   1
#define PIN_BTN_FM_DOWN 2
#define PIN_BTN_SET     3
#define PIN_BTN_SNOOZE  4

// ── Configuración ─────────────────────────────────────────────────────────────
#define FM_MIN          875   // 87.5 MHz (x10 para evitar float)
#define FM_MAX         1080   // 108.0 MHz
#define FM_STEP           1   // 0.1 MHz por pulsación
#define FM_DEFAULT      936   // 93.6 MHz al arrancar
#define SNOOZE_MINUTES    5
#define DEBOUNCE_MS      50
#define ALARM_HOUR_DEF    7
#define ALARM_MIN_DEF     0

// ── Tipos ─────────────────────────────────────────────────────────────────────
struct AlarmConfig {
    uint8_t hour;
    uint8_t minute;
    bool    enabled;
};

enum AlarmEvent { EVT_NONE, EVT_FIRE, EVT_SNOOZE, EVT_OFF };

// ── Objetos globales ──────────────────────────────────────────────────────────
RTC_DS3231       rtc;
TEA5767          radio;
Adafruit_ILI9341 tft(PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
Preferences      prefs;

// ── Variables compartidas (protegidas por mutex o acceso desde una sola tarea)
volatile uint16_t  g_fmFreq      = FM_DEFAULT;   // frecuencia x10
volatile bool      g_alarmFired  = false;
volatile bool      g_snoozed     = false;
AlarmConfig        g_alarm       = { ALARM_HOUR_DEF, ALARM_MIN_DEF, false };
DateTime           g_now;         // actualizada por rtc_task

// ── Primitivas FreeRTOS ───────────────────────────────────────────────────────
SemaphoreHandle_t  mutex_i2c;
SemaphoreHandle_t  sem_alarm;       // señal de alarma disparada
QueueHandle_t      q_alarm_event;   // mensajes entre alarm_task y ui_task

// ── Prototipos ────────────────────────────────────────────────────────────────
void rtc_task   (void *pv);
void alarm_task (void *pv);
void ui_task    (void *pv);
void applyFmFreq();
void drawScreen();
void drawTime(DateTime &dt);
void drawFreq();
void drawAlarmStatus();
bool readBtn(uint8_t pin);

// ═════════════════════════════════════════════════════════════════════════════
// setup
// ═════════════════════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);

    // ── 1. PRIMERO CREAR PRIMITIVAS FREERTOS (Para evitar punteros NULL) ──────
    mutex_i2c     = xSemaphoreCreateMutex();
    sem_alarm     = xSemaphoreCreateBinary();
    q_alarm_event = xQueueCreate(4, sizeof(AlarmEvent));

    // ── 2. GPIO ──────────────────────────────────────────────────────────────
    pinMode(PIN_BTN_FM_UP,   INPUT_PULLUP);
    pinMode(PIN_BTN_FM_DOWN, INPUT_PULLUP);
    pinMode(PIN_BTN_SET,     INPUT_PULLUP);
    pinMode(PIN_BTN_SNOOZE,  INPUT_PULLUP);

    // ── 3. I2C ───────────────────────────────────────────────────────────────
    Wire.begin(PIN_SDA, PIN_SCL);

    // ── 4. RTC ───────────────────────────────────────────────────────────────
    if (!rtc.begin()) {
        Serial.println("[ERROR] RTC no encontrado");
    }
    if (rtc.lostPower()) {
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
        Serial.println("[RTC] Hora ajustada a compilacion");
    }

    // ── 5. TFT ───────────────────────────────────────────────────────────────
    SPI.begin(PIN_TFT_CLK, -1, PIN_TFT_MOSI, PIN_TFT_CS);
    tft.begin();
    tft.setRotation(1);   // landscape
    tft.fillScreen(ILI9341_BLACK);
    tft.setTextColor(ILI9341_WHITE);
    tft.setTextSize(2);
    tft.setCursor(20, 100);
    tft.print("Iniciando...");

    // ── 6. TEA5767 ───────────────────────────────────────────────────────────
    radio.init();
    radio.setBandFrequency(RADIO_BAND_FM, g_fmFreq * 10); 
    radio.setVolume(15);   
    radio.setMono(false);
    radio.setMute(true);   // Silenciar al arrancar

    // ── 7. Cargar alarma guardada en NVS ─────────────────────────────────────
    prefs.begin("despertador", false);
    g_alarm.hour    = prefs.getUChar("alh",  ALARM_HOUR_DEF);
    g_alarm.minute  = prefs.getUChar("alm",  ALARM_MIN_DEF);
    g_alarm.enabled = prefs.getBool ("ale",  false);
    g_fmFreq        = prefs.getUShort("freq", FM_DEFAULT);
    prefs.end();

    // Ahora sí se puede llamar de forma segura porque mutex_i2c ya existe
    applyFmFreq(); 

    // ── 8. Crear Tareas de FreeRTOS ──────────────────────────────────────────
    xTaskCreatePinnedToCore(rtc_task,   "rtc",   2048, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(alarm_task, "alarm", 2048, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(ui_task,    "ui",    4096, NULL, 2, NULL, 1);

    Serial.println("[OK] Setup completo");
}

// loop vacío — todo corre en tareas
void loop() { vTaskDelay(portMAX_DELAY); }

// ═════════════════════════════════════════════════════════════════════════════
// rtc_task — lee el DS3231 cada segundo
// ═════════════════════════════════════════════════════════════════════════════
void rtc_task(void *pv) {
    for (;;) {
        if (xSemaphoreTake(mutex_i2c, pdMS_TO_TICKS(100)) == pdTRUE) {
            g_now = rtc.now();
            xSemaphoreGive(mutex_i2c);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// alarm_task — comprueba si hay que disparar la alarma
// ═════════════════════════════════════════════════════════════════════════════
void alarm_task(void *pv) {
    bool lastMinuteTriggered = 99; // para no disparar dos veces el mismo minuto

    for (;;) {
        if (g_alarm.enabled && !g_alarmFired) {
            uint8_t h = g_now.hour();
            uint8_t m = g_now.minute();

            if (h == g_alarm.hour && m == g_alarm.minute && m != lastMinuteTriggered) {
                lastMinuteTriggered = m;
                g_alarmFired = true;
                g_snoozed    = false;

                // Encender audio por software
                if (xSemaphoreTake(mutex_i2c, pdMS_TO_TICKS(100)) == pdTRUE) {
                    radio.setMute(false);
                    xSemaphoreGive(mutex_i2c);
                }

                // Señal a ui_task
                AlarmEvent ev = EVT_FIRE;
                xQueueSend(q_alarm_event, &ev, 0);
                xSemaphoreGive(sem_alarm);

                Serial.println("[ALARM] Disparada!");
            }
        }

        // Resetear el flag de minuto cuando ya ha pasado
        if (g_now.minute() != g_alarm.minute) {
            lastMinuteTriggered = 99;
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// ui_task — pantalla + botones
// ═════════════════════════════════════════════════════════════════════════════
void ui_task(void *pv) {

    enum SetMode { SET_NONE, SET_HOUR, SET_MIN, SET_CONFIRM };
    SetMode setMode = SET_NONE;
    uint8_t tmpHour = g_alarm.hour;
    uint8_t tmpMin  = g_alarm.minute;

    uint32_t lastDraw      = 0;
    uint8_t  lastSecDrawn  = 99;

    for (;;) {
        // ── Leer botones ──────────────────────────────────────────────────────

        if (readBtn(PIN_BTN_FM_UP) && setMode == SET_NONE) {
            g_fmFreq = (g_fmFreq >= FM_MAX) ? FM_MIN : g_fmFreq + FM_STEP;
            applyFmFreq();
            prefs.begin("despertador", false);
            prefs.putUShort("freq", g_fmFreq);
            prefs.end();
            drawFreq();
        }

        if (readBtn(PIN_BTN_FM_DOWN) && setMode == SET_NONE) {
            g_fmFreq = (g_fmFreq <= FM_MIN) ? FM_MAX : g_fmFreq - FM_STEP;
            applyFmFreq();
            prefs.begin("despertador", false);
            prefs.putUShort("freq", g_fmFreq);
            prefs.end();
            drawFreq();
        }

        if (readBtn(PIN_BTN_SNOOZE)) {
            if (g_alarmFired) {
                if (!g_snoozed) {
                    // Primer pulso = snooze
                    g_snoozed = true;
                    g_alarmFired = false;
                    
                    if (xSemaphoreTake(mutex_i2c, pdMS_TO_TICKS(100)) == pdTRUE) {
                        radio.setMute(true);
                        xSemaphoreGive(mutex_i2c);
                    }

                    uint8_t newMin = (g_alarm.minute + SNOOZE_MINUTES) % 60;
                    uint8_t newHour = g_alarm.hour;
                    if (g_alarm.minute + SNOOZE_MINUTES >= 60) newHour = (newHour + 1) % 24;

                    g_alarm.minute = newMin;
                    g_alarm.hour   = newHour;

                    AlarmEvent ev = EVT_SNOOZE;
                    xQueueSend(q_alarm_event, &ev, 0);
                    Serial.printf("[SNOOZE] Reactivando a %02d:%02d\n", newHour, newMin);
                } else {
                    // Segundo pulso = apagar del todo
                    g_alarmFired = false;
                    g_snoozed    = false;
                    g_alarm.hour   = tmpHour;
                    g_alarm.minute = tmpMin;
                    
                    if (xSemaphoreTake(mutex_i2c, pdMS_TO_TICKS(100)) == pdTRUE) {
                        radio.setMute(true);
                        xSemaphoreGive(mutex_i2c);
                    }
                    
                    AlarmEvent ev = EVT_OFF;
                    xQueueSend(q_alarm_event, &ev, 0);
                    Serial.println("[ALARM] Apagada");
                }
            }
        }

        if (readBtn(PIN_BTN_SET)) {
            switch (setMode) {
                case SET_NONE:
                    setMode = SET_HOUR;
                    tmpHour = g_alarm.hour;
                    tmpMin  = g_alarm.minute;
                    tft.fillScreen(ILI9341_BLACK);
                    tft.setTextColor(ILI9341_YELLOW);
                    tft.setTextSize(2);
                    tft.setCursor(10, 10);
                    tft.print("Ajustar alarma");
                    tft.setCursor(10, 40);
                    tft.print("FM+ / FM- = hora");
                    tft.setCursor(10, 65);
                    tft.printf("Hora: %02d", tmpHour);
                    break;

                case SET_HOUR:
                    setMode = SET_MIN;
                    tft.fillScreen(ILI9341_BLACK);
                    tft.setTextColor(ILI9341_CYAN);
                    tft.setTextSize(2);
                    tft.setCursor(10, 10);
                    tft.print("Ajustar alarma");
                    tft.setCursor(10, 40);
                    tft.print("FM+ / FM- = minuto");
                    tft.setCursor(10, 65);
                    tft.printf("Min: %02d", tmpMin);
                    break;

                case SET_MIN:
                    g_alarm.hour    = tmpHour;
                    g_alarm.minute  = tmpMin;
                    g_alarm.enabled = true;
                    prefs.begin("despertador", false);
                    prefs.putUChar("alh", g_alarm.hour);
                    prefs.putUChar("alm", g_alarm.minute);
                    prefs.putBool ("ale", true);
                    prefs.end();
                    setMode = SET_NONE;
                    tft.fillScreen(ILI9341_BLACK);
                    tft.setTextColor(ILI9341_GREEN);
                    tft.setTextSize(2);
                    tft.setCursor(10, 100);
                    tft.printf("Alarma: %02d:%02d OK", g_alarm.hour, g_alarm.minute);
                    vTaskDelay(pdMS_TO_TICKS(1500));
                    tft.fillScreen(ILI9341_BLACK);
                    lastSecDrawn = 99;
                    break;

                default: break;
            }
        }

        if (setMode == SET_HOUR) {
            if (readBtn(PIN_BTN_FM_UP)) {
                tmpHour = (tmpHour + 1) % 24;
                tft.fillRect(10, 65, 200, 20, ILI9341_BLACK);
                tft.setTextColor(ILI9341_YELLOW);
                tft.setTextSize(2);
                tft.setCursor(10, 65);
                tft.printf("Hora: %02d", tmpHour);
            }
            if (readBtn(PIN_BTN_FM_DOWN)) {
                tmpHour = (tmpHour == 0) ? 23 : tmpHour - 1;
                tft.fillRect(10, 65, 200, 20, ILI9341_BLACK);
                tft.setTextColor(ILI9341_YELLOW);
                tft.setTextSize(2);
                tft.setCursor(10, 65);
                tft.printf("Hora: %02d", tmpHour);
            }
        }

        if (setMode == SET_MIN) {
            if (readBtn(PIN_BTN_FM_UP)) {
                tmpMin = (tmpMin + 1) % 60;
                tft.fillRect(10, 65, 200, 20, ILI9341_BLACK);
                tft.setTextColor(ILI9341_CYAN);
                tft.setTextSize(2);
                tft.setCursor(10, 65);
                tft.printf("Min: %02d", tmpMin);
            }
            if (readBtn(PIN_BTN_FM_DOWN)) {
                tmpMin = (tmpMin == 0) ? 59 : tmpMin - 1;
                tft.fillRect(10, 65, 200, 20, ILI9341_BLACK);
                tft.setTextColor(ILI9341_CYAN);
                tft.setTextSize(2);
                tft.setCursor(10, 65);
                tft.printf("Min: %02d", tmpMin);
            }
        }

        if (setMode == SET_NONE) {
            uint8_t s = g_now.second();
            if (s != lastSecDrawn) {
                lastSecDrawn = s;
                drawScreen();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(80));
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Funciones auxiliares
// ═════════════════════════════════════════════════════════════════════════════
void applyFmFreq() {
    if (xSemaphoreTake(mutex_i2c, pdMS_TO_TICKS(100)) == pdTRUE) {
        radio.setBandFrequency(RADIO_BAND_FM, (uint32_t)g_fmFreq * 100);
        xSemaphoreGive(mutex_i2c);
    }
}

void drawScreen() {
    tft.fillRect(0, 0, 320, 80, ILI9341_BLACK);
    tft.setTextColor(ILI9341_WHITE);
    tft.setTextSize(5);
    tft.setCursor(30, 15);
    tft.printf("%02d:%02d", g_now.hour(), g_now.minute());

    tft.setTextSize(2);
    tft.setTextColor(ILI9341_DARKGREY);
    tft.setCursor(250, 45);
    tft.printf(":%02d", g_now.second());

    drawFreq();
    drawAlarmStatus();
}

void drawFreq() {
    tft.fillRect(0, 90, 320, 40, ILI9341_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(ILI9341_CYAN);
    tft.setCursor(10, 100);
    tft.printf("FM  %d.%d MHz", g_fmFreq / 10, g_fmFreq % 10);
}

void drawAlarmStatus() {
    tft.fillRect(0, 140, 320, 50, ILI9341_BLACK);
    tft.setTextSize(2);

    if (g_alarmFired) {
        tft.setTextColor(ILI9341_RED);
        tft.setCursor(10, 150);
        tft.print("! ALARMA SONANDO !");
        tft.setCursor(10, 175);
        tft.print("Snooze o apagar");
    } else if (g_alarm.enabled) {
        tft.setTextColor(ILI9341_GREEN);
        tft.setCursor(10, 150);
        tft.printf("Alarma: %02d:%02d ON", g_alarm.hour, g_alarm.minute);
        if (g_snoozed) {
            tft.setTextColor(ILI9341_ORANGE);
            tft.setCursor(10, 175);
            tft.print("(snooze activo)");
        }
    } else {
        tft.setTextColor(ILI9341_DARKGREY);
        tft.setCursor(10, 150);
        tft.print("Alarma: OFF");
        tft.setCursor(10, 175);
        tft.print("SET para programar");
    }
}

bool readBtn(uint8_t pin) {
    if (digitalRead(pin) == LOW) {
        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
        if (digitalRead(pin) == LOW) {
            while (digitalRead(pin) == LOW) vTaskDelay(pdMS_TO_TICKS(10));
            return true;
        }
    }
    return false;
}
*/
// ─────────────────────────────────────────────────────────────────────────────
// D.05 — Despertador con Radio FM Real (Versión Auriculares)
// Eduard Bravo Sánchez · Procesadores Digitales · UPC 2025/2026
//
// Hardware:
//   ESP32-S3        — MCU principal
//   TEA5767         — Sintonizador FM (I2C: SDA=GPIO8, SCL=GPIO9)
//   DS3231          — RTC (I2C compartido con TEA5767)
//   ILI9341 2.4"    — Display TFT no táctil (SPI: MOSI=11, CLK=12, CS=10, DC=13, RST=14)
//   BTN_FM_UP       — GPIO 1  — Seek FM hacia arriba
//   BTN_FM_DOWN     — GPIO 2  — Seek FM hacia abajo
//   BTN_SET         — GPIO 3  — Entrar/confirmar modo ajuste alarma
//   BTN_SNOOZE      — GPIO 4  — Snooze 5 min (1er pulso) / apagar alarma (2º pulso)
//   BTN_MONO_ST     — GPIO 5  — Alternar mono/stereo forzado
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <RTClib.h>
#include <Radio.h>
#include <TEA5767.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>

// ── Pines ─────────────────────────────────────────────────────────────────────
#define PIN_SDA          8
#define PIN_SCL          9
#define PIN_TFT_MOSI    11
#define PIN_TFT_CLK     12
#define PIN_TFT_CS      10
#define PIN_TFT_DC      13
#define PIN_TFT_RST     14
#define PIN_BTN_FM_UP    1
#define PIN_BTN_FM_DOWN  2
#define PIN_BTN_SET      3
#define PIN_BTN_SNOOZE   4
#define PIN_BTN_MONO_ST  5   // nuevo: alternar mono / stereo

// ── Frecuencias en unidades de 10 kHz ────────────────────────────────────────
#define FM_MIN          8750
#define FM_MAX         10800
#define FM_DEFAULT      9360

// Nivel de parada del seek hardware del TEA5767 (SSL bits)
#define TEA_SSL  0x40

// ── Alarma ────────────────────────────────────────────────────────────────────
#define SNOOZE_MINUTES     5
#define DEBOUNCE_MS       50
#define ALARM_HOUR_DEF     7
#define ALARM_MIN_DEF      0

// ── Tipos ─────────────────────────────────────────────────────────────────────
struct AlarmConfig {
    uint8_t hour;
    uint8_t minute;
    bool    enabled;
};

enum AlarmEvent { EVT_NONE, EVT_FIRE, EVT_SNOOZE, EVT_OFF };

// ── Objetos globales ──────────────────────────────────────────────────────────
RTC_DS3231       rtc;
TEA5767          radio;
Adafruit_ILI9341 tft(PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
Preferences      prefs;

// ── Variables compartidas ─────────────────────────────────────────────────────
volatile uint16_t  g_fmFreq      = FM_DEFAULT;
volatile bool      g_alarmFired  = false;
volatile bool      g_snoozed     = false;
volatile bool      g_forcesMono  = false;   // true = usuario forzó mono
volatile bool      g_isStereo    = false;   // true = el chip indica señal estéreo
AlarmConfig        g_alarm       = { ALARM_HOUR_DEF, ALARM_MIN_DEF, false };
DateTime           g_now;

// ── Primitivas FreeRTOS ───────────────────────────────────────────────────────
SemaphoreHandle_t  mutex_i2c;
SemaphoreHandle_t  sem_alarm;
QueueHandle_t      q_alarm_event;

// ── Prototipos ────────────────────────────────────────────────────────────────
void     rtc_task       (void *pv);
void     alarm_task     (void *pv);
void     ui_task        (void *pv);
void     tea_write      (uint16_t freq10kHz, bool searchMode, bool searchUp, uint8_t ssl);
uint16_t tea_seek_once  (bool up, uint16_t fromFreq);
bool     tea_read_stereo();               // lee el bit STEREO del chip
void     seekAndUpdate  (bool up);
void     applyFmFreq    ();
void     applyMonoStereo();               // aplica g_forcesMono al chip
void     drawScreen     ();
void     drawFreq       ();
void     drawSeeking    ();
void     drawAlarmStatus();
void     drawMonoStereo ();               // zona inferior: mono/stereo
bool     readBtn        (uint8_t pin);

// ═════════════════════════════════════════════════════════════════════════════
// setup
// ═════════════════════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);

    mutex_i2c     = xSemaphoreCreateMutex();
    sem_alarm     = xSemaphoreCreateBinary();
    q_alarm_event = xQueueCreate(4, sizeof(AlarmEvent));

    pinMode(PIN_BTN_FM_UP,   INPUT_PULLUP);
    pinMode(PIN_BTN_FM_DOWN, INPUT_PULLUP);
    pinMode(PIN_BTN_SET,     INPUT_PULLUP);
    pinMode(PIN_BTN_SNOOZE,  INPUT_PULLUP);
    pinMode(PIN_BTN_MONO_ST, INPUT_PULLUP);

    Wire.begin(PIN_SDA, PIN_SCL);

    if (!rtc.begin()) Serial.println("[ERROR] RTC no encontrado");
    if (rtc.lostPower()) {
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
        Serial.println("[RTC] Hora ajustada a compilacion");
    }

    SPI.begin(PIN_TFT_CLK, -1, PIN_TFT_MOSI, PIN_TFT_CS);
    tft.begin();
    tft.setRotation(1);
    tft.fillScreen(ILI9341_BLACK);
    tft.setTextColor(ILI9341_WHITE);
    tft.setTextSize(2);
    tft.setCursor(20, 100);
    tft.print("Iniciando...");

    radio.init();
    radio.setBandFrequency(RADIO_BAND_FM, FM_DEFAULT);
    radio.setVolume(15);
    radio.setMono(false);
    radio.setMute(true);

    prefs.begin("despertador", false);
    g_alarm.hour    = prefs.getUChar ("alh",  ALARM_HOUR_DEF);
    g_alarm.minute  = prefs.getUChar ("alm",  ALARM_MIN_DEF);
    g_alarm.enabled = prefs.getBool  ("ale",  false);
    g_fmFreq        = prefs.getUShort("freq", FM_DEFAULT);
    g_forcesMono    = prefs.getBool  ("mono", false);
    prefs.end();

    applyFmFreq();
    applyMonoStereo();

    xTaskCreatePinnedToCore(rtc_task,   "rtc",   2048, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(alarm_task, "alarm", 2048, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(ui_task,    "ui",    4096, NULL, 2, NULL, 1);

    Serial.println("[OK] Setup completo");
}

void loop() { vTaskDelay(portMAX_DELAY); }

// ═════════════════════════════════════════════════════════════════════════════
// rtc_task — lee DS3231 cada segundo y actualiza g_isStereo
// ═════════════════════════════════════════════════════════════════════════════
void rtc_task(void *pv) {
    for (;;) {
        if (xSemaphoreTake(mutex_i2c, pdMS_TO_TICKS(100)) == pdTRUE) {
            g_now = rtc.now();

            // Leer el bit STEREO del TEA5767 (byte 2, bit 7 de la respuesta)
            // Solo tiene sentido si no estamos forzando mono
            if (!g_forcesMono) {
                uint8_t buf[5] = {0};
                Wire.requestFrom(0x60, 5);
                for (int i = 0; i < 5; i++) buf[i] = Wire.available() ? Wire.read() : 0;
                g_isStereo = (buf[2] >> 7) & 0x01;
            } else {
                g_isStereo = false;
            }

            xSemaphoreGive(mutex_i2c);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// alarm_task
// ═════════════════════════════════════════════════════════════════════════════
void alarm_task(void *pv) {
    uint8_t lastMinuteTriggered = 99;
    for (;;) {
        if (g_alarm.enabled && !g_alarmFired) {
            uint8_t h = g_now.hour();
            uint8_t m = g_now.minute();
            if (h == g_alarm.hour && m == g_alarm.minute && m != lastMinuteTriggered) {
                lastMinuteTriggered = m;
                g_alarmFired = true;
                g_snoozed    = false;
                if (xSemaphoreTake(mutex_i2c, pdMS_TO_TICKS(100)) == pdTRUE) {
                    radio.setMute(false);
                    xSemaphoreGive(mutex_i2c);
                }
                AlarmEvent ev = EVT_FIRE;
                xQueueSend(q_alarm_event, &ev, 0);
                xSemaphoreGive(sem_alarm);
                Serial.println("[ALARM] Disparada!");
            }
        }
        if (g_now.minute() != g_alarm.minute) lastMinuteTriggered = 99;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// ui_task
// ═════════════════════════════════════════════════════════════════════════════
void ui_task(void *pv) {
    enum SetMode { SET_NONE, SET_HOUR, SET_MIN };
    SetMode setMode  = SET_NONE;
    uint8_t tmpHour  = g_alarm.hour;
    uint8_t tmpMin   = g_alarm.minute;
    uint8_t lastSec  = 99;
    bool    lastStereoState = !g_isStereo;   // forzar primer dibujo

    for (;;) {

        // ── BTN_FM_UP ─────────────────────────────────────────────────────────
        if (readBtn(PIN_BTN_FM_UP)) {
            if (setMode == SET_NONE) {
                seekAndUpdate(true);
            } else if (setMode == SET_HOUR) {
                tmpHour = (tmpHour + 1) % 24;
                tft.fillRect(10, 65, 200, 20, ILI9341_BLACK);
                tft.setTextColor(ILI9341_YELLOW); tft.setTextSize(2);
                tft.setCursor(10, 65); tft.printf("Hora: %02d", tmpHour);
            } else if (setMode == SET_MIN) {
                tmpMin = (tmpMin + 1) % 60;
                tft.fillRect(10, 65, 200, 20, ILI9341_BLACK);
                tft.setTextColor(ILI9341_CYAN); tft.setTextSize(2);
                tft.setCursor(10, 65); tft.printf("Min: %02d", tmpMin);
            }
        }

        // ── BTN_FM_DOWN ───────────────────────────────────────────────────────
        if (readBtn(PIN_BTN_FM_DOWN)) {
            if (setMode == SET_NONE) {
                seekAndUpdate(false);
            } else if (setMode == SET_HOUR) {
                tmpHour = (tmpHour == 0) ? 23 : tmpHour - 1;
                tft.fillRect(10, 65, 200, 20, ILI9341_BLACK);
                tft.setTextColor(ILI9341_YELLOW); tft.setTextSize(2);
                tft.setCursor(10, 65); tft.printf("Hora: %02d", tmpHour);
            } else if (setMode == SET_MIN) {
                tmpMin = (tmpMin == 0) ? 59 : tmpMin - 1;
                tft.fillRect(10, 65, 200, 20, ILI9341_BLACK);
                tft.setTextColor(ILI9341_CYAN); tft.setTextSize(2);
                tft.setCursor(10, 65); tft.printf("Min: %02d", tmpMin);
            }
        }

        // ── BTN_SET ───────────────────────────────────────────────────────────
        if (readBtn(PIN_BTN_SET)) {
            if (setMode == SET_NONE) {
                setMode = SET_HOUR; tmpHour = g_alarm.hour; tmpMin = g_alarm.minute;
                tft.fillScreen(ILI9341_BLACK);
                tft.setTextColor(ILI9341_YELLOW); tft.setTextSize(2);
                tft.setCursor(10, 10);  tft.print("Ajustar alarma");
                tft.setCursor(10, 40);  tft.print("FM+/FM- cambia hora");
                tft.setCursor(10, 65);  tft.printf("Hora: %02d", tmpHour);
                tft.setTextColor(ILI9341_DARKGREY);
                tft.setCursor(10, 95);  tft.print("SET = siguiente");
            } else if (setMode == SET_HOUR) {
                setMode = SET_MIN;
                tft.fillScreen(ILI9341_BLACK);
                tft.setTextColor(ILI9341_CYAN); tft.setTextSize(2);
                tft.setCursor(10, 10);  tft.print("Ajustar alarma");
                tft.setCursor(10, 40);  tft.print("FM+/FM- cambia min");
                tft.setCursor(10, 65);  tft.printf("Min: %02d", tmpMin);
                tft.setTextColor(ILI9341_DARKGREY);
                tft.setCursor(10, 95);  tft.print("SET = guardar");
            } else if (setMode == SET_MIN) {
                g_alarm.hour = tmpHour; g_alarm.minute = tmpMin; g_alarm.enabled = true;
                prefs.begin("despertador", false);
                prefs.putUChar("alh", g_alarm.hour);
                prefs.putUChar("alm", g_alarm.minute);
                prefs.putBool ("ale", true);
                prefs.end();
                setMode = SET_NONE;
                tft.fillScreen(ILI9341_BLACK);
                tft.setTextColor(ILI9341_GREEN); tft.setTextSize(2);
                tft.setCursor(10, 100);
                tft.printf("Alarma: %02d:%02d  OK!", g_alarm.hour, g_alarm.minute);
                vTaskDelay(pdMS_TO_TICKS(1500));
                tft.fillScreen(ILI9341_BLACK);
                lastSec = 99;
                lastStereoState = !g_isStereo;  // forzar redibujado
                Serial.printf("[SET] Alarma guardada %02d:%02d\n", g_alarm.hour, g_alarm.minute);
            }
        }

        // ── BTN_SNOOZE ────────────────────────────────────────────────────────
        if (readBtn(PIN_BTN_SNOOZE) && g_alarmFired) {
            if (!g_snoozed) {
                g_snoozed = true; g_alarmFired = false;
                if (xSemaphoreTake(mutex_i2c, pdMS_TO_TICKS(100)) == pdTRUE) {
                    radio.setMute(true); xSemaphoreGive(mutex_i2c);
                }
                uint8_t newMin  = (g_alarm.minute + SNOOZE_MINUTES) % 60;
                uint8_t newHour = g_alarm.hour;
                if (g_alarm.minute + SNOOZE_MINUTES >= 60) newHour = (newHour + 1) % 24;
                g_alarm.minute = newMin; g_alarm.hour = newHour;
                AlarmEvent ev = EVT_SNOOZE;
                xQueueSend(q_alarm_event, &ev, 0);
                Serial.printf("[SNOOZE] Reactivando a %02d:%02d\n", newHour, newMin);
            } else {
                g_alarmFired = false; g_snoozed = false;
                g_alarm.hour = tmpHour; g_alarm.minute = tmpMin;
                if (xSemaphoreTake(mutex_i2c, pdMS_TO_TICKS(100)) == pdTRUE) {
                    radio.setMute(true); xSemaphoreGive(mutex_i2c);
                }
                AlarmEvent ev = EVT_OFF;
                xQueueSend(q_alarm_event, &ev, 0);
                Serial.println("[ALARM] Apagada");
            }
        }

        // ── BTN_MONO_ST — alternar mono / stereo ──────────────────────────────
        if (readBtn(PIN_BTN_MONO_ST) && setMode == SET_NONE) {
            g_forcesMono = !g_forcesMono;
            applyMonoStereo();

            // Guardar preferencia en NVS
            prefs.begin("despertador", false);
            prefs.putBool("mono", g_forcesMono);
            prefs.end();

            Serial.printf("[AUDIO] Modo: %s\n", g_forcesMono ? "MONO forzado" : "STEREO auto");

            // Redibujar zona mono/stereo inmediatamente
            lastStereoState = !g_isStereo;  // forzar redibujado en el siguiente ciclo
        }

        // ── Redibujado pantalla principal ─────────────────────────────────────
        if (setMode == SET_NONE) {
            uint8_t s = g_now.second();
            if (s != lastSec) {
                lastSec = s;
                drawScreen();
                lastStereoState = g_isStereo;
            } else if (g_isStereo != lastStereoState) {
                // El estado mono/stereo cambió sin que cambie el segundo
                // (puede pasar al cambiar emisora o pulsar el botón)
                lastStereoState = g_isStereo;
                drawMonoStereo();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(80));
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// tea_write
// ═════════════════════════════════════════════════════════════════════════════
void tea_write(uint16_t freq10kHz, bool searchMode, bool searchUp, uint8_t ssl) {
    uint32_t freqKhz = (uint32_t)freq10kHz * 10;
    uint16_t pll     = (uint16_t)((4.0f * (float)(freqKhz + 225)) / 32.768f);

    uint8_t b[5];
    b[0] = (searchMode ? 0x40 : 0x00) | (uint8_t)((pll >> 8) & 0x3F);
    b[1] = (uint8_t)(pll & 0xFF);
    b[2] = (searchUp ? 0x80 : 0x00) | ssl | 0x10   // HLSI=1
         | (g_forcesMono ? 0x08 : 0x00);            // MS=1 → mono forzado
    b[3] = 0x10 | 0x04 | 0x02;  // XTAL | HCC | SNC
    b[4] = 0x00;

    Wire.beginTransmission(0x60);
    for (int i = 0; i < 5; i++) Wire.write(b[i]);
    Wire.endTransmission();
}

// ═════════════════════════════════════════════════════════════════════════════
// tea_seek_once
// ═════════════════════════════════════════════════════════════════════════════
uint16_t tea_seek_once(bool up, uint16_t fromFreq) {
    tea_write(fromFreq, true, up, TEA_SSL);

    uint8_t  resp[5] = {0};
    uint32_t t0      = millis();

    while (millis() - t0 < 5000) {
        vTaskDelay(pdMS_TO_TICKS(50));
        Wire.requestFrom(0x60, 5);
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

// ═════════════════════════════════════════════════════════════════════════════
// applyMonoStereo — aplica el modo mono/stereo al chip sin cambiar frecuencia
// ═════════════════════════════════════════════════════════════════════════════
void applyMonoStereo() {
    if (xSemaphoreTake(mutex_i2c, pdMS_TO_TICKS(200)) == pdTRUE) {
        // Reescribir los registros con la frecuencia actual y el bit MS actualizado
        tea_write(g_fmFreq, false, false, 0);
        vTaskDelay(pdMS_TO_TICKS(80));
        xSemaphoreGive(mutex_i2c);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// seekAndUpdate
// ═════════════════════════════════════════════════════════════════════════════
void seekAndUpdate(bool up) {
    drawSeeking();

    uint16_t original = g_fmFreq;
    uint16_t result   = original;

    if (xSemaphoreTake(mutex_i2c, pdMS_TO_TICKS(15000)) == pdTRUE) {

        uint16_t startFreq = original;
        int maxAttempts    = 3;

        for (int attempt = 0; attempt < maxAttempts; attempt++) {
            uint16_t from = startFreq;
            if (up) {
                from += 1;
                if (from > FM_MAX) from = FM_MIN;
            } else {
                from -= 1;
                if (from < FM_MIN) from = FM_MAX;
            }

            Serial.printf("[SEEK] Intento %d desde %d.%d MHz\n",
                attempt + 1, from / 100, (from % 100) / 10);

            uint16_t found = tea_seek_once(up, from);

            if (found == 0) {
                Serial.println("[SEEK] Limite de banda, wrap");
                startFreq = up ? FM_MIN : FM_MAX;
                continue;
            }

            if (found != original) {
                result = found;
                Serial.printf("[SEEK] OK: %d.%d MHz\n",
                    result / 100, (result % 100) / 10);
                break;
            }

            Serial.println("[SEEK] Misma frecuencia, saltando");
            startFreq = found;
        }

        tea_write(result, false, false, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
        g_fmFreq = result;
        xSemaphoreGive(mutex_i2c);
    }

    prefs.begin("despertador", false);
    prefs.putUShort("freq", g_fmFreq);
    prefs.end();

    drawFreq();
}

// ═════════════════════════════════════════════════════════════════════════════
// applyFmFreq
// ═════════════════════════════════════════════════════════════════════════════
void applyFmFreq() {
    if (xSemaphoreTake(mutex_i2c, pdMS_TO_TICKS(200)) == pdTRUE) {
        tea_write(g_fmFreq, false, false, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        xSemaphoreGive(mutex_i2c);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Funciones de pantalla
// ═════════════════════════════════════════════════════════════════════════════

// Layout de la pantalla (320x240 landscape):
//   Y   0-84   → Hora grande (HH:MM) + segundos
//   Y  90-124  → Frecuencia FM
//   Y 130-164  → Estado alarma
//   Y 195-229  → Mono / Stereo

void drawScreen() {
    // ── Hora ──────────────────────────────────────────────────────────────────
    tft.fillRect(0, 0, 320, 85, ILI9341_BLACK);
    tft.setTextColor(ILI9341_WHITE); tft.setTextSize(5);
    tft.setCursor(30, 15);
    tft.printf("%02d:%02d", g_now.hour(), g_now.minute());
    tft.setTextColor(ILI9341_DARKGREY); tft.setTextSize(2);
    tft.setCursor(250, 50);
    tft.printf(":%02d", g_now.second());

    drawFreq();
    drawAlarmStatus();
    drawMonoStereo();
}

void drawFreq() {
    tft.fillRect(0, 90, 320, 35, ILI9341_BLACK);
    tft.setTextColor(ILI9341_CYAN); tft.setTextSize(2);
    tft.setCursor(10, 100);
    tft.printf("FM  %d.%d MHz", g_fmFreq / 100, (g_fmFreq % 100) / 10);
}

void drawSeeking() {
    tft.fillRect(0, 90, 320, 35, ILI9341_BLACK);
    tft.setTextColor(ILI9341_YELLOW); tft.setTextSize(2);
    tft.setCursor(10, 100);
    tft.print("Buscando emisora...");
}

void drawAlarmStatus() {
    tft.fillRect(0, 130, 320, 60, ILI9341_BLACK);
    tft.setTextSize(2);
    if (g_alarmFired) {
        tft.setTextColor(ILI9341_RED);
        tft.setCursor(10, 138); tft.print("! ALARMA SONANDO !");
        tft.setCursor(10, 160); tft.print("Snooze o apagar");
    } else if (g_alarm.enabled) {
        tft.setTextColor(ILI9341_GREEN);
        tft.setCursor(10, 138);
        tft.printf("Alarma: %02d:%02d  ON", g_alarm.hour, g_alarm.minute);
        if (g_snoozed) {
            tft.setTextColor(ILI9341_ORANGE);
            tft.setCursor(10, 160); tft.print("(snooze activo)");
        }
    } else {
        tft.setTextColor(ILI9341_DARKGREY);
        tft.setCursor(10, 138); tft.print("Alarma: OFF");
        tft.setCursor(10, 160); tft.print("SET para programar");
    }
}

// drawMonoStereo — zona inferior de la pantalla
// Muestra tres casos:
//   - MONO forzado por el usuario (amarillo)
//   - STEREO activo (verde)
//   - MONO por falta de señal estéreo en la emisora (gris)
void drawMonoStereo() {
    tft.fillRect(0, 195, 320, 45, ILI9341_BLACK);
    tft.setTextSize(2);

    if (g_forcesMono) {
        tft.setTextColor(ILI9341_RED);
        tft.setCursor(10, 205);
        tft.print("MONO  (forzado)");
        tft.setTextColor(ILI9341_YELLOW);
        tft.setCursor(10, 222);
        tft.print("Pulsa MONO/ST p/ stereo");
    } else if (g_isStereo) {
        tft.setTextColor(ILI9341_YELLOW);
        tft.setCursor(10, 205);
        tft.print("STEREO");
        tft.setTextColor(ILI9341_YELLOW);
        tft.setCursor(10, 222);
        tft.print("Pulsa MONO/ST p/ mono");
    } else {
        tft.setTextColor(ILI9341_RED);
        tft.setCursor(10, 205);
        tft.print("MONO  (emisora)");
        tft.setTextColor(ILI9341_YELLOW);
        tft.setCursor(10, 222);
        tft.print("Pulsa MONO/ST p/ forzar");
    }
}

bool readBtn(uint8_t pin) {
    if (digitalRead(pin) == LOW) {
        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
        if (digitalRead(pin) == LOW) {
            while (digitalRead(pin) == LOW) vTaskDelay(pdMS_TO_TICKS(10));
            return true;
        }
    }
    return false;
}