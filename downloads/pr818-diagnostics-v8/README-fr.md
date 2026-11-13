# Kit V8 : localiser les copies et les attentes sur ESP32-P4

Ce kit ajoute des mesures au moteur testé en V7. Il compare automatiquement, dans chaque firmware, le parcours actuel (`legacy`) et le même parcours avec le timer d’affichage strictement contrôlé (`strict`). Les autres timers LVGL restent actifs. Les résultats matériels V8 sont encore à mesurer.

Deux firmwares par carte suffisent : officiel à deux unités de dessin et notre moteur avec des lots de 64. Le réglage 128 n’a pas montré de gain utile dans les essais V7. Les sources du moteur restent celles du kit V7 ; les changements V8 portent sur le programme de diagnostic et ses sondes.

## Téléchargements directs

| Carte | Référence officielle | Notre moteur |
|---|---|---|
| Tab5 | [Officiel — .bin](https://github.com/youkorr/ppa-alternative-/raw/refs/heads/test/pr818-card-firmwares-20261112/downloads/pr818-diagnostics-v8/results/tab5/official/tab5-diagnostics-v8-official.bin) | [Native64 — .bin](https://github.com/youkorr/ppa-alternative-/raw/refs/heads/test/pr818-card-firmwares-20261112/downloads/pr818-diagnostics-v8/results/tab5/native64/tab5-diagnostics-v8-native64.bin) |
| Waveshare 7B | [Officiel — .bin](https://github.com/youkorr/ppa-alternative-/raw/refs/heads/test/pr818-card-firmwares-20261112/downloads/pr818-diagnostics-v8/results/waveshare-7b/official/waveshare-7b-diagnostics-v8-official.bin) | [Native64 — .bin](https://github.com/youkorr/ppa-alternative-/raw/refs/heads/test/pr818-card-firmwares-20261112/downloads/pr818-diagnostics-v8/results/waveshare-7b/native64/waveshare-7b-diagnostics-v8-native64.bin) |

## Essai

1. Gardez votre firmware habituel pour le réinstaller ensuite : ce programme autonome remplace temporairement l’application.
2. Flashez le `.bin` de votre carte à l’adresse **0x0**, avec l’outil habituel.
3. Ouvrez le journal série à **115200 bauds**, puis redémarrez. Le test démarre automatiquement après trois secondes.
4. Laissez les deux modes s’exécuter jusqu’à **`AB_DONE`**. Comptez environ deux à quatre minutes par firmware selon la carte. Vérifiez aussi l’apparence des formes et des images.
5. Exportez le journal complet, depuis `AB_READY` jusqu’à `AB_DONE`, en conservant toutes les lignes `AB_RESULT`, `AB_TIMING`, `AB_REFRESH` et `AB_COST`. Répétez avec l’autre firmware de la carte.

Chaque scène comprend 16 images d’échauffement, puis 64 images mesurées. Deux passages sont exécutés ; l’ordre des modes est inversé au second passage. Les trois scènes communes sont l’interface animée, les images changeantes en PSRAM et le fond fixe en flash sans cache. Le moteur natif ajoute une scène distincte avec cache de fond fixe.

## Ce que les journaux permettront de distinguer

- `AB_TIMING` : acquisition du verrou, mise à jour de la scène, rafraîchissement manuel, déverrouillage et pauses, avec le temps résiduel comptabilisé séparément.
- `AB_REFRESH` : rafraîchissements manuels et automatiques. Le mode strict doit compter 64 rafraîchissements manuels et aucun automatique par passage.
- `AB_COST` : appels de copie observés, synchronisations de cache, soumissions PPA et attentes natives. `pending` distingue les tickets encore en attente des simples appels à la fonction d’attente.
- `AB_RESULT` : débit des générations d’images sélectionnées pour le balayage DMA, temps de rafraîchissement, mémoire et contrôles d’intégrité.

Les temps des catégories sont **inclusifs et peuvent se recouvrir** : il ne faut pas les additionner. Les compteurs de copies portent sur la charge demandée ; ceux du cache portent sur les plages synchronisées. Ils ne mesurent pas tout le trafic physique de la mémoire. Les sondes ont elles-mêmes un coût : ce kit sert à localiser les dépenses, puis un test de performance sans cette instrumentation devra confirmer le gain d’un éventuel correctif.

Le compteur DMA ne constitue pas une mesure optique de la dalle. Les images changeantes sont synthétiques ; elles ne mesurent pas les FPS d’un capteur caméra. Le bénéfice du cache de fond fixe reste séparé de celui des images qui changent.

## Analyse des captures

```sh
python fixtures/parse_logs.py officiel.txt native64.txt --out diagnostic-v8.json
```

Le parseur vérifie les deux modes, chaque scène et passage, la cohérence des temps, les compteurs d’affichage et l’absence d’erreur. Il compare les moteurs uniquement pour une même carte, scène et mode. Il calcule aussi l’écart `strict`/`legacy` dans chaque firmware. La durée et le parcours mesurés diffèrent de V7 : gardez les analyses des deux kits séparées.
