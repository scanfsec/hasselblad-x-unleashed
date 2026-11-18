"""Interact with M100-GPS and capture its raw data over BLE."""

import argparse
import asyncio
import json
import struct
import time
from contextlib import ExitStack
from pathlib import Path

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError
from nmea import NmeaState
from ubx import PVT, SATELLITES, VERSION, UbxDecoder, describe

SERVICE = "a5720001-0d52-4d11-a825-8b94b63bf10c"
RAW = "a5720002-0d52-4d11-a825-8b94b63bf10c"
QUERY = "a5720003-0d52-4d11-a825-8b94b63bf10c"
DIAGNOSTICS = "a5720004-0d52-4d11-a825-8b94b63bf10c"
DIAGNOSTIC_LAYOUT = struct.Struct("<BBHIIII")


class Stream:
    def __init__(self):
        self.decoder = UbxDecoder()
        self.nmea = NmeaState()
        self.output = None
        self.packets = self.received = self.missing = self.malformed = 0
        self.next_offset = None
        self.pvt = None
        self.replies = {}

    def on_raw(self, _, data):
        if len(data) <= 4:
            self.malformed += 1
            return False, []
        offset, = struct.unpack_from("<I", data)
        if self.next_offset is not None:
            gap = (offset - self.next_offset) & 0xFFFFFFFF
            if gap >= 0x80000000:
                self.malformed += 1
                return False, []
            if gap:
                self.missing += gap
                self.decoder.buffer.clear()
                self.nmea = NmeaState()
                self.pvt = None
        raw = data[4:]
        if self.output:
            self.output.write(raw)
        self.packets += 1
        self.received += len(raw)
        self.next_offset = (offset + len(raw)) & 0xFFFFFFFF
        navigation_frame = self.nmea.feed(raw)
        positions = []
        for key, payload in self.decoder.feed(raw):
            navigation_frame |= key[0] == PVT[0]
            if key == PVT and len(payload) == 92:
                self.pvt = (time.monotonic(), payload)
                positions.append(payload)
            reply = self.replies.get(key)
            if reply is not None and not reply.done():
                summary = describe(key, payload)
                if summary is not None:
                    reply.set_result(summary)
        return navigation_frame, positions

    def report(self):
        return (f"packets={self.packets} bytes={self.received} "
                f"missing={self.missing} malformed={self.malformed}")

    def status(self):
        if self.pvt and time.monotonic() - self.pvt[0] < 2.5:
            print("UBX NAV-PVT", describe(PVT, self.pvt[1]))
        rows = self.nmea.lines()
        if rows:
            print("\n".join(rows))
        elif not self.pvt or time.monotonic() - self.pvt[0] >= 2.5:
            print("No recent GPS navigation data.")
        print(self.report())


def decode_diagnostics(data):
    version, flags, mtu, uptime, received, sent, failed = DIAGNOSTIC_LAYOUT.unpack(data)
    if version != 1 or flags & ~7:
        raise ValueError("Unsupported M100 diagnostics response")
    return {"version": version, "subscribed": bool(flags & 1), "this_peer": bool(flags & 2),
            "overflowed": bool(flags & 4), "mtu": mtu, "uptime_ms": uptime,
            "uart_rx_bytes": received, "notify_accepted": sent, "notify_failed": failed}


async def diagnose(client, stream, disconnected, seconds):
    if client.services.get_characteristic(DIAGNOSTICS) is None:
        print("The connected firmware does not expose BLE diagnostics.")
        return 2
    print(json.dumps({"before": decode_diagnostics(await client.read_gatt_char(DIAGNOSTICS))}), flush=True)
    try:
        await asyncio.wait_for(disconnected.wait(), timeout=seconds)
    except TimeoutError:
        print(json.dumps({"after": decode_diagnostics(await client.read_gatt_char(DIAGNOSTICS))}), flush=True)
    print(stream.report())
    if disconnected.is_set():
        print("BLE disconnected during diagnostics.")
        return 3
    await client.stop_notify(RAW)
    return 0


async def query(client, stream, disconnected, key, selector):
    if client.services.get_characteristic(QUERY) is None:
        print("Query characteristic missing. Check firmware version and try --fresh-gatt.")
        return False
    reply = asyncio.get_running_loop().create_future()
    stream.replies[key] = reply
    connection_lost = asyncio.create_task(disconnected.wait())
    try:
        await client.write_gatt_char(QUERY, bytes((selector,)), response=True)
        await asyncio.wait({reply, connection_lost}, timeout=5, return_when=asyncio.FIRST_COMPLETED)
        if reply.done():
            print(reply.result())
            return True
        if disconnected.is_set():
            print("BLE disconnected while waiting for GPS.")
        else:
            print("No valid GPS reply within 5 seconds. Check GPS RX to GPIO17.")
        return False
    finally:
        stream.replies.pop(key, None)
        reply.cancel()
        connection_lost.cancel()


QUERIES = {"version": (VERSION, 1), "satellites": (SATELLITES, 2)}
HELP = "Commands: status | nmea | version | satellites | watch [seconds] | save <file> | stop | help | quit"


