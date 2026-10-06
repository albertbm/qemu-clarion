# Clarion QY8XXX in QEMU: building and running

The `clarion_qy8` branch adds the `clarion-qy8` machine: an emulated board
of the Clarion QY8XXX head unit (Renesas R-Car).

No guest image is included in this repository. The machine boots from a
raw 64 MiB flash image that you supply.

Tested on macOS (Apple Silicon). A Linux build should work the same way,
but is not covered here.

## 1. Dependencies

* Xcode Command Line Tools;
* Homebrew: `brew install ninja pkg-config glib pixman`;
* Python 3 (`configure` creates its own venv with meson).

## 2. Building

Two build directories are used. Run the optimized one by default; the
debug build makes the guest noticeably slower.

```sh
# optimized build - use this one to run
mkdir -p build-release && cd build-release
../configure --target-list=arm-softmmu --disable-werror --enable-plugins --disable-docs
ninja qemu-system-arm
cd ..

# debug build - only when you need QEMU assertions and symbols
mkdir -p build && cd build
../configure --target-list=arm-softmmu --disable-werror --enable-debug
ninja qemu-system-arm
```

After a code change, `ninja qemu-system-arm` in the relevant directory is
enough.

## 3. Supplying the flash image and running

The machine has no kernel loader. It takes one raw image of the board's
NOR flash (chip select 0, exactly 64 MiB, no OOB data), maps it at
physical address 0 and starts the CPU from the reset vector. The image is
not parsed in any way. There are two ways to pass it, and exactly one of
them must be given:

| Option | Flash model | Guest writes |
|---|---|---|
| `-drive if=pflash,format=raw,file=IMAGE` | a real NOR chip (`cfi.pflash02`): the guest can read the ID, erase sectors and program words | go into `IMAGE` and survive a restart |
| `-bios IMAGE` | read-only ROM | are not possible; `IMAGE` is never modified |

With `-drive`, work on a copy, because the guest does write to flash:

```sh
cp my-image.bin flash-rw.bin

build-release/qemu-system-arm -M clarion-qy8 \
    -drive if=pflash,format=raw,file=flash-rw.bin -nographic
```

The guest console (SCIF3) is on the terminal. Leave `-nographic` with
`Ctrl-A X`; the QEMU monitor is `Ctrl-A C`.

With the 800x480 display window (macOS):

```sh
build-release/qemu-system-arm -M clarion-qy8,du-dotclk=33333333 \
    -drive if=pflash,format=raw,file=flash-rw.bin \
    -display cocoa,show-cursor=on -serial mon:stdio
```

`show-cursor=on` keeps the host cursor visible: the window hides it when
an absolute pointer device is present.

## 4. Machine properties

Set with `-M clarion-qy8,name=value`; a second `-machine name=value` on
the same command line is merged with the first.

| Property | Default | Purpose |
|---|---|---|
| `dipsw` | `5` | value of the board DIP switches, 0..7 |
| `micom` | `on` | built-in companion MCU on SCIF4; `off` lets you attach your own responder via `-serial` |
| `dispmicom` | `on` | built-in display panel MCU on SCIF1; `off` works the same way |
| `du-dotclk` | `0` | display dot clock in Hz; `0` means no frame tick. Use `33333333` for the window |
| `du-spi` | `31` | GIC line of the display frame interrupt |
| `i2c-empty` | `off` | I2C0..I2C2 as empty buses: an immediate NACK instead of a bus timeout |
| `i2c4` | `on` | Bounded I2C4 controller model at `0xffc73000` (the touchscreen bus) |
| `i2c4-recorder` | `off` | transaction recorder on I2C4, address `0x24` |
| `tma460` | `on` | TMA460 touchscreen controller model on I2C4 (requires `i2c4=on`) |
| `tma460-profile` | `on` | Synthetic TMA460 register profile with pointer-to-touch input |

A typical configuration with touch:

```
-M clarion-qy8,du-dotclk=33333333,i2c-empty=on
```

The touchscreen bus, controller, and synthetic profile are enabled by default.
Disable them explicitly with `i2c4=off,tma460=off,tma460-profile=off` when a
test needs the original no-touch behavior. `i2c-empty` remains opt-in.

## 5. Serial ports and SD cards

**`-serial` order.** The first one is SCIF3 (the console), then SCIF0,
SCIF1, SCIF2, SCIF4, SCIF5, HSCIF0. With `micom=on` and `dispmicom=on`,
SCIF4 and SCIF1 are taken by the built-in models.

