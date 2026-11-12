# Tab5 V5.2 — synchroniser la suppression des threads de dessin

[Télécharger directement tab5-after_v5_2.bin](tab5-after_v5_2.bin?raw=true)

Image USB fusionnée pour le **Tab5 d’origine, ILI9881C 720×1280, P4 v0/v1**,
à flasher à **0x0**. L’application du P4 est remplacée pendant le test ;
le C6 n’est pas modifié. Ce n’est pas une image OTA ESPHome.

## Constat

Le Waveshare V5 a terminé 64 cycles avec zéro perte mémoire et 64 mutex
créés/libérés. Ce résultat est conservé.

Le Tab5 V5.1 réussit ses quatre cycles d’échauffement, puis s’arrête au premier
cycle mesuré : **7 tâches au lieu de 6 après 200 ms**, sans mémoire récupérée
pendant cette attente. Cela ne permet plus d’attribuer l’échec à une simple
mesure immédiate après l’arrêt. Le journal ne donne pas le nom de la tâche restante.

## Correction testée

Le code contient une course possible entre deux cœurs :

1. Le propriétaire demande l’arrêt du thread de dessin et appelle sa suppression.
2. Si ce thread est encore exécuté sur l’autre cœur, FreeRTOS place sa structure
   dans la liste de suppression différée.
3. Le thread termine son callback et se suspend. Cette suspension peut retirer
   sa structure de la liste de suppression pour la placer dans la liste des
   tâches suspendues : le thread reste alors présent.

La V5.2 suspend le thread **depuis son propriétaire**, puis attend qu’il ne
soit plus la tâche courante d’aucun cœur avant d’appeler la suppression.
L’API de suppression normale ou `WithCaps` reste celle qui correspond au
mode d’allocation. Le handle est invalidé après suppression.

Cette correction reste conditionnée à ROTATE_IN_DRAW. Les gardes désactivées
conservent le comportement amont. Elle est compatible avec le hook existant
des piles PSRAM, activé avant ou après elle.

La libération du mutex et le découpage du cache sont conservés. Les seuils
restent inchangés : **256 octets cumulés**, contrôle Unity par cas inchangé,
et attente de stabilisation limitée à **200 ms**. Le banc n’augmente aucun
de ces seuils pour obtenir un PASS.

## Essai Tab5

Flasher **tab5-after_v5_2.bin à 0x0 par USB**, puis ouvrir les logs à
**115200 bauds**. Après trois secondes, le test effectue quatre cycles
d’échauffement et **64 cycles mesurés**, en environ une minute.

Le journal doit identifier `PR818_V5_2_READY` et `revision=after_v5_2`.
La fin attendue est :

```text
PR818_MUTEX_SUMMARY created=64 deleted=64 balanced=1
PR818_MEMORY_DONE result=PASS cycles=64
PR818_DONE: memory test complete; save the full log
```

Envoyer le journal complet, y compris en cas de FAIL ou de PR818_ABORT.
Si des tâches restent présentes, le banc cherche aussi les handles des tâches
`swdraw`, `lv_ortho2` et `lvgl` et imprime `PR818_TASK_HANDLE` sans déréférencer
un handle potentiellement récupéré entre-temps.

## Vérifications et limites

Un examen indépendant du noyau FreeRTOS confirme l’ordre de manipulation des
listes à l’origine de cette course possible. Le correctif utilise la même
condition d’absence d’exécution sur les cœurs que le chemin de suppression
synchrone WithCaps du SDK utilisé.

Le test `check_freertos_shutdown.py` compile les vraies fonctions LVGL de
suppression et de retour de callback avec un modèle déterministe d’ordonnancement.
Il force l’enchaînement problématique. L’ancien chemin et une mutation retirant
l’attente reproduisent une tâche orpheline dans ce modèle ; le nouveau chemin
libère une seule fois les ressources. **Ce modèle ne constitue pas une mesure
du matériel ni une émulation complète de FreeRTOS.**

42 programmes compilés couvrent LVGL 9.5/9.6, le hook des piles caps appliqué
avant/après, les gardes activées/désactivées, les cœurs multiples ou uniques,
l’éviction retardée et les handles invalides. Les tests passent aussi avec
les backends déjà patchés. Le backend 9.6 exact utilisé pour la compilation
native a passé ses 21 configurations.

Le Tab5 compile. Le binaire est contrôlé : l’assembleur de `lv_thread_delete`
appelle bien `vTaskSuspend`, consulte les tâches courantes des cœurs puis
appelle `vTaskDelete`. Les segments de flash sont vérifiés.
**La validation physique V5.2 reste attendue.** Le test vise l’arrêt et la
mémoire ; il ne mesure pas les FPS. La PR818 n’est pas modifiée par ce kit.

## Reconstruction

Le manifeste fournit le SHA256 du patch `source/candidate-v5.patch` appliqué
au commit `9db972e7008d4c5adab0fde59dc3b0b1343dac4c`, ainsi que celui du
banc de stabilisation. ESP-IDF 5.5.6, LVGL 9.6.0 et les réglages de V5 sont
conservés. Dans un terminal ESP-IDF, avec les commits des deux dépôts disponibles :

```sh
python source/prepare_v5_2.py --repo /chemin/esp-iot-solution --fixtures /chemin/ppa-alternative- --out /chemin/essai-v5_2 --idf "$IDF_PATH"
cd /chemin/essai-v5_2/components/display/tools/esp_lvgl_adapter/test_apps/lvgl9
idf.py -B /chemin/build-v5_2 -D SDKCONFIG_DEFAULTS=sdkconfig.bench -D IDF_TARGET=esp32p4 build
```

Utiliser un dossier neuf et recopier `tab5-dependencies.lock` dans
l’application avant sa première compilation. Le test du modèle est fourni
dans le patch sous `test_apps/host_ortho/check_freertos_shutdown.py` ; lui
passer `--lvgl /chemin/lvgl` pour utiliser des sources locales, sans téléchargement.
Aucun script n’est nécessaire sous Windows pour flasher le binaire.
