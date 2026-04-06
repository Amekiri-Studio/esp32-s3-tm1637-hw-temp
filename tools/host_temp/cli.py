from __future__ import annotations

import argparse
import signal
import time

from .network_io import open_udp_sender
from .providers import get_provider
from .providers.base import SensorCandidate
from .serial_io import ReadableLinePort, auto_detect_serial_port, list_serial_ports, open_serial_reader_writer, open_serial_sender


DEFAULT_WIFI_PORT = 4210
SERIAL_OPEN_DELAY_SECONDS = 2.0
WIFI_COMMAND_DELAY_SECONDS = 0.2
SERIAL_RESPONSE_POLL_SECONDS = 0.05
SERIAL_RESPONSE_IDLE_SECONDS = 0.75
DEFAULT_COMMAND_RESPONSE_TIMEOUT_SECONDS = 1.5
STATUS_RESPONSE_TIMEOUT_SECONDS = 2.5
CONNECT_RESPONSE_TIMEOUT_SECONDS = 12.0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Read the host CPU temperature and send it to the ESP32-S3 over serial or WiFi."
    )
    parser.add_argument(
        "--transport",
        default="serial",
        choices=("serial", "wifi"),
        help="Transport used to send the temperature to the ESP32-S3",
    )
    parser.add_argument(
        "--platform",
        default="auto",
        choices=("auto", "linux", "windows", "macos"),
        help="Host platform sensor provider to use",
    )
    parser.add_argument("--port", help="Serial port connected to the ESP32-S3. Defaults to auto-detection.")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate for --transport serial")
    parser.add_argument("--wifi-host", help="ESP32-S3 IP address or hostname for --transport wifi")
    parser.add_argument(
        "--wifi-port",
        type=int,
        default=DEFAULT_WIFI_PORT,
        help=f"UDP port used by the ESP32-S3 for --transport wifi (default: {DEFAULT_WIFI_PORT})",
    )
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
    parser.add_argument("--list-ports", action="store_true", help="Print detected serial ports and exit")
    parser.add_argument("--dry-run", action="store_true", help="Print the detected temperature without opening the serial port")
    parser.add_argument("--wifi-ssid", help="Configure the ESP32-S3 WiFi SSID over serial")
    parser.add_argument("--wifi-pass", help="Configure the ESP32-S3 WiFi password over serial")
    parser.add_argument("--wifi-connect", action="store_true", help="Send `WIFI CONNECT` over serial and exit")
    parser.add_argument("--wifi-status", action="store_true", help="Send `WIFI STATUS` over serial and exit")
    parser.add_argument("--wifi-clear", action="store_true", help="Send `WIFI CLEAR` over serial and exit")
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


def print_ports() -> int:
    ports = list_serial_ports()
    if not ports:
        print("No serial ports were found.")
        return 1

    print("Available serial ports:")
    for port in ports:
        details = [port.description]
        if port.manufacturer:
            details.append(port.manufacturer)
        if port.hwid:
            details.append(port.hwid)

        print(
            f"  score={port.score:>3}  {port.device:<20}  "
            f"[{port.detection_method}] {' | '.join(details)}"
        )

    return 0


def has_wifi_serial_action(args: argparse.Namespace) -> bool:
    return any((args.wifi_ssid, args.wifi_pass, args.wifi_connect, args.wifi_status, args.wifi_clear))


def build_wifi_serial_commands(args: argparse.Namespace) -> list[str]:
    commands: list[str] = []
    if args.wifi_clear:
        commands.append("WIFI CLEAR")
    if args.wifi_ssid:
        commands.append(f"WIFI SSID {args.wifi_ssid}")
    if args.wifi_pass:
        commands.append(f"WIFI PASS {args.wifi_pass}")
    if args.wifi_connect:
        commands.append("WIFI CONNECT")
    if args.wifi_status:
        commands.append("WIFI STATUS")
    return commands


