#!/usr/bin/env bash
# build.sh: package the plugin as a zip for Decky Loader's "Install plugin
# from ZIP" (Settings, Developer). No build step: dist/index.js is the plugin.
#   tools/decky-c2-battery/build.sh   ->  tools/decky-c2-battery/out/decky-c2-battery.zip
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$here/out"
python3 -I - "$here" <<'PY'
import json, os, sys, zipfile
here = sys.argv[1]
ver = json.load(open(os.path.join(here, "package.json")))["version"]
out = os.path.join(here, "out", "decky-c2-battery.zip")
files = ["plugin.json", "package.json", "main.py", "LICENSE", "README.md", "dist/index.js"]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for f in files:
        z.write(os.path.join(here, f), "decky-c2-battery/" + f)   # one top folder: the plugin's
print(f"{out} (version {ver})")
PY
