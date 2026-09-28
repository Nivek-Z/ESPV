"""Measure complete MJPEG frames through the specified local network interface."""
import argparse
import http.client
import json
import re
import time

ap = argparse.ArgumentParser()
ap.add_argument('--host', required=True, help='Camera IP address')
ap.add_argument('--source', help='Optional local interface IP address')
ap.add_argument('--seconds', type=float, default=20)
args = ap.parse_args()
source = (args.source, 0) if args.source else None
c = http.client.HTTPConnection(args.host, 80, timeout=5, source_address=source)
c.request('GET', '/api/status')
r = c.getresponse()
print('STATUS', r.status, r.read().decode(), flush=True)
c.close()
c = http.client.HTTPConnection(args.host, 81, timeout=4, source_address=source)
frames = invalid = received = 0
buffer = bytearray()
started = time.monotonic()
try:
    c.request('GET', '/stream')
    r = c.getresponse()
    print('STREAM_HTTP', r.status, flush=True)
    if r.status != 200:
        raise RuntimeError('Stream unavailable')
    while time.monotonic() - started < args.seconds:
        chunk = r.read1(16384)
        if not chunk:
            break
        received += len(chunk)
        buffer.extend(chunk)
        while True:
            marker = buffer.find(b'--espvframe\r\n')
            if marker < 0:
                if len(buffer) > 100:
                    del buffer[:-100]
                break
            if marker:
                del buffer[:marker]
            header_end = buffer.find(b'\r\n\r\n')
            if header_end < 0:
                break
            match = re.search(rb'Content-Length: (\d+)', buffer[:header_end])
            if not match:
                raise RuntimeError('Missing frame length')
            end = header_end + 4 + int(match.group(1))
            if len(buffer) < end:
                break
            frame = buffer[header_end+4:end]
            if frame.startswith(b'\xff\xd8') and frame.endswith(b'\xff\xd9'):
                frames += 1
            else:
                invalid += 1
            del buffer[:end]
except Exception as e:
    print('ERROR', type(e).__name__, str(e), flush=True)
finally:
    elapsed = time.monotonic() - started
    c.close()
    print(json.dumps(dict(frames=frames, invalid=invalid, seconds=round(elapsed, 2),
                          fps=round(frames/elapsed, 2), bytes=received)), flush=True)
