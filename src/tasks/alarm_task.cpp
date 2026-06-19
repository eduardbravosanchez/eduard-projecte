#include <Arduino.h>
#include "globals.h"
#include "app_tasks.h"

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
