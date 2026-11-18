#!/usr/bin/env python3
"""c2cfg.py: configure the C2 (this project's firmware) over its configuration
protocol (docs/config-protocol.md): over Bluetooth or USB in Steam mode, or
over the USB log port (the pad's cable or the receiver) in any mode.

  tools/c2cfg.py info                  firmware, board, link, battery
  tools/c2cfg.py list                  every setting: value, range, default
  tools/c2cfg.py get NAME|KEY
  tools/c2cfg.py set NAME|KEY VALUE    saved on the pad 2 s later; map_* take button
                                       names (set map_a b, set map_paddle_l none)
  tools/c2cfg.py defaults              all settings back to their defaults
  tools/c2cfg.py reboot
  tools/c2cfg.py bootloader            the USB bootloader (cable needed)
  tools/c2cfg.py cal [PARTS]           guided calibration (PARTS: sticks,triggers,gyro; default all)
  tools/c2cfg.py cal show              the stored calibration
  tools/c2cfg.py cal clear [PARTS]     back to learning at runtime
  tools/c2cfg.py cal start|status|finish|cancel [PARTS]   the steps one by one
  options: --device /dev/hidrawN | --serial [/dev/ttyACMn] [--target pad|receiver]

Talks to the pad's hidraw node (2E8A:10C6): requests go out as SInput
output report 0x03 with command 0xC0, replies come back in input report
0x02 with the same first byte. Works while SDL or Steam has the pad open.
Without one (Xbox, DS4 or Switch mode), or with --serial, it uses the debug log port
instead: "cfg <hex>" out, "cfg> <hex>" back. Only one program can read
that port: stop tools/c2log.py first.
"""
import argparse
import glob
import os
import re
import select
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import c2log  # noqa: E402  (the log port: find_ports, open_port)

VID, PID = 0x2E8A, 0x10C6
CMD = 0xC0
OP_INFO, OP_LIST, OP_GET, OP_SET, OP_DEFAULTS, OP_REBOOT, OP_BOOTLOADER = range(7)
OP_CAL_START, OP_CAL_STATUS, OP_CAL_FINISH, OP_CAL_CANCEL, OP_CAL_CLEAR, OP_CAL_GET = range(0x10, 0x16)
CAL_PARTS = {'sticks': 1, 'triggers': 2, 'gyro': 4}
CAL_PHASE = {0: 'idle', 1: 'rest', 2: 'range', 3: 'done'}
AXES = ['LX', 'LY', 'RX', 'RY']
STATUS = {0: 'ok', 1: 'unknown operation', 2: 'unknown setting', 3: 'value out of range',
          4: 'not possible now', 5: 'request too short',
          6: 'done, but NOT saved (the store write failed): in use until power-off only'}
MODES = {0: 'Steam (SInput)', 1: 'Xbox', 2: 'DS4 (DualShock 4)', 3: 'Switch (Pro Controller)'}
PLUG = {2: 'charging', 3: 'full', 4: 'on battery'}
F_RESTART, F_PROFILE = 0x01, 0x02
BUTTONS = ['a', 'b', 'x', 'y', 'up', 'down', 'left', 'right', 'l3', 'r3', 'lb', 'rb',
           'paddle_l', 'paddle_r', 'start', 'back', 'guide', 'capture']   # map targets; len = none
CURVES = ['linear', '^1.5', '^2', '^3']


def find_device():
    for d in sorted(glob.glob('/sys/class/hidraw/hidraw*')):
        try:
            with open(os.path.join(d, 'device', 'uevent')) as f:
                ev = dict(l.strip().split('=', 1) for l in f if '=' in l)
        except OSError:
            continue
        _, vid, pid = ev.get('HID_ID', '0:0:0').split(':')
        if int(vid, 16) == VID and int(pid, 16) == PID:
            return '/dev/' + os.path.basename(d)
    return None


