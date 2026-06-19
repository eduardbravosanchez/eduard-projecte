// ─────────────────────────────────────────────────────────────────────────────
// D.05 — Despertador amb Ràdio FM Real (Versió Auriculars)
// Eduard Bravo Sánchez · Processadors Digitals · UPC 2025/2026
//
// Hardware:
//   ESP32-S3        — MCU principal
//   TEA5767         — Sintonitzador FM (I2C: SDA=GPIO8, SCL=GPIO9)
//   DS3231          — RTC (I2C compartit amb TEA5767)
//   ILI9341 2.4"    — Display TFT no tàctil (SPI: MOSI=11, CLK=12, CS=10, DC=13, RST=14)
//   BTN_FM_UP       — GPIO 1  — Seek FM cap amunt
//   BTN_FM_DOWN     — GPIO 2  — Seek FM cap avall
//   BTN_SET         — GPIO 3  — Polsació curta: ajust alarma / Polsació llarga (3s): ajust hora rellotge
//   BTN_SNOOZE      — GPIO 4  — Snooze 5 min (1r pols) / apagar alarma (2n pols)
//   BTN_MONO_ST     — GPIO 5  — Alternar mono/estèreo forçat
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include "config.h"             // Configuració global de pins, ràdio i FreeRTOS
#include "version.h"            // Control de versions del firmware
#include <Wire.h>               // Comunicació I2C (TEA5767 + DS3231)
#include <SPI.h>                // Comunicació SPI (TFT ILI9341)
#include <RTClib.h>             // Llibreria per al rellotge DS3231
#include <Radio.h>              // Classe base de la llibreria mathertel/Radio
#include <TEA5767.h>            // Driver específic per al sintonitzador TEA5767
#include <Adafruit_GFX.h>       // Gràfics genèrics per a displays
#include <Adafruit_ILI9341.h>   // Driver per al TFT ILI9341
#include <Preferences.h>        // Emmagatzematge persistent en NVS (flash interna)
#include <freertos/FreeRTOS.h>  // Sistema operatiu en temps real
#include <freertos/task.h>      // Gestió de tasques FreeRTOS
#include <freertos/semphr.h>    // Semàfors i mutexos FreeRTOS
#include <freertos/queue.h>     // Cues de missatges FreeRTOS

// ── Tipus de dades ────────────────────────────────────────────────────────────

// Configuració de l'alarma: hora, minut i si està activada
struct AlarmConfig {
    uint8_t hour;
    uint8_t minute;
    bool    enabled;
};

// Esdeveniments que la alarm_task envia a la ui_task per actualitzar la pantalla
enum AlarmEvent { EVT_NONE, EVT_FIRE, EVT_SNOOZE, EVT_OFF };

// ── Objectes globals ──────────────────────────────────────────────────────────
RTC_DS3231       rtc;                                  // Rellotge en temps real DS3231
TEA5767          radio;                                // Sintonitzador FM TEA5767
Adafruit_ILI9341 tft(PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST); // Display TFT
Preferences      prefs;                                // Emmagatzematge NVS

// ── Variables compartides entre tasques ──────────────────────────────────────
// S'accedeix des de múltiples tasques FreeRTOS; les volàtils eviten optimitzacions
// del compilador que podrien llegir valors obsolets de registres de la CPU
volatile uint16_t  g_fmFreq      = FM_DEFAULT; // Freqüència actual en unitats 10kHz
volatile bool      g_alarmFired  = false;      // true quan l'alarma s'ha disparat
volatile bool      g_snoozed     = false;      // true si s'ha activat el snooze
volatile bool      g_forcesMono  = false;      // true si l'usuari ha forçat mode mono
volatile bool      g_isStereo    = false;      // true si el chip detecta senyal estèreo
volatile bool g_radioMuted = true;  // la radio arranca silenciada
AlarmConfig        g_alarm       = { ALARM_HOUR_DEF, ALARM_MIN_DEF, false };
DateTime           g_now;                       // Hora actual, actualitzada per rtc_task

