# Setup auf der Linux-Kiste

Getestet gegen Ubuntu 24.04 mit Intel E810. Der Ablauf entspricht dem aus
`obs-mtl-st2110/docs/setup-linux.md`; wer den schon durchhat, kann direkt zu
„Bauen" springen.

## 1. Pakete

```bash
sudo apt install -y build-essential cmake pkg-config git meson ninja-build \
  python3-pyelftools libnuma-dev libssl-dev libjson-c-dev \
  libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
  linuxptp
```

## 2. DPDK + Media Transport Library

MTL bringt eigene DPDK-Patches mit – DPDK also nicht aus der Distribution
nehmen, sondern nach MTLs Anleitung bauen (`Media-Transport-Library/doc/build.md`).
Danach:

```bash
cd Media-Transport-Library
./build.sh
sudo ldconfig
pkg-config --modversion mtl
```

## 3. Hugepages und NIC-Bindung

```bash
sudo sysctl -w vm.nr_hugepages=4096
sudo modprobe vfio-pci
sudo Media-Transport-Library/script/nicctl.sh create_vf 0000:4b:00.0
```

`create_vf` legt VFs an und bindet sie an vfio-pci. Die BDF einer VF (etwa
`0000:4b:01.0`) kommt in `device.port` der Konfiguration.

Wichtig: die gebundene VF existiert für den Kernel nicht mehr. Die IP in
`device.local_ip` ist eine reine MTL-Angabe. Die **Control-Plane** (Web-GUI,
NMOS) läuft über eine andere NIC – deren Adresse gehört in `nmos.host_address`.

## 4. PTP

Zwei Möglichkeiten, sie schließen sich gegenseitig aus:

**a) MTLs eingebauter PTP-Client** (`"ptp": true`, `"phc_device": ""`).
Spricht PTP selbst auf dem Medienport. Kein ptp4l nötig, disziplinert aber auch
nicht die Systemuhr.

**b) ptp4l + phc2sys auf der PHC** (`"phc_device": "/dev/ptp0"`).
Bevorzugt, wenn die Kiste ohnehin einen PTP-Stack fährt oder andere Software
dieselbe Uhr braucht.

```bash
sudo ptp4l -i enp75s0f0 -m -H -f /etc/linuxptp/gPTP.cfg
sudo phc2sys -a -r -m
```

Gegen den GM im Büronetz prüfen; `ptp_locked` und der Offset stehen in der GUI
oben rechts.

## 5. NMOS (optional)

`nmos-cpp` bauen und installieren, dann `-DENABLE_NMOS=ON` (Default). Ohne
gefundenes `nmos-cpp` baut CMake mit einer Warnung ohne NMOS weiter.

`nmos-cpp` ändert gelegentlich Helper-Namen zwischen Tags. `src/nmos/nmos-node.cpp`
ist die Datei, die dann kleine Anpassungen braucht – deshalb einen Tag pinnen.

## 6. Bauen

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
sudo cmake --install build
```

## 7. Konfigurieren und starten

```bash
sudo mkdir -p /var/lib/st2110-vcard/{media,recordings}
sudo cp config/vcard.example.json /etc/st2110-vcard.json
sudo $EDITOR /etc/st2110-vcard.json      # BDF, IPs, Multicast-Gruppen
sudo ./build/st2110-vcard -c /etc/st2110-vcard.json -v
```

GUI: `http://<management-ip>:8088`

Als Dienst:

```bash
sudo cp packaging/st2110-vcard.service /etc/systemd/system/
sudo systemctl enable --now st2110-vcard
```

## 8. Gegenprobe ohne zweite Kiste

MTLs `RxTxApp` als Gegenstelle auf demselben Host, oder einfacher: einen Player
auf eine Multicast-Gruppe senden lassen und den Recorder auf dieselbe Gruppe
setzen. Dann steht das gesendete Bild als Preview auf der Recorder-Seite –
Loopback über den Switch, inklusive IGMP.

## Wenn es nicht läuft

* `mtl_init failed` → Hugepages leer, VF nicht an vfio-pci gebunden, oder ein
  anderer Prozess hält die NIC.
* Bild da, aber `frames_incomplete` steigt → Paketverlust. Erst Switch-Puffer
  und IGMP-Snooping prüfen, dann `rx_queues` erhöhen.
* `frames_repeated` steigt beim Playout → der Decoder kommt nicht nach.
  `queue_frames` hilft nur gegen Ausreisser, nicht gegen dauerhaft zu langsame
  Dekodierung; dann Mezzanine-Codec statt Long-GOP verwenden.
* `frames_dropped` steigt bei der Aufnahme → die Platte kommt nicht nach.
  Codec wechseln oder auf NVMe schreiben.
* PTP unlocked → beim eingebauten Client prüfen, ob der PTP-Multicast überhaupt
  bis zur VF durchkommt.
