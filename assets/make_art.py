#!/usr/bin/env python3
"""Draw the project's artwork.

There is no clip art here and no external asset. Everything is emitted as
ImageMagick MVG and rendered supersampled, then scaled down, which is what gives
clean edges where a stroke is one pixel wide.

    py -3 assets/make_art.py social  -> assets/social.png        1280x640 card
    py -3 assets/make_art.py icon    -> assets/wrlines.ico       16..256
    py -3 assets/make_art.py swoop   -> assets/wrlines_swoop.ico the first icon
    py -3 assets/make_art.py all       social + icon

THE CARD is one run seen from the floor of a surf ramp: a 3-D curve lying on a
3-D ramp plane, both through the same pinhole projection. The line thins with
distance because it is further away, not because a gradient was painted along
it. Its green and red are the literal constants out of EfficiencyColour in
wr_render.cpp, so the picture cannot advertise colours the program does not
draw.

THE ICON is not that picture scaled down, and cannot be: what makes the card
work is the run arriving out of the distance, and most of that stroke is one
pixel wide, which at 64 px is one pixel and at 16 px is nothing. It is a
triangular prism -- the shape of a ramp -- with a run boarding it from above,
crossing the face diagonally, and leaving over the lower SIDE edge into the
air. Not off the end, and never down to the floor: that is not what the
movement does, and an icon that showed it would be teaching the wrong thing.
Drawn flat and deliberately overstated. wrlines_swoop.ico is the earlier
design, kept buildable so it is not a binary nobody can regenerate.

Needs ImageMagick 7 ("magick" on PATH). Nothing else -- no PIL, no numpy.
"""

import math
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TMP = os.path.join(HERE, "_build")

# --- the palette, taken from EfficiencyColour in wr_render.cpp ---------------
# These are the literal constants in the shipped function. If they ever change
# there they must change here, because the whole point of using them is that the
# artwork shows what the tool actually draws.
GAIN = (0.10, 1.00, 0.10)          # eta > 0: the strafe is adding energy
LOSS = (1.00, 0.13, 0.10)          # eta < 0: it is bleeding it
NEUTRAL = (0.52, 0.50, 0.44)       # the run colour pulled most of the way to grey

BG = "#06080C"
RAMP_NEAR = (0.26, 0.30, 0.38)
RAMP_FAR = (0.05, 0.06, 0.09)

# --- the scene ---------------------------------------------------------------
# Camera at the origin looking along +Y. +X right, +Z up. A point at depth d
# lands at (cx + F*x/d, cy - F*z/d), so the line thinning with distance falls
# out of the projection rather than being faked.
#
# The ramp is the plane z = RAMP_A*x + RAMP_B. Its depth lines converge on
# (cx, cy); its cross lines have no depth component at all, so they stay
# parallel to each other. Both of those are properties of the plane, which is
# why it is drawn in 3-D instead of as hand-placed converging strokes.
RAMP_A = 0.50
RAMP_B = -2.60
RIDE_H = 0.35                      # how far above the surface the line rides


def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def hexof(c, scale=1.0):
    return "#%02X%02X%02X" % tuple(
        int(clamp(v * scale, 0.0, 1.0) * 255 + 0.5) for v in c)


def eff_colour(eta):
    """EfficiencyColour, with the neutral band and saturation of the defaults."""
    band, sat = 0.05, 1.00
    a = abs(eta)
    if a <= band:
        return NEUTRAL
    u = clamp((a - band) / (sat - band), 0.0, 1.0)
    return mix(NEUTRAL, GAIN if eta > 0 else LOSS, u)


def ramp_z(x):
    return RAMP_A * x + RAMP_B


class Scene:
    def __init__(self, w, h, f, cx, cy):
        self.w, self.h, self.f, self.cx, self.cy = w, h, f, cx, cy

    def project(self, p):
        x, d, z = p
        d = max(d, 0.30)
        return (self.cx + self.f * x / d, self.cy - self.f * z / d)

    def visible(self, a, b, slack):
        for pt in (a, b):
            if -slack < pt[0] < self.w + slack and -slack < pt[1] < self.h + slack:
                return True
        return False


