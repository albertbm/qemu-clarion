#!/usr/bin/env python3
"""Stand in for the head unit's u-blox GNSS receiver.

navdrv.dll opens SCI2: and frames what comes back, NMEA and UBX alike, but the
decisions belong to Navi.exe. It writes the UBX configuration through IOCTL
0x80112010 and reads packets back with 0x8011200c.

Two things it insists on. It cold-starts the receiver with UBX-CFG-RST and waits for
an answer, so a port that only listens leaves it resetting forever. Its NMEA handler
at 0x357bd0 buffers sentences into a 1280-byte block and hands that block to the
locator only when a sentence arrives whose id ends in GLL. Without one nothing is
parsed, and the block overflows about once a second.

This answers the UBX, prints a startup banner the way a real module does, and sends
one NMEA burst a second ending in $GPGLL, plus the NAV-POSECEF, NAV-POSLLH and
NAV-VELNED that the unit asks for by CFG-MSG.

    ublox.py LAT LON [SPEED_KN] [COURSE]

The GNSS line is the fourth -serial, which is SCIF2. Two nulls fill the indices the
launcher's own mon:stdio does not:

    QY8_ARGS='-serial null -serial null -serial unix:/tmp/gps.sock,server=on,wait=off'

QY8_GPS_SOCK moves the socket if /tmp/gps.sock does not suit.

Navi.exe bounds the fix by mesh primary from a table at va 0xe6558, Europe being lon
-20..68. A position west of 20 W is dropped before the locator sees it, and the
parser logs "GPS without Shipment Area!!".
"""
import math, os, socket, struct, sys, threading, time

SOCK = os.environ.get('QY8_GPS_SOCK', '/tmp/gps.sock')

UBXNAME = {
    (0x01, 0x02): 'NAV-POSLLH', (0x01, 0x03): 'NAV-STATUS', (0x01, 0x06): 'NAV-SOL',
    (0x01, 0x07): 'NAV-PVT', (0x01, 0x12): 'NAV-VELNED', (0x01, 0x21): 'NAV-TIMEUTC',
    (0x01, 0x30): 'NAV-SVINFO', (0x01, 0x35): 'NAV-SAT', (0x01, 0x20): 'NAV-TIMEGPS',
    (0x02, 0x10): 'RXM-RAW', (0x05, 0x00): 'ACK-NAK', (0x05, 0x01): 'ACK-ACK',
    (0x06, 0x00): 'CFG-PRT', (0x06, 0x01): 'CFG-MSG', (0x06, 0x04): 'CFG-RST',
    (0x06, 0x08): 'CFG-RATE', (0x06, 0x09): 'CFG-CFG', (0x06, 0x13): 'CFG-ANT',
    (0x06, 0x23): 'CFG-NAVX5', (0x06, 0x24): 'CFG-NAV5', (0x06, 0x3e): 'CFG-GNSS',
    (0x0a, 0x04): 'MON-VER',
    (0xf0, 0x00): 'NMEA-GGA', (0xf0, 0x01): 'NMEA-GLL', (0xf0, 0x02): 'NMEA-GSA',
    (0xf0, 0x03): 'NMEA-GSV', (0xf0, 0x04): 'NMEA-RMC', (0xf0, 0x05): 'NMEA-VTG',
    (0xf0, 0x06): 'NMEA-GRS', (0xf0, 0x07): 'NMEA-GST', (0xf0, 0x08): 'NMEA-ZDA',
    (0xf0, 0x09): 'NMEA-GBS', (0xf0, 0x0a): 'NMEA-DTM', (0xf0, 0x0d): 'NMEA-GNS',
    (0xf1, 0x00): 'PUBX-00', (0xf1, 0x01): 'PUBX-01', (0xf1, 0x03): 'PUBX-03',
    (0xf1, 0x04): 'PUBX-04',
}


def ubxname(c, i):
    return UBXNAME.get((c, i), f'cls {c:#04x} id {i:#04x}')


def ck(b):
    a = c = 0
    for x in b:
        a = (a + x) & 0xff; c = (c + a) & 0xff
    return bytes((a, c))

def ubx(cls, idd, payload=b''):
    body = bytes((cls, idd)) + len(payload).to_bytes(2, 'little') + payload
    return b'\xb5\x62' + body + ck(body)

