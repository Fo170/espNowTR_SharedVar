// ============================================================================
//  exemple_ESP32 — espNowTR_SharedVar.h
//
//  Variable partagée ESP-NOW :
//    - déclare deux variables (float + couple float/unité)
//    - demande le jeton d'écriture de la variable puissance, puis l'écrit
//      (l'horodatage UTC et l'ID du dernier écrivain sont ajoutés automatiquement)
//    - libère le jeton périodiquement (transmission de main)
//    - lit la copie locale de la variable température (écrite par un autre iOT)
//
//  Installation :
//    - Arduino IDE : copier espNowTR_SharedVar.h dans le dossier du sketch
//    - PlatformIO  : voir exemples/platformio.ini (pio run -e esp32)
// ============================================================================

#define _ESP_NOW_TR_SHARED_VAR_
#include "espNowTR_SharedVar.h"

// ---- IDs de variables (définis par votre projet) ----
#define _DONNEE_TEMPERATURE_   1
#define _DONNEE_PUISSANCE_     2

// ---- ID iOT (unique sur le réseau) ----
#define ID_ESP32_RADIATEUR     11

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);                        // ESP-NOW en mode station (sans AP)

  espNowTR_SharedVar::Config cfg;
  cfg.id_iot = ID_ESP32_RADIATEUR;
  espNowTR_sharedVar.init(cfg);            // après le démarrage du WiFi

  espNowTR_sharedVar.declare_float(_DONNEE_TEMPERATURE_, 21.5);
  espNowTR_sharedVar.declare_couple(_DONNEE_PUISSANCE_, 0.0, "W");

  Serial.println(F("[exemple] espNowTR_SharedVar initialisé"));
}

void loop() {
  espNowTR_sharedVar.loop();               // OBLIGATOIRE : réception + timers jeton

  // heure UTC fournie par votre lib NTP (ex. NTP_FR) ; horodatage auto des écritures
  uint32_t epoch_utc = 0;
  espNowTR_sharedVar.regler_heure_utc(epoch_utc);

  // ---- écriture de la puissance : demande du jeton puis écriture ----
  static unsigned long dernier_ecriture = 0;
  if (millis() - dernier_ecriture >= 5000) {
    dernier_ecriture = millis();

    if (!espNowTR_sharedVar.detiens_jeton(_DONNEE_PUISSANCE_)) {
      espNowTR_sharedVar.demander_jeton(_DONNEE_PUISSANCE_);
      Serial.println(F("[exemple] jeton puissance demandé"));
    } else {
      // j'ai le jeton : j'écris (un seul iOT peut écrire à la fois)
      float p = 100.0f + (millis() % 50);
      if (espNowTR_sharedVar.ecrire_couple(_DONNEE_PUISSANCE_, p, "W"))
        Serial.print(F("[exemple] puissance écrite : "));
      Serial.print(p);
      Serial.println(F(" W"));
    }
  }

  // ---- libération du jeton toutes les 30 s (transmission de main) ----
  static unsigned long dernier_liberation = 0;
  if (millis() - dernier_liberation >= 30000) {
    dernier_liberation = millis();
    if (espNowTR_sharedVar.liberer_jeton(_DONNEE_PUISSANCE_))
      Serial.println(F("[exemple] jeton puissance libéré"));
  }

  // ---- lecture de la copie locale (écrite par un autre iOT) ----
  static unsigned long dernier_lecture = 0;
  if (millis() - dernier_lecture >= 2000) {
    dernier_lecture = millis();

    float t;
    if (espNowTR_sharedVar.lire_float(_DONNEE_TEMPERATURE_, t)) {
      Serial.print(F("Température : "));
      Serial.print(t);
      Serial.print(F(" °C, epoch="));
      Serial.print(espNowTR_sharedVar.lire_epoch(_DONNEE_TEMPERATURE_));
      Serial.print(F(", écrit par #"));
      Serial.println(espNowTR_sharedVar.dernier_ecrivain(_DONNEE_TEMPERATURE_));
    }

    float p; char unite[8];
    if (espNowTR_sharedVar.lire_couple(_DONNEE_PUISSANCE_, p, unite, sizeof(unite))) {
      Serial.print(F("Puissance : "));
      Serial.print(p);
      Serial.print(F(" "));
      Serial.print(unite);
      Serial.print(F(" (détenteur jeton : "));
      Serial.print(espNowTR_sharedVar.detenteur_jeton(_DONNEE_PUISSANCE_));
      Serial.println(F(")"));
    }
  }
}
