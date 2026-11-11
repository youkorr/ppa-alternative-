# Test de la mise à jour PR818 sur Tab5 et Waveshare 7B

Ce kit compare les deux versions de la **PR Espressif #818**, avec les pilotes
de vos cartes. Ce sont des firmwares de banc autonomes pour le P4 : aucun Wi-Fi,
Home Assistant, ESPHome ou identifiant n’est nécessaire. Le firmware du C6
n’est pas modifié. Ils remplacent l’application actuelle du P4 pendant le test.

## Cartes et versions

- `tab5` : **M5Stack Tab5 d’origine**, ILI9881C 720×1280, paysage à 270°,
  reset via PI4IOE5V6408, rétroéclairage GPIO22, flash 16 Mio.
- `waveshare-7b` : **ESP32-P4-WIFI6-Touch-LCD-7B**, EK79007 1024×600 à 180°,
  reset GPIO33, rétroéclairage GPIO32 actif bas, flash 32 Mio.
- Les deux firmwares ciblent le silicium **v0.x/v1.x (jusqu’à v1.99)**,
  correspondant aux Tab5 v1.0 et Waveshare v1.3 de vos mesures.
- `before` : PR818 avant la mise à jour, commit
  `ba1ed66c40c840cd1b2dbe9062760c2c86262b5a`.
- `after` : mise à jour publiée, commit
  `7965432ed3220ab29a2b44c84d78f341d4185224`.

Réglages communs : ESP-IDF 5.5.6, LVGL 9.6.0, CPU 360 MHz, PSRAM 200 MHz,
cache L2 128 Kio, flash DIO 40 MHz, RGB565, deux cœurs de rendu, trois buffers du panneau,
rotation-in-draw. Le moniteur FPS en surimpression est désactivé. Le code et
les données flash restent en flash (`SPIRAM_XIP_FROM_PSRAM=n`), notamment pour
tester l’admission automatique du fond fixe dans le cache.

Les résultats ne se comparent directement qu’entre les firmwares de ce kit,
avec ces mêmes réglages. Les anciens chiffres publiés utilisaient parfois
256 Kio de L2 et l’exécution depuis la PSRAM.

## Flasher et enregistrer les mesures

Installer Python 3.10 ou plus puis, depuis le dossier extrait :

```sh
python -m pip install -r requirements.txt
```

Remplacer `COM5` par le port USB du **P4** de la carte. Sous Linux, par exemple
`/dev/ttyACM0`. Une image `firmware.bin` est une image **fusionnée à écrire à
l’adresse 0x0**, pas un fichier OTA ESPHome.

Pour conserver le contenu actuel de la flash, ajouter `--backup sauvegarde.bin`
à la première commande. Cela enregistre la totalité des 16/32 Mio avant l’écriture
et refuse d’écraser une sauvegarde existante. Garder aussi votre YAML et votre
firmware ESPHome habituels pour revenir à votre installation.

### Tab5

```sh
python flash.py tab5 before --port COM5 --log tab5-before.log
python flash.py tab5 after --port COM5 --log tab5-after.log
python mesurer.py tab5-before.log tab5-after.log --csv tab5-resultats.csv
```

### Waveshare 7B

```sh
python flash.py waveshare-7b before --port COM5 --log waveshare-before.log
python flash.py waveshare-7b after --port COM5 --log waveshare-after.log
python mesurer.py waveshare-before.log waveshare-after.log --csv waveshare-resultats.csv
```

Le programme vérifie le SHA256 du binaire, flashe, puis enregistre le port série
à 115200 bauds. Le banc attend 10 secondes après son initialisation pour laisser
le temps au port USB de revenir. Aucun toucher n’est nécessaire. Compter environ
deux minutes pour `before` et trois minutes pour `after`. La fin porte la ligne
`PR818_DONE` ; Unity doit indiquer `PR818 board comparison:PASS`.

Pour restaurer une sauvegarde complète après les tests :

```sh
python -m esptool --chip esp32p4 --port COM5 write-flash 0x0 sauvegarde.bin
```

## Ce qui est mesuré

Chaque scène dure 8 secondes et revient **trois fois**. Le programme d’analyse
compare les médianes des images/s ; il distingue la mise à jour du moteur et
l’effet du cache dans le même moteur.

| Scène | Objet de la comparaison |
|---|---|
| `ui` | Interface : texte, cartes, ombres, dégradé, arc, boutons. Cache désactivé. |
| `fixed_off` | Grand fond RGB565 immuable en flash, redessiné, cache désactivé. |
| `fixed_on` | Même fond, cache de 4 Mio activé et préchauffé. Version `after`. |
| `moving_off` | Deux frames RGB565 en PSRAM alternées, marquées modifiables. Cache désactivé. |
| `moving_on` | Même séquence avec cache activé : les frames doivent en rester exclues. Version `after`. |
| `moving_native` | Frames déjà dans l’orientation du panneau : mesure le chemin de copie, modifié dans la mise à jour. |

**Les scènes mobiles n’utilisent pas les capteurs SC2356/OV5647.** Elles isolent
le coût d’affichage de frames et de leur rotation/copie. Les FPS de ces scènes
ne sont pas les FPS d’une caméra complète : capture, ISP et conversion ne sont
pas mesurés. Pour reprendre vos essais avec les capteurs réels sous ESPHome,
les configurations existantes sont dans `youkorr/ppa-alternative-`,
`yaml/tab5-test-camera.yaml` et `yaml/waveshare-test-camera.yaml` ; elles testent
l’intégration ESPHome du projet, qui est distincte de l’adaptateur de la PR818.

Le cache est rempli avant la mesure de `fixed_on`. `warmup_ms` donne la durée
totale des trois premières images, qui ne font pas partie de la cadence mesurée.
Les images mobiles alternent entre deux buffers préremplis pour isoler le rendu,
sans faire entrer le coût de génération de chaque pixel dans le chronométrage.

## Journaux à conserver

Garder les **quatre fichiers `.log` complets**, pas uniquement la moyenne FPS.
`PR818_START` donne le commit et les réglages. Chaque `PR818_RESULT` donne :

- cadence sur le temps écoulé, durée moyenne de rendu et pire durée ;
- durée du refresh et plus grand intervalle entre deux débuts de rendu ;
- compteurs de cache, images tournées/natives et octets transmis au PPA ;
- état de blocage du moteur (`failed=0` attendu).

Le compteur mesure les refreshs rendus/soumis par LVGL, pas un comptage physique
des balayages du panneau. Les mesures englobent les attentes du chemin de refresh
et doivent être accompagnées d’une observation visuelle (fluidité, couleurs,
absence de déchirure).

L’analyse refuse les journaux incomplets, les échecs Unity, un PPA bloqué, un
cache jamais utilisé dans `fixed_on`, des hits de cache sur une frame modifiable,
ou des réglages différents entre les deux versions d’une même carte. Une
régression sur `ui` doit être conservée et rapportée : le garde des polices peut
réduire le parallélisme des pages riches en texte.

## Provenance et reconstruction

Chaque dossier de firmware contient son manifeste SHA256, le `sdkconfig` réel
et le fichier de dépendances résolues. `SHA256SUMS` permet une vérification globale.
Les sources du banc et les instructions de reconstruction sont dans `source/`.
Ces binaires ont été compilés et leurs images de flash vérifiées ; leur exécution
sur les écrans reste à effectuer chez vous.
