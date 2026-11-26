# Installation auf dem Linux-Host

## Der kurze Weg

```bash
cd st2110-vcard
./install.sh                                   # zeigt nur an, was schon da ist
./install.sh --simple --iface enp3s0f0         # minimal, kein Reboot
```

`install.sh` prüft den Ist-Zustand, installiert nur was fehlt, baut MTL und
den Daemon, reserviert Hugepages dauerhaft und schreibt eine passende
`/etc/st2110-vcard.json`. Mehrfach ausführbar — jeder Schritt prüft erst, ob
er schon erledigt ist.

### Zwei Modi

| | `--simple` | `--full` |
|---|---|---|
| Datenpfad | MTL Kernel-Socket | DPDK PMD auf E810-VF |
| IOMMU / VT-d | nicht nötig | **nötig**, mit Reboot |
| Gepatchter ice-Treiber | nicht nötig | **nötig**, nach jedem Kernel-Update neu |
| VF-Binding an vfio-pci | nicht nötig | nötig |
| NIC | beliebig, mit IP | E810/E830 |
| ST 2110-21 Narrow Pacing | **nein** | ja |
| PTP | ptp4l auf dem Interface, MTL liest die PHC | MTLs eingebauter Client oder ptp4l |
| Sinnvoll für | Bring-up, Funktionstest, 1080p | echter Betrieb, UHD |

`--simple` überspringt die drei Schritte, die tatsächlich weh tun, und braucht
keinen Reboot:

```bash
./install.sh --simple --iface enp3s0f0
```

Das reicht, um Files auf eine Multicast-Gruppe zu spielen, eingehendes Video
anzusehen und aufzunehmen. Was fehlt, ist die harte Paketverteilung im Frame —
der Kernel-Pfad hält kein Narrow Pacing, und ein 2110-Analyzer wird das
anmerken. Für „läuft die Kette überhaupt" ist das egal, für eine Abnahme nicht.

PTP im einfachen Modus: der Kernel behält die NIC, also macht `ptp4l` das PTP
und MTL liest die Hardware-Uhr, die es diszipliniert (`phc_device`). Das Script
trägt die passende `/dev/ptpN` ein und sagt, wenn `ptp4l` nicht läuft. Ohne
laufendes `ptp4l` sind die RTP-Zeitstempel freilaufend – für einen Loopback auf
derselben Kiste egal, gegen fremde Geräte nicht.

Später umsteigen geht ohne Neubau: `--full` laufen lassen und in der Config
`device.port` von `kernel:enp3s0f0` auf die VF-BDF ändern.

```bash
./install.sh --full --pf 0000:4b:00.0
```

Der Rest dieses Dokuments beschreibt dieselben Schritte von Hand — zum
Nachvollziehen und zur Fehlersuche, wenn das Script irgendwo abbricht.

---

# Von Hand

Gegen Ubuntu 24.04 mit Intel E810 geschrieben. Die Schritte 1–5 sind reines
MTL-Setup und identisch zu dem, was `obs-mtl-st2110` braucht — wer das auf der
Kiste schon durchhat, springt direkt zu **6. st2110-vcard bauen**.

Reihenfolge einhalten: 1–3 brauchen je einen Reboot bzw. einen Treiber-Neubau,
und ohne sie schlägt `mtl_init` fehl. **Für `--simple` entfallen 1, 3 und 5** —
dort genügt eine NIC mit IP, Hugepages und `"port": "kernel:<ifname>"`.

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

**Stand der Prüfung:** alles ausser dem NMOS-Node kompiliert warnungsfrei
(GCC 15, FFmpeg 8, gegen die echten MTL-Header) und läuft im Ende-zu-Ende-Test
gegen einen MTL-Ersatz, siehe [testing.md](testing.md). Gegen die **echte**
MTL-Bibliothek gelinkt und auf einer NIC gelaufen ist es noch nie — das hier ist
der erste Versuch. Ubuntu 24.04 bringt FFmpeg 6.1 mit; dagegen ist nicht gebaut
worden, die verwendeten Aufrufe gibt es aber seit 5.1.

Vor dem ersten Start auf Hardware lohnt der Mock-Test auf derselben Kiste: er
zeigt, ob FFmpeg-Build, Codecs und Dateipfade stimmen, bevor MTL ins Spiel
kommt.

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
