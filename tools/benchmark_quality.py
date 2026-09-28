"""Benchmark JPEG quality at the current resolution and restore settings."""

import argparse
import getpass
import http.client
import json
import time

import benchmark_resolutions as bench


def set_quality(host, source, key, current, quality):
    payload = {
        "ssid": current["ssid"], "wifi_password": "",
        "resolution": current["resolution"], "quality": quality,
        "mirror": current["mirror"], "flip": current["flip"],
    }
    conn = bench.connection(host, 80, source)
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
    deadline = time.monotonic() + 40
    while time.monotonic() < deadline:
        try:
            updated = bench.status(host, source)
            if updated["camera"] and updated["quality"] == quality:
                return updated
        except (OSError, ValueError, KeyError):
            pass
        time.sleep(1)
    raise TimeoutError(f"Camera did not return at quality {quality}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--source", help="Local network interface IP")
    parser.add_argument("--port", help="Serial port for reading saved admin key")
    parser.add_argument("--seconds", type=float, default=10)
    parser.add_argument("--qualities", type=int, nargs="+", default=[8, 12, 16, 20, 30])
    args = parser.parse_args()

    original = bench.wait_for_status(args.host, args.source)
    key = bench.key_from_serial(args.port) if args.port else getpass.getpass("Current admin key: ")
    bench.wait_for_status(args.host, args.source)
    try:
        for quality in args.qualities:
            current = bench.wait_for_status(args.host, args.source)
            if current["quality"] != quality:
                current = set_quality(args.host, args.source, key, current, quality)
            result = bench.benchmark(args.host, args.source, args.seconds)
            bandwidth = result["average_jpeg_bytes"]
            result["jpeg_mbytes_per_s"] = round(bandwidth * result["fps"] / 1_000_000, 3) if bandwidth else None
            print(json.dumps({"resolution": current["resolution"], "quality": quality, **result}), flush=True)
    finally:
        current = bench.wait_for_status(args.host, args.source, require_camera=False)
        if current["quality"] != original["quality"]:
            restored = set_quality(args.host, args.source, key, current, original["quality"])
            print(json.dumps({"restored_resolution": restored["resolution"],
                              "restored_quality": restored["quality"]}), flush=True)


if __name__ == "__main__":
    main()
