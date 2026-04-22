#!/usr/bin/env python3
"""Serves mGBA test recordings from /tmp (ephemeral, verification) and
~/recordings (permanent, feature demos) as a browseable video library on
port 8080."""

import http.server
import os
import urllib.parse

# Each directory is tagged with a label that appears in the index and is used
# as a URL prefix so files in the two locations don't collide.
SOURCES = [
    ("final",  os.path.expanduser("~/recordings"),
        "Feature demos — permanent archive of shipped features."),
    ("scratch", "/tmp/mgba-recordings",
        "Verification runs — ephemeral, cleared on reboot."),
]
PORT = 8080

PAGE = """<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>mGBA Recordings</title>
<style>
* {{ box-sizing: border-box; margin: 0; padding: 0; }}
body {{ background: #0d0d0d; color: #e0e0e0; font-family: monospace; padding: 1em; }}
h1 {{ color: #00ff88; margin-bottom: 1em; font-size: 1.2em; }}
h2 {{ color: #00c0ff; margin-top: 1.5em; margin-bottom: 0.2em; font-size: 1em; }}
.section-note {{ color: #777; font-size: 0.8em; margin-bottom: 1em; }}
.empty {{ color: #555; font-size: 0.85em; margin-bottom: 1em; }}
.card {{ background: #1a1a1a; border: 1px solid #2a2a2a; border-radius: 6px;
         margin-bottom: 1.5em; padding: 1em; }}
.name {{ color: #00ff88; font-size: 1em; margin-bottom: 0.4em; }}
.meta {{ color: #666; font-size: 0.8em; margin-bottom: 0.8em; }}
video {{ width: 100%; border-radius: 4px; background: #000; }}
</style>
</head>
<body>
<h1>mGBA Test Recordings</h1>
{body}
</body>
</html>"""


def _list_section(label, directory, note):
    """Render one section of the index (final or scratch)."""
    os.makedirs(directory, exist_ok=True)
    files = sorted(
        [f for f in os.listdir(directory) if f.endswith(".mp4")],
        reverse=True,
    )
    header = (f'<h2>{label} — {directory}</h2>'
              f'<div class="section-note">{note}</div>')
    if not files:
        return header + '<p class="empty">(no recordings)</p>'

    cards = []
    for f in files:
        name = f[:-4]  # strip .mp4
        size_mb = os.path.getsize(os.path.join(directory, f)) / 1e6
        cards.append(
            f'<div class="card">'
            f'<div class="name">{name}</div>'
            f'<div class="meta">{size_mb:.1f} MB</div>'
            f'<video controls preload="metadata">'
            f'<source src="/{label}/{f}" type="video/mp4"></video>'
            f"</div>"
        )
    return header + "\n".join(cards)


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        path = urllib.parse.unquote(self.path.split("?")[0])

        if path == "/":
            self._serve_index()
            return

        # Expect /<label>/<filename.mp4>
        parts = path.lstrip("/").split("/", 1)
        if len(parts) == 2 and parts[1].endswith(".mp4") and ".." not in path:
            label, filename = parts
            directory = next(
                (d for (lbl, d, _) in SOURCES if lbl == label), None)
            if directory is not None:
                self._serve_video(os.path.join(directory, filename))
                return

        self.send_error(404)

    def _serve_index(self):
        body = "\n".join(_list_section(lbl, d, note)
                         for (lbl, d, note) in SOURCES)
        html = PAGE.format(body=body)
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.end_headers()
        self.wfile.write(html.encode())

    def _serve_video(self, filepath):
        if not os.path.exists(filepath):
            self.send_error(404)
            return

        size = os.path.getsize(filepath)
        range_header = self.headers.get("Range", "")

        with open(filepath, "rb") as f:
            if range_header.startswith("bytes="):
                # Partial content for seeking on mobile
                parts = range_header[6:].split("-")
                start = int(parts[0])
                end = int(parts[1]) if parts[1] else size - 1
                length = end - start + 1

                self.send_response(206)
                self.send_header("Content-Type", "video/mp4")
                self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
                self.send_header("Content-Length", str(length))
                self.send_header("Accept-Ranges", "bytes")
                self.end_headers()
                f.seek(start)
                self.wfile.write(f.read(length))
            else:
                self.send_response(200)
                self.send_header("Content-Type", "video/mp4")
                self.send_header("Content-Length", str(size))
                self.send_header("Accept-Ranges", "bytes")
                self.end_headers()
                self.wfile.write(f.read())

    def log_message(self, fmt, *args):
        pass  # suppress per-request noise


if __name__ == "__main__":
    for (_, directory, _) in SOURCES:
        os.makedirs(directory, exist_ok=True)
    server = http.server.HTTPServer(("0.0.0.0", PORT), Handler)
    print(f"Serving recordings at http://0.0.0.0:{PORT}")
    for (lbl, d, _) in SOURCES:
        print(f"  [{lbl}] {d}")
    server.serve_forever()
