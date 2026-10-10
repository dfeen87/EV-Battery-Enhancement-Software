# Copyright (c) Don Michael Feeney Jr.
# Licensed under the MIT License.
"""
DS EV Enhancer — Orchestrator for AILEE Automotive HP & EV Control Governance.

Calculates Mechanical and Electrical Horse Power, checks consistency, evaluates
signals against the AILEE Automotive trust pipeline, and writes JSON audit logs.
"""

from __future__ import annotations

import json
import os
import sys
from datetime import datetime, timezone
from typing import Any, Dict, Optional, Tuple

# Ensure local ailee package is importable
_script_dir = os.path.dirname(os.path.abspath(__file__))
if _script_dir not in sys.path:
    sys.path.insert(0, _script_dir)

from ailee.core_min import AileeTrustPipeline, GovernanceDecision, _validated_signals, _rejected_decision
from ailee.domains.automotive.ailee_automotive_domain import AileeAutomotiveDomain


def compute_mechanical_hp(torque_nm: float, rpm: float) -> float:
    """
    Computes Mechanical Horse Power from torque (Nm) and rotational speed (RPM).
    Formula: HP_mech = (Torque_Nm * RPM) / 7121.23
    """
    if torque_nm < 0 or rpm < 0:
        return 0.0
    return (float(torque_nm) * float(rpm)) / 7121.23


def compute_electrical_hp(v_batt: float, i_batt: float) -> float:
    """
    Computes Electrical Horse Power from battery voltage (Volts) and current (Amps).
    Formula: P_elec_kW = (V_batt * I_batt) / 1000.0
             HP_elec = P_elec_kW * 1.34102
    """
    if v_batt < 0 or i_batt < 0:
        return 0.0
    p_elec_kw = (float(v_batt) * float(i_batt)) / 1000.0
    return p_elec_kw * 1.34102


def hp_consistency_score(hp_mech: float, hp_elec: float) -> float:
    """
    Computes consistency score between Mechanical and Electrical Horse Power.
    Score ranges from 1.0 (perfect match) to 0.0 (inconsistent / zero input).
    """
    m = float(hp_mech)
    e = float(hp_elec)
    if m <= 0.0 or e <= 0.0:
        return 0.0
    return min(m, e) / max(m, e)


class AileeAuditLogger:
    """
    Black-box audit logger for governance decisions written to logs/ailee_automotive_audit.log
    """

    def __init__(self, log_path: Optional[str] = None):
        if log_path is None:
            repo_root = os.path.abspath(os.path.join(_script_dir, "../.."))
            log_dir = os.path.join(repo_root, "logs")
            os.makedirs(log_dir, exist_ok=True)
            log_path = os.path.join(log_dir, "ailee_automotive_audit.log")
        else:
            log_dir = os.path.dirname(log_path)
            if log_dir:
                os.makedirs(log_dir, exist_ok=True)
        self.log_path = log_path

    def log_decision(self, decision: GovernanceDecision, hp_mech: float, hp_elec: float) -> None:
        """Logs governance decision to audit log in JSON format"""
        entry = {
            "timestamp": decision.timestamp or datetime.now(timezone.utc).isoformat(),
            "level": decision.level,
            "hp_mech": round(hp_mech, 2),
            "hp_elec": round(hp_elec, 2),
            "hp_consistency_score": round(decision.hp_consistency_score, 4),
            "trust_score": round(decision.trust_score, 4),
            "governed_hp": round(decision.governed_hp, 2),
            "governed_torque": round(decision.governed_torque, 2),
            "governed_discharge_current": round(decision.governed_discharge_current, 2),
            "reason": decision.reason,
            "used_fallback": decision.used_fallback,
        }
        try:
            with open(self.log_path, "a", encoding="utf-8") as f:
                f.write(json.dumps(entry) + "\n")
        except Exception as err:
            sys.stderr.write(f"Warning: Failed to write AILEE audit log to {self.log_path}: {err}\n")


class DSEVEnhancerOrchestrator:
    """
    Main orchestrator interfacing EV signals with AILEE Automotive domain trust pipeline.
    """

    def __init__(self, log_path: Optional[str] = None):
        self.domain = AileeAutomotiveDomain()
        self.pipeline = AileeTrustPipeline(domain=self.domain)
        self.audit_logger = AileeAuditLogger(log_path)

    def evaluate_signals(
        self,
        torque_nm: float,
        rpm: float,
        v_batt: float,
        i_batt: float,
        soc: float = 100.0,
        soh: float = 100.0,
        temp_c: float = 25.0,
        sensor_valid: bool = True,
        max_torque_nm: float = 400.0,
        max_current_a: float = 500.0,
    ) -> Dict[str, Any]:
        """
        Calculates HP metrics, evaluates trust pipeline, records audit log,
        and returns dictionary suitable for Python caller or C++ bindings.
        """
        try:
            hp_mech = compute_mechanical_hp(float(torque_nm), float(rpm))
            hp_elec = compute_electrical_hp(float(v_batt), float(i_batt))
            consistency = hp_consistency_score(hp_mech, hp_elec)
            signals_dict = _validated_signals({
                "torque_nm": torque_nm,
                "rpm": rpm,
                "v_batt": v_batt,
                "i_batt": i_batt,
                "soc": soc,
                "soh": soh,
                "temp_c": temp_c,
                "hp_mech": hp_mech,
                "hp_elec": hp_elec,
                "hp_consistency_score": consistency,
                "sensor_valid": sensor_valid,
                "max_torque_nm": max_torque_nm,
                "max_current_a": max_current_a,
            })
            decision = self.pipeline.process(signals_dict)
        except (TypeError, ValueError, OverflowError) as err:
            hp_mech = hp_elec = 0.0
            decision = _rejected_decision(f"Invalid EV signal evidence: {err}")
        self.audit_logger.log_decision(decision, hp_mech, hp_elec)

        return {
            "level": decision.level,
            "governed_hp": decision.governed_hp,
            "governed_torque": decision.governed_torque,
            "governed_discharge_current": decision.governed_discharge_current,
            "trust_score": decision.trust_score,
            "hp_consistency_score": decision.hp_consistency_score,
            "hp_mech": hp_mech,
            "hp_elec": hp_elec,
            "reason": decision.reason,
            "used_fallback": decision.used_fallback,
            "timestamp": decision.timestamp,
        }


# Global singleton instance for module-level direct calls
_global_orchestrator = DSEVEnhancerOrchestrator()


def evaluate_governance(
    torque_nm: float,
    rpm: float,
    v_batt: float,
    i_batt: float,
    soc: float,
    soh: float,
    temp_c: float,
    sensor_valid: bool = True,
    max_torque_nm: float = 400.0,
    max_current_a: float = 500.0,
) -> Dict[str, Any]:
    """
    Module-level function exposed for C++ Pybind11 / ctypes bridge.
    """
    return _global_orchestrator.evaluate_signals(
        torque_nm=torque_nm,
        rpm=rpm,
        v_batt=v_batt,
        i_batt=i_batt,
        soc=soc,
        soh=soh,
        temp_c=temp_c,
        sensor_valid=sensor_valid,
        max_torque_nm=max_torque_nm,
        max_current_a=max_current_a,
    )


if __name__ == "__main__":
    print("Testing ds_ev_enhancer orchestrator...")
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
    print("Governance Decision Result:")
    for k, v in res.items():
        print(f"  {k}: {v}")
