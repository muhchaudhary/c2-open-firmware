// C2 Battery: shows the battery of controllers on the GameSir C2 receiver in
// Steam's Big Picture header, as Steam does for Bluetooth controllers.
//
// Steam's header item for a controller (its battery next to the clock;
// selecting it opens that controller's settings) leaves these out: it draws
// only for controllers on Bluetooth or Valve's dongle (bBluetooth,
// bWireless) that aren't Xbox types, and to SDL the receiver is a USB
// device ("wired") with a pad that says it's an Xbox One controller.
//
// So Steam's own item is let through: Steam builds its elements with
// React's jsx runtime, `(0, jsx)(Item, props)`, looking `jsx` up on the
// module each time. The plugin wraps it; for the header's controller item
// (recognised by its own code, not by minified names) and a controller on
// the receiver it renders the item with a view of that controller that
// reads as a non-Xbox wireless one, with the level and charging from
// Linux (main.py). Nothing else in Steam sees the view; unloading puts the
// jsx runtime back.
//
// Level and charging: Steam keeps the level but resets it to 100 when it
// rebuilds its controller list (reconnects), until the level next changes,
// and never sets charging for these. Linux has both (the receiver's
// battery report, docs/sinput.md 2.6). One receiver: its entry. Several:
// the one at Steam's level (both from the same pad report; Steam's serial
// for the controller isn't the device's), else Steam's values.
//
// Plain ES module for Decky Loader 3 (no build step): React and the Decky UI
// library are the loader's globals (window.SP_REACT, DFL).

const RECEIVER_VID = 0x2e8a;        // the C2 receiver with a pad on it (docs/dongle.md)
const RECEIVER_PID = 0x10c6;
const STEAM_NON_XBOX = 30;          // EControllerType: unknown non-Steam controller
const POLL_MS = 1000;
const React = window.SP_REACT;

// Decky's API connection (as @decky/api does): the backend calls.
const manifest = { name: "C2 Battery" };
const loaderAPI = window.__DECKY_SECRET_INTERNALS_DO_NOT_USE_OR_YOU_WILL_BE_FIRED_deckyLoaderAPIInit;
let api = null;
try {
  api = loaderAPI.connect(2, manifest.name);
} catch (e) {
  try {
    api = loaderAPI.connect(1, manifest.name);
  } catch (e2) {
    console.warn("[C2 Battery] no loader API: Steam's level only, no charging", e2);
  }
}

const isReceiver = (c) => c && c.unVendorID === RECEIVER_VID && c.unProductID === RECEIVER_PID;

// ---- Level and charging from Linux ---------------------------------------
let linux = [];                     // main.py: [{capacity, charging}] per receiver
let battery = new Map();            // controller index -> {level, charging} (from Linux)
const listeners = new Set();

function receiverControllers() {
  const store = window.ControllerStore;
  if (!store || typeof store.GetControllers !== "function") return [];
  return (store.GetControllers() || []).filter(isReceiver);
}

function match() {
  const rx = receiverControllers();
  const next = new Map();
  if (rx.length === 1 && linux.length === 1) {
    next.set(rx[0].nControllerIndex, { level: linux[0].capacity, charging: linux[0].charging });
  } else {
    for (const c of rx) {
      const same = linux.filter((b) => b.capacity === c.ucBatteryLevel);
      if (same.length === 1) next.set(c.nControllerIndex, { level: same[0].capacity, charging: same[0].charging });
    }
  }
  const key = (m) => JSON.stringify([...m.entries()].sort());
  if (key(next) !== key(battery)) {
    battery = next;
    listeners.forEach((f) => f());
  }
}

let polling = false;
async function poll() {
  if (polling) return;
  polling = true;
  try {
    linux = api ? (await api.call("receiver_batteries")) || [] : [];
  } catch (e) {
    linux = [];
  } finally {
    polling = false;
  }
  match();
}

// ---- Steam's header item, let through -------------------------------------
// The view Steam's item gets of a receiver controller: not Xbox, wireless,
// level and charging from Linux when known. Everything else is Steam's.
const views = new WeakMap();
function view(c) {
  let v = views.get(c);
  if (!v) {
    v = new Proxy(c, {
      get(t, k) {
        if (k === "eControllerType") return STEAM_NON_XBOX;
        if (k === "bWireless") return true;
        const b = battery.get(t.nControllerIndex);
        if (b && k === "ucBatteryLevel") return b.level;
        if (b && k === "bCharging") return t.bCharging || b.charging;
        return Reflect.get(t, k, t);
      },
    });
    views.set(c, v);
  }
  return v;
}

