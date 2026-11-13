# V9 — vérifier le correctif des copies DIRECT vides

Le correctif est maintenant dans le moteur de test : il conserve les zones à
synchroniser lorsqu’un rafraîchissement DIRECT n’a rien à redessiner. La prochaine
vraie mise à jour synchronise les zones nécessaires. Les rafraîchissements
automatiques, leurs événements et les autres timers LVGL restent actifs.

Chaque firmware compare automatiquement deux états du **même moteur** :
`eager` (comportement précédent) et `deferred` (correctif activé). Le parcours,
les sources d’images, les réglages et les sondes sont identiques entre ces deux
états. Contrairement à V8, aucun mode ne bloque le timer de rafraîchissement.
Ce test mesure l’effet du correctif ; il ne fournit pas une nouvelle référence
Espressif à comparer directement aux anciens chiffres V7/V8.

## Deux fichiers seulement

| Carte | Firmware V9 |
|---|---|
| Tab5 | [Télécharger le .bin](https://github.com/youkorr/ppa-alternative-/raw/refs/heads/test/pr818-card-firmwares-20261112/downloads/pr818-empty-sync-v9/results/tab5/native64/tab5-empty-sync-v9-native64.bin) |
| Waveshare 7B | [Télécharger le .bin](https://github.com/youkorr/ppa-alternative-/raw/refs/heads/test/pr818-card-firmwares-20261112/downloads/pr818-empty-sync-v9/results/waveshare-7b/native64/waveshare-7b-empty-sync-v9-native64.bin) |

1. Gardez votre firmware habituel pour le remettre après ce test autonome.
2. Flashez le fichier de votre carte à **0x0**.
3. Ouvrez le journal série à **115200 bauds**, puis redémarrez.
4. Laissez les deux modes aller jusqu’à **AB_DONE** : environ deux à quatre minutes selon la carte.
5. Vérifiez visuellement les cartes animées et les images : signalez toute zone ancienne qui reste affichée, scintillement ou forme incorrecte.
6. Transmettez le journal complet, avec toutes les lignes AB_RESULT, AB_TIMING, AB_REFRESH et AB_COST.

Le journal indique `sync_mode=eager/deferred` et `defer_empty_sync=0/1`. Les
compteurs de rafraîchissements automatiques restent autorisés dans les deux
modes. Le résultat attendu est une baisse des copies de fond sans suppression
des actualisations nécessaires ; le gain matériel reste à mesurer.

Le test utilise 16 images d’échauffement, puis 64 images mesurées, deux fois par
mode/scène. L’ordre des modes est inversé au second passage. Les images
changeantes en RAM sont synthétiques ; ce n’est pas une mesure du capteur caméra.
Le cache d’images fixes reste une scène séparée. Les temps des catégories sont
inclusifs et peuvent se recouvrir : ne pas les additionner.

## Vérifications déjà réalisées

- LVGL 9.5 et 9.6 : **864 comparaisons d’images exactes chacun**, soit 1728 au total.
- Un, deux et trois buffers LVGL ; quatre rotations ; modes PARTIAL/FULL/DIRECT ; objets imbriqués transformés et mises à jour complètes/partielles.
- Timer automatique et autre timer actifs, attente de transfert asynchrone, politique désactivée par défaut, sélection NULL, isolation des écrans, bascule et recréation.
- Callbacks de synchronisation personnalisés préservés sur LVGL 9.6 ; patch atomique/idempotent et refus d’un code source inattendu.

Ces vérifications couvrent le cœur LVGL. Le chemin réel de l’adaptateur emploie
deux descripteurs LVGL et trois buffers physiques : l’essai sur vos cartes reste
nécessaire pour valider son résultat visuel et les performances réelles.

La source de test `f8ce296abfe2f6cc1fa38fc5ef1928361f1dc3a7` intègre l’adaptateur
officiel 0.7.8 et notre proposition. Elle est reconstruite avec le patch fourni ;
ce commit est un identifiant local, pas un téléchargement Git requis. Les sources,
preuves et commandes de reconstruction se trouvent dans `portable/` et
`host-evidence/`. Le correctif n’est pas encore ajouté à la branche de code de la
PR818 : les résultats récents et leurs limites sont déjà ajoutés à sa description.
