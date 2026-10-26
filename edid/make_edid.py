#!/usr/bin/env python3
"""Generate an EDID that offers high-resolution, high-refresh modes to a VM.

QEMU's virtio-gpu EDID only offers 50-75 Hz modes. This builds a replacement
for the kernel's drm.edid_firmware= option:

  * base block (EDID 1.4): the preferred mode as a detailed timing, plus a few
    low-resolution fallbacks
  * DisplayID 1.3 extension blocks: every resolution x refresh combination as
    Type I detailed timings (no 655 MHz DTD pixel clock limit). Type I is
    read by both the kernel and libdisplay-info; DisplayID 2.0 Type VII is
    not decoded by libdisplay-info.

All timings are CVT reduced blanking v2. No range-limits descriptor is
included and "continuous frequency" is off, so the kernel adds no inferred
modes: the mode list is exactly what is generated here.

The preferred mode must be 1024x768, or match the scanout size QEMU reports:
virtio-gpu deletes any other mode marked preferred, and compositors then fall
back to the largest mode in the list. The reported size starts as the device's
xres/yres but can follow the console client (e.g. noVNC remote resizing), so
1024x768 is the only size that is always kept.

Run with --help for options; --list prints the modes without writing a file.
"""
import argparse
import math
import struct
import sys

DEFAULT_RESOLUTIONS = [
    # 16:9
    "1920x1080", "2560x1440", "3840x2160", "5120x2880",
    # 16:10
    "1920x1200", "2560x1600",
    # 21:9 and similar ultrawides
    "2560x1080", "3440x1440", "3840x1600", "5120x2160",
    # 32:9 super ultrawides
    "3840x1080", "5120x1440", "7680x2160",
]
DEFAULT_RATES = [60, 75, 100, 120, 144, 165, 175, 180, 200, 240, 280, 360]
DEFAULT_PREFERRED = "1024x768@60"   # virtio-gpu always keeps a 1024x768 preferred mode
DEFAULT_NAME = "VM Display"
TIMINGS_PER_BLOCK = 5          # 5 x 20-byte Type I timings fit one DisplayID block
MAX_DTD_CLOCK_KHZ = 655350     # EDID detailed timing limit
MAX_TYPE1_CLOCK_KHZ = (1 << 24) * 10  # 24-bit field in 10 kHz units
MAX_SIZE = 8192                # virtio-gpu's maximum width/height
SIZE_MM = (600, 340)           # ~27" 16:9; only used for DPI hints


def cvt_rb2(w, h, hz):
    """CVT reduced blanking v2 timing."""
    hfront, hsync, hback = 8, 32, 40
    vsync, vback, vfront_min, min_vblank_us = 8, 6, 1, 460.0
    htotal = w + hfront + hsync + hback
    hperiod_us = (1e6 / hz - min_vblank_us) / h
    vblank = max(math.ceil(min_vblank_us / hperiod_us), vsync + vback + vfront_min)
    vtotal = h + vblank
    return dict(w=w, h=h, hz=hz, clock_khz=round(hz * htotal * vtotal / 1000),
                htotal=htotal, vtotal=vtotal, hfront=hfront, hsync=hsync,
                hblank=htotal - w, vfront=vblank - vsync - vback, vsync=vsync, vblank=vblank)


