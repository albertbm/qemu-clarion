# QY8 helper tools

Local tooling for the Clarion QY8 (Nissan Leaf ZE1) emulator. Boot it with `../../qy8-shell.sh`.

| tool | what it does |
|---|---|
| `exports.py IMAGE MODULE...` | lists a ROM module's exports as `ordinal address name`. Needs a Windows CE ROM extractor that provides `ximg.py` (not included); point `$NANDX` at its directory. |
| `qmp.py SOCK 'hmp cmd'...` | runs monitor commands over a QMP socket, e.g. `screendump out.png -f png` |
| `shell.py SOCK SECS cmd...` | waits for boot output, then types debug-shell commands and prints the replies |
| `tap.py SOCK X Y` | taps screen pixel X,Y (800x480) through QMP input events; the touch panel model turns it into a touch |
| `autoagree.py SRC DST [skip\|timer]` | patches a card image or NAND dump so the telematics consent screen does not stop the boot; see below |
| `ublox.py LAT LON [SPEED_KN] [COURSE]` | stands in for the GNSS receiver so the unit gets a fix; see below |
| `rgba2png.py FILE W H OUT` | turns a frame the GL plugin dumped (`out=DIR`) into a PNG |

## Rebuilding the GL symbol table

`contrib/plugins/qy8gl.syms` holds addresses for nav image `G218ENNI.120` and its NK1. Another
firmware version needs a new table:

```
python3 exports.py G218ENNI.flash.img libGLESv2.dll libEGL.dll  > syms
python3 exports.py NK1.bin coredll.dll | grep -E 'CreateDIBSection|CreateBitmap' >> syms
python3 exports.py NK1.bin gdisub.dll | grep DDWaitForBltDone >> syms
```

`NK1.bin` is the NAND dump from offset `0x1c0000` on. The `eglCreateImageKHR` and `REL` extension addresses come from the name-to-function table in
`libIMGEGL.dll`; `glEGLImageTargetTexture2DOES` is caught at runtime from `eglGetProcAddress`.
The four fragment-shader addresses at the top of the renderer in `qy8gl.c` are the
`glShaderBinary` sources inside `auirtdll.dll`.

## GPS

The unit's GNSS receiver is a u-blox on `SCI2:`. In the emulator SCIF2 is the fourth `-serial`,
index 3, because the launcher's own `-serial mon:stdio` takes index 0, so two nulls fill 1 and 2:

```
QY8_ARGS='-serial null -serial null -serial unix:/tmp/gps.sock,server=on,wait=off' \
    ./qy8-shell.sh NAND CARD
python3 tools/qy8/ublox.py 52.5200 13.4050
```

`QY8_GPS_SOCK` moves the socket if `/tmp/gps.sock` does not suit.

Info > GPS Position then shows 11 satellites and the coordinates, and the map moves there. A
speed in knots and a course make the position walk, so the icon tracks.

Feeding NMEA at the port is not enough, and neither is answering the UBX. `navdrv.dll` only
frames what arrives: `SetGpsData` reads SCI2 into a 128-byte buffer, `CheckOnePacket` cuts one
packet out of it and `SetGpsDataBuff` drops it into a 128-slot ring, while `NAV_Read` and
`NAV_Write` both return -1. Everything goes through `NAV_IOControl`.

`Navi.exe` is the client. It opens `NAV1:` and pumps the device at `0x358314`, up to ten packets
an event, using `0x8011200c` to read, `0x80112010` to write and `0x80112014` for status. Its log
tag is `=SNSW=`. The UBX configuration the receiver sees comes from there: `CFG-NAV5` with
dynModel 4 and fixMode 2, `CFG-GNSS`, four `CFG-MSG`s that enable NAV-POSECEF, NAV-POSLLH and
NAV-VELNED at 1 Hz and disable NMEA-GNS, then `CFG-CFG` and a `MON-VER` poll.

The NMEA handler at `0x357bd0` parses nothing. It appends each sentence to a 1280-byte block and
hands the block to the locator only when one arrives whose id ends in `GLL`, which is the only
NMEA sentence id in `Navi.exe`. A feed without `$GPGLL` grows the block until it overflows and is
discarded, so the locator never sees a sentence. The unit reads that as a sick receiver and
reissues `CFG-RST` on a timer, which looks like a cold-start loop and is only the symptom.

Three limits are worth knowing. A packet over 128 bytes is dropped at three separate checks.
`NavDrv >> GPS:SetGpsDataBuff NAV_BUFFER_FULL` in `gl.log` means the consumer is behind; it
appears once while `Navi.exe` starts and clears by itself. `SckDrv.cpp SckGpsCheck changed OK`
means the port is receiving at all.

`Navi.exe` also bounds the fix by mesh primary from a table at va `0xe6558`, Europe being lon
-20..68, so a position west of 20 W is dropped before the locator sees it and the parser logs
`GPS without Shipment Area!!`. Test inside the area first.

To trace the driver, `QY8_SYMS` takes an extra symbol file and the launcher concatenates it:

```
2000 0xef6b5214 NAV_IOControl
2001 0xef6b7298 SendGpsData
2002 0xef6b6900 SetGpsDataBuff
2003 0xef6b64c8 GetGpsData
```

## The telematics consent screen

Both units stop on it every cold boot and nothing remembers the answer, so an unattended boot
never reaches the map. `autoagree.py` patches `ScreenStateCC.dll` inside NK2 to get past it,
keeping CarWings alive: config code 0x08 stays 1, `Telema.exe` still gets its agree notice, and
NissanConnect stays in the Info menu.

```
python3 tools/qy8/autoagree.py CARD CARD_PATCHED skip
./qy8-shell.sh NAND CARD_PATCHED
```

Patch the card. The nav image runs from there, and patching it alone is enough against a stock
NAND, so the NAND never has to be written. The script reads the build from the .mod name and
knows `G218ENNI.120` and `G214ELNI.062`.

`skip` means the screen never appears. `SK_CWS_AGREE_EXECUTE` and `SK_CWS_AGREE_SKIP` sit side by
side in INI_STARTUP's EventHandler and run the same composite action; the only difference is the
string each writes to `VAR_CWS_AGREE_SKIP`, so whether the screen comes up is that one variable.
The EXECUTE handler is rewritten to write "true" and, in the instructions that frees, to do the
agree itself: `VAR_I_AGREE_STATUS` and `VAR_START_UP_I_AGREE_STATUS` set, then
`ExecuteFunction(25, TELEMA_NOTIFY_AGREE)`. The stock handler spends four instructions reloading
a context it already holds, which is what pays for the additions. 29 instructions in, 29 out.

`timer` leaves the real screen running and has it accept itself, by spending the entry action's
slack on `SetEventTimer(SK_SCT_AGREE_CLICK, 2000)`. The screen shows for about two seconds first.
ZE1 only.

The code section is uncompressed in the ROM but sits inside an LZ4 chunk, so the chunk is
recompressed with `LZ4_compress_HC`, which fits where the stock packer did. Every checksum in the
container is a 32-bit word sum, and the 512-byte .mod header has to stay byte-identical because
the loader only boots the card's copy when it matches the NAND's. The chunk's own sum is
rewritten and the segment's is held constant by a compensating word in the reserved zeros at
MTCP+0x1c. The loader sums the first 0x1e0 bytes of the MTCP header sector plus the chunk
sectors and nothing else, so a word in that sector's tail padding is invisible to it.
