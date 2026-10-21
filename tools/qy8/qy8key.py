#!/usr/bin/env python3
# qy8key.py SOCK KEY [HOLD_MS] [--panel jpn|row|auto] -- press a bezel key through the
# panel micom (/machine/panel-micom). KEY is a function name below, a hex matrix code,
# or 'frame HEX...' for a raw panel frame. --panel picks the matrix code table; default
# auto reads the running machine's own panel=jpn|row property over qom-get.
#
# Functions are the same on every QY8 unit; only the matrix code differs by table.
# See REPORT-bezel-keys.md and QY8KEYS.md.
import json, socket, sys

FUNCS = {
    # name: (jpn code, row code)
    'menu':      ('01', '13'),
    'back':      ('03', '30'),
    'displayoff': ('31', '20'),
    'map':       ('32', '00'),
    'camera':    ('13', '01'),
    'audio':     ('30', '03'),
    'seekback':  ('00', '31'),
    'seekfwd':   ('20', '32'),
    'tune':      ('10', '21'),
}

argv = sys.argv[1:]
panel = 'auto'
if '--panel' in argv:
    i = argv.index('--panel')
    panel = argv[i + 1]
    del argv[i:i + 2]

sock, key = argv[0], argv[1]
hold = argv[2] if len(argv) > 2 and argv[1] != 'frame' else None
frame_arg = argv[2] if len(argv) > 2 and argv[1] == 'frame' else None

s = socket.socket(socket.AF_UNIX); s.connect(sock); f = s.makefile('rw')

def cmd(c, **a):
    f.write(json.dumps({'execute': c, 'arguments': a}) + '\n'); f.flush()
    while True:
        r = json.loads(f.readline())
        if 'return' in r or 'error' in r: return r

json.loads(f.readline()); cmd('qmp_capabilities')

if key == 'frame':
    print(cmd('qom-set', path='/machine/panel-micom', property='frame', value=frame_arg))
    sys.exit()

if key in FUNCS:
    if panel == 'auto':
        r = cmd('qom-get', path='/machine/panel-micom', property='panel')
        panel = r.get('return', 'row')
    if panel not in ('jpn', 'row'):
        sys.exit(f"--panel: expected jpn, row or auto, got {panel!r}")
    code = FUNCS[key][0 if panel == 'jpn' else 1]
else:
    code = key   # raw hex matrix code, bypasses the function table

value = code + (',' + hold if hold else '')
print(cmd('qom-set', path='/machine/panel-micom', property='key', value=value))
