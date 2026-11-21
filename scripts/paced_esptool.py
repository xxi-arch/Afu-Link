"""Give the CH343 adapter time to drain each serial write."""

import sys
import time

import esptool
import serial


def main():
    original_write = serial.Serial.write

    def paced_write(port, data):
        payload = bytes(data)
        sent = 0
        while sent < len(payload):
            count = original_write(port, payload[sent:sent + 64])
            if not count:
                raise serial.SerialTimeoutException("Serial write made no progress")
            sent += count
            time.sleep(count * 10.0 / port.baudrate + 0.002)
        return sent

    serial.Serial.write = paced_write
    try:
        sys.argv[0] = "esptool"
        esptool._main()
    finally:
        serial.Serial.write = original_write


if __name__ == "__main__":
    main()
