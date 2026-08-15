// ============================================================================
//  exemple_ESP32 — espNow_SharedVariable.h
//
//  Variable partagée ESP-NOW :
//    - déclare deux variables (float + couple float/unité)
//    - demande le jeton d'écriture de la variable puissance, puis l'écrit
//      (l'horodatage UTC et l'ID du dernier écrivain sont ajoutés automatiquement)
//    - libère le jeton périodiquement (transmission de main)
//    - lit la copie locale de la variable température (écrite par un autre iOT)
//
//  Installation :
//    - Arduino IDE : copier espNow_SharedVariable.h dans le dossier du sketch
//    - PlatformIO  : voir exemples/platformio.ini (pio run -e esp32)
// ============================================================================

#define _ESP_NOW_SHARED_VARIABLE_
#include "espNow_SharedVariable.h"

// ---- IDs de variables (définis par votre projet) ----
#define _DONNEE_TEMPERATURE_   1
#define _DONNEE_PUISSANCE_     2

// ---- ID iOT (unique sur le réseau) ----
#define ID_ESP32_RADIATEUR     11

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);                        // ESP-NOW en mode station (sans AP)

  EspNowSharedVariable::Config cfg;
  cfg.id_iot = ID_ESP32_RADIATEUR;
  espNow_sharedVariable.init(cfg);            // après le démarrage du WiFi

  espNow_sharedVariable.declare_float(_DONNEE_TEMPERATURE_, 21.5);
  espNow_sharedVariable.declare_couple(_DONNEE_PUISSANCE_, 0.0, "W");

  Serial.println(F("[exemple] espNow_SharedVariable initialisé"));
}

void loop() {
  espNow_sharedVariable.loop();               // OBLIGATOIRE : réception + timers jeton

  // heure UTC fournie par votre lib NTP (ex. NTP_FR) ; horodatage auto des écritures
  uint32_t epoch_utc = 0;
  espNow_sharedVariable.regler_heure_utc(epoch_utc);

  // ---- écriture de la puissance : demande du jeton puis écriture ----
  static unsigned long dernier_ecriture = 0;
  if (millis() - dernier_ecriture >= 5000) {
    dernier_ecriture = millis();

    if (!espNow_sharedVariable.detiens_jeton(_DONNEE_PUISSANCE_)) {
      espNow_sharedVariable.demander_jeton(_DONNEE_PUISSANCE_);
      Serial.println(F("[exemple] jeton puissance demandé"));
    } else {
      // j'ai le jeton : j'écris (un seul iOT peut écrire à la fois)
      float p = 100.0f + (millis() % 50);
      if (espNow_sharedVariable.ecrire_couple(_DONNEE_PUISSANCE_, p, "W"))
        Serial.print(F("[exemple] puissance écrite : "));
      Serial.print(p);
      Serial.println(F(" W"));
    }
  }

  // ---- libération du jeton toutes les 30 s (transmission de main) ----
  static unsigned long dernier_liberation = 0;
  if (millis() - dernier_liberation >= 30000) {
    dernier_liberation = millis();
    if (espNow_sharedVariable.liberer_jeton(_DONNEE_PUISSANCE_))
      Serial.println(F("[exemple] jeton puissance libéré"));
  }

  // ---- lecture de la copie locale (écrite par un autre iOT) ----
  static unsigned long dernier_lecture = 0;
  if (millis() - dernier_lecture >= 2000) {
    dernier_lecture = millis();

    float t;
    if (espNow_sharedVariable.lire_float(_DONNEE_TEMPERATURE_, t)) {
      Serial.print(F("Température : "));
      Serial.print(t);
      Serial.print(F(" °C, epoch="));
      Serial.print(espNow_sharedVariable.lire_epoch(_DONNEE_TEMPERATURE_));
      Serial.print(F(", écrit par #"));
      Serial.println(espNow_sharedVariable.dernier_ecrivain(_DONNEE_TEMPERATURE_));
    }

    float p; char unite[8];
    if (espNow_sharedVariable.lire_couple(_DONNEE_PUISSANCE_, p, unite, sizeof(unite))) {
      Serial.print(F("Puissance : "));
      Serial.print(p);
      Serial.print(F(" "));
      Serial.print(unite);
      Serial.print(F(" (détenteur jeton : "));
      Serial.print(espNow_sharedVariable.detenteur_jeton(_DONNEE_PUISSANCE_));
      Serial.println(F(")"));
    }
  }
}
