# PC tools

Python 3 (standard library), no installs, except where noted. On Linux the
controller's and receiver's HID nodes (`2E8A:10C6`) are user-accessible; the
USB serial (debug log) ports need the `dialout` (or `uucp`) group. Each
script's `--help` (or its header) has the details.

## Configuration

- **`c2cfg.html`**: the configurator in the browser (Chrome or Edge):
  settings, profiles with response plots, button remap and turbo,
  calibration, device info, the debug log. Connects over WebHID in Steam
  mode (Bluetooth, USB, the receiver) or WebSerial over the debug log port
  in any mode. Open the file, or serve it:
  `python3 -m http.server -d tools 8765` → `localhost:8765/c2cfg.html`.
- **`c2cfg.py`**: the same from the command line (`info`, `list`, `get`,
  `set`, `defaults`, `reboot`, `bootloader`, `cal …`); finds the SInput
  device, else the log port (`--serial`, `--target pad|receiver`).
  The protocol: `docs/config-protocol.md`.

## Debugging and measuring

- **`c2log.py`**: the debug log (controller on its cable, or the receiver):
  wall-clock time in front, survives restarts, `--target pad|receiver`,
  `--boot` / `--reset`. Log commands are typed into the same port (only one
  program can read it at a time).
- **`c2lat.py`**: latency from the PC: a round trip to the controller, and
  how long reports wait (`docs/latency.md`).
- **`c2rate.py`**: report rate and gaps from the PC, any firmware and mode.
- **`c2si_dump.py`**: SInput reports decoded live.
- **`c2xbox_watch.py`**: when each input starts to change, Xbox Bluetooth
  mode.
- **`c2addr.sh`**: names the functions at addresses from the log (STALL
  lines), from the build's ELF.
- **`c2eeprom.py`**: reads the controller's EEPROM (8 KB) through the log,
  read-only.

## Battery

- **`c2batlog.py`**: logs the battery (INFO over the configuration
  protocol) to a CSV, for the voltage curve.
- **`c2batview.py`** + `c2batview.html`: graphs those logs live in the
  browser (`localhost:8765`).

## LEDs

- **`c2led.py`**: sets the LEDs from the command line (Steam mode).
- **`c2led-ui.py`**: the same in a GTK 4 window (`nix develop -c
  tools/c2led-ui.py`).

## Recovery

- **`usbkey-mega2560/`**: an Arduino Mega 2560 sketch that sends the chip's
  ROM USB key, for a controller (or receiver, on its board's pads) whose
  firmware no longer boots.

Flashing and the bootloader: `fw-re/jlflash.sh` (README, "Flashing").
