# Kit PR818 V2 — Tab5 et Waveshare 7B

[Télécharger le kit V2 complet (2,4 Mo)](https://github.com/youkorr/ppa-alternative-/raw/refs/heads/test/pr818-card-firmwares-20261112/downloads/pr818-tab5-waveshare/pr818-cartes-v2.zip)

[Guide de flash et d’analyse en français](GUIDE-V2.md) · [Résultats des premiers essais](RESULTATS-V1.md)

La V2 utilise le correctif `9db972e7008d4c5adab0fde59dc3b0b1343dac4c` de la PR Espressif #818 :
verrou limité au dessin du texte, hauteur du cache PPA corrigée et suppression
unique des threads LVGL FreeRTOS. Les tests logiciels et les compilations passent ;
les FPS et la stabilité de la V2 restent à mesurer sur vos cartes.

Les références **before** restent identiques. Vous pouvez réutiliser vos anciens
journaux before et flasher seulement `after_v2`. Les 16 cycles de démarrage/arrêt
viennent après les mesures pour préserver leur état initial.

Le ZIP contient les quatre firmwares (before et after_v2 pour chaque carte),
les sources, les outils USB et d’analyse, ainsi que les résultats structurés du premier kit.
La scène mobile mesure l’affichage de frames en RAM ; les capteurs caméra ne sont pas utilisés.

Cartes : Tab5 d’origine ILI9881C 720×1280 à 270°, Waveshare ESP32-P4-WIFI6-Touch-LCD-7B
EK79007 1024×600 à 180°, silicium v0.x/v1.x jusqu’à v1.99.
Images fusionnées à flasher en USB sur le P4 à **0x0**, avec sauvegarde optionnelle.

SHA256 du ZIP V2 :

```text
abf8e2c1e444f72fb2a9bb4befac0b89aab51ae01298550f72b3b99b21e7f2cf
```

Le premier kit `pr818-cartes.zip` reste présent pour reproduction historique.
Ses mesures ont révélé une régression de l’interface et des défauts corrigés par V2.
