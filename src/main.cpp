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

// ── Pines ─────────────────────────────────────────────────────────────────────
#define PIN_SDA          8    // Línia de dades I2C (TEA5767 + DS3231 compartits)
#define PIN_SCL          9    // Línia de rellotge I2C
#define PIN_TFT_MOSI    11    // Dades SPI cap al TFT
#define PIN_TFT_CLK     12    // Rellotge SPI
#define PIN_TFT_CS      10    // Chip Select del TFT
#define PIN_TFT_DC      13    // Data/Command del TFT
#define PIN_TFT_RST     14    // Reset del TFT
#define PIN_BTN_FM_UP    1    // Polsador: buscar emisora cap amunt
#define PIN_BTN_FM_DOWN  2    // Polsador: buscar emisora cap avall
#define PIN_BTN_SET      3    // Polsador: ajust alarma (curt) / ajust hora (llarg)
#define PIN_BTN_SNOOZE   4    // Polsador: snooze / apagar alarma
#define PIN_BTN_MONO_ST  5    // Polsador: alternar mono / estèreo

// ── Freqüències en unitats de 10 kHz ─────────────────────────────────────────
// El TEA5767 treballa internament en kHz. Per evitar floats usem x10:
// 8750 = 87.50 MHz, 10800 = 108.00 MHz, 9360 = 93.60 MHz
#define FM_MIN          8750
#define FM_MAX         10800
#define FM_DEFAULT      9360

// Nivell de parada del seek hardware del TEA5767 (bits SSL al registre 3):
//   0x20 = nivell 5  (troba emisores dèbils)
//   0x40 = nivell 7  (recomanat, equilibri senyal/soroll)
//   0x60 = nivell 10 (només emisores molt fortes)
#define TEA_SSL  0x40

