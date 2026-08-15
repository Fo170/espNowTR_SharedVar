#ifdef _ESP_NOW_SHARED_VARIABLE_

// ============================================================================
//  espNow_SharedVariable.h
//  Variable partagée ESP-NOW pour ESP8266 et ESP32 — v1.0.0
//
//  Rends une variable logique virtuellement accessible entre plusieurs iOT :
//  chaque appareil déclare la variable (ID + type) et en garde une copie
//  locale à jour, propagée en broadcast par un DÉTENTEUR DE JETON (un seul
//  écrivain à la fois, par variable).
//
//  Chaque variable porte :
//    - un ID unique (uint16_t, 0 = non affecté / non initialisé)
//    - un horodatage UTC (epoch) de sa dernière écriture
//    - l'ID du dernier écrivain (id_iOT)
//    - une valeur typée : bool, int (int32), float, double, string (192 o max)
//      ou un couple float + unité (16 o UTF-8 max)
//    - un CRC32 garantissant l'intégrité de chaque trame
//
//  JETON D'ÉCRITURE (par variable, dynamique avec location) :
//    - demander_jeton(id) → REQ broadcast ; le détenteur actuel libère (RELEASE)
//    - après jeton_attente_ms sans détenteur → TAKE (conflit : le plus petit
//      id_iot gagne, fenêtre jeton_conflit_ms)
//    - le détenteur re-broadcast sa valeur toutes les jeton_heartbeat_ms
//      (heartbeat = rafraîchit les copies locales + prolonge sa location)
//    - sans heartbeat pendant jeton_location_ms → jeton libre (reprise auto)
//
//  Horodatage : l'application fournit l'heure UTC via regler_heure_utc(epoch)
//  (ex. après une synchro NTP) ; les écritures sont horodatées automatiquement.
//
//  UTILISATION (inclusion UNIQUE par projet) :
//
//    #define _ESP_NOW_SHARED_VARIABLE_
//    #include "espNow_SharedVariable.h"
//
//    #define _DONNEE_TEMP_  1
//
//    void setup() {
//      WiFi.mode(WIFI_STA);
//      EspNowSharedVariable::Config cfg;
//      cfg.id_iot = 10;
//      espNow_sharedVariable.init(cfg);          // après le démarrage du WiFi (STA)
//      espNow_sharedVariable.declare_float(_DONNEE_TEMP_, 21.5);
//    }
//
//    void loop() {
//      espNow_sharedVariable.loop();             // OBLIGATOIRE
//      espNow_sharedVariable.regler_heure_utc(epoch_utc);
//
//      if (!espNow_sharedVariable.detiens_jeton(_DONNEE_TEMP_))
//        espNow_sharedVariable.demander_jeton(_DONNEE_TEMP_);
//      else
//        espNow_sharedVariable.ecrire_float(_DONNEE_TEMP_, 21.5);
//
//      float t;
//      if (espNow_sharedVariable.lire_float(_DONNEE_TEMP_, t)) {
//        uint32_t h = espNow_sharedVariable.lire_epoch(_DONNEE_TEMP_);
//        uint16_t w = espNow_sharedVariable.dernier_ecrivain(_DONNEE_TEMP_);
//      }
//    }
//
//  Supporte ESP8266 et ESP32 (core Arduino ≥ 2.0 pour l'ESP32).
//
//  Auteur : Olivier FOURNET — Licence GPL-3.0-only — org GitHub Fo170
// ============================================================================

#if defined(ESP8266)
#include <ESP8266WiFi.h>
extern "C" {
#include <espnow.h>
}
#elif defined(ESP32)
#include <WiFi.h>
#include <esp_now.h>
#include "esp_idf_version.h"               // pour détecter la signature de callback (IDF ≥ 5)
#else
#error "espNow_SharedVariable.h : ne supporte que les ESP8266 et ESP32"
#endif
#include <string.h>
#include <stdint.h>

// ----------------------------------------------------------------------------
//  Constantes du protocole
// ----------------------------------------------------------------------------
#define ESPNOW_VAR_MAGIC_HI       0xE5
#define ESPNOW_VAR_MAGIC_LO       0x03        // distinct de la messagerie (0x02)
#define ESPNOW_VAR_VERSION        0x01
#define ESPNOW_VAR_TAILLE_MAX     250         // limite de la fenêtre ESP-NOW

#define ESPNOW_VAR_TYPE_DONNEE    0x01        // écriture / heartbeat du détenteur
#define ESPNOW_VAR_TYPE_JETON     0x02        // gestion du jeton
#define ESPNOW_VAR_JETON_REQ      0x01        // sous-op : demande de jeton
#define ESPNOW_VAR_JETON_TAKE     0x02        // sous-op : prise de jeton
#define ESPNOW_VAR_JETON_RELEASE  0x03        // sous-op : libération de jeton

#define ESPNOW_VAR_DONNEE_BOOL    0x00
#define ESPNOW_VAR_DONNEE_INT     0x01        // int32
#define ESPNOW_VAR_DONNEE_FLOAT   0x02        // float32
#define ESPNOW_VAR_DONNEE_DOUBLE  0x03        // float64
#define ESPNOW_VAR_DONNEE_STRING  0x04        // 192 o max
#define ESPNOW_VAR_DONNEE_COUPLE  0x05        // float + unité (16 o max)

#ifndef ESPNOW_VAR_MAX_VARIABLES
#define ESPNOW_VAR_MAX_VARIABLES  8           // nb max de variables déclarées
#endif
#ifndef ESPNOW_VAR_MAX_STRING
#define ESPNOW_VAR_MAX_STRING     192         // longueur max d'une valeur string
#endif
#ifndef ESPNOW_VAR_MAX_UNITE
#define ESPNOW_VAR_MAX_UNITE      16          // longueur max d'une unité (string)
#endif
#ifndef ESPNOW_VAR_RX_TAMPON
#define ESPNOW_VAR_RX_TAMPON      4           // nb de trames reçues tamponnées (ring buffer)
#endif

