# espNow_SharedVariable

**Variable partagée ESP-NOW** pour **ESP8266** et **ESP32** en un **seul fichier `.h`** (classe `EspNowSharedVariable` + instance globale `espNow_sharedVariable`).

Rends une variable logique **virtuellement accessible** entre plusieurs iOT : chaque appareil **déclare** la variable (ID unique + type) et en garde une **copie locale à jour**, propagée en broadcast par le **détenteur du jeton** d'écriture (un seul écrivain à la fois, par variable).

Auteur : Olivier FOURNET — Licence GPL-3.0-only.

## Fonctionnalités

- **Une variable, plusieurs lecteurs** : `lire_*()` renvoie la copie locale (dernière valeur valide), mise à jour automatiquement par les broadcasts du détenteur.
- **Jeton d'écriture par variable** : un seul iOT peut écrire à la fois. Arbitrage **dynamique avec location** (REQ / TAKE / RELEASE / heartbeat), **sans horloges synchronisées** — reprise automatique si le détenteur disparaît.
- **Horodatage UTC** : l'application fournit l'heure (`regler_heure_utc`) ; chaque écriture est horodatée automatiquement.
- **Métadonnées** : ID du dernier écrivain (`dernier_ecrivain`), horodatage (`lire_epoch`).
- **CRC32** (`0xEDB88320`) sur chaque trame : une valeur corrompue est rejetée.
- **Types** : bool, int (int32), float, double, string (192 o max), couple **float + unité** (16 o UTF-8 max).
- **Registre statique** : variables déclarées au `setup()`, taille fixe (`ESPNOW_VAR_MAX_VARIABLES`), **aucune allocation dynamique**.

## Compatibilité

| | |
|---|---|
| ESP8266 | core Arduino ESP8266 (API `espnow.h`) |
| ESP32 | core Arduino ESP32 ≥ 2.0 — l'API est identique |
| Arduino IDE | oui (copier le `.h` dans le dossier du sketch) |
| PlatformIO | oui (`library.json` + exemples) |

## Installation

### Arduino IDE

Copier `espNow_SharedVariable.h` dans le dossier de votre sketch, puis :

```cpp
#define _ESP_NOW_SHARED_VARIABLE_
#include "espNow_SharedVariable.h"
```

**Une seule inclusion par projet** : le fichier définit l'instance globale `espNow_sharedVariable` (pas de garde `#ifndef` — il est activé par `_ESP_NOW_SHARED_VARIABLE_`).

### PlatformIO

En tant que dépendance (convention Fo170 — toujours la forme git complète) :

```ini
lib_deps = https://github.com/Fo170/espNow_SharedVariable.git
```

Ou en local, dans les exemples fournis : `lib_deps = ../..` (chemin vers la racine du dépôt).

## Utilisation rapide

```cpp
#define _ESP_NOW_SHARED_VARIABLE_
#include "espNow_SharedVariable.h"

#define ID_MA_SONDE     10          // mon id iOT (unique sur le réseau)
#define _DONNEE_TEMP_    1
#define _DONNEE_PUISS_   2

void setup() {
  WiFi.mode(WIFI_STA);                        // ESP-NOW en mode station (sans AP)

  EspNowSharedVariable::Config cfg;
  cfg.id_iot = ID_MA_SONDE;
  espNow_sharedVariable.init(cfg);            // après le démarrage du WiFi

  espNow_sharedVariable.declare_float(_DONNEE_TEMP_, 21.5);
  espNow_sharedVariable.declare_couple(_DONNEE_PUISS_, 0.0, "W");
}

void loop() {
  espNow_sharedVariable.loop();               // OBLIGATOIRE
  espNow_sharedVariable.regler_heure_utc(epoch_utc);   // fourni par votre lib NTP

  // --- pour écrire : demander puis attendre le jeton, puis écrire ---
  if (!espNow_sharedVariable.detiens_jeton(_DONNEE_TEMP_))
    espNow_sharedVariable.demander_jeton(_DONNEE_TEMP_);
  else
    espNow_sharedVariable.ecrire_float(_DONNEE_TEMP_, 21.5);

  // --- lire (toujours disponible, même sans jeton) ---
  float t;
  if (espNow_sharedVariable.lire_float(_DONNEE_TEMP_, t)) {
    uint32_t h  = espNow_sharedVariable.lire_epoch(_DONNEE_TEMP_);
    uint16_t id = espNow_sharedVariable.dernier_ecrivain(_DONNEE_TEMP_);
  }
}
```

## API

### Configuration

| Champ | Défaut | Rôle |
|---|---|---|
| `cfg.id_iot` | `0` | mon ID iOT (u16, unique sur le réseau) |
| `cfg.jeton_attente_ms` | `800` | après une REQ, délai avant de s'attribuer un jeton libre |
| `cfg.jeton_conflit_ms` | `200` | fenêtre de conflit après TAKE (le plus petit id_iot gagne) |
| `cfg.jeton_heartbeat_ms` | `1000` | intervalle de re-broadcast de la valeur quand je détiens |
| `cfg.jeton_location_ms` | `3000` | sans heartbeat du détenteur → jeton libre (reprise auto) |

### Déclaration (setup)