// ── Primitives FreeRTOS ───────────────────────────────────────────────────────
SemaphoreHandle_t  mutex_i2c;      // Mutex per protegir l'accés al bus I2C compartit
SemaphoreHandle_t  sem_alarm;      // Semàfor binari: alarm_task → ui_task (alarma disparada)
QueueHandle_t      q_alarm_event;  // Cua: missatges d'estat de l'alarma cap a ui_task

// ── Prototips de funcions ─────────────────────────────────────────────────────
void     rtc_task       (void *pv);
void     alarm_task     (void *pv);
void     ui_task        (void *pv);
void     tea_write      (uint16_t freq10kHz, bool searchMode, bool searchUp, uint8_t ssl);
uint16_t tea_seek_once  (bool up, uint16_t fromFreq);
void     seekAndUpdate  (bool up);
void     applyFmFreq    ();
void     applyMonoStereo();
void     drawScreen     ();
void     drawFreq       ();
void     drawSeeking    ();
void     drawAlarmStatus();
void     drawMonoStereo ();
bool     readBtn        (uint8_t pin);

// ═════════════════════════════════════════════════════════════════════════════
// setup — s'executa una sola vegada en arrencar l'ESP32
// ═════════════════════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200); // Port sèrie per depuració (monitor sèrie PlatformIO)

    Serial.printf("\n=============================================\n");
    Serial.printf(" Projecte: D.05 Despertador FM Real\n");
    Serial.printf(" Firmware Versió: %s\n", FIRMWARE_VERSION_STR);
    Serial.printf(" Data de compilació: %s - %s\n", FIRMWARE_BUILD_DATE, FIRMWARE_BUILD_TIME);
    Serial.printf("=============================================\n\n");

    // ── 1. Crear primitives FreeRTOS ABANS de crear tasques ───────────────────
    // Si es creessin les tasques primer, podrien intentar usar mutex_i2c = NULL
    mutex_i2c     = xSemaphoreCreateMutex();           // Protegeix el bus I2C
    sem_alarm     = xSemaphoreCreateBinary();          // Senyal d'alarma disparada
    q_alarm_event = xQueueCreate(4, sizeof(AlarmEvent)); // Cua d'esdeveniments d'alarma

    // ── 2. Configurar polsadors com a entrades amb pull-up intern ─────────────
    // INPUT_PULLUP: pin a 3.3V quan el polsador no és premut, a GND quan sí
    // Així no cal cap resistència externa
    pinMode(PIN_BTN_FM_UP,   INPUT_PULLUP);
    pinMode(PIN_BTN_FM_DOWN, INPUT_PULLUP);
    pinMode(PIN_BTN_SET,     INPUT_PULLUP);
    pinMode(PIN_BTN_SNOOZE,  INPUT_PULLUP);
    pinMode(PIN_BTN_MONO_ST, INPUT_PULLUP);

    // ── 3. Inicialitzar bus I2C amb els pins definits ─────────────────────────
    // TEA5767 (0x60) i DS3231 (0x68) comparteixen el mateix bus
    Wire.begin(PIN_SDA, PIN_SCL);

    // ── 4. Inicialitzar el rellotge DS3231 ────────────────────────────────────
    if (!rtc.begin()) {
        Serial.println("[ERROR] RTC no trobat — comprova les connexions I2C");
    }
    // Si el DS3231 ha perdut alimentació (pila gastada o primer ús),
    // ajustar l'hora a la data/hora de compilació del firmware
    if (rtc.lostPower()) {
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
        Serial.println("[RTC] Hora ajustada a hora de compilació");
    }

    // ── 5. Inicialitzar el display TFT ILI9341 per SPI ───────────────────────
    // El segon paràmetre (-1) indica que no hi ha pin MISO (no llegim del TFT)
    SPI.begin(PIN_TFT_CLK, -1, PIN_TFT_MOSI, PIN_TFT_CS);
    tft.begin();
    tft.setRotation(1);              // Orientació horitzontal (landscape 320x240)
    tft.fillScreen(ILI9341_BLACK);   // Netejar pantalla
    tft.setTextColor(ILI9341_WHITE);
    tft.setTextSize(2);
    tft.setCursor(20, 100);
    tft.print("Iniciant...");        // Missatge d'arrencada mentre carrega

    // ── 6. Inicialitzar el sintonitzador TEA5767 ──────────────────────────────
    radio.init();                                     // Inicialitza el chip per I2C
    radio.setBandFrequency(RADIO_BAND_FM, FM_DEFAULT); // Sintonitzar freqüència per defecte
    radio.setVolume(15);                              // Volum màxim del chip (sense efecte real al TEA5767)
    radio.setMono(false);                             // Mode estèreo per defecte
    radio.setMute(true);                              // Silenciar al arrencar (fins que soni l'alarma)

    // ── 7. Carregar configuració guardada a NVS (memòria flash) ──────────────
    // Les dades persisteixen entre reinicis i talls de corrent
    prefs.begin(NVS_NAMESPACE, false);
    g_alarm.hour    = prefs.getUChar (NVS_KEY_ALARM_H, ALARM_HOUR_DEF); // Hora alarma
    g_alarm.minute  = prefs.getUChar (NVS_KEY_ALARM_M, ALARM_MIN_DEF);  // Minut alarma
    g_alarm.enabled = prefs.getBool  (NVS_KEY_ALARM_E, false);          // Alarma activa?
    g_fmFreq        = prefs.getUShort(NVS_KEY_FREQ,    FM_DEFAULT);     // Última freqüència
    g_forcesMono    = prefs.getBool  (NVS_KEY_MONO,    false);          // Mode mono forçat?
    prefs.end();

    // Aplicar la freqüència i el mode mono/estèreo carregats de NVS
    applyFmFreq();
    applyMonoStereo();

    // ── 8. Crear les tasques FreeRTOS ─────────────────────────────────────────
    // Cada tasca té la seva pròpia pila i prioritat. Les tasques d'alta prioritat
    // interrompen les de baixa quan necessiten CPU.
    // Paràmetres: funció, nom, mida pila (bytes), paràmetre, prioritat, handle, nucli
    xTaskCreatePinnedToCore(rtc_task,   "rtc",   TASK_RTC_STACK,   NULL, TASK_RTC_PRIO,   NULL, TASK_RUNNING_CORE); // Prio 3: llegeix hora
    xTaskCreatePinnedToCore(alarm_task, "alarm", TASK_ALARM_STACK, NULL, TASK_ALARM_PRIO, NULL, TASK_RUNNING_CORE); // Prio 4: comprova alarma
    xTaskCreatePinnedToCore(ui_task,    "ui",    TASK_UI_STACK,    NULL, TASK_UI_PRIO,    NULL, TASK_RUNNING_CORE); // Prio 2: pantalla + polsadors

    Serial.println("[OK] Setup complet");
}

