# Testen ohne NIC

```bash
tests/run-in-docker.sh              # v210
tests/run-in-docker.sh prores422hq  # oder ffv1, h264
```

Baut den Daemon mit `-DVCARD_MOCK_MTL=ON` in einem Container und lässt
`tests/e2e.sh` laufen. Braucht nur Docker und einen MTL-Checkout für die Header
(`MTL_DIR=...`, Default `../Media-Transport-Library`) – kein DPDK, keine
Hugepages, läuft auch auf dem Mac.

Direkt auf einer Linux-Kiste mit FFmpeg, curl und jq:

```bash
cmake -B build-mock -DVCARD_MOCK_MTL=ON -DMTL_SOURCE_DIR=../Media-Transport-Library -DENABLE_NMOS=OFF
cmake --build build-mock -j
tests/e2e.sh build-mock/st2110-vcard
```

## Was der Mock ist

`tests/mock-mtl/mock_mtl.cpp` implementiert genau die MTL-Funktionen, die der
Daemon aufruft, als Loopback im Prozess: ein TX gibt Frames im Sessiontakt aus
und reicht sie an jeden RX auf derselben IP:Port. Übersetzt wird gegen die
**echten** MTL-Header, damit er nicht von der API wegdriften kann.

Er bildet die Eigenschaften nach, auf die sich das Design stützt:

* ohne gestartetes Device (`mtl_start` / `DEV_AUTO_START_STOP`) bewegt sich nichts;
* `BLOCK_GET` gibt einen Puffer erst zurück, wenn einer frei ist, Timeout 1 s;
* eine Epoche ohne eingereihten Frame sendet nichts; verschlafene Epochen sind weg;
* ein RX ohne freien Puffer verliert den Frame;
* RX-Frames tragen RTP-Zeitstempel + Ankunftszeit, `notify_frame_done` die Sendezeit.

Fehler lassen sich einspeisen: `SIGUSR1` lässt drei Video-Epochen aus, `SIGUSR2`
vier Audio-Puffer.

## Was der Test prüft

* Bring-up, Status, PTP-Anzeige, GUI wird ausgeliefert
* ein Player ohne Material bleibt mit Schwarz/Stille auf dem Draht
* Auto-Detect des Formats auf der Empfangsseite
* 25p-Datei auf 50p-Session läuft in richtiger Dauer
* Datei, deren Zeitstempel bei 1 h beginnen
* Preview beider Seiten ist ein JPEG, zeigt Bild und bewegt sich; Pegel stimmen
* Aufnahme: Framezahl, PCM-24-Audio, 10 bit 4:2:2, Spuren gleich lang
* **Lip-Sync über die ganze Kette**: ein Clip mit Blitz und Beep wird gespielt,
  aufgenommen und in der Aufnahme vermessen (Grenze ±25 ms)
* **Erholung nach Fehlern**: im Loop werden Video-Epochen und Audio-Puffer
  ausgelassen und der Prozess 0,7 s eingefroren; danach muss der Lip-Sync wieder
  stimmen und das Bild auf der ursprünglichen Zeitachse liegen
* Pause, Resume, Seek, Next, Stop, Playlist; Video-only-Datei
* Fehlermeldungen: Pfad ausserhalb des Medienordners, unbekannter Player,
  Aufnahme, deren Datei sich nicht öffnen lässt
* sauberes Beenden auf SIGTERM

## Was er nicht prüfen kann

Alles Netzseitige: ST 2110-21-Pacing, PTP, IGMP, Paketverlust, Durchsatz,
2022-7. Und ob die echte Bibliothek sich in jedem Punkt so verhält wie der Mock
– der ist aus MTLs Quelltext abgeleitet, aber ein Nachbau. NMOS ist gar nicht
abgedeckt.

Zeitgrenzen sind für eine kleine VM gewählt. Auf einer überlasteten Maschine
können einzelne Zeitchecks kippen, ohne dass etwas kaputt ist; die Ausgabe nennt
dann die langsamsten API-Aufrufe und die relevanten Logzeilen.
