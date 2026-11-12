# Tab5 V3 — suivi du blocage pendant le test V2

> **Résultat matériel :** V3 atteint `PR818_ABORT` pendant la préparation du cache. Le contournement PPA ne suffit pas. La suite est le [test V4 avec préparation en petits blocs](../pr818-tab5-v4/README.md), à valider sur la carte.

Firmware de test pour le **M5Stack Tab5 d’origine, écran ILI9881C 720×1280**,
ESP32-P4 v0/v1. Il remplace l’application du P4, sans modifier le C6.

## Résultat de votre essai V2

Le test `fixed_on` signale `failed=1`, puis Unity affiche `PPA timeout`.
La cadence de 24,998 fps de cette phase est donc **invalide**.
Le banc continue ensuite et la création de l’écran échoue avec
`No free interrupt inputs for DSI_BRIDGE`, puis `ESP_ERROR_CHECK` provoque
le redémarrage. Ce plantage est différent de la double suppression des tâches
de rendu corrigée dans V2. Le journal ne donne pas la transaction PPA bloquée,
car les diagnostics LVGL étaient désactivés.

Le Waveshare V2 a terminé ses 18 mesures sans blocage PPA et ses 16 cycles
de création/destruction. Son interface est à 16,393 fps (médiane), son fond
fixe en cache à 38,459 fps. Aucun nouveau flash Waveshare n’est nécessaire ici.

## Ce que change cet essai

- Le SDK PPA utilise le contournement fourni par Espressif dans
  `esp_lvgl_adapter/0001-bugfix-ppa-Temporary-fix-for-the-PPA-hang-issue.patch` :
  le bypass de l’ordre des macroblocs est activé systématiquement. La même
  modification est adaptée à ESP-IDF 5.5.6 et appliquée dans un composant
  local à l’application de test, sans modifier le SDK partagé.
- Un échec Unity arrête la suite automatique : aucun cycle supplémentaire
  de création de l’écran n’est lancé après un benchmark en échec.
- Les transactions PPA bloquées sont imprimées par le banc même si
  `LV_USE_LOG` est désactivé. Un échec de création du panneau affiche aussi
  la table des interruptions et retourne une erreur au test.

Le code de rendu reste celui de la PR818 au commit
`9db972e7008d4c5adab0fde59dc3b0b1343dac4c`. **La suppression du blocage PPA
sur votre carte reste à confirmer.** Cet essai change aussi le comportement
du pilote PPA : il ne mesure pas le gain d’une seule modification du cache.
Aucune modification de la PR Espressif n’est publiée par ce kit.

## Test

Télécharger directement **tab5-after_v3.bin**. C’est une image fusionnée à
flasher à l’adresse **0x0**, avec le même outil USB que précédemment.
Ne pas la flasher à 0x10000 et ne pas utiliser le binaire Waveshare sur le Tab5.

Ouvrir le journal série à **115200 bauds**, puis redémarrer. Les mesures
commencent automatiquement après 10 secondes. Attendre environ trois minutes.
En cas de réussite, les lignes attendues sont :

```text
PR818_PPA_WORKAROUND mode=espressif_bypass
PR818_START board=tab5 revision=after_v3 ...
... 18 lignes PR818_RESULT avec failed=0 ...
... 16 tests de cycle de vie PASS ...
PR818_RESTART_OK count=16
PR818_DONE: ...
```

En cas d’erreur, conserver aussi toutes les lignes `PR818_PPA_STALL`,
`PR818_PPA_PHASE`, `PR818_PANEL_ALLOC_FAILED` ou `PR818_ABORT`. Le menu après
`PR818_ABORT` ne signifie pas une réussite. Envoyer le journal complet ; il
n’est pas nécessaire d’exécuter un script sur Windows.

Garder la protection Windows active. Les sommes SHA256 servent à vérifier
l’intégrité du téléchargement ; elles ne constituent pas une analyse antivirus.

## Reconstruction et vérifications

Dans un terminal ESP-IDF **5.5.6**, avec les deux dépôts et les commits V2
présents localement :

```sh
python source/prepare_v3.py --repo /chemin/esp-iot-solution --fixtures /chemin/ppa-alternative- --out /chemin/essai-v3 --idf "$IDF_PATH"
cd /chemin/essai-v3/components/display/tools/esp_lvgl_adapter/test_apps/lvgl9
idf.py -B /chemin/build-v3 -D SDKCONFIG_DEFAULTS=sdkconfig.bench -D IDF_TARGET=esp32p4 build
```

Utiliser un dossier neuf et recopier le fichier `dependencies.lock` fourni
dans l’application avant la première compilation pour figer les versions.
Le manifeste et `sdkconfig.txt` donnent les réglages réellement compilés.
L’option `--ppa-workaround stock` permet un essai diagnostic sans le
contournement du pilote ; le binaire fourni utilise `espressif_bypass`.

Le contrôle du banc a été exécuté sur ordinateur avec des échecs Unity injectés
au benchmark et à plusieurs cycles de redémarrage. Il vérifie l’arrêt immédiat,
l’absence de faux message de réussite et détecte une mutation qui ignore
les échecs. La compilation native et le contrôle des images de flash sont
distincts d’une validation sur l’écran physique, qui reste attendue.
