# QY8 bezel keys on the keyboard

The alex emulator build passes host F1-F9 to the guest as display panel micom key
frames (`34 CODE`, bit 7 set while held). They work in a `qy8boot.sh -g` window and
over QMP `input-send-event`. The panel model picks the matrix code from a `panel=jpn|row`
property, set automatically by `qy8boot.sh` from the NAND model code, so a given key
means the same function on every unit even though the wire code differs.
`tools/qy8key.py` sends the same codes by function name through qom-set and reads
`panel` back from the running machine when `--panel` is left at its default, `auto`.

Same function, same key, every unit:

| Key | Function | JPN code (0x10180202) | ROW (EUR, USA, UK) code (0x10180201) |
|-----|----------|-----------------------|--------------------------------------|
| F1 | Menu | 01 | 13 |
| F2, Esc | Back | 03 | 30 |
| F3 | Display off (hold) | 31 | 20 |
| F4 | Map / current position | 32 | 00 |
| F5 | Camera | 13 | 01 |
| F6 | Audio | 30 | 03 |
| F7 | Seek back | 00 | 31 |
| F8 | Seek forward | 20 | 32 |
| F9, Enter | TUNE knob push | 10 | 21 |
| PageUp, wheel up | TUNE turn, one detent back (map: zoom out) | 35 00 81 | 35 81 00 |
| PageDown, wheel down | TUNE turn, one detent (map: zoom in) | 35 00 01 | 35 01 00 |
| = | VOL turn up | 35 01 00 | 35 00 01 |
| - | VOL turn down | 35 81 00 | 35 00 81 |

Any other key is ignored. The measured status of each function, and the
physical bezel layout, are in `REPORT-bezel-keys.md`.

Knob frames carry two detent counts in sign and magnitude: 01 is one detent forward, 81
one back. FF is 127 back and throws the map to its widest scale. kepdrv swaps the two
bytes by panel, so JPN sends `35 VOL TUNE` and row sends `35 TUNE VOL`. PageUp/PageDown
were flipped 21 Oct 2026 so PageUp moves a list highlight up, as the owner expects from
the Japanese unit; on the EUR map this means PageDown zooms in and PageUp zooms out.
Verified on the EUR ZE1 bench (Settings highlight, Esc back) and earlier on the JPN unit
(Settings highlight only; TUNE does not zoom the JPN map).
Over QMP, wheel events need `"device": "qy8-du"`, because the touch model listens on
the display's console and passes the wheel on to the panel.

`python3 tools/qy8key.py SOCK menu` presses by function name, auto-detecting the
table from the running machine. `--panel jpn` or `--panel row` overrides it. A raw hex
matrix code or `frame HEX...` still works, bypassing the function table.

Boot a unit with `tools/qy8boot UNIT` (presets, tab completion) instead of the
long `qy8boot.sh` flag line.