def curve(s, carve=2.6, drift=0.6, lift=3.0, far=70.0, near=1.30):
    """One run down the ramp: in from the distance, a carve across the face,
    then past the camera and away.

    x falls faster than linearly and d falls as a high power of (1-s), because
    almost all of a run's apparent movement happens in the last fraction of it
    -- which is exactly the part worth drawing large. The line is pinned to the
    ramp surface rather than floating: on a plane that rises with x, sliding
    left IS descending, and that is what makes the dive read as surfing instead
    of as a swoosh.

    Two constants exist purely so the interesting part happens on screen. The
    carve is scaled by s, so the far end runs straight out of the vanishing
    point instead of curling into a fishhook. And `lift` takes the line up off
    the surface over the second half -- which is what leaving a ramp actually
    looks like, and which also keeps it in frame until it is fifteen times
    thicker than where it entered. Without it the run exits the left edge while
    still thin, and the whole point of the picture is the part that does not."""
    d = near + (far - near) * (1.0 - s) ** 2.6
    x = (11.0 * (1.0 - s) ** 1.3 - drift * s
         + carve * (0.30 + 0.70 * s) * math.sin(2.0 * math.pi * (0.85 * s + 0.10)))
    z = ramp_z(x) + RIDE_H + lift * clamp((s - 0.50) / 0.50, 0.0, 1.0) ** 2
    return (x, d, z)


def eta_at(u, centre=0.50, sigma=0.30, wob_s=0.0):
    """Red where it is bleeding energy, green through the carve, red again on
    the way out. u is a position along the DRAWN LINE, not along s.

    That distinction is the whole of it. Spreading the colour over the curve
    parameter put the green at s = 0.33, which is up by the vanishing point
    where the stroke is two pixels wide, and left the entire thick near half a
    single flat red. By screen arc length instead, each of the three bands gets
    a third of the ink, which is what makes all three legible.

    The wobble is there because a real line is never smooth: a perfectly clean
    gradient reads as a logo rather than as data."""
    base = -1.0 + 2.0 * math.exp(-(((u - centre) / sigma) ** 2))
    wob = 0.11 * math.sin(wob_s * 21.0) + 0.06 * math.sin(wob_s * 41.0 + 1.7)
    return clamp(base + wob, -1.0, 1.0)


def exit_s(sc, carve, steps=3000):
    """Where the run leaves the frame, found by walking it rather than kept as
    a hand-maintained constant -- every one of the curve and camera numbers
    above has been retuned at least once, and a stale copy of this would
    silently put the colour bands in the wrong place."""
    slack = 0.03 * sc.w
    for i in range(steps + 1):
        s = i / steps
        q = sc.project(curve(s, carve=carve))
        if not (-slack < q[0] < sc.w + slack and -slack < q[1] < sc.h + slack):
            return s
    return 1.0


def arc_fractions(pts):
    """Cumulative screen distance along a projected polyline, normalised to
    0..1. Returns one value per point."""
    acc, total = [0.0], 0.0
    for i in range(1, len(pts)):
        total += math.hypot(pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1])
        acc.append(total)
    if total <= 0.0:
        return [0.0] * len(pts)
    return [a / total for a in acc]


def line_mvg(sc, ss, steps, width, wmin, wmax, carve, s_end):
    """width/wmin/wmax are in FINAL pixels; everything drawn here is in
    supersampled ones, hence the ss. Getting that wrong is invisible in the
    numbers and obvious in the picture.

    Drawing stops at s_end, where the run leaves the frame. That is not just an
    optimisation: the off-screen tail is enormous in screen space -- tens of
    thousands of pixels as the projection divides by a depth heading for zero --
    so leaving it in would swamp the arc-length measure and squeeze every colour
    band into the first inch of the visible line."""
    pts, depths = [], []
    for i in range(steps + 1):
        s = s_end * i / steps
        p = curve(s, carve=carve)
        pts.append(sc.project(p))
        depths.append(p[1])
    frac = arc_fractions(pts)

    out = ["stroke-linecap round", "fill none"]
    for i in range(1, len(pts)):
        u = 0.5 * (frac[i] + frac[i - 1])
        w = clamp(width / depths[i], wmin, wmax) * ss
        out.append("stroke %s" % hexof(eff_colour(eta_at(u, wob_s=u))))
        out.append("stroke-width %.2f" % w)
        out.append("line %.2f,%.2f %.2f,%.2f" %
                   (pts[i - 1][0], pts[i - 1][1], pts[i][0], pts[i][1]))
    return out


