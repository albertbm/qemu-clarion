# The `clarion-qy7` machine

`clarion-qy7` models the QY7221NL SH-4A unit. The `qemu-clarion` launcher
selects the machine from the NOR image and can keep both the NOR and SD card
unchanged with `--read-only`:

```sh
build-release/qemu-clarion --read-only --headless qy7-nor.bin map-card.img \
    -qmp unix:/tmp/qy7.sock,server=on,wait=off
```

Omit `--headless` to use the display window. The QY7 guest takes longer to
reach its display than the QY8 guest. The machine connects its display
microcontroller to SCIF3; panel input is represented as guest protocol frames.

## Front-panel keys

QEMU key-down and key-up events become command `0x34` frames. The high bit in
the data byte marks key-down; the low seven bits are the panel key code. The
frame uses the display-micom `DLE STX … DLE ETX` framing and BCC, and the model
waits for the guest's `DLE ACK` before completing the key event.

| Host key | Panel control | Panel code | Evidence |
|---|---|---:|---|
| `M` | MAP/VOICE | `0x01` | Active KEPDRV tables map `01 -> 01 -> 0x22`; frame reaches `AuiKeyEvent`. |
| `B` | Brightness/day-night control (inferred) | `0x21` | Active tables map `21 -> 02 -> 0xD9`; this panel code showed the `Lysstyrke` brightness overlay (observed with its previous host alias `S`). |

The active QY7 `kepdrv.dll` table observed after boot has two first-stage
entries, `01 -> 01` and `21 -> 02`, and two second-stage entries,
`01 -> 0x22` and `02 -> 0xD9`. Both codes reach `AuiKeyEvent`; the first
translation is VK `0x22`, while the second is `0xD9`. The displayed brightness
overlay associates code `0x21` with brightness/day-night control, but its
physical button label remains an inference from the panel photo.

The panel photo also shows MENU, STATUS, TUNE/SEEK/CH, FM-AM, XM, CD-AUX,
VOL+/VOL−, CAMERA, OPEN/TILT, power, and Zero Emission. Their protocol codes
have not been established. Do not infer codes for them from their labels.

Send a key through QMP in headless mode:

```json
{"execute":"send-key","arguments":{"keys":[{"type":"qcode","data":"m"}],"hold-time":100}}
```

The same host keys can be pressed in the QEMU display window.

## Raw panel-frame diagnostic

The QY7 display-micom exposes a persistent write-only `key-raw` QOM property
for investigating panel commands whose meaning is unknown. Its value is the
frame body as hexadecimal bytes, not a complete framed packet. The model adds
DLE/STX, DLE escaping, DLE/ETX, and BCC, sends the bytes through SCIF3, and
waits for DLE ACK (retrying up to three times on timeout or NAK).

For example, inject the tested one-byte `0x34` body and a four-byte `0x36`
body:

```json
{"execute":"qom-set","arguments":{"path":"/machine/qy7-dispmicom","property":"key-raw","value":"34 81"}}
{"execute":"qom-set","arguments":{"path":"/machine/qy7-dispmicom","property":"key-raw","value":"36 86 60 01 00"}}
{"execute":"qom-set","arguments":{"path":"/machine/qy7-dispmicom","property":"key-raw","value":"36 83 3B FF 01"}}
{"execute":"qom-set","arguments":{"path":"/machine/qy7-dispmicom","property":"key-raw","value":"36 86 6A 01 00"}}
```

Spaces, commas, and colons may separate bytes. One to 64 bytes are accepted.
The diagnostic does not validate command semantics, so use it with a
read-only guest run. It is enabled only on `clarion-qy7`; QY8 does not enable
the key input or this property.

Dynamic QY7 replay confirmed that each tested subtype `36 86 60 xx yy`,
`36 83 3B FF xx`, and `36 86 6A xx yy` reaches the `lcddrv` key queue,
signals `EVENT_LCD_KEY`, and enters KEPDRV's `InputLcdKey` handler. KEPDRV
routes these to distinct state structures, but their physical meanings
(volume, tune/seek, or channel) are not established. A repeated key-down while
held is suppressed by the model; key-up still emits its release frame.

In the tested headless state, KEPDRV's `AuiKeyEvent` took its `PostMessageW`
path; its `keybd_event` call site was not reached. Statically, a nonzero
readiness flag at `ctx+0x64` selects `PostMessageW`; otherwise
`IsAPIReady(SH_WMGR)` is checked, and when that is false `IsWindowReady`
selects `keybd_event` for a ready window or `PostMessageW` otherwise. Tested
`0x37` caused a black display in the observed state; its semantic meaning is
unknown.