// états du jeton (point de vue local)
#define ESPNOW_VAR_JETON_LIBRE     0          // personne ne détient
#define ESPNOW_VAR_JETON_DEMANDE   1          // j'ai envoyé REQ, j'attends
#define ESPNOW_VAR_JETON_ATTENTE_TAKE 2       // j'ai envoyé TAKE, fenêtre de conflit
#define ESPNOW_VAR_JETON_DETENU    3          // je détiens (j'heartbeate)

// décalages de l'en-tête de trame (petit-boutiste)
#define ESPNOW_VAR_OFF_MAGIC_HI    0
#define ESPNOW_VAR_OFF_MAGIC_LO    1
#define ESPNOW_VAR_OFF_VERSION     2
#define ESPNOW_VAR_OFF_TYPE        3
#define ESPNOW_VAR_OFF_FLAGS       4          // sous-op jeton (REQ/TAKE/RELEASE)
#define ESPNOW_VAR_OFF_ID_EMETTEUR 5
#define ESPNOW_VAR_OFF_ID_VARIABLE 7
#define ESPNOW_VAR_OFF_LONGUEUR    9
#define ESPNOW_VAR_OFF_PAYLOAD     11         // DONNEE : epoch(4)+ecrivain(2)+type(1)+len(1)...

// ----------------------------------------------------------------------------
//  Utilitaires mémoire (petit-boutiste) — préfixés pour éviter tout conflit
//  de symboles avec une autre librairie ESP-NOW incluse dans le même projet.
// ----------------------------------------------------------------------------
static void espnow_var_put_u8(uint8_t* b, uint16_t& p, uint8_t v)   { b[p++] = v; }
static void espnow_var_put_u16(uint8_t* b, uint16_t& p, uint16_t v) { b[p++] = (uint8_t)(v & 0xFF); b[p++] = (uint8_t)((v >> 8) & 0xFF); }
static void espnow_var_put_u32(uint8_t* b, uint16_t& p, uint32_t v) { for (uint8_t i = 0; i < 4; i++) b[p++] = (uint8_t)((v >> (8 * i)) & 0xFF); }

static uint8_t  espnow_var_get_u8(const uint8_t* b, uint16_t& p)          { return b[p++]; }
static uint16_t espnow_var_get_u16(const uint8_t* b, uint16_t& p)         { uint16_t v = (uint16_t)b[p] | ((uint16_t)b[p + 1] << 8); p += 2; return v; }
static uint32_t espnow_var_get_u32(const uint8_t* b, uint16_t& p)         { uint32_t v = 0; for (uint8_t i = 0; i < 4; i++) v |= ((uint32_t)b[p + i]) << (8 * i); p += 4; return v; }

// CRC32 IEEE (polynôme 0xEDB88320) — calculé sur toute la trame avant le champ CRC.
static uint32_t espnow_var_crc32(const uint8_t* d, uint16_t len)
{
  uint32_t crc = 0xFFFFFFFFUL;
  for (uint16_t i = 0; i < len; i++)
  {
    crc ^= d[i];
    for (uint8_t j = 0; j < 8; j++)
    {
      if (crc & 1) crc = (crc >> 1) ^ 0xEDB88320UL;
      else         crc >>= 1;
    }
  }
  return ~crc;
}

// Copie d'une unité (string) vers un buffer avec bornage.
static void espnow_var_copie_unite(char* dest, uint8_t max_dest, const char* src, uint8_t len)
{
  if (!dest || max_dest == 0) return;
  uint8_t l = (len < max_dest - 1) ? len : (max_dest - 1);
  if (l) memcpy(dest, src, l);
  dest[l] = 0;
}

// ----------------------------------------------------------------------------
//  Classe EspNowSharedVariable — variable partagée ESP-NOW
// ----------------------------------------------------------------------------
class EspNowSharedVariable
{
public:
  struct Config
  {
    uint16_t id_iot            = 0;      // mon ID iOT (0 = non configuré)
    uint16_t jeton_attente_ms  = 800;    // après REQ, délai avant de s'attribuer un jeton libre
    uint16_t jeton_conflit_ms  = 200;    // fenêtre de conflit après TAKE (le plus petit id gagne)
    uint16_t jeton_heartbeat_ms = 1000;  // intervalle de re-broadcast de la valeur quand je détiens
    uint16_t jeton_location_ms = 3000;   // sans heartbeat du détenteur → jeton libre
  };

  void init(const Config& cfg);
  void loop();                                     // à appeler dans loop() du sketch (OBLIGATOIRE)
  void regler_heure_utc(uint32_t epoch);           // heure UTC fournie par l'app (NTP) ; écritures horodatées auto
  uint32_t heure_utc();

  // ---- déclaration (setup) : définit l'ID unique + le type d'une variable ----
  bool declare_bool(uint16_t id, bool val);
  bool declare_int(uint16_t id, int32_t val);
  bool declare_float(uint16_t id, float val);
  bool declare_double(uint16_t id, double val);
  bool declare_string(uint16_t id, const char* val);
  bool declare_couple(uint16_t id, float val, const char* unite);

  // ---- écriture (réservée au détenteur du jeton ; horodatage automatique) ----
  bool ecrire_bool(uint16_t id, bool val);
  bool ecrire_int(uint16_t id, int32_t val);
  bool ecrire_float(uint16_t id, float val);
  bool ecrire_double(uint16_t id, double val);
  bool ecrire_string(uint16_t id, const char* val);
  bool ecrire_couple(uint16_t id, float val, const char* unite);

