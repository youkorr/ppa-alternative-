# Waveshare 7B — firmware d’essai V6

Le défaut de routage des rotations et zooms de style est corrigé dans cette
version d’essai. LVGL conserve la rotation matricielle de l’écran ; les objets
transformés passent par ses calques logiciels. Le choix est propre à chaque écran
et désactivé par défaut pour préserver les moteurs capables de traiter leurs matrices.

## Téléchargement et flash

Fichier direct, sans archive ni outil Windows à exécuter :
[waveshare-7b-transforms-v6.bin](waveshare-7b-transforms-v6.bin?raw=true).

Il cible la **Waveshare ESP32-P4-WIFI6-Touch-LCD-7B, écran EK79007 1024×600**,
flash 32 Mo. Cette image remplace l’application ESPHome pendant l’essai ; conservez
votre firmware habituel pour remettre ensuite votre configuration.

1. Flasher l’image fusionnée sur le **P4 à l’adresse `0x0`**, avec votre outil USB habituel.
   Réglages si demandés : **ESP32-P4, DIO, 40 MHz, 32 MB**.
2. Ouvrir le journal série à **115200 bauds**, puis redémarrer.
3. Attendre environ **trois minutes**. Le test affiche 16 cas avec les rotations
   écran 180/90/270/0°, puis recommence : 128 étapes au total.
4. Comparer les deux images : **SOFTWARE REFERENCE** à gauche,
   **LIVE LVGL** à droite. Les formes, angles et tailles doivent correspondre.
   À 90/270°, l’interface entière tourne ; le dernier cas reste affiché à 180°.

Le test n’utilise ni caméra, ni Wi-Fi, ni tactile. Il ne nécessite pas de modification du C6.

Envoyer le journal complet depuis `PR818_V6_READY` jusqu’à :

```text
PR818_V6_DONE completed=128 expected=128 heap_ok=1 pixel_equality=VISUAL_CHECK_REQUIRED
```

La ligne de fin valide le déroulement et l’intégrité du tas ; elle **ne remplace pas
la comparaison visuelle**. Signaler aussi une image différente, un gel ou un redémarrage.

## Vérifications avant distribution

- Image réellement compilée : ESP-IDF **5.5.6**, LVGL **9.6.0**, deux cœurs de rendu,
  CPU 360 MHz, PSRAM 200 MHz, cache L2 128 Kio, rendu DIRECT.
- Même LVGL **9.6.0** pour les références logicielles embarquées ; elles ont été
  rendues indépendamment avec les transformations matricielles d’objets désactivées.
- **384/384 rendus DIRECT exactement identiques** à la référence sur ordinateur,
  en combinant 16 scènes, quatre rotations écran et RGB565/XRGB8888,
  sur CPU, avec pthread et avec faux PPA.
- **1 536 rendus** au total sur LVGL 9.5/9.6 : aucun plantage. Les comparaisons
  PARTIAL peuvent conserver des différences d’arrondi ; sur LVGL 9.6, deux cas
  dépassent ±1 niveau sur un seul pixel (2/255). Ce firmware utilise DIRECT.
- Anciennes régressions LVGL 9.6 : **342/342** sur CPU, **353/353** avec pthread.
  Mutex équilibrés et libération du cache vérifiés.
- Option par écran, valeur initiale false, indépendance de deux écrans, suppression
  et recréation vérifiées. Patch idempotent et protection des caches hardlinkés testés.
- Les références RGB565 réellement intégrées dans l’ELF ont été relues et comparées
  aux fichiers sources. Les segments bootloader, partitions et application ont été
  vérifiés octet par octet dans l’image fusionnée ; checksums d’images valides.
- Reconstruction du source distribuée testée : 12 fichiers source/asset compilés
  reproduits à l’identique. Détails dans les manifestes.

**La validation physique de cette V6 reste à faire sur votre Waveshare.**
Ces tests ne mesurent pas un gain de FPS sur carte. Le skew et les matrices
personnalisées ne deviennent pas pris en charge par le moteur logiciel avec ce correctif.
Le coût RAM/temps des calques transformés doit également être observé sur la carte.

## Sources et reconstruction

Base PR818 : `9db972e7008d4c5adab0fde59dc3b0b1343dac4c`, plus
`source/candidate-v6.patch`, qui conserve les correctifs de cache, de mutex et
de destruction des tâches de V5.2. Fixtures cartes :
`6cf0eed1277032489fd0281890b9a326c93f063f` de `youkorr/ppa-alternative-`.
Ce kit est publié sur la branche de test ; `main` et la PR818 ne sont pas modifiés.

Depuis un environnement ESP-IDF 5.5.6, avec ces commits dans les dépôts locaux :

```sh
python source/prepare_v6.py --repo /chemin/esp-iot-solution --fixtures /chemin/ppa-alternative- --out /chemin/essai-v6
cd /chemin/essai-v6/components/display/tools/esp_lvgl_adapter/test_apps/lvgl9
idf.py -B /chemin/build-v6 -D SDKCONFIG_DEFAULTS=sdkconfig.bench -D IDF_TARGET=esp32p4 build
```

Utiliser un dossier de sortie vide. `source/oracle-base64.json` contient les 16 cadres
logiciels figés nécessaires aux références embarquées ; le SDK complet n’est
pas livré. Les scripts sont réservés à la reconstruction, inutiles pour flasher le `.bin` fourni.
Les manifestes documentent les contrôles du binaire compilé et des sources.

L’empreinte SHA256 vérifie l’intégrité du fichier, pas un verdict antivirus :

```
66bf6aeb088ba557bd87688386e7a012546f3d5747275d557bf561821ddee765  waveshare-7b-transforms-v6.bin
```
