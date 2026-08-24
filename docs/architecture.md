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

`st20p_tx_get_frame()` mit `ST20P_TX_FLAG_BLOCK_GET` liefert genau einmal pro
Frame-Epoch einen Puffer zurück, abgeleitet aus PTP. Genauso `st30p_tx_get_frame()`
pro Audio-Frameperiode. Die beiden TX-Threads schlafen also nie auf einem Timer –
sie blockieren im Netz-Stack und werden vom PTP-Takt freigegeben.

Daraus folgt der Rest des Designs:

* Der Decoder läuft **voraus** in die Queues und blockiert, wenn sie voll sind
  (`BoundedQueue::push`). Eine Datei hat keinen Takt, PTP schon; die Queue
  absorbiert die Differenz.
* Läuft die Video-Queue leer, wird das **letzte Bild gehalten** statt einen
  Epoch auszulassen. Ein Standbild ist für einen Empfänger harmlos, eine Lücke
  in der RTP-Sequenz nicht. `frames_repeated` zählt das mit.
* Läuft der Audio-FIFO leer, wird mit **digitaler Stille** aufgefüllt
  (`AudioFifo::read_exact` blockiert bewusst nie). `audio_underruns` zählt das.
* Auf der RX-Seite gilt das Gegenteil: die Platte darf den Draht nie bremsen.
  Deshalb `try_push`/`try_write` – wenn der Encoder nicht mitkommt, werden
  Frames verworfen (`frames_dropped`), statt MTLs Empfangspfad zu blockieren
  und daraus Paketverlust für alle zu machen.

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

## Aufnahmeseite: Codecwahl ist die eigentliche Entscheidung

| Format | Datenrate 1080p50 | Datenrate UHDp50 | Anmerkung |
|---|---|---|---|
| Unkomprimiert v210 | ~330 MB/s | ~1,3 GB/s | bit-exakt, braucht NVMe-RAID |
| ProRes 422 HQ | ~25 MB/s | ~90 MB/s | Default, echtzeitfähig |
| FFV1 | variabel | variabel | verlustfrei, CPU-hungrig |
| H.264 | klein | klein | nur Sichtungskopie |

Audio wird immer als PCM 24 bit geschrieben – das ist ein Broadcast-Recorder,
kein Videoplayer.

## Bekannte Lücken (bewusst v0)

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
