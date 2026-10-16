"""Build the 1024x1024 app icon from the game's own x2icon.ico (largest entry), so no game art lives in git."""
import struct, subprocess, sys, tempfile, os

ico, out = sys.argv[1], sys.argv[2]
d = open(ico, "rb").read()
n = struct.unpack_from("<H", d, 4)[0]
best = max(range(n), key=lambda i: (d[6 + 16 * i] or 256))
w, h, cc, _, planes, bpp, size, off = struct.unpack_from("<BBBBHHII", d, 6 + 16 * best)
hdr = d[off:off + 40]
W, H2 = struct.unpack_from("<ii", hdr, 4)
H, bits = H2 // 2, struct.unpack_from("<H", hdr, 14)[0]
if bits != 24:
    sys.exit(f"unsupported icon depth {bits}")
stride = (W * 3 + 3) & ~3
pix = d[off + 40: off + 40 + stride * H]
bmp = bytearray(b"BM") + struct.pack("<IHHI", 54 + len(pix), 0, 0, 54)
bmp += struct.pack("<IiiHHIIiiII", 40, W, H, 1, 24, 0, len(pix), 2835, 2835, 0, 0) + pix
with tempfile.TemporaryDirectory() as t:
    src = os.path.join(t, "icon.bmp")
    open(src, "wb").write(bmp)
    subprocess.run(["sips", "-s", "format", "png", "-z", "1024", "1024", src, "--out", out], check=True, capture_output=True)
print(f"icon {W}x{H} -> {out}")
