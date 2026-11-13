# Waveshare 7B — diagnostic de fin de test V8.1

Ce firmware recherche l’étape où le test officiel cesse de progresser après ses mesures. Votre journal V8 contient déjà les douze passages attendus ; l’absence de `AB_DONE` ne permet pas encore d’attribuer le blocage à Espressif.

Il conserve le même adaptateur officiel 0.7.8, LVGL 9.6.0~1, les mêmes réglages et le même parcours V8. Il ajoute des traces avant et après les opérations de nettoyage. Un observateur indépendant est activé après les dernières mesures ; s’il peut encore s’exécuter, il signale l’étape restée immobile plus de cinq secondes. Il ne débloque ni ne recommence aucune opération.

## Un seul fichier à tester

[Télécharger le firmware Waveshare officiel V8.1 (.bin)](https://github.com/youkorr/ppa-alternative-/raw/refs/heads/test/pr818-card-firmwares-20261112/downloads/pr818-cleanup-v8-1/results/waveshare-7b/official/waveshare-7b-cleanup-v8-1-official.bin)

1. Gardez votre firmware habituel pour le réinstaller après ce diagnostic autonome.
2. Flashez ce `.bin` à l’adresse **0x0** sur le Waveshare 7B.
3. Ouvrez le journal série à **115200 bauds**, puis redémarrez. Comptez environ deux minutes pour les mesures, plus les traces.
4. À l’arrêt apparent, laissez le journal ouvert **quinze secondes**. Sauvegardez le journal complet, notamment les dernières lignes `AB_CLEANUP` et tout `AB_CLEANUP_WAIT`.

`stage=... edge=begin` annonce une opération ; `edge=end` confirme son retour. Si le programme reste sur un début d’étape, nous saurons quel appel examiner. Si tout termine, le journal contiendra `AB_DONE` et `AB_CLEANUP_COMPLETE`.

Les étapes couvrent les assertions finales, la libération des petits tableaux de mesure, le verrou LVGL, la suppression des objets, le cache d’images, la pause du timer, le déverrouillage et chacune des quatre banques PSRAM. L’observateur reste un diagnostic au mieux de ses possibilités : un arrêt de l’ordonnanceur peut empêcher tout nouveau message ; la dernière trace avant l’opération reste alors utile.

Les résultats de rendu de ce firmware ne doivent pas servir à annoncer un gain supplémentaire : les traces de nettoyage modifient le déroulement entre les mesures. Ce test sert à localiser la fin manquante. Il ne modifie pas le dépôt source officiel ni la PR818.

Les sources et les empreintes du correctif de diagnostic sont fournies dans `fixtures/`. La préparation reproductible se fait avec `portable/prepare_ab.py --repo /path/esp-iot-solution --out /path/diagnostic --board waveshare-7b --variant official`, sous ESP-IDF 5.5.6. Utilisez un répertoire privé neuf et les paramètres de compilation décrits dans `portable/REPRODUCE.md`.
