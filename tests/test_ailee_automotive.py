# Copyright (c) Don Michael Feeney Jr.
# Licensed under the MIT License.

import os
import json
import math
import sys
import tempfile
import unittest
from unittest.mock import patch
from ds_core.python.ds_ev_enhancer import (
    compute_mechanical_hp,
    compute_electrical_hp,
    hp_consistency_score,
    evaluate_governance,
    DSEVEnhancerOrchestrator,
)
from ds_core.python.ailee.core_min import (
    AileeTrustPipeline,
    AileeConfig,
    GovernanceLevel,
    GovernanceDecision,
)
from ds_core.python.ailee.domains.automotive.ailee_automotive_domain import (
    AileeAutomotiveDomain,
    AutonomyLevel,
)


class TestAileeAutomotiveDomain(unittest.TestCase):

    def setUp(self):
        self.audit_directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.audit_directory.cleanup)
        self.audit_path = os.path.join(self.audit_directory.name, "audit.log")
        logger_patch = patch(
            "ds_core.python.ds_ev_enhancer._global_orchestrator.audit_logger.log_path",
            self.audit_path,
        )
        logger_patch.start()
        self.addCleanup(logger_patch.stop)

    def assert_rejected(self, result):
        values = result if isinstance(result, dict) else result.to_dict()
        self.assertEqual(values["level"], 3)
        self.assertEqual(values["trust_score"], 0.0)
        self.assertTrue(values["used_fallback"])
        for key in ("governed_hp", "governed_torque", "governed_discharge_current",
                    "hp_consistency_score"):
            self.assertTrue(math.isfinite(values[key]), key)
            self.assertEqual(values[key], 0.0, key)
        for key in ("hp_mech", "hp_elec"):
            if key in values:
                self.assertTrue(math.isfinite(values[key]), key)
                self.assertEqual(values[key], 0.0, key)

    @staticmethod
    def valid_evidence():
        return {
            "hp_mech": 224.68, "hp_elec": 214.56,
            "hp_consistency_score": 214.56 / 224.68,
            "soc": 80.0, "soh": 90.0, "temp_c": 30.0,
            "sensor_valid": True, "max_torque_nm": 400.0, "max_current_a": 500.0,
        }

    def test_missing_or_malformed_direct_evidence_rejected(self):
        required = ("hp_mech", "hp_elec", "hp_consistency_score", "soc", "soh",
                    "temp_c", "sensor_valid")
        handlers = (AileeAutomotiveDomain().evaluate_signals,
                    AileeTrustPipeline().process,
                    AileeTrustPipeline(domain=AileeAutomotiveDomain()).process)
        for handler in handlers:
            with self.subTest(handler=handler, missing="all"):
                self.assert_rejected(handler({}))
            for key in required:
                evidence = self.valid_evidence()
                del evidence[key]
                with self.subTest(handler=handler, missing=key):
                    self.assert_rejected(handler(evidence))
            for key in self.valid_evidence():
                invalid_values = (False, "false", None) if key == "sensor_valid" else (
                    float("nan"), float("inf"), -float("inf"), None, "bad", [],
                )
                for invalid in invalid_values:
                    evidence = self.valid_evidence()
                    evidence[key] = invalid
                    with self.subTest(handler=handler, field=key, value=invalid):
                        self.assert_rejected(handler(evidence))
            for key, value in (("soc", -1.0), ("soc", 101.0), ("soh", -1.0),
                               ("soh", 101.0), ("hp_consistency_score", -0.1),
                               ("hp_consistency_score", 1.1), ("hp_mech", -1.0),
                               ("hp_elec", -1.0), ("max_torque_nm", -1.0),
                               ("max_current_a", -1.0)):
                evidence = self.valid_evidence()
                evidence[key] = value
                with self.subTest(handler=handler, field=key, value=value):
                    self.assert_rejected(handler(evidence))

    def test_invalid_or_overflowing_orchestrator_evidence_rejected(self):
        orchestrator = DSEVEnhancerOrchestrator(log_path=self.audit_path)
        normal = dict(torque_nm=400.0, rpm=4000.0, v_batt=400.0, i_batt=400.0,
                      soc=80.0, soh=90.0, temp_c=30.0,
                      max_torque_nm=400.0, max_current_a=500.0)
        for key in normal:
            for invalid in (float("nan"), float("inf"), -float("inf"), None, "bad", []):
                values = dict(normal, **{key: invalid})
                with self.subTest(field=key, value=invalid):
                    self.assert_rejected(orchestrator.evaluate_signals(**values))
        for key, invalid in (("torque_nm", -1.0), ("rpm", -1.0), ("v_batt", 0.0),
                             ("i_batt", -1.0), ("soc", 101.0), ("soh", -1.0),
                             ("sensor_valid", "false"), ("sensor_valid", False)):
            with self.subTest(field=key, value=invalid):
                self.assert_rejected(orchestrator.evaluate_signals(**dict(normal, **{key: invalid})))
        values = dict(normal, torque_nm=sys.float_info.max, rpm=sys.float_info.max,
                      i_batt=sys.float_info.max)
        self.assert_rejected(orchestrator.evaluate_signals(**values))
        with open(self.audit_path, encoding="utf-8") as audit:
            for line in audit:
                json.loads(line, parse_constant=lambda value: self.fail(f"Nonfinite audit value: {value}"))

    def test_valid_critical_policy_preserved(self):
        evidence = dict(self.valid_evidence(), temp_c=60.0)
        direct = AileeAutomotiveDomain().evaluate_signals(evidence)
        self.assertEqual(direct.level, 3)
        self.assertEqual(direct.governed_torque, 100.0)
        self.assertEqual(direct.governed_discharge_current, 125.0)

    def test_malformed_domain_decision_rejected(self):
        class Domain:
            def evaluate_signals(self, signals):
                return GovernanceDecision(
                    level=0, governed_hp=float("nan"), governed_torque=float("inf"),
                    governed_discharge_current=500.0, trust_score=float("nan"),
                    hp_consistency_score=1.0, reason="invalid result",
                )
        self.assert_rejected(AileeTrustPipeline(domain=Domain()).process(self.valid_evidence()))

    def test_boolean_domain_decision_numbers_rejected(self):
        class Domain:
            def evaluate_signals(self, signals):
                return GovernanceDecision(
                    level=0, governed_hp=True, governed_torque=True,
                    governed_discharge_current=True, trust_score=True,
                    hp_consistency_score=True, reason="Invalid boolean numbers",
                )
        self.assert_rejected(AileeTrustPipeline(domain=Domain()).process(self.valid_evidence()))

    def test_domain_zero_authorization_is_preserved(self):
        class Domain:
            def evaluate_signals(self, signals):
                return GovernanceDecision(
                    level=3, governed_hp=0.0, governed_torque=0.0,
                    governed_discharge_current=0.0, trust_score=0.0,
                    hp_consistency_score=0.0, reason="Evidence rejected", used_fallback=True,
                )
        self.assert_rejected(AileeTrustPipeline(domain=Domain()).process(self.valid_evidence()))

    def test_hp_calculations(self):
        # 400 Nm @ 4000 RPM -> 224.68 HP
        hp_mech = compute_mechanical_hp(400.0, 4000.0)
        self.assertAlmostEqual(hp_mech, 224.681, places=2)

        # 400V, 400A -> 160 kW -> 214.56 HP
        hp_elec = compute_electrical_hp(400.0, 400.0)
        self.assertAlmostEqual(hp_elec, 214.563, places=2)

        score = hp_consistency_score(hp_mech, hp_elec)
        self.assertGreater(score, 0.90)

    def test_governance_normal_mode(self):
        res = evaluate_governance(
            torque_nm=300.0,
            rpm=4000.0,
            v_batt=380.0,
            i_batt=350.0,
            soc=85.0,
            soh=95.0,
            temp_c=30.0,
            sensor_valid=True,
        )
        self.assertEqual(res["level"], 0)
        self.assertGreaterEqual(res["trust_score"], 0.85)
        self.assertFalse(res["used_fallback"])

    def test_governance_thermal_derating(self):
        res = evaluate_governance(
            torque_nm=300.0,
            rpm=4000.0,
            v_batt=380.0,
            i_batt=350.0,
            soc=85.0,
            soh=95.0,
            temp_c=52.0,  # High temperature
            sensor_valid=True,
        )
        self.assertGreaterEqual(res["level"], 1)
        self.assertLess(res["governed_hp"], 200.0)

    def test_governance_sensor_fault_fallback(self):
        res = evaluate_governance(
            torque_nm=300.0,
            rpm=4000.0,
            v_batt=380.0,
            i_batt=350.0,
            soc=85.0,
            soh=95.0,
            temp_c=30.0,
            sensor_valid=False,  # Sensor fault
        )
        self.assertEqual(res["level"], 3)
        self.assertTrue(res["used_fallback"])

    def test_audit_logging(self):
        test_log_path = self.audit_path
        if os.path.exists(test_log_path):
            os.remove(test_log_path)

        orchestrator = DSEVEnhancerOrchestrator(log_path=test_log_path)
        res = orchestrator.evaluate_signals(
            torque_nm=350.0,
            rpm=4200.0,
            v_batt=390.0,
            i_batt=380.0,
            soc=80.0,
            soh=90.0,
            temp_c=28.0,
        )

        self.assertTrue(os.path.exists(test_log_path))
        with open(test_log_path, "r", encoding="utf-8") as f:
            lines = f.readlines()
            self.assertGreater(len(lines), 0)
            log_data = json.loads(lines[-1])
            self.assertEqual(log_data["level"], res["level"])
            self.assertIn("timestamp", log_data)

        if os.path.exists(test_log_path):
            os.remove(test_log_path)


if __name__ == "__main__":
    unittest.main()
