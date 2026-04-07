from __future__ import annotations

import asyncio
from dataclasses import dataclass


@dataclass(frozen=True)
class BluetoothDeviceCandidate:
    address: str
    name: str
    rssi: int | None
    details: str


DEFAULT_BLUETOOTH_NAME = "HWTempDisplay"
BLE_CHARACTERISTIC_UUID_RX = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
DEFAULT_BLUETOOTH_SCAN_TIMEOUT_SECONDS = 5.0


class BleTemperatureSender:
    def __init__(self, address: str | None, name: str | None, scan_timeout: float = DEFAULT_BLUETOOTH_SCAN_TIMEOUT_SECONDS) -> None:
        self._address = address
        self._name = name
        self._scan_timeout = scan_timeout
        self._loop: asyncio.AbstractEventLoop | None = None
        self._client: object | None = None

    async def _resolve_target(self) -> object:
        try:
            from bleak import BleakScanner
        except ImportError as exc:
            raise RuntimeError("Bluetooth transport requires the `bleak` package. Install it with `python3 -m pip install bleak`.") from exc

        if self._address:
            device = await BleakScanner.find_device_by_address(self._address, timeout=self._scan_timeout)
            if device is None:
                raise RuntimeError(f"Bluetooth device not found for address: {self._address}")
            return device

        target_name = self._name or DEFAULT_BLUETOOTH_NAME
        devices = await BleakScanner.discover(timeout=self._scan_timeout)
        for device in devices:
            if getattr(device, "name", None) == target_name:
                return device

            metadata = getattr(device, "metadata", {})
            if isinstance(metadata, dict) and metadata.get("local_name") == target_name:
                return device

        raise RuntimeError(f"Bluetooth device not found for name: {target_name}")

    async def _connect(self) -> None:
        try:
            from bleak import BleakClient
        except ImportError as exc:
            raise RuntimeError("Bluetooth transport requires the `bleak` package. Install it with `python3 -m pip install bleak`.") from exc

        target = await self._resolve_target()
        self._client = BleakClient(target)
        await self._client.connect()

    async def _disconnect(self) -> None:
        if self._client is None:
            return

        client = self._client
        self._client = None
        await client.disconnect()

    async def _write(self, text: str) -> None:
        if self._client is None:
            raise RuntimeError("Bluetooth sender is not connected")

        await self._client.write_gatt_char(BLE_CHARACTERISTIC_UUID_RX, text.encode("ascii"), response=True)

    def send_temperature(self, text: str) -> None:
        if self._loop is None:
            raise RuntimeError("Bluetooth sender has not been opened")

        self._loop.run_until_complete(self._write(text))

    def close(self) -> None:
        if self._loop is None:
            return

        try:
            self._loop.run_until_complete(self._disconnect())
        finally:
            self._loop.close()
            self._loop = None

    def __enter__(self) -> "BleTemperatureSender":
        self._loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self._loop)
        self._loop.run_until_complete(self._connect())
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        self.close()


def open_bluetooth_sender(address: str | None, name: str | None, scan_timeout: float = DEFAULT_BLUETOOTH_SCAN_TIMEOUT_SECONDS) -> BleTemperatureSender:
    return BleTemperatureSender(address=address, name=name, scan_timeout=scan_timeout)


async def _discover_bluetooth_devices(scan_timeout: float) -> list[BluetoothDeviceCandidate]:
    try:
        from bleak import BleakScanner
    except ImportError as exc:
        raise RuntimeError("Bluetooth features require the `bleak` package. Install it with `python3 -m pip install bleak`.") from exc

    devices = await BleakScanner.discover(timeout=scan_timeout)
    candidates: list[BluetoothDeviceCandidate] = []
    for device in devices:
        name = getattr(device, "name", None) or "<unknown>"
        address = getattr(device, "address", None) or "<unknown>"
        rssi = getattr(device, "rssi", None)
        metadata = getattr(device, "metadata", {})
        details = ""
        if isinstance(metadata, dict) and metadata:
            uuids = metadata.get("uuids")
            local_name = metadata.get("local_name")
            detail_parts: list[str] = []
            if local_name and local_name != name:
                detail_parts.append(f"local_name={local_name}")
            if uuids:
                detail_parts.append(f"uuids={','.join(uuids)}")
            details = " | ".join(detail_parts)

        candidates.append(BluetoothDeviceCandidate(address=address, name=name, rssi=rssi, details=details))

    candidates.sort(key=lambda candidate: (candidate.name == "<unknown>", -(candidate.rssi or -999), candidate.name, candidate.address))
    return candidates


def list_bluetooth_devices(scan_timeout: float = DEFAULT_BLUETOOTH_SCAN_TIMEOUT_SECONDS) -> list[BluetoothDeviceCandidate]:
    loop = asyncio.new_event_loop()
    try:
        asyncio.set_event_loop(loop)
        return loop.run_until_complete(_discover_bluetooth_devices(scan_timeout))
    finally:
        loop.close()