| Méthode | Rôle |
|---|---|
| `declare_bool / declare_int / declare_float / declare_double / declare_string(id, val)` | déclare la variable + sa valeur initiale (false si déjà déclarée ou registre plein) |
| `declare_couple(id, val, unite)` | déclare un couple float + unité |

### Écriture (réservée au détenteur du jeton)

| Méthode | Rôle |
|---|---|
| `ecrire_bool / ecrire_int / ecrire_float / ecrire_double / ecrire_string(id, val)` | écrit localement + broadcast ; horodatage UTC et `id_ecrivain` ajoutés automatiquement. **false si je ne détiens pas le jeton** |
| `ecrire_couple(id, val, unite)` | idem avec unité |

### Lecture (copie locale)

| Méthode | Rôle |
|---|---|
| `lire_bool / lire_int / lire_double(id, &val)` | lit la valeur (type exact requis) |
| `lire_float(id, &val, unite=NULL, max_unite=0)` | lit un float **ou** un couple |
| `lire_string(id, val, max_val)` | lit un string |
| `lire_couple(id, &val, unite, max_unite)` | lit un couple (float + unité) |
| `lire_epoch(id)` | horodatage UTC de la dernière écriture |
| `dernier_ecrivain(id)` | ID du dernier écrivain |
| `type_de(id)` | type de la variable (`0xFF` si inconnue) |
| `connue(id)` | la variable est-elle déclarée localement ? |

### Jeton d'écriture

| Méthode | Rôle |
|---|---|
| `demander_jeton(id)` | démarre l'acquisition (asynchrone, broadcast REQ) |
| `detiens_jeton(id)` | ai-je le jeton de cette variable ? |
| `jeton_libre(id)` | personne ne détient le jeton ? |
| `detenteur_jeton(id)` | ID du détenteur (`0` = libre) |
| `liberer_jeton(id)` | je libère (uniquement si je détiens) |

## Protocole (une variable par trame, broadcast, petit-boutiste)

- **En-tête (11 octets)** : magic `0xE5 0x03` (distinct de la messagerie `0x02` → les deux peuvent coexister), version `0x01`, type (DONNEE / JETON), flags (sous-op jeton REQ/TAKE/RELEASE), id_émetteur (u16), id_variable (u16), longueur totale (u16).
- **DONNEE** : epoch UTC (u32) + id_ecrivain (u16) + type (u8) + longueur valeur (u8) + valeur (bool 1 o / int 4 o / float 4 o / double 8 o / string ≤192 o) + longueur unité (u8) + unité (≤16 o).
- **JETON** : sous-op REQ / TAKE / RELEASE, aucune charge utile.
- **CRC32** (polynôme `0xEDB88320`) en fin de trame, calculé sur toute la trame avant le champ CRC.

### Le jeton en détail

- `demander_jeton(id)` envoie `REQ` en broadcast ; **le détenteur actuel libère immédiatement** (`RELEASE`, transmission de main).
- Après `jeton_attente_ms` sans détenteur : envoi de `TAKE`, puis fenêtre de conflit `jeton_conflit_ms`. **En cas de conflit (deux appareils), le plus petit `id_iot` gagne** — les IDs iOT doivent être uniques sur le réseau.
- Le détenteur **re-broadcast sa valeur** (heartbeat) toutes les `jeton_heartbeat_ms` : cela rafraîchit les copies locales des lecteurs **et** prolonge sa location.
- Sans heartbeat pendant `jeton_location_ms`, le jeton redevient libre : **reprise automatique** si le détenteur meurt.
- Un récepteur n'accepte une DONNEE que si **CRC OK**, type cohérent et **epoch ≥ epoch local** (les trames périmées sont ignorées).

## Macros de compilation

| Macro | Effet |
|---|---|
| `_ESP_NOW_SHARED_VARIABLE_` | active le fichier (obligatoire, à définir avant l'`#include`) |
| `_DEBUG_ESP_NOW_SHARED_VARIABLE_` | logs Serial (envoi/réception/jeton/CRC) |
| `ESPNOW_VAR_MAX_VARIABLES` | nb max de variables déclarées (défaut 8) |
| `ESPNOW_VAR_MAX_STRING` | longueur max d'une valeur string (défaut 192) |
| `ESPNOW_VAR_MAX_UNITE` | longueur max d'une unité (défaut 16) |
| `ESPNOW_VAR_RX_TAMPON` | taille du ring buffer de réception (défaut 4) |

### Mémoire

Chaque variable déclarée consomme ≈ 234 octets de RAM (dominés par le buffer string de 192 o). Réduire `ESPNOW_VAR_MAX_VARIABLES` et/ou `ESPNOW_VAR_MAX_STRING` pour les petits montages.

## Exemples

`exemples/` contient un exemple par plateforme (déclaration, jeton, écriture, lecture, horodatage, dernier écrivain) :

```bash
pio run -d exemples/exemple_ESP8266
pio run -d exemples/exemple_ESP32
```

## Vérification

- Aucun test automatisé dans le dépôt (code embarqué).
- Validation possible avec des mocks `Arduino.h`/`espnow.h`/`esp_now.h` (`g++ -std=gnu++11`), ou mieux : compilation **PlatformIO** des exemples.
