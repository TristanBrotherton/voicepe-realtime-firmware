#!/usr/bin/env python3
"""Fail on private data or stray artifacts in this public repository.

Scans every git-tracked file for:
  * private IPv4 addresses (RFC 1918) — examples must use 192.0.2.0/24 etc.
  * e-mail addresses other than GitHub noreply / example domains
  * credential-shaped strings, coding-session URLs, assistant attribution
  * audio outside sounds/, model artifacts outside models/, trainer
    calibration dumps, and per-device secrets files

Names of real people cannot be listed in a public file. Maintainers can point
VOICEPE_PRIVATE_DENYLIST at a local, untracked file (one case-insensitive
term per line) to include them in the same scan.
"""
from __future__ import annotations

import os
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

PATTERNS = {
    "private IPv4 address": re.compile(
        r"(?<![\d.])(10\.\d{1,3}\.\d{1,3}\.\d{1,3}|192\.168\.\d{1,3}\.\d{1,3}|172\.(1[6-9]|2\d|3[01])\.\d{1,3}\.\d{1,3})(?![\d.])"
    ),
    "e-mail address": re.compile(r"[A-Za-z0-9._%+-]+@(?!users\.noreply\.github\.com)(?!example\.(com|org|net))[A-Za-z0-9-]+\.[A-Za-z0-9.-]*[A-Za-z]{2,}"),
    "credential": re.compile(r"(sk-[A-Za-z0-9_-]{20,}|eyJ[A-Za-z0-9_-]{30,}\.[A-Za-z0-9_-]+|ghp_[A-Za-z0-9]{30,})"),
    "coding-session URL or assistant attribution": re.compile(r"(claude\.ai/code|Co-Authored-By:\s*Claude)", re.I),
}
TEXT_SUFFIXES = {".yaml", ".yml", ".md", ".py", ".h", ".cpp", ".c", ".json", ".html", ".txt", ".example", ".cfg", ".toml", ""}
AUDIO = re.compile(r"\.(wav|flac|mp3|m4a|ogg|opus|webm)$", re.I)
MODEL = re.compile(r"\.(tflite|onnx|pt|pth|npy|npz|pkl)$", re.I)
ALLOWED_AUDIO = re.compile(r"^sounds/[^/]+\.flac$")
ALLOWED_MODEL = re.compile(r"^models/(previous/)?[^/]+\.tflite$")
# Upstream sources vendored verbatim keep their own contact lines.
SKIP_TEXT = re.compile(r"^(LICENSE|\.github/ISSUE_TEMPLATE/)")


def tracked_files() -> list[str]:
    out = subprocess.run(["git", "ls-files"], cwd=REPO, capture_output=True, text=True, check=True).stdout
    return [line for line in out.splitlines() if line]


def denylist() -> list[str]:
    path = os.environ.get("VOICEPE_PRIVATE_DENYLIST", "")
    if not path:
        return []
    terms = [t.strip() for t in Path(path).read_text(encoding="utf-8").splitlines()]
    return [t for t in terms if t and not t.startswith("#")]


def main() -> int:
    problems = []
    private_terms = denylist()
    for rel in tracked_files():
        if AUDIO.search(rel) and not ALLOWED_AUDIO.match(rel):
            problems.append(f"{rel}: audio file outside sounds/")
        if MODEL.search(rel) and not ALLOWED_MODEL.match(rel):
            problems.append(f"{rel}: model/tensor artifact outside models/")
        if rel.endswith("detection_calibration.json") or rel.endswith("secrets.yaml"):
            problems.append(f"{rel}: trainer calibration dump or per-device secrets")
        path = REPO / rel
        if Path(rel).suffix.lower() not in TEXT_SUFFIXES or SKIP_TEXT.match(rel) or not path.is_file():
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        for number, line in enumerate(text.splitlines(), 1):
            for label, pattern in PATTERNS.items():
                if pattern.search(line):
                    problems.append(f"{rel}:{number}: {label}")
            lowered = line.lower()
            for term in private_terms:
                if term.lower() in lowered:
                    problems.append(f"{rel}:{number}: private denylist term")
    for problem in problems:
        print(f"ERROR: {problem}")
    if not problems:
        print(f"public scan clean ({len(tracked_files())} tracked files"
              + (f", {len(private_terms)} private terms" if private_terms else "") + ")")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
