# PR818 V2 — firmwares individuels Tab5 et Waveshare 7B

> **Retour matériel V2 :** le Tab5 signale un blocage PPA puis plante à la réinitialisation de l’écran. Utiliser le [test Tab5 V3 et ses instructions](../pr818-tab5-v3/README.md) pour la suite. Le Waveshare V2 termine ses 18 mesures avec `failed=0` et ses 16 cycles de création/destruction. La disparition du blocage sur Tab5 reste à confirmer.

Un blocage antivirus Windows a été signalé lors du téléchargement du ZIP V2.
La cause et le nom de la détection restent à établir. Ne désactivez pas la protection
et n’ajoutez pas d’exclusion. Les vérifications d’intégrité ne constituent pas un verdict antivirus.

Les images suivantes sont **identiques à celles du ZIP V2**, publiées séparément
pour isoler l’élément concerné, sans archive ni scripts du kit à installer :

- [Tab5 d’origine — tab5-after_v2.bin](https://github.com/youkorr/ppa-alternative-/raw/refs/heads/test/pr818-card-firmwares-20261112/downloads/pr818-tab5-waveshare/firmwares/tab5-after_v2.bin)
- [Waveshare 7B — waveshare-7b-after_v2.bin](https://github.com/youkorr/ppa-alternative-/raw/refs/heads/test/pr818-card-firmwares-20261112/downloads/pr818-tab5-waveshare/firmwares/waveshare-7b-after_v2.bin)
- [SHA256 des deux images](firmwares/SHA256SUMS.txt)
- [Notice Windows et flash USB](firmwares/LIRE-MOI-WINDOWS.md)

Si Windows signale aussi un fichier individuel, conservez le blocage et relevez
le détail dans l’historique de protection. Nous n’avons pas de conclusion de faux positif.

Ces images ciblent les ESP32-P4 v0.x/v1.x jusqu’à v1.99. Flash **USB à 0x0** sur le P4
avec votre outil habituel. Le C6 n’est pas flashé ; l’application du P4 est remplacée
pendant le test. Ce ne sont pas des images OTA ESPHome.

La source V2 est le commit `9db972e7008d4c5adab0fde59dc3b0b1343dac4c` de
[la PR Espressif #818](https://github.com/espressif/esp-iot-solution/pull/818) : mutex autour
du texte, hauteur du cache PPA corrigée et suppression unique des threads LVGL.
Les tests logiciels et compilations passent ; les mesures et la stabilité V2 sur les cartes
restent à vérifier. Les références before du premier kit sont conservées.

[Guide complet V2](GUIDE-V2.md) · [Résultats matériels du premier kit](RESULTATS-V1.md)

Les archives précédentes restent présentes pour reproduction historique. L’alerte Windows
sur `pr818-cartes-v2.zip` n’est pas encore diagnostiquée. Son SHA256 attendu est
`abf8e2c1e444f72fb2a9bb4befac0b89aab51ae01298550f72b3b99b21e7f2cf`.
