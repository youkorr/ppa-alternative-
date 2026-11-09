<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 youkorr
Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
-->

# La PR pour Espressif : esp-iot-solution #818

**Ouverte et prête à être relue :**
<https://github.com/espressif/esp-iot-solution/pull/818>

Elle ajoute `lv_draw_ortho` au composant `esp_lvgl_adapter` d'Espressif, sous
une option désactivée par défaut : `CONFIG_ESP_LVGL_ADAPTER_ROTATE_IN_DRAW`.

| où | quoi |
|---|---|
| branche `feat/esp_lvgl_adapter-rotate-in-draw` de [youkorr/esp-iot-solution](https://github.com/youkorr/esp-iot-solution) | le code de la PR ; tout ce qui y est poussé arrive dans la PR |
| `esp-iot-solution/PR.md` | la description de la PR, telle qu'elle est sur GitHub |
| `esp-iot-solution/comment-results.md` | un commentaire à coller dans la PR : il remplace celui qui la gardait en brouillon (mesures de l'adaptateur, deux corrections) |
| `esp-iot-solution/comment-hardware.md` | un commentaire à coller dans la PR : ce qui n'a pas pu être testé (rev 3.0, RGB) |
| `../../espressif/` | les firmwares de test pour la Waveshare 7B et la Tab5 |
| workflow *Espressif pull request* | compile la branche de la PR comme la CI d'Espressif : ESP-IDF v5.5.5, release-v6.0, release-v6.1 et latest, avec et sans l'option |
| workflow *Espressif test app on a Waveshare 7B and a Tab5* | fabrique les firmwares à flasher (`.bin` à l'adresse `0x0`) |

## Ce que fait la PR

Avec un écran tourné en mode `DOUBLE_DIRECT`, LVGL dessine directement dans
le sens de l'écran, au lieu de dessiner droit puis de recopier en tournant :

- les widgets sont dessinés par LVGL dans de petites tuiles en RAM interne,
  et le PPA tourne chaque tuile dans le frame buffer pendant que le CPU
  dessine la suivante ;
- avec `LV_DRAW_SW_DRAW_UNIT_CNT` supérieur à 1, les tuiles sont dessinées
  sur les deux cœurs ; quand la RAM interne n'a de place que pour un bloc de
  128 Ko (cache L2 de 256 Ko), les tuiles sont découpées dans ce bloc ;
- l'écran garde les **trois** frame buffers d'un écran tourné : LVGL dessine
  l'image suivante dans le troisième pendant que l'écran attend son
  rafraîchissement. Avec deux, chaque image attendait ce rafraîchissement
  (66,1 ms au lieu de 58,4 sur la Waveshare) ;
- les autres modes, la rotation 0 et LVGL v8 ne changent pas.

## Mesuré sur tes cartes

L'application de test d'Espressif (`test_apps/lvgl9`), le même firmware
compilé deux fois, avec et sans l'option, ESP-IDF v5.5.5 :

| | Espressif aujourd'hui | avec la PR | |
|---|---|---|---|
| Waveshare 7B, 90° | 15,8 images/s | **17,2 - 17,3 images/s** | **+9 %** |
| Waveshare 7B, 180° | 16,6 images/s | **18,0 images/s** | **+8 %** |
| Tab5, 270° | 11,3 images/s | **12,9 - 13,0 images/s** | **+15 %** |
| Waveshare 7B, 90°, cache L2 de 128 Ko (le défaut d'ESP-IDF) | 15,5 - 15,6 images/s | **23,3 - 23,4 images/s** | **+50 %** |
| Waveshare 7B, 180°, cache L2 de 128 Ko | 16,7 images/s | **25,9 - 26,1 images/s** | **+56 %** |
| Tab5, 270°, cache L2 de 128 Ko | 11,2 images/s | **17,9 - 18,0 images/s** | **+61 %** |

Tous les tests de l'application passent dans tous les cas. Sur la Tab5,
ESP-IDF v6.1 donne la même chose (13,0 - 13,2 images/s).
L'application de test d'Espressif règle le cache L2 à 256 Ko ; avec les
128 Ko par défaut d'ESP-IDF, il reste plus de RAM interne pour les tuiles :
leur méthode ne bouge pas, la nôtre gagne de 35 à 45 %, sur les deux cartes. Le détail est dans le
[README principal](../../README.md#measured-on-the-boards).

## Ce qui reste à faire

1. **L'accord de contributeur d'Espressif** : signé, le robot de la PR dit
   « All committers have signed the CLA », y compris pour les derniers
   commits.
2. **Leur CI** : elle compile la PR pour plusieurs versions d'ESP-IDF. Si elle
   échoue, copier l'erreur ici ou dans la conversation : la correction se fait
   sur la branche du fork et arrive dans la PR toute seule.
3. **Les questions des relecteurs**, probablement :
   - les puces **rev 3.0** et les écrans **RGB**, qu'aucune de tes cartes
     n'a ;
   - la taille du changement ;
   - le fait que `lv_draw_ortho` vienne de ce dépôt (même auteur, licence
     MIT, testé pixel par pixel contre LVGL).
4. **Le test rev 3.0, poussé dans la PR** : `test_adapter_esp32p4_rotate_in_draw`
   (14 lignes de `pytest_adapter_lvgl9.py`) fait tourner les tests de
   l'application avec l'option, écran tourné à 90°, sur la carte rev 3.0 de
   leur CI. Il tourne quand un mainteneur lance leur CI ; rien à faire de ton
   côté. Le commentaire à coller dans la PR est
   `esp-iot-solution/comment-hardware.md`.

## Les règles d'Espressif, déjà respectées

- Licence : le code ajouté est sous MIT, que leurs composants acceptent.
- Formatage : leur outil astyle (version 3.4.7, leurs règles) a été passé.
- Orthographe : codespell, sans erreur.
- Messages de commit au format qu'ils imposent : `feat(esp_lvgl_adapter): ...`,
  `fix(...)`, `docs(...)`.
- CHANGELOG, README et README_CN du composant à jour.
- Copyright : leur outil `check-copyright` (v1.1.1, leur configuration) a
  été passé ; il met l'année à « 2025-2026 » sur les fichiers d'Espressif que
  la PR modifie de 5 lignes ou plus.
