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

## Schnellstart

Setzt voraus, dass MTL/DPDK schon steht und eine VF an vfio-pci gebunden ist —
sonst erst [docs/setup-linux.md](docs/setup-linux.md) von vorn.

```bash
cmake -B build && cmake --build build -j
sudo cmake --install build
sudo ./build/st2110-vcard --write-default-config /etc/st2110-vcard.json
sudo $EDITOR /etc/st2110-vcard.json   # VF-BDF, IPs, Multicast-Gruppen
sudo ./build/st2110-vcard -c /etc/st2110-vcard.json -v
```

GUI auf `http://<host>:8088`.

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
