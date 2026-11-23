"""Serial transport for the firmware's existing RT-Thread console."""

import re
from time import monotonic

import serial
from serial.tools import list_ports


def find_device(port, usb_serial, usb_location):
    matches = [device for device in list_ports.comports()
               if (not port or device.device.casefold() == port.casefold())
               and (not usb_serial or device.serial_number == usb_serial)
               and (not usb_location or device.location == usb_location)]
    if len(matches) != 1:
        names = [device.device for device in matches]
        raise ValueError(f"Expected exactly one matching serial device, found {names}")
    return matches[0]


class Console:
    def __init__(self, port, baud, log):
        self.port = serial.Serial(baudrate=baud, timeout=0.05, write_timeout=1)
        self.port.port = port
        self.port.dtr = False
        self.port.rts = False
        self.log = log

    def __enter__(self):
        self.port.open()
        return self

    def __exit__(self, *exc):
        self.port.close()

    def _read(self):
        data = self.port.read(self.port.in_waiting or 1)
        if data:
            self.log.write(data)
            self.log.flush()
        return data

    def command(self, command, expected, timeout=3, settle=0):
        pending = self.port.in_waiting
        if pending:
            self.log.write(self.port.read(pending))
        self.log.write(f"\n>>> {command}\n".encode("utf-8"))
        self.log.flush()
        self.port.write(command.encode("ascii") + b"\r")
        self.port.flush()
        deadline = monotonic() + timeout
        reply = bytearray()
        last_data = monotonic()
        while monotonic() < deadline:
            data = self._read()
            if data:
                reply.extend(data)
                last_data = monotonic()
            decoded = reply.decode("utf-8", errors="replace")
            if re.search(expected, decoded) and monotonic() - last_data >= settle:
                return decoded
        raise TimeoutError(f"{command!r} timed out waiting for {expected!r}")
