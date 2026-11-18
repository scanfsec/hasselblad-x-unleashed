"""Parse checksum-validated NMEA from the M100's BLE byte stream."""

import re
import time
from functools import reduce
from operator import xor

SENTENCE = re.compile(rb"\$([A-Z]{5},[ -~]*)\*([0-9A-Fa-f]{2})\r?\n")
SYSTEMS = {"GP": "GPS", "GA": "Galileo", "GB": "BeiDou", "BD": "BeiDou",
           "GL": "GLONASS", "GQ": "QZSS", "GN": "Mixed"}
FRESH_SECONDS = 2.5
NAVIGATION_SENTENCES = {"RMC", "VTG", "GGA", "GSA", "GSV", "GLL", "GNS", "ZDA"}


def coordinate(value, hemisphere, degree_digits):
    positive, negative = "NS" if degree_digits == 2 else "EW"
    if hemisphere not in (positive, negative):
        raise ValueError("invalid hemisphere")
    maximum = 90 if degree_digits == 2 else 180
    degrees = int(value[:degree_digits])
    minutes = float(value[degree_digits:])
    if not (0 <= degrees <= maximum and 0 <= minutes < 60 and
            (degrees != maximum or minutes == 0)):
        raise ValueError("invalid coordinate")
    return (-1 if hemisphere == negative else 1) * (degrees + minutes / 60)


def position(fields, index):
    lat = coordinate(fields[index], fields[index + 1], 2)
    lon = coordinate(fields[index + 2], fields[index + 3], 3)
    return f"lat={lat:.7f} lon={lon:.7f}"


class NmeaState:
    def __init__(self):
        self.buffer = bytearray()
        self.navigation = {}
        self.satellite_pages = {}
        self.satellites = {}

    def feed(self, data):
        navigation_frame = False
        for byte in data:
            if byte == ord("$"):
                self.buffer[:] = b"$"
            elif self.buffer:
                self.buffer.append(byte)
                if byte == 10:
                    match = SENTENCE.fullmatch(bytes(self.buffer))
                    self.buffer.clear()
                    if match and reduce(xor, match[1], 0) == int(match[2], 16):
                        fields = match[1].decode("ascii").split(",")
                        self.update(fields)
                        name = fields[0]
                        navigation_frame |= name[:2] in SYSTEMS and name[2:] in NAVIGATION_SENTENCES
                elif len(self.buffer) >= 128 or (byte < 32 and byte != 13) or byte > 126:
                    self.buffer.clear()
        return navigation_frame

    def update(self, fields):
        name = fields[0]
        kind = name[2:]
        now = time.monotonic()
        try:
            if kind == "RMC":
                valid = fields[2] == "A"
                row = f"{name} utc={fields[1] or '--'} date={fields[9] or '--'} fix={int(valid)}"
                if valid:
                    row += " " + position(fields, 3)
                    if fields[7]:
                        row += f" speed={float(fields[7]) * 0.514444:.2f}m/s"
                self.navigation[kind] = (now, row)
            elif kind == "GGA":
                quality, used = int(fields[6]), int(fields[7])
                row = f"{name} utc={fields[1] or '--'} quality={quality} used={used}"
                if quality > 0:
                    row += " " + position(fields, 2)
                    if fields[9] and fields[10] == "M":
                        row += f" alt_msl={float(fields[9]):.1f}m"
                self.navigation[kind] = (now, row)
            elif kind == "GSV":
                total, page, visible = map(int, fields[1:4])
                payload = fields[4:]
                groups, remainder = divmod(len(payload), 4)
                if remainder > 1 or not 1 <= page <= total <= 9:
                    return
                signal = payload[-1] if remainder else ""
                key = name[:2], signal
                if page == 1:
                    self.satellite_pages[key] = (now, total, 1, [])
                started, expected_total, expected_page, rows = self.satellite_pages[key]
                if (total, page) != (expected_total, expected_page) or now - started >= FRESH_SECONDS:
                    return
                for i in range(0, 4 * groups, 4):
                    sv, elev, azim, cno = payload[i:i + 4]
                    if sv:
                        rows.append(f"  SV {sv}  C/N0={cno or '--'} dB-Hz"
                                    f"  elev={elev or '--'}  azim={azim or '--'}")
                if page == total:
                    del self.satellite_pages[key]
                    system = SYSTEMS.get(name[:2], name[:2])
                    label = f"{system} signal={signal or '--'} reported={visible}"
                    self.satellites[key] = (now, "\n".join([label, *rows]))
                else:
                    self.satellite_pages[key] = (started, total, page + 1, rows)
        except (IndexError, KeyError, ValueError):
            # A checksum can be correct even if a sentence has missing fields.
            return

    def lines(self, include_satellites=False):
        now = time.monotonic()
        records = list(self.navigation.values())
        if include_satellites:
            records.extend(self.satellites.values())
        return [line for timestamp, line in records if now - timestamp < FRESH_SECONDS]
