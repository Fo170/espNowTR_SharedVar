# espNowTR_SharedVar

**Variable partagée ESP-NOW** pour **ESP8266** et **ESP32** (classe `espNowTR_SharedVar` + instance globale `espNowTR_sharedVar`).

Rends une variable logique **virtuellement accessible** entre plusieurs iOT : chaque appareil **déclare** la variable (ID unique + type) et en garde une **copie locale à jour**, propagée en broadcast par le **détenteur du jeton** d'écriture (un seul écrivain à la fois, par variable).

Auteur : Olivier FOURNET — Licence GPL-3.0-only.

> **Version : 1.0.1**
> - **v1.0.1** : ajout de **`rearm()`** (ré-initialise ESP-NOW + callbacks + peer broadcast après un
>   reset WiFi — même problème que `espNow_SharedVar`, à appeler à chaque (re)connexion WiFi) ;
>   correction du **peer broadcast ESP8266 figé sur le canal 1** → canal 0 (= canal courant du WiFi) ;
>   **tampon RX porté à 16 trames** (au lieu de 4, un `loop()` bloqué par de l'HTTP ne perdait plus
>   les trames entrantes) + compteur public **`rx_pertes`**.

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

Copier `espNowTR_SharedVar.h` dans le dossier de votre sketch, puis :

```cpp
#define _ESP_NOW_TR_SHARED_VAR_
#include "espNowTR_SharedVar.h"
```

**Une seule inclusion par projet** : le fichier définit l'instance globale `espNowTR_sharedVar` (pas de garde `#ifndef` — il est activé par `_ESP_NOW_TR_SHARED_VAR_`).

### PlatformIO

En tant que dépendance (convention Fo170 — toujours la forme git complète) :

```ini
lib_deps = https://github.com/Fo170/espNowTR_SharedVar.git
```

Ou en local, dans les exemples fournis : `lib_deps = ../..` (chemin vers la racine du dépôt).

## Utilisation rapide

