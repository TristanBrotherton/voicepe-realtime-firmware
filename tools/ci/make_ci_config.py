#!/usr/bin/env python3
"""Generate a self-contained ESPHome config that compiles THIS checkout.

The published package pulls va_client from GitHub (``ref: ${va_client_ref}``)
and the wake model from a pinned commit, so compiling it as-is would build
whatever is already published, not the change under review. This script
writes a copy of home-assistant-voice.realtime.yaml that uses the local
component and model files, plus a stub with throwaway build-only secrets.

Usage: make_ci_config.py <output-dir>   (prints the stub path to compile)
"""
from __future__ import annotations

import base64
import re
import secrets
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
PACKAGE = REPO / "home-assistant-voice.realtime.yaml"

VA_CLIENT_SOURCE = re.compile(
    r"  - source:\n      type: git\n      url: https://github\.com/TristanBrotherton/voicepe-realtime-firmware\n"
    r"      ref: \$\{va_client_ref\}\n    components: \[va_client\]\n    refresh: 0s\n"
)
MODEL_SUBSTITUTION = re.compile(r'^  wake_word_model: ".*"$', re.M)


def build(out_dir: Path) -> Path:
    text = PACKAGE.read_text(encoding="utf-8")
    text, n_source = VA_CLIENT_SOURCE.subn(
        "  - source:\n      type: local\n      path: " + str(REPO / "esphome" / "components")
        + "\n    components: [va_client]\n", text)
    text, n_model = MODEL_SUBSTITUTION.subn(
        '  wake_word_model: "' + str(REPO / "models" / "hey_leonard.json") + '"', text)
    if n_source != 1 or n_model != 1:
        raise SystemExit(f"could not localize package (source={n_source}, model={n_model})")
    out_dir.mkdir(parents=True, exist_ok=True)
    package = out_dir / "home-assistant-voice.realtime.ci.yaml"
    package.write_text(text, encoding="utf-8")
    # Build-only throwaway secrets: the image is compiled, never flashed.
    encryption_key = base64.b64encode(secrets.token_bytes(32)).decode()
    stub = out_dir / "voice-pe-ci.yaml"
    stub.write_text(
        "substitutions:\n"
        "  name: voice-pe-ci\n"
        "  friendly_name: Voice PE CI\n"
        "  wifi_ssid: ci-build-only\n"
        f"  wifi_password: {secrets.token_hex(12)}\n"
        f"  ota_password: {secrets.token_hex(12)}\n"
        f"  api_key: {encryption_key}\n"
        f"  va_token: {secrets.token_hex(16)}\n"
        "packages:\n"
        f"  realtime: !include {package.name}\n",
        encoding="utf-8",
    )
    return stub


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    print(build(Path(sys.argv[1]).resolve()))
