#include <Arduino.h>
#include "globals.h"

// ═════════════════════════════════════════════════════════════════════════════
// LÒGICA D'INTERFÍCIE VISUAL (UI) I BOTONS
// ═════════════════════════════════════════════════════════════════════════════

void drawScreen() {
    tft.fillRect(10, 10, 220, 45, ILI9341_BLACK);
    tft.setTextColor(ILI9341_WHITE);
    tft.setTextSize(3);
    tft.setCursor(10, 10);
    tft.printf("%02d:%02d:%02d", g_now.hour(), g_now.minute(), g_now.second());

    tft.setTextSize(1);
    tft.setTextColor(ILI9341_GREEN);
    tft.setCursor(10, 42);
    tft.printf("%02d/%02d/%04d", g_now.day(), g_now.month(), g_now.year());

    drawFreq();
    drawAlarmStatus();
    drawMonoStereo();
}

void drawFreq() {
    tft.fillRect(10, 140, 300, 50, ILI9341_BLACK);
    tft.setTextSize(4);
    if (g_radioMuted) {
        tft.setTextColor(ILI9341_DARKGREY);
    } else {
        tft.setTextColor(ILI9341_ORANGE);
    }
    tft.setCursor(10, 140);
    tft.printf("%3d.%02d", g_fmFreq / 100, g_fmFreq % 100);
    
    tft.setTextSize(2);
    tft.setCursor(200, 155);
    tft.print("MHz");

    if (g_radioMuted) {
        tft.setTextSize(1);
        tft.setTextColor(ILI9341_RED);
        tft.setCursor(250, 160);
        tft.print("[MUTED]");
    }
}

void drawSeeking() {
    tft.fillRect(240, 145, 75, 20, ILI9341_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(ILI9341_YELLOW);
    tft.setCursor(240, 150);
    tft.print("SEARCHING");
}

void drawAlarmStatus() {
    tft.fillRect(10, 210, 300, 20, ILI9341_BLACK);
    tft.setTextSize(2);
    if (g_alarm.enabled) {
        if (g_alarmFired) {
            tft.setTextColor(ILI9341_RED);
            tft.setCursor(10, 210);
            tft.print("¡ALARMA SONANT!");
        } else if (g_snoozed) {
            tft.setTextColor(ILI9341_YELLOW);
            tft.setCursor(10, 210);
            tft.printf("Snooze... %02d:%02d", g_alarm.hour, g_alarm.minute);
        } else {
            tft.setTextColor(ILI9341_GREEN);
            tft.setCursor(10, 210);
            tft.printf("Alarma ON  %02d:%02d", g_alarm.hour, g_alarm.minute);
        }
    } else {
        tft.setTextColor(ILI9341_DARKGREY);
        tft.setCursor(10, 210);
        tft.print("Alarma OFF");
    }
}

void drawMonoStereo() {
    tft.fillRect(240, 10, 75, 25, ILI9341_BLACK);
    tft.setTextSize(1);
    if (g_forcesMono) {
        tft.setTextColor(ILI9341_BLUE);
        tft.setCursor(240, 15);
        tft.print("M_FORCE");
    } else {
        if (g_isStereo) {
            tft.setTextColor(ILI9341_CYAN);
            tft.setCursor(240, 15);
            tft.print("STEREO");
        } else {
            tft.setTextColor(ILI9341_LIGHTGREY);
            tft.setCursor(240, 15);
            tft.print("MONO");
        }
    }
}

bool readBtn(uint8_t pin) {
    if (digitalRead(pin) == LOW) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (digitalRead(pin) == LOW) {
            while (digitalRead(pin) == LOW) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            return true;
        }
    }
    return false;
}