def ramp_mvg(sc, ss):
    out = ["fill none", "stroke-linecap butt"]

    def seg(a, b, shade, wid):
        pa, pb = sc.project(a), sc.project(b)
        if not sc.visible(pa, pb, sc.w):
            return
        out.append("stroke %s" % hexof(shade))
        out.append("stroke-width %.2f" % (wid * ss))
        out.append("line %.2f,%.2f %.2f,%.2f" % (pa[0], pa[1], pb[0], pb[1]))

    near, far = 1.5, 90.0

    # Stop where the surface would rise past the camera's own height. Beyond
    # that the plane is ABOVE the horizon and its lines cross back over the top
    # of the frame, which stops reading as a floor and starts reading as a
    # lattice. x_max is where ramp_z(x) reaches -0.3.
    x_max = (-0.3 - RAMP_B) / RAMP_A

    # Running away from the camera, one per x. Split into pieces so each piece
    # can be shaded by its own depth -- a single stroke could only have one
    # colour, and the whole effect is that the grid fades into the distance.
    x = -16.0
    while x <= x_max + 0.01:
        steps = 30
        for i in range(steps):
            d0 = near + (far - near) * (i / steps) ** 2.1
            d1 = near + (far - near) * ((i + 1) / steps) ** 2.1
            t = clamp(1.0 - d0 / 26.0, 0.0, 1.0) ** 1.4
            seg((x, d0, ramp_z(x)), (x, d1, ramp_z(x)),
                mix(RAMP_FAR, RAMP_NEAR, t), 0.45 + 1.5 * t)
        x += 2.6

    # Across it, one per depth, spaced so they crowd towards the horizon the way
    # equally spaced lines on a real plane do.
    i = 0
    while True:
        d = near + 1.15 * (i ** 1.9)
        if d > far:
            break
        t = clamp(1.0 - d / 26.0, 0.0, 1.0) ** 1.4
        seg((-16.0, d, ramp_z(-16.0)), (16.0, d, ramp_z(16.0)),
            mix(RAMP_FAR, RAMP_NEAR, t * 0.7), 0.45 + 1.2 * t)
        i += 1
    return out


# --- rendering ---------------------------------------------------------------

def run(*args):
    subprocess.run(args, check=True)


def write_mvg(name, lines):
    p = os.path.join(TMP, name)
    with open(p, "w") as fh:
        fh.write("\n".join(lines) + "\n")
    return p


def render(w, h, f, cx, cy, width, out, ss=3, ramp=True, vignette=0.0,
           flop=False, wmin=1.0, wmax=60.0, carve=2.4, steps=560):
    """The glow is two blurs of the line screened back under a sharp copy -- one
    tight for the core, one wide for the bloom -- because a single radius either
    loses the thin far end or smears the near one into a blob."""
    W, H = w * ss, h * ss
    sc = Scene(W, H, f * ss, cx * ss, cy * ss)

    canvas = os.path.join(TMP, "bg.png")
    run("magick", "-size", "%dx%d" % (W, H), "xc:" + BG, canvas)
    if ramp:
        run("magick", canvas, "-draw", "@" + write_mvg("ramp.mvg", ramp_mvg(sc, ss)),
            canvas)

    s_end = min(1.0, exit_s(sc, carve) + 0.01)
    core = os.path.join(TMP, "core.png")
    run("magick", "-size", "%dx%d" % (W, H), "xc:none", "-draw",
        "@" + write_mvg("line.mvg",
                        line_mvg(sc, ss, steps, width, wmin, wmax, carve, s_end)),
        core)

    g1, g2 = os.path.join(TMP, "g1.png"), os.path.join(TMP, "g2.png")
    run("magick", core, "-channel", "RGBA", "-blur", "0x%d" % (4 * ss), "+channel",
        "-evaluate", "multiply", "0.90", g1)
    run("magick", core, "-channel", "RGBA", "-blur", "0x%d" % (16 * ss), "+channel",
        "-evaluate", "multiply", "0.60", g2)

    args = ["magick", canvas,
            g2, "-compose", "screen", "-composite",
            g1, "-compose", "screen", "-composite",
            core, "-compose", "over", "-composite"]
    if vignette > 0.0:
        vg = os.path.join(TMP, "vg.png")
        run("magick", "-size", "%dx%d" % (W, H), "radial-gradient:none-black",
            "-evaluate", "multiply", str(vignette), vg)
        args += [vg, "-compose", "over", "-composite"]
    if flop:
        args += ["-flop"]
    args += ["-filter", "Lanczos", "-resize", "%dx%d" % (w, h), out]
    run(*args)


