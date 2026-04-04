from __future__ import annotations

from pathlib import Path

from .base import SensorCandidate


PREFERRED_KEYWORDS = {
    "package id": 120,
    "tctl": 110,
    "tdie": 110,
    "cpu package": 110,
    "cpu": 90,
    "core": 70,
    "coretemp": 90,
    "k10temp": 90,
    "zenpower": 90,
    "x86_pkg_temp": 120,
    "cpu_thermal": 100,
    "soc_thermal": 80,
}

AVOID_KEYWORDS = {
    "gpu": -200,
    "nvme": -200,
    "pch": -150,
    "wireless": -150,
    "iwlwifi": -150,
    "bat": -150,
    "battery": -150,
    "acpitz": -100,
}


def build_priority(*parts: str) -> int:
    text = " ".join(part for part in parts if part).lower()
    priority = 0

    for keyword, weight in PREFERRED_KEYWORDS.items():
        if keyword in text:
            priority += weight

    for keyword, weight in AVOID_KEYWORDS.items():
        if keyword in text:
            priority += weight

    return priority


def discover_hwmon_candidates() -> list[SensorCandidate]:
    candidates: list[SensorCandidate] = []

    for hwmon_dir in sorted(Path("/sys/class/hwmon").glob("hwmon*")):
        name_path = hwmon_dir / "name"
        hwmon_name = name_path.read_text(encoding="utf-8").strip() if name_path.exists() else hwmon_dir.name

        for input_path in sorted(hwmon_dir.glob("temp*_input")):
            stem = input_path.stem.removesuffix("_input")
            label_path = hwmon_dir / f"{stem}_label"
            label = label_path.read_text(encoding="utf-8").strip() if label_path.exists() else ""

            priority = build_priority(hwmon_name, label)
            source = f"hwmon:{hwmon_name}"
            display_label = label or stem
            candidates.append(SensorCandidate(priority, source, str(input_path), display_label))

    return candidates


def discover_thermal_zone_candidates() -> list[SensorCandidate]:
    candidates: list[SensorCandidate] = []

    for zone_dir in sorted(Path("/sys/class/thermal").glob("thermal_zone*")):
        temp_path = zone_dir / "temp"
        if not temp_path.exists():
            continue

        type_path = zone_dir / "type"
        zone_type = type_path.read_text(encoding="utf-8").strip() if type_path.exists() else zone_dir.name
        priority = build_priority(zone_type)
        source = f"thermal:{zone_dir.name}"
        candidates.append(SensorCandidate(priority, source, str(temp_path), zone_type))

    return candidates


class LinuxTemperatureProvider:
    platform_name = "linux"

    def list_candidates(self) -> list[SensorCandidate]:
        candidates = discover_hwmon_candidates() + discover_thermal_zone_candidates()
        return sorted(candidates, reverse=True)

    def choose_sensor(self, explicit_identifier: str | None) -> SensorCandidate:
        if explicit_identifier:
            input_path = Path(explicit_identifier)
            if not input_path.exists():
                raise FileNotFoundError(f"Sensor path does not exist: {input_path}")

            return SensorCandidate(0, "manual", str(input_path), input_path.name)

        candidates = self.list_candidates()
        if not candidates:
            raise RuntimeError("No temperature sensors were found in /sys/class/hwmon or /sys/class/thermal")

        return candidates[0]

    def read_celsius(self, sensor: SensorCandidate) -> float:
        raw_value = Path(sensor.identifier).read_text(encoding="utf-8").strip()
        return int(raw_value) / 1000.0

    def close(self) -> None:
        return None
