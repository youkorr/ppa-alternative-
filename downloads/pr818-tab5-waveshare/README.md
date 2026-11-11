# Kit PR818 — Tab5 et Waveshare 7B

[Télécharger le ZIP complet (2,4 Mo)](https://github.com/youkorr/ppa-alternative-/raw/refs/heads/test/pr818-card-firmwares-20261112/downloads/pr818-tab5-waveshare/pr818-cartes.zip)

Le ZIP contient les quatre firmwares de banc déjà compilés (avant/après pour chaque carte),
les outils de flash et d’analyse, les sources et les réglages exacts.

[Guide en français](GUIDE.md)

- Tab5 d’origine : ILI9881C, 720×1280, rotation 270°, flash 16 Mio.
- Waveshare ESP32-P4-WIFI6-Touch-LCD-7B : EK79007, 1024×600, rotation 180°, flash 32 Mio.
- Silicium ESP32-P4 v0.x/v1.x, jusqu’à v1.99.
- Flash USB du P4 à l’adresse 0x0 ; ce ne sont pas des images OTA ESPHome.
- La scène mobile mesure le rendu de frames en RAM. La capture des capteurs caméra est un test distinct.

SHA256 du ZIP :

```text
7ac7c3ecfa63d646bf0c303dc5bdf4581fd3078c91355707b6a7d1613e003b5c
```

Les quatre compilations et les checksums des images ont été vérifiés. L’exécution sur les cartes reste à effectuer.
