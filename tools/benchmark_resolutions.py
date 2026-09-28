"""Benchmark camera resolutions, then restore the original setting.

Requires pyserial only when --port is used. No Wi-Fi or admin passwords are logged.
"""

import argparse
import getpass
import http.client
import json
import re
import time


def connection(host, port, source=None, timeout=5):
    address = (source, 0) if source else None
    return http.client.HTTPConnection(host, port, timeout=timeout, source_address=address)


def status(host, source):
    conn = connection(host, 80, source)
    try:
        conn.request("GET", "/api/status")
        response = conn.getresponse()
        if response.status != 200:
            raise RuntimeError(f"Status HTTP {response.status}")
        return json.loads(response.read())
    finally:
        conn.close()


def wait_for_status(host, source, resolution=None, timeout=40, require_camera=True):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            current = status(host, source)
            if (not require_camera or current["camera"]) and (resolution is None or current["resolution"] == resolution):
                return current
        except (OSError, ValueError, KeyError):
            pass
        time.sleep(1)
    raise TimeoutError(f"Camera did not become ready at {resolution or 'startup'}")


def key_from_serial(port_name):
    import serial

    with serial.Serial(port_name, 115200, timeout=0.5) as port:
        port.dtr = False
        port.rts = True
        time.sleep(0.15)
        port.rts = False
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            line = port.readline().decode("utf-8", errors="replace")
            line = re.sub(r"\x1b\[[0-9;]*m", "", line)
            match = re.search(r"Configuration/AP password: (\S+)", line)
            if match:
                return match.group(1)
    raise TimeoutError("Could not read admin key from serial boot log")


def set_resolution(host, source, key, current, resolution):
    payload = {
        "ssid": current["ssid"],
        "wifi_password": "",
        "resolution": resolution,
        "quality": current["quality"],
        "mirror": current["mirror"],
        "flip": current["flip"],
    }
    conn = connection(host, 80, source)
    try:
        conn.request("POST", "/api/config", body=json.dumps(payload), headers={
            "Content-Type": "application/json", "X-Config-Key": key,
        })
        response = conn.getresponse()
        response.read()
        if response.status != 200:
            raise RuntimeError(f"Config HTTP {response.status}")
    finally:
        conn.close()
    time.sleep(2)
    return wait_for_status(host, source, resolution, require_camera=False)


def jpeg_dimensions(frame):
    if not (frame.startswith(b"\xff\xd8") and frame.endswith(b"\xff\xd9")):
        return None
    i = 2
    sof = {0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF}
    while i + 4 < len(frame):
        if frame[i] != 0xFF:
            return None
        while frame[i] == 0xFF:
            i += 1
        marker = frame[i]
        i += 1
        if marker == 0xDA:
            break
        length = int.from_bytes(frame[i:i + 2], "big")
        if length < 2 or i + length > len(frame):
            return None
        if marker in sof:
            return (int.from_bytes(frame[i + 5:i + 7], "big"),
                    int.from_bytes(frame[i + 3:i + 5], "big"))
        i += length
    return None


def benchmark(host, source, seconds):
    conn = connection(host, 81, source, timeout=8)
    frames = invalid = total_bytes = 0
    dimensions = None
    buffer = bytearray()
    started = time.monotonic()
    error = None
    try:
        conn.request("GET", "/stream")
        response = conn.getresponse()
        if response.status != 200:
            raise RuntimeError(f"Stream HTTP {response.status}")
        while time.monotonic() - started < seconds:
            chunk = response.read1(16384)
            if not chunk:
                break
            buffer.extend(chunk)
            while True:
                marker = buffer.find(b"--espvframe\r\n")
                if marker < 0:
                    if len(buffer) > 100:
                        del buffer[:-100]
                    break
                if marker:
                    del buffer[:marker]
                header_end = buffer.find(b"\r\n\r\n")
                if header_end < 0:
                    break
                match = re.search(rb"Content-Length: (\d+)", buffer[:header_end])
                if not match:
                    raise RuntimeError("Missing frame length")
                end = header_end + 4 + int(match.group(1))
                if len(buffer) < end:
                    break
                frame = buffer[header_end + 4:end]
                size = jpeg_dimensions(frame)
                if size is None or (dimensions is not None and size != dimensions):
                    invalid += 1
                else:
                    dimensions = size
                    frames += 1
                    total_bytes += len(frame)
                del buffer[:end]
    except (OSError, RuntimeError) as exc:
        error = f"{type(exc).__name__}: {exc}"
    finally:
        elapsed = time.monotonic() - started
        conn.close()
    return {
        "dimensions": dimensions, "frames": frames, "invalid": invalid,
        "seconds": round(elapsed, 2), "fps": round(frames / elapsed, 2),
        "average_jpeg_bytes": round(total_bytes / frames) if frames else None,
        "error": error,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--source", help="Local network interface IP")
    parser.add_argument("--port", help="Serial port for reading the saved admin key")
    parser.add_argument("--seconds", type=float, default=15)
    parser.add_argument("--modes", nargs="+", default=["xga", "hd", "sxga", "uxga"])
    args = parser.parse_args()

    original = wait_for_status(args.host, args.source)
    key = key_from_serial(args.port) if args.port else getpass.getpass("Current admin key: ")
    wait_for_status(args.host, args.source)
    results = {}
    try:
        for mode in args.modes:
            current = wait_for_status(args.host, args.source, require_camera=False)
            set_resolution(args.host, args.source, key, current, mode)
            result = benchmark(args.host, args.source, args.seconds)
            results[mode] = result
            print(json.dumps({"mode": mode, **result}, ensure_ascii=False), flush=True)
    finally:
        current = wait_for_status(args.host, args.source, require_camera=False)
        if current["resolution"] != original["resolution"]:
            restored = set_resolution(args.host, args.source, key, current, original["resolution"])
            print(json.dumps({"restored_resolution": restored["resolution"]}), flush=True)
    return results


if __name__ == "__main__":
    main()
