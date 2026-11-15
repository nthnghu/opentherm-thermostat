# Usage: snap.py PORT OUT.png  (or OUT.bmp when PIL is missing)
import serial, sys, time, struct, zlib
port, out = sys.argv[1], sys.argv[2]
s = serial.Serial(port, 115200, timeout=0.5)
s.reset_input_buffer()
s.write(b"\nSNAP\n")
buf = b""
t = time.time()
while b"SNAP " not in buf or not buf[buf.index(b"SNAP "):].count(b"\n"):
    buf += s.read(4096)
    if time.time() - t > 5:
        sys.exit("no answer")
i = buf.index(b"SNAP ")
j = buf.index(b"\n", i)
n = int(buf[i + 5:j])
data = buf[j + 1:]
s.timeout = 2
while len(data) < n:
    chunk = s.read(n - len(data))
    if not chunk:
        sys.exit("short read %d/%d" % (len(data), n))
    data += chunk
data = data[:n]
W, H = 480, 800
rows = []
for y in range(H):
    row = bytearray(b"\x00")
    for x in range(W):
        p = data[(y * W + x) * 2] | (data[(y * W + x) * 2 + 1] << 8)
        row += bytes((((p >> 11) & 31) * 255 // 31, ((p >> 5) & 63) * 255 // 63, (p & 31) * 255 // 31))
    rows.append(bytes(row))
raw = b"".join(rows)
def chunk(tag, d):
    return struct.pack(">I", len(d)) + tag + d + struct.pack(">I", zlib.crc32(tag + d) & 0xffffffff)
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
open(out, "wb").write(png)
print("saved", out)
