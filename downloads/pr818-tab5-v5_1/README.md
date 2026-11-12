# Tab5 V5.1 — attendre la récupération des tâches avant de mesurer le tas

[Télécharger directement tab5-after_v5_1.bin](tab5-after_v5_1.bin?raw=true)

Image USB fusionnée pour le **Tab5 d’origine, ILI9881C 720×1280, P4 v0/v1**,
à flasher à **0x0**. Elle remplace l’application du P4 pendant le test ; le C6
n’est pas modifié. Ce n’est pas une image OTA ESPHome.

## Résultat des deux essais V5

**Waveshare V5 : 64 cycles PASS**, pertes totale et interne nulles sur les
64 relevés, 64 créations et 64 libérations du mutex. Chaque libération rend
88 octets au tas. Aucun nouvel essai Waveshare n’est nécessaire pour ce point.

**Tab5 V5 : essai incomplet.** Le premier cycle mesure une perte nulle et
une libération de 88 octets. Au deuxième, le contrôle Unity voit 33 128 octets
encore occupés et interrompt la suite avant le relevé cumulé. Ce n’est pas
un panic du processeur ; le banc atteint `PR818_ABORT` et son menu.

La configuration réserve 32 768 octets à chaque tâche de dessin LVGL.
FreeRTOS peut différer la récupération d’une tâche supprimée sur l’autre
cœur jusqu’au passage d’une tâche idle. Le montant observé est compatible
avec une pile et ses structures de contrôle, mais le journal V5 ne prouve
pas à lui seul que cette mémoire sera récupérée ensuite.

## Ce que change V5.1

L’attente de la boucle V5 venait **après** le contrôle mémoire de Unity.
V5.1 attend **avant la mesure dans `tearDown()`** : le nombre de tâches doit
revenir au niveau d’avant le test pendant deux observations séparées de 10 ms,
avec une limite de 200 ms. Cela laisse aussi le temps de terminer la libération
après le retrait d’une tâche de la liste d’attente.

Le banc imprime `PR818_TASK_SETTLE` avec le temps d’attente, le nombre de
tâches attendu/observé et la mémoire récupérée pendant cette attente.

**Les seuils ne sont pas augmentés** : le contrôle Unity conserve 2 000 octets
par cas, le bilan des 64 cycles conserve **256 octets cumulés au maximum**.
Une vraie perte de 33 128 octets qui persiste après l’attente échoue toujours.
Une fuite de 88 octets par cycle reste détectée par le bilan cumulé.
Une tâche qui ne disparaît pas dans les 200 ms provoque aussi un échec.

Le rendu, la libération du mutex, le découpage du cache et le pilote PPA sont
**identiques à V5**. Il s’agit d’une correction de l’ordre du contrôle mémoire
dans le banc, sans nouvelle optimisation de rendu ni contournement du pilote.

## Essai Tab5 seulement

Flasher **tab5-after_v5_1.bin à 0x0 par USB**, puis ouvrir le journal à
**115200 bauds**. Après trois secondes, le banc effectue quatre cycles
d’échauffement et 64 cycles mesurés. Compter environ une minute.

Le journal doit identifier `revision=after_v5_1`. Conserver les lignes
`PR818_TASK_SETTLE` et `PR818_MEMORY`, ainsi que la fin :

```text
PR818_MUTEX_SUMMARY created=64 deleted=64 balanced=1
PR818_MEMORY_DONE result=PASS cycles=64
PR818_DONE: memory test complete; save the full log
```

Si `FAIL` ou `PR818_ABORT` apparaît, garder aussi le journal complet.
Les lignes `reclaimed` permettront de vérifier l’hypothèse de récupération
différée ; une valeur nulle n’est pas une erreur si la mémoire était déjà libérée.

## Vérifications et provenance

Le vrai code `tearDown()` généré est compilé sur ordinateur avec plusieurs
résultats injectés : récupération différée, perte persistante de 33 128 octets,
perte de 88 octets conservée, tâche restant présente, deux observations stables
et débordement du compteur de ticks. Tous les cas passent. Retirer l’attente
fait échouer le cas de récupération différée.

Le programme Tab5 est compilé et ses segments de flash sont contrôlés.
**Le Tab5 V5.1 s’arrête : 7 tâches au lieu de 6 après 200 ms, sans récupération observée pendant cette attente.** La suite est le [test Tab5 V5.2](../pr818-tab5-v5_2/README.md), qui synchronise l’arrêt du thread avant sa suppression. La validation physique de V5.2 reste attendue. Le résultat du
Waveshare V5 reste conservé. Aucun nouveau correctif n’est publié dans la
PR818 par ce kit.

La base de rendu et son patch sont les mêmes qu’en V5 : commit
`9db972e7008d4c5adab0fde59dc3b0b1343dac4c`, puis `source/candidate-v5.patch`.
Le manifeste fournit aussi le SHA256 du nouveau `source/task_settle.h`.
Les fichiers de configuration et dépendances résolues sont fournis.

Pour reconstruire dans un terminal ESP-IDF 5.5.6, dans un dossier neuf :

```sh
python source/prepare_v5_1.py --repo /chemin/esp-iot-solution --fixtures /chemin/ppa-alternative- --out /chemin/essai-v5_1 --idf "$IDF_PATH"
cd /chemin/essai-v5_1/components/display/tools/esp_lvgl_adapter/test_apps/lvgl9
idf.py -B /chemin/build-v5_1 -D SDKCONFIG_DEFAULTS=sdkconfig.bench -D IDF_TARGET=esp32p4 build
```

Recopier le `tab5-dependencies.lock` fourni dans l’application avant sa
première compilation pour figer les versions. Aucun script n’est nécessaire
sous Windows pour flasher le binaire ou recueillir le journal. Garder la
protection Windows active ; les SHA256 vérifient l’intégrité des fichiers.
