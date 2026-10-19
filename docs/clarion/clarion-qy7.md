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
| `M` | MAP/VOICE | `0x01` | Opens the map after the consent screen; down/up frames were ACKed. |
| `S` | STATUS / settings | `0x02` | Opens the map settings list; down/up frames were ACKed. |

The panel photo also shows MENU, TUNE/SEEK/CH, FM-AM, XM, CD-AUX, VOL+/VOL−,
CAMERA, day/night, OPEN/TILT, power, and Zero Emission controls. Their QY7
protocol codes have not yet been identified. The current host map deliberately
contains only the two observed controls. Do not infer codes for other buttons
from their labels.

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
```

Spaces, commas, and colons may separate bytes. One to 64 bytes are accepted.
The diagnostic does not validate command semantics, so use it with a
read-only guest run. It is enabled only on `clarion-qy7`; QY8 does not enable
the key input or this property.

Dynamic QY7 replay confirmed that the tested `0x34`, `0x37`, and `0x3f`
frames reach the `lcddrv` key queue, signal `EVENT_LCD_KEY`, and enter
KEPDRV's `InputLcdKey` handler. The `0x36 86 60 01 00` body was framed and
ACKed, but did not increase the queue or `InputLcdKey` count, contrary to the
static branch analysis at `lcddrv` VA `0x03CF6198`. In the tested headless
state, KEPDRV's `AuiKeyEvent` took its `PostMessageW` path; its `keybd_event`
call site was not reached. Tested `0x37` and `0x3f` values produced no
persistent visible screen change, so their button meanings remain unknown.
