"""Gate, selection, tiers, release consistency and promotion policy (metrics only)."""
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
REPO = TOOLS.parents[1]
sys.path.insert(0, str(TOOLS))

import check_release  # noqa: E402
import gate  # noqa: E402
import promotion  # noqa: E402
from gate import OperatingPoint  # noqa: E402

BASE = OperatingPoint(0.71, 3, 0.966873, 0.827265, 9.670422)


def op(cutoff, window, recall, faph, hours=9.670422):
    return OperatingPoint(cutoff, window, recall, faph, hours)


class TestGate(unittest.TestCase):
    def test_margins_are_fixed_policy(self):
        self.assertEqual((gate.RECALL_TOLERANCE, gate.FAPH_TOLERANCE), (0.01, 0.05))

    def test_boundaries(self):
        self.assertTrue(gate.evaluate_gate(op(0.6, 3, BASE.recall - 0.01, BASE.false_accepts_per_hour + 0.05), BASE).passed)
        self.assertFalse(gate.evaluate_gate(op(0.6, 3, BASE.recall - 0.0101, 0.5), BASE).passed)
        self.assertFalse(gate.evaluate_gate(op(0.6, 3, 0.99, BASE.false_accepts_per_hour + 0.0501), BASE).passed)

    def test_eligible_is_not_improving(self):
        # A real retrain outcome: inside the gate, slightly lower recall, same FA/h.
        result = gate.evaluate_gate(op(0.29, 5, 0.973526, 0.827269), op(0.64, 3, 0.974885, 0.827265))
        self.assertTrue(result.passed)
        self.assertFalse(result.improves)
        self.assertIn("does not improve", " ".join(result.reasons))

    def test_select_evaluates_every_pair_and_prefers_improvement(self):
        grid = [op(c / 100, w, 0.95 + c / 10000, 0.8 + (100 - c) / 100) for c in range(50, 100) for w in (3, 5)]
        grid.append(op(0.66, 4, 0.9800, 0.80))
        point, result, count = gate.select_operating_point(grid, BASE)
        self.assertEqual(count, len(grid))
        self.assertEqual((point.cutoff, point.window), (0.66, 4))
        self.assertTrue(result.improves)

    def test_select_reports_no_passing_pair(self):
        grid = [op(0.5, 3, 0.90, 2.0)]
        self.assertEqual(gate.select_operating_point(grid, BASE)[0], None)

    def test_tier_rule(self):
        hours = 9.670422
        grid = [op(c / 100, 3, 0.97 + (64 - c) / 10000, events / hours)
                for c, events in ((50, 15), (51, 12), (54, 11), (57, 9), (60, 9), (64, 8), (70, 8))]
        tiers = gate.derive_tiers(grid, op(0.64, 3, 0.97, 8 / hours))
        self.assertEqual((tiers["moderate"]["cutoff"], tiers["very"]["cutoff"]), (0.57, 0.51))
        self.assertEqual((tiers["moderate_delta_uint8"], tiers["very_delta_uint8"]), (18, 33))


class TestReleaseCheck(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self.tmp.name)
        shutil.copytree(REPO / "models", self.repo / "models")
        shutil.copy(REPO / "home-assistant-voice.realtime.yaml", self.repo)

    def tearDown(self):
        self.tmp.cleanup()

    def edit_yaml(self, old, new):
        path = self.repo / "home-assistant-voice.realtime.yaml"
        text = path.read_text()
        self.assertIn(old, text)
        path.write_text(text.replace(old, new, 1))

    def test_repository_is_consistent(self):
        self.assertEqual(check_release.check(self.repo), [])

    def test_window_mismatch_fails(self):
        self.edit_yaml('wake_model_window: "3"', 'wake_model_window: "4"')
        self.assertTrue(any("window" in e for e in check_release.check(self.repo)))

    def test_unpinned_model_fails(self):
        text = (self.repo / "home-assistant-voice.realtime.yaml").read_text()
        pinned = check_release.PIN.search(text).group(0)
        self.edit_yaml(pinned, pinned.split("@")[0] + '@main"')
        self.assertTrue(any("pinned" in e for e in check_release.check(self.repo)))

    def test_model_json_cutoff_must_match_manifest(self):
        path = self.repo / "models" / "hey_leonard.json"
        model = json.loads(path.read_text())
        model["micro"]["probability_cutoff"] = 0.85
        path.write_text(json.dumps(model, indent=2) + "\n")
        errors = check_release.check(self.repo)
        self.assertTrue(any("cutoff" in e for e in errors))

    def test_loosened_gate_margin_fails(self):
        path = self.repo / "models" / "hey_leonard.eval.json"
        manifest = json.loads(path.read_text())
        manifest["gate"]["faph_tolerance"] = 0.2
        path.write_text(json.dumps(manifest))
        self.assertTrue(any("margins" in e for e in check_release.check(self.repo)))

    def test_contradictory_gate_result_fails(self):
        path = self.repo / "models" / "hey_leonard.eval.json"
        manifest = json.loads(path.read_text())
        manifest["operating_point"]["recall"] = 0.90
        path.write_text(json.dumps(manifest))
        self.assertTrue(any("contradicts" in e for e in check_release.check(self.repo)))

    def test_missing_rollback_manifest_fails(self):
        (self.repo / "models" / "previous" / "hey_leonard.eval.json").unlink()
        self.assertTrue(any("rollback" in e for e in check_release.check(self.repo)))

    def test_manifests_contain_no_audio_or_people(self):
        for path in (REPO / "models").rglob("*.eval.json"):
            text = path.read_text().lower()
            for forbidden in (".wav", "transcript\":", "voiceprint\":"):
                self.assertNotIn(forbidden, text, path)


class TestPromotion(unittest.TestCase):
    def setUp(self):
        self.current = json.loads((REPO / "models" / "hey_leonard.eval.json").read_text())

    def test_offline_requires_improvement(self):
        hold = json.loads(json.dumps(self.current))
        hold["operating_point"].update(recall=self.current["operating_point"]["recall"] - 0.001)
        self.assertEqual(promotion.offline_decision(hold, self.current).action, "hold")
        better = json.loads(json.dumps(self.current))
        better["operating_point"].update(recall=self.current["operating_point"]["recall"] + 0.002)
        self.assertEqual(promotion.offline_decision(better, self.current).action, "start_shadow")

    def test_shadow_needs_seven_days_and_quiet_candidate(self):
        report = {"period_days": 7, "devices": {"kitchen": {"wakes": 70, "shadow_detections": {"cand": 72}}}}
        self.assertEqual(promotion.shadow_decision(report, "kitchen", "cand").action, "start_canary")
        report["devices"]["kitchen"]["shadow_detections"]["cand"] = 70 + 30
        self.assertEqual(promotion.shadow_decision(report, "kitchen", "cand").action, "reject")
        report["period_days"] = 3
        self.assertEqual(promotion.shadow_decision(report, "kitchen", "cand").action, "continue")

    def test_canary_rolls_back_on_flag_rate(self):
        baseline = {"period_days": 7, "devices": {"kitchen": {"false_wake_flags": 7}}}
        ok = {"period_days": 7, "devices": {"kitchen": {"false_wake_flags": 9}}}
        bad = {"period_days": 2, "devices": {"kitchen": {"false_wake_flags": 9}}}
        self.assertEqual(promotion.canary_decision(ok, baseline, "kitchen").action, "promote_fleet")
        self.assertEqual(promotion.canary_decision(bad, baseline, "kitchen").action, "rollback")


if __name__ == "__main__":
    unittest.main()
