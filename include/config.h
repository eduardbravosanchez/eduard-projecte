/**
 * @file config.h
 * @brief Configuració global, mapeig de pins i paràmetres del sistema.
 * @project D.05 — Despertador amb Ràdio FM Real
 * @author Eduard Bravo Sánchez · Processadors Digitals · UPC
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ─────────────────────────────────────────────────────────────────────────────
// 1. CONFIGURACIÓ DE HARDWARE (PINOUT ESP32-S3)
// ─────────────────────────────────────────────────────────────────────────────

// Bus I2C (Compartit per TEA5767 + DS3231)
#define PIN_SDA             8    ///< Línia de dades I2C
#define PIN_SCL             9    ///< Línia de rellotge I2C

// Bus SPI (Display TFT ILI9341)
#define PIN_TFT_MOSI       11    ///< Dades SPI cap al TFT
#define PIN_TFT_CLK        12    ///< Rellotge SPI
#define PIN_TFT_CS         10    ///< Chip Select del TFT
#define PIN_TFT_DC         13    ///< Data/Command del TFT
#define PIN_TFT_RST        14    ///< Reset del TFT

// Polsadors (Entrades de control)
#define PIN_BTN_FM_UP       1    ///< Polsador: buscar emisora cap amunt
#define PIN_BTN_FM_DOWN     2    ///< Polsador: buscar emisora cap avall
#define PIN_BTN_SET         3    ///< Polsador: ajust alarma (curt) / hora (llarg)
#define PIN_BTN_SNOOZE      4    ///< Polsador: snooze / apagar alarma
#define PIN_BTN_MONO_ST     5    ///< Polsador: alternar mono / estèreo

// ─────────────────────────────────────────────────────────────────────────────
// 2. PARÀMETRES DEL SINTONITZADOR FM (TEA5767)
// ─────────────────────────────────────────────────────────────────────────────
#define FM_MIN           8750    ///< Freqüència mínima FM (87.50 MHz) en unitats de 10 kHz
#define FM_MAX          10800    ///< Freqüència màxima FM (108.00 MHz) en unitats de 10 kHz
#define FM_DEFAULT       9360    ///< Freqüència per defecte (93.60 MHz)

/**
 * Nivell de parada del seek hardware (bits SSL al registre 3):
 * 0x20 = nivell 5  (emisores dèbils)
 * 0x40 = nivell 7  (equilibri senyal/soroll recomanat)
 * 0x60 = nivell 10 (només emisores molt fortes)
 */
#define TEA_SSL          0x40
#define TEA5767_I2C_ADDR 0x60    ///< Adreça I2C estàndard del chip de ràdio

// ─────────────────────────────────────────────────────────────────────────────
// 3. PARÀMETRES DE L'ALARMA I TEMPORITZACIONS
// ─────────────────────────────────────────────────────────────────────────────
#define SNOOZE_MINUTES      5    ///< Minuts que espera el snooze abans de tornar a sonar
#define DEBOUNCE_MS        50    ///< Temps de filtratge de rebot mecànic dels polsadors
#define ALARM_HOUR_DEF      7    ///< Hora per defecte de l'alarma (primer arrencada)
#define ALARM_MIN_DEF       0    ///< Minut per defecte de l'alarma

// ─────────────────────────────────────────────────────────────────────────────
// 4. ARQUITECTURA DE SOFTWARE (FreeRTOS)
// ─────────────────────────────────────────────────────────────────────────────
#define TASK_RUNNING_CORE   1    ///< Nucli de l'ESP32 assignat a l'aplicació d'usuari

// Tasca RTC (Lectura de l'hora)
#define TASK_RTC_PRIO       3    ///< Prioritat mitjana
#define TASK_RTC_STACK   2048    ///< Mida de la pila en bytes

// Tasca ALARMA (Control de seguretat crític)
#define TASK_ALARM_PRIO     4    ///< Prioritat màxima (crítica per al temps real)
#define TASK_ALARM_STACK 2048    ///< Mida de la pila en bytes

// Tasca UI (Interfície de l'usuari i Pantalla)
#define TASK_UI_PRIO        2    ///< Prioritat baixa (tolerant a petits retards)
#define TASK_UI_STACK    4096    ///< Mida de la pila major per moure els gràfics del TFT

// ─────────────────────────────────────────────────────────────────────────────
// 5. ESPACIS DE MEMÒRIA PERSISTENT (NVS Preferences)
// ─────────────────────────────────────────────────────────────────────────────
#define NVS_NAMESPACE "despertador" ///< Espai de noms en la Flash NVS
#define NVS_KEY_ALARM_H   "alh"     ///< Clau per a l'hora de l'alarma
#define NVS_KEY_ALARM_M   "alm"     ///< Clau per al minut de l'alarma
#define NVS_KEY_ALARM_E   "ale"     ///< Clau per a l'estat de l'alarma (on/off)
#define NVS_KEY_FREQ      "freq"    ///< Clau per a l'última freqüència guardada
#define NVS_KEY_MONO      "mono"    ///< Clau per al mode mono forçat

#endif // CONFIG_H