def social():
    out = os.path.join(HERE, "social.png")
    # flop: the scene is built with the ramp rising to the right and the line
    # leaving bottom-left, then mirrored, so the run exits bottom-RIGHT and the
    # wordmark gets the quiet corner instead of fighting the thickest stroke.
    render(1280, 640, 520, 538, 180, 42.0, out,
           ss=3, vignette=0.28, flop=True, wmin=1.0, wmax=58.0)

    # The wordmark goes on after the downsample, so the text is rasterised once
    # at final size rather than resampled.
    run("magick", out,
        "-font", "Consolas-Bold", "-pointsize", "58", "-fill", "#F2F5FA",
        "-gravity", "NorthWest", "-annotate", "+58+398", "Demo Line Practice Tool",
        "-font", "Consolas", "-pointsize", "25", "-fill", "#98A5B8",
        "-annotate", "+62+474", "Other players' runs, drawn in the map you are playing",
        "-font", "Consolas", "-pointsize", "22", "-fill", "#5A6880",
        "-annotate", "+62+519", "Momentum Mod   /   reads .mtv demos   /   MIT",
        out)
    print("social.png")


# --- the icon ----------------------------------------------------------------
# The card and the icon cannot be the same picture at two sizes. What makes the
# card work is the run arriving out of the distance, and "arriving out of the
# distance" means most of the stroke is a pixel wide -- which at 64 px is one
# pixel total, and at 16 px is nothing at all.
#
# So the icon is the same curve with the far two thirds thrown away: only the
# near, fat part of the swoop, cropped to fill the square. The camera is not
# moved to do that. The kept arc is projected exactly as before and then fitted
# to the frame in 2-D, so the shape is still a real perspective curve rather
# than a hand-drawn S that happens to resemble one.
ICON_S0, ICON_S1 = 0.42, 0.885


def mark_mvg(size, ss, pad, wmin, wmax, steps=260):
    """Project the kept arc, then fit its bounding box to the square. Fitting in
    2-D after projection, instead of hunting for a camera that happens to frame
    it, means the mark fills the icon by construction at any size.

    The width range is deliberately narrow. On the card the stroke runs about
    forty to one, because that ratio IS the perspective; here a fortieth of the
    mark is a fifth of a pixel at 16 px, so the thin end would simply not be
    drawn and the icon would be a red comma."""
    base = Scene(1000, 1000, 520, 500, 180)
    pts, ws = [], []
    for i in range(steps + 1):
        s = ICON_S0 + (ICON_S1 - ICON_S0) * i / steps
        p = curve(s)
        pts.append(base.project(p))
        ws.append(1.0 / p[1])

    xs, ys = [q[0] for q in pts], [q[1] for q in pts]
    bw, bh = max(xs) - min(xs), max(ys) - min(ys)
    avail = size - 2.0 * pad
    k = avail / max(bw, bh)
    ox = pad + (avail - bw * k) * 0.5 - min(xs) * k
    oy = pad + (avail - bh * k) * 0.5 - min(ys) * k
    fit = [(q[0] * k + ox, q[1] * k + oy) for q in pts]

    frac = arc_fractions(fit)
    lo, hi = min(ws), max(ws)
    out = ["stroke-linecap round", "fill none"]
    for i in range(1, len(fit)):
        u = 0.5 * (frac[i] + frac[i - 1])
        t = (ws[i] - lo) / (hi - lo) if hi > lo else 0.0
        w = (wmin + (wmax - wmin) * t) * ss
        out.append("stroke %s" % hexof(eff_colour(eta_at(u, sigma=0.27))))
        out.append("stroke-width %.2f" % w)
        out.append("line %.2f,%.2f %.2f,%.2f" %
                   (fit[i - 1][0] * ss, fit[i - 1][1] * ss,
                    fit[i][0] * ss, fit[i][1] * ss))
    return out


