#!/usr/bin/env python3
"""c2led-ui.py: GTK 4 window for the C2's LEDs (Steam mode).

  nix develop -c tools/c2led-ui.py

Draws the controller; click the Home button, a grip or the player lights to
pick what to change, then drag the sliders (sent live) or pick a swatch.
Each change goes to the pad through c2led.py's hidraw writer; the pad is
looked up again on every send, so a reconnect needs no restart.

The firmware can't report its colours, so the window starts from what this PC
last sent (c2led.py's state file) and saves each change there.
"""
import colorsys
import math
import sys

import gi
gi.require_version('Gtk', '4.0')
gi.require_version('Gdk', '4.0')
from gi.repository import Gdk, GLib, Gtk  # noqa: E402

import c2led  # noqa: E402

import cairo  # noqa: E402  (pycairo, pulled in by PyGObject)

# The drawing follows the C2 as GameSir Connect shows it (884 x 585 design
# units). Shapes are given for the left half and mirrored about x = CX.
W, H = 884, 585
CX = 441.5
PAD = 24
SLOTS = ('home', 'left', 'right')
PRESETS = ('#ff0000', '#ff4000', '#ffff00', '#00ff00', '#00ffff',
           '#0000ff', '#8000ff', '#ff00ff', '#ffffff', '#000000')
ACCENT = (0.49, 0.49, 1.0)

BODY_RGB = (0.176, 0.176, 0.176)
DARK = (0.047, 0.047, 0.047)
GREY = (0.235, 0.247, 0.259)
INK = (0.03, 0.03, 0.03)

# left half of the body outline, bottom centre -> grip -> outer edge -> top centre
BODY_L = [(CX, 432), (289, 432), (271, 435), (258, 438), (244, 445), (230, 457), (202, 484),
          (154, 534), (129, 560), (112, 575), (100, 581), (88, 583), (70, 579), (54, 571),
          (41, 565), (27, 556), (18, 548), (11, 539), (5, 527), (1, 510), (0, 488), (2, 444),
          (7, 409), (13, 380), (26, 330), (42, 278), (61, 222), (87, 151), (113, 85), (126, 72),
          (140, 63), (160, 54), (200, 40), (230, 33), (260, 29), (300, 26), (346, 24), (CX, 23)]
BUMPER_L = [(113, 85), (124, 72), (132, 67), (138, 50), (154, 34), (162, 17), (177, 10),
            (196, 5), (222, 4), (253, 1), (294, 1), (317, 24), (260, 29), (200, 40), (140, 63)]
# centre line of the left grip's light bar
BAR_L = [(93, 238), (82, 262), (75, 290), (75, 318), (84, 338), (101, 354), (128, 370),
         (162, 389), (196, 409)]
HOME = (CX, 90)
PLAYER_Y = (228, 250, 272, 294)
LS, RS, DPAD = (218, 163), (551, 307), (326, 312)
FACE = (((661, 117), 'Y'), ((603, 173), 'X'), ((720, 173), 'B'), ((661, 231), 'A'))
# what GameSir Connect shows for the four channel lights
PLAYER_RGB = ((0.5, 1, 0), (1, 1, 0), (1, 0.5, 0), (1, 0, 0))

CSS = '''
scale.grad trough { min-height: 12px; border-radius: 6px; border: none; }
scale.grad highlight { background: none; border: none; box-shadow: none; }
scale.grad slider { background: #fff; border: 2px solid rgba(0,0,0,.4); }
scale.hue trough { background-image: linear-gradient(to right,
  #f00, #ff0, #0f0, #0ff, #00f, #f0f, #f00); }
.status-err { color: @error_color; }
''' + ''.join(f'button.sw-{c[1:]} {{ background: {c}; min-width: 28px; min-height: 28px; '
              f'border-radius: 999px; padding: 0; border: 1px solid alpha(currentColor, .3); '
              f'box-shadow: none; }}\n'
              f'button.sw-{c[1:]}:hover {{ box-shadow: 0 0 0 2px alpha(currentColor, .5); }}\n'
              for c in PRESETS)


def mx(p):
    return (2 * CX - p[0], p[1])


