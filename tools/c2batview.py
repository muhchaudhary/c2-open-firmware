#!/usr/bin/env python3
"""c2batview.py: graph the C2's battery logs in the browser, live.

  tools/c2batview.py [--port 8765]     then open http://127.0.0.1:8765

Sources (picked on the page, newest first):
  firmware/build/batlog-*.csv   tools/c2batlog.py's CSV (pad on: Bluetooth,
                                the receiver or USB)
  firmware/build/c2log*.txt     tools/c2log.py's logs: its "charging: bat .."
                                lines (pad off on the cable or the dock)
The page re-reads the chosen source every 10 s, so a log still being
written shows up as it grows. Local only (binds 127.0.0.1); reads only
those files.
"""
import argparse
import datetime
import glob
import http.server
import json
import os
import re
import urllib.parse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, 'firmware', 'build')
PAGE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'c2batview.html')
# c2log.py: "HH:MM:SS  uptime charging: bat 4075 mV 97% plug 2 pb02 0 5V docked"
CHG = re.compile(r'^(\d\d):(\d\d):(\d\d)\s+\S+\s+charging: bat (\d+) mV (\d+)% plug (\d+) pb02 (-?\d+)(.*)$')


def sources():
    out = []
    for p in glob.glob(os.path.join(BUILD, 'batlog-*.csv')):
        out.append(p)
    out += glob.glob(os.path.join(BUILD, 'c2log*.txt'))
    out.sort(key=os.path.getmtime, reverse=True)
    return [dict(name=os.path.basename(p), mtime=os.path.getmtime(p), size=os.path.getsize(p)) for p in out]


def read_csv(path):
    rows = []
    with open(path) as f:
        head = f.readline().strip().split(',')
        for line in f:
            v = line.strip().split(',')
            if len(v) != len(head):
                continue
            r = dict(zip(head, v))
            try:
                t = datetime.datetime.strptime(r['time'], '%Y-%m-%d %H:%M:%S').timestamp()
                rows.append(dict(t=t, mv=int(r['mv']), pct=int(r['percent']), plug=int(r['plug']),
                                 pb02=int(r['pb02']) if r.get('pb02', '') != '' else None,
                                 usb=int(r.get('usb5v') or 0), dock=int(r.get('docked') or 0)))
            except (KeyError, ValueError):
                continue
    return rows


def read_c2log(path):
    """Only HH:MM:SS in the log: dated from the file's end (its mtime) backwards,
    a time going backwards between lines read as the day before."""
    hits = []
    with open(path, errors='replace') as f:
        for line in f:
            m = CHG.match(line.strip())
            if m:
                hits.append(m)
    rows = []
    day = datetime.datetime.fromtimestamp(os.path.getmtime(path)).date()
    later = None
    for m in reversed(hits):
        h, mi, s = int(m[1]), int(m[2]), int(m[3])
        t = datetime.datetime.combine(day, datetime.time(h, mi, s))
        if later is not None and t > later:
            day -= datetime.timedelta(days=1)
            t -= datetime.timedelta(days=1)
        later = t
        rest = m[8]
        rows.append(dict(t=t.timestamp(), mv=int(m[4]), pct=int(m[5]), plug=int(m[6]),
                         pb02=int(m[7]) if int(m[7]) >= 0 else None,
                         usb=1 if ' 5V' in rest else 0, dock=1 if 'docked' in rest else 0))
    rows.reverse()
    return rows


class Handler(http.server.BaseHTTPRequestHandler):
    def send(self, code, body, ctype):
        b = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(b)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        if u.path == '/':
            with open(PAGE, 'rb') as f:
                return self.send(200, f.read(), 'text/html; charset=utf-8')
        if u.path == '/api/sources':
            return self.send(200, json.dumps(sources()), 'application/json')
        if u.path == '/api/data':
            name = urllib.parse.parse_qs(u.query).get('src', [''])[0]
            if name not in {s['name'] for s in sources()}:      # only the listed files
                return self.send(404, '{"error":"no such source"}', 'application/json')
            p = os.path.join(BUILD, name)
            rows = read_c2log(p) if name.endswith('.txt') else read_csv(p)
            return self.send(200, json.dumps(rows), 'application/json')
        self.send(404, 'not found', 'text/plain')

    def log_message(self, *a):
        pass


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, default=8765)
    a = ap.parse_args()
    srv = http.server.ThreadingHTTPServer(('127.0.0.1', a.port), Handler)
    print(f'http://127.0.0.1:{a.port}  (Ctrl-C to stop)')
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
