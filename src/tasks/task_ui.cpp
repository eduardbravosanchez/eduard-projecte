#include <Arduino.h>
#include "globals.h"
#include "app_tasks.h"

// ═════════════════════════════════════════════════════════════════════════════
// ui_task — Prioritat 2
// Gestiona tots els polsadors i actualitza el display TFT.
// És la tasca de menor prioritat perquè un retard de ms en la pantalla
// és imperceptible, mentre que l'àudio i l'alarma han de ser puntuals.
// ═════════════════════════════════════════════════════════════════════════════
void task_ui(void *pv) {

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
        if (digitalRead(PIN_BTN_SET) == LOW) {
            vTaskDelay(pdMS_TO_TICKS(50)); // Antirebote inicial
            if (digitalRead(PIN_BTN_SET) == LOW) {
                uint32_t t0 = millis();
                while (digitalRead(PIN_BTN_SET) == LOW) vTaskDelay(pdMS_TO_TICKS(10));
                uint32_t held = millis() - t0;

                if (held >= 3000 && setMode == SET_NONE) {
                    setMode = SET_CLOCK_HOUR;
                    tmpClockHour = g_now.hour();
                    tmpClockMin  = g_now.minute();
                    tft.fillScreen(ILI9341_BLACK);
                    tft.setTextColor(ILI9341_MAGENTA); tft.setTextSize(2);
                    tft.setCursor(10, 10);  tft.print("Ajustar hora rellotge");
                    tft.setCursor(10, 40);  tft.print("FM+/FM- canvia hora");
                    tft.setCursor(10, 65);  tft.printf("Hora: %02d", tmpClockHour);
                    tft.setTextColor(ILI9341_DARKGREY);
                    tft.setCursor(10, 95);  tft.print("SET = seguent");

                } else if (held < 3000) {
                    if (setMode == SET_NONE) {
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
                        setMode = SET_MIN;
                        tft.fillScreen(ILI9341_BLACK);
                        tft.setTextColor(ILI9341_CYAN); tft.setTextSize(2);
                        tft.setCursor(10, 10);  tft.print("Ajustar alarma");
                        tft.setCursor(10, 40);  tft.print("FM+/FM- canvia min");
                        tft.setCursor(10, 65);  tft.printf("Min: %02d", tmpMin);
                        tft.setTextColor(ILI9341_DARKGREY);
                        tft.setCursor(10, 95);  tft.print("SET = guardar");

                    } else if (setMode == SET_MIN) {
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
                        vTaskDelay(pdMS_TO_TICKS(1500));
                        tft.fillScreen(ILI9341_BLACK);
                        lastSec = 99;
                        lastStereoState = !g_isStereo;
                        Serial.printf("[SET] Alarma guardada %02d:%02d\n", g_alarm.hour, g_alarm.minute);

                    } else if (setMode == SET_CLOCK_HOUR) {
                        setMode = SET_CLOCK_MIN;
                        tft.fillScreen(ILI9341_BLACK);
                        tft.setTextColor(ILI9341_MAGENTA); tft.setTextSize(2);
                        tft.setCursor(10, 10);  tft.print("Ajustar hora rellotge");
                        tft.setCursor(10, 40);  tft.print("FM+/FM- canvia min");
                        tft.setCursor(10, 65);  tft.printf("Min: %02d", tmpClockMin);
                        tft.setTextColor(ILI9341_DARKGREY);
                        tft.setCursor(10, 95);  tft.print("SET = guardar");

                    } else if (setMode == SET_CLOCK_MIN) {
                        if (xSemaphoreTake(mutex_i2c, pdMS_TO_TICKS(200)) == pdTRUE) {
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
                g_radioMuted = !g_radioMuted;
                if (xSemaphoreTake(mutex_i2c, pdMS_TO_TICKS(100)) == pdTRUE) {
                    radio.setMute(g_radioMuted);
                    xSemaphoreGive(mutex_i2c);
                }
                Serial.printf("[RADIO] %s\n", g_radioMuted ? "MUTE" : "ON");
                drawFreq();
            }
        }

        // ── BTN_MONO_ST ───────────────────────────────────────────────────────────
        if (readBtn(PIN_BTN_MONO_ST) && setMode == SET_NONE) {
            g_forcesMono = !g_forcesMono;
            applyMonoStereo();

            prefs.begin(NVS_NAMESPACE, false);
            prefs.putBool(NVS_KEY_MONO, g_forcesMono);
            prefs.end();

            Serial.printf("[AUDIO] Mode: %s\n", g_forcesMono ? "MONO forçat" : "STEREO auto");
            lastStereoState = !g_isStereo;
        }

        // ── Redibuixat de la pantalla principal ───────────────────────────────
        if (setMode == SET_NONE) {
            uint8_t s = g_now.second();
            if (s != lastSec) {
                lastSec = s;
                drawScreen();
                lastStereoState = g_isStereo;
            } else if (g_isStereo != lastStereoState) {
                lastStereoState = g_isStereo;
                drawMonoStereo();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(80));
    }
}