# --- the ramp icon -----------------------------------------------------------
# A triangular prism -- which is the shape of a surf ramp -- with a run boarding
# it from above, riding it down, and launching off the end.
#
# The colour key here is the ICON'S OWN and not one of the tool's ramps: blue
# for the fast approach, red down the face, green off the launch. Worth saying
# out loud, because the tool's SpeedColour puts blue at the SLOW end. What is
# borrowed is the three literal colours, so the icon at least uses paint the
# program owns: the blue is EfficiencyColour's colourblind-mode gaining blue,
# and the red and green are its ordinary losing and gaining pair.
FAST = (0.20, 0.55, 1.00)

# Everything below is a fraction of the icon's side, so the design is resolved
# once and holds at every size.
#
# The prism is given as a cross-section plus a length, which is what a prism
# actually is, and it is LONG. That is not decoration: a run does not slide to
# the bottom of a ramp and hop off the end -- it never reaches the floor at all.
# It crosses the face diagonally, travelling mostly along the ramp while the
# slope carries it down, and leaves over the lower SIDE edge into the air. A
# short wedge cannot show that, because there is no length to travel along.
# A LONG prism, laid out flat rather than in perspective, because at 16 px a
# vanishing point buys nothing and costs the near end cap all of its size.
#
# The length matters. A run does not slide to the bottom of a ramp and hop off
# the end -- it never reaches the floor at all. It crosses the face diagonally,
# travelling along the ramp while the slope trades height for speed, and leaves
# over the lower SIDE edge into the air. There has to be a length to travel
# along for any of that to be visible, and the first two attempts at this were
# a stubby wedge with nowhere to go.
PRISM_R = (0.100, 0.260)             # ridge, near end
PRISM_K = (0.100, 0.720)             # foot of the back wall, near end
PRISM_T = (0.420, 0.720)             # toe -- the lower side edge, near end
PRISM_L = (0.520, -0.060)            # along the ramp, away to the right


def _off(p, d, k=1.0):
    return (p[0] + d[0] * k, p[1] + d[1] * k)


def _face(a, b):
    """A point on the sloped face. a runs 0..1 along the ramp, b runs 0 at the
    ridge to 1 at the toe."""
    R, T, L = PRISM_R, PRISM_T, PRISM_L
    return (R[0] + L[0] * a + (T[0] - R[0]) * b,
            R[1] + L[1] * a + (T[1] - R[1]) * b)


def prism_mvg(size, ss):
    """The sliding face, the near end cap that shows the triangular section,
    and one outline so the whole thing holds together as a solid."""
    R, K, T, L = PRISM_R, PRISM_K, PRISM_T, PRISM_L
    R2, T2 = _off(R, L), _off(T, L)

    def poly(pts, fill):
        s = " ".join("%.2f,%.2f" % (p[0] * size * ss, p[1] * size * ss) for p in pts)
        return ["fill %s" % fill, "stroke none", "polygon %s" % s]

    # The face first, then the end cap much darker on top. The two fills
    # together are the whole silhouette, so the prism is one solid object -- but
    # only if the tones are far enough apart to say which is in front. At two
    # shades of the same grey it read as two overlapping rectangles.
    out = []
    out += poly([R, T, T2, R2], "#3B4860")       # the face the run rides
    out += poly([R, K, T], "#1C2430")            # near end cap, in shadow

    # One outline round the outside, so the shape holds together at the sizes
    # where the fills are three pixels of nearly the same colour.
    edge = " ".join("%.2f,%.2f" % (p[0] * size * ss, p[1] * size * ss)
                    for p in (R2, R, K, T, T2))
    out += ["fill none", "stroke #5E6E8D", "stroke-linejoin round",
            "stroke-width %.2f" % (0.014 * size * ss), "polygon %s" % edge]
    return out