class Pad:
    def __init__(self, dev):
        self.fd = os.open(dev, os.O_RDWR | os.O_NONBLOCK)
        self.seq = int(time.time()) & 0xFF

    def exchange(self, body, op, timeout):
        """One try over hidraw: the reply report (from its ID), or None."""
        os.write(self.fd, bytes([0x03]) + body + bytes(47 - len(body)))
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            r, _, _ = select.select([self.fd], [], [], 0.02)
            if not r:
                continue
            try:
                d = os.read(self.fd, 64)
            except BlockingIOError:
                continue
            if len(d) >= 5 and d[0] == 0x02 and d[1] == CMD and d[2] == self.seq and d[3] == op:
                return d
        return None

    def request(self, op, payload=b'', timeout=0.4, tries=4, check=True):
        """Send a request, wait for the reply with the same seq. Over
        Bluetooth a request can take ~200 ms to reach the pad (more with
        Bluetooth audio on the same PC), so it is resent a few times."""
        self.seq = (self.seq + 1) & 0xFF
        body = bytes([CMD, self.seq, op]) + payload
        for _ in range(tries):
            d = self.exchange(body, op, timeout)
            if d is None:
                continue
            status = d[4]
            if not check:
                return status, d[5:]
            if status:
                raise SystemExit(f'pad: {STATUS.get(status, status)}')
            return d[5:]
        raise SystemExit(f'no reply from the pad ({self.hint})')

    hint = 'Steam mode? connected?'


    def items(self):
        n = self.info()['settings']
        out = []
        for i in range(n):
            p = self.request(OP_LIST, bytes([i]))
            key, flags, lo, hi, dflt, val = struct.unpack_from('<BBHHHH', p)
            name = p[10:34].split(b'\0')[0].decode()
            out.append(dict(key=key, flags=flags, min=lo, max=hi, default=dflt, value=val, name=name))
        return out

    def info(self):
        p = self.request(OP_INFO)
        return dict(proto=p[0], fw=p[1:17].split(b'\0')[0].decode(), board=p[17:33].split(b'\0')[0].decode(),
                    mode=p[33], link='USB' if p[34] else 'Bluetooth', mv=p[35] | p[36] << 8,
                    percent=p[37], plug=p[38], settings=p[39], profile=p[40],
                    release=f'{p[43]}.{p[44]}.{p[45]}' if p[0] >= 3 else None)


class SerialPad(Pad):
    """The same protocol over the debug log port (pad on the cable, or the
    receiver, any mode): "cfg <hex>" out, a line "cfg> <hex>" back, among
    the log's other lines. Trailing zeros are left out both ways."""
    REPLY = re.compile(rb'cfg> ([0-9a-f]+)')
    hint = 'pad connected? another program reading the log port?'

    def __init__(self, dev):
        self.fd = c2log.open_port(dev)
        self.seq = int(time.time()) & 0xFF
        self.buf = b''
        time.sleep(0.1)                                  # opening raises DTR: let the firmware start sending
        self.drain()

    def drain(self):
        while select.select([self.fd], [], [], 0)[0]:
            try:
                if not os.read(self.fd, 4096):
                    break
            except BlockingIOError:
                break
        self.buf = b''

    def exchange(self, body, op, timeout):
        body += bytes(47 - len(body))                    # whole, as over HID (protocol 3 firmware pads, older didn't)
        os.write(self.fd, b'\ncfg ' + body.hex().encode() + b'\n')
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            r, _, _ = select.select([self.fd], [], [], 0.02)
            if not r:
                continue
            try:
                self.buf += os.read(self.fd, 4096)
            except BlockingIOError:
                continue
            while b'\n' in self.buf:
                line, self.buf = self.buf.split(b'\n', 1)
                m = self.REPLY.search(line)
                if not m:
                    continue
                d = b'\x02' + bytes.fromhex(m.group(1).decode())
                d += bytes(64 - len(d))
                if d[1] == CMD and d[2] == self.seq and d[3] == op:
                    return d
        return None


def parts_mask(args):
    if not args:
        return 7
    m = 0
    for a in ','.join(args).split(','):
        if a not in CAL_PARTS:
            raise SystemExit(f'unknown part {a!r} (sticks, triggers, gyro)')
        m |= CAL_PARTS[a]
    return m


def parts_names(m):
    return ', '.join(n for n, b in CAL_PARTS.items() if m & b) or 'none'


def cal_status(pad):
    p = pad.request(OP_CAL_STATUS)
    st = dict(phase=p[0], mask=p[1], stored=p[2], progress=p[3], restarts=p[4], short=p[5])
    v = struct.unpack_from('<16H6H3h', p, 6)
    st['sticks'] = [v[4 * i:4 * i + 4] for i in range(4)]        # now, min, centre, max
    st['trig'] = [v[16 + 3 * i:16 + 3 * i + 3] for i in range(2)]  # now, rest, pressed
    st['gyro'] = v[22:25]
    return st