async def interact(client, stream, disconnected):
    print(HELP)
    while not disconnected.is_set():
        try:
            line = (await asyncio.to_thread(input, "m100> ")).strip()
        except EOFError:
            break
        if disconnected.is_set():
            break
        command, _, arg = line.partition(" ")
        arg = arg.strip()
        if command in ("quit", "exit"):
            break
        if command == "help":
            print(HELP)
        elif command == "status":
            stream.status()
        elif command == "nmea":
            print("\n".join(stream.nmea.lines(include_satellites=True)) or "No recent NMEA data.")
        elif command in QUERIES:
            try:
                await query(client, stream, disconnected, *QUERIES[command])
            except (BleakError, TimeoutError) as exc:
                print(str(exc) or type(exc).__name__)
        elif command == "watch":
            try:
                seconds = int(arg or "10")
                if not 1 <= seconds <= 3600:
                    raise ValueError
            except ValueError:
                print("Use watch [1-3600].")
                continue
            for _ in range(seconds):
                if disconnected.is_set():
                    break
                stream.status()
                try:
                    await asyncio.wait_for(disconnected.wait(), timeout=1)
                except TimeoutError:
                    pass
        elif command == "save" and arg:
            if stream.output:
                print("Stop the active capture before starting another.")
                continue
            try:
                stream.output = Path(arg.strip('"')).open("xb")
                print(f"Saving raw bytes to {stream.output.name}")
            except OSError as exc:
                print(exc)
        elif command == "stop":
            if stream.output:
                stream.output.close()
                stream.output = None
            print(stream.report())
        elif command:
            print(HELP)


async def capture(args):
    device = await BleakScanner.find_device_by_filter(
        lambda _, adv: SERVICE in adv.service_uuids, timeout=args.scan_timeout
    )
    if device is None:
        print("M100-GPS service not found. Check that the ESP32 runs the BLE sketch.")
        return 2
    print(f"Found {device.name or 'M100-GPS'} ({device.address})")
    if args.advertisement_only:
        return 0
    disconnected = asyncio.Event()
    options = {"winrt": {"use_cached_services": False}} if args.fresh_gatt else {}
    async with BleakClient(device, disconnected_callback=lambda _: disconnected.set(), **options) as client:
        if client.services.get_characteristic(RAW) is None:
            print("Raw stream characteristic missing.")
            return 2
        if args.scan_only:
            print("Raw stream characteristic present.")
            return 0
        stream = Stream()
        await client.start_notify(RAW, stream.on_raw)
        if args.diagnostics:
            return await diagnose(client, stream, disconnected, args.seconds)
        with ExitStack() as files:
            files.callback(lambda: stream.output.close() if stream.output else None)
            if args.interactive:
                await interact(client, stream, disconnected)
                if client.is_connected:
                    await client.stop_notify(RAW)
                return 3 if disconnected.is_set() else 0
            if args.query:
                ok = True
                for name in args.query:
                    ok = await query(client, stream, disconnected, *QUERIES[name]) and ok
                if client.is_connected:
                    await client.stop_notify(RAW)
                return 0 if ok else 3
            output = args.output or Path("m100_uart.bin")
            stream.output = output.open("xb")
            baseline = stream.packets, stream.received, stream.missing, stream.malformed
            try:
                await asyncio.wait_for(disconnected.wait(), timeout=args.seconds)
            except TimeoutError:
                await client.stop_notify(RAW)
            packets, received, missing, malformed = (
                current - before for current, before in zip(
                    (stream.packets, stream.received, stream.missing, stream.malformed), baseline)
            )
            print(f"packets={packets} bytes={received} missing={missing} malformed={malformed}")
            print(f"Saved UART bytes to {output}")
            if disconnected.is_set():
                print("BLE disconnected before capture finished.")
            return 0 if received and not (missing or malformed or disconnected.is_set()) else 3


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scan-timeout", type=float, default=10)
    parser.add_argument("--seconds", type=float, default=20)
    parser.add_argument("--output", type=Path, help="Raw capture path (default: m100_uart.bin)")
    parser.add_argument("--fresh-gatt", action="store_true", help="Refresh GATT services on Windows")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--advertisement-only", action="store_true", help="Find the BLE advertisement without opening GATT")
    mode.add_argument("--scan-only", action="store_true", help="Discover and check the GATT characteristic")
    mode.add_argument("--interactive", "-i", action="store_true", help="Open the BLE command prompt")
    mode.add_argument("--query", choices=QUERIES, action="append", help="Query GPS over BLE and exit")
    mode.add_argument("--diagnostics", action="store_true", help="Read module counters over BLE before and after sampling")
    args = parser.parse_args()
    if args.seconds <= 0 or args.scan_timeout <= 0:
        parser.error("timeouts must be positive")
    try:
        return asyncio.run(capture(args))
    except (BleakError, OSError, TimeoutError, ValueError, struct.error) as exc:
        print(str(exc) or type(exc).__name__)
        return 2
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