def aspect_code(w, h):
    """DisplayID Type I aspect ratio code (8 = undefined, compute from pixels)."""
    codes = {(1, 1): 0, (5, 4): 1, (4, 3): 2, (15, 9): 3, (16, 9): 4,
             (16, 10): 5, (64, 27): 6, (256, 135): 7}
    g = math.gcd(w, h)
    return codes.get((w // g, h // g), 8)


def fit_dtd(t):
    """Same timing with the vertical front porch capped at a DTD's 6 bits.

    The excess becomes back porch, so total blanking and refresh are unchanged.
    The preferred mode uses this in both the base block and DisplayID, so the
    kernel sees two identical modes and merges them.
    """
    return dict(t, vfront=min(t["vfront"], 63))


def dtd(t, size_mm):
    """18-byte EDID detailed timing descriptor (hsync+, vsync-, as CVT-RB)."""
    clk = round(t["clock_khz"] / 10)
    w, hb, h, vb = t["w"], t["hblank"], t["h"], t["vblank"]
    hf, hs, vf, vs = t["hfront"], t["hsync"], t["vfront"], t["vsync"]
    assert vf <= 63, "use fit_dtd() first"
    wmm, hmm = size_mm
    return bytes([
        clk & 0xFF, clk >> 8,
        w & 0xFF, hb & 0xFF, ((w >> 8) << 4) | (hb >> 8),
        h & 0xFF, vb & 0xFF, ((h >> 8) << 4) | (vb >> 8),
        hf & 0xFF, hs & 0xFF, ((vf & 0xF) << 4) | (vs & 0xF),
        ((hf >> 8) << 6) | ((hs >> 8) << 4) | ((vf >> 4) << 2) | (vs >> 4),
        wmm & 0xFF, hmm & 0xFF, ((wmm >> 8) << 4) | (hmm >> 8),
        0, 0, 0x1A,
    ])


def text_descriptor(tag, text):
    body = text.encode("ascii")[:13]
    if len(body) < 13:
        body += b"\n" + b" " * (12 - len(body))
    return bytes([0, 0, 0, tag, 0]) + body


def dummy_descriptor():
    return bytes([0, 0, 0, 0x10]) + bytes(14)


def checksum_fix(block):
    block[-1] = (-sum(block[:-1])) & 0xFF


def base_block(preferred, name, extensions):
    b = bytearray(128)
    b[0:8] = b"\x00\xff\xff\xff\xff\xff\xff\x00"
    mfg = ((ord("L") - 64) << 10) | ((ord("N") - 64) << 5) | (ord("X") - 64)
    b[8:10] = struct.pack(">H", mfg)       # "LNX", as the kernel's built-in EDIDs
    b[10:12] = struct.pack("<H", 0x0502)
    b[16], b[17] = 0, 2026 - 1990
    b[18], b[19] = 1, 4                    # EDID 1.4
    b[20] = 0xA5                           # digital, 8 bpc, DisplayPort
    b[21], b[22] = SIZE_MM[0] // 10, SIZE_MM[1] // 10   # cm; only used for DPI hints
    b[23] = 120                            # gamma 2.2
    b[24] = 0x06                           # sRGB default, preferred is native; not continuous
    b[25:35] = bytes.fromhex("ee91a3544c99260f5054")
    b[35:38] = bytes([0x21, 0x00, 0x00])   # 640x480@60, 800x600@60 (1024x768 is the preferred DTD)
    std = [(1280, 720), (1280, 800)]
    for i in range(8):
        if i < len(std):
            w, h = std[i]
            b[38 + 2 * i] = w // 8 - 31
            b[39 + 2 * i] = (0b11 if w * 9 == h * 16 else 0b00) << 6  # 60 Hz
        else:
            b[38 + 2 * i:40 + 2 * i] = b"\x01\x01"
    descs = [dtd(preferred, SIZE_MM), text_descriptor(0xFC, name),
             dummy_descriptor(), dummy_descriptor()]
    for i, d in enumerate(descs):
        b[54 + 18 * i:72 + 18 * i] = d
    b[126] = extensions
    checksum_fix(b)
    return b


def type1(t, preferred=False):
    """20-byte DisplayID 1.3 Type I timing (pixel clock in 10 kHz, fields minus 1)."""
    le16 = lambda v: struct.pack("<H", v)
    d = bytearray(struct.pack("<I", round(t["clock_khz"] / 10) - 1)[:3])
    d.append((0x80 if preferred else 0) | aspect_code(t["w"], t["h"]))
    d += le16(t["w"] - 1) + le16(t["hblank"] - 1)
    d += le16((t["hfront"] - 1) | 0x8000) + le16(t["hsync"] - 1)   # hsync positive
    d += le16(t["h"] - 1) + le16(t["vblank"] - 1)
    d += le16(t["vfront"] - 1) + le16(t["vsync"] - 1)               # vsync negative
    return bytes(d)


def display_parameters(native, size_mm):
    """DisplayID 1.3 Display Parameters data block (tag 0x01)."""
    w, h = native
    wmm, hmm = size_mm
    aspect = min(round(w / h * 100) - 100, 255)
    body = struct.pack("<HHHH", wmm * 10, hmm * 10, w, h)   # size in 0.1 mm, native pixels
    body += bytes([0x00, 120, aspect, 0x77])                 # no features, gamma 2.2, aspect, 8 bpc
    return bytes([0x01, 0x00, len(body)]) + body


def displayid_blocks(preferred, timings, size_mm):
    """DisplayID sections, one per extension block. The first starts with the
    display parameters and the preferred mode (flagged preferred)."""
    entries = [(preferred, True)] + [(t, False) for t in timings]
    chunks = [entries[i:i + TIMINGS_PER_BLOCK] for i in range(0, len(entries), TIMINGS_PER_BLOCK)]
    blocks = []
    for n, chunk in enumerate(chunks):
        descs = b"".join(type1(t, pref) for t, pref in chunk)
        payload = b""
        if n == 0:
            payload += display_parameters((preferred["w"], preferred["h"]), size_mm)
        payload += bytes([0x03, 0x00, len(descs)]) + descs  # Type I timing data block
        # Zero filler inside the section: libdisplay-info misreports "non-zero
        # padding" when the data blocks end exactly at the section checksum.
        payload += bytes(3)
        # The first section names the product type (3 = standalone display) and
        # counts the sections that follow; later sections are extensions (0).
        product, ext_count = (3, len(chunks) - 1) if n == 0 else (0, 0)
        section = bytearray([0x13, len(payload), product, ext_count]) + payload
        section.append((-sum(section)) & 0xFF)
        b = bytearray(128)
        b[0] = 0x70
        b[1:1 + len(section)] = section
        checksum_fix(b)
        blocks.append(b)
    return blocks


def parse_res(s):
    w, h = (int(v) for v in s.lower().split("x"))
    if not (0 < w <= MAX_SIZE and 0 < h <= MAX_SIZE):
        raise argparse.ArgumentTypeError(f"{s}: width and height must be 1-{MAX_SIZE}")
    return w, h


def parse_mode(s):
    res, _, hz = s.partition("@")
    return (*parse_res(res), int(hz or 60))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("-o", "--output", default="vm-edid.bin", help="output file (default: %(default)s)")
    ap.add_argument("-r", "--resolutions", type=lambda s: [parse_res(x) for x in s.split(",")],
                    default=[parse_res(x) for x in DEFAULT_RESOLUTIONS],
                    help="comma-separated WxH list (default: %s)" % ",".join(DEFAULT_RESOLUTIONS))
    ap.add_argument("-f", "--rates", type=lambda s: [int(x) for x in s.split(",")],
                    default=DEFAULT_RATES,
                    help="comma-separated refresh rates in Hz (default: %s)" % ",".join(map(str, DEFAULT_RATES)))
    ap.add_argument("-p", "--preferred", type=parse_mode, default=parse_mode(DEFAULT_PREFERRED),
                    help="mode the desktop starts in, WxH@Hz (default: %(default)s)".replace(
                        "%(default)s", DEFAULT_PREFERRED))
    ap.add_argument("--host-size", type=parse_res, default=None,
                    help="scanout size QEMU reports (the virtio-gpu device's xres/yres); "
                         "required for a preferred mode other than 1024x768")
    ap.add_argument("--max-clock-mhz", type=float, default=0,
                    help="drop modes above this pixel clock (default: no limit)")
    ap.add_argument("-n", "--name", default=DEFAULT_NAME, help="monitor name, max 13 chars (default: %(default)s)")
    ap.add_argument("--list", action="store_true", help="print the modes and exit")
    args = ap.parse_args()

    preferred = fit_dtd(cvt_rb2(*args.preferred))
    pw, ph = preferred["w"], preferred["h"]
    # virtio_gpu_conn_mode_valid() keeps a preferred mode only if it is 1024x768
    # or at most 16 pixels smaller than the reported scanout in each direction.
    if (pw, ph) != (1024, 768):
        if not args.host_size:
            sys.exit(f"virtio-gpu deletes a preferred {pw}x{ph} mode unless the scanout size QEMU "
                     f"reports matches it. Set the VM's virtio-gpu xres/yres to {pw}x{ph} and pass "
                     f"--host-size {pw}x{ph}, or keep the default 1024x768 preferred mode.")
        hw, hh = args.host_size
        if not (hw - 16 <= pw <= hw and hh - 16 <= ph <= hh):
            sys.exit(f"preferred {pw}x{ph} doesn't match --host-size {hw}x{hh}; virtio-gpu would delete it")
    if preferred["clock_khz"] > MAX_DTD_CLOCK_KHZ:
        sys.exit(f"preferred mode needs {preferred['clock_khz'] / 1000:.0f} MHz; "
                 f"an EDID detailed timing allows at most {MAX_DTD_CLOCK_KHZ / 1000:.2f} MHz")

    seen = {(preferred["w"], preferred["h"], preferred["hz"])}
    timings = []
    for w, h in args.resolutions:
        for hz in sorted(set(args.rates)):
            if (w, h, hz) in seen:
                continue
            seen.add((w, h, hz))
            t = cvt_rb2(w, h, hz)
            if args.max_clock_mhz and t["clock_khz"] > args.max_clock_mhz * 1000:
                continue
            if t["clock_khz"] >= MAX_TYPE1_CLOCK_KHZ:
                continue
            timings.append(t)

    ext = displayid_blocks(preferred, timings, SIZE_MM)
    if len(ext) > 254:
        sys.exit(f"{len(timings)} modes need {len(ext)} extension blocks; the maximum is 254")

    if args.list:
        for t in [preferred] + timings:
            print(f"{t['w']}x{t['h']}@{t['hz']}: {t['clock_khz'] / 1000:.1f} MHz")
        print(f"{1 + len(timings)} modes, {1 + len(ext)} blocks")
        return

    edid = base_block(preferred, args.name, len(ext)) + b"".join(ext)
    with open(args.output, "wb") as f:
        f.write(edid)
    print(f"wrote {args.output}: {1 + len(timings)} modes, {len(edid)} bytes "
          f"({1 + len(ext)} blocks), preferred {preferred['w']}x{preferred['h']}@{preferred['hz']}")


if __name__ == "__main__":
    main()