**SD cards.** Both slots always exist; without an image a slot is empty.

```sh
-drive if=sd,index=0,format=raw,file=sd.img    # front slot; index=1 is the second one
```

## 6. Pointer input

With `tma460-profile=on`, pointer events are reported only after the
controller exits bootloader and enters working mode. Button transitions are
preserved until the previous report is read; intermediate movement can be
coalesced. Raw report coordinates use the target profile offsets `X + 14`
and `Y + 9`. Y is reported bottom to top (`479 - Y`), because `kepdrv.dll`
mirrors it before it posts the touch to the window.

| Pointer event | TMA460 event ID | Queue state |
|---|---:|---|
| Left button down | 1 | Pressed |
| Movement while held | 2 | Pressed |
| Left button up | 3 | Released |

The same input path can be scripted over QMP (`-qmp
unix:path,server=on,wait=off`), with coordinates on the `0..0x7fff` scale.
The handler is bound to the display, so the events need `"device": "qy8-du"`;
without it QEMU answers "Input handler not found":

```json
{"execute": "qmp_capabilities"}
{"execute": "input-send-event", "arguments": {"device": "qy8-du", "events": [
  {"type": "abs", "data": {"axis": "x", "value": 16383}},
  {"type": "abs", "data": {"axis": "y", "value": 16383}},
  {"type": "btn", "data": {"button": "left", "down": true}}]}}
```

followed by the same event with `"down": false`.

## 7. Diagnostics

**Environment variables** (the main ones):

| Variable | Effect |
|---|---|
| `QY8_SCIF3_TXI=0` | disable the SCIF3 transmit interrupt |
| `QY8_CAN=off` | remove the CAN controller model |
| `QY8_SGX=off` | remove the GPU model |
| `QY8_SGX_EXEC=1`, `QY8_SGX_NULLRENDER=all` | the mode MIRROR needs, see section 8 |
| `QY8_UNIMP_PC=1` | add the guest code address to the unimplemented-peripheral log |

**The usual QEMU tools:**

* `-d unimp -D file` logs accesses to unimplemented devices and the model
  traces;
* `-s -S` waits for gdb/lldb on port 1234. Use hardware breakpoints for
  code that executes from flash;
* `-monitor tcp:127.0.0.1:PORT,server,nowait` gives a monitor, including
  `screendump file.ppm`.

## 8. MIRROR: GPU-rendered content in the window

The GPU model (`hw/display/clarion_sgx.c`) accepts commands but does not
rasterize. MIRROR works around that: a TCG plugin intercepts the guest's
EGL/GLES calls, replays them on the host GPU through ANGLE and puts the
finished frame into the guest display buffer.

MIRROR has three parts, and at the moment they are built separately from
QEMU. Only the first one can be built from this branch alone. The plugin
is tied to the current GPU approach and is expected to change; a shader
translator is planned for this branch so that pre-translated shaders are
no longer needed:

* **ANGLE** - the host EGL/GLES implementation;
* **the renderer** (`libqy8r`) - a thin layer over ANGLE.
  `subprojects/qy8r/` holds a basic version; the plugin needs an extended
  one (texture upload and copy) that is not in this branch yet;
* **the plugin** (`qy8_m138_plugin`) - not in this branch yet. It also
  needs translated guest shaders and their lookup table, which are
  specific to the guest image.

Build steps on macOS:

1. **ANGLE** (Metal backend) from the official sources; `args.gn`:

   ```
   is_debug = false
   is_component_build = false
   symbol_level = 0
   angle_enable_metal = true
   angle_enable_vulkan = false
   angle_enable_gl = false
   angle_enable_swiftshader = false
   angle_build_tests = false
   use_custom_libcxx = false
   treat_warnings_as_errors = false
   ```

   You need `libEGL.dylib` and `libGLESv2.dylib`.

2. **The renderer:**

   ```sh
   clang -dynamiclib -fPIC -O2 -D__APPLE__ -I<dir with qy8r.h> \
       -I<angle>/include qy8r.c -L<angle>/out/Release -lEGL -lGLESv2 \
       -Wl,-rpath,<angle>/out/Release -o libqy8r.dylib
   ```

3. **The plugin** (QEMU must be configured with `--enable-plugins`; the
   plugin headers come from this branch):

   ```sh
   clang -dynamiclib -fPIC -O2 -D__APPLE__ -undefined dynamic_lookup \
       -I include/plugins -I<dir with the shader tables> \
       $(pkg-config --cflags glib-2.0) qy8_m138_plugin.c \
       -o qy8_m138_plugin.dylib $(pkg-config --libs glib-2.0) -ldl
   ```