def catmull(ctrl, per=48):
    """Catmull-Rom through the control points, ends duplicated. Used so the
    corner where the ramp becomes the launch is a rounded arc rather than a
    kink -- a hard corner survives downsampling as a smudge."""
    P = [ctrl[0]] + list(ctrl) + [ctrl[-1]]
    out = []
    for i in range(len(P) - 3):
        p0, p1, p2, p3 = P[i], P[i + 1], P[i + 2], P[i + 3]
        for j in range(per):
            t = j / per
            t2, t3 = t * t, t * t * t
            out.append(tuple(
                0.5 * ((2 * p1[k]) + (-p0[k] + p2[k]) * t
                       + (2 * p0[k] - 5 * p1[k] + 4 * p2[k] - p3[k]) * t2
                       + (-p0[k] + 3 * p1[k] - 3 * p2[k] + p3[k]) * t3)
                for k in range(2)))
    out.append(tuple(ctrl[-1]))
    return out


def ramp_colour(u):
    """Blue in, red down the face, green off the launch.

    Blue is held flat for the whole approach instead of starting to blend from
    the first pixel. Blending from u = 0 meant the approach was already a third
    of the way to red by the time it entered the tile, and it read as purple --
    a fourth colour nobody asked for, in the one band that is supposed to say
    "fast".

    The flip to green is quick for the same kind of reason: a slow blend spends
    the pixels that ought to be carrying green on a muddy olive."""
    if u < 0.30:                       # the drop in: touchdown is at u = 0.307
        return FAST
    if u < 0.66:
        return mix(FAST, LOSS, (u - 0.30) / 0.36)
    if u < 0.79:                       # the flip straddles the departure, 0.737
        return mix(LOSS, GAIN, (u - 0.66) / 0.13)
    return GAIN


def ramp_line_mvg(size, ss, wmin=0.052, wmax=0.088):
    """The run: in from above across the ridge, diagonally along the face, off
    the lower side edge and away.

    It crosses the face rather than running down it. A surfer holds height by
    travelling along the ramp and lets the slope trade it for speed, so the
    track is a long diagonal that reaches the lower edge near the far end --
    and it leaves over that SIDE edge, into the air, without ever reaching the
    floor. Sliding to the toe and hopping off the end is not what the movement
    does.

    The approach is set perpendicular to the ridge rather than merely steep, so
    the two lines read as crossing rather than as one bent line. Both ends run
    off the tile: a stroke that stops inside the frame reads as a finished
    shape, and this is meant to read as something passing through.

    The stroke is thin and tapers slightly, widening as it comes towards you.
    A line as fat as the surface it rides stops being a line on a ramp and
    becomes a stripe, which is what the first version of this was."""
    L = PRISM_L
    ln = math.hypot(L[0], L[1])
    perp = (-L[1] / ln, L[0] / ln)     # across the ridge and onto the face

    touch = _face(0.12, 0.20)          # just over the ridge, near end
    leave = _face(0.55, 1.00)          # out over the lower side edge, mid-ramp

    ctrl = [
        (touch[0] - perp[0] * 0.42, touch[1] - perp[1] * 0.42),   # off the top
        touch,
        _face(0.28, 0.48),             # across the face, along and down at once
        _face(0.44, 0.78),
        leave,
        (0.845, 0.730),                # airborne, levelling off
        (1.060, 0.735),                # away, off the tile
    ]
    pts = catmull(ctrl, per=54)
    frac = arc_fractions(pts)

    out = ["stroke-linecap round", "fill none"]
    for i in range(1, len(pts)):
        u = 0.5 * (frac[i] + frac[i - 1])
        w = (wmin + (wmax - wmin) * u) * size * ss
        out.append("stroke %s" % hexof(ramp_colour(u)))
        out.append("stroke-width %.2f" % w)
        out.append("line %.2f,%.2f %.2f,%.2f" %
                   (pts[i - 1][0] * size * ss, pts[i - 1][1] * size * ss,
                    pts[i][0] * size * ss, pts[i][1] * size * ss))
    return out