  // ---- lecture (copie locale, dernière valeur valide reçue ou déclarée) ----
  bool lire_bool(uint16_t id, bool& val);
  bool lire_int(uint16_t id, int32_t& val);
  bool lire_float(uint16_t id, float& val, char* unite = NULL, uint8_t max_unite = 0);
  bool lire_double(uint16_t id, double& val);
  bool lire_string(uint16_t id, char* val, uint8_t max_val);
  bool lire_couple(uint16_t id, float& val, char* unite, uint8_t max_unite);
  uint32_t lire_epoch(uint16_t id);                // horodatage UTC de la dernière écriture
  uint16_t dernier_ecrivain(uint16_t id);          // ID de celui qui a écrit en dernier
  uint8_t  type_de(uint16_t id);                   // type de la variable (0xFF si inconnue)
  bool     connue(uint16_t id);                    // la variable est-elle déclarée localement ?

  // ---- jeton d'écriture (par variable, un seul détenteur à la fois) ----
  bool     demander_jeton(uint16_t id);            // démarre l'acquisition (asynchrone)
  bool     detiens_jeton(uint16_t id);             // ai-je le jeton de cette variable ?
  bool     jeton_libre(uint16_t id);               // personne ne détient le jeton ?
  uint16_t detenteur_jeton(uint16_t id);           // ID du détenteur (0 = libre)
  bool     liberer_jeton(uint16_t id);             // je libère (uniquement si je détiens)

private:
  struct _Variable
  {
    uint16_t id = 0;                    // 0 = slot libre
    uint8_t  type = 0;
    uint8_t  valeur[ESPNOW_VAR_MAX_STRING];   // valeur brute (petit-boutiste pour les numériques)
    uint8_t  len_valeur = 0;
    char     unite[ESPNOW_VAR_MAX_UNITE + 1];
    uint32_t epoch = 0;                 // horodatage UTC de la dernière écriture
    uint16_t id_ecrivain = 0;           // dernier écrivain

    // état du jeton (point de vue local)
    uint16_t jeton_detenteur = 0;       // 0 = libre
    uint32_t jeton_fin_ms = 0;          // millis limite de location (détenteur distant)
    uint32_t last_beat_ms = 0;          // dernier heartbeat que J'AI envoyé
    uint32_t requete_ms = 0;            // moment de la dernière REQ / prise de jeton
    uint8_t  etat = ESPNOW_VAR_JETON_LIBRE;
  };

  _Variable* _trouver(uint16_t id);
  _Variable* _creer(uint16_t id, uint8_t type);

  void _envoyer_donnee(_Variable* v);
  void _envoyer_jeton(uint8_t sous_op, uint16_t id_var);
  void _traiter_trame_recue(const uint8_t* t, uint16_t len);
  void _traiter_donnee(_Variable* v, const uint8_t* t, uint16_t len);
  void _traiter_jeton(_Variable* v, const uint8_t* t, uint16_t len);
  bool _ecoule(uint32_t depuis, uint32_t delta);

  Config  _cfg;
  bool    _initialise = false;
  uint32_t _epoch_utc = 0;

  uint8_t _buf_tx[ESPNOW_VAR_TAILLE_MAX];

  // ring buffer de réception : produit dans le callback (task WiFi), consommé dans loop()
  uint8_t  _rx_frames[ESPNOW_VAR_RX_TAMPON][ESPNOW_VAR_TAILLE_MAX];
  uint8_t  _rx_lens[ESPNOW_VAR_RX_TAMPON];
  volatile uint8_t _rx_head = 0;                 // producteur (callback)
  uint8_t  _rx_tail = 0;                         // consommateur (loop)

  _Variable _variables[ESPNOW_VAR_MAX_VARIABLES];

  #if defined(ESP8266)
  static void _cb_recv(uint8_t* mac, uint8_t* data, uint8_t len);
  static void _cb_sent(uint8_t* mac, uint8_t status);
  #elif defined(ESP32)
  #if ESP_IDF_VERSION_MAJOR >= 5
  static void _cb_recv(const esp_now_recv_info_t* info, const uint8_t* data, int len);
  #else
  static void _cb_recv(const uint8_t* mac, const uint8_t* data, int len);
  #endif
  static void _cb_sent(const uint8_t* mac, esp_now_send_status_t status);
  #endif
  static void _on_recv(const uint8_t* data, uint8_t len);
  static void _on_sent(uint8_t status);

  static EspNowSharedVariable* s_instance;
};

