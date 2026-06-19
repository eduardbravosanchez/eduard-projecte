/**
 * @file globals.h
 * @brief Declaració de variables globals, estructures i objectes de hardware compartits.
 * @project D.05 — Despertador amb Ràdio FM Real
 * @author Eduard Bravo Sánchez · Processadors Digitals · UPC
 */

#ifndef GLOBALS_H
#define GLOBALS_H

#include <Arduino.h>
#include <RTClib.h>
#include <TEA5767.h>
#include <Adafruit_ILI9341.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include "config.h"

// ── Tipus de dades ────────────────────────────────────────────────────────────
struct AlarmConfig {
    uint8_t hour;
    uint8_t minute;
    bool    enabled;
};

enum AlarmEvent { EVT_NONE, EVT_FIRE, EVT_SNOOZE, EVT_OFF };

// ── Objectes globals (declaració externa per a enllaçat) ──────────────────────
extern RTC_DS3231       rtc;
extern TEA5767          radio;
extern Adafruit_ILI9341 tft;
extern Preferences      prefs;

// ── Variables compartides (declaració externa) ────────────────────────────────
extern volatile uint16_t g_fmFreq;
extern volatile bool     g_alarmFired;
extern volatile bool     g_snoozed;
extern volatile bool     g_forcesMono;
extern volatile bool     g_isStereo;
extern volatile bool     g_radioMuted;
extern AlarmConfig       g_alarm;
extern DateTime          g_now;

// ── Primitives FreeRTOS (declaració externa) ──────────────────────────────────
extern SemaphoreHandle_t mutex_i2c;
extern SemaphoreHandle_t sem_alarm;
extern QueueHandle_t     q_alarm_event;

// ── Prototips de funcions (UI i Drivers que es desglossaran després) ──────────
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

#endif // GLOBALS_H
