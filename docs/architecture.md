# Architektur

## Was das Ding ist

Eine virtuelle SMPTE ST 2110 Karte: ein Linux-Daemon, der Dateien als
ST 2110-20 (Video) und ST 2110-30 (Audio) aussendet, eingehende Streams
anzeigt und aufzeichnet, und sich per NMOS IS-04/IS-05 wie ein Gateway
patchen lässt. Kein Systemgerät (kein v4l2loopback), sondern eine Appliance
mit REST-API und Web-GUI.

```
   Dateien                                        Netz (E810, DPDK)
      │                                                 ▲
      ▼                                                 │
 ┌──────────┐   VideoFrame-Queue    ┌──────────┐        │
 │ Decoder  │ ────────────────────▶ │ Player   │ ── st20p_tx ──┐
 │ (FFmpeg) │   AudioFifo (L24BE)   │          │ ── st30p_tx ──┤
 └──────────┘ ────────────────────▶ └──────────┘        │      │
                                          │             │      │
                                     Preview (MJPEG)    │      │
                                                        │      │
 ┌──────────┐   VideoFrame-Queue    ┌──────────┐        │      │
 │ Encoder  │ ◀──────────────────── │ Recorder │ ◀─ st20p_rx ──┤
 │ (FFmpeg) │   AudioFifo (L24BE)   │          │ ◀─ st30p_rx ──┘
 └──────────┘ ◀──────────────────── └──────────┘
      │                                   │
   Aufnahme                          Preview (MJPEG) + Pegel
```

## Der wichtigste Punkt: wer taktet

**MTL taktet, nicht die Datei und nicht die CPU.**

`st20p_tx_get_frame()` mit `ST20P_TX_FLAG_BLOCK_GET` gibt einen Puffer genau dann
zurück, wenn der vorherige Frame gesendet ist – also einmal pro Frame-Epoch,
abgeleitet aus PTP. Genauso `st30p_tx_get_frame()` pro Audio-Puffer (10 ms).
Die beiden TX-Threads schlafen nie auf einem Timer.

Voraussetzung, und leicht zu übersehen: das Device muss **gestartet** sein.
`mtl_init` allein bewegt kein Paket; ohne `mtl_start()` bzw.
`MTL_FLAG_DEV_AUTO_START_STOP` bleiben die Sender stumm und `get_frame` läuft
einmal pro Sekunde in den Timeout. Der Daemon setzt das Flag in
`MtlDevice`.

Daraus folgt der Rest des Designs:

* Der Decoder läuft **voraus** in die Queues und blockiert, wenn sie voll sind.
  Eine Datei hat keinen Takt, PTP schon; die Queue absorbiert die Differenz.
* Die TX-Threads nehmen **nie blockierend** aus der Queue (`try_pop`,
  `read_exact`). Ein getakteter Thread, der auf den Decoder wartet, verpasst
  seine Epoche.
* Auf der RX-Seite gilt das Gegenteil: die Platte darf den Draht nie bremsen.
  Deshalb `try_push`/`try_write` – kommt der Encoder nicht mit, werden Frames
  verworfen (`frames_dropped`), statt MTLs Empfangspfad zu blockieren.

## Lip-Sync: drei Mechanismen, die zusammengehören

Raten-Gleichlauf liefert der Takt von selbst. Dass Bild und Ton auch
*zueinander* stimmen, nicht:

**1. Gemeinsamer Start (Preroll).** Bei jedem neuen Item und nach jedem Seek
hält der Player das letzte Bild, bis beide Queues gefüllt sind, und gibt dann
Bild und Ton mit *einer* Entscheidung frei. Der Audio-Thread kommt bis zu einem
Puffer später dran und überspringt genau diesen Rückstand, damit das erste
Sample zum ersten Bild passt.

**2. Gleiche Pipeline-Tiefe.** Mit `BLOCK_GET` geht das, was man jetzt füllt,
`framebuff_cnt` Perioden später raus – 60 ms bei drei Video-Puffern in 50p.
Die Zahl der Audio-Puffer wird daraus abgeleitet, sonst läuft der Ton um die
Differenz voraus.

**3. Rückmeldung vom Transport.** MTL meldet im `notify_frame_done` die
PTP-Zeit, zu der ein Frame tatsächlich rausging. Daraus liest der Player:

* *Lücken*: liegt zwischen zwei Frames mehr als eine Periode, hatte der
  Transport für eine Epoche nichts zu senden – der Thread war zu spät. Der
  Inhalt danach ist um genau das verschoben, also wird genau so viel
  übersprungen. Dasselbe, wenn die Queue leer lief (Bild gehalten / Stille):
  die Schuld wird später durch Überspringen beglichen. Ohne das addiert sich
  jeder Aussetzer dauerhaft zum A/V-Versatz.
* *Versatz*: die Zeiten des ersten Video-Frames und des ersten Audio-Puffers
  eines Items ergeben den realen A/V-Versatz am Sender. Er steht als
  `av_skew_ms` im Status und in der GUI.

`av_offset_ms` pro Player trimmt, was eine Messung am echten Netz noch zeigt.