// Steam's header controller item, by its own code (both of Steam's bundles
// have a copy; minified names differ).
const checked = new WeakMap();
function isHeaderItem(type) {
  if (typeof type !== "function") return false;
  let v = checked.get(type);
  if (v === undefined) {
    let src = "";
    try {
      src = Function.prototype.toString.call(type);
    } catch (e) {}
    v = src.includes("#HeaderActions_Controller") && src.includes("ControllerBatteryImgContainer") && src.includes("bWireless");
    checked.set(type, v);
  }
  return v;
}

let origJsx = null;

// Every header controller item goes through here; a receiver's gets the
// view. Decided when drawing, not when Steam creates the element: a
// controller that just connected has no USB ID yet when the header first
// draws it, and Steam's item redraws only when its level or charging
// changes, so the battery stayed hidden until the header redrew (Home).
// Drawn again every poll (at most four small items) and when Linux's
// values change.
function C2HeaderItem(props) {
  const { c2Item, details, ...rest } = props;
  const [, bump] = React.useReducer((x) => x + 1, 0);
  React.useEffect(() => {
    listeners.add(bump);
    return () => listeners.delete(bump);
  }, []);
  return origJsx(c2Item, { ...rest, details: isReceiver(details) ? view(details) : details });
}

// The wrapper. Decky starts a reloaded plugin before it stops the old copy
// (a Steam restart): the old copy putting its saved jsx back took the new
// copy's wrapper with it, and the battery was gone until a reinstall. So a
// wrapper carries its owner and Steam's own jsx: installing unwraps any
// copy of ours first, removing touches only this copy's own, and every
// poll puts it back if anything took it off.
const OWNER = Symbol("c2-battery");

function unwrapped(fn) {
  while (fn && fn.__c2) fn = fn.__c2.orig;
  return fn;
}

function wrap(orig) {
  const w = function (type, props, key) {
    if (props && props.details && isHeaderItem(type)) {
      return orig.call(this, C2HeaderItem, { ...props, c2Item: type }, key);
    }
    return orig.call(this, type, props, key);
  };
  w.__c2 = { orig, owner: OWNER };
  return w;
}

function jsxModules() {
  return DFL.findAllModules(
    (m) => m && typeof m === "object" && typeof m.jsx === "function" && typeof m.jsxs === "function" && m.Fragment !== undefined,
  );
}

let modules = [];

function ensurePatched() {
  if (!modules.length) {
    modules = jsxModules();
    if (!modules.length) return;
  }
  for (const m of modules) {
    for (const k of ["jsx", "jsxs"]) {
      if (m[k].__c2 && m[k].__c2.owner === OWNER) continue;
      const orig = unwrapped(m[k]);
      if (!origJsx && k === "jsx") origJsx = orig;
      m[k] = wrap(orig);
    }
  }
}

function unpatchJsx() {
  for (const m of modules) {
    for (const k of ["jsx", "jsxs"]) {
      if (m[k].__c2 && m[k].__c2.owner === OWNER) m[k] = m[k].__c2.orig;
    }
  }
  modules = [];
}

let timer = null;

function tick() {
  ensurePatched();
  poll();
  listeners.forEach((f) => f());   // redraw: a controller's USB ID may have just arrived
}

export default DFL.definePlugin(() => {
  tick();
  timer = setInterval(tick, POLL_MS);
  return {
    name: "C2 Battery",
    title: React.createElement("div", { className: DFL.staticClasses.Title }, "C2 Battery"),
    content: React.createElement(
      DFL.PanelSection,
      null,
      React.createElement(
        DFL.PanelSectionRow,
        null,
        "Shows the battery of controllers on the GameSir C2 receiver in the header.",
      ),
    ),
    icon: React.createElement("span", null, "🔋"),
    onDismount() {
      clearInterval(timer);
      timer = null;
      unpatchJsx();
      listeners.clear();
    },
  };
});
