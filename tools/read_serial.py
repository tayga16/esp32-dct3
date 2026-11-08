"""Read a bounded console capture without toggling the board's reset lines."""
import argparse
from pathlib import Path
import time
import sys

import serial


def main():
    sys.stdout.reconfigure(errors="replace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port")
    parser.add_argument("--seconds", type=float, default=15)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--send", help="Send a console command before capture")
    parser.add_argument("--reset-usb", action="store_true",
                        help="Reset ESP32 through native USB before capture")
    parser.add_argument("--reset-uart", action="store_true",
                        help="Reset ESP32 through the UART bridge before capture")
    args = parser.parse_args()
    connection = serial.Serial()
    connection.port = args.port
    connection.baudrate = 115200
    connection.timeout = 0.2
    connection.dtr = False
    connection.rts = False
    connection.open()
    data = bytearray()
    try:
        if args.reset_usb:
            from esptool.reset import HardReset
            HardReset(connection, uses_usb=True)()
        if args.reset_uart:
            from esptool.reset import HardReset
            HardReset(connection, uses_usb=False)()
        if args.send:
            connection.write(args.send.encode("ascii"))
            connection.flush()
        end = time.monotonic() + args.seconds
        while time.monotonic() < end:
            data.extend(connection.read(4096))
    finally:
        connection.close()
    if args.output:
        args.output.write_bytes(data)
    print(data.decode("utf-8", errors="replace"))


if __name__ == "__main__":
    main()
