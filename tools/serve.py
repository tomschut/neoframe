#!/usr/bin/env python3
"""Local hardware-test endpoint; no image decoding or dithering dependencies."""
import argparse
import hashlib
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import re
import time

FRAME_SIZE = 960000


def reference_frame(path):
    text = Path(path).read_text()
    body = text.split("{", 1)[1].split("}", 1)[0]
    frame = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})\b", body))
    if len(frame) != FRAME_SIZE:
        raise ValueError("Reference image must contain exactly 960000 bytes")
    if any((b >> 4) not in (0, 1, 2, 3, 5, 6) or (b & 15) not in (0, 1, 2, 3, 5, 6) for b in frame):
        raise ValueError("Reference image contains invalid Spectra-6 pixels")
    return frame


def handler(frame, image_url):
    etag = '"' + hashlib.sha256(frame).hexdigest() + '"'

    class Handler(BaseHTTPRequestHandler):
        def send(self, status, body=b"", content_type="application/octet-stream", length=None):
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body) if length is None else length))
            if self.path == "/frame":
                self.send_header("ETag", etag)
            self.end_headers()
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def do_GET(self):
            if self.path == "/frame":
                if self.headers.get("If-None-Match") == etag:
                    self.send(304)
                else:
                    self.send(200, frame)
            elif self.path == "/frame/truncated":
                self.send(200, frame[:1000], length=FRAME_SIZE)
            elif self.path == "/frame/oversized":
                self.send(200, frame + b"\x11")
            elif self.path == "/frame/invalid":
                self.send(200, b"\xff" + frame[1:])
            elif self.path == "/config":
                self.send(200, json.dumps({
                    "update_interval_s": 180,
                    "active_start": "08:00", "active_end": "00:00",
                    "image_url": image_url, "led_enabled": False,
                }).encode(), "application/json")
            elif self.path == "/config/garbage":
                self.send(200, b"not JSON", "application/json")
            elif self.path == "/config/partial":
                self.send(200, b'{"update_interval_s":180}', "application/json")
            elif self.path == "/config/500":
                self.send(500)
            elif self.path == "/config/timeout":
                time.sleep(15)
                self.send(200, b"{}", "application/json")
            else:
                self.send(404)

    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--frame", type=Path, help="Packed binary frame")
    source.add_argument("--reference", type=Path, help="GD example image.h")
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--image-url", default="http://127.0.0.1:8000/frame",
                        help="Device-reachable image URL returned by /config")
    args = parser.parse_args()
    if args.reference:
        frame = reference_frame(args.reference)
    elif args.frame:
        frame = args.frame.read_bytes()
    else:
        # Six vertical color bars in the GD native 1200 x 1600 buffer layout.
        row = b"".join(bytes([v]) * 100 for v in (0x00, 0x11, 0x22, 0x33, 0x55, 0x66))
        frame = row * 1600
    if len(frame) != FRAME_SIZE or any((b >> 4) not in (0, 1, 2, 3, 5, 6) or
                                      (b & 15) not in (0, 1, 2, 3, 5, 6) for b in frame):
        parser.error("Frame must be exactly 960000 bytes with valid color nibbles")
    print(f"Serving test endpoints on {args.bind}:{args.port}", flush=True)
    ThreadingHTTPServer((args.bind, args.port), handler(frame, args.image_url)).serve_forever()


if __name__ == "__main__":
    main()
