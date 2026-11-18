#!/usr/bin/env python3
"""c2led.py: set the GameSir C2's LEDs in Steam mode (SInput).

Writes output report 0x03 straight to the pad's hidraw node (docs/sinput.md 2.5):
  03 04 R G B          one colour on Home + both grips (player LEDs keep the number)
  03 80 R G B x8 F     one colour per LED: Home, player 1..4, left grip, right
                       grip, the dock; F = 1 also sets the player LEDs,
                       F = 0 leaves them showing the player number
  03 03 n              player number 1..4 (how many player LEDs are lit)

Usage:
  c2led.py ff0080                       everything magenta
  c2led.py red --left blue --right 00ff00
  c2led.py --home white --left off      slots not given are off
  c2led.py --player 3                   light 3 player LEDs
  c2led.py blue --players red           the 4 player LEDs red, the rest blue
  c2led.py --home white --p1 green --p4 cyan
                                        player LEDs not given are off
  c2led.py off

Steam/SDL re-send their own colour when they open the pad, so run this after
they start (or turn off Steam Input for the pad). The colour lasts until the
next host colour command or a reconnect. While the player LEDs show host
colours, --player only takes effect once a plain colour (03 04) or a per-LED
command without player colours hands them back.

The firmware can't report its colours, so what this PC last sent is kept in
$XDG_STATE_HOME/c2led.json (shared with c2led-ui.py).
"""
import argparse
import glob
import json
import os
import sys

VID, PID = 0x2E8A, 0x10C6
REPORT_LEN = 48

NAMES = {
    'off': '000000', 'black': '000000', 'white': 'ffffff', 'red': 'ff0000',
    'green': '00ff00', 'blue': '0000ff', 'yellow': 'ffff00', 'cyan': '00ffff',
    'magenta': 'ff00ff', 'purple': '8000ff', 'orange': 'ff4000', 'pink': 'ff2060',
}


def colour(s):
    s = NAMES.get(s.lower(), s).lstrip('#')
    if len(s) == 3:
        s = ''.join(c * 2 for c in s)
    try:
        if len(s) != 6:
            raise ValueError
        return bytes.fromhex(s)
    except ValueError:
        raise argparse.ArgumentTypeError(
            f'{s!r}: use RRGGBB, RGB or one of {", ".join(sorted(NAMES))}')


STATE = os.path.join(os.environ.get('XDG_STATE_HOME') or os.path.expanduser('~/.local/state'),
                     'c2led.json')
STATE_KEYS = ('home', 'left', 'right', 'player')


def load_state():
    """What this PC last sent: {'home'|'left'|'right': 'rrggbb' or None, 'player': n or None}."""
    try:
        with open(STATE) as f:
            st = json.load(f)
    except (OSError, ValueError):
        st = {}
    return {k: st.get(k) if isinstance(st, dict) else None for k in STATE_KEYS}


def save_state(**kw):
    st = load_state()
    st.update({k: (v.hex() if isinstance(v, bytes) else v) for k, v in kw.items()})
    try:
        os.makedirs(os.path.dirname(STATE), exist_ok=True)
        tmp = STATE + '.tmp'
        with open(tmp, 'w') as f:
            json.dump(st, f)
        os.replace(tmp, STATE)
    except OSError as e:
        print(f'warning: could not save {STATE}: {e}', file=sys.stderr)


def find_hidraw():
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


def per_led(home, left, right, players=None):
    """Payload of the per-LED command 03 80 (3-byte colours, frame order).
    players: four colours for player LEDs 1..4, or None to leave them to the
    stock player-number display."""
    p = b''.join(players) if players else bytes(12)
    return bytes([0x80]) + home + p + left + right + bytes(3) + bytes([1 if players else 0])


def send(dev, payload):
    report = bytes([0x03]) + payload
    report += bytes(REPORT_LEN - len(report))
    fd = os.open(dev, os.O_WRONLY)
    try:
        os.write(fd, report)
    finally:
        os.close(fd)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('colour', nargs='?', type=colour,
                    help='colour for every slot (RRGGBB, RGB or a name)')
    ap.add_argument('--home', type=colour, help='Home button colour')
    ap.add_argument('--left', type=colour, help='left grip colour')
    ap.add_argument('--right', type=colour, help='right grip colour')
    ap.add_argument('--players', type=colour, metavar='COLOUR',
                    help='colour for all four player LEDs')
    for i in range(1, 5):
        ap.add_argument(f'--p{i}', type=colour, metavar='COLOUR',
                        help=f'player LED {i} colour (overrides --players)')
    ap.add_argument('--player', type=int, choices=range(1, 5),
                    help='number of player LEDs to light (1-4)')
    ap.add_argument('--device', help='hidraw node (default: find 2E8A:10C6)')
    a = ap.parse_args()

    pl = [getattr(a, f'p{i}') for i in range(1, 5)]
    players = None
    if a.players is not None or any(x is not None for x in pl):
        players = [x or a.players or bytes(3) for x in pl]
    per_slot = players is not None or any(x is not None for x in (a.home, a.left, a.right))
    if a.colour is None and not per_slot and a.player is None:
        ap.error('give a colour, --home/--left/--right, --players/--p1..--p4 or --player')

    dev = a.device or find_hidraw()
    if not dev:
        sys.exit('C2 not found: is it on, connected, in Steam mode (Home + Y held 3 s)?')

    try:
        if per_slot:
            base = a.colour or bytes(3)
            home, left, right = (x or base for x in (a.home, a.left, a.right))
            send(dev, per_led(home, left, right, players))
            save_state(home=home, left=left, right=right)
        elif a.colour is not None:
            send(dev, bytes([0x04]) + a.colour)
            save_state(home=a.colour, left=a.colour, right=a.colour)
        if a.player is not None:
            send(dev, bytes([0x03, a.player]))
            save_state(player=a.player)
    except PermissionError:
        sys.exit(f'no write access to {dev} (needs a udev rule or sudo)')
    except OSError as e:
        sys.exit(f'{dev}: {e}')
    print(f'sent to {dev}')


if __name__ == '__main__':
    main()
