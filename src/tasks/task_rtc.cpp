#include <Arduino.h>
#include <Wire.h>
#include "globals.h"
#include "app_tasks.h"

// ═════════════════════════════════════════════════════════════════════════════
// rtc_task — Prioritat 3
// Llegeix el DS3231 cada segon i actualitza g_now i g_isStereo.
// Protegeix l'accés I2C amb mutex_i2c per evitar col·lisions amb alarm_task.
// ═════════════════════════════════════════════════════════════════════════════
void task_rtc(void *pv) {
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
