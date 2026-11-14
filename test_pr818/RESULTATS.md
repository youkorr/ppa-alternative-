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

## Ce que ces journaux ne disent pas

- Rien sur le Tab5 en V7 (officiel + PPA, lots de 128), ni l'officiel V7 du
  Waveshare : ces journaux ne sont pas dans le dossier.
- Les images changeantes sont synthétiques : ce n'est pas un capteur.
- Les temps d'`AB_COST` sont inclusifs et ne s'additionnent pas ; dans la
  scène du tableau de bord ils n'expliquent que 3,5 ms des 18,5 ms d'une
  image native. Le reste est du dessin, que ces sondes ne détaillent pas.
