# st2110-vcard

Virtuelle SMPTE ST 2110 Video-/Audiokarte für Linux.

Ein Daemon auf Intel MTL (DPDK), der

* **Dateien abspielt** als ST 2110-20 Video + ST 2110-30 Audio, PTP-getaktet,
  mit Playlist, Loop und Seek,
* **eingehende Streams anzeigt** – Live-Preview und Pegel, auch ohne laufende
  Aufnahme,
* **aufzeichnet** nach ProRes, FFV1, v210 oder H.264 mit PCM-24-Audio,
* sich per **NMOS IS-04/IS-05** von einem Controller patchen lässt,
* über **REST + Web-GUI** bedient wird.

Kein Systemgerät (kein v4l2loopback), sondern eine Appliance. Warum, und wie
das Timing funktioniert, steht in [docs/architecture.md](docs/architecture.md).
Installation: [docs/setup-linux.md](docs/setup-linux.md).

## Installation

```bash
./install.sh                              # zeigt nur an, was schon da ist
./install.sh --simple --iface enp3s0f0    # minimal, kein Reboot
./install.sh --full --pf 0000:4b:00.0     # volles Pacing auf E810
```

`--simple` nutzt MTLs Kernel-Datenpfad: kein IOMMU, kein vfio, kein gepatchter
NIC-Treiber, beliebige NIC. Dafür kein ST 2110-21 Narrow Pacing — gut für
Bring-up und 1080p-Tests, nicht für den Betrieb. Umstieg auf `--full` später
ist ein Config-Feld, kein Neubau.

Details und der Weg von Hand: [docs/setup-linux.md](docs/setup-linux.md).

GUI danach auf `http://<host>:8088`.

## Testen

```bash
tests/run-in-docker.sh         # Medienpfad
tests/run-in-docker.sh nmos    # dazu NMOS gegen Sonys nmos-cpp-registry
```

Baut gegen einen MTL-Ersatz (Software-Loopback) und fährt den echten Daemon
Ende-zu-Ende: abspielen, empfangen, Preview, aufnehmen, Lip-Sync messen, Fehler
einspeisen; im NMOS-Modus zusätzlich registrieren, per IS-05 verbinden, umlegen
und trennen. Kein NIC nötig, läuft auch auf dem Mac. Was das abdeckt und was
nicht: [docs/testing.md](docs/testing.md).

## API

| Methode | Pfad | Zweck |
|---|---|---|
| GET | `/api/status` | Device, PTP, alle Player und Recorder |
| GET | `/api/media` | Dateien in `media_dir`, geprobt |
| GET | `/api/recordings` | Aufnahmen auf Platte |
| POST | `/api/player/{name}/load` | `{"file": "clip.mov"}` |
| POST | `/api/player/{name}/playlist` | `{"items": ["a.mov", "b.mov"]}` |
| POST | `/api/player/{name}/play\|pause\|stop\|next\|previous` | Transport |
| POST | `/api/player/{name}/seek` | `{"seconds": 12.5}` |
| POST | `/api/player/{name}/loop` | `{"enabled": true}` |
| POST | `/api/recorder/{name}/start` | `{"filename": "take1.mov"}` (optional) |
| POST | `/api/recorder/{name}/stop` | Aufnahme beenden |
| GET | `/api/preview/{player\|recorder}/{name}/mjpeg` | Live-Bild als MJPEG |
| GET | `/api/preview/{player\|recorder}/{name}/jpeg` | Einzelbild |
| GET | `/api/log` | letzte Logzeilen |

Im Status pro Player u. a. `frames_repeated`, `audio_underruns`,
`video_epochs_skipped`, `audio_gap_ms` und `av_skew_ms` (A/V-Versatz am Sender
laut MTL); pro Recorder `frames_incomplete`, `frames_dropped`, `audio_gap_ms`.

Eingehendes Bild kurz ansehen, ohne GUI:

```bash
curl -s http://host:8088/api/preview/recorder/recorder1/jpeg > frame.jpg
```

## Verwandt

Teilt Kern-Code mit [obs-mtl-st2110](../obs-mtl-st2110) (MTL-Device,
Format-Mapping, NMOS-Node). Beide sollten sich das perspektivisch als
gemeinsame Bibliothek teilen.

## Lizenz

GPL-2.0-or-later.