def side(pts, s):
    return pts if s == 'left' else [mx(p) for p in pts]


def catmull(pts, n=10, closed=False):
    """Points along a Catmull-Rom spline through pts."""
    P = pts + pts[:3] if closed else [pts[0]] + pts + [pts[-1]]
    out = []
    for i in range(len(P) - 3):
        p0, p1, p2, p3 = P[i:i + 4]
        for k in range(n):
            t = k / n
            t2, t3 = t * t, t * t * t
            out.append(tuple(0.5 * (2 * b + (-a + c) * t + (2 * a - 5 * b + 4 * c - d) * t2
                                    + (-a + 3 * b - 3 * c + d) * t3)
                             for a, b, c, d in zip(p0, p1, p2, p3)))
    if not closed:
        out.append(pts[-1])
    return out


BODY = catmull(BODY_L + [mx(p) for p in reversed(BODY_L[1:-1])], closed=True)
BAR = {s: catmull(side(BAR_L, s), 8) for s in ('left', 'right')}


def poly(cr, pts, close=True):
    cr.new_path()
    cr.move_to(*pts[0])
    for p in pts[1:]:
        cr.line_to(*p)
    if close:
        cr.close_path()


def outline(pts, r):
    """Closed outline of a thick line along pts (half-width r, round ends)."""
    left, right = [], []
    for i, (x, y) in enumerate(pts):
        a, b = pts[max(i - 1, 0)], pts[min(i + 1, len(pts) - 1)]
        dx, dy = b[0] - a[0], b[1] - a[1]
        L = math.hypot(dx, dy) or 1
        nx, ny = -dy / L * r, dx / L * r
        left.append((x + nx, y + ny))
        right.append((x - nx, y - ny))

    def cap(c, a, b, start):       # half circle round c, through the direction a -> b
        t = math.atan2(b[1] - a[1], b[0] - a[0]) + (math.pi if start else 0)
        return [(c[0] + r * math.cos(t + math.pi / 2 - math.pi * k / 8),
                 c[1] + r * math.sin(t + math.pi / 2 - math.pi * k / 8)) for k in range(1, 8)]
    return (left + cap(pts[-1], pts[-2], pts[-1], False) + right[::-1]
            + cap(pts[0], pts[0], pts[1], True))


def dist_to(pts, x, y):
    best = 1e9
    for (x1, y1), (x2, y2) in zip(pts, pts[1:]):
        dx, dy = x2 - x1, y2 - y1
        t = max(0, min(1, ((x - x1) * dx + (y - y1) * dy) / ((dx * dx + dy * dy) or 1)))
        best = min(best, math.hypot(x - x1 - t * dx, y - y1 - t * dy))
    return best


def hexrgb(s):
    return tuple(b / 255 for b in bytes.fromhex(s.lstrip('#')))


def lum(c):
    return max(c)


def lerp(a, b, t):
    return tuple(x + (y - x) * t for x, y in zip(a, b))


def _lighten(c, k=0.35):
    return lerp(c, (1, 1, 1), k)


def _circle(cr, x, y, r, rgb=None):
    cr.new_path()
    cr.arc(x, y, r, 0, 2 * math.pi)
    if rgb:
        cr.set_source_rgb(*rgb)
        cr.fill()


def _rrect(cr, x, y, w, h, r):
    cr.new_path()
    cr.arc(x + w - r, y + r, r, -math.pi / 2, 0)
    cr.arc(x + w - r, y + h - r, r, 0, math.pi / 2)
    cr.arc(x + r, y + h - r, r, math.pi / 2, math.pi)
    cr.arc(x + r, y + r, r, math.pi, 1.5 * math.pi)
    cr.close_path()


def _pill(cr, x, y, w, h, angle=0):
    cr.save()
    cr.translate(x, y)
    cr.rotate(angle)
    _rrect(cr, -w / 2, -h / 2, w, h, h / 2)
    cr.restore()


def _radial(x, y, r0, r1, stops):
    g = cairo.RadialGradient(x, y, r0, x, y, r1)
    for off, rgba in stops:
        g.add_color_stop_rgba(off, *rgba)
    return g