def show_status(st):
    print(f"phase {CAL_PHASE.get(st['phase'], st['phase'])}, parts {parts_names(st['mask'])}, "
          f"rest {st['progress']} % ({st['restarts']} restarts), short: {parts_names(st['short'])}")
    for n, (now, lo, c, hi) in zip(AXES, st['sticks']):
        print(f"  {n}: now {now:5}  centre {c:5}  travel -{c - lo:5} / +{hi - c:5}")
    for n, (now, rest, pr) in zip(['LT', 'RT'], st['trig']):
        print(f"  {n}: now {now:4}  rest {rest:4}  travel {rest - pr:4}")
    print(f"  gyro bias {st['gyro']}")


def cal_guided(pad, mask):
    pad.request(OP_CAL_START, bytes([mask]))
    print(f"Calibrating {parts_names(mask)}.")
    print("Step 1: let go of the sticks and triggers and lay the pad flat, still.")
    last = None
    while True:
        st = cal_status(pad)
        if st['phase'] != 1:
            break
        if (st['progress'], st['restarts']) != last:
            last = (st['progress'], st['restarts'])
            print(f"\r  resting {st['progress']:3} %  (restarted {st['restarts']}x: keep it still)", end='', flush=True)
        time.sleep(0.1)
    print('\r  rest done.' + ' ' * 40)
    if st['phase'] == 2:
        print("Step 2: roll both sticks around their edge a few times, and press both triggers")
        print("all the way. Press Enter when done.")
        import threading
        done = threading.Event()
        threading.Thread(target=lambda: (sys.stdin.readline(), done.set()), daemon=True).start()
        while not done.is_set():
            st = cal_status(pad)
            stick = min(min(c - lo, hi - c) for (_, lo, c, hi) in st['sticks'])
            trig = min(rest - pr for (_, rest, pr) in st['trig'])
            print(f"\r  sticks: least travel {stick:5} (need 6000)   triggers: least {trig:4} (need 200)  ",
                  end='', flush=True)
            time.sleep(0.15)
        print()
    while True:
        status, p = pad.request(OP_CAL_FINISH, check=False)
        if status == 0:
            print(f"Stored. Calibrated now: {parts_names(p[1])}.")
            return
        if status == 6:
            raise SystemExit(f"In use now ({parts_names(p[1])}), but NOT saved: the pad's store write "
                             f"failed (see its log). Run the calibration again.")
        if status == 3:
            ans = input(f"Not far enough: {parts_names(p[0])}. Move them further and press Enter "
                        f"(or q to cancel): ")
            if ans.strip().lower() == 'q':
                pad.request(OP_CAL_CANCEL)
                print('cancelled, nothing stored')
                return
            continue
        raise SystemExit(f'pad: {STATUS.get(status, status)}')


def cal_show(pad):
    p = pad.request(OP_CAL_GET)
    print(f"stored: {parts_names(p[0])}")
    v = struct.unpack_from('<12H4H3h', p, 1)
    if p[0] & 1:
        for i, n in enumerate(AXES):
            c, pos, neg = v[3 * i:3 * i + 3]
            print(f"  {n}: centre {c:5}  travel -{neg:5} / +{pos:5}")
    if p[0] & 2:
        for i, n in enumerate(['LT', 'RT']):
            print(f"  {n}: rest {v[12 + 2 * i]:4}  travel {v[13 + 2 * i]:4}")
    if p[0] & 4:
        print(f"  gyro bias {v[16:19]}")


def show_value(name, v):
    if name.startswith('map_'):
        return BUTTONS[v] if v < len(BUTTONS) else 'none'
    if name == 'stick_curve':
        return CURVES[v] if v < len(CURVES) else str(v)
    if name == 'profile':
        return str(v + 1)
    return str(v)


def parse_value(name, text):
    if name.startswith('map_') and not text.isdigit():
        if text == 'none':
            return len(BUTTONS)
        if text not in BUTTONS:
            raise SystemExit(f'unknown button {text!r} ({", ".join(BUTTONS)}, none)')
        return BUTTONS.index(text)
    if name == 'stick_curve' and text in CURVES:
        return CURVES.index(text)
    if name == 'profile':
        return int(text) - 1                    # profiles are 1..4 to people
    return int(text)