def nmea(body):
    c = 0
    for ch in body:
        c ^= ord(ch)
    return f'${body}*{c:02X}\r\n'

def gsv(n=11):
    """Satellites in view, with SNR. Without these the unit has no signals to trust."""
    sats = [(i + 1, 20 + (i * 7) % 60, (i * 31) % 360, 38 + (i * 3) % 12) for i in range(n)]
    out = ''
    total = (len(sats) + 3) // 4
    for k in range(total):
        chunk = sats[k * 4:(k + 1) * 4]
        body = f'GPGSV,{total},{k + 1},{len(sats):02d}'
        for prn, el, az, snr in chunk:
            body += f',{prn:02d},{el:02d},{az:03d},{snr:02d}'
        out += nmea(body)
    return out


def dm(v, lat):
    h = ('NS' if lat else 'EW')[0 if v >= 0 else 1]
    v = abs(v); d = int(v)
    return f'{d:0{2 if lat else 3}d}{(v - d) * 60:07.4f}', h

def ecef(lat, lon, h):
    """WGS84 geodetic to ECEF, in cm."""
    a, f = 6378137.0, 1 / 298.257223563
    e2 = f * (2 - f)
    la, lo = math.radians(lat), math.radians(lon)
    n = a / math.sqrt(1 - e2 * math.sin(la) ** 2)
    return (int((n + h) * math.cos(la) * math.cos(lo) * 100),
            int((n + h) * math.cos(la) * math.sin(lo) * 100),
            int((n * (1 - e2) + h) * math.sin(la) * 100))


GPS_EPOCH = 315964800          # unix seconds at 1980-01-06
LEAP = 18                      # GPS - UTC, as of 2017

