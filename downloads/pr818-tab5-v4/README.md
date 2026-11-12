# Tab5 V4 — préparation du cache en petits blocs

Firmware de test pour le **Tab5 d’origine, ILI9881C 720×1280, ESP32-P4 v0/v1**.
Image USB fusionnée à flasher à **0x0**, avec le même outil que précédemment.
Le C6 n’est pas modifié. Ce n’est pas une image OTA ESPHome.

[Télécharger directement tab5-after_v4.bin](tab5-after_v4.bin?raw=true)

## Ce que montre le journal V3

Le banc s’arrête correctement avec `PR818_ABORT`, sans le redémarrage consécutif
à l’échec observé en V2. La préparation du cache échoue toujours : la rotation
complète **1280×720 vers 720×1280** n’a pas terminé au moment où le délai
de 250 ms expire. La rotation précédente **1280×360** avait terminé.

Le contournement d’Espressif activé en V3 ne suffit donc pas. Le journal V3
conserve les compteurs au moment du délai dépassé ; leur répétition huit
secondes plus tard ne prouve pas que le matériel est resté bloqué tout ce temps.
Les deux demi-images sans cache prennent environ 338 ms au total. Une grosse
rotation peut donc être simplement trop longue pour le délai sans notification
de fin. Cette explication reste à confirmer sur la carte.

## Modification testée

La création du cache tourne l’image par morceaux d’au plus **128 Kio**, avec
une attente de fin entre les morceaux, au lieu d’envoyer l’image entière en
une opération. Pour 1280×720 RGB565, les bandes de préparation font
1280×48 pixels, soit **122 880 octets**. Le délai reste **250 ms**.
Le rendu doit ainsi progresser sans que les limites de détection d’un blocage
soient relâchées. Le découpage concerne la création du cache ; il ne réduit
pas la résolution de l’image.

Le binaire fourni utilise **le pilote ESP-IDF 5.5.6 d’origine** (`stock`), sans
le bypass systématique essayé en V3. Les autres réglages sont ceux de V2 :
CPU 360 MHz, PSRAM 200 MHz, cache L2 128 Kio, RGB565, deux cœurs de rendu,
trois buffers et rotation 270°. Le fond fixe est en flash ; aucune caméra
n’est capturée par ce banc (`sensor_capture=0`).

Un éventuel nouveau dépassement imprime aussi le **compteur actuel** de fins
de transfert, en plus de celui mémorisé au délai dépassé. Cela permettra
de distinguer une opération qui termine tard d’une opération toujours inachevée.
L’arrêt automatique après un échec reste actif.

## Test à effectuer

Télécharger **tab5-after_v4.bin**, puis flasher par USB à **0x0**.
Ouvrir le journal série à **115200 bauds**, redémarrer et attendre environ
trois minutes. Le début doit identifier :

```text
PR818_CACHE_PREPARATION max_bytes=131072 watchdog_ms=250
PR818_PPA_WORKAROUND mode=stock
PR818_START board=tab5 revision=after_v4 ...
```

Pour considérer l’essai complet, il faut les **18 mesures avec `failed=0`**, les
**16 tests de cycle de vie PASS**, puis `PR818_RESTART_OK count=16` et
`PR818_DONE`. Si `PR818_ABORT` apparaît, conserver également toutes les lignes
`PR818_PPA_STALL`. Envoyer le journal complet. Aucun script n’est nécessaire
sur Windows. Garder la protection Windows active.

## Vérifications et provenance

- LVGL 9.5 et 9.6 : **342/342** cas sur un cœur et **352/352** sur deux cœurs.
- LVGL 9.6 : mêmes résultats avec AddressSanitizer/UndefinedBehaviorSanitizer.
- Le test de préparation lente simule un transfert complet fini mais prenant
  460 ms. Les petits blocs doivent respecter le délai inchangé et produire
  tous les pixels attendus aux rotations 90/180/270°.
- Les cinq mutations sont détectées : protection du texte, marge FIFO,
  géométrie du cache, préparation en blocs et libération du cache.
- La compilation ESP-IDF et les segments réels de flash sont contrôlés.

**L’exécution sur votre Tab5 reste à valider.** Les temps du simulateur
ne sont pas des mesures du matériel. Aucun gain FPS du Tab5 ne peut être
retenu à partir des phases V2/V3 ayant `failed=1`.

Le code de départ est celui de la PR818 au commit
`9db972e7008d4c5adab0fde59dc3b0b1343dac4c`, avec le patch de test
`source/cache-build-bands.patch` dont le SHA256 figure dans le manifeste.
Cette publication reste sur la branche de test des firmwares ; la PR818
n’est pas mise à jour par ce kit. Les fichiers `manifest.json`, `sdkconfig.txt`,
`dependencies.lock` et `SHA256SUMS` permettent de vérifier la provenance.
Les SHA256 servent à l’intégrité, pas à établir un verdict antivirus.

## Reconstruction

Dans un terminal ESP-IDF **5.5.6**, avec les commits V2 des deux dépôts
présents localement :

```sh
python source/prepare_v4.py --repo /chemin/esp-iot-solution --fixtures /chemin/ppa-alternative- --out /chemin/essai-v4 --idf "$IDF_PATH"
cd /chemin/essai-v4/components/display/tools/esp_lvgl_adapter/test_apps/lvgl9
idf.py -B /chemin/build-v4 -D SDKCONFIG_DEFAULTS=sdkconfig.bench -D IDF_TARGET=esp32p4 build
```

Utiliser un dossier neuf et recopier le `dependencies.lock` fourni dans
l’application avant la première compilation. Le script applique le patch
dans la copie de test, sans modifier le dépôt ni le SDK partagés.
