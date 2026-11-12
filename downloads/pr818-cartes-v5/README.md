# V5 — test court de la mémoire à l’arrêt de LVGL

Deux images USB fusionnées, à flasher à **0x0** sur le P4 avec le même outil
que précédemment. Elles remplacent l’application du P4 pendant le test ; le C6
n’est pas modifié. Aucun script n’est nécessaire sous Windows.

- [Tab5 d’origine, ILI9881C 720×1280](tab5-after_v5.bin?raw=true)
- [Waveshare 7B, EK79007 1024×600](waveshare-7b-after_v5.bin?raw=true)

Ces images ciblent les ESP32-P4 v0.x/v1.x jusqu’à v1.99.

## Modification

La V4 a passé le test du cache sur Tab5, mais son journal perd 1 420 octets
sur 16 créations/destructions de l’écran, généralement 88 octets par cycle.
Les seuils de 2 000 octets **par cas** laissaient passer cette accumulation.

LVGL 9.5/9.6 crée son mutex global dans `lv_os_init()` sans libération
correspondante dans `lv_deinit()`. La V5 libère ce mutex après l’arrêt des
utilisateurs et avant la destruction de l’état mémoire LVGL. Le patch reste
conditionné au mode ROTATE_IN_DRAW ; il reconnaît une libération amont pour
éviter de l’ajouter deux fois.

Le découpage de préparation du cache de V4 est conservé : blocs de 128 Kio
maximum et délai de 250 ms. La V5 utilise le pilote ESP-IDF d’origine.
Elle ne modifie pas la résolution et n’ajoute pas de réglage de performance.

## Essai demandé

Flasher le fichier correspondant à la carte à **0x0**. Ouvrir le journal
à **115200 bauds**, puis redémarrer. Le test commence après trois secondes.
Il effectue un démarrage initial et quatre cycles d’échauffement, puis
**64 cycles mesurés**. Compter environ une minute par carte.

Le long benchmark des images n’est pas lancé automatiquement. Le code de
rendu est conservé, mais ce test ne fournit pas une nouvelle mesure de FPS.

Chaque cycle donne une ligne `PR818_MEMORY` :

- `lost` et `internal_lost` : pertes depuis le début des 64 cycles ;
- `mutex_init` / `mutex_delete` : créations et libérations du mutex global
  pendant ce cycle, attendues à **1 / 1** ;
- `payload` : taille de l’allocation observée pour ce mutex ;
- `returned` : mémoire interne rendue autour de sa libération. Cette valeur
  peut varier si une autre tâche libère de la mémoire au même instant.

Pour réussir, il faut les 64 cycles, 64 créations et 64 libérations observées,
une intégrité du tas correcte, et une perte cumulée maximale de **256 octets**
dans chacun des deux relevés de mémoire. Les régions se recouvrent : leurs
pertes ne s’additionnent pas. L’ancienne perte de 88 octets par cycle
atteindrait 5 632 octets et serait donc détectée.

La fin attendue est :

```text
PR818_MEMORY_SUMMARY ... max_lost=... internal_max=... budget=256 heap_ok=1
PR818_MUTEX_SUMMARY created=64 deleted=64 balanced=1
PR818_MEMORY_DONE result=PASS cycles=64
PR818_RESTART_OK count=64
PR818_DONE: memory test complete; save the full log
```

Les valeurs `max_lost` et `internal_max` doivent être inférieures ou égales
à 256. Des messages Unity PASS isolés ne suffisent pas. En cas d’échec,
le banc imprime `PR818_ABORT` ou `result=FAIL` sans annoncer de réussite.
Envoyer le journal complet de chaque carte, quelle que soit son issue.

## Vérifications et limites

Le correctif est testé avec les vraies sources LVGL 9.5/9.6 sur ordinateur :
342 cas sans OS et 353 avec des threads. Les compteurs de mutex doivent être
équilibrés à l’arrêt. Retirer la libération du mutex global fait échouer le test.
Les régressions de rendu et les six mutations sont contrôlées ; les tests
LVGL 9.6 passent aussi avec AddressSanitizer/UndefinedBehaviorSanitizer.

Le programme réel du banc mémoire est également compilé sur ordinateur avec
des résultats injectés : fonctionnement normal, absence de libération, autre
fuite de 88 octets malgré des mutex équilibrés, tas corrompu, échec Unity et
petite variation bornée. Retirer le seuil cumulé doit être détecté.

Les images sont compilées pour les cartes et leurs segments de flash sont
contrôlés. **La validation physique de V5 reste attendue.** Ce kit de test ne
publie pas encore les nouveaux correctifs dans la PR818.

## Provenance et reconstruction

Base : PR818 au commit `9db972e7008d4c5adab0fde59dc3b0b1343dac4c`, plus
`source/candidate-v5.patch` dont le SHA256 figure dans chaque manifeste.
Cartes : adaptations au commit `6cf0eed1277032489fd0281890b9a326c93f063f`
de `ppa-alternative-`. ESP-IDF 5.5.6, LVGL 9.6.0, CPU 360 MHz, PSRAM 200 MHz,
cache L2 128 Kio, deux cœurs de rendu, trois buffers, RGB565.
Les sources et données flash restent en flash ; le test n’utilise pas de caméra.

Dans un terminal ESP-IDF 5.5.6, avec ces commits disponibles localement :

```sh
python source/prepare_v5.py --repo /chemin/esp-iot-solution --fixtures /chemin/ppa-alternative- --out /chemin/essai-v5 --idf "$IDF_PATH" --board tab5
cd /chemin/essai-v5/components/display/tools/esp_lvgl_adapter/test_apps/lvgl9
idf.py -B /chemin/build-v5 -D SDKCONFIG_DEFAULTS=sdkconfig.bench -D IDF_TARGET=esp32p4 build
```

Utiliser un dossier neuf et recopier le `dependencies.lock` de la carte dans
l’application avant la première compilation. `--board waveshare-7b` prépare
l’autre carte. Le script ne modifie pas le dépôt ni le SDK partagés.
Les manifestes, configurations réelles et SHA256 sont fournis. Une somme SHA256
vérifie l’intégrité d’un fichier ; elle ne constitue pas un verdict antivirus.
