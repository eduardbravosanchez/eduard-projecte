#include <Arduino.h>
#include <unity.h>
#include <Wire.h>
#include "globals.h"

// Unity requereix aquestes dues funcions per configuració inicial/neteja
void setUp(void) {
    // Es crida abans de cada test
}

void tearDown(void) {
    // Es crida després de cada test
}

// ── TEST 1: Comprovació de maquinari I2C (TEA5767) ──────────────────────────
void test_tea5767_is_connected(void) {
    Wire.begin(PIN_SDA, PIN_SCL);
    
    // Iniciem transmissió cap a la direcció del xip de ràdio
    Wire.beginTransmission(TEA5767_I2C_ADDR);
    uint8_t error = Wire.endTransmission();
    
    // Wire.endTransmission() retorna 0 si el dispositiu respon correctament
    TEST_ASSERT_EQUAL_MESSAGE(0, error, "ERROR I2C: El TEA5767 no respon. Revisa els cables SDA/SCL i alimentacio.");
}

// ── TEST 2: Comprovació d'estat de variables ──────────────────────────────
void test_default_frequency_is_valid(void) {
    // Verifiquem que la freqüència per defecte estigui dins el rang FM vàlid
    TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(FM_MIN, FM_DEFAULT, "Freq inicial massa baixa");
    TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(FM_MAX, FM_DEFAULT, "Freq inicial massa alta");
}

void setup() {
    // Petita pausa perquè el port sèrie tingui temps d'iniciar-se
    delay(2000); 
    
    UNITY_BEGIN();
    RUN_TEST(test_tea5767_is_connected);
    RUN_TEST(test_default_frequency_is_valid);
    UNITY_END();
}

void loop() {
    // Un cop acaben els tests, es queda aturat
    delay(1000);
}
