#!/usr/bin/env python3
"""
ReptiMon Dev Server — hot-reload development proxy
====================================================
Serves data/ as static files and proxies all /api/* HTTP requests
and /ws* WebSocket connections to a live ESP32 device.

Edit HTML / CSS / JS in data/ → save → browser refresh. No flashing needed.

Usage:
    python scripts/dev_server.py --esp <ESP32-IP>
    python scripts/dev_server.py --esp 192.168.1.113 --port 5500

Install dependency once:
    pip install aiohttp
"""

import argparse
import asyncio
import mimetypes
import sys
from pathlib import Path

try:
    from aiohttp import web, ClientSession, WSMsgType, ClientConnectorError
except ImportError:
    print("\n  Missing dependency. Install it with:\n    pip install aiohttp\n")
    sys.exit(1)

DATA_DIR = Path(__file__).parent.parent / "data"

# ── Colour helpers ────────────────────────────────────────────────────────────
R = "\033[91m"; G = "\033[92m"; Y = "\033[93m"; C = "\033[96m"; W = "\033[0m"

def log(tag, colour, msg):
    print(f"  {colour}[{tag}]{W} {msg}")

# ── WebSocket proxy ───────────────────────────────────────────────────────────
async def proxy_ws(request, esp_host):
    path = request.path
    esp_uri = f"ws://{esp_host}{path}"
    log("WS ", C, f"{path}  →  {esp_uri}")

    ws_client = web.WebSocketResponse()
    await ws_client.prepare(request)

    try:
        async with ClientSession() as session:
            async with session.ws_connect(esp_uri) as ws_esp:

                async def client_to_esp():
                    async for msg in ws_client:
                        if msg.type == WSMsgType.TEXT:
                            await ws_esp.send_str(msg.data)
                        elif msg.type == WSMsgType.BINARY:
                            await ws_esp.send_bytes(msg.data)
                        elif msg.type in (WSMsgType.CLOSE, WSMsgType.ERROR):
                            break

                async def esp_to_client():
                    async for msg in ws_esp:
                        if msg.type == WSMsgType.TEXT:
                            await ws_client.send_str(msg.data)
                        elif msg.type == WSMsgType.BINARY:
                            await ws_client.send_bytes(msg.data)
                        elif msg.type in (WSMsgType.CLOSE, WSMsgType.ERROR):
                            break

                await asyncio.gather(client_to_esp(), esp_to_client())

    except ClientConnectorError:
        log("WS ", R, f"Cannot reach ESP32 at {esp_uri}")
    except Exception as e:
        log("WS ", R, str(e))

    return ws_client

# ── HTTP API proxy ────────────────────────────────────────────────────────────
async def proxy_http(request, esp_host):
    url = f"http://{esp_host}{request.path_qs}"
    skip = {"host", "transfer-encoding"}
    headers = {k: v for k, v in request.headers.items() if k.lower() not in skip}
    body = await request.read()

    try:
        async with ClientSession() as session:
            async with session.request(
                request.method, url, data=body or None, headers=headers
            ) as resp:
                out_headers = {
                    k: v for k, v in resp.headers.items()
                    if k.lower() not in ("transfer-encoding", "content-encoding")
                }
                out_headers["Access-Control-Allow-Origin"] = "*"
                body_out = await resp.read()
                code = resp.status
    except ClientConnectorError:
        log("API", R, f"ESP32 unreachable — {url}")
        return web.Response(status=502, text="ESP32 unreachable")
    except Exception as e:
        return web.Response(status=500, text=str(e))

    status_colour = G if 200 <= code < 300 else Y if code < 400 else R
    log("API", status_colour, f"{request.method} {request.path_qs}  {status_colour}{code}{W}")
    return web.Response(body=body_out, status=code, headers=out_headers)

# ── Static file handler ───────────────────────────────────────────────────────
async def handle_static(request):
    rel = request.path.lstrip("/") or "index.html"
    path = DATA_DIR / rel

    # Directory → index.html
    if path.is_dir():
        path = path / "index.html"

    if not path.exists():
        raise web.HTTPNotFound()

    ct, _ = mimetypes.guess_type(str(path))
    ct = ct or "application/octet-stream"

    # Add charset for text types to avoid browser encoding warnings
    if ct.startswith("text/") and "charset" not in ct:
        ct += "; charset=utf-8"

    return web.FileResponse(path, headers={"Content-Type": ct})

# ── Master router ─────────────────────────────────────────────────────────────
async def router(request, esp_host):
    p = request.path
    if p.startswith("/ws"):
        return await proxy_ws(request, esp_host)
    if p.startswith("/api/"):
        return await proxy_http(request, esp_host)
    return await handle_static(request)

# ── Entry point ───────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(description="ReptiMon hot-reload dev server")
    parser.add_argument("--esp",  default="192.168.4.1", metavar="IP",
                        help="ESP32 IP address (default: 192.168.4.1)")
    parser.add_argument("--port", default=5500, type=int,
                        help="Local port to listen on (default: 5500)")
    args = parser.parse_args()

    app = web.Application()
    app.router.add_route("*", "/{path_info:.*}", lambda r: router(r, args.esp))

    print(f"\n  {G}ReptiMon Dev Server{W}")
    print(f"  Local  →  {C}http://localhost:{args.port}{W}")
    print(f"  ESP32  →  {C}http://{args.esp}{W}")
    print(f"\n  {Y}Edit files in data/ then just refresh the browser.{W}")
    print(f"  Press Ctrl+C to stop.\n")

    web.run_app(app, port=args.port, print=None, access_log=None)

if __name__ == "__main__":
    main()
