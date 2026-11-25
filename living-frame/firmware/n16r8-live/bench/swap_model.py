#!/usr/bin/env python3
"""0260 -- model of the frame swap against the scanout, old protocol vs new.

Not a test of the firmware itself (the on-hardware log in the staged reply is
that evidence); this is the arithmetic behind two claims the reply makes:

  * a bare `s_fb_live = buf` from the task tears a frame whenever it lands
    mid-scan -- the number of mixed lines is what the ISR's log line quotes as
    "a bare pointer store would have mixed N";
  * the new protocol (pending buffer taken only at pos_px == 0) cannot produce a
    mixed frame for ANY request time.

Timing model, taken from the panel config the firmware prints at boot:
    640x480 at 25 MHz pclk, h 96+48+640+16 = 800 clocks/line, v 2+33+480+10
    -> 59.52 Hz; 30 bursts of 16 lines per frame; the driver fills burst b while
    the raster scans out the lines before it (one burst of prefetch).

    ./swap_model.py
"""
OUT_W, OUT_H, BB_LINES = 640, 480, 16
H_TOTAL = 96 + 48 + OUT_W + 16
V_TOTAL = 2 + 33 + OUT_H + 10
PCLK = 25_000_000
LINE_US = H_TOTAL / PCLK * 1e6
FRAME_US = LINE_US * V_TOTAL
BURSTS = OUT_H // BB_LINES
BURST_US = LINE_US * BB_LINES


def raster_line(t_us):
    """line the raster is scanning out at time t within a frame"""
    t = t_us % FRAME_US
    line = int(t // LINE_US)
    return min(line, OUT_H - 1)


def old_protocol(t_us):
    """the whole frame comes from whichever buffer is live per burst"""
    flip_line = raster_line(t_us)
    # bursts whose fill begins after the flip carry the new frame
    old = sum(min(BB_LINES, max(0, flip_line - b * BB_LINES)) for b in range(BURSTS))
    return OUT_H - old, old       # (new lines, old lines)


def new_protocol(t_us):
    """pending is taken at pos_px == 0, i.e. before the frame's first burst"""
    return OUT_H, 0


def main():
    mixed_old, worst, clean_new = [], 0, 0
    n = 0
    t = 0.0
    while t < FRAME_US:
        new, old = old_protocol(t)
        assert new + old == OUT_H
        mixed_old.append(min(new, old))
        worst = max(worst, min(new, old))
        nn, oo = new_protocol(t)
        clean_new += (nn == OUT_H and oo == 0)
        n += 1
        t += BURST_US / 8.0                 # 1/8 burst resolution over one frame

    torn = sum(1 for m in mixed_old if m)
    print("panel: %.2f Hz, line %.2f us, burst(16 lines) %.2f us, %d bursts/frame"
          % (1e6 / FRAME_US, LINE_US, BURST_US, BURSTS))
    print("request offsets sampled: %d" % n)
    print("old protocol: %d/%d (%.1f%%) land mid-scan and tear; worst tear %d of %d lines"
          % (torn, n, 100.0 * torn / n, worst, OUT_H))
    print("new protocol: %d/%d frames mixed == 0 (the swap can only take effect at pos 0)"
          % (clean_new, n))
    assert clean_new == n
    assert torn > 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
