from __future__ import annotations

import argparse
import signal
import time

from .providers import get_provider
from .providers.base import SensorCandidate
from .serial_io import open_serial_writer


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Read the host CPU temperature and send it to the ESP32-S3 over serial."
    )
    parser.add_argument(
        "--platform",
        default="auto",
        choices=("auto", "linux", "windows", "macos"),
        help="Host platform sensor provider to use",
    )
    parser.add_argument("--port", default="/dev/ttyACM0", help="Serial port connected to the ESP32-S3")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate")
    parser.add_argument("--interval", type=float, default=1.0, help="Seconds between temperature updates")
    parser.add_argument(
        "--sensor",
        help="Optional provider-specific sensor identifier, such as /sys/class/hwmon/hwmon0/temp1_input or cpu_temp_avg",
    )
    parser.add_argument(
        "--librehardwaremonitor-dll",
        help="Windows only: path to LibreHardwareMonitorLib.dll. You can also set LIBREHARDWAREMONITOR_DLL.",
    )
    parser.add_argument("--list-sensors", action="store_true", help="Print detected temperature sensors and exit")
    parser.add_argument("--dry-run", action="store_true", help="Print the detected temperature without opening the serial port")
    return parser.parse_args()


def print_candidates(candidates: list[SensorCandidate]) -> int:
    if not candidates:
        print("No temperature sensors were found.")
        return 1

    print("Available temperature sensors:")
    for candidate in sorted(candidates, reverse=True):
        print(
            f"  priority={candidate.priority:>4}  {candidate.source:<18}  "
            f"{candidate.label:<20}  {candidate.identifier}"
        )

    return 0


def main() -> int:
    args = parse_args()
    provider = get_provider(args.platform, lhm_dll_path=args.librehardwaremonitor_dll)

    print(f"Using provider: {provider.platform_name}")
    try:
        if args.list_sensors:
            return print_candidates(provider.list_candidates())

        sensor = provider.choose_sensor(args.sensor)
        print(f"Using sensor: {sensor.source} / {sensor.label} ({sensor.identifier})")

        should_stop = False

        def handle_signal(_signum: int, _frame: object) -> None:
            nonlocal should_stop
            should_stop = True

        signal.signal(signal.SIGINT, handle_signal)
        signal.signal(signal.SIGTERM, handle_signal)

        if args.dry_run:
            while not should_stop:
                temp_c = provider.read_celsius(sensor)
                print(f"{temp_c:.2f}")
                time.sleep(args.interval)

            print("Stopped temperature sender")
            return 0

        with open_serial_writer(args.port, args.baud) as writer:
            print(f"Opened serial port: {args.port} @ {args.baud} baud")
            time.sleep(2.0)

            while not should_stop:
                temp_c = provider.read_celsius(sensor)
                payload = f"{temp_c:.2f}"
                writer.write_line(payload)
                print(f"Sent {payload} C")
                time.sleep(args.interval)

        print("Stopped temperature sender")
        return 0
    finally:
        provider.close()
