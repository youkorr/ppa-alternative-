# Résultats matériels du premier kit — 12 novembre 2026

Source : quatre journaux transmis par le propriétaire des deux cartes.
Les CSV joints contiennent les lignes de mesures structurées et leurs médianes,
sans publier les journaux de démarrage ni leur contenu brut.

Comparaison de `ba1ed66c` (before) à `7965432e` (after, premier kit).
Trois passages de 8 secondes par scène ; médiane des refreshs/s rendus et soumis
par LVGL. CPU 360 MHz, PSRAM 200 MHz, cache L2 128 Kio, flash DIO 40 MHz,
RGB565, trois buffers du panneau, deux cœurs, code/données flash en flash.

| Scène | Tab5 270° : avant → après | Waveshare 7B 180° : avant → après |
|---|---|---|
| Interface, cache désactivé | 12,068 → 9,900 : **−18,0 %** | 16,392 → 14,406 : **−12,1 %** |
| Fond immuable, cache désactivé → activé après préchauffage | 2,958 → 23,800 : **×8,05** | 4,464 → 38,461 : **×8,62** |
| Frames PSRAM modifiables, cache désactivé | 23,793 → 23,277 : **−2,2 %** | 38,167 → 37,732 : **−1,1 %** |
| Frames PSRAM déjà orientées pour le panneau | 23,738 → 23,249 : **−2,1 %** | 37,318 → 38,181 : **+2,3 %** |

Le fond sans cache donne les mêmes FPS sur les deux versions. Les frames
modifiables restent exclues : zéro hit et zéro allocation de cache. Activer le
budget du cache ne change pratiquement pas la cadence de cette scène mobile.
Le fond fixe tient environ 1,76 Mio sur Tab5 et 1,17 Mio sur Waveshare.

Le gain ×8 concerne ce grand fond **initialement lu en flash**, puis conservé
en PSRAM dans l’orientation du panneau. Il comprend l’effet du changement de
mémoire source ; ce n’est pas une mesure isolée de la seule rotation PPA.
Ces scènes ne capturent pas de caméra et ne mesurent pas le décodage JPEG/H.264.
La cadence est celle des refreshs LVGL, pas un comptage physique des balayages.

## Défauts révélés

Les quatre séquences chronométrées se terminent avec Unity PASS et `failed=0`.
Cela n’efface pas les autres erreurs présentes dans leurs journaux :

- **Interface ralentie** sur les deux cartes. La protection des polices
  sérialisait des paires de tuiles entières. La V2 la remplace par un mutex
  autour de chaque dessin de texte et de la consommation de ses glyphes.
  Les FPS obtenus avec cette modification restent à mesurer sur les cartes.
- **Tab5 : trois erreurs `out.pic_w/h mismatch with out.buffer_size`**, une
  pendant chaque préchauffage du cache. La ligne RGB565 de 720 pixels fait
  1440 octets ; après l’alignement inférieur à 128 octets, la hauteur déclarée
  incluait encore une ligne de remplissage qui ne tenait plus dans la taille
  donnée au pilote. Le rendu récupérait par le CPU. La V2 ajuste la hauteur
  déclarée à la taille réellement annoncée et ajoute le cas exact au test.
- **Waveshare : un plantage pendant l’arrêt initial**, avant la séquence qui
  redémarre ensuite et passe. Le décodage avec un ELF reconstruit de même
  disposition localise `uxListRemove → vTaskDelete → lv_thread_delete →
  lv_draw_sw_delete → lv_draw_deinit → lv_deinit`. Le backend FreeRTOS
  supprimait la tâche au retour du callback, tandis que son propriétaire
  pouvait supprimer le même handle. La V2 conserve la tâche terminée en
  attente de sa suppression par son propriétaire, lorsque ROTATE_IN_DRAW
  est activé.

Les écarts de tas rapportés à l’arrêt sont de 320 à 492 octets, sous le seuil
de 2000 octets prévu par le banc. Ils ne montrent pas un cache de 1–2 Mio retenu.
Ce test court ne démontre pas l’absence de toute fuite à long terme.

## Validation des correctifs V2

Commit : `9db972e7008d4c5adab0fde59dc3b0b1343dac4c`.

- LVGL 9.5 : 339/339 cas simples et 349/349 à deux cœurs.
- LVGL 9.6 : mêmes cas avec AddressSanitizer/UndefinedBehaviorSanitizer.
- Quatre mutations détectées : garde des polices, marge FIFO, géométrie du
  cache avec ligne de remplissage et nettoyage du cache.
- Le hook de fin de thread est vérifié sur les backends LVGL 9.5 et 9.6,
  sans modification lorsque l’option est désactivée, et sans double application.
- Les nouveaux firmwares effectuent les mêmes scènes, puis 16 cycles
  d’initialisation/arrêt. Ces cycles viennent après les mesures pour conserver
  un état initial comparable au premier kit.

Les mesures et la stabilité de la **V2 sur les écrans physiques restent à
revalider**. Les chiffres de ce document décrivent exclusivement le premier kit.
