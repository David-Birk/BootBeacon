# BootBeacon

Diagnose-Kext (Lilu-Plugin) für ThinkPads, die beim Hackintosh-Boot ohne Bild einfrieren.

| Signal | Bedeutung |
|---|---|
| 1 Piep + Power-LED blinkt langsam (1 s) | Kext läuft, EC erreichbar |
| hoher Piep + LED blinkt mittel (0,4 s) | Root-Dateisystem gemountet |
| 3 kurze Pieps + LED blinkt schnell (0,15 s) | launchd (PID 1) läuft |
| hoch-tief-Piep | NVRAM-Schreiben fehlgeschlagen |
| **LED bleibt stehen** | **Kernel ist in diesem Moment eingefroren** |

Außerdem schreibt die Kext alle 10 s die letzten ~3 KB des Kernel-Logs in die NVRAM-Variable
`7C436110-AB2A-4BBB-A880-FE41995C9F82:bootbeacon-log`. Nach dem Freeze unter Linux mit
`sudo tools/bblog.sh` auslesen. Die erste Zeile zeigt Stufe, Uptime und Anzahl der Einträge.

## Bauen
Auf GitHub pushen, dann baut der Workflow die Kext unter *Actions → Build BootBeacon → Artifacts*.
Lokal auf einem Mac: `./build.sh`.

## Einbauen (OpenCore)
`BootBeacon.kext` nach `EFI/OC/Kexts/` kopieren. In der config.plist unter Kernel → Add
**direkt nach Lilu** eintragen (BundlePath `BootBeacon.kext`, ExecutablePath `Contents/MacOS/BootBeacon`,
PlistPath `Contents/Info.plist`, MinKernel/MaxKernel leer, Enabled true).

## Boot-Args
`-bbeacoff` aus · `-bbnobeep` stumm · `-bbnonvram` kein NVRAM · `bbled=0x401` LED-Maske
(Bit 0 = Power-LED, Bit 10 = i-Punkt im Deckel) · `bbint=10` Intervall in Sekunden · `bbmax=90` max. Schreibvorgänge · `bbsize=3072` Größe des Log-Ausschnitts.
