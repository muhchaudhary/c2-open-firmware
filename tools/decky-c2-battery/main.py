# C2 Battery's backend: the charging state Steam doesn't pass on.
#
# Steam's ControllerStore has the level of a pad on the C2 receiver but
# leaves bCharging false while it charges (its battery notification never
# says so, 2026-11-24), while Linux has it: the receiver's battery report
# (0x04, docs/sinput.md 2.6) is a HID battery at
# /sys/class/power_supply/hid-<serial>-battery-4, its status "Charging" on
# 5 V. That report is read only while something has the gamepad open; in
# Big Picture Steam does.
import os

RECEIVER = ("2e8a", "10c6")         # the C2 receiver with a pad on it (docs/dongle.md)
POWER_SUPPLY = "/sys/class/power_supply"


def _read(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return None


def _usb_ids(dev):
    """The USB vendor and product of the device a power supply belongs to."""
    p = os.path.realpath(os.path.join(dev, "device"))
    while p != "/":
        vid, pid = _read(os.path.join(p, "idVendor")), _read(os.path.join(p, "idProduct"))
        if vid and pid:
            return vid.lower(), pid.lower()
        p = os.path.dirname(p)
    return None


class Plugin:
    async def receiver_batteries(self):
        """[{capacity: 0..100, charging: bool}] per receiver whose battery is known."""
        out = []
        try:
            names = os.listdir(POWER_SUPPLY)
        except OSError:
            return out
        for name in names:
            if not name.startswith("hid-"):
                continue
            dev = os.path.join(POWER_SUPPLY, name)
            if _usb_ids(dev) != RECEIVER:
                continue
            capacity, status = _read(os.path.join(dev, "capacity")), _read(os.path.join(dev, "status"))
            if capacity is None or status is None:
                continue                     # no battery report yet
            out.append({"capacity": int(capacity), "charging": status in ("Charging", "Full")})
        return out

    async def _main(self):
        pass

    async def _unload(self):
        pass