// loop buit — tot corre dins de tasques FreeRTOS
void loop() { vTaskDelay(portMAX_DELAY); }

// ═════════════════════════════════════════════════════════════════════════════
// rtc_task — Prioritat 3
// Llegeix el DS3231 cada segon i actualitza g_now i g_isStereo.
// Protegeix l'accés I2C amb mutex_i2c per evitar col·lisions amb alarm_task.
// ═════════════════════════════════════════════════════════════════════════════
void rtc_task(void *pv) {
    for (;;) {
        // Intentar agafar el mutex I2C; si no s'aconsegueix en 100ms, saltar
        if (xSemaphoreTake(mutex_i2c, pdMS_TO_TICKS(100)) == pdTRUE) {

            // Llegir la data i hora actual del DS3231
            g_now = rtc.now();

            // Llegir l'estat estèreo del TEA5767 directament per I2C
            // El chip retorna 5 bytes d'estat; el bit 7 del byte 2 indica STEREO
            if (!g_forcesMono) {
                uint8_t buf[5] = {0};
                Wire.requestFrom(TEA5767_I2C_ADDR, 5); // Sol·licitar 5 bytes al TEA5767
                for (int i = 0; i < 5; i++) buf[i] = Wire.available() ? Wire.read() : 0;
                g_isStereo = (buf[2] >> 7) & 0x01; // Bit 7 del byte 2 = indicador STEREO
            } else {
                g_isStereo = false; // En mode mono forçat sempre mostrem MONO
            }

            xSemaphoreGive(mutex_i2c); // Alliberar el mutex perquè altres tasques puguin usar I2C
        }
        vTaskDelay(pdMS_TO_TICKS(1000)); // Esperar 1 segon fins a la propera lectura
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// alarm_task — Prioritat 4 (la més alta de les tres)
// Comprova cada 500ms si l'hora actual coincideix amb l'alarma programada.
// Quan es dispara, activa l'àudio del TEA5767 i notifica la ui_task.
// ═════════════════════════════════════════════════════════════════════════════
void alarm_task(void *pv) {
    uint8_t lastMinuteTriggered = 99; // Evita disparar l'alarma dues vegades al mateix minut

    for (;;) {
        // Comprovar si l'alarma és activa i no s'ha disparat ja
        if (g_alarm.enabled && !g_alarmFired) {
            uint8_t h = g_now.hour();
            uint8_t m = g_now.minute();

            // Comparar hora i minut actuals amb l'alarma programada
            if (h == g_alarm.hour && m == g_alarm.minute && m != lastMinuteTriggered) {
                lastMinuteTriggered = m;
                g_alarmFired = true;
                g_snoozed    = false;

                // Activar l'àudio: treure el mute del TEA5767
                if (xSemaphoreTake(mutex_i2c, pdMS_TO_TICKS(100)) == pdTRUE) {
                    radio.setMute(false); // La ràdio FM comença a sonar pels auriculars
                    xSemaphoreGive(mutex_i2c);
                }

                // Notificar a ui_task que l'alarma s'ha disparat
                AlarmEvent ev = EVT_FIRE;
                xQueueSend(q_alarm_event, &ev, 0); // Enviar event a la cua
                xSemaphoreGive(sem_alarm);          // Senyalar el semàfor
                Serial.println("[ALARM] Disparada!");
            }
        }

        // Resetar el control de minut quan el minut actual ja ha passat
        if (g_now.minute() != g_alarm.minute) lastMinuteTriggered = 99;

        vTaskDelay(pdMS_TO_TICKS(500)); // Comprovar dues vegades per segon
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// ui_task — Prioritat 2
// Gestiona tots els polsadors i actualitza el display TFT.
// És la tasca de menor prioritat perquè un retard de ms en la pantalla
// és imperceptible, mentre que l'àudio i l'alarma han de ser puntuals.
// ═════════════════════════════════════════════════════════════════════════════
void ui_task(void *pv) {

    // Màquina d'estats del menú de configuració
    enum SetMode {
        SET_NONE,        // Mode normal: mostra hora, FM i estat alarma
        SET_HOUR,        // Ajustant hora de l'alarma
        SET_MIN,         // Ajustant minut de l'alarma
        SET_CLOCK_HOUR,  // Ajustant hora del rellotge del sistema
        SET_CLOCK_MIN    // Ajustant minut del rellotge del sistema
    };

    SetMode setMode      = SET_NONE;
    uint8_t tmpHour      = g_alarm.hour;    // Hora temporal mentre s'ajusta l'alarma
    uint8_t tmpMin       = g_alarm.minute;  // Minut temporal mentre s'ajusta l'alarma
    uint8_t tmpClockHour = 0;               // Hora temporal mentre s'ajusta el rellotge
    uint8_t tmpClockMin  = 0;               // Minut temporal mentre s'ajusta el rellotge
    uint8_t lastSec      = 99;              // Últim segon dibuixat (evita redibuixar si no ha canviat)
    bool    lastStereoState = !g_isStereo;  // Estat anterior mono/estèreo per detectar canvis

    for (;;) {

        // ── BTN_FM_UP — comportament depenent del mode actual ─────────────────
        if (readBtn(PIN_BTN_FM_UP)) {
            if (setMode == SET_NONE) {
                // Mode normal: buscar la propera emisora cap amunt
                seekAndUpdate(true);
            } else if (setMode == SET_HOUR) {
                // Mode ajust alarma: incrementar hora
                tmpHour = (tmpHour + 1) % 24;
                tft.fillRect(10, 65, 200, 20, ILI9341_BLACK);
                tft.setTextColor(ILI9341_YELLOW); tft.setTextSize(2);
                tft.setCursor(10, 65); tft.printf("Hora: %02d", tmpHour);
            } else if (setMode == SET_MIN) {
                // Mode ajust alarma: incrementar minut
                tmpMin = (tmpMin + 1) % 60;
                tft.fillRect(10, 65, 200, 20, ILI9341_BLACK);
                tft.setTextColor(ILI9341_CYAN); tft.setTextSize(2);
                tft.setCursor(10, 65); tft.printf("Min: %02d", tmpMin);
            } else if (setMode == SET_CLOCK_HOUR) {
                // Mode ajust rellotge: incrementar hora
                tmpClockHour = (tmpClockHour + 1) % 24;
                tft.fillRect(10, 65, 200, 20, ILI9341_BLACK);
                tft.setTextColor(ILI9341_YELLOW); tft.setTextSize(2);
                tft.setCursor(10, 65); tft.printf("Hora: %02d", tmpClockHour);
            } else if (setMode == SET_CLOCK_MIN) {
                // Mode ajust rellotge: incrementar minut
                tmpClockMin = (tmpClockMin + 1) % 60;
                tft.fillRect(10, 65, 200, 20, ILI9341_BLACK);
                tft.setTextColor(ILI9341_CYAN); tft.setTextSize(2);
                tft.setCursor(10, 65); tft.printf("Min: %02d", tmpClockMin);
            }
        }

        // ── BTN_FM_DOWN — igual que FM_UP però en sentit contrari ─────────────
        if (readBtn(PIN_BTN_FM_DOWN)) {
            if (setMode == SET_NONE) {
                seekAndUpdate(false); // Buscar emisora cap avall
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
            } else if (setMode == SET_CLOCK_HOUR) {
                tmpClockHour = (tmpClockHour == 0) ? 23 : tmpClockHour - 1;
                tft.fillRect(10, 65, 200, 20, ILI9341_BLACK);
                tft.setTextColor(ILI9341_YELLOW); tft.setTextSize(2);
                tft.setCursor(10, 65); tft.printf("Hora: %02d", tmpClockHour);
            } else if (setMode == SET_CLOCK_MIN) {
                tmpClockMin = (tmpClockMin == 0) ? 59 : tmpClockMin - 1;
                tft.fillRect(10, 65, 200, 20, ILI9341_BLACK);
                tft.setTextColor(ILI9341_CYAN); tft.setTextSize(2);
                tft.setCursor(10, 65); tft.printf("Min: %02d", tmpClockMin);
            }
        }

        // ── BTN_SET — detecció de polsació curta vs llarga ────────────────────
        // Polsació curta (<3s): navegar pel menú d'alarma o confirmar valors
        // Polsació llarga (≥3s) en mode normal: entrar al menú d'ajust del rellotge
        if (digitalRead(PIN_BTN_SET) == LOW) {
            vTaskDelay(pdMS_TO_TICKS(50)); // Antirebote inicial
            if (digitalRead(PIN_BTN_SET) == LOW) {
                uint32_t t0 = millis();
                // Esperar a que l'usuari deixi anar el polsador, mesurant el temps premut
                while (digitalRead(PIN_BTN_SET) == LOW) vTaskDelay(pdMS_TO_TICKS(10));
                uint32_t held = millis() - t0; // Temps total que ha estat premut (ms)

                if (held >= 3000 && setMode == SET_NONE) {
                    // ── POLSACIÓ LLARGA: entrar en mode ajust hora del rellotge ──
                    setMode = SET_CLOCK_HOUR;
                    tmpClockHour = g_now.hour();   // Partir de l'hora actual
                    tmpClockMin  = g_now.minute();
                    tft.fillScreen(ILI9341_BLACK);
                    tft.setTextColor(ILI9341_MAGENTA); tft.setTextSize(2); // Magenta = ajust rellotge
                    tft.setCursor(10, 10);  tft.print("Ajustar hora rellotge");
                    tft.setCursor(10, 40);  tft.print("FM+/FM- canvia hora");
                    tft.setCursor(10, 65);  tft.printf("Hora: %02d", tmpClockHour);
                    tft.setTextColor(ILI9341_DARKGREY);
                    tft.setCursor(10, 95);  tft.print("SET = seguent");

                } else if (held < 3000) {
                    // ── POLSACIÓ CURTA: lògica de navegació del menú ─────────────
                    if (setMode == SET_NONE) {
                        // Entrar al mode ajust alarma: primer ajustar l'hora
                        setMode = SET_HOUR;
                        tmpHour = g_alarm.hour;
                        tmpMin  = g_alarm.minute;
                        tft.fillScreen(ILI9341_BLACK);
                        tft.setTextColor(ILI9341_YELLOW); tft.setTextSize(2);
                        tft.setCursor(10, 10);  tft.print("Ajustar alarma");
                        tft.setCursor(10, 40);  tft.print("FM+/FM- canvia hora");
                        tft.setCursor(10, 65);  tft.printf("Hora: %02d", tmpHour);
                        tft.setTextColor(ILI9341_DARKGREY);
                        tft.setCursor(10, 95);  tft.print("SET = seguent");

                    } else if (setMode == SET_HOUR) {
                        // Hora confirmada, passar a ajustar el minut
                        setMode = SET_MIN;
                        tft.fillScreen(ILI9341_BLACK);
                        tft.setTextColor(ILI9341_CYAN); tft.setTextSize(2);
                        tft.setCursor(10, 10);  tft.print("Ajustar alarma");
                        tft.setCursor(10, 40);  tft.print("FM+/FM- canvia min");
                        tft.setCursor(10, 65);  tft.printf("Min: %02d", tmpMin);
                        tft.setTextColor(ILI9341_DARKGREY);
                        tft.setCursor(10, 95);  tft.print("SET = guardar");

                    } else if (setMode == SET_MIN) {
                        // Minut confirmat: guardar alarma a NVS i tornar a mode normal
                        g_alarm.hour    = tmpHour;
                        g_alarm.minute  = tmpMin;
                        g_alarm.enabled = true;
                        prefs.begin(NVS_NAMESPACE, false);
                        prefs.putUChar(NVS_KEY_ALARM_H, g_alarm.hour);
                        prefs.putUChar(NVS_KEY_ALARM_M, g_alarm.minute);
                        prefs.putBool (NVS_KEY_ALARM_E, true);
                        prefs.end();
                        setMode = SET_NONE;
                        tft.fillScreen(ILI9341_BLACK);
                        tft.setTextColor(ILI9341_GREEN); tft.setTextSize(2);
                        tft.setCursor(10, 100);
                        tft.printf("Alarma: %02d:%02d  OK!", g_alarm.hour, g_alarm.minute);
                        vTaskDelay(pdMS_TO_TICKS(1500)); // Mostrar confirmació 1.5 s
                        tft.fillScreen(ILI9341_BLACK);
                        lastSec = 99;
                        lastStereoState = !g_isStereo;
                        Serial.printf("[SET] Alarma guardada %02d:%02d\n", g_alarm.hour, g_alarm.minute);

                    } else if (setMode == SET_CLOCK_HOUR) {
                        // Hora del rellotge confirmada, passar a ajustar el minut
                        setMode = SET_CLOCK_MIN;
                        tft.fillScreen(ILI9341_BLACK);
                        tft.setTextColor(ILI9341_MAGENTA); tft.setTextSize(2);
                        tft.setCursor(10, 10);  tft.print("Ajustar hora rellotge");
                        tft.setCursor(10, 40);  tft.print("FM+/FM- canvia min");
                        tft.setCursor(10, 65);  tft.printf("Min: %02d", tmpClockMin);
                        tft.setTextColor(ILI9341_DARKGREY);
                        tft.setCursor(10, 95);  tft.print("SET = guardar");

                    } else if (setMode == SET_CLOCK_MIN) {
                        // Minut confirmat: escriure la nova hora al DS3231
                        if (xSemaphoreTake(mutex_i2c, pdMS_TO_TICKS(200)) == pdTRUE) {
                            // Mantenim any, mes i dia actuals; canviem hora i minut
                            rtc.adjust(DateTime(
                                g_now.year(), g_now.month(), g_now.day(),
                                tmpClockHour, tmpClockMin, 0
                            ));
                            xSemaphoreGive(mutex_i2c);
                        }
                        setMode = SET_NONE;
                        tft.fillScreen(ILI9341_BLACK);
                        tft.setTextColor(ILI9341_MAGENTA); tft.setTextSize(2);
                        tft.setCursor(10, 100);
                        tft.printf("Hora: %02d:%02d  OK!", tmpClockHour, tmpClockMin);
                        vTaskDelay(pdMS_TO_TICKS(1500));
                        tft.fillScreen(ILI9341_BLACK);
                        lastSec = 99;
                        lastStereoState = !g_isStereo;
                        Serial.printf("[CLOCK] Hora ajustada a %02d:%02d\n", tmpClockHour, tmpClockMin);
                    }
                }
            }
        }

        // ── BTN_SNOOZE ────────────────────────────────────────────────────────────
        if (readBtn(PIN_BTN_SNOOZE)) {
            if (g_alarmFired) {
                // ── Alarma sonando: comportament actual ───────────────────────────
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
                    Serial.printf("[SNOOZE] Reactivant a %02d:%02d\n", newHour, newMin);
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
            } else {
                // ── Alarma NO sonant: toggle mute/unmute de la radio ──────────────
                g_radioMuted = !g_radioMuted;
                if (xSemaphoreTake(mutex_i2c, pdMS_TO_TICKS(100)) == pdTRUE) {
                    radio.setMute(g_radioMuted);
                    xSemaphoreGive(mutex_i2c);
                }
                Serial.printf("[RADIO] %s\n", g_radioMuted ? "MUTE" : "ON");
                // Redibuixar zona freqüència per mostrar l'estat
                drawFreq();
            }
        }

        // ── BTN_MONO_ST — alternar entre mono forçat i estèreo automàtic ─────
        if (readBtn(PIN_BTN_MONO_ST) && setMode == SET_NONE) {
            g_forcesMono = !g_forcesMono; // Invertir l'estat actual
            applyMonoStereo();             // Aplicar el canvi al chip TEA5767

            // Guardar la preferència a NVS perquè es recordi en reinicis
            prefs.begin(NVS_NAMESPACE, false);
            prefs.putBool(NVS_KEY_MONO, g_forcesMono);
            prefs.end();

            Serial.printf("[AUDIO] Mode: %s\n", g_forcesMono ? "MONO forçat" : "STEREO auto");
            lastStereoState = !g_isStereo; // Forçar redibuixat de la zona mono/estèreo
        }

        // ── Redibuixat de la pantalla principal ───────────────────────────────
        if (setMode == SET_NONE) {
            uint8_t s = g_now.second();
            if (s != lastSec) {
                // El segon ha canviat: redibuixar tota la pantalla principal
                lastSec = s;
                drawScreen();
                lastStereoState = g_isStereo;
            } else if (g_isStereo != lastStereoState) {
                // L'estat mono/estèreo ha canviat sense que hagi canviat el segon
                // (per exemple, en canviar d'emisora o prémer el botó MONO/ST)
                lastStereoState = g_isStereo;
                drawMonoStereo(); // Redibuixar només la zona afectada
            }
        }

        vTaskDelay(pdMS_TO_TICKS(80)); // Bucle cada 80ms (~12 lectures/s dels polsadors)
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// tea_write — escriu els 5 bytes de configuració al TEA5767 directament per I2C
//
// El TEA5767 s'inicialitza/configura escrivint 5 bytes al bus I2C.
// Cada byte controla uns determinats paràmetres del chip (PLL, seek, mode...).
// Sempre cridar amb mutex_i2c ja agafat.
// ═════════════════════════════════════════════════════════════════════════════
void tea_write(uint16_t freq10kHz, bool searchMode, bool searchUp, uint8_t ssl) {

    // Càlcul del valor PLL per a la freqüència donada (injecció per costat alt):
    // PLL = 4 × (freq_kHz + 225) / 32.768
    // On freq_kHz = freq10kHz × 10  i  225 kHz = freqüència intermèdia (IF)
    uint32_t freqKhz = (uint32_t)freq10kHz * 10;
    uint16_t pll     = (uint16_t)((4.0f * (float)(freqKhz + 225)) / 32.768f);

    uint8_t b[5];

    // Byte 0: SM[6]=mode_seek | PLL[13:8]
    b[0] = (searchMode ? 0x40 : 0x00) | (uint8_t)((pll >> 8) & 0x3F);

    // Byte 1: PLL[7:0]
    b[1] = (uint8_t)(pll & 0xFF);

    // Byte 2: SUD[7]=direcció_seek | SSL[6:5]=nivell_parada | HLSI[4]=injecció_alta | MS[3]=mono
    b[2] = (searchUp    ? 0x80 : 0x00)   // SUD: 1=cerca cap amunt, 0=cap avall
         | ssl                             // SSL: nivell mínim de senyal per parar
         | 0x10                            // HLSI=1: injecció costat alt (estàndard europeu)
         | (g_forcesMono ? 0x08 : 0x00);  // MS=1: forçar mono

    // Byte 3: XTAL[4]=1 (cristall 32.768kHz) | HCC[2]=1 (tall aguts en senyal dèbil) | SNC[1]=1 (cancel·lació soroll estèreo)
    b[3] = 0x10 | 0x04 | 0x02;

    // Byte 4: no usat
    b[4] = 0x00;

    // Enviar els 5 bytes al TEA5767 per I2C (adreça 0x60)
    Wire.beginTransmission(TEA5767_I2C_ADDR);
    for (int i = 0; i < 5; i++) Wire.write(b[i]);
    Wire.endTransmission();
}

// ═════════════════════════════════════════════════════════════════════════════
// tea_seek_once — llança un seek hardware des de fromFreq en la direcció indicada
//
// El TEA5767 té un circuit intern de detecció de senyal que escaneja
// la banda FM i s'atura quan troba una emisora amb senyal ≥ TEA_SSL.
// Retorna la freqüència trobada, o 0 si va arribar al límit de banda.
// Sempre cridar amb mutex_i2c ja agafat.
// ═════════════════════════════════════════════════════════════════════════════
uint16_t tea_seek_once(bool up, uint16_t fromFreq) {

    // Iniciar el seek hardware escrivint amb el bit SM=1
    tea_write(fromFreq, true, up, TEA_SSL);

    uint8_t  resp[5] = {0};
    uint32_t t0      = millis();

    // Esperar fins que el chip indiqui que ha acabat (màxim 5 segons)
    while (millis() - t0 < 5000) {
        vTaskDelay(pdMS_TO_TICKS(50));

        // Llegir els 5 bytes d'estat del TEA5767
        Wire.requestFrom(TEA5767_I2C_ADDR, 5);
        for (int i = 0; i < 5; i++) resp[i] = Wire.available() ? Wire.read() : 0;

        bool rf  = (resp[0] >> 7) & 0x01; // RF=1: ha trobat una emisora
        bool blf = (resp[0] >> 6) & 0x01; // BLF=1: ha arribat al límit de banda sense trobar res

        if (blf) return 0; // Límit de banda: no ha trobat emisora en aquesta direcció

        if (rf) {
            // Calcular la freqüència on s'ha aturat a partir del valor PLL de resposta
            uint16_t pllFound = ((uint16_t)(resp[0] & 0x3F) << 8) | resp[1];
            uint32_t freqKhz  = (uint32_t)((float)pllFound * 32.768f / 4.0f) - 225;
            uint16_t found    = (uint16_t)(freqKhz / 10); // Convertir a unitats 10kHz
            // Assegurar que la freqüència és dins del rang vàlid
            if (found < FM_MIN) found = FM_MIN;
            if (found > FM_MAX) found = FM_MAX;
            return found;
        }
    }
    return 0; // Timeout: el chip no ha respost en 5 segons
}

// ════════════════════════════════════════════