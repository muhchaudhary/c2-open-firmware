# C2 Battery (Decky plugin)

Shows the battery of controllers on the GameSir C2 receiver in Steam's Big
Picture header, next to the clock, as Steam does for Bluetooth controllers.

## Why

Through the receiver Steam has the pad's battery (SDL reads it from every
report) but doesn't show it. Steam's header (`steamui`, the controller item
next to the clock) draws a controller's battery only when both hold:

- the controller isn't an Xbox type (Steam's `EControllerType` 31, 32, 46),
  and the pad reports itself as an Xbox One controller (SInput type 3);
- it's on Bluetooth (`is_bluetooth`) or Valve's own dongle
  (`is_wireless_steam_dongle`), and to SDL the receiver is a USB device:
  wired.

Tested 2026-11-24: one pad, type 1 (standard), showed the battery in Steam
over Bluetooth only; not on its USB cable, not through the receiver; type 3
showed it nowhere.

## How

Steam's own header item is let through. Steam builds its elements with
React's jsx runtime, `(0, jsx)(Item, props)`, looking `jsx` up on the
module each time; the plugin wraps it. For the header's controller item
(recognised by its own code: `#HeaderActions_Controller`,
`ControllerBatteryImgContainer`, `bWireless`, not minified names, which
differ between Steam's two bundles) and a controller with the receiver's
USB ID `2E8A:10C6`, it renders the item with a view of that controller
that reads as a non-Xbox wireless one. So it is Steam's item: its look,
the bolt while charging, selectable with the controller, and selecting it
opens that controller's settings. Nothing else in Steam sees the view
(the type stays Xbox for prompts and Steam Input); unloading the plugin
puts the jsx runtime back.

The item appears the next time the header redraws after the plugin loads
(open the Quick Access menu, press Home, or move to another page). A
controller that just connected has no USB ID yet when Steam first draws
its item, so the decision is made when drawing, and the items are drawn
again every second: one that connects shows within a second.

The jsx wrapper carries its owner: Decky starts a reloaded plugin (a Steam
restart) before it stops the old copy, and the old copy putting jsx back
used to take the new copy's wrapper with it. Installing unwraps any copy of
ours first, removing touches only this copy's own, and every second it is
put back if something took it off.

**Level and charging.** Steam keeps the level, but resets it to 100 when it
rebuilds its controller list (a reconnect) until the level next changes,
and never sets charging for these pads. Linux has both: the receiver's
battery report makes a HID battery, `/sys/class/power_supply/hid-*-battery-4`,
"Charging" on 5 V. The backend (`main.py`) reads those for the receivers'
USB ID. One receiver: its entry. Several: the one at Steam's level (both
come from the same pad report), else Steam's values. Steam has nothing
else that ties a controller to a receiver: both get the same serial base,
no USB path, no MAC address off Bluetooth; and its controller numbering
follows neither Linux's device order nor the USB port.

So with **two receivers**, after a pad connects or Steam restarts, the items
show 100 % (no bolt) until Steam's levels are real again: the next level
change of each pad, or pressing Home. Then each shows its own pad's level
and charging. Two pads at the same level, one charging: Steam's values.

Only in Big Picture / Game Mode, where Decky Loader runs.

## Install

`./build.sh` makes `out/decky-c2-battery.zip`. In Decky: Settings,
Developer, **Install plugin from ZIP**. No build tools: `dist/index.js` is
the plugin as it runs (plain JavaScript, Decky Loader 3's globals).
