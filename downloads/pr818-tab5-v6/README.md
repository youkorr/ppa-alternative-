# Tab5 V6 — test des objets transformés

Firmware pour le **M5Stack Tab5 d’origine : écran ILI9881C 720×1280**, présenté
en paysage 1280×720 à 270°, flash 16 Mo. C’est la même correction V6 que celle
testée sur le Waveshare, avec le pilote et les correctifs de stabilité du Tab5 V5.2.

[Télécharger directement tab5-transforms-v6.bin](tab5-transforms-v6.bin?raw=true)

## Essai

1. Flasher l’image fusionnée sur le **P4 à `0x0`**. Réglages si demandés :
   **ESP32-P4, DIO, 40 MHz, 16 MB**.
2. Ouvrir les logs à **115200 bauds**, puis redémarrer.
3. Attendre environ **trois minutes**. Les 16 scènes passent dans les orientations
   270/90/180/0°, deux fois : 128 étapes au total.
4. Comparer **SOFTWARE REFERENCE** à gauche avec **LIVE LVGL** à droite.
   Formes, angles et tailles doivent correspondre. La scène finale reste à 270°.

Cette application de test remplace temporairement ESPHome. Conservez votre
firmware habituel pour restaurer ensuite votre configuration. Ni caméra,
ni Wi-Fi, ni audio, ni tactile ne sont nécessaires. Le C6 n’est pas modifié.

Les logs commencent par `PR818_V6_READY board=tab5`. La fin attendue est :

```text
PR818_V6_DONE completed=128 expected=128 heap_ok=1 pixel_equality=VISUAL_CHECK_REQUIRED
```

Envoyer le journal complet et confirmer si les deux images correspondent.
Cette ligne finale valide le déroulement et les contrôles du tas ; elle ne
remplace pas votre comparaison visuelle.

## Ce qui est corrigé et ce qui est mesuré

Les rotations et zooms appliqués par style aux objets/parents pouvaient être
ignorés quand LVGL utilisait une matrice de tâches non prise en charge.
Un réglage par écran force leur rendu dans des calques logiciels, tout en
conservant la rotation matricielle accélérée de l’écran. Il est désactivé par
défaut pour laisser les autres moteurs utiliser leurs propres capacités.

Les calques consomment de la mémoire et du temps pour les objets transformés.
**Cette V6 valide la compatibilité et la stabilité ; elle ne chiffre pas un gain de vitesse.**
`render_us` comprend le verrouillage, la reconstruction de toute la page et le
rendu. Une pause volontaire d’une seconde sépare les scènes. Il faudrait un
benchmark avant/après à contenu et réglages identiques pour mesurer un gain.
Le skew et les matrices personnalisées ne sont pas couverts par ce test.

## État de validation

- **Waveshare 7B réel** :128/128 étapes,128 contrôles mémoire valides, aucun crash.
  Les 8 nettoyages reviennent exactement à 34 080 503 octets libres,
  532 643 internes et 6 tâches. L’utilisateur confirme que les images correspondent.
  L’essai dure environ 146 secondes et ne constitue pas un test d’endurance.
- **Ordinateur, LVGL 9.6** :384/384 rendus DIRECT exactement identiques aux
  références, sur CPU, pthread et faux PPA. Les 1 536 rendus de transformations
  LVGL 9.5/9.6 terminent sans plantage ; les écarts PARTIAL restants sont documentés
  dans `host-validation-summary.json`. Le firmware utilise DIRECT.
- Régressions historiques :342/342 CPU et353/353 pthread réussies.
- **Tab5 V6 physique : validation attendue.** Le pilote, les broches, les correctifs
  de cache/mutex/tâches proviennent de la version V5.2 déjà testée sur ce Tab5.
- ELF contrôlé : les références LVGL 9.6 embarquées correspondent exactement aux
  fichiers d’origine. Segments de flash, configuration 16 Mo et checksums vérifiés.
- Reconstruction source testée : 18 fichiers identiques au stage compilé.

ESP-IDF 5.5.6, LVGL 9.6.0, CPU 360 MHz, PSRAM 200 MHz, cache L2 128 Kio,
deux cœurs de rendu. Le manifeste détaille les empreintes et les paramètres.

## Reconstruction

Sources de base PR818 : `9db972e7008d4c5adab0fde59dc3b0b1343dac4c`.
Fixtures : `6cf0eed1277032489fd0281890b9a326c93f063f` de `ppa-alternative-`.
Le paquet applique le patch V5.2 puis le delta V6 distinct ; l’API par écran
est installée dans la copie LVGL gérée par l’application. Les caches partagés
hardlinkés sont préservés par remplacement atomique des fichiers patchés.

Dans un terminal ESP-IDF 5.5.6 avec ces commits disponibles :

```sh
python source/prepare_tab5_v6.py --repo /chemin/esp-iot-solution --fixtures /chemin/ppa-alternative- --out /chemin/essai-tab5-v6 --idf "$IDF_PATH"
cd /chemin/essai-tab5-v6/components/display/tools/esp_lvgl_adapter/test_apps/lvgl9
idf.py -B /chemin/build-tab5-v6 -D SDKCONFIG_DEFAULTS=sdkconfig.bench -D IDF_TARGET=esp32p4 build
```

Utiliser une sortie vide. Les scripts sont nécessaires uniquement pour
reconstruire ; aucun script Windows n’est requis pour flasher le `.bin` fourni.

## For Clyde

This test combines object-style rotation/scaling, transformed parents and nested
objects with four screen rotations. A per-display software-layer policy avoids
sending unsupported object matrices to the ordinary software fallback while
retaining matrix-based screen rotation. The policy defaults to false.

The same V6 correction has completed the Waveshare 7B hardware run:128 steps,
no crash, stable post-teardown heap/task counts, and a user-confirmed visual
match with the embedded independent LVGL 9.6 software references. The original
Tab5 build is provided here for further hardware testing. Its hardware result
is pending. These tests establish correctness/stability for the exercised cases,
not an acceleration percentage or video FPS. Visual comparison remains required.

SHA256 du firmware :

```text
13e03aaa8fb54f37aec00a3b652e3e86385b9f70d2f5783fa5a999d47aa2b061  tab5-transforms-v6.bin
```
