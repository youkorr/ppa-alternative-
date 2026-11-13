# Kit V7 : comparaison de performances ESP32-P4

Ce programme autonome compare l’adaptateur officiel Espressif **0.7.8** à notre moteur intégré sur cette même version. Les performances matérielles restent à mesurer sur vos cartes. Les essais V6 ont déjà validé les transformations visuelles ; ils ne mesuraient pas la vitesse.

| Variante | Chemin d’affichage | Buffers panneau | Dessin logiciel |
|---|---|---:|---:|
| `official` | Officiel, TRIPLE_PARTIAL | 3 | 2 unités |
| `official_ppa1` | Officiel, TRIPLE_PARTIAL, PPA activé | 3 | 1 unité requise par ce chemin |
| `native64` | Notre moteur, rotation pendant le dessin | 3 | 2 unités, lots de 64 |
| `native128` | Même moteur, lots agrandis à tester | 3 | 2 unités, lots de 128 |

Même CPU à 360 MHz, PSRAM à 200 MHz, cache L2 de 128 Kio, LVGL 9.6.0~1, ESP-IDF 5.5.6 et séquence d’images. Les méthodes d’affichage sont volontairement différentes : c’est leur efficacité que l’on compare. Pour chaque scène, le rapport retient la meilleure des deux références officielles.

## Téléchargements directs

| Carte | Officiel | Officiel avec PPA | Notre moteur | Notre réglage optimisé |
|---|---|---|---|---|
| Tab5 | [A — .bin](https://github.com/youkorr/ppa-alternative-/raw/refs/heads/test/pr818-card-firmwares-20261112/downloads/pr818-performance-v7/results/tab5/official/tab5-ab-official.bin) | [B — .bin](https://github.com/youkorr/ppa-alternative-/raw/refs/heads/test/pr818-card-firmwares-20261112/downloads/pr818-performance-v7/results/tab5/official_ppa1/tab5-ab-official_ppa1.bin) | [C — .bin](https://github.com/youkorr/ppa-alternative-/raw/refs/heads/test/pr818-card-firmwares-20261112/downloads/pr818-performance-v7/results/tab5/native64/tab5-ab-native64.bin) | [D — .bin](https://github.com/youkorr/ppa-alternative-/raw/refs/heads/test/pr818-card-firmwares-20261112/downloads/pr818-performance-v7/results/tab5/native128/tab5-ab-native128.bin) |
| Waveshare 7B | [A — .bin](https://github.com/youkorr/ppa-alternative-/raw/refs/heads/test/pr818-card-firmwares-20261112/downloads/pr818-performance-v7/results/waveshare-7b/official/waveshare-7b-ab-official.bin) | [B — .bin](https://github.com/youkorr/ppa-alternative-/raw/refs/heads/test/pr818-card-firmwares-20261112/downloads/pr818-performance-v7/results/waveshare-7b/official_ppa1/waveshare-7b-ab-official_ppa1.bin) | [C — .bin](https://github.com/youkorr/ppa-alternative-/raw/refs/heads/test/pr818-card-firmwares-20261112/downloads/pr818-performance-v7/results/waveshare-7b/native64/waveshare-7b-ab-native64.bin) | [D — .bin](https://github.com/youkorr/ppa-alternative-/raw/refs/heads/test/pr818-card-firmwares-20261112/downloads/pr818-performance-v7/results/waveshare-7b/native128/waveshare-7b-ab-native128.bin) |

## Essai

1. Gardez votre firmware habituel pour le remettre après l’essai : ces fichiers remplacent temporairement l’application.
2. Choisissez les fichiers de **votre carte** dans `results/tab5/` ou `results/waveshare-7b/`. Flashez chaque `.bin` fusionné à l’adresse **0x0**, avec l’outil habituel.
3. Ouvrez le journal série à **115200 bauds**, puis redémarrez la carte. Le test démarre automatiquement après trois secondes. Comptez environ 1,5 à 3 minutes par firmware selon les performances.
4. Sauvegardez le journal complet, depuis `AB_READY` jusqu’à `AB_DONE`. Vérifiez que l’écran dessine correctement les trois cartes animées, puis les images plein écran. Signalez tout défaut visuel.
5. Répétez avec les quatre variantes et transmettez les quatre journaux, en précisant la carte. Commencez par une seule carte si vous préférez.

Les trois scènes communes sont une interface animée avec ses zones réellement modifiées, quatre images changeantes en PSRAM, et un fond fixe en flash avec le cache désactivé. Chaque scène est répétée trois fois. Les images changeantes sont synthétiques : elles isolent le moteur d’affichage et ne mesurent pas les FPS d’un capteur caméra.

Notre moteur ajoute un essai séparé `fixed_flash_warm_cache`. Son gain éventuel ne doit pas être attribué à la vidéo changeante.

Le journal mesure les rafraîchissements soumis et les générations d’images distinctes sélectionnées pour le balayage DMA. Il ne compte pas simplement les VSYNC et ne constitue pas une mesure optique de la dalle. Il inclut les temps médian/p95, la mémoire et les images remplacées avant leur sélection.

## Sources et analyse

Sources officielles : commit `da53cfaa90b63085ef21f721f7593b5e2747d448` d’`espressif/esp-iot-solution`. Le candidat intégré est reconstruit avec `integration-on-0.7.8.patch`, sans modifier la PR818 pendant cette comparaison.

Les sources du test, la sonde identique entre variantes et les pilotes des deux cartes sont fournis. Voir [portable/REPRODUCE.md](portable/REPRODUCE.md) pour reconstruire les programmes, et [fixtures/README.md](fixtures/README.md) pour le protocole détaillé. Chaque répertoire de résultat contient le SHA256 du binaire, les réglages compilés et les versions de dépendances.

```sh
python fixtures/parse_logs.py official.txt official_ppa1.txt native64.txt native128.txt --out comparaison.json
```

Le réglage de lots à 128 réduit d’environ 30 % les pixels transférés sur deux scènes de stress sur ordinateur, sans différence d’image dans ces tests. **Ce chiffre n’est pas un gain en FPS mesuré sur ESP32-P4.** Le kit permet de vérifier si ce réglage améliore effectivement la fluidité et à quel coût mémoire.
