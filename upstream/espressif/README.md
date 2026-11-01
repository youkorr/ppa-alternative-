<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 youkorr
Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
-->

# La PR pour Espressif (pas encore envoyée)

Ce dossier contient une pull request **prête mais pas envoyée** pour
[espressif/esp-iot-solution](https://github.com/espressif/esp-iot-solution),
composant `esp_lvgl_adapter`. Elle n'est pas publique tant que l'étape 4
ci-dessous n'est pas faite.

| fichier | ce que c'est |
|---|---|
| `esp-iot-solution/0001-esp_lvgl_adapter-rotate-in-draw.patch` | la modification, prête à appliquer sur leur code |
| `esp-iot-solution/PR.md` | le titre et le texte à coller dans la PR (en anglais) |

La CI de ce dépôt (*Espressif pull request*) applique le patch sur leur code et
compile leur application de test pour l'ESP32-P4 avec ESP-IDF 5.5.5, avec et
sans l'option. Si elle est verte, la PR compile chez eux.

## Ce que fait la PR

Une nouvelle option, `CONFIG_ESP_LVGL_ADAPTER_ROTATE_IN_DRAW`, désactivée par
défaut. Avec un écran tourné en mode `DOUBLE_DIRECT`, LVGL dessine directement
dans le sens de l'écran (avec `lv_draw_ortho` et le PPA), au lieu de dessiner
droit puis de recopier en tournant. Une image en moins à recopier, et un frame
buffer en moins (2 au lieu de 3).

## Avant d'envoyer : tester

1. Sur le Tab5 et le Waveshare, avec `yaml/test-lvgl.yaml` : noter les
   « LVGL ms per frame » en mode `lvgl` et en mode `ortho`.
2. Remplacer les `[crochets]` de la section *Testing* de `PR.md` par ces
   chiffres.

Une PR testée seulement en simulation serait mal reçue : ces chiffres sont le
plus important du texte.

## Envoyer, étape par étape

1. **Faire une copie (fork)** : sur la page
   <https://github.com/espressif/esp-iot-solution>, bouton **Fork**. Cela crée
   `youkorr/esp-iot-solution` sur ton compte.
2. **Mettre le patch sur une branche de ta copie** (sur un PC avec git) :
   ```
   git clone https://github.com/youkorr/esp-iot-solution
   cd esp-iot-solution
   git checkout d2d935d0ee3e78deffa3981cc8c39ac8f534932b
   git checkout -b feat/esp_lvgl_adapter-rotate-in-draw
   git am chemin/vers/0001-esp_lvgl_adapter-rotate-in-draw.patch
   git push -u origin feat/esp_lvgl_adapter-rotate-in-draw
   ```
   Si leur code a changé entre-temps et que `git am` refuse, demande-moi : je
   remets le patch à jour sur leur dernière version.
3. **Ouvrir la PR** : GitHub propose alors un bouton *Compare & pull request*.
   La destination est `espressif/esp-iot-solution`, branche `master`.
4. **Coller le titre et le texte** de `PR.md`, puis *Create pull request*.
   C'est à ce moment-là que la PR devient publique.
5. **Signer l'accord de contributeur** d'Espressif : un robot le demande
   automatiquement dans la PR, il suffit de suivre son lien.

Ensuite les mainteneurs relisent, posent des questions ou demandent des
changements. Je peux t'aider à répondre et à mettre la PR à jour.

## Les règles d'Espressif, déjà respectées

- Licence : le code ajouté est sous MIT, que leurs composants acceptent.
- Formatage : leur outil astyle (version 3.4.7, leurs règles) a été passé.
- Orthographe : codespell, sans erreur.
- Message de commit au format qu'ils imposent : `feat(esp_lvgl_adapter): ...`.
- Nom de branche : une seule barre oblique, pas de majuscule.
- Leur vérification des en-têtes de copyright n'a pas pu tourner ici ;
  l'en-tête suit leur format SPDX.
