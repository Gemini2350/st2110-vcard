# Installation auf dem Linux-Host

Gegen Ubuntu 24.04 mit Intel E810 geschrieben. Die Schritte 1–5 sind reines
MTL-Setup und identisch zu dem, was `obs-mtl-st2110` braucht — wer das auf der
Kiste schon durchhat, springt direkt zu **6. st2110-vcard bauen**.

Reihenfolge einhalten: 1–3 brauchen je einen Reboot bzw. einen Treiber-Neubau,
und ohne sie schlägt `mtl_init` fehl.

---

## 1. IOMMU einschalten (Reboot)

Der häufigste Grund, warum später nichts geht. Prüfen:

```bash
ls /sys/kernel/iommu_groups/
```

Ist das leer, fehlt IOMMU. Im BIOS **VT-d** aktivieren, dann:

```bash
sudo sed -i 's/GRUB_CMDLINE_LINUX_DEFAULT="/GRUB_CMDLINE_LINUX_DEFAULT="intel_iommu=on iommu=pt /' /etc/default/grub
sudo update-grub
sudo reboot
```

## 2. Build-Abhängigkeiten

```bash
sudo apt update
sudo apt install -y git gcc g++ cmake meson python3 python3-pip pkg-config \
  libnuma-dev libjson-c-dev libpcap-dev libgtest-dev libssl-dev \
  systemtap-sdt-dev llvm clang flex byacc linuxptp \
  libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev
sudo pip install --break-system-packages pyelftools ninja
```

FFmpeg muss ≥ 5.1 sein (`AVChannelLayout`, `swr_alloc_set_opts2`). Ubuntu 24.04
liefert 6.1 — passt. CMake prüft das und bricht sonst ab.

## 3. E810-Treiber mit MTL-Patches (Neubau nach jedem Kernel-Update)

Der Serien-`ice`-Treiber hat die Rate-Limit-Patches nicht, die MTL für
ST 2110-21 Narrow Pacing braucht. MTL baut ihn selbst:

```bash
cd Media-Transport-Library
sudo ./script/build_ice_driver.sh
```

Zieht ice 2.6.6, patcht, baut, installiert. Kontrolle:

```bash
modinfo ice | grep ^version    # muss "Kahawai_2.6.6" zeigen
```

Wichtig: Ubuntu zieht gern still einen neuen Kernel. Nach jedem Kernel-Update
diesen Schritt wiederholen, sonst lädt der gepatchte Treiber nicht mehr.

## 4. DPDK + MTL bauen

DPDK **nicht** aus der Distribution nehmen — MTL braucht seine eigenen Patches
(aktuell DPDK 26.03, gepinnt in `versions.env`).

```bash
cd Media-Transport-Library
sudo ./script/build_dpdk.sh
./build.sh
sudo ldconfig
pkg-config --modversion mtl     # muss eine Version ausgeben
```

## 5. Hugepages und VF anlegen

```bash
sudo sysctl -w vm.nr_hugepages=4096
sudo modprobe vfio-pci
./script/nicctl.sh list all                    # PF-BDF herausfinden
sudo ./script/nicctl.sh create_vf 0000:4b:00.0 # BDF der PF einsetzen
./script/nicctl.sh list 0000:4b:00.0           # zeigt die VFs
```

Hugepages sind nach einem Reboot weg. Dauerhaft:

```bash
echo "vm.nr_hugepages=4096" | sudo tee /etc/sysctl.d/99-hugepages.conf
```

Die VF-BDF (z. B. `0000:4b:01.0`) kommt gleich in `device.port`.

**Zwei Netze im Kopf behalten:** die VF gehört ab jetzt vfio-pci, der Kernel
sieht sie nicht mehr. `device.local_ip` ist eine reine MTL-Angabe für den
Medienpfad. Web-GUI und NMOS laufen über eine *andere* NIC — deren Adresse
gehört in `nmos.host_address`.

## 6. st2110-vcard bauen

```bash
cd st2110-vcard
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(nproc)
sudo cmake --install build
```

CMake lädt `nlohmann/json` und `cpp-httplib` beim Konfigurieren per
FetchContent — dafür braucht die Kiste einmal Internet. Ohne Internet
stattdessen aus der Distribution nehmen (`sudo apt install nlohmann-json3-dev`)
und die `FetchContent`-Blöcke in `CMakeLists.txt` durch `find_package` ersetzen.