def _text(cr, x, y, s, size, rgb, bold=False, family='Sans'):
    cr.select_font_face(family, 0, 1 if bold else 0)
    cr.set_font_size(size)
    e = cr.text_extents(s)
    cr.move_to(x - e.width / 2 - e.x_bearing, y - e.height / 2 - e.y_bearing)
    cr.set_source_rgb(*rgb)
    cr.show_text(s)


class Controller(Gtk.DrawingArea):
    """The C2, drawn; reports clicks on its LEDs."""

    def __init__(self, win):
        super().__init__(content_width=W // 2, content_height=H // 2, hexpand=True, vexpand=True)
        self.win = win
        self.xf = (0, 0, 1)
        self.hover = None
        self.set_draw_func(self.draw)
        click = Gtk.GestureClick()
        click.connect('pressed', lambda _g, _n, x, y: self.win.on_hit(self.hit(x, y)))
        self.add_controller(click)
        motion = Gtk.EventControllerMotion()
        motion.connect('motion', self.on_motion)
        motion.connect('leave', lambda _c: self.on_motion(None, -1, -1))
        self.add_controller(motion)

    def hit(self, x, y):
        ox, oy, s = self.xf
        x, y = (x - ox) / s, (y - oy) / s
        if math.hypot(x - HOME[0], y - HOME[1]) <= 40:
            return 'home'
        if abs(x - CX) <= 18 and PLAYER_Y[0] - 11 <= y <= PLAYER_Y[-1] + 11:
            return ('player', min(range(4), key=lambda i: abs(y - PLAYER_Y[i])) + 1)
        for s in ('left', 'right'):
            if dist_to(BAR[s], x, y) <= 30:
                return s
        return None

    def on_motion(self, _c, x, y):
        h = self.hit(x, y) if x >= 0 else None
        if h != self.hover:
            self.hover = h
            self.set_cursor_from_name('pointer' if h else None)
            self.queue_draw()

    # drawing

    def draw(self, _area, cr, w, h):
        s = min(w / (W + 2 * PAD), h / (H + 2 * PAD))
        ox, oy = (w - W * s) / 2, (h - H * s) / 2
        self.xf = (ox, oy, s)
        cr.translate(ox, oy)
        cr.scale(s, s)
        cr.set_line_join(cairo.LINE_JOIN_ROUND)
        col = self.win.col

        for sd in ('left', 'right'):                     # bumpers
            poly(cr, side(BUMPER_L, sd))
            g = cairo.LinearGradient(0, 0, 0, 80)
            g.add_color_stop_rgb(0, 0.30, 0.31, 0.33)
            g.add_color_stop_rgb(1, *GREY)
            cr.set_source(g)
            cr.fill_preserve()
            cr.set_source_rgb(*INK)
            cr.set_line_width(1.2)
            cr.stroke()

        poly(cr, BODY)                                   # body
        g = cairo.LinearGradient(0, 20, 0, 585)
        g.add_color_stop_rgb(0, 0.20, 0.20, 0.205)
        g.add_color_stop_rgb(0.6, *BODY_RGB)
        g.add_color_stop_rgb(1, 0.15, 0.15, 0.155)
        cr.set_source(g)
        cr.fill_preserve()
        cr.set_source_rgb(*INK)
        cr.set_line_width(1.5)
        cr.stroke()

        cr.save()
        poly(cr, BODY)
        cr.clip()
        self.draw_seams(cr)
        for sd in ('left', 'right'):
            self.draw_bar(cr, sd, col[sd])
        cr.restore()

        self.draw_sticks(cr)
        self.draw_dpad(cr)
        for (x, y), letter in FACE:
            _circle(cr, x, y, 30, (0.06, 0.06, 0.065))
            cr.arc(x, y, 30, 0, 2 * math.pi)
            cr.set_source_rgb(0.02, 0.02, 0.02)
            cr.set_line_width(1.5)
            cr.stroke()
            _text(cr, x, y, letter, 34, (0.6, 0.61, 0.63), family='DejaVu Sans Condensed')
        self.draw_centre(cr)
        self.draw_home(cr, col['home'])
        self.draw_selection(cr)

    def draw_seams(self, cr):
        cr.set_source_rgb(*INK)
        cr.set_line_width(1.3)
        for sd in ('left', 'right'):
            for line in ([(346, 24), (404, 74)],                  # around Home
                         [(286, 48), (372, 152), (420, 152)],     # View diagonal + bar
                         [(374, 160), (331, 238)],                # down to the d-pad
                         [(118, 190), (88, 290)]):                # grip edge
                poly(cr, side(line, sd), close=False)
                cr.stroke()
            poly(cr, side([(372, 150), (396, 150), (393, 157), (376, 157)], sd))
            cr.fill()
        # band across the bottom and the GAMESIR badge
        cr.move_to(200, 414)
        cr.curve_to(250, 408, 300, 406, 360, 406)
        cr.line_to(523, 406)
        cr.curve_to(583, 406, 633, 408, 683, 414)
        cr.set_source_rgba(0, 0, 0, 0.6)
        cr.stroke()
        cr.set_source_rgb(*INK)
        poly(cr, [(286, 426), (366, 426), (378, 410), (505, 410), (517, 426), (597, 426)], close=False)
        cr.stroke()
        _text(cr, CX, 420, 'GAMESIR', 20, INK, bold=True)

    def draw_bar(self, cr, sd, c):
        lit = lum(c) > 0.02
        pts = BAR[sd]
        n = len(pts) - 1
        passes = ((30, 0.22), (20, 0.5), (11, 1.0)) if lit else ((11, 0),)
        cr.set_line_cap(cairo.LINE_CAP_ROUND)
        for width, k in passes:
            cr.set_line_width(width)
            for i in range(n):
                t = i / n
                fade = min(1, t / 0.2, (1 - t) / 0.2)
                fade = fade * fade * (3 - 2 * fade)
                core = _lighten(c, 0.25) if width < 12 else c
                if lit:
                    rgb = lerp(BODY_RGB, core, k * fade)
                else:
                    rgb = lerp(BODY_RGB, (0.11, 0.11, 0.11), fade)
                cr.move_to(*pts[i])
                cr.line_to(*pts[i + 1])
                cr.set_source_rgb(*rgb)
                cr.stroke()
        cr.set_line_cap(cairo.LINE_CAP_BUTT)

    def draw_sticks(self, cr):
        for (x, y), arc in ((LS, (-95, 2)), (RS, (-50, 50))):
            _circle(cr, x, y, 68)
            cr.set_source_rgba(0, 0, 0, 0.35)
            cr.set_line_width(1.2)
            cr.stroke()
            _circle(cr, x, y, 58, (0.075, 0.075, 0.08))
            cr.arc(x, y, 50, 0, 2 * math.pi)
            cr.set_source(_radial(x - 12, y - 14, 4, 56, [(0, (0.10, 0.10, 0.11, 1)), (1, (0.04, 0.04, 0.045, 1))]))
            cr.fill()
            for r in (33, 37):
                _circle(cr, x, y, r)
                cr.set_source_rgba(1, 1, 1, 0.07)
                cr.set_line_width(1.2)
                cr.stroke()
            self.ticks(cr, x, y, 82, *arc)

    def ticks(self, cr, x, y, r, a0, a1):
        cr.set_source_rgb(*INK)
        cr.set_line_width(1.5)
        cr.new_path()
        cr.arc(x, y, r, math.radians(a0), math.radians(a1))
        cr.stroke()
        for a in range(a0, a1 + 1, (a1 - a0) // 4):
            ca, sa = math.cos(math.radians(a)), math.sin(math.radians(a))
            cr.move_to(x + r * ca, y + r * sa)
            cr.line_to(x + (r - 7) * ca, y + (r - 7) * sa)
            cr.stroke()

    def draw_dpad(self, cr):
        x, y = DPAD
        _circle(cr, x, y, 69, (0.13, 0.13, 0.135))
        cr.save()                                        # hatching around the cross
        _circle(cr, x, y, 66)
        cr.clip()
        cr.set_source_rgba(0, 0, 0, 0.75)
        cr.set_line_width(1.6)
        for k in range(-140, 141, 7):
            cr.move_to(x + k - 80, y - 80)
            cr.line_to(x + k + 80, y + 80)
        cr.stroke()
        cr.restore()
        cr.set_source_rgb(*INK)
        _rrect(cr, x - 28, y - 69, 56, 138, 10)
        cr.fill()
        _rrect(cr, x - 69, y - 28, 138, 56, 10)
        cr.fill()
        cr.set_source_rgb(*GREY)
        _rrect(cr, x - 25, y - 66, 50, 132, 8)
        cr.fill()
        _rrect(cr, x - 66, y - 25, 132, 50, 8)
        cr.fill()
        self.ticks(cr, x, y, 82, 100, 176)

    def draw_centre(self, cr):
        for sd, ang in (('left', 0.95), ('right', -0.95)):     # View / Menu
            px, py = side([(321, 100)], sd)[0]
            _pill(cr, px, py, 46, 26, ang)
            cr.set_source_rgb(*DARK)
            cr.fill_preserve()
            cr.set_source_rgb(*INK)
            cr.set_line_width(1.5)
            cr.stroke()
            gx, gy = side([(331, 132)], sd)[0]
            _text(cr, gx, gy, '−' if sd == 'left' else '+', 22, (0.3, 0.3, 0.31))
        _pill(cr, CX, 160, 48, 24)                             # Share
        cr.set_source_rgb(*DARK)
        cr.fill_preserve()
        cr.set_source_rgb(*INK)
        cr.stroke()
        cr.set_source_rgb(0.27, 0.28, 0.29)                    # capture glyph
        _rrect(cr, CX - 8, 181, 16, 15, 2)
        cr.set_line_width(1.6)
        cr.stroke()
        _circle(cr, CX, 188.5, 3.5, (0.27, 0.28, 0.29))
        n = self.win.player
        for i, y in enumerate(PLAYER_Y):                       # channel lights
            on = n is not None and i < n
            if on:
                c = PLAYER_RGB[i]
                cr.set_source(_radial(CX, y, 2, 16, [(0, (*c, 0.55)), (1, (*c, 0))]))
                _circle(cr, CX, y, 16)
                cr.fill()
            cr.set_source_rgb(*(PLAYER_RGB[i] if on else (0.1, 0.1, 0.1)))
            _rrect(cr, CX - 8, y - 2.5, 16, 5, 2.5)
            cr.fill()
        _pill(cr, CX, 373, 48, 26)                             # M
        cr.set_source_rgb(*DARK)
        cr.fill_preserve()
        cr.set_source_rgb(*INK)
        cr.stroke()
        _text(cr, CX, 373, 'M', 16, (0.4, 0.41, 0.42))

    def draw_home(self, cr, c):
        x, y = HOME
        lit = lum(c) > 0.02
        if lit:
            cr.set_source(_radial(x, y, 26, 58, [(0, (*c, 0.55)), (1, (*c, 0))]))
            _circle(cr, x, y, 58)
            cr.fill()
        _circle(cr, x, y, 35, (0.02, 0.02, 0.02))
        _circle(cr, x, y, 29)
        cr.set_source_rgb(*(_lighten(c, 0.15) if lit else (0.12, 0.12, 0.12)))
        cr.set_line_width(4.5)
        cr.stroke()
        _circle(cr, x, y, 25, (0.06, 0.06, 0.065))
        cr.new_path()                                          # "G" mark
        cr.arc(x, y, 11, math.radians(25), math.radians(330))
        cr.line_to(x + 11, y)
        cr.line_to(x + 2, y)
        cr.set_source_rgb(0.5, 0.51, 0.53)
        cr.set_line_width(3.5)
        cr.stroke()

    def draw_selection(self, cr):
        sel = self.win.sel
        cr.set_line_width(2.5)
        for slot in SLOTS:
            on = slot == sel or sel == 'all'
            if not (on or self.hover == slot):
                continue
            cr.set_dash([7, 5] if on else [])
            cr.set_source_rgba(*ACCENT, 0.95 if on else 0.5)
            if slot == 'home':
                _circle(cr, HOME[0], HOME[1], 44)
            else:
                poly(cr, outline(BAR[slot], 24))
            cr.stroke()
        cr.set_dash([])
        if isinstance(self.hover, tuple):
            cr.set_source_rgba(*ACCENT, 0.6)
            _rrect(cr, CX - 18, PLAYER_Y[0] - 13, 36, PLAYER_Y[-1] - PLAYER_Y[0] + 26, 10)
            cr.stroke()


class Window(Gtk.ApplicationWindow):
    def __init__(self, app):
        super().__init__(application=app, title='C2 LEDs')
        self.set_default_size(760, 800)
        st = c2led.load_state()
        known = any(st[k] for k in SLOTS)
        self.col = {k: hexrgb(st[k] or 'ff0080') for k in SLOTS}
        self.player = st['player']
        self.save_pending = {}
        self.save_timer = None
        self.sel = 'all'
        self.updating = False
        self.pending = None

        self.dyn_css = Gtk.CssProvider()
        Gtk.StyleContext.add_provider_for_display(
            Gdk.Display.get_default(), self.dyn_css, Gtk.STYLE_PROVIDER_PRIORITY_USER + 2)

        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=14,
                      margin_top=12, margin_bottom=18, margin_start=18, margin_end=18)
        self.set_child(box)

        self.pad = Controller(self)
        box.append(self.pad)
        hint = Gtk.Label(label='Click the Home ring, a grip light or the channel lights')
        hint.add_css_class('dim-label')
        box.append(hint)

        seg = Gtk.Box(halign=Gtk.Align.CENTER)
        seg.add_css_class('linked')
        self.seg = {}
        group = None
        for key, label in (('all', 'All'), ('home', 'Home'), ('left', 'Left grip'), ('right', 'Right grip')):
            t = Gtk.ToggleButton(label=label, active=key == 'all')
            if group:
                t.set_group(group)
            group = group or t
            t.connect('toggled', lambda t, k: t.get_active() and self.select(k), key)
            seg.append(t)
            self.seg[key] = t
        box.append(seg)

        grid = Gtk.Grid(column_spacing=12, row_spacing=6)
        self.scales = {}
        for row, (key, label, top) in enumerate((('hue', 'Hue', 360), ('sat', 'Saturation', 100),
                                                 ('val', 'Brightness', 100))):
            grid.attach(Gtk.Label(label=label, xalign=0), 0, row, 1, 1)
            sc = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0, top, 1)
            sc.set_draw_value(False)
            sc.set_hexpand(True)
            sc.add_css_class('grad')
            sc.add_css_class(key)
            sc.connect('value-changed', self.on_slider)
            grid.attach(sc, 1, row, 1, 1)
            self.scales[key] = sc
        box.append(grid)

        presets = Gtk.Box(spacing=8, halign=Gtk.Align.CENTER)
        for c in PRESETS:
            b = Gtk.Button(tooltip_text='Off' if c == '#000000' else c, valign=Gtk.Align.CENTER)
            b.add_css_class(f'sw-{c[1:]}')
            b.connect('clicked', lambda _b, c: self.apply(hexrgb(c), sync=True), c)
            presets.append(b)
        custom = Gtk.Button(label='Custom…')
        custom.connect('clicked', self.on_custom)
        presets.append(custom)
        box.append(presets)

        self.status = Gtk.Label(label='Showing the colours last sent from this PC (Steam or a game may '
                                      'have changed them since)' if known else
                                'No colours sent from this PC yet: the pad keeps its own until you pick one',
                                xalign=0, wrap=True)
        self.status.add_css_class('dim-label')
        box.append(self.status)

        self.sync_sliders()

    # selection / colour state

    def targets(self):
        return SLOTS if self.sel == 'all' else (self.sel,)

    def select(self, key):
        self.sel = key
        if not self.seg[key].get_active():
            self.seg[key].set_active(True)
        self.sync_sliders()
        self.pad.queue_draw()

    def on_hit(self, hit):
        if isinstance(hit, tuple):
            self.player = hit[1]
            self.pad.queue_draw()
            self.send(bytes([0x03, hit[1]]), player=hit[1])
        elif hit:
            self.select('all' if hit == self.sel else hit)

    def sync_sliders(self):
        h, s, v = colorsys.rgb_to_hsv(*self.col[self.targets()[0]])
        self.updating = True
        if s > 0 and v > 0:
            self.scales['hue'].set_value(h * 360)
        if v > 0:
            self.scales['sat'].set_value(s * 100)
        self.scales['val'].set_value(v * 100)
        self.updating = False
        self.update_gradients()

    def update_gradients(self):
        h = self.scales['hue'].get_value() / 360
        s = self.scales['sat'].get_value() / 100

        def css(r, g, b):
            return f'rgb({r * 255:.0f},{g * 255:.0f},{b * 255:.0f})'
        self.dyn_css.load_from_string(
            f'scale.sat trough {{ background-image: linear-gradient(to right, '
            f'{css(*colorsys.hsv_to_rgb(h, 0, 1))}, {css(*colorsys.hsv_to_rgb(h, 1, 1))}); }}\n'
            f'scale.val trough {{ background-image: linear-gradient(to right, '
            f'#000, {css(*colorsys.hsv_to_rgb(h, s, 1))}); }}\n')

    def on_slider(self, _sc):
        self.update_gradients()
        if self.updating:
            return
        v = [self.scales[k].get_value() for k in ('hue', 'sat', 'val')]
        self.apply(colorsys.hsv_to_rgb(v[0] / 360, v[1] / 100, v[2] / 100))

    def apply(self, rgb, sync=False):
        for k in self.targets():
            self.col[k] = tuple(rgb)
        if sync:
            self.sync_sliders()
        self.pad.queue_draw()
        self.queue_colours()

    def on_custom(self, _b):
        dlg = Gtk.ColorDialog(with_alpha=False, title='LED colour')
        init = Gdk.RGBA()
        init.red, init.green, init.blue = self.col[self.targets()[0]]
        init.alpha = 1

        def done(d, res):
            try:
                c = d.choose_rgba_finish(res)
            except GLib.Error:
                return
            self.apply((c.red, c.green, c.blue), sync=True)
        dlg.choose_rgba(self, init, None, done)

    # sending (slider drags are coalesced to one report per 25 ms)

    def queue_colours(self):
        if self.pending is None:
            self.pending = GLib.timeout_add(25, self.flush)

    def flush(self):
        self.pending = None
        c = {k: bytes(round(min(1, max(0, v)) * 255) for v in self.col[k]) for k in SLOTS}
        self.send(c2led.per_led(c['home'], c['left'], c['right']), **c)
        return False

    def send(self, payload, **state):
        dev = c2led.find_hidraw()
        try:
            if not dev:
                raise OSError('C2 not found: is it on, connected, in Steam mode (Home + Y held 3 s)?')
            c2led.send(dev, payload)
            self.show_status(f'Sent to {dev}', False)
            self.save_later(state)
        except PermissionError:
            self.show_status(f'No write access to {dev}', True)
        except OSError as e:
            self.show_status(str(e), True)

    # the state file is written at most every 300 ms while dragging, and on close

    def save_later(self, state):
        self.save_pending.update(state)
        if self.save_timer is None:
            self.save_timer = GLib.timeout_add(300, self.save_now)

    def save_now(self, cancel=False):
        if cancel and self.save_timer is not None:
            GLib.source_remove(self.save_timer)
        self.save_timer = None
        if self.save_pending:
            c2led.save_state(**self.save_pending)
            self.save_pending = {}
        return False

    def do_close_request(self):
        if self.pending is not None:
            GLib.source_remove(self.pending)
            self.flush()
        self.save_now(cancel=True)
        return False

    def show_status(self, text, err):
        self.status.set_label(text)
        (self.status.add_css_class if err else self.status.remove_css_class)('status-err')


class App(Gtk.Application):
    def __init__(self):
        super().__init__(application_id='dev.gamesir.C2Leds')

    def do_activate(self):
        css = Gtk.CssProvider()
        css.load_from_string(CSS)
        Gtk.StyleContext.add_provider_for_display(
            Gdk.Display.get_default(), css, Gtk.STYLE_PROVIDER_PRIORITY_USER + 1)
        (self.get_active_window() or Window(self)).present()


if __name__ == '__main__':
    sys.exit(App().run(sys.argv))