def key_of(pad, name):
    if name.isdigit():
        return int(name)
    n = pad.info()['settings']
    for i in range(n):                          # stop at the first match: a LIST each
        p = pad.request(OP_LIST, bytes([i]))
        if p[10:34].split(b'\0')[0].decode() == name:
            return p[0]
    raise SystemExit(f'unknown setting {name!r} (see: list)')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--device', help='hidraw node (default: find 2E8A:10C6)')
    ap.add_argument('--serial', nargs='?', const='auto', help='use the debug log port (default: find it)')
    ap.add_argument('--target', choices=('pad', 'receiver'), help='which log port, with both plugged in')
    ap.add_argument('cmd', choices=['info', 'list', 'get', 'set', 'defaults', 'reboot', 'bootloader', 'cal'])
    ap.add_argument('args', nargs='*')
    a = ap.parse_args()
    dev = None if a.serial else (a.device or find_device())
    if dev:
        pad = Pad(dev)
    else:
        port = a.serial if a.serial and a.serial != 'auto' else None
        if not port:
            ports = c2log.find_ports(a.target)
            if len(ports) > 1 and not a.target:
                ports = [p for p in ports if p[1] == 'pad'] or ports   # the pad on its cable answers itself
            if not ports:
                raise SystemExit('no C2 found: no SInput HID device (Steam mode) and no log port (USB)')
            port = ports[0][0]
            dev = f'{port} ({ports[0][1]} log port)'
        pad = SerialPad(port)
        dev = dev or port
    if a.cmd == 'info':
        i = pad.info()
        rel = f"{i['release']} ({i['fw']})" if i['release'] else i['fw']
        print(f"{dev}: {i['board']}, firmware {rel}, protocol {i['proto']}")
        print(f"mode {MODES.get(i['mode'], i['mode'])}, over {i['link']}")
        print(f"battery {i['mv']} mV, {i['percent']} %, {PLUG.get(i['plug'], i['plug'])}")
        if i['proto'] >= 2:
            print(f"profile {i['profile'] + 1} active (per-profile settings below are its own)")
    elif a.cmd == 'list':
        for it in pad.items():
            extra = ' (restarts the pad)' if it['flags'] & F_RESTART else ''
            extra += ' [profile]' if it['flags'] & F_PROFILE else ''
            rng = f"{show_value(it['name'], it['min'])}..{show_value(it['name'], it['max'])}"
            if it['name'].startswith('map_'):
                rng = 'a button or none'
            print(f"{it['key']:#04x}  {it['name']:<20} {show_value(it['name'], it['value']):>8}   "
                  f"range {rng}, default {show_value(it['name'], it['default'])}{extra}")
    elif a.cmd == 'get':
        if len(a.args) != 1:
            raise SystemExit('get NAME|KEY')
        p = pad.request(OP_GET, bytes([key_of(pad, a.args[0])]))
        print(show_value(a.args[0], p[1] | p[2] << 8))
    elif a.cmd == 'set':
        if len(a.args) != 2:
            raise SystemExit('set NAME|KEY VALUE')
        name = a.args[0]
        v = parse_value(name, a.args[1])
        p = pad.request(OP_SET, bytes([key_of(pad, name)]) + struct.pack('<H', v))
        print(show_value(name, p[1] | p[2] << 8))
    elif a.cmd == 'defaults':
        pad.request(OP_DEFAULTS)
        print('defaults restored')
    elif a.cmd == 'reboot':
        pad.request(OP_REBOOT)
        print('restarting')
    elif a.cmd == 'cal':
        sub = a.args[0] if a.args and a.args[0] in ('show', 'clear', 'start', 'status', 'finish', 'cancel') else None
        rest = a.args[1:] if sub else a.args
        if sub is None:
            cal_guided(pad, parts_mask(rest))
        elif sub == 'show':
            cal_show(pad)
        elif sub == 'clear':
            p = pad.request(OP_CAL_CLEAR, bytes([parts_mask(rest)]))
            print(f"cleared; still calibrated: {parts_names(p[0])}")
        elif sub == 'start':
            p = pad.request(OP_CAL_START, bytes([parts_mask(rest)]))
            print(f"started: {parts_names(p[0])} (rest step: keep it still)")
        elif sub == 'status':
            show_status(cal_status(pad))
        elif sub == 'finish':
            status, p = pad.request(OP_CAL_FINISH, check=False)
            if status == 0:
                print(f"stored; calibrated: {parts_names(p[1])}")
            elif status == 6:
                raise SystemExit(f"in use ({parts_names(p[1])}), but NOT saved: the store write failed")
            elif status == 3:
                raise SystemExit(f"not far enough: {parts_names(p[0])}")
            else:
                raise SystemExit(f'pad: {STATUS.get(status, status)}')
        elif sub == 'cancel':
            pad.request(OP_CAL_CANCEL)
            print('cancelled')
    elif a.cmd == 'bootloader':
        pad.request(OP_BOOTLOADER)
        print('entering the USB bootloader (then: sudo fw-re/jlflash.sh write FLASH.bin)')


if __name__ == '__main__':
    main()
