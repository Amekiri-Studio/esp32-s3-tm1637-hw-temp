from __future__ import annotations

import os
import sys
from pathlib import Path

from .base import SensorCandidate


PREFERRED_KEYWORDS = {
    "package": 140,
    "package id": 150,
    "cpu package": 150,
    "tctl": 140,
    "tdie": 140,
    "core max": 110,
    "core average": 100,
    "cpu": 70,
    "core": 40,
}


class WindowsTemperatureProvider:
    platform_name = "windows"

    def __init__(self, *, lhm_dll_path: str | None = None) -> None:
        self._lhm_dll_path = lhm_dll_path or os.environ.get("LIBREHARDWAREMONITOR_DLL")
        self._computer = None
        self._sensor_type_temperature = None
        self._sensor_cache: dict[str, object] = {}
        self._runtime_loaded = False

    def _candidate_dll_paths(self) -> list[Path]:
        paths: list[Path] = []

        if self._lhm_dll_path:
            paths.append(Path(self._lhm_dll_path))

        project_root = Path(__file__).resolve().parents[3]
        paths.extend(
            [
                Path.cwd() / "LibreHardwareMonitorLib.dll",
                project_root / "LibreHardwareMonitorLib.dll",
                project_root / "tools" / "LibreHardwareMonitorLib.dll",
            ]
        )

        unique_paths: list[Path] = []
        seen: set[Path] = set()
        for path in paths:
            resolved = path.expanduser()
            if resolved in seen:
                continue
            seen.add(resolved)
            unique_paths.append(resolved)

        return unique_paths

    def _resolve_dll_path(self) -> Path:
        for dll_path in self._candidate_dll_paths():
            if dll_path.exists():
                return dll_path

        raise RuntimeError(
            "LibreHardwareMonitorLib.dll was not found. Download LibreHardwareMonitor on Windows and pass "
            "`--librehardwaremonitor-dll C:\\path\\to\\LibreHardwareMonitorLib.dll`, or set "
            "LIBREHARDWAREMONITOR_DLL."
        )

    def _ensure_runtime(self) -> None:
        if self._runtime_loaded:
            return

        try:
            import clr  # type: ignore[import-not-found]
        except ImportError as exc:
            raise RuntimeError(
                "The Windows LibreHardwareMonitor provider requires pythonnet. Install it with "
                "`py -m pip install pythonnet`."
            ) from exc

        dll_path = self._resolve_dll_path()
        dll_directory = str(dll_path.parent)

        if hasattr(os, "add_dll_directory"):
            os.add_dll_directory(dll_directory)

        if dll_directory not in sys.path:
            sys.path.insert(0, dll_directory)

        clr.AddReference(str(dll_path))

        from LibreHardwareMonitor.Hardware import Computer, SensorType  # type: ignore[import-not-found]

        self._computer = Computer()
        self._computer.IsCpuEnabled = True
        self._computer.Open()
        self._sensor_type_temperature = SensorType.Temperature
        self._runtime_loaded = True

    def _update_hardware_tree(self, hardware: object) -> None:
        hardware.Update()
        for sub_hardware in hardware.SubHardware:
            self._update_hardware_tree(sub_hardware)

    def _iter_hardware(self):
        self._ensure_runtime()
        for hardware in self._computer.Hardware:
            yield hardware
            for sub_hardware in hardware.SubHardware:
                yield from self._iter_subhardware(sub_hardware)

    def _iter_subhardware(self, hardware: object):
        yield hardware
        for sub_hardware in hardware.SubHardware:
            yield from self._iter_subhardware(sub_hardware)

    def _refresh_sensors(self) -> list[SensorCandidate]:
        self._ensure_runtime()

        for hardware in self._computer.Hardware:
            self._update_hardware_tree(hardware)

        candidates: list[SensorCandidate] = []
        sensor_cache: dict[str, object] = {}

        for hardware in self._iter_hardware():
            if str(hardware.HardwareType).lower() != "cpu":
                continue

            for sensor in hardware.Sensors:
                if sensor.SensorType != self._sensor_type_temperature:
                    continue
                if sensor.Value is None:
                    continue

                identifier = str(sensor.Identifier)
                label = str(sensor.Name)
                source = f"lhm:{hardware.Name}"
                priority = self._build_priority(hardware_name=str(hardware.Name), sensor_name=label, identifier=identifier)

                sensor_cache[identifier] = sensor
                candidates.append(SensorCandidate(priority, source, identifier, label))

        self._sensor_cache = sensor_cache
        return sorted(candidates, reverse=True)

    def _build_priority(self, *, hardware_name: str, sensor_name: str, identifier: str) -> int:
        text = f"{hardware_name} {sensor_name} {identifier}".lower()
        priority = 0

        for keyword, weight in PREFERRED_KEYWORDS.items():
            if keyword in text:
                priority += weight

        return priority

    def list_candidates(self) -> list[SensorCandidate]:
        candidates = self._refresh_sensors()
        if not candidates:
            raise RuntimeError(
                "LibreHardwareMonitor did not report any CPU temperature sensors. Try running the command from an "
                "elevated terminal, or inspect sensors in the LibreHardwareMonitor app first."
            )

        return candidates

    def choose_sensor(self, explicit_identifier: str | None) -> SensorCandidate:
        candidates = self.list_candidates()
        if explicit_identifier is None:
            return candidates[0]

        for candidate in candidates:
            if candidate.identifier == explicit_identifier:
                return candidate

        lowered = explicit_identifier.lower()
        for candidate in candidates:
            if candidate.label.lower() == lowered or lowered in candidate.identifier.lower():
                return candidate

        raise RuntimeError(f"LibreHardwareMonitor sensor not found: {explicit_identifier}")

    def read_celsius(self, sensor: SensorCandidate) -> float:
        self._refresh_sensors()
        sensor_object = self._sensor_cache.get(sensor.identifier)
        if sensor_object is None:
            raise RuntimeError(f"LibreHardwareMonitor sensor disappeared: {sensor.identifier}")
        if sensor_object.Value is None:
            raise RuntimeError(f"LibreHardwareMonitor sensor has no current value: {sensor.identifier}")

        return float(sensor_object.Value)

    def close(self) -> None:
        if self._computer is not None:
            self._computer.Close()
            self._computer = None
