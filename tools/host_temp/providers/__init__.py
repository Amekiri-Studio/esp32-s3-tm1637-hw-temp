from __future__ import annotations

import platform

from .base import TemperatureProvider
from .linux import LinuxTemperatureProvider
from .macos import MacOSTemperatureProvider
from .windows import WindowsTemperatureProvider


def normalize_platform_name(name: str) -> str:
    lowered = name.lower()

    if lowered == "darwin":
        return "macos"
    if lowered in {"win32", "cygwin"}:
        return "windows"
    if lowered.startswith("linux"):
        return "linux"

    return lowered


def get_provider(requested_platform: str, *, lhm_dll_path: str | None = None) -> TemperatureProvider:
    platform_name = normalize_platform_name(requested_platform)
    if platform_name == "auto":
        platform_name = normalize_platform_name(platform.system())

    if platform_name == "linux":
        return LinuxTemperatureProvider()
    if platform_name == "windows":
        return WindowsTemperatureProvider(lhm_dll_path=lhm_dll_path)
    if platform_name == "macos":
        return MacOSTemperatureProvider()

    raise RuntimeError(f"Unsupported platform provider: {requested_platform}")
