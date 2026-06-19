#include <Arduino.h>
#include <Wire.h>
#include "globals.h"

// ═════════════════════════════════════════════════════════════════════════════
// LOGICA DE DRIVERS INTERNS (TEA5767)
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