// ── Alarma ────────────────────────────────────────────────────────────────────
#define SNOOZE_MINUTES     5   // Minuts que espera el snooze abans de tornar a sonar
#define DEBOUNCE_MS       50   // Temps de filtratge de rebot mecànic dels polsadors (ms)
#define ALARM_HOUR_DEF     7   // Hora per defecte de l'alarma en el primer arrencada
#define ALARM_MIN_DEF      0   // Minut per defecte de l'alarma

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
volatile bool      g_alarmFired  = false;       // true quan l'alarma s'ha disparat
volatile bool      g_snoozed     = false;       // true si s'ha activat el snooze
volatile bool      g_forcesMono  = false;       // true si l'usuari ha forçat mode mono
volatile bool      g_isStereo    = false;       // true si el chip detecta senyal estèreo
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

    // ── 1. Crear primitives FreeRTOS ABANS de crear tasques ───────────────────
    // Si es creessin les tasques primer, podrien intentar usar mutex_i2c = NULL
    mutex_i2c     = xSemaphoreCreateMutex();           // Protegeix el bus I2C
    sem_alarm     = xSemaphoreCreateBinary();           // Senyal d'alarma disparada
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
    prefs.begin("despertador", false);
    g_alarm.hour    = prefs.getUChar ("alh",  ALARM_HOUR_DEF); // Hora alarma
    g_alarm.minute  = prefs.getUChar ("alm",  ALARM_MIN_DEF);  // Minut alarma
    g_alarm.enabled = prefs.getBool  ("ale",  false);           // Alarma activa?
    g_fmFreq        = prefs.getUShort("freq", FM_DEFAULT);      // Última freqüència
    g_forcesMono    = prefs.getBool  ("mono", false);           // Mode mono forçat?
    prefs.end();

    // Aplicar la freqüència i el mode mono/estèreo carregats de NVS
    applyFmFreq();
    applyMonoStereo();

    // ── 8. Crear les tasques FreeRTOS ─────────────────────────────────────────
    // Cada tasca té la seva pròpia pila i prioritat. Les tasques d'alta prioritat
    // interrompen les de baixa quan necessiten CPU.
    // Paràmetres: funció, nom, mida pila (bytes), paràmetre, prioritat, handle, nucli
    xTaskCreatePinnedToCore(rtc_task,   "rtc",   2048, NULL, 3, NULL, 1); // Prio 3: llegeix hora
    xTaskCreatePinnedToCore(alarm_task, "alarm", 2048, NULL, 4, NULL, 1); // Prio 4: comprova alarma
    xTaskCreatePinnedToCore(ui_task,    "ui",    4096, NULL, 2, NULL, 1); // Prio 2: pantalla + polsadors

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
                Wire.requestFrom(0x60, 5); // Sol·licitar 5 bytes al TEA5767 (adreça 0x60)
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

        // ── BTN_SNOOZE — només actua quan l'alarma està sonant ───────────────
        if (readBtn(PIN_BTN_SNOOZE) && g_alarmFired) {
            if (!g_snoozed) {
                // Primer pols: activar snooze — silenciar i reprogramar 5 minuts més tard
                g_snoozed    = true;
                g_alarmFired = false;
                if (xSemaphoreTake(mutex_i2c, pdMS_TO_TICKS(100)) == pdTRUE) {
                    radio.setMute(true); // Silenciar la ràdio
                    xSemaphoreGive(mutex_i2c);
                }
                // Calcular nova hora d'alarma (amb gestió del pas de hora)
                uint8_t newMin  = (g_alarm.minute + SNOOZE_MINUTES) % 60;
                uint8_t newHour = g_alarm.hour;
                if (g_alarm.minute + SNOOZE_MINUTES >= 60) newHour = (newHour + 1) % 24;
                g_alarm.minute = newMin;
                g_alarm.hour   = newHour;
                AlarmEvent ev = EVT_SNOOZE;
                xQueueSend(q_alarm_event, &ev, 0);
                Serial.printf("[SNOOZE] Reactivant a %02d:%02d\n", newHour, newMin);
            } else {
                // Segon pols: apagar l'alarma definitivament i restaurar hora original
                g_alarmFired   = false;
                g_snoozed      = false;
                g_alarm.hour   = tmpHour;   // Recuperar hora original (abans del snooze)
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

        // ── BTN_MONO_ST — alternar entre mono forçat i estèreo automàtic ─────
        if (readBtn(PIN_BTN_MONO_ST) && setMode == SET_NONE) {
            g_forcesMono = !g_forcesMono; // Invertir l'estat actual
            applyMonoStereo();             // Aplicar el canvi al chip TEA5767

            // Guardar la preferència a NVS perquè es recordi en reinicis
            prefs.begin("despertador", false);
            prefs.putBool("mono", g_forcesMono);
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
    Wire.beginTransmission(0x60);
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
        Wire.requestFrom(0x60, 5);
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

// ═════════════════════════════════════════════════════════════════════════════
// seekAndUpdate — cerca la propera emisora i actualitza la pantalla
//
// Garanteix que SEMPRE es canvia a una freqüència diferent a l'actual:
//  1. Comença un pas més enllà de la freqüència actual
//  2. Si el chip retorna la mateixa freqüència, salta un pas i reintenta
//  3. Si arriba al límit de banda, intenta des de l'extrem contrari (wrap)
//  4. Màxim 3 intents abans de rendir-se
// ═════════════════════════════════════════════════════════════════════════════
void seekAndUpdate(bool up) {
    drawSeeking(); // Mostrar "Buscant emisora..." mentre dura la cerca

    uint16_t original = g_fmFreq; // Guardar la freqüència actual per comparar
    uint16_t result   = original;

    // Agafar el mutex I2C amb timeout generós (el seek pot trigar fins a 5s)
    if (xSemaphoreTake(mutex_i2c, pdMS_TO_TICKS(15000)) == pdTRUE) {

        uint16_t startFreq = original;
        int maxAttempts    = 3;

        for (int attempt = 0; attempt < maxAttempts; attempt++) {
            // Calcular el punt de partida: sempre un pas més enllà de l'actual
            uint16_t from = startFreq;
            if (up) {
                from += 1;
                if (from > FM_MAX) from = FM_MIN; // Wrap al principi de la banda
            } else {
                from -= 1;
                if (from < FM_MIN) from = FM_MAX; // Wrap al final de la banda
            }

            Serial.printf("[SEEK] Intent %d des de %d.%d MHz\n",
                attempt + 1, from / 100, (from % 100) / 10);

            uint16_t found = tea_seek_once(up, from);

            if (found == 0) {
                // Ha arribat al límit de banda → intentar des de l'extrem contrari
                Serial.println("[SEEK] Límit de banda, wrap");
                startFreq = up ? FM_MIN : FM_MAX;
                continue;
            }

            if (found != original) {
                // Ha trobat una freqüència diferent → èxit
                result = found;
                Serial.printf("[SEEK] OK: %d.%d MHz\n", result / 100, (result % 100) / 10);
                break;
            }

            // Ha retornat la mateixa freqüència (problema de rodoneig del PLL)
            // → saltar un pas i reintentar
            Serial.println("[SEEK] Mateixa freqüència, saltant");
            startFreq = found;
        }

        // Sintonitzar en mode normal (SM=0) a la freqüència final trobada
        tea_write(result, false, false, 0);
        vTaskDelay(pdMS_TO_TICKS(150)); // Esperar estabilització del chip
        g_fmFreq = result;
        xSemaphoreGive(mutex_i2c);
    }

    // Guardar la nova freqüència a NVS per recordar-la en el proper reinici
    prefs.begin("despertador", false);
    prefs.putUShort("freq", g_fmFreq);
    prefs.end();

    drawFreq(); // Actualitzar la freqüència a la pantalla
}

// ═════════════════════════════════════════════════════════════════════════════
// applyFmFreq — sintonitza g_fmFreq en mode normal (sense seek)
// S'usa en l'arrencada per aplicar la freqüència guardada a NVS
// ═════════════════════════════════════════════════════════════════════════════
void applyFmFreq() {
    if (xSemaphoreTake(mutex_i2c, pdMS_TO_TICKS(200)) == pdTRUE) {
        tea_write(g_fmFreq, false, false, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        xSemaphoreGive(mutex_i2c);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// applyMonoStereo — aplica el mode mono/estèreo al chip sense canviar freqüència
// Reescriu els registres del TEA5767 amb el bit MS actualitzat
// ═════════════════════════════════════════════════════════════════════════════
void applyMonoStereo() {
    if (xSemaphoreTake(mutex_i2c, pdMS_TO_TICKS(200)) == pdTRUE) {
        tea_write(g_fmFreq, false, false, 0); // tea_write ja inclou el bit MS
        vTaskDelay(pdMS_TO_TICKS(80));
        xSemaphoreGive(mutex_i2c);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Funcions de pantalla
//
// Layout del TFT (320×240 píxels, orientació landscape):
//   Y   0- 84  → Hora gran (HH:MM) + segons petits
//   Y  90-124  → Freqüència FM actual
//   Y 130-189  → Estat de l'alarma
//   Y 195-240  → Indicador mono/estèreo
// ═════════════════════════════════════════════════════════════════════════════

// Redibuixa tota la pantalla principal (s'invoca cada segon)
void drawScreen() {
    // ── Hora gran ──────────────────────────────────────────────────────────────
    tft.fillRect(0, 0, 320, 85, ILI9341_BLACK); // Esborrar zona de l'hora
    tft.setTextColor(ILI9341_WHITE); tft.setTextSize(5); // Text gran blanc
    tft.setCursor(30, 15);
    tft.printf("%02d:%02d", g_now.hour(), g_now.minute()); // HH:MM

    // Segons petits en gris (confirma que el RTC està actualitzant-se)
    tft.setTextColor(ILI9341_DARKGREY); tft.setTextSize(2);
    tft.setCursor(250, 50);
    tft.printf(":%02d", g_now.second());

    drawFreq();
    drawAlarmStatus();
    drawMonoStereo();
}

// Dibuixa la freqüència FM actual (zona Y 90-124)
void drawFreq() {
    tft.fillRect(0, 90, 320, 35, ILI9341_BLACK);
    tft.setTextColor(ILI9341_CYAN); tft.setTextSize(2);
    tft.setCursor(10, 100);
    // Convertir de unitats 10kHz a format "XX.X MHz"
    // Exemple: 9360 → 93.6 MHz → "93.6 MHz"
    tft.printf("FM  %d.%d MHz", g_fmFreq / 100, (g_fmFreq % 100) / 10);
}

// Dibuixa "Buscant emisora..." mentre es fa el seek (zona Y 90-124)
void drawSeeking() {
    tft.fillRect(0, 90, 320, 35, ILI9341_BLACK);
    tft.setTextColor(ILI9341_YELLOW); tft.setTextSize(2);
    tft.setCursor(10, 100);
    tft.print("Buscant emisora...");
}

// Dibuixa l'estat de l'alarma (zona Y 130-189)
void drawAlarmStatus() {
    tft.fillRect(0, 130, 320, 60, ILI9341_BLACK);
    tft.setTextSize(2);
    if (g_alarmFired) {
        // L'alarma està sonant: text vermell parpellejant
        tft.setTextColor(ILI9341_RED);
        tft.setCursor(10, 138); tft.print("! ALARMA SONANT !");
        tft.setCursor(10, 160); tft.print("Snooze o apagar");
    } else if (g_alarm.enabled) {
        // Alarma programada i activa: mostrar hora en verd
        tft.setTextColor(ILI9341_GREEN);
        tft.setCursor(10, 138);
        tft.printf("Alarma: %02d:%02d  ON", g_alarm.hour, g_alarm.minute);
        if (g_snoozed) {
            // El snooze està actiu: mostrar avís en taronja
            tft.setTextColor(ILI9341_ORANGE);
            tft.setCursor(10, 160); tft.print("(snooze actiu)");
        }
    } else {
        // Alarma desactivada
        tft.setTextColor(ILI9341_DARKGREY);
        tft.setCursor(10, 138); tft.print("Alarma: OFF");
        tft.setCursor(10, 160); tft.print("SET per programar");
    }
}

// Dibuixa l'indicador mono/estèreo (zona Y 195-240)
// Tres estats possibles segons g_forcesMono i g_isStereo:
//   MONO (forçat) — l'usuari ha forçat mono manualment → vermell
//   STEREO        — l'emisora emet en estèreo i el chip ho detecta → groc
//   MONO (emisora)— mode automàtic però l'emisora no emet estèreo → vermell
void drawMonoStereo() {
    tft.fillRect(0, 195, 320, 45, ILI9341_BLACK);
    tft.setTextSize(2);

    if (g_forcesMono) {
        // L'usuari ha forçat mono: indicador vermell + instrucció per tornar a estèreo
        tft.setTextColor(ILI9341_RED);
        tft.setCursor(10, 205);
        tft.print("MONO  (forcat)");
        tft.setTextColor(ILI9341_YELLOW);
        tft.setCursor(10, 222);
        tft.print("Prem MONO/ST p/ stereo");
    } else if (g_isStereo) {
        // Estèreo actiu: indicador groc + instrucció per forçar mono
        tft.setTextColor(ILI9341_YELLOW);
        tft.setCursor(10, 205);
        tft.print("STEREO");
        tft.setTextColor(ILI9341_YELLOW);
        tft.setCursor(10, 222);
        tft.print("Prem MONO/ST p/ mono");
    } else {
        // Mode automàtic però l'emisora no dona estèreo
        tft.setTextColor(ILI9341_RED);
        tft.setCursor(10, 205);
        tft.print("MONO  (emisora)");
        tft.setTextColor(ILI9341_YELLOW);
        tft.setCursor(10, 222);
        tft.print("Prem MONO/ST p/ forcar");
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// readBtn — lectura de polsador amb antirebote per software
//
// Retorna true si el polsador ha estat premut i alliberat.
// El bucle while espera a que es deixi anar per evitar deteccions múltiples.
// ═════════════════════════════════════════════════════════════════════════════
bool readBtn(uint8_t pin) {
    if (digitalRead(pin) == LOW) {                       // Pin a GND = polsador premut
        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));          // Esperar estabilització mecànica
        if (digitalRead(pin) == LOW) {                   // Confirmar que segueix premut
            while (digitalRead(pin) == LOW)              // Esperar a que es deixi anar
                vTaskDelay(pdMS_TO_TICKS(10));
            return true;                                 // Polsació vàlida detectada
        }
    }
    return false; // No s'ha detectat cap polsació
}