def itow():
    """GPS time of week in ms and the full GPS week number."""
    t = time.time() - GPS_EPOCH + LEAP
    return int((t % 604800) * 1000), int(t // 604800)


class Gps:
    def __init__(self, s, lat, lon, spd, crs):
        self.s, self.lat, self.lon, self.spd, self.crs = s, lat, lon, spd, crs
        self.quiet_until = 0.0

    def banner(self):
        """What a module prints when it comes out of reset."""
        for t in ('u-blox ag - www.u-blox.com',
                  'HW  UBX-G70xx   00070000',
                  'ROM CORE 1.00 (59842) Jun 27 2012 17:43:52'):
            self.s.sendall(nmea(f'GPTXT,01,01,02,{t}').encode())

    def reader(self):
        buf = b''
        while True:
            b = self.s.recv(256)
            if not b:
                return
            buf += b
            while len(buf) >= 8:
                i = buf.find(b'\xb5\x62')
                if i < 0:
                    buf = b''; break
                if len(buf) < i + 6:
                    break
                cls, idd = buf[i + 2], buf[i + 3]
                ln = buf[i + 4] | (buf[i + 5] << 8)
                if len(buf) < i + 8 + ln:
                    break
                buf_payload = buf[i + 6:i + 6 + ln]
                buf = buf[i + 8 + ln:]
                pl = buf_payload
                print(f'  <- {ubxname(cls, idd)} len {ln} {pl.hex()}')
                if (cls, idd) == (0x06, 0x01) and ln >= 3:
                    rates = ' '.join(str(b) for b in pl[2:])
                    print(f'       wants {ubxname(pl[0], pl[1])} at rate(s) {rates}')
                if (cls, idd) == (0x06, 0x04):          # cold start: go quiet, then come back up
                    self.quiet_until = time.time() + 1.0
                    time.sleep(1.0)
                    self.banner()
                    print('  -> reset, banner sent')
                elif cls == 0x06:                        # acknowledge
                    self.s.sendall(ubx(0x05, 0x01, bytes((cls, idd))))
                    print(f'  -> ACK {cls:#04x}/{idd:#04x}')
                elif (cls, idd) == (0x0a, 0x04):          # MON-VER poll
                    self.s.sendall(ubx(0x0a, 0x04,
                                       b'1.00 (59842)'.ljust(30, b'\0') +
                                       b'00070000'.ljust(10, b'\0')))

    def send_ubx(self):
        """The three the unit asked for by CFG-MSG, plus NAV-STATUS and NAV-SOL so the
        fix flag and satellite count land."""
        tow, week = itow()
        lat, lon, alt = self.lat, self.lon, 40.0
        x, y, z = ecef(lat, lon, alt)
        hdg = int(self.crs * 1e5)
        gspd = int(self.spd * 1852 / 3600 * 100)          # knots -> cm/s
        vn = int(gspd * math.cos(math.radians(self.crs)))
        ve = int(gspd * math.sin(math.radians(self.crs)))
        nsv = 11

        posecef = struct.pack('<IiiiI', tow, x, y, z, 300)
        posllh = struct.pack('<IiiiiII', tow, int(lon * 1e7), int(lat * 1e7),
                             int(alt * 1000), int(alt * 1000), 3000, 4000)
        velned = struct.pack('<IiiiIIiII', tow, vn, ve, 0, gspd, gspd, hdg, 50, 200000)
        status = struct.pack('<IBBBBII', tow, 3, 0x0d, 0, 0, 12000, tow)
        sol = struct.pack('<IihBBiiiIiiiIHBBI', tow, 0, week, 3, 0x0d,
                          x, y, z, 300, 0, 0, 0, 50, 150, 0, nsv, 0)
        for cls, idd, pl in ((0x01, 0x01, posecef), (0x01, 0x02, posllh),
                             (0x01, 0x12, velned), (0x01, 0x03, status),
                             (0x01, 0x06, sol)):
            self.s.sendall(ubx(cls, idd, pl))

    def run(self):
        threading.Thread(target=self.reader, daemon=True).start()
        self.banner()
        while True:
            if time.time() >= self.quiet_until:
                self.send_ubx()
                t = time.gmtime()
                hms = time.strftime('%H%M%S', t) + '.00'
                dmy = time.strftime('%d%m%y', t)
                la, lah = dm(self.lat, True)
                lo, loh = dm(self.lon, False)
                self.s.sendall((
                    nmea(f'GPGGA,{hms},{la},{lah},{lo},{loh},1,11,0.9,40.0,M,45.0,M,,')
                    + nmea(f'GPGSA,A,3,01,02,03,04,05,06,07,08,09,10,11,,2.0,0.9,1.2')
                    + gsv()
                    + nmea(f'GPRMC,{hms},A,{la},{lah},{lo},{loh},'
                           f'{self.spd:.2f},{self.crs:.2f},{dmy},,,A')
                    + nmea(f'GPVTG,{self.crs:.2f},T,,M,{self.spd:.2f},N,'
                           f'{self.spd * 1.852:.2f},K,A')
                    + nmea(f'PUBX,00,{hms},{la},{lah},{lo},{loh},40.000,G3,2.0,2.0,'
                           f'{self.spd * 1.852:.3f},{self.crs:.2f},0.000,,0.9,1.2,0.8,11,0,0')
                    # Navi.exe buffers NMEA until a sentence with GLL in it, then hands the
                    # whole burst to the locator -- without one nothing is ever parsed
                    + nmea(f'GPGLL,{la},{lah},{lo},{loh},{hms},A,A')
                ).encode())
                if self.spd:
                    d = self.spd * 1852 / 3600 / 6371000
                    self.lat += math.degrees(d * math.cos(math.radians(self.crs)))
                    self.lon += math.degrees(d * math.sin(math.radians(self.crs)) /
                                             math.cos(math.radians(self.lat)))
            time.sleep(1)

def main():
    a = sys.argv[1:]
    if a and a[0] in ('-h', '--help'):
        sys.exit(__doc__.strip())
    lat = float(a[0]) if a else 52.5200
    lon = float(a[1]) if len(a) > 1 else 13.4050
    spd = float(a[2]) if len(a) > 2 else 0.0
    crs = float(a[3]) if len(a) > 3 else 0.0
    s = socket.socket(socket.AF_UNIX)
    for _ in range(120):
        try:
            s.connect(SOCK); break
        except OSError:
            time.sleep(1)
    else:
        sys.exit(f'no {SOCK} -- is the emulator up with the extra -serial?')
    print(f'u-blox stand-in at {lat:.5f},{lon:.5f}, {spd} kn, course {crs}', flush=True)
    Gps(s, lat, lon, spd, crs).run()

if __name__ == '__main__':
    main()