Running:

```sh
QY8_SGX_EXEC=1 QY8_SGX_NULLRENDER=all \
build-release/qemu-system-arm \
    -M clarion-qy8,du-dotclk=33333333,i2c-empty=on \
    -drive if=pflash,format=raw,file=flash-rw.bin \
    -display cocoa,show-cursor=on -serial mon:stdio \
    -plugin file=qy8_m138_plugin.dylib,out=mirror.calls.jsonl,qy8r=libqy8r.dylib,artifacts=<shaders>,lookup=<table.tsv>,lookup_sha256=<sha256 of the table>,mirror=1,present=1
```

`QY8_SGX_NULLRENDER=all` is a diagnostic mode: the model reports each
render as complete without drawing anything; the pixels come from the
plugin only. Current limitation: only the first few frames are presented;
the guest is not redrawn after that.

## 9. GNSS

The unit's GNSS receiver is a u-blox on `SCI2:`. `tools/qy8/ublox.py` stands in
for it over a socket, and the unit then takes a fix: Info > GPS Position lists 11
satellites and the coordinates, and the map moves there. Give it a speed in knots
and a course and the position walks, so the icon tracks.

SCIF2 is the fourth `-serial` (section 5), so two nulls fill the two in between:

```sh
build-release/qemu-system-arm -M clarion-qy8 \
    -drive if=pflash,format=raw,file=flash-rw.bin \
    -serial mon:stdio -serial null -serial null \
    -serial unix:/tmp/gps.sock,server=on,wait=off

python3 tools/qy8/ublox.py 52.5200 13.4050      # LAT LON [SPEED_KN] [COURSE]
```

`QY8_GPS_SOCK` moves the socket if `/tmp/gps.sock` does not suit.

**What the driver does.** `navdrv.dll` only frames what arrives: `SetGpsData`
reads SCI2 into a 128-byte buffer, `CheckOnePacket` cuts one packet out of it and
`SetGpsDataBuff` drops it into a 128-slot ring, while `NAV_Read` and `NAV_Write`
both return -1. Everything goes through `NAV_IOControl`. The decisions belong to
`Navi.exe`, which opens `NAV1:` and pumps the device at `0x358314`, up to ten
packets an event, using `0x8011200c` to read, `0x80112010` to write and
`0x80112014` for status. Its log tag is `=SNSW=`. The UBX configuration the
receiver sees comes from there: `CFG-NAV5` with dynModel 4 and fixMode 2,
`CFG-GNSS`, four `CFG-MSG`s that enable NAV-POSECEF, NAV-POSLLH and NAV-VELNED at
1 Hz and disable NMEA-GNS, then `CFG-CFG` and a `MON-VER` poll.

**The GLL trigger.** Feeding NMEA at the port is not enough, and neither is
answering the UBX. The NMEA handler at `0x357bd0` parses nothing. It appends each
sentence to a 1280-byte block and hands the block to the locator only when one
arrives whose id ends in `GLL`, which is the only NMEA sentence id in `Navi.exe`.
A feed without `$GPGLL` grows the block until it overflows and is discarded, so
the locator never sees a sentence. The unit reads that as a sick receiver and
reissues `CFG-RST` on a timer, which looks like a cold-start loop and is only the
symptom. `ublox.py` sends `$GPGLL` last in every burst for that reason.

**Limits.** A packet over 128 bytes is dropped at three separate checks.
`Navi.exe` also bounds the fix by mesh primary from a shipment-area table, 16 rows
of lon_max, lon_min, lat_min, lat_max in whole degrees with longitude offset by
-100. The address moves with the build: va `0xe6558` on `G214ELNI.062`, `0xe7ca0`
on `G218ENNI.120`. Europe is lon -20..68, so a position west of 20 W is dropped
before the locator sees it and the parser logs `GPS without Shipment Area!!`. Test inside the area
first. `NavDrv >> GPS:SetGpsDataBuff NAV_BUFFER_FULL` means the consumer is
behind; it appears once while `Navi.exe` starts and clears by itself. `SckDrv.cpp
SckGpsCheck changed OK` means the port is receiving at all.

## 10. Limitations

* The GPU model does not rasterize; GPU-rendered content appears in the
  window only through MIRROR (section 8).
* Not modeled: the CAN bus beyond the controller itself, USB, I2C devices
  other than the TMA460.
