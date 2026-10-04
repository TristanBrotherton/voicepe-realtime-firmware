#!/usr/bin/env python3
"""Check that the packaged wake model, its eval manifest and the firmware agree.

Run from the repository root (CI does). Fails when:

* an eval manifest is missing a required field, has a gate margin other than
  the fixed policy (recall -0.01, FA/h +0.05), or records a gate result that
  does not follow from its own numbers;
* the model JSON's cutoff/window differ from the manifest's operating point
  (calibration and runtime metadata must be identical);
* the tflite/json sha256 differ from the manifest;
* the firmware's wake_model_sha256 / wake_model_window / tier deltas differ
  from the packaged model and its manifest;
* the model reference is not pinned to an immutable 40-hex commit, or (with
  --git) that commit does not contain the model the firmware claims to run;
* the rollback model in models/previous/ lacks a matching manifest.

Metrics only — no audio is read.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import List

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gate import FAPH_TOLERANCE, RECALL_TOLERANCE, OperatingPoint, derive_tiers, evaluate_gate  # noqa: E402

SCHEMA = "voicepe.wakeword.eval/1"
REQUIRED = {
    "schema": str,
    "model": dict,
    "provenance": dict,
    "data_counts": dict,
    "evaluation": dict,
    "operating_point": dict,
    "stratified_recall": dict,
    "gate": dict,
}
MODEL_KEYS = ("name", "generation", "tflite_sha256", "json_sha256", "cutoff", "window")
OPERATING_KEYS = ("cutoff", "cutoff_uint8", "window", "recall", "false_accepts_per_hour", "ambient_hours")
EVAL_KEYS = ("positive_dataset", "positive_tracks", "ambient_dataset", "ambient_hours", "pairs_evaluated")
STRATA = ("close", "conversational", "across_room")
PIN = re.compile(r'wake_word_model: "github://[^"@]+@([0-9a-f]{40})"')


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def substitution(yaml_text: str, key: str) -> str:
    match = re.search(rf'^  {re.escape(key)}: "?([^"\n]*)"?\s*$', yaml_text, re.M)
    return match.group(1).strip() if match else ""


def validate_manifest(manifest: dict, label: str) -> List[str]:
    errors = []
    for key, kind in REQUIRED.items():
        if not isinstance(manifest.get(key), kind):
            errors.append(f"{label}: missing or invalid '{key}'")
    if errors:
        return errors
    if manifest["schema"] != SCHEMA:
        errors.append(f"{label}: schema must be {SCHEMA}")
    for key in MODEL_KEYS:
        if key not in manifest["model"]:
            errors.append(f"{label}: model.{key} missing")
    for key in OPERATING_KEYS:
        if key not in manifest["operating_point"]:
            errors.append(f"{label}: operating_point.{key} missing")
    for key in EVAL_KEYS:
        if key not in manifest["evaluation"]:
            errors.append(f"{label}: evaluation.{key} missing")
    strata = manifest["stratified_recall"]
    for key in STRATA:
        if key not in strata:
            errors.append(f"{label}: stratified_recall.{key} missing (null + status when not measured)")
    if any(strata.get(k) is None for k in STRATA) and not strata.get("status"):
        errors.append(f"{label}: unmeasured strata need a status explaining why")
    gate = manifest["gate"]
    if gate.get("recall_tolerance") != RECALL_TOLERANCE or gate.get("faph_tolerance") != FAPH_TOLERANCE:
        errors.append(f"{label}: gate margins must be exactly recall {RECALL_TOLERANCE} / FA/h {FAPH_TOLERANCE}")
    op = manifest["operating_point"]
    try:
        if int(op["cutoff_uint8"]) != int(round(float(op["cutoff"]) * 255)):
            errors.append(f"{label}: cutoff_uint8 does not match cutoff")
    except (KeyError, TypeError, ValueError):
        errors.append(f"{label}: operating point is not numeric")
    baseline = manifest.get("baseline")
    if baseline:
        candidate = OperatingPoint(float(op["cutoff"]), int(op["window"]), float(op["recall"]),
                                   float(op["false_accepts_per_hour"]), float(op["ambient_hours"]))
        base = OperatingPoint(float(baseline["cutoff"]), int(baseline["window"]), float(baseline["recall"]),
                              float(baseline["false_accepts_per_hour"]),
                              float(baseline.get("ambient_hours", op["ambient_hours"])))
        result = evaluate_gate(candidate, base)
        recorded = gate.get("result")
        if recorded != ("pass" if result.passed else "fail"):
            errors.append(f"{label}: recorded gate result '{recorded}' contradicts its numbers")
        if bool(gate.get("improves_baseline")) != result.improves:
            errors.append(f"{label}: improves_baseline contradicts its numbers")
    elif gate.get("result") not in ("baseline", "not_evaluable") or (
        gate.get("result") == "not_evaluable" and not gate.get("reasons")
    ):
        errors.append(f"{label}: without a baseline the gate result must be 'baseline' or "
                      "'not_evaluable' with reasons")
    evidence = manifest.get("tier_evidence")
    tiers = manifest.get("tiers")
    if tiers and evidence:
        grid = [OperatingPoint.from_dict(p) for p in evidence]
        selected = OperatingPoint(float(op["cutoff"]), int(op["window"]), float(op["recall"]),
                                  float(op["false_accepts_per_hour"]), float(op["ambient_hours"]))
        derived = derive_tiers(grid, selected)
        for key in ("moderate_delta_uint8", "very_delta_uint8"):
            if derived[key] != tiers.get(key):
                errors.append(f"{label}: tiers.{key} {tiers.get(key)} != derived {derived[key]}")
    return errors


def check(repo: Path, use_git: bool = False) -> List[str]:
    errors: List[str] = []
    yaml_text = (repo / "home-assistant-voice.realtime.yaml").read_text(encoding="utf-8")
    for folder, label in ((repo / "models", "current"), (repo / "models" / "previous", "rollback")):
        json_path = folder / "hey_leonard.json"
        tflite_path = folder / "hey_leonard.tflite"
        manifest_path = folder / "hey_leonard.eval.json"
        if not manifest_path.exists():
            errors.append(f"{label}: {manifest_path.relative_to(repo)} missing")
            continue
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        errors.extend(validate_manifest(manifest, label))
        if errors and any(e.startswith(f"{label}: missing") for e in errors):
            continue
        model_json = json.loads(json_path.read_text(encoding="utf-8"))
        micro = model_json.get("micro", {})
        op = manifest["operating_point"]
        if round(float(micro.get("probability_cutoff", -1)) * 255) != int(op["cutoff_uint8"]):
            errors.append(f"{label}: model JSON cutoff {micro.get('probability_cutoff')} != manifest {op['cutoff']}")
        if int(micro.get("sliding_window_size", -1)) != int(op["window"]):
            errors.append(f"{label}: model JSON window {micro.get('sliding_window_size')} != manifest {op['window']}")
        if sha256(tflite_path) != manifest["model"]["tflite_sha256"]:
            errors.append(f"{label}: tflite sha256 differs from the manifest")
        if sha256(json_path) != manifest["model"]["json_sha256"]:
            errors.append(f"{label}: model JSON sha256 differs from the manifest")
        if label == "current":
            current = manifest
    if errors:
        return errors

    tflite_sha = current["model"]["tflite_sha256"]
    if substitution(yaml_text, "wake_model_sha256") != tflite_sha:
        errors.append("firmware wake_model_sha256 != packaged model (update it, then the pin)")
    if substitution(yaml_text, "wake_model_window") != str(current["operating_point"]["window"]):
        errors.append("firmware wake_model_window != packaged model window")
    tiers = current.get("tiers") or {}
    for sub_key, tier_key in (("wake_cutoff_moderate_delta", "moderate_delta_uint8"),
                              ("wake_cutoff_very_delta", "very_delta_uint8")):
        if substitution(yaml_text, sub_key) != str(tiers.get(tier_key)):
            errors.append(f"firmware {sub_key} != manifest tiers.{tier_key}")
    pin = PIN.search(yaml_text)
    if not pin:
        errors.append("wake_word_model must be pinned to a 40-hex commit (no @main)")
    elif use_git:
        commit = pin.group(1)
        try:
            blob = subprocess.run(["git", "show", f"{commit}:models/hey_leonard.tflite"], cwd=repo,
                                  capture_output=True, check=True).stdout
            if hashlib.sha256(blob).hexdigest() != substitution(yaml_text, "wake_model_sha256"):
                errors.append(f"pinned commit {commit[:12]} does not contain the model wake_model_sha256 names")
        except subprocess.CalledProcessError:
            errors.append(f"pinned commit {commit[:12]} is not in this repository's history")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--git", action="store_true", help="also verify the pinned commit's model via git")
    args = parser.parse_args()
    errors = check(args.repo.resolve(), use_git=args.git)
    for error in errors:
        print(f"ERROR: {error}")
    if not errors:
        print("wake-word release metadata is consistent")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
