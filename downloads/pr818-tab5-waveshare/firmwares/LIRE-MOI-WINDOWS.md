# Firmwares V2 individuels — Tab5 et Waveshare 7B

Un blocage antivirus Windows a été signalé lors du téléchargement du ZIP V2.
Le nom de la détection n’a pas encore été fourni : la cause reste à établir.
Ne désactivez pas la protection et n’ajoutez pas d’exclusion pour poursuivre.

Les deux fichiers de ce dossier sont les **images ESP32-P4 exactes déjà présentes
dans le ZIP V2**, distribuées séparément pour isoler le fichier concerné.
Elles n’ont pas été modifiées pour changer leur détection. Aucun script Python
du kit ni archive n’est nécessaire avec votre outil USB habituel.

Le contenu de l’archive, les empreintes et les en-têtes/checksums ESP32-P4 ont
été vérifiés. Ces vérifications établissent l’intégrité et le format des fichiers ;
elles ne constituent pas un verdict antivirus et ne prouvent pas un faux positif.
Le kit ne contient pas de programme Windows `.exe` ou `.dll`.

## Choisir le bon fichier

| Carte | Fichier | Taille |
|---|---|---:|
| M5Stack Tab5 d’origine, ILI9881C | `tab5-after_v2.bin` | 2 854 368 octets |
| Waveshare ESP32-P4-WIFI6-Touch-LCD-7B, EK79007 | `waveshare-7b-after_v2.bin` | 2 194 992 octets |

Ces images ciblent les P4 v0.x/v1.x jusqu’à v1.99. Elles contiennent le bootloader,
la table de partitions et l’application : flash **USB à l’adresse 0x0** sur le P4,
avec le même outil que pour le premier kit. Ce ne sont pas des images OTA ESPHome.
L’application actuelle du P4 est remplacée pendant le test ; conservez votre
sauvegarde ou votre firmware habituel pour le restaurer. Le C6 n’est pas flashé.

Après avoir ouvert les journaux avec votre outil habituel, redémarrez le P4 pour
capturer le début. Le banc démarre après 10 secondes, mesure les scènes puis
effectue 16 cycles de démarrage/arrêt. Attendez `PR818_RESTART_OK count=16`,
les 16 résultats PASS correspondants et `PR818_DONE`. Vous pouvez transmettre
les journaux directement pour analyse sans lancer `mesurer.py` sous Windows.

## Vérification des empreintes

Les SHA256 sont disponibles dans `SHA256SUMS.txt`. Dans PowerShell :

```powershell
Get-FileHash .\tab5-after_v2.bin -Algorithm SHA256
Get-FileHash .\waveshare-7b-after_v2.bin -Algorithm SHA256
```

Une empreinte identique confirme l’identité du fichier, pas sa classification
antivirus. Si Windows signale aussi l’un de ces fichiers, conservez le blocage.
Avec Microsoft Defender, le détail est dans **Sécurité Windows → Protection
contre les virus et menaces → Historique de protection**. Relever le nom de
la menace et le fichier concerné permettra d’identifier le problème.

## Provenance

Source corrigée : `youkorr/esp-iot-solution`, commit
`9db972e7008d4c5adab0fde59dc3b0b1343dac4c`, PR Espressif #818.
ESP-IDF 5.5.6, LVGL 9.6.0~1, CPU 360 MHz, PSRAM 200 MHz, L2 128 Kio.
Les chiffres de performance de cette V2 restent à mesurer sur les cartes.