static uint8_t _espnow_var_broadcast[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
EspNowSharedVariable* EspNowSharedVariable::s_instance = NULL;

// ============================================================================
//  Initialisation
// ============================================================================
void EspNowSharedVariable::init(const Config& cfg)
{
  _cfg = cfg;
  s_instance = this;

  if (esp_now_init() != 0)
  {
    #ifdef _DEBUG_ESP_NOW_SHARED_VARIABLE_
    Serial.println(F("[espNow_SharedVariable] erreur esp_now_init"));
    #endif
    return;
  }
  #if defined(ESP8266)
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_add_peer(_espnow_var_broadcast, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
  #elif defined(ESP32)
  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, _espnow_var_broadcast, 6);
  peerInfo.channel = 0;
  peerInfo.ifidx = WIFI_IF_STA;
  peerInfo.encrypt = false;
  if (esp_now_add_peer(&peerInfo) != ESP_OK)
  {
    #ifdef _DEBUG_ESP_NOW_SHARED_VARIABLE_
    Serial.println(F("[espNow_SharedVariable] erreur esp_now_add_peer"));
    #endif
  }
  #endif
  esp_now_register_send_cb(_cb_sent);
  esp_now_register_recv_cb(_cb_recv);
  _initialise = true;
  #ifdef _DEBUG_ESP_NOW_SHARED_VARIABLE_
  Serial.println(F("[espNow_SharedVariable] init OK"));
  #endif
}

// ============================================================================
//  Horloge UTC (fournie par l'application)
// ============================================================================
void EspNowSharedVariable::regler_heure_utc(uint32_t epoch)
{
  _epoch_utc = epoch;
}

uint32_t EspNowSharedVariable::heure_utc()
{
  return _epoch_utc;
}

// ============================================================================
//  Boucle de service : trames reçues + timers jeton (heartbeat, leases, REQ)
// ============================================================================
void EspNowSharedVariable::loop()
{
  // traite toutes les trames reçues depuis le dernier appel
  while (_rx_tail != _rx_head)
  {
    uint8_t idx = _rx_tail;
    _traiter_trame_recue(_rx_frames[idx], _rx_lens[idx]);
    _rx_tail = (uint8_t)(_rx_tail + 1) % ESPNOW_VAR_RX_TAMPON;
  }

  // timers par variable
  for (uint8_t i = 0; i < ESPNOW_VAR_MAX_VARIABLES; i++)
  {
    _Variable* v = &_variables[i];
    if (v->id == 0) continue;

    if (v->etat == ESPNOW_VAR_JETON_DEMANDE)
    {
      if (_ecoule(v->requete_ms, _cfg.jeton_attente_ms))
      {
        if (v->jeton_detenteur == 0)
        {
          _envoyer_jeton(ESPNOW_VAR_JETON_TAKE, v->id);
          v->etat = ESPNOW_VAR_JETON_ATTENTE_TAKE;
          v->requete_ms = millis();
          #ifdef _DEBUG_ESP_NOW_SHARED_VARIABLE_
          Serial.print(F("[espNow_SharedVariable] jeton prise var "));
          Serial.println(v->id);
          #endif
        }
        else
        {
          v->etat = ESPNOW_VAR_JETON_LIBRE;    // un autre détient → j'observe
        }
      }
    }
    else if (v->etat == ESPNOW_VAR_JETON_ATTENTE_TAKE)
    {
      if (_ecoule(v->requete_ms, _cfg.jeton_conflit_ms))
      {
        v->etat = ESPNOW_VAR_JETON_DETENU;
        v->last_beat_ms = 0;                   // heartbeat immédiat
        #ifdef _DEBUG_ESP_NOW_SHARED_VARIABLE_
        Serial.print(F("[espNow_SharedVariable] jeton DÉTENU var "));
        Serial.println(v->id);
        #endif
      }
    }
    else if (v->etat == ESPNOW_VAR_JETON_DETENU)
    {
      if (_ecoule(v->last_beat_ms, _cfg.jeton_heartbeat_ms))
      {
        _envoyer_donnee(v);
        v->last_beat_ms = millis();
      }
    }

    // expiration de la location d'un détenteur distant → jeton libre (reprise auto)
    if (v->etat != ESPNOW_VAR_JETON_DETENU && v->jeton_detenteur != 0)
    {
      if ((int32_t)(millis() - v->jeton_fin_ms) >= 0)
      {
        #ifdef _DEBUG_ESP_NOW_SHARED_VARIABLE_
        Serial.print(F("[espNow_SharedVariable] location expirée var "));
        Serial.println(v->id);
        #endif
        v->jeton_detenteur = 0;
        v->etat = ESPNOW_VAR_JETON_LIBRE;
      }
    }
  }
}

bool EspNowSharedVariable::_ecoule(uint32_t depuis, uint32_t delta)
{
  return (uint32_t)(millis() - depuis) >= delta;
}

// ============================================================================
//  Registre des variables
// ============================================================================
EspNowSharedVariable::_Variable* EspNowSharedVariable::_trouver(uint16_t id)
{
  if (id == 0) return NULL;
  for (uint8_t i = 0; i < ESPNOW_VAR_MAX_VARIABLES; i++)
    if (_variables[i].id == id) return &_variables[i];
  return NULL;
}

EspNowSharedVariable::_Variable* EspNowSharedVariable::_creer(uint16_t id, uint8_t type)
{
  if (id == 0) return NULL;
  for (uint8_t i = 0; i < ESPNOW_VAR_MAX_VARIABLES; i++)
  {
    if (_variables[i].id == 0)
    {
      _Variable* v = &_variables[i];
      *v = _Variable();
      v->id = id;
      v->type = type;
      v->etat = ESPNOW_VAR_JETON_LIBRE;
      return v;
    }
  }
  return NULL;                                     // registre plein
}

// ============================================================================
//  Déclaration
// ============================================================================
bool EspNowSharedVariable::declare_bool(uint16_t id, bool val)
{
  if (_trouver(id)) return false;
  _Variable* v = _creer(id, ESPNOW_VAR_DONNEE_BOOL);
  if (!v) return false;
  v->valeur[0] = val ? 1 : 0;
  v->len_valeur = 1;
  return true;
}

bool EspNowSharedVariable::declare_int(uint16_t id, int32_t val)
{
  if (_trouver(id)) return false;
  _Variable* v = _creer(id, ESPNOW_VAR_DONNEE_INT);
  if (!v) return false;
  memcpy(v->valeur, &val, 4);
  v->len_valeur = 4;
  return true;
}

bool EspNowSharedVariable::declare_float(uint16_t id, float val)
{
  if (_trouver(id)) return false;
  _Variable* v = _creer(id, ESPNOW_VAR_DONNEE_FLOAT);
  if (!v) return false;
  memcpy(v->valeur, &val, 4);
  v->len_valeur = 4;
  return true;
}

bool EspNowSharedVariable::declare_double(uint16_t id, double val)
{
  if (_trouver(id)) return false;
  _Variable* v = _creer(id, ESPNOW_VAR_DONNEE_DOUBLE);
  if (!v) return false;
  memcpy(v->valeur, &val, 8);
  v->len_valeur = 8;
  return true;
}

bool EspNowSharedVariable::declare_string(uint16_t id, const char* val)
{
  if (_trouver(id)) return false;
  _Variable* v = _creer(id, ESPNOW_VAR_DONNEE_STRING);
  if (!v) return false;
  uint8_t l = val ? (uint8_t)strlen(val) : 0;
  if (l > ESPNOW_VAR_MAX_STRING) l = ESPNOW_VAR_MAX_STRING;
  v->len_valeur = l;
  if (l) memcpy(v->valeur, val, l);
  return true;
}

bool EspNowSharedVariable::declare_couple(uint16_t id, float val, const char* unite)
{
  if (_trouver(id)) return false;
  _Variable* v = _creer(id, ESPNOW_VAR_DONNEE_COUPLE);
  if (!v) return false;
  memcpy(v->valeur, &val, 4);
  v->len_valeur = 4;
  espnow_var_copie_unite(v->unite, sizeof(v->unite), unite ? unite : "", unite ? (uint8_t)strlen(unite) : 0);
  return true;
}

// ============================================================================
//  Écriture (réservée au détenteur du jeton)
// ============================================================================
bool EspNowSharedVariable::ecrire_bool(uint16_t id, bool val)
{
  _Variable* v = _trouver(id);
  if (!v || v->type != ESPNOW_VAR_DONNEE_BOOL) return false;
  if (v->etat != ESPNOW_VAR_JETON_DETENU) return false;
  v->valeur[0] = val ? 1 : 0;
  v->len_valeur = 1;
  v->epoch = _epoch_utc;
  v->id_ecrivain = _cfg.id_iot;
  _envoyer_donnee(v);
  v->last_beat_ms = millis();
  return true;
}

bool EspNowSharedVariable::ecrire_int(uint16_t id, int32_t val)
{
  _Variable* v = _trouver(id);
  if (!v || v->type != ESPNOW_VAR_DONNEE_INT) return false;
  if (v->etat != ESPNOW_VAR_JETON_DETENU) return false;
  memcpy(v->valeur, &val, 4);
  v->len_valeur = 4;
  v->epoch = _epoch_utc;
  v->id_ecrivain = _cfg.id_iot;
  _envoyer_donnee(v);
  v->last_beat_ms = millis();
  return true;
}

bool EspNowSharedVariable::ecrire_float(uint16_t id, float val)
{
  _Variable* v = _trouver(id);
  if (!v || v->type != ESPNOW_VAR_DONNEE_FLOAT) return false;
  if (v->etat != ESPNOW_VAR_JETON_DETENU) return false;
  memcpy(v->valeur, &val, 4);
  v->len_valeur = 4;
  v->epoch = _epoch_utc;
  v->id_ecrivain = _cfg.id_iot;
  _envoyer_donnee(v);
  v->last_beat_ms = millis();
  return true;
}

bool EspNowSharedVariable::ecrire_double(uint16_t id, double val)
{
  _Variable* v = _trouver(id);
  if (!v || v->type != ESPNOW_VAR_DONNEE_DOUBLE) return false;
  if (v->etat != ESPNOW_VAR_JETON_DETENU) return false;
  memcpy(v->valeur, &val, 8);
  v->len_valeur = 8;
  v->epoch = _epoch_utc;
  v->id_ecrivain = _cfg.id_iot;
  _envoyer_donnee(v);
  v->last_beat_ms = millis();
  return true;
}

bool EspNowSharedVariable::ecrire_string(uint16_t id, const char* val)
{
  _Variable* v = _trouver(id);
  if (!v || v->type != ESPNOW_VAR_DONNEE_STRING) return false;
  if (v->etat != ESPNOW_VAR_JETON_DETENU) return false;
  uint8_t l = val ? (uint8_t)strlen(val) : 0;
  if (l > ESPNOW_VAR_MAX_STRING) l = ESPNOW_VAR_MAX_STRING;
  v->len_valeur = l;
  if (l) memcpy(v->valeur, val, l);
  v->epoch = _epoch_utc;
  v->id_ecrivain = _cfg.id_iot;
  _envoyer_donnee(v);
  v->last_beat_ms = millis();
  return true;
}

bool EspNowSharedVariable::ecrire_couple(uint16_t id, float val, const char* unite)
{
  _Variable* v = _trouver(id);
  if (!v || v->type != ESPNOW_VAR_DONNEE_COUPLE) return false;
  if (v->etat != ESPNOW_VAR_JETON_DETENU) return false;
  memcpy(v->valeur, &val, 4);
  v->len_valeur = 4;
  espnow_var_copie_unite(v->unite, sizeof(v->unite), unite ? unite : "", unite ? (uint8_t)strlen(unite) : 0);
  v->epoch = _epoch_utc;
  v->id_ecrivain = _cfg.id_iot;
  _envoyer_donnee(v);
  v->last_beat_ms = millis();
  return true;
}

// ============================================================================
//  Lecture (copie locale)
// ============================================================================
bool EspNowSharedVariable::lire_bool(uint16_t id, bool& val)
{
  _Variable* v = _trouver(id);
  if (!v || v->type != ESPNOW_VAR_DONNEE_BOOL) return false;
  val = (v->valeur[0] != 0);
  return true;
}

bool EspNowSharedVariable::lire_int(uint16_t id, int32_t& val)
{
  _Variable* v = _trouver(id);
  if (!v || v->type != ESPNOW_VAR_DONNEE_INT) return false;
  memcpy(&val, v->valeur, 4);
  return true;
}

bool EspNowSharedVariable::lire_float(uint16_t id, float& val, char* unite, uint8_t max_unite)
{
  _Variable* v = _trouver(id);
  if (!v) return false;
  if (v->type == ESPNOW_VAR_DONNEE_FLOAT) memcpy(&val, v->valeur, 4);
  else if (v->type == ESPNOW_VAR_DONNEE_COUPLE) memcpy(&val, v->valeur, 4);
  else return false;
  if (unite && max_unite) espnow_var_copie_unite(unite, max_unite, v->unite, (uint8_t)strlen(v->unite));
  return true;
}

bool EspNowSharedVariable::lire_double(uint16_t id, double& val)
{
  _Variable* v = _trouver(id);
  if (!v || v->type != ESPNOW_VAR_DONNEE_DOUBLE) return false;
  memcpy(&val, v->valeur, 8);
  return true;
}

bool EspNowSharedVariable::lire_string(uint16_t id, char* val, uint8_t max_val)
{
  if (!val || max_val == 0) return false;
  _Variable* v = _trouver(id);
  if (!v || v->type != ESPNOW_VAR_DONNEE_STRING) return false;
  uint8_t l = (v->len_valeur < max_val - 1) ? v->len_valeur : (max_val - 1);
  if (l) memcpy(val, v->valeur, l);
  val[l] = 0;
  return true;
}

bool EspNowSharedVariable::lire_couple(uint16_t id, float& val, char* unite, uint8_t max_unite)
{
  _Variable* v = _trouver(id);
  if (!v || v->type != ESPNOW_VAR_DONNEE_COUPLE) return false;
  memcpy(&val, v->valeur, 4);
  if (unite && max_unite) espnow_var_copie_unite(unite, max_unite, v->unite, (uint8_t)strlen(v->unite));
  return true;
}

uint32_t EspNowSharedVariable::lire_epoch(uint16_t id)
{
  _Variable* v = _trouver(id);
  return v ? v->epoch : 0;
}

uint16_t EspNowSharedVariable::dernier_ecrivain(uint16_t id)
{
  _Variable* v = _trouver(id);
  return v ? v->id_ecrivain : 0;
}

uint8_t EspNowSharedVariable::type_de(uint16_t id)
{
  _Variable* v = _trouver(id);
  return v ? v->type : 0xFF;
}

bool EspNowSharedVariable::connue(uint16_t id)
{
  return _trouver(id) != NULL;
}

// ============================================================================
//  Jeton d'écriture
// ============================================================================
bool EspNowSharedVariable::demander_jeton(uint16_t id)
{
  _Variable* v = _trouver(id);
  if (!v) return false;
  if (v->etat != ESPNOW_VAR_JETON_LIBRE || v->jeton_detenteur != 0) return false;
  _envoyer_jeton(ESPNOW_VAR_JETON_REQ, id);
  v->etat = ESPNOW_VAR_JETON_DEMANDE;
  v->requete_ms = millis();
  return true;
}

bool EspNowSharedVariable::detiens_jeton(uint16_t id)
{
  _Variable* v = _trouver(id);
  return v && v->etat == ESPNOW_VAR_JETON_DETENU;
}

bool EspNowSharedVariable::jeton_libre(uint16_t id)
{
  _Variable* v = _trouver(id);
  return v && v->etat == ESPNOW_VAR_JETON_LIBRE && v->jeton_detenteur == 0;
}

uint16_t EspNowSharedVariable::detenteur_jeton(uint16_t id)
{
  _Variable* v = _trouver(id);
  if (!v) return 0;
  if (v->etat == ESPNOW_VAR_JETON_DETENU) return _cfg.id_iot;
  return v->jeton_detenteur;
}

bool EspNowSharedVariable::liberer_jeton(uint16_t id)
{
  _Variable* v = _trouver(id);
  if (!v || v->etat != ESPNOW_VAR_JETON_DETENU) return false;
  _envoyer_jeton(ESPNOW_VAR_JETON_RELEASE, id);
  v->etat = ESPNOW_VAR_JETON_LIBRE;
  v->jeton_detenteur = 0;
  return true;
}

// ============================================================================
//  Construction des trames
// ============================================================================
void EspNowSharedVariable::_envoyer_donnee(_Variable* v)
{
  if (!_initialise) return;
  uint16_t p = ESPNOW_VAR_OFF_ID_EMETTEUR;
  _buf_tx[ESPNOW_VAR_OFF_MAGIC_HI] = ESPNOW_VAR_MAGIC_HI;
  _buf_tx[ESPNOW_VAR_OFF_MAGIC_LO] = ESPNOW_VAR_MAGIC_LO;
  _buf_tx[ESPNOW_VAR_OFF_VERSION]  = ESPNOW_VAR_VERSION;
  _buf_tx[ESPNOW_VAR_OFF_TYPE]     = ESPNOW_VAR_TYPE_DONNEE;
  _buf_tx[ESPNOW_VAR_OFF_FLAGS]    = 0;
  espnow_var_put_u16(_buf_tx, p, _cfg.id_iot);          // emetteur
  espnow_var_put_u16(_buf_tx, p, v->id);                // id_variable
  p = ESPNOW_VAR_OFF_PAYLOAD;
  espnow_var_put_u32(_buf_tx, p, v->epoch);             // horodatage UTC
  espnow_var_put_u16(_buf_tx, p, v->id_ecrivain);       // dernier écrivain
  espnow_var_put_u8(_buf_tx, p, v->type);
  espnow_var_put_u8(_buf_tx, p, v->len_valeur);
  if (v->len_valeur) memcpy(_buf_tx + p, v->valeur, v->len_valeur);
  p += v->len_valeur;
  uint8_t lu = (uint8_t)strlen(v->unite);
  espnow_var_put_u8(_buf_tx, p, lu);
  if (lu) memcpy(_buf_tx + p, v->unite, lu);
  p += lu;

  uint16_t longueur = p + 4;                            // + CRC
  uint16_t off = ESPNOW_VAR_OFF_LONGUEUR;
  espnow_var_put_u16(_buf_tx, off, longueur);
  espnow_var_put_u32(_buf_tx, p, espnow_var_crc32(_buf_tx, p));

  esp_now_send(NULL, _buf_tx, (uint8_t)longueur);
  #ifdef _DEBUG_ESP_NOW_SHARED_VARIABLE_
  Serial.print(F("[espNow_SharedVariable] envoi DONNEE var "));
  Serial.print(v->id);
  Serial.print(F(" epoch="));
  Serial.print(v->epoch);
  Serial.print(F(" taille="));
  Serial.println(longueur);
  #endif
}

void EspNowSharedVariable::_envoyer_jeton(uint8_t sous_op, uint16_t id_var)
{
  if (!_initialise) return;
  uint16_t p = ESPNOW_VAR_OFF_ID_EMETTEUR;
  _buf_tx[ESPNOW_VAR_OFF_MAGIC_HI] = ESPNOW_VAR_MAGIC_HI;
  _buf_tx[ESPNOW_VAR_OFF_MAGIC_LO] = ESPNOW_VAR_MAGIC_LO;
  _buf_tx[ESPNOW_VAR_OFF_VERSION]  = ESPNOW_VAR_VERSION;
  _buf_tx[ESPNOW_VAR_OFF_TYPE]     = ESPNOW_VAR_TYPE_JETON;
  _buf_tx[ESPNOW_VAR_OFF_FLAGS]    = sous_op;
  espnow_var_put_u16(_buf_tx, p, _cfg.id_iot);
  espnow_var_put_u16(_buf_tx, p, id_var);
  p = ESPNOW_VAR_OFF_PAYLOAD;

  uint16_t longueur = p + 4;                            // + CRC
  uint16_t off = ESPNOW_VAR_OFF_LONGUEUR;
  espnow_var_put_u16(_buf_tx, off, longueur);
  espnow_var_put_u32(_buf_tx, p, espnow_var_crc32(_buf_tx, p));

  esp_now_send(NULL, _buf_tx, (uint8_t)longueur);
  #ifdef _DEBUG_ESP_NOW_SHARED_VARIABLE_
  Serial.print(F("[espNow_SharedVariable] jeton var "));
  Serial.print(id_var);
  Serial.print(F(" sous-op "));
  Serial.println(sous_op);
  #endif
}

// ============================================================================
//  Réception (callbacks ESP-NOW)
// ============================================================================
#if defined(ESP8266)
void EspNowSharedVariable::_cb_recv(uint8_t* mac, uint8_t* data, uint8_t len)
{
  (void)mac;
  _on_recv(data, len);
}
void EspNowSharedVariable::_cb_sent(uint8_t* mac, uint8_t status)
{
  (void)mac;
  _on_sent(status);
}
#elif defined(ESP32)
#if ESP_IDF_VERSION_MAJOR >= 5
void EspNowSharedVariable::_cb_recv(const esp_now_recv_info_t* info, const uint8_t* data, int len)
{
  (void)info;
  _on_recv(data, (uint8_t)len);
}
#else
void EspNowSharedVariable::_cb_recv(const uint8_t* mac, const uint8_t* data, int len)
{
  (void)mac;
  _on_recv(data, (uint8_t)len);
}
#endif
void EspNowSharedVariable::_cb_sent(const uint8_t* mac, esp_now_send_status_t status)
{
  (void)mac;
  _on_sent((uint8_t)(status == ESP_NOW_SEND_SUCCESS ? 0 : 1));
}
#endif

void EspNowSharedVariable::_on_recv(const uint8_t* data, uint8_t len)
{
  if (!s_instance) return;
  if (len > ESPNOW_VAR_TAILLE_MAX) return;
  uint8_t suivant = (uint8_t)(s_instance->_rx_head + 1) % ESPNOW_VAR_RX_TAMPON;
  if (suivant == s_instance->_rx_tail) return;       // tampon plein → trame perdue
  memcpy(s_instance->_rx_frames[s_instance->_rx_head], data, len);
  s_instance->_rx_lens[s_instance->_rx_head] = len;
  s_instance->_rx_head = suivant;                    // publie après la copie complète
}

void EspNowSharedVariable::_on_sent(uint8_t status)
{
  (void)status;
  #ifdef _DEBUG_ESP_NOW_SHARED_VARIABLE_
  Serial.print(F("[espNow_SharedVariable] envoi "));
  Serial.println(status == 0 ? F("OK") : F("ÉCHEC"));
  #endif
}

void EspNowSharedVariable::_traiter_trame_recue(const uint8_t* t, uint16_t len)
{
  if (len < ESPNOW_VAR_OFF_PAYLOAD + 4) return;       // minimum : en-tête + CRC
  if (t[ESPNOW_VAR_OFF_MAGIC_HI] != ESPNOW_VAR_MAGIC_HI ||
      t[ESPNOW_VAR_OFF_MAGIC_LO] != ESPNOW_VAR_MAGIC_LO) return;
  if (t[ESPNOW_VAR_OFF_VERSION] != ESPNOW_VAR_VERSION) return;

  uint16_t p = ESPNOW_VAR_OFF_LONGUEUR;
  uint16_t longueur = espnow_var_get_u16(t, p);
  if (longueur != len || longueur < 4) return;
  p = longueur - 4;
  uint32_t crc_attendu = espnow_var_get_u32(t, p);
  if (espnow_var_crc32(t, longueur - 4) != crc_attendu)
  {
    #ifdef _DEBUG_ESP_NOW_SHARED_VARIABLE_
    Serial.println(F("[espNow_SharedVariable] CRC invalide"));
    #endif
    return;
  }

  p = ESPNOW_VAR_OFF_ID_VARIABLE;
  uint16_t id_var = espnow_var_get_u16(t, p);
  _Variable* v = _trouver(id_var);
  if (!v) return;                                     // variable non déclarée localement

  if (t[ESPNOW_VAR_OFF_TYPE] == ESPNOW_VAR_TYPE_DONNEE) _traiter_donnee(v, t, len);
  else if (t[ESPNOW_VAR_OFF_TYPE] == ESPNOW_VAR_TYPE_JETON) _traiter_jeton(v, t, len);
}

void EspNowSharedVariable::_traiter_donnee(_Variable* v, const uint8_t* t, uint16_t len)
{
  uint16_t p = ESPNOW_VAR_OFF_ID_EMETTEUR;
  uint16_t emetteur = espnow_var_get_u16(t, p);
  p = ESPNOW_VAR_OFF_PAYLOAD;
  if (p + 4 + 2 + 1 + 1 > len) return;                // epoch(4)+ecrivain(2)+type(1)+len(1)
  uint32_t epoch = espnow_var_get_u32(t, p);
  uint16_t id_ecrivain = espnow_var_get_u16(t, p);
  uint8_t type = espnow_var_get_u8(t, p);
  uint8_t len_valeur = espnow_var_get_u8(t, p);
  if (p + len_valeur + 1 > len) return;
  const uint8_t* val = t + p; p += len_valeur;
  uint8_t len_unite = espnow_var_get_u8(t, p);
  if (p + len_unite > len) return;
  const char* unite = (const char*)(t + p);

  if (type != v->type) return;                        // type incohérent → trame ignorée
  if (epoch < v->epoch) return;                       // trame périmée (ancienne écriture)

  v->epoch = epoch;
  v->id_ecrivain = id_ecrivain;
  v->len_valeur = len_valeur;
  if (len_valeur) memcpy(v->valeur, val, len_valeur);
  if (len_unite > ESPNOW_VAR_MAX_UNITE) len_unite = ESPNOW_VAR_MAX_UNITE;
  if (len_unite) memcpy(v->unite, unite, len_unite);
  v->unite[len_unite] = 0;

  // le détenteur est l'émetteur ; le plus petit id gagne en cas de conflit
  if (emetteur != 0 && emetteur != _cfg.id_iot)
  {
    if (v->etat == ESPNOW_VAR_JETON_DETENU)
    {
      // je détiens : je ne cède qu'à un id plus petit (conflit résolu)
      if (emetteur < _cfg.id_iot)
      {
        #ifdef _DEBUG_ESP_NOW_SHARED_VARIABLE_
        Serial.print(F("[espNow_SharedVariable] un plus petit id écrit, je cède var "));
        Serial.println(v->id);
        #endif
        v->jeton_detenteur = emetteur;
        v->jeton_fin_ms = millis() + _cfg.jeton_location_ms;
        v->etat = ESPNOW_VAR_JETON_LIBRE;
      }
    }
    else if (v->jeton_detenteur == 0 || emetteur == v->jeton_detenteur ||
             emetteur < v->jeton_detenteur)
    {
      v->jeton_detenteur = emetteur;
      v->jeton_fin_ms = millis() + _cfg.jeton_location_ms;
    }
  }

  #ifdef _DEBUG_ESP_NOW_SHARED_VARIABLE_
  Serial.print(F("[espNow_SharedVariable] DONNEE var "));
  Serial.print(v->id);
  Serial.print(F(" emetteur="));
  Serial.print(emetteur);
  Serial.print(F(" epoch="));
  Serial.println(epoch);
  #endif
}

void EspNowSharedVariable::_traiter_jeton(_Variable* v, const uint8_t* t, uint16_t len)
{
  (void)len;
  uint16_t p = ESPNOW_VAR_OFF_ID_EMETTEUR;
  uint16_t emetteur = espnow_var_get_u16(t, p);
  uint8_t sous_op = t[ESPNOW_VAR_OFF_FLAGS];

  if (sous_op == ESPNOW_VAR_JETON_REQ)
  {
    // le détenteur actuel libère immédiatement (transmission de main)
    if (v->etat == ESPNOW_VAR_JETON_DETENU)
    {
      _envoyer_jeton(ESPNOW_VAR_JETON_RELEASE, v->id);
      v->etat = ESPNOW_VAR_JETON_LIBRE;
      v->jeton_detenteur = 0;
    }
  }
  else if (sous_op == ESPNOW_VAR_JETON_TAKE)
  {
    if (v->etat == ESPNOW_VAR_JETON_ATTENTE_TAKE)
    {
      // fenêtre de conflit : le plus petit id_iot gagne
      if (emetteur < _cfg.id_iot)
      {
        v->etat = ESPNOW_VAR_JETON_LIBRE;              // je cède
        v->jeton_detenteur = emetteur;
        v->jeton_fin_ms = millis() + _cfg.jeton_location_ms;
      }
      // sinon je continue : mon TAKE (plus petit) gagnera
    }
    else if (v->etat == ESPNOW_VAR_JETON_DEMANDE)
    {
      // quelqu'un a pris le jeton avant moi → j'observe
      v->etat = ESPNOW_VAR_JETON_LIBRE;
      v->jeton_detenteur = emetteur;
      v->jeton_fin_ms = millis() + _cfg.jeton_location_ms;
    }
    else if (v->etat == ESPNOW_VAR_JETON_DETENU)
    {
      // je détiens : je ne cède qu'à un id plus petit
      if (emetteur < _cfg.id_iot)
      {
        v->etat = ESPNOW_VAR_JETON_LIBRE;
        v->jeton_detenteur = emetteur;
        v->jeton_fin_ms = millis() + _cfg.jeton_location_ms;
      }
    }
    else if (v->jeton_detenteur == 0 || emetteur < v->jeton_detenteur)
    {
      v->jeton_detenteur = emetteur;
      v->jeton_fin_ms = millis() + _cfg.jeton_location_ms;
    }
  }
  else if (sous_op == ESPNOW_VAR_JETON_RELEASE)
  {
    // simple observateur : je note que le jeton est libre
    if (v->etat == ESPNOW_VAR_JETON_LIBRE)
    {
      v->jeton_detenteur = 0;
      v->etat = ESPNOW_VAR_JETON_LIBRE;
    }
    // pendant une demande (DEMANDE / ATTENTE_TAKE) : la libération du détenteur
    // me laisse continuer → j'irai TAKE quand le délai d'attente sera écoulé
  }
}

// instance globale unique — inclusion unique par projet (pas de garde #ifndef)
EspNowSharedVariable espNow_sharedVariable;

#endif // esp_now_sharedvariable
