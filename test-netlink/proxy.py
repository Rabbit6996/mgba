#!/usr/bin/env python3
"""TCP relay that adds a fixed one-way delay (and optional jitter) in both directions."""
import asyncio, random, sys, time

listen_port, target_port, delay_ms = int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3])
jitter_ms = float(sys.argv[4]) if len(sys.argv) > 4 else 0.0

async def pipe(reader, writer):
    queue = asyncio.Queue()
    async def rx():
        while True:
            data = await reader.read(65536)
            queue.put_nowait((time.monotonic(), data))
            if not data:
                return
    async def tx():
        last = 0.0
        while True:
            ts, data = await queue.get()
            due = max(ts + (delay_ms + random.uniform(0, jitter_ms)) / 1000.0, last)
            last = due
            wait = due - time.monotonic()
            if wait > 0:
                await asyncio.sleep(wait)
            if not data:
                writer.close()
                return
            writer.write(data)
            await writer.drain()
    await asyncio.gather(rx(), tx())

async def handle(reader, writer):
    r2, w2 = await asyncio.open_connection('127.0.0.1', target_port)
    for sock in (writer.get_extra_info('socket'), w2.get_extra_info('socket')):
        import socket
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    try:
        await asyncio.gather(pipe(reader, w2), pipe(r2, writer))
    except Exception:
        pass

async def main():
    server = await asyncio.start_server(handle, '127.0.0.1', listen_port)
    async with server:
        await server.serve_forever()

asyncio.run(main())
