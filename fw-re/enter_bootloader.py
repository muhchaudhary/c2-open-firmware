"""Put a USB-connected GameSir C2 into its JieLi UDisk bootloader (4c4a:2342).

Sends the command GameSir Connect's C2 handler uses for reqQuickUpdate
(getQuickUpdateCommand = [0x0F, 0x17, 0x55, 0x88]), padded to the 64-byte output
report 0x0F of the vendor (0xFFF0) interface. ([0x0F, 0xFC] does not work on the C2.)
Also finds the receiver's firmware (1209:0001 in stage A / B, 2E8A:10C6 from stage C,
054C:09CC in DS4 mode; docs/dongle.md): the 0x0F report is what is checked, so the pad's own SInput is not hit. The stock
receiver (3537:0575 / 100B) only obeys it while a stock pad is linked.

  enter_bootloader.py [--target pad|receiver] [--yes]
The pad and the receiver can both offer the interface: --target picks one by
the HID name (a receiver's has "receiver" in it, as "C2 receiver"); without
it, more than one candidate is refused.
"""
import glob
import os
import sys

OUT_REPORT_0F = bytes.fromhex('850f0922953f9102')  # report ID 0x0F, 63-byte output report


def candidates():
    """[(hidraw node, 'pad' | 'receiver', HID name)] with the 0x0F output report."""
    found = []
    for h in sorted(glob.glob('/sys/class/hidraw/hidraw*')):
        uevent = open(os.path.join(h, 'device/uevent')).read()
        ds4 = 'HID_ID=0003:0000054C:000009CC' in uevent   # DS4 mode: only the receiver has the 0x0F report
        if not ds4 and not any(i in uevent for i in ('HID_ID=0003:00003537', 'HID_ID=0003:00001209:00000001',
                                                     'HID_ID=0003:00002E8A:000010C6')):
            continue
        if OUT_REPORT_0F in open(os.path.join(h, 'device/report_descriptor'), 'rb').read():
            name = next((l[9:] for l in uevent.splitlines() if l.startswith('HID_NAME=')), '')
            found.append(('/dev/' + os.path.basename(h), 'receiver' if ds4 or 'receiver' in name.lower() else 'pad', name))
    return found


target = None
if '--target' in sys.argv:
    i = sys.argv.index('--target')
    target = sys.argv[i + 1] if i + 1 < len(sys.argv) else ''
    if target not in ('pad', 'receiver'):
        sys.exit('--target pad or --target receiver')
found = [c for c in candidates() if target is None or c[1] == target]
if not found:
    sys.exit(f'no GameSir vendor HID interface with output report 0x0F found{" for the " + target if target else ""}')
if len(found) > 1:
    sys.exit('more than one: ' + ', '.join(f'{n} ({k}: {name})' for n, k, name in found)
             + '; pick one with --target pad|receiver')
node, k, name = found[0]
if '--yes' not in sys.argv:
    sys.exit(f'would send enter-bootloader to {node} ({k}: {name}); pass --yes to do it')
fd = os.open(node, os.O_WRONLY)
os.write(fd, bytes([0x0F, 0x17, 0x55, 0x88]) + bytes(60))
os.close(fd)
print(f'sent enter-bootloader to {node} ({k}: {name})')