def resolve_serial_port(args: argparse.Namespace) -> str:
    port = args.port or auto_detect_serial_port()
    if args.port:
        print(f"Using serial port: {port}")
    else:
        print(f"Auto-detected serial port: {port}")
    return port


def response_timeout_for_command(command: str) -> float:
    if command == "WIFI CONNECT":
        return CONNECT_RESPONSE_TIMEOUT_SECONDS
    if command == "WIFI STATUS":
        return STATUS_RESPONSE_TIMEOUT_SECONDS
    return DEFAULT_COMMAND_RESPONSE_TIMEOUT_SECONDS


def print_serial_response(port: ReadableLinePort, timeout_seconds: float) -> bool:
    deadline = time.monotonic() + timeout_seconds
    last_data_at: float | None = None
    saw_output = False

    while time.monotonic() < deadline:
        chunk = port.read_available_text()
        if chunk:
            print(chunk, end="")
            saw_output = True
            last_data_at = time.monotonic()
            continue

        if saw_output and last_data_at is not None and time.monotonic() - last_data_at >= SERIAL_RESPONSE_IDLE_SECONDS:
            break

        time.sleep(SERIAL_RESPONSE_POLL_SECONDS)

    return saw_output


def run_wifi_serial_actions(args: argparse.Namespace) -> int:
    if args.transport != "serial":
        raise RuntimeError("WiFi serial management commands require `--transport serial`")

    commands = build_wifi_serial_commands(args)
    if not commands:
        return 0

    port = resolve_serial_port(args)
    with open_serial_reader_writer(port, args.baud) as writer:
        print(f"Opened serial port: {port} @ {args.baud} baud")
        time.sleep(SERIAL_OPEN_DELAY_SECONDS)
        print_serial_response(writer, DEFAULT_COMMAND_RESPONSE_TIMEOUT_SECONDS)

        for command in commands:
            writer.write_line(command)
            print(f"Sent serial command: {command}")
            time.sleep(WIFI_COMMAND_DELAY_SECONDS)
            saw_output = print_serial_response(writer, response_timeout_for_command(command))
            if not saw_output:
                print("(No serial response received)")

    print("Completed WiFi serial command sequence")
    return 0


def main() -> int:
    args = parse_args()
    if args.list_ports:
        if args.transport != "serial":
            print("`--list-ports` is only available with --transport serial.")
            return 1
        return print_ports()

    if has_wifi_serial_action(args):
        if args.list_sensors or args.dry_run:
            raise RuntimeError("WiFi serial management commands cannot be combined with `--list-sensors` or `--dry-run`")
        return run_wifi_serial_actions(args)

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

        if args.transport == "serial":
            port = resolve_serial_port(args)

            with open_serial_sender(port, args.baud) as sender:
                print(f"Opened serial port: {port} @ {args.baud} baud")
                time.sleep(SERIAL_OPEN_DELAY_SECONDS)

                while not should_stop:
                    temp_c = provider.read_celsius(sensor)
                    payload = f"{temp_c:.2f}"
                    sender.send_temperature(payload)
                    print(f"Sent {payload} C over serial")
                    time.sleep(args.interval)
        else:
            if args.port:
                raise RuntimeError("`--port` is only valid with --transport serial")

            if not args.wifi_host:
                raise RuntimeError("`--wifi-host` is required with --transport wifi")

            with open_udp_sender(args.wifi_host, args.wifi_port) as sender:
                print(f"Sending UDP temperature updates to {args.wifi_host}:{args.wifi_port}")

                while not should_stop:
                    temp_c = provider.read_celsius(sensor)
                    payload = f"{temp_c:.2f}"
                    sender.send_temperature(payload)
                    print(f"Sent {payload} C over WiFi to {args.wifi_host}:{args.wifi_port}")
                    time.sleep(args.interval)

        print("Stopped temperature sender")
        return 0
    finally:
        provider.close()