Pause/Resume ist deshalb intern ein Seek auf das gerade gezeigte Bild: die
beiden Threads halten auf verschiedenen Ticks an, und einfaches Weiterlaufen
würde Bild und Ton mit jeder Pause etwas weiter auseinanderschieben.

## Dateien haben krumme Zeitstempel

Der Decoder rechnet alles auf eine Zeitachse um, die bei Dateianfang mit null
beginnt:

* Container-Zeitstempel beginnen selten bei null (Kamerafiles, MXF mit
  Timecode, Transportströme). Ursprung ist `start_time` des Containers –
  gemeinsam für beide Essenzen, damit ihr Versatz zueinander erhalten bleibt.
* Audio wird einmal am Anfang und nach jedem Seek platziert: fängt es später an
  als das Bild, kommt Stille davor; fängt es früher an (nach einem Seek immer,
  der landet auf einem Keyframe davor), wird abgeschnitten.
* Am Dateiende wird der Ton exakt auf die Länge des Bildes gebracht. Dateien
  sind selten so genau geschnitten, und im Loop würde sich die Differenz mit
  jedem Durchlauf aufaddieren.

## Framerate-Konformierung

Die Session läuft auf einer festen Rate. Der Decoder legt jedes dekodierte Bild
auf das Session-Raster: bei 25p-Datei auf 50p-Session wird jedes Bild zweimal
ausgegeben, bei 60p auf 50p werden Slots übersprungen. Damit stimmt die
**Dauer** – ohne das würde eine 25p-Datei auf einer 50p-Session doppelt so
schnell laufen. Der letzte Slot eines Bildes *moved* den Puffer statt ihn zu
kopieren, so dass der 1:1-Fall (korrekt angelegte Datei) ganz ohne memcpy
auskommt; bei UHD wären das sonst 12 MB pro Frame.

## Pixelformat: genau eines

`AV_PIX_FMT_YUV422P10LE` (FFmpeg) und `ST_FRAME_FMT_YUV422PLANAR10LE` (MTL)
haben identisches Speicherlayout. Deshalb gibt es auf dem Medienpfad **keine**
Pixelkonvertierung – nur Zeilenkopien, und auch die entfallen, wenn die Strides
übereinstimmen. Jede Konvertierung hier würde bei UHD mehr kosten als das
Dekodieren. Bitte so lassen.

Audio analog: ST 2110-30 trägt L24 big-endian interleaved, FFmpeg liefert S32
interleaved – eine reine Byte-Umsortierung in `s32_to_l24be()`.

## Confidence-Monitoring

Beide Richtungen haben einen Preview-Pfad, und der Recorder empfängt und zeigt
**auch ohne laufende Aufnahme** – eingehendes Video anzusehen soll nicht
voraussetzen, dass man erst scharf schaltet.

Damit das den Draht nicht ausbremst:

1. `Preview::submit()` läuft auf dem TX/RX-Thread und macht nur eine
   **dezimierende Kopie** (jedes N-te Pixel, 10→8 bit) in einen kleinen Puffer.
   Vorher greift ein Ratenlimit (Default 10 fps), so dass die meisten Frames
   gar nicht angefasst werden.
2. Der JPEG-Encode passiert auf einem eigenen Thread über ein Ein-Slot-Postfach:
   neuestes Bild gewinnt, ältere werden verworfen. Ein Monitor, der hinterherhinkt,
   nützt ohnehin nichts.
3. Die GUI holt das als `multipart/x-mixed-replace`. Egal wie viele Leute
   zusehen – kodiert wird einmal.

Dazu Peak-Pegel pro Kanal (`AudioMeters`) aus demselben Bytestrom, der auf den
Draht geht bzw. von ihm kommt.

## Aufnahme folgt den Zeitstempeln, nicht der Ankunft

MTL liefert RX-Frames mit dem 32-bit-RTP-Zeitstempel und der PTP-Zeit des ersten
Pakets. `rx_sample_time_ns()` verankert den RTP-Wert an der Ankunftszeit (er
läuft sonst alle paar Stunden über, für Video und Audio in verschiedenen
Perioden) und ergibt den Abtastzeitpunkt auf der PTP-Zeitachse – das Einzige,
was Bild und Ton einer 2110-Quelle wirklich gemeinsam haben.

* Nullpunkt der Datei ist das erste aufgenommene Bild. Audio wird darauf
  zugeschnitten oder mit Stille aufgefüllt.
* Video-PTS kommt aus dem Zeitstempel, nicht aus einem Zähler: ein verlorener
  oder verworfener Frame hinterlässt eine Lücke der richtigen Länge.
* Fehlt Audio (Paketverlust, pausierender Sender), wird das Loch als Stille
  geschrieben. Sonst wäre alles danach zu früh. `audio_gap_ms` zählt mit.

## Aufnahmeseite: Codecwahl ist die eigentliche Entscheidung

