import struct
import unittest
from types import SimpleNamespace
from unittest.mock import AsyncMock, patch

from pc_capture import Stream, capture, decode_diagnostics


def notification(offset, data):
    return struct.pack("<I", offset) + data


def ubx_frame(message, payload):
    body = bytes(message) + struct.pack("<H", len(payload)) + payload
    a = b = 0
    for byte in body:
        a = (a + byte) & 255
        b = (b + a) & 255
    return b"\xb5\x62" + body + bytes((a, b))


class StreamTest(unittest.TestCase):
    def test_ble_diagnostics_counters(self):
        data = struct.pack("<BBHIIII", 1, 7, 247, 27000, 72000, 300, 6)
        self.assertEqual(decode_diagnostics(data), {"version": 1, "subscribed": True, "this_peer": True,
                         "overflowed": True, "mtu": 247, "uptime_ms": 27000, "uart_rx_bytes": 72000,
                         "notify_accepted": 300, "notify_failed": 6})
        with self.assertRaises(ValueError):
            decode_diagnostics(b"\x02" + data[1:])

    def test_presence_requires_complete_checksum_valid_navigation_frame(self):
        stream = Stream()
        # RMC explicitly indicates no fix, but a receiver is producing data.
        nmea = b"$GNRMC,,V,,,,,,,,,,N*4D\r\n"
        head, tail = nmea[:8], nmea[8:]
        self.assertEqual(stream.on_raw(None, notification(0, head)), (False, []))
        self.assertEqual(stream.on_raw(None, notification(len(head), tail)), (True, []))
        self.assertEqual(stream.on_raw(None, notification(len(nmea), nmea.replace(b"4D", b"00"))),
                         (False, []))

    def test_nav_pvt_and_nav_sat_count_version_reply_does_not(self):
        stream = Stream()
        reply = ubx_frame((0x0A, 0x04), b"\0" * 40)
        sat = ubx_frame((1, 0x35), b"\0" * 8)
        pvt = b"\0" * 92
        self.assertEqual(stream.on_raw(None, notification(0, reply)), (False, []))
        self.assertEqual(stream.on_raw(None, notification(len(reply), sat)), (True, []))
        self.assertEqual(stream.on_raw(None, notification(len(reply) + len(sat), ubx_frame((1, 7), pvt))),
                         (True, [pvt]))

    def test_gap_cannot_complete_a_frame(self):
        stream = Stream()
        pvt = ubx_frame((1, 7), b"\0" * 92)
        self.assertEqual(stream.on_raw(None, notification(0, pvt[:16])), (False, []))
        self.assertEqual(stream.on_raw(None, notification(17, pvt[16:])), (False, []))
        self.assertEqual(stream.missing, 1)


class DiscoveryTest(unittest.IsolatedAsyncioTestCase):
    async def test_advertisement_discovery_leaves_gatt_disconnected(self):
        peer = SimpleNamespace(name="M100-GPS", address="14:C1:9F:2E:BB:A9")
        args = SimpleNamespace(scan_timeout=3, advertisement_only=True)
        with (patch("pc_capture.BleakScanner.find_device_by_filter", new=AsyncMock(return_value=peer)) as scan,
              patch("pc_capture.BleakClient") as connect):
            self.assertEqual(await capture(args), 0)
        scan.assert_awaited_once()
        connect.assert_not_called()
