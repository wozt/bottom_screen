#!/usr/bin/env python3
"""Checks the browser's half of the protocol without a browser.

WebCodecs needs a real browser, but everything under it -- the upgrade,
the framing, the greeting and the input path -- is plain bytes, and
those are worth checking somewhere that fails with a message rather than
a blank canvas.
"""
import asyncio
import struct
import sys

import websockets

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 5800
WANT_FRAMES = int(sys.argv[2]) if len(sys.argv) > 2 else 60

MSG_VIDEO, MSG_AUDIO, MSG_STREAM_INFO = 1, 2, 3
MSG_INPUT, MSG_PING, MSG_PONG, MSG_REQUEST_KEYFRAME = 16, 17, 18, 19


def message(kind, body=b""):
    return struct.pack("<B3xI", kind, len(body)) + body


async def main():
    url = f"ws://127.0.0.1:{PORT}/ws"
    async with websockets.connect(url, max_size=4 * 1024 * 1024) as ws:
        greeting = await asyncio.wait_for(ws.recv(), timeout=5)
        if len(greeting) < 20:
            print("FAIL: greeting too short")
            return 1

        magic, version, accepted, console, codec, w, h, fps, extra = \
            struct.unpack_from("<IBBBBHHHH", greeting, 0)
        if magic != 0x31435342:
            print(f"FAIL: bad magic {magic:#x}")
            return 1
        if not accepted:
            print("FAIL: refused")
            return 1
        print(f"greeting ok: console={console} {w}x{h} @{fps} "
              f"extradata={extra} ({len(greeting)} bytes)")
        if len(greeting) != 20 + extra:
            print("FAIL: greeting length does not match its extradata field")
            return 1

        await ws.send(message(MSG_REQUEST_KEYFRAME))

        video = audio = keyframes = 0
        first_is_key = None
        while video < WANT_FRAMES:
            raw = await asyncio.wait_for(ws.recv(), timeout=10)
            kind, size = struct.unpack_from("<B3xI", raw, 0)
            if size != len(raw) - 8:
                print("FAIL: framed length disagrees with the frame")
                return 1
            body = raw[8:]

            if kind == MSG_VIDEO:
                flags = struct.unpack_from("<H", body, 12)[0]
                key = bool(flags & 0x01)
                if first_is_key is None:
                    first_is_key = key
                keyframes += key
                video += 1
                if video == 5:
                    # The same event the page sends on a tap, so the input
                    # path is exercised rather than assumed.
                    ev = struct.pack("<IIBBhhH", 0, 0, 1, 0, w // 2, h // 2, 0)
                    await ws.send(message(MSG_INPUT, ev))
            elif kind == MSG_AUDIO:
                audio += 1
            elif kind == MSG_PING:
                await ws.send(message(MSG_PONG))

        print(f"{video} video, {audio} audio, {keyframes} keyframes")
        if not first_is_key:
            print("FAIL: the first frame was not a keyframe, so nothing "
                  "would decode")
            return 1
        print("PASS")
        return 0


sys.exit(asyncio.run(main()))
