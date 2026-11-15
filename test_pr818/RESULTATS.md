# PR 818 contre l'adaptateur d'Espressif : ce que disent les journaux

Dépouillement des neuf journaux de ce dossier (kits A/B V7, V8 et V9, mesurés
sur le Waveshare 7B et le Tab5 du propriétaire, novembre 2026). Pour
le refaire : `python3 test_pr818/depouiller.py`, et `--couts` pour le détail.

Images par seconde, médiane des passages. « Natif » est le moteur de la PR
(rotation pendant le dessin) ; « natif + V9 » le même avec le correctif V9
des copies différées, qui n'est pas dans la PR.

## Waveshare 7B, 1024x600 tourné de 180°, dalle à 60,3 Hz

| Scène | Officiel | Officiel + PPA | Natif (la PR) | Natif + V9 | Natif + V9 contre officiel |
|---|---:|---:|---:|---:|---:|
| Tableau de bord | **61,2** | 60,8 | 37,9 à 38,5 | 49,2 | **−20 %** |
| Images changeantes en PSRAM | 15,3 | 12,7 | 19,2 | **30,1** | **+97 %** |
| Image fixe en flash, sans cache | **5,11** | 4,92 | 4,00 | 4,33 | **−15 %** |
| Image fixe en flash, cache chaud | — | — | 19,3 | 30,3 | (pas d'équivalent) |

## Tab5, 1280x720 tourné de 270°, dalle à environ 48 Hz

| Scène | Officiel | Natif (la PR) | Natif + V9 | Natif + V9 contre officiel |
|---|---:|---:|---:|---:|
| Tableau de bord | **48,9** | 29,3 | 35,1 | **−28 %** |
| Images changeantes en PSRAM | 13,0 | 11,4 | **15,6** | **+20 %** |
| Image fixe en flash, sans cache | **3,73** | 2,60 | 2,77 | **−26 %** |
| Image fixe en flash, cache chaud | — | 11,4 | 15,6 | (pas d'équivalent) |

Sans le correctif V9, la PR telle qu'elle est perd donc aussi les images
changeantes sur le Tab5 (11,4 contre 13,0).

## Pourquoi, scène par scène

**Tableau de bord : l'officiel est au plafond de la dalle.** Son
rafraîchissement médian est de 16,1 ms sur le Waveshare et de 20,3 ms sur le
Tab5, p95 identique : une image par balayage, sur les deux cartes. On ne peut
pas faire plus d'images que la dalle n'en montre ; au mieux, on l'égale.

Le moteur natif, lui, dessinait cette scène **sur un seul cœur**. Le deuxième
cœur peint la deuxième tuile d'une paire, et chacune des cinq zones qu'une
image redessine (l'arc, la barre, le curseur, le compteur, le témoin) tient
dans une seule tuile. Reproduit sur PC avec la même scène
(`check_dashboard` dans `test/main.c`) : **1 tuile sur 120** pour le deuxième
cœur. L'officiel a deux unités de dessin logiciel qui se partagent chaque
zone.

Corrigé dans `lv_draw_ortho.c` le 14 novembre : une zone d'une seule tuile,
ou la tuile impaire d'une plus grande, est coupée en deux moitiés, une par
cœur (au-dessus de `LV_DRAW_ORTHO_PAIR_MIN_PX`, 8192 pixels). Sur la même
scène : 74 tuiles sur 193 pour le deuxième cœur. **Le gain en images par
seconde n'est pas mesuré** : il faut une carte.

**Image fixe en flash : le PPA lit la flash plus lentement que le CPU.** Le
moteur natif donne au PPA l'adresse de l'image en flash, et attend : 223,7 ms
par image sur le Waveshare, 337,8 ms sur le Tab5 (`native_wait`). L'officiel
fait lire la flash par le CPU vers un tampon en RAM interne, puis tourne ce
tampon par le PPA en 21,8 ms et 27,1 ms : 193 ms et 266 ms en tout. La flash
est réglée en DIO à 40 MHz dans ces kits ; les deux moteurs y passent presque
tout leur temps. Pas encore corrigé : une image en flash doit passer par les
tuiles, comme le reste du dessin, au lieu d'aller directement au PPA.

**Images changeantes : le natif gagne, une fois les copies inutiles
évitées.** Le PPA tourne l'image entière en une fois : 25,5 ms d'attente sur
le Waveshare, 40,8 ms sur le Tab5, contre 63 ms et 75 ms par image pour
l'officiel. Mais sans le correctif V9, LVGL recopie l'écran entier d'un
tampon à l'autre à chaque rafraîchissement vide entre deux images : 13,2 ms
par image sur le Waveshare et 20,7 ms sur le Tab5 (`background copy`), ce
qui suffit à faire perdre le Tab5.

## Ce que le correctif V9 a d'embarrassant, et l'autre voie

Il modifie le cœur de LVGL (`lv_refr.c`), ce qu'une PR sur l'adaptateur ne
peut guère porter. LVGL appelle pourtant, pour chaque copie de
synchronisation, une fonction attachée au tampon de destination
(`buf_copy_cb`, LVGL 9.5 ; `sync_cb` de l'affichage en 9.6), et ces tampons
sont ceux que le moteur fournit. La même économie peut donc se faire depuis
le moteur, sans toucher à LVGL. Pas encore écrit.

## Le 15 novembre : les trois corrections, et le banc pour les mesurer

Les trois causes ci-dessus sont corrigées dans le moteur, ici
(`components/lvgl_ortho`) et dans la branche de test de la PR,
`test/rotate-in-draw-sync` sur youkorr/esp-iot-solution :

1. **Tableau de bord : les deux cœurs sur chaque zone.** Une zone qui tient
   dans une tuile est coupée en deux, une moitié par cœur. Sauf si elle
   contient une image agrandie, tournée ou inclinée : LVGL la rééchantillonne
   différemment selon l'endroit coupé (602 pixels décalés d'un niveau dans le
   banc des objets transformés de la PR), donc elle garde ses tuiles.
2. **Image en flash : par les tuiles, jamais par le PPA.** Le CPU la dessine
   dans les tuiles en RAM interne, sur les deux cœurs, et le PPA tourne les
   tuiles pendant que les suivantes se dessinent. Avec le cache d'images, le
   CPU la lit une seule fois.
3. **Les recopies de LVGL entre tampons : reprises par le moteur, sans
   modifier LVGL.** LVGL les fait par `lv_draw_buf_copy()`, qui appelle le
   `buf_copy_cb` du tampon de destination : le moteur le remplace. Chaque
   copie est notée, et au début de l'image suivante (`LV_EVENT_RENDER_START`)
   seul ce qu'elle ne redessine pas est copié, par le PPA. Un
   rafraîchissement vide ne copie plus rien. C'est l'effet du correctif V9,
   mais intégrable à la PR puisqu'il ne touche pas LVGL.

Vérifié sur PC : 615 cas sur LVGL 9.5, 9.6 et 10, 629 à deux cœurs, sous
ASan et UBSan ; dans le banc de la PR, 447 et 462 cas (342 et 353 avant),
les objets transformés dans leurs bornes, et chaque mutant attrapé.
**Rien n'est mesuré sur carte** : c'est l'étape suivante.

### Le banc de mesure

`espressif/pr818-ab/` est le banc V9 de Codex (mêmes scènes, même trace,
mêmes sondes), construit par GitHub Actions
(`.github/workflows/pr818-ab.yml`, ESP-IDF 5.5.6) pour les deux cartes et
deux variantes :

| Firmware | Ce qu'il contient |
|---|---|
| `*-ab-official.bin` | l'adaptateur d'Espressif tel qu'il est (da53cfa, TRIPLE_PARTIAL) |
| `*-ab-native.bin` | la PR avec les trois corrections (ROTATE_IN_DRAW, DOUBLE_DIRECT) |

Dans le firmware natif, les deux modes de V9 sont gardés : `eager` est LVGL
qui fait ses recopies lui-même, `deferred` est le moteur qui les reprend.
L'officiel et le natif sont donc mesurés avec le même code de banc, le même
jour, et on peut les comparer ligne à ligne.

Les firmwares du 15 novembre sont les artefacts de l'exécution
https://github.com/youkorr/ppa-alternative-/actions/runs/43695049197
(`waveshare-7b-ab-official`, `waveshare-7b-ab-native`, `tab5-ab-official`,
`tab5-ab-native` ; chaque zip contient le `.bin`, le `sdkconfig` et le `.elf`).

Pour chaque carte : flasher le `.bin` à **0x0**, ouvrir le journal à
**115200 bauds**, redémarrer, attendre `AB_DONE` (deux à quatre minutes),
et envoyer le journal complet. Puis la même chose avec l'autre firmware.
`python3 test_pr818/depouiller.py` lit les journaux.

## Le 15 novembre, mesuré sur carte

Les quatre firmwares ci-dessus, mêmes scènes, même jour (journaux
`*-v10 ESP-logs.txt` dans ce dossier). Images par seconde, médiane de deux
passages :

| Scène | Waveshare officiel | Waveshare PR (`deferred`) | Tab5 officiel | Tab5 PR (`deferred`) |
|---|---|---|---|---|
| tableau de bord | **61,2** | 52,0 | **48,9** | 40,4 |
| images changeantes | 15,2 | **29,8** (×1,95) | 13,1 | **15,5** (+18 %) |
| image en flash, sans cache | 5,1 | **5,7** (+11 %) | 3,75 | **4,0** (+7 %) |
| image en flash, avec cache | (pas de cache) | **30,0** | (pas de cache) | **15,5** |

Ce qu'apportent les trois corrections, d'après les mesures précédentes :

- **Image en flash** : 4,0 → 5,2 images/s sur le Waveshare et 2,6 → 3,7
  sur le Tab5, sans même la reprise des recopies (`eager`). La PR passe
  devant l'officiel.
- **Recopies reprises par le moteur** : le tableau de bord passe de 38,4 à
  52,0 sur le Waveshare et de 30,0 à 40,4 sur le Tab5 (`eager` →
  `deferred`). Sur le Tab5 c'est mieux que le correctif V9 de Codex, qui
  modifiait LVGL (35,1).
- **Les deux cœurs sur le tableau de bord** : rien. 38,58 → 38,41 sur le
  Waveshare, 29,4 → 30,0 sur le Tab5 (`eager`).

**Le tableau de bord reste derrière l'officiel**, de 15 % et 17 %. L'officiel
y est au rythme de l'écran (16,1 ms par image = une image par balayage) ; la
PR met 19,3 ms par image sur le Waveshare et 24,7 ms sur le Tab5.

### Pourquoi : chaque tuile d'une carte était relue avant d'être peinte

Le banc du PC refait le tableau de bord aux tailles des deux cartes et
compte ce que fait le moteur, image par image. Sur le Waveshare : **82 000
pixels tournés par le PPA pour 48 800 redessinés**, dont 33 000 relus depuis
l'écran. La carte dit la même chose : 151 Kio tournés par image dans son
journal.

Une tuile commence par ce qu'il y a déjà sous elle, sauf si la première
chose dessinée la couvre entièrement. Dans une carte, la première chose
dessinée est l'ombre, que le fond de la carte recouvre ensuite. Le moteur le
savait, mais seulement si le fond occupe exactement le rectangle de l'ombre.
Or une carte avec une bordure a son fond dessiné **un pixel en retrait**
(LVGL laisse la place à l'anticrénelage de la bordure). La règle ne
s'appliquait donc jamais. Chaque tuile d'une carte était d'abord relue
depuis la PSRAM par le PPA pendant que le CPU attendait, puis l'ombre était
peinte pour rien sous le fond.

La correction : ce qu'un fond arrondi recouvre à coup sûr est sa **croix**,
le rectangle moins ses quatre coins. Même chose pour l'intérieur d'une
bordure arrondie. Sur le PC, aux tailles des deux cartes : **plus aucune
tuile relue**, 48 800 pixels tournés pour 48 800 redessinés (au lieu de
82 000), et 35 % de pixels peints en moins par le CPU.

Le nouveau test des cartes met des objets au milieu d'une carte, le long de
ses bords et dans son coin arrondi, sur trois rotations, au CPU et au PPA.
Il échoue sur l'ancien moteur (tuiles relues), et quatre erreurs
volontaires sont attrapées : l'ancienne règle, les coins comptés comme
couverts, un seul bras de la croix, et les coins d'une bordure comptés comme
vides.

**C'est probablement aussi pourquoi les deux cœurs n'apportaient rien** : la
relecture de chaque tuile passait par le PPA, l'un après l'autre, et les
deux cœurs l'attendaient. Il reste un second défaut possible, corrigé dans
le même temps. La tâche qui peint sur le second cœur était fixée une fois
pour toutes au cœur opposé à celui où se trouvait LVGL au démarrage. Mais la
tâche de LVGL n'a pas de cœur attitré, et dans le banc, c'est la tâche
principale (cœur 0) qui rafraîchit. Si les deux se retrouvaient sur le même
cœur, les deux moitiés de chaque zone étaient peintes l'une après l'autre.
Il y a maintenant une tâche par cœur, la seconde démarrée la première fois
qu'elle sert, et le travail va toujours à celle de l'autre cœur. Une ligne
`AB_NATIVE` dans le journal donne désormais les tuiles relues, la part du
second cœur et les cœurs où tournent ces tâches.

### Les firmwares à mesurer

Exécution https://github.com/youkorr/ppa-alternative-/actions/runs/43721830314
(PR : `test/rotate-in-draw-sync` à fab3be6). **Seuls les deux firmwares
natifs ont changé** : `waveshare-7b-ab-native` et `tab5-ab-native`.
L'officiel est le même, ses journaux du 15 novembre restent valables. Même
procédure : `.bin` à 0x0, journal à 115200 bauds jusqu'à `AB_DONE`.
`python3 test_pr818/depouiller.py --moteur` lit en plus les lignes
`AB_NATIVE`.

## Le 15 novembre, après la correction des cartes : la PR devant ou à égalité partout

Les deux firmwares natifs reconstruits (exécution 43721830314, journaux
`*-native-v11 ESP-logs.txt`), comparés aux journaux officiels du matin (même
firmware officiel). Images par seconde, médiane de deux passages :

| Scène | Waveshare officiel | Waveshare PR | Tab5 officiel | Tab5 PR |
|---|---|---|---|---|
| tableau de bord | 61,2 | **61,2** (52,0 avant) | 48,9 | **48,9** (40,4 avant) |
| images changeantes | 15,2 | **29,5** (×1,93) | 13,1 | **15,5** (+18 %) |
| image en flash, sans cache | 5,1 | **5,7** (+11 %) | 3,75 | **4,0** (+6 %) |
| image en flash, avec cache | (pas de cache) | **29,8** | (pas de cache) | **15,5** |

**Le tableau de bord est au maximum de l'écran sur les deux cartes**, comme
l'officiel : une image à chaque balayage (rafraîchissement médian 15,7 ms
sur le Waveshare, 19,8 ms sur le Tab5, p95 15,9 et 20,0). Aucun
adaptateur ne peut faire plus sur cette scène. Avec LVGL qui fait ses
recopies lui-même (`eager`), la correction seule fait passer le tableau de
bord de 38,4 à 49,1 sur le Waveshare et de 30,0 à 41,1 sur le Tab5.

La ligne `AB_NATIVE` confirme la cause : sur le tableau de bord, 0,3 tuile
relue par image sur 7,5 (il en restait 5 sur 8 avant), et le PPA tourne
86 Kio par image au lieu de 151 sur le Waveshare (112 au lieu de 203 sur le
Tab5). Le second cœur peint 2,3 tuiles par image. `helper_cores=2` : une
seule tâche, sur le cœur 1. Dans ce banc, la tâche du second cœur était donc
déjà sur le bon cœur. Le placement par cœur ne change rien ici ; il protège
les applications dont la tâche de dessin change de cœur.

### La charge du processeur et un redessin plein écran

Exécution https://github.com/youkorr/ppa-alternative-/actions/runs/43725165875,
**les quatre firmwares** (officiel et PR, Waveshare et Tab5), car le banc
a changé pour les deux :

- une scène de plus, `dashboard_full` : le tableau de bord avec tout l'écran
  redessiné à chaque image, ce que coûte un changement de page ou une liste
  qu'on fait défiler ;
- une ligne `AB_CPU` par scène : le temps que chaque cœur a passé à ne rien
  faire (compteurs de FreeRTOS), d'où la part du processeur occupée et le
  temps de travail par image. Là où les deux adaptateurs sont au rythme de
  l'écran, c'est ce qui les distingue : le temps qui reste pour le reste de
  l'application.

`python3 test_pr818/depouiller.py` affiche les deux.

## Le 15 novembre au soir : redessin plein écran et charge du processeur

Firmwares de l'exécution 43725165875, journaux `*-v12 ESP-logs.txt`.
« Travail par image » : le temps où les deux cœurs ne sont pas au
repos, divisé par le nombre d'images. Une attente active compte comme du
travail.

| Tab5 | officiel | PR (`deferred`) |
|---|---|---|
| tableau de bord | 48,9 images/s, 26 % occupé, **10,8 ms** par image | 48,6 images/s, 34 % occupé, 14,0 ms par image |
| **plein écran** | **8,3 images/s**, 76 % occupé, 183 ms par image | **15,9 images/s (×1,9)**, 83 % occupé, **105 ms** par image |
| images changeantes | 13,3 images/s, 80 ms par image | **15,5 images/s**, **67 ms** par image |
| image en flash | 3,76 images/s, 408 ms par image | **3,95 images/s**, 404 ms par image |
| image en flash, avec cache | (pas de cache) | **15,5 images/s**, 67 ms par image |

| Waveshare 7B | officiel | PR (`deferred`) |
|---|---|---|
| tableau de bord | 61,3 images/s, 31 % occupé, **10,2 ms** par image | 61,3 images/s, 37 % occupé, 12,0 ms par image |
| **plein écran** | **15,2 images/s**, 95 ms par image | **24,1 images/s (×1,58)**, **69 ms** par image (−27 %) |
| images changeantes | 15,3 images/s, 72 ms par image | **29,6 images/s (×1,94)**, **34 ms** par image (−53 %) |
| image en flash | 5,14 images/s, 298 ms par image | **5,69 images/s**, 280 ms par image |
| image en flash, avec cache | (pas de cache) | **29,9 images/s**, 34 ms par image |

Le journal officiel du Waveshare est arrivé ensuite (`waveshare-7b-ab-official-v12`,
`variant=official`) ; le fichier reçu d'abord sous ce nom était un natif de
l'exécution précédente.

Ce que ça dit :

- **Plein écran, ce qu'on sent en changeant de page : ×1,9 sur le Tab5 et
  ×1,58 sur le Waveshare, avec 43 % et 27 % de travail en moins par image.**
- **Partout où quelque chose de gros change, la PR travaille moins par
  image** (images changeantes : −53 % sur le Waveshare, −16 % sur le Tab5).
- **Sur le tableau de bord, l'officiel travaille moins** : 10,8 ms par image
  contre 14,0 sur le Tab5, 10,2 contre 12,0 sur le Waveshare. Une cause est vue dans le code : la PR attend le PPA en
  boucle active (`wait_on()` dans `lv_draw_ortho_ppa.c`, toutes les 10 µs),
  et le journal compte 2,6 ms d'attente du PPA par image sur ce tableau de
  bord. Attendre le PPA en dormant (son rappel de fin de transfert) rendrait
  ce temps au reste de l'application. Pas fait, pas mesuré.

## Le 15 novembre, 17 h : l'attente du PPA endormie

Firmwares natifs de l'exécution 43732115749 (commit `45f7420` de
`test/rotate-in-draw-sync`), journaux `*-native-v13 ESP-logs.txt`. Les
journaux officiels restent ceux de la v12 : le banc n'a pas changé.

Le changement : une tâche qui attend le PPA dort sur un sémaphore que
l'interruption de fin de transfert lui donne, au lieu de tourner toutes les
10 µs. Le PPA ne va pas plus vite (le temps d'attente mesuré par
`native_wait` est le même qu'en v12, à 0,1 ms près) ; ce temps-là n'est plus
du travail.

Mode `deferred`, travail des deux cœurs par image :

| | officiel | PR v12 | **PR v13** |
|---|---|---|---|
| Tab5, tableau de bord (48,9 images/s partout) | **10,8 ms** | 14,0 ms | **11,6 ms** |
| Tab5, plein écran | 183 ms, 8,3 images/s | 105 ms, 15,9 images/s | **103 ms, 15,8 images/s** |
| Tab5, images changeantes | 80 ms, 13,3 images/s | 67 ms, 15,5 images/s | **27 ms, 15,4 images/s** |
| Waveshare, tableau de bord (61,3 images/s partout) | **10,2 ms** | 12,0 ms | **10,6 ms** |
| Waveshare, plein écran | 95 ms, 15,2 images/s | 69 ms, 24,1 images/s | **69 ms, 23,6 images/s** |
| Waveshare, images changeantes | 72 ms, 15,3 images/s | 34 ms, 29,6 images/s | **9,1 ms, 29,3 images/s** |

Ce que ça dit :

- **Le temps gagné est exactement le temps d'attente** : 41 ms d'attente du
  PPA par image changeante sur le Tab5, 40 ms de travail en moins ; 2,6 ms
  sur le tableau de bord, 2,4 ms en moins.
- **Images changeantes : la PR fait plus d'images avec 3 fois (Tab5) à
  8 fois (Waveshare) moins de processeur que l'officiel.** Sur le Waveshare
  le processeur est occupé 13 % du temps à 29 images/s, contre 55 % à
  15 images/s pour l'officiel.
- **Tableau de bord : l'écart avec l'officiel est presque fermé** (11,6
  contre 10,8 ms sur le Tab5, 10,6 contre 10,2 sur le Waveshare).
- Le nombre d'images ne bouge pas, à 1 ou 2 % près, dans un sens ou dans
  l'autre : c'était attendu, le PPA et l'écran fixent le rythme.
- Aucune erreur, aucun PPA bloqué, `hardware_measurement=COMPLETE` sur les
  deux cartes.

## Ce que ces journaux ne disent pas

- Rien sur le Tab5 en V7 (officiel + PPA, lots de 128), ni l'officiel V7 du
  Waveshare : ces journaux ne sont pas dans le dossier.
- Les images changeantes sont synthétiques : ce n'est pas un capteur.
- Les temps d'`AB_COST` sont inclusifs et ne s'additionnent pas ; dans la
  scène du tableau de bord ils n'expliquent que 3,5 ms des 18,5 ms d'une
  image native. Le reste est du dessin, que ces sondes ne détaillent pas.