```cpp
#define _ESP_NOW_TR_SHARED_VAR_
#include "espNowTR_SharedVar.h"

#define ID_MA_SONDE     10          // mon id iOT (unique sur le réseau)
#define _DONNEE_TEMP_    1
#define _DONNEE_PUISS_   2

void setup() {
  WiFi.mode(WIFI_STA);                        // ESP-NOW en mode station (sans AP)

  espNowTR_SharedVar::Config cfg;
  cfg.id_iot = ID_MA_SONDE;
  espNowTR_sharedVar.init(cfg);            // après le démarrage du WiFi

  espNowTR_sharedVar.declare_float(_DONNEE_TEMP_, 21.5);
  espNowTR_sharedVar.declare_couple(_DONNEE_PUISS_, 0.0, "W");
}

void loop() {
  espNowTR_sharedVar.loop();               // OBLIGATOIRE
  espNowTR_sharedVar.regler_heure_utc(epoch_utc);   // fourni par votre lib NTP

  // --- pour écrire : demander puis attendre le jeton, puis écrire ---
  if (!espNowTR_sharedVar.detiens_jeton(_DONNEE_TEMP_))
    espNowTR_sharedVar.demander_jeton(_DONNEE_TEMP_);
  else
    espNowTR_sharedVar.ecrire_float(_DONNEE_TEMP_, 21.5);

  // --- lire (toujours disponible, même sans jeton) ---
  float t;
  if (espNowTR_sharedVar.lire_float(_DONNEE_TEMP_, t)) {
    uint32_t h  = espNowTR_sharedVar.lire_epoch(_DONNEE_TEMP_);
    uint16_t id = espNowTR_sharedVar.dernier_ecrivain(_DONNEE_TEMP_);
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

### Le jeton en détail (machinerie Token Ring)

Le jeton est **par variable** : chaque variable possède son propre jeton et sa propre machine à états locale `LIBRE → DEMANDE → ATTENTE_TAKE → DETENU` (constantes `ESPNOW_VAR_JETON_*`). Un seul iOT à la fois peut détenir le jeton d'une variable — c'est lui, et lui seul, qui est autorisé à `ecrire_*()`.

**Timings (champs `Config`, modifiables)** :

| Constante | Défaut | Rôle |
|---|---|---|
| `jeton_attente_ms` | 800 | après un `REQ`, délai avant de s'attribuer un jeton libre |
| `jeton_conflit_ms` | 200 | fenêtre de conflit après un `TAKE` (le plus petit `id_iot` gagne) |
| `jeton_heartbeat_ms` | 1000 | le détenteur re-broadcast sa valeur (heartbeat) |
| `jeton_location_ms` | 3000 | sans heartbeat, le jeton redevient libre |

**Acquisition (celui qui veut écrire)** :

1. `demander_jeton(id)` n'aboutit que si l'état local est `LIBRE` **et** qu'aucun détenteur n'est connu → broadcast `REQ`.
   - Le détenteur actuel, s'il en existe un, libère **immédiatement** (`RELEASE`, transmission de main).
2. Après `jeton_attente_ms` sans détenteur connu : broadcast `TAKE`, puis ouverture de la fenêtre de conflit `jeton_conflit_ms`.
   - Deux candidats simultanés : **le plus petit `id_iot` gagne**, l'autre cède et devient observateur.
3. Après `jeton_conflit_ms` sans candidat plus petit : passage en **`DETENU`**, heartbeat immédiat puis toutes les `jeton_heartbeat_ms`.

**Maintien (celui qui détient)** :

- Le détenteur **re-broadcast sa valeur** (trame DONNEE) toutes les `jeton_heartbeat_ms` : cela rafraîchit les copies locales des lecteurs **et** prolonge la location que ceux-ci lui accordent.
- Chaque observateur renouvelle son `jeton_fin_ms` à chaque DONNEE reçue du détenteur.
- Sans DONNEE du détenteur pendant `jeton_location_ms`, chaque observateur le déclare libre : **reprise automatique** (détenteur mort ou hors de portée).

**Libération** : `liberer_jeton(id)` broadcast `RELEASE` ; un `REQ` reçu par le détenteur déclenche aussi une libération immédiate.

**Résolution de conflit et auto-réparation** :

- Deux `TAKE` simultanés : le plus petit `id_iot` garde le jeton, l'autre observe.
- Le broadcast ESP-NOW n'a **pas d'acquittement** : si deux `TAKE` se perdent et que deux nœuds passent en `DETENU`, leurs heartbeats (DONNEE) finissent par se croiser et **le plus petit `id_iot` l'emporte en 1 à 2 périodes de heartbeat** : la DONNEE du plus petit portant un epoch plus ancien est d'abord rejetée (`epoch < local`), le plus petit adopte la valeur la plus récente puis la re-broadcast, ce qui déclenche la cession du plus grand.
- Un détenteur `DETENU` qui reçoit une DONNEE d'un `id_iot` plus petit (epoch ≥ local) cède immédiatement le jeton.

### Robustesse et cohérence de lecture

**Lecture toujours atomique (pas de lecture « pendante »)** :

- Une valeur tient **dans une seule trame** (≤ 216 o) : epoch + id_ecrivain + type + longueur + valeur + unité, protégée par CRC32.
- La copie locale n'est mise à jour qu'en `loop()` (`_traiter_donnee`), à partir d'une trame **complète et validée** (CRC OK, type identique, epoch ≥ local). Un `lire_*()` renvoie donc toujours un état cohérent — l'ancienne valeur complète ou la nouvelle valeur complète, jamais un mélange.
- Réception et lecture s'exécutent dans le même contexte (`loop()`, mono-thread Arduino) : aucun accès concurrent à `_Variable`.
- **Conclusion** : un flag « variable en cours d'écriture / verrouillée » n'est **pas nécessaire** — une trame arrive entière ou pas, et le flag introduirait lui-même une course (quand le marquer, quand le dé-marquer). La fraîcheur se juge déjà via `lire_epoch()` / `dernier_ecrivain()`.

**Un seul écrivain** : garanti par le jeton dans les conditions nominales (IDs iOT uniques, nœuds coopératifs). En cas de trames perdues, l'arbitrage par plus petit `id_iot` + comparaison d'epoch reconverge.

### Robustesse sous FreeRTOS (ESP32)

Sur ESP32, la librairie tourne dans un environnement **multitâche multi-cœur** (FreeRTOS) : le callback ESP-NOW s'exécute dans la tâche WiFi (core 0), pendant que `loop()` (et vos appels `lire_*()` / `ecrire_*()`) s'exécute sur l'autre cœur. La robustesse est préservée par construction :

- **Le callback ne touche jamais les données** : il se contente de pousser la trame reçue dans un ring buffer `_rx_frames` (production, core 0). L'application de la trame sur `_Variable` (consommation) se fait exclusivement en `loop()`. Toute modification d'une variable est donc **mono-thread** : pas de course sur les valeurs, lecture toujours atomique.
- **Ring buffer SPSC sans mutex** : un seul producteur (callback) et un seul consommateur (`loop()`) ; `_rx_head` est `volatile` et publié **après** la copie complète de la trame (ordre producteur → consommateur), `_rx_tail` n'est écrit que par `loop()`.
- **Limite théorique du multi-cœur** : `volatile` n'est pas une barrière mémoire stricte sur Xtensa — dans le pire cas, `loop()` pourrait voir le nouveau `_rx_head` avant que les données du slot soient visibles. Conséquence : la trame partielle est rejetée par le **CRC** → dégradation en *trame perdue*, jamais en corruption de valeur (ni lecture pendante). Le heartbeat et la reprise automatique compensent ces pertes.
- **Règle d'usage** : `lire_*()` / `ecrire_*()` / `regler_heure_utc()` doivent être appelés depuis `loop()` (ou une seule tâche dédiée). Les appeler depuis une tâche concurrente de `loop()` introduirait une vraie course sur `_Variable` — c'est l'usage documenté, et les exemples fournis le respectent.

**Limites connues** :

- Broadcast non fiable (pas d'ACK) : une collision de `TAKE` peut donner une courte fenêtre à deux écrivains ; l'epoch (le plus récent gagne) et l'arbitrage par plus petit id rétablissent la cohérence.
- Dépendance aux **IDs iOT uniques** : deux nœuds de même `id_iot` ne peuvent pas s'arbitrer.
- Horodatage : l'epoch provient de `regler_heure_utc()` (NTP côté application) ; un nœud sans heure écrit avec l'epoch 0, et ses écritures sont **ignorées** par des lecteurs dont l'epoch local est déjà supérieur.
- Le jeton est **par variable** : il n'existe pas de transaction atomique multi-variables. Un lecteur qui veut plusieurs variables cohérentes « ensemble » doit le gérer lui-même (comparaison d'epochs), ou attendre une éventuelle extension (numéro de lot / séquence).

## Macros de compilation

| Macro | Effet |
|---|---|
| `_ESP_NOW_TR_SHARED_VAR_` | active le fichier (obligatoire, à définir avant l'`#include`) |
| `_DEBUG_ESP_NOW_TR_SHARED_VAR_` | logs Serial (envoi/réception/jeton/CRC) |
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
