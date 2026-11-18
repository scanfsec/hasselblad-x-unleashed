"""Incremental UBX decoder for the M100's mixed UBX/NMEA output."""

import struct


class UbxDecoder:
    def __init__(self):
        self.buffer = bytearray()

    def feed(self, data):
        self.buffer.extend(data)
        while self.buffer:
            start = self.buffer.find(b"\xb5\x62")
            if start < 0:
                self.buffer[:] = self.buffer[-1:] if self.buffer[-1] == 0xB5 else b""
                return
            del self.buffer[:start]
            if len(self.buffer) < 6:
                return
            length, = struct.unpack_from("<H", self.buffer, 4)
            if length > 4096:
                del self.buffer[0]
                continue
            end = length + 8
            if len(self.buffer) < end:
                return
            a = b = 0
            for value in self.buffer[2:end - 2]:
                a = (a + value) & 255
                b = (b + a) & 255
            if self.buffer[end - 2:end] != bytes((a, b)):
                del self.buffer[0]
                continue
            key = tuple(self.buffer[2:4])
            payload = bytes(self.buffer[6:end - 2])
            del self.buffer[:end]
            yield key, payload


PVT = (0x01, 0x07)
VERSION = (0x0A, 0x04)
SATELLITES = (0x01, 0x35)
SYSTEMS = {0: "GPS", 1: "SBAS", 2: "Galileo", 3: "BeiDou", 5: "QZSS", 6: "GLONASS"}
QUALITY = ("none", "searching", "acquired", "unusable", "code lock",
           "carrier lock", "carrier lock", "carrier lock")


def describe(key, payload):
    if key == VERSION and len(payload) >= 40 and (len(payload) - 40) % 30 == 0:
        def field(raw):
            return raw.split(b"\0", 1)[0].decode("ascii", "replace")

        rows = [f"SW: {field(payload[:30])}", f"HW: {field(payload[30:40])}"]
        rows.extend(field(payload[i:i + 30]) for i in range(40, len(payload), 30))
        return "\n".join(rows)

    if key == SATELLITES and len(payload) >= 8 and payload[4] == 1:
        count = payload[5]
        if len(payload) != 8 + count * 12:
            return None
        rows = [f"Satellites reported: {count}",
                "System     SV dB-Hz  Elev  Azim  Used  Tracking"]
        for i in range(8, len(payload), 12):
            system, sv, cno, elev, azim, _, flags = struct.unpack_from("<BBBbhhI", payload, i)
            elevation = str(elev) if -90 <= elev <= 90 else "--"
            azimuth = str(azim) if elevation != "--" and 0 <= azim <= 360 else "--"
            rows.append(f"{SYSTEMS.get(system, str(system)):9} {sv:3} {cno:5} "
                        f"{elevation:>5} {azimuth:>5} {'yes' if flags & 8 else 'no':>5}  "
                        f"{QUALITY[flags & 7]}")
        return "\n".join(rows)

    if key == PVT and len(payload) == 92:
        year, month, day, hour, minute, second, valid = struct.unpack_from("<H6B", payload, 4)
        fix_type, flags, _, satellites = payload[20:24]
        valid_fix = bool(flags & 1 and fix_type in (2, 3, 4) and not payload[78] & 1)
        utc = (f"{year:04}-{month:02}-{day:02} {hour:02}:{minute:02}:{second:02} UTC"
               if valid & 3 == 3 else "UTC unavailable")
        if valid & 3 == 3:
            accuracy, nano = struct.unpack_from("<Ii", payload, 12)
            resolved = "yes" if valid & 4 else "no"
            estimate = "unknown" if accuracy == 0xFFFFFFFF else f"{accuracy}ns"
            utc += f" nano={nano:+d}ns tAcc={estimate} fullyResolved={resolved}"
        result = f"{utc} | fix={int(valid_fix)} type={fix_type} used={satellites}"
        if valid_fix:
            lon, lat, _, msl = struct.unpack_from("<4i", payload, 24)
            h_acc, v_acc = struct.unpack_from("<2I", payload, 40)
            speed, = struct.unpack_from("<i", payload, 60)
            result += f" lat={lat * 1e-7:.7f} lon={lon * 1e-7:.7f}"
            result += f" alt_msl={msl / 1000:.1f}m speed={speed / 1000:.2f}m/s"
            result += f" hAcc={h_acc / 1000:.3f}m vAcc={v_acc / 1000:.3f}m"
        return result
    return None