def compose_icon(mark, ico_name, preview_name, under=None, size=256, ss=4,
                 glow=3):
    S = size * ss
    r = int(S * 0.17)

    plate = os.path.join(TMP, "plate.png")
    run("magick", "-size", "%dx%d" % (S, S), "xc:none",
        "-fill", "#0A0E15", "-stroke", "#1B2130", "-strokewidth", str(2 * ss),
        "-draw", "roundrectangle %d,%d %d,%d %d,%d" % (ss, ss, S - ss, S - ss, r, r),
        plate)
    if under:
        # Clipped to the plate, so the wedge can run off the edges without
        # spilling out of the rounded corners.
        body = os.path.join(TMP, "body.png")
        run("magick", "-size", "%dx%d" % (S, S), "xc:none",
            "-draw", "@" + write_mvg("under.mvg", under), body)
        run("magick", body, plate, "-alpha", "set",
            "-compose", "dst-in", "-composite", body)
        run("magick", plate, body, "-compose", "over", "-composite", plate)

    core = os.path.join(TMP, "mark.png")
    run("magick", "-size", "%dx%d" % (S, S), "xc:none",
        "-draw", "@" + write_mvg("mark.mvg", mark), core)

    # One tight glow only. A wide bloom is invisible at 32 px and turns the
    # whole tile into a smudge at 16.
    gl = os.path.join(TMP, "mark_glow.png")
    run("magick", core, "-channel", "RGBA", "-blur", "0x%d" % (glow * ss),
        "+channel", "-evaluate", "multiply", "0.85", gl)

    master = os.path.join(TMP, "icon_master.png")
    run("magick", plate,
        gl, "-compose", "screen", "-composite",
        core, "-compose", "over", "-composite",
        "-filter", "Lanczos", "-resize", "%dx%d" % (size, size),
        plate, "-compose", "dst-in", "-composite",     # keep the rounded corners
        master)

    run("magick", master, "-define", "icon:auto-resize=256,128,96,64,48,32,24,16",
        os.path.join(HERE, ico_name))

    # Proof at the sizes that actually matter, laid out side by side and blown
    # back up, because "does this survive 16 px" is not a question to answer by
    # looking at a 256 px render of it.
    tiles = []
    for n in (64, 48, 32, 16):
        t = os.path.join(TMP, "p%d.png" % n)
        run("magick", master, "-filter", "Lanczos", "-resize", "%dx%d" % (n, n), t)
        big = os.path.join(TMP, "b%d.png" % n)
        run("magick", t, "-filter", "point", "-resize", "192x192", big)
        tiles.append(big)
    run("magick", "montage", *tiles, "-tile", "4x1", "-geometry", "+8+8",
        "-background", "#1C1C1C", os.path.join(HERE, preview_name))
    print("%s  +  %s" % (ico_name, preview_name))


def icon_ramp():
    size, ss = 256, 4
    compose_icon(ramp_line_mvg(size, ss), "wrlines.ico", "icon_preview.png",
                 under=prism_mvg(size, ss), size=size, ss=ss)


def icon_swoop():
    """The first design, kept buildable so the file it produced is not a binary
    nobody can regenerate."""
    size, ss = 256, 4
    compose_icon(mark_mvg(size, ss, pad=22.0, wmin=27.0, wmax=48.0),
                 "wrlines_swoop.ico", "icon_preview_swoop.png", size=size, ss=ss)


if __name__ == "__main__":
    os.makedirs(TMP, exist_ok=True)
    what = sys.argv[1] if len(sys.argv) > 1 else "all"
    if what in ("social", "all"):
        social()
    if what in ("icon", "all"):
        icon_ramp()
    if what == "swoop":
        icon_swoop()
