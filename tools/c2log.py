#!/usr/bin/env python3
"""c2log.py: read the C2's USB debug log (firmware/, a USB serial port: the
wired gamepad 2E8A:10C6, or the log alone: the pad 3537:01AB, the idle
receiver 3537:0575).

  tools/c2log.py [--target pad|receiver] [--device /dev/ttyACMn] [--out FILE]
  tools/c2log.py --boot   --target pad|receiver   enter the USB bootloader
  tools/c2log.py --reset  --target pad|receiver   restart it
The pad and the C2 receiver share the USB IDs; the product string tells them
apart ("C2 receiver" vs the pad's name). --target picks one; without it the
log follows whichever is plugged in, and --boot / --reset refuse to guess
when both are.

Prints each line with the PC's wall-clock time in front of the controller's
own uptime (seconds since boot). When the controller resets or the cable
drops it says so and waits for the port to come back, so a crash shows as
"--- port gone" followed by a new "boot: rst_src .." line. Everything is
also appended to --out (default firmware/build/c2log.txt). Ctrl-C to stop.

Needs read/write access to the tty (the dialout / uucp group, or sudo).
"""
import argparse
import glob
import os
import select
import sys
import termios
import time

# The debug log's USB IDs: log alone (the pad charging 3537:01AB, the
# receiver with no pad 3537:0575), SInput
# gamepad + log, the Xbox modes: the pad wired 3537:1053, the receiver
# 3537:100B (both shown as "Xbox 360 Controller for Windows"), and DS4 mode:
# 054C:09CC "Wireless Controller" on both, told apart by the receiver's
# vendor collection (output report 0x0F) in its HID report descriptor.
IDS = {('3537', '01ab'), ('3537', '0575'), ('2e8a', '10c6'), ('3537', '1053'), ('3537', '100b'), ('054c', '09cc')}
RECEIVER_IDS = {('3537', '100b'), ('3537', '0575')}
DS4_IDS = {('054c', '09cc')}
OUT_REPORT_0F = bytes.fromhex('850f0922953f9102')  # the vendor collection's output report 0x0F
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def usb_product(p):
    try:
        return open(os.path.join(p, 'product')).read().strip()
    except OSError:
        return ''


def has_vendor_report(usb_dev):
    """The USB device's HID interfaces carry the vendor output report 0x0F."""
    for d in glob.glob(os.path.join(usb_dev, '*:*', '*:*.*', 'report_descriptor')):
        try:
            if OUT_REPORT_0F in open(d, 'rb').read():
                return True
        except OSError:
            pass
    return False


def kind(product, vid_pid=None, usb_dev=None):
    if vid_pid in DS4_IDS:
        return 'receiver' if usb_dev and has_vendor_report(usb_dev) else 'pad'
    return 'receiver' if vid_pid in RECEIVER_IDS or 'receiver' in product.lower() else 'pad'


def find_ports(target=None):
    """[(tty, 'pad' | 'receiver', product)] of the debug log ports, target only if given."""
    found = []
    for tty in sorted(glob.glob('/sys/class/tty/ttyACM*')):
        p = os.path.realpath(os.path.join(tty, 'device'))
        while p != '/':
            try:
                vid = open(os.path.join(p, 'idVendor')).read().strip()
                pid = open(os.path.join(p, 'idProduct')).read().strip()
            except OSError:
                p = os.path.dirname(p)
                continue
            if (vid.lower(), pid.lower()) in IDS:
                prod = usb_product(p)
                k = kind(prod, (vid.lower(), pid.lower()), p)
                if target is None or k == target:
                    found.append(('/dev/' + os.path.basename(tty), k, prod))
            break
    return found


def find_port(target=None):
    ports = find_ports(target)
    return ports[0][0] if ports else None


def open_port(dev):
    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    t = termios.tcgetattr(fd)
    t[0] = 0                                    # iflag: raw
    t[1] = 0                                    # oflag
    t[2] = termios.CS8 | termios.CREAD | termios.CLOCAL | termios.HUPCL
    t[3] = 0                                    # lflag: no echo, no canonical
    termios.tcsetattr(fd, termios.TCSANOW, t)  # opening raises DTR: the firmware starts sending
    return fd


def command(dev, cmd):
    fd = open_port(dev)
    os.write(fd, ('\n' + cmd + '\n').encode())   # leading newline: drop any partial line in the firmware's buffer
    time.sleep(0.3)
    os.close(fd)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--device', help='tty (default: find the debug log by its USB IDs)')
    ap.add_argument('--target', choices=('pad', 'receiver'), help='which device (by USB product string)')
    ap.add_argument('--out', default=os.path.join(ROOT, 'firmware/build/c2log.txt'))
    ap.add_argument('--boot', action='store_true', help='enter the USB bootloader')
    ap.add_argument('--reset', action='store_true', help='restart the controller')
    a = ap.parse_args()

    if a.boot or a.reset:
        dev = a.device
        if not dev:
            ports = find_ports(a.target)
            if not ports:
                sys.exit(f'{a.target or "C2"} debug log port not found: USB connected, firmware/ image?')
            if len(ports) > 1:
                sys.exit('more than one debug log port: ' + ', '.join(f'{t} ({k}: {n})' for t, k, n in ports)
                         + '; pick one with --target pad|receiver')
            dev, k, prod = ports[0]
            print(f'{dev}: {k} ({prod})')
        command(dev, 'boot' if a.boot else 'reset')
        print(f'{dev}: sent {"boot" if a.boot else "reset"}')
        return

    out = open(a.out, 'a', buffering=1)

    def emit(s):
        line = f'{time.strftime("%H:%M:%S")} {s}'
        print(line, flush=True)
        out.write(line + '\n')

    emit(f'=== c2log started ({a.out})')
    waiting_said = False
    while True:
        dev = a.device or find_port(a.target)
        if not dev or not os.path.exists(dev):
            if not waiting_said:
                emit('--- waiting for the controller (its USB debug log)')
                waiting_said = True
            time.sleep(0.2)
            continue
        try:
            fd = open_port(dev)
        except OSError as e:
            if not waiting_said:
                emit(f'--- cannot open {dev}: {e}')
                waiting_said = True
            time.sleep(0.5)
            continue
        waiting_said = False
        emit(f'--- port {dev} open')
        buf = b''
        try:
            while True:
                r, _, _ = select.select([fd], [], [], 1.0)
                if not r:
                    continue
                data = os.read(fd, 4096)
                if not data:
                    raise OSError('eof')
                buf += data
                while b'\n' in buf:
                    line, buf = buf.split(b'\n', 1)
                    emit(line.rstrip(b'\r').decode('utf-8', 'replace'))
        except OSError:
            if buf:
                emit(buf.decode('utf-8', 'replace') + '  [cut]')
            emit('--- port gone (reset, crash, or cable out)')
            try:
                os.close(fd)
            except OSError:
                pass


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        pass