**Erwartung setzen:** die FFmpeg- und HTTP-abhängigen Dateien wurden bisher nie
kompiliert. Rechne beim ersten Durchlauf mit Fehlern in `media/decoder.cpp`,
`media/encoder.cpp`, `monitor/preview.cpp` und `api/http-server.cpp` — meist
Header- oder Signaturkleinigkeiten. Die MTL-abhängigen Dateien sind gegen die
echten Header geprüft.

## 7. NMOS (optional)

`nmos-cpp` bauen und installieren, dann greift `-DENABLE_NMOS=ON` (Default).
Wird `nmos-cpp` nicht gefunden, baut CMake mit einer Warnung ohne NMOS weiter —
alles andere funktioniert dann trotzdem.

`nmos-cpp` benennt zwischen Tags gelegentlich Helper um. `src/nmos/nmos-node.cpp`
ist die Datei, die dann kleine Anpassungen braucht; deshalb einen Tag pinnen und
notieren, welchen.

## 8. PTP

Zwei Wege, sie schließen sich gegenseitig aus:

**a) MTLs eingebauter PTP-Client** — `"ptp": true`, `"phc_device": ""`.
Spricht PTP selbst auf dem Medienport, kein ptp4l nötig. Disziplinert aber auch
die Systemuhr nicht.

**b) ptp4l + phc2sys auf der PHC** — `"phc_device": "/dev/ptp0"`.
Der bessere Weg, wenn die Kiste ohnehin einen PTP-Stack fährt.

```bash
sudo ptp4l -i enp75s0f0 -m -H -f /etc/linuxptp/gPTP.cfg
sudo phc2sys -a -r -m
```

Gegen den GM im Büronetz prüfen; Lock-Status und Offset stehen oben in der GUI.

## 9. Konfigurieren und starten

```bash
sudo mkdir -p /var/lib/st2110-vcard/{media,recordings}
sudo cp config/vcard.example.json /etc/st2110-vcard.json
sudo $EDITOR /etc/st2110-vcard.json
```

Mindestens anzupassen:

| Feld | Wert |
|---|---|
| `device.port` | VF-BDF aus Schritt 5, z. B. `0000:4b:01.0` |
| `device.local_ip` | IP im Medien-VLAN |
| `nmos.host_address` | IP der **Management**-NIC |
| `nmos.registry_address` | `registry.example.com`, Port 8010 |
| `players[].video_dest_ip` / `audio_dest_ip` | Ziel-Multicast-Gruppen |
| `recorders[].video_multicast_ip` / `audio_multicast_ip` | Quell-Gruppen |

Erstmal im Vordergrund starten, damit man die Logs sieht:

```bash
sudo ./build/st2110-vcard -c /etc/st2110-vcard.json -v
```

GUI: `http://<management-ip>:8088`

Läuft es, als Dienst einrichten:

```bash
sudo cp packaging/st2110-vcard.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now st2110-vcard
journalctl -u st2110-vcard -f
```

## 10. Gegenprobe ohne zweite Kiste

Player auf eine Multicast-Gruppe senden lassen und den Recorder auf dieselbe
Gruppe setzen. Dann steht das gesendete Bild als Preview auf der Recorder-Seite —
echter Loopback über den Switch, inklusive IGMP. Alternativ MTLs `RxTxApp` als
Gegenstelle.

## Wenn es klemmt

| Symptom | Ursache |
|---|---|
| `mtl_init failed` | IOMMU aus, Hugepages leer, VF nicht an vfio-pci, oder ein anderer Prozess hält die NIC |
| `Cannot get hugepage information` | `vm.nr_hugepages` nach Reboot wieder auf 0 |
| Bild da, `frames_incomplete` steigt | Paketverlust — erst Switch-Puffer und IGMP-Snooping prüfen, dann `rx_queues` erhöhen |
| `frames_repeated` steigt beim Playout | Decoder kommt nicht nach. `queue_frames` hilft nur gegen Ausreisser; sonst Mezzanine-Codec statt Long-GOP |
| `frames_dropped` steigt bei der Aufnahme | Platte kommt nicht nach — Codec wechseln oder auf NVMe schreiben |
| PTP unlocked | beim eingebauten Client prüfen, ob der PTP-Multicast bis zur VF durchkommt |
| Treiber lädt nach Kernel-Update nicht | Schritt 3 wiederholen |