| Format | Datenrate 1080p50 | Datenrate UHDp50 | Anmerkung |
|---|---|---|---|
| Unkomprimiert v210 | ~330 MB/s | ~1,3 GB/s | bit-exakt, braucht NVMe-RAID |
| ProRes 422 HQ | ~25 MB/s | ~90 MB/s | Default; `prores_ks` ist ein Software-Encoder – ob er UHDp50 in Echtzeit schafft, hängt an der CPU und ist nicht gemessen |
| FFV1 | variabel | variabel | verlustfrei, CPU-hungrig |
| H.264 | klein | klein | nur Sichtungskopie |

Audio wird immer als PCM 24 bit geschrieben – das ist ein Broadcast-Recorder,
kein Videoplayer.

Ein Aufnahmestart wartet, bis die Datei wirklich offen ist, und meldet sonst den
Grund (Encoder fehlt im FFmpeg-Build, Verzeichnis nicht schreibbar, Platte
voll). „REC leuchtet, aber es wird nichts geschrieben" darf ein Recorder nicht
können.

## Hardware: was die NIC bestimmt

Nur **eine** Eigenschaft hängt wirklich an der Karte: ST 2110-21 **Narrow
Pacing** braucht den Hardware-Rate-Limiter, und den nutzt MTL ausschließlich auf
Intel E810/E830 über den DPDK-PMD (deshalb die Patches im ice-Treiber). Überall
sonst — andere DPDK-NICs, Kernel-Socket, AF_XDP — pact MTL über den TSC, und das
ist *wide*.

Deshalb ist `pacing` in der Player-Config kein fester Wert: `"auto"` deklariert
narrow auf dem DPDK-Pfad und wide auf dem Kernel-Pfad. Ein fest verdrahtetes
`TP=2110TPN` im SDP wäre auf anderer Hardware schlicht gelogen, und genau darauf
verlässt sich ein Empfänger beim Dimensionieren seines Puffers.

Für alles andere ist die NIC egal:

| Aufgabe | Anforderung |
|---|---|
| Empfangen, Aufnehmen, Monitoring | beliebige NIC mit genug Durchsatz — hier gibt es kein Pacing |
| Senden, das ein Analyzer als konform abnimmt | E810/E830 + DPDK-PMD |
| Senden für Funktionstests | beliebige NIC, wide pacing, die meisten Empfänger locken trotzdem |
| PTP | NIC mit Hardware-Timestamping (PHC); bei allen Server-NICs dieser Klasse gegeben |
| UHD | 25G+ und DPDK — der Kernel-Pfad trägt das nicht |

## Was geprüft ist und was nicht

`tests/e2e.sh` lässt den echten Daemon gegen einen MTL-Ersatz laufen
(`tests/mock-mtl/`, ein Software-Loopback mit dem Verhalten, auf das sich das
Design stützt): Datei abspielen → Loopback → Preview → Aufnahme, mit Messung
des Lip-Sync über die ganze Kette und mit eingespeisten Fehlern (ausgelassene
Epochen, eingefrorener Prozess). Details in [testing.md](testing.md).

Der NMOS-Node ist gegen `nmos-cpp` gelinkt und läuft im selben Rahmen gegen
Sonys Registry (`tests/e2e-nmos.sh`): Registrierung, SDP, Verbinden, Umlegen,
Trennen, stabile IDs.

**Nicht** geprüft ist alles, was erst mit der echten Bibliothek und einem
echten Netz existiert: Pacing, PTP-Lock, IGMP, Paketverlust, Durchsatz, und ob
MTLs reale Pipeline-Latenzen der Annahme aus Punkt 2 oben entsprechen. Dafür ist
`av_skew_ms` da – der Wert kommt von MTL selbst und lässt sich gegen einen
Analyzer halten.

## Bekannte Lücken (bewusst v0)

* **NMOS**: Unicast-Receiver werden ignoriert; mDNS-Discovery und der
  Zwei-Leg-Betrieb (ST 2022-7) sind ungetestet; eine IS-05-Aktivierung auf einem
  Receiver beendet eine laufende Aufnahme.
* **Farbmetrik** der Aufnahme ist fest BT.709 getaggt; das SDP des Senders wird
  nicht gelesen.

* **ST 2110-40 (ANC/Timecode)** fehlt. Für eine „echte" Karte gehört LTC/ST 12-2
  im ANC-Stream dazu; MTL hat die `st40`-API, die Anbindung ist noch nicht
  gebaut.
* **Frame-Pooling**: `VideoFrame::alloc()` allokiert pro Frame. Bei UHD lohnt
  eine Freiliste, die der Consumer zurückgibt.
* **GPU-/Hardware-Decode** ist nicht angebunden; UHD-Playout aus H.264/HEVC
  hängt damit an der CPU. Mezzanine- oder unkomprimierte Files sind für eine
  Testkarte ohnehin der ehrlichere Weg.
* **ST 2110-22 (JPEG-XS)** wäre über `st22p_*` naheliegend, ist aber nicht
  verdrahtet.
* **Interlaced** ist in den Ops durchgereicht, aber nicht getestet.
* Der NMOS-Node ist aus `obs-mtl-st2110` kopiert. Beide Projekte sollten sich
  diese Datei irgendwann als gemeinsame Bibliothek teilen, statt zwei Kopien
  von Hand synchron zu halten.
