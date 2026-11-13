# Traces de nettoyage V8.1 — Waveshare officiel

Ce diagnostic conserve les scènes, la trace, les opérations chronométrées, les assertions et les formats `AB_READY`, `AB_RESULT`, `AB_TIMING`, `AB_REFRESH`, `AB_COST` et `AB_DONE` de V8. Il ajoute seulement des jalons après les mesures pour déterminer où la fin du programme s’arrête.

Le patch refuse toute source autre que le `ab_main.c` V8 épinglé et refuse une seconde application. Il travaille sur une copie privée ; il ne modifie ni le pilote officiel, ni LVGL, ni le SDK partagé.

```sh
python patch_cleanup_trace.py --source /path/to/original-v8/ab_main.c --out /path/to/private-app/main/ab_main.c
```

L’original correspond au commit `c80e12920a622d5102ef66fb80ebd268d079d847`, chemin `downloads/pr818-diagnostics-v8/fixtures/ab_main.c` dans `youkorr/ppa-alternative-`.

## Interprétation

Chaque opération de nettoyage reçoit un marqueur `AB_CLEANUP` avant (`edge=begin`) et après (`edge=end`) son appel. Les derniers jalons concernent la libération des quatre banques d’images, puis l’émission du vrai `AB_DONE`. Aucun faux marqueur de fin n’est ajouté avant la libération des ressources.

| `stage_id` | Opération entourée |
|---:|---|
| 1 | Création de l’observateur final |
| 2 | Assertions de cohérence du passage |
| 3 | Libération des échantillons de temps |
| 4 | Libération de la copie des compteurs |
| 5 | Acquisition du verrou LVGL |
| 6 | `lv_obj_clean` |
| 7 | `lv_image_cache_drop` |
| 8 | `cache_budget(0)` ; le profil officiel vérifie seulement que le budget vaut zéro |
| 9 | Pause du timer d’affichage |
| 10 | Libération du verrou LVGL |
| 11 | Retour de la fin de nettoyage du passage |
| 20 | Libération d’une banque source, précisée par `bank=0..3` |
| 21 | Émission de `AB_DONE` |

Après le dernier `AB_COST` de la dernière phase officielle, une tâche d’observation de priorité 6, sans affinité de cœur, surveille les jalons. Si le même jalon reste inchangé au moins cinq secondes, elle émet `AB_CLEANUP_WAIT` puis le répète toutes les cinq secondes. Dans cette ligne numérique, `edge_id=0` signifie avant l’opération, `edge_id=1` après.

Cette sortie utilise uniquement l’aide d’impression `panic_print_str` du SDK 5.5.6 : elle écrit directement sur l’UART et la console USB Serial/JTAG secondaire, sans entrer dans le gestionnaire de panique ni prendre le verrou `ESP_LOG`. Un vidage explicite envoie les courts paquets USB. Son délai USB est borné par l’implémentation SDK. Un petit `snprintf` local formate seulement des entiers, sans sortie standard.

Le mot `panic` est le nom de l’aide d’impression du SDK : ce diagnostic ne déclenche ni panique, ni redémarrage, ni déverrouillage forcé. Il ne supprime aucune libération et ne réessaie pas les opérations. Si la création de l’observateur échoue, les jalons normaux restent actifs et le programme poursuit son nettoyage original.

Après le retour du journal `AB_DONE`, `AB_CLEANUP_COMPLETE` confirme le passage par le chemin de sortie directe. Le parseur V8 ignore les nouveaux tags et exige toujours le vrai `AB_DONE`.

## Limites et validation

Les jalons se trouvent hors des fenêtres chronométrées. Ils peuvent toutefois modifier l’ordonnancement du nettoyage ; ce firmware localise un arrêt, il ne sert pas à annoncer une nouvelle amélioration de performances. La surveillance est une tâche de diagnostic : un blocage global du processeur ou des interruptions peut aussi empêcher sa sortie. Le dernier jalon identifie un point atteint, pas à lui seul une preuve contre une bibliothèque : l’écriture du marqueur peut également être concernée par un défaut de transport.

`structural-checks.json` vérifie notamment les parties chronométrées identiques, les formats existants identiques, l’ordre original des appels de nettoyage, l’armement seulement après le dernier passage, et l’absence de journalisation/allocation/opération LVGL sous le petit mutex de l’observateur. La compilation syntaxique avec le vrai compilateur P4 et `-Werror` réussit sans diagnostic (`syntax-check.log`). La compilation et la liaison complètes du firmware restent sous la responsabilité du pilote de construction du kit.
