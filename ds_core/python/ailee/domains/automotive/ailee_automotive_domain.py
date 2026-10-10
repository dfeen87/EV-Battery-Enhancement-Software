# Copyright (c) Don Michael Feeney Jr.
# Licensed under the MIT License.
"""
AILEE Trust Layer — Automotive Domain (Vendored & EV HP Adapted)
Version: 4.7.0 - Production Grade

Governance domain for automotive decision integrity and HP dynamic limits.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import IntEnum, Enum
from typing import Any, Dict, List, Optional, Tuple
import time
import statistics

# ---- Core imports ----
try:
    from ...core_min import (
        AileeTrustPipeline,
        AileeConfig,
        DecisionResult,
        SafetyStatus,
        GovernanceLevel,
        GovernanceDecision,
        _validated_signals,
        _rejected_decision,
    )
except ImportError:
    try:
        from ailee.core_min import (
            AileeTrustPipeline,
            AileeConfig,
            DecisionResult,
            SafetyStatus,
            GovernanceLevel,
            GovernanceDecision,
            _validated_signals,
            _rejected_decision,
        )
    except ImportError:
        AileeTrustPipeline = None
        AileeConfig = None
        DecisionResult = None
        SafetyStatus = None
        GovernanceLevel = None
        GovernanceDecision = None


# -----------------------------
# Governance / Autonomy Levels
# -----------------------------

class AutonomyLevel(IntEnum):
    """Discrete governance / autonomy levels (0..3)"""
    MANUAL_ONLY = 0            # Level 0 in autonomy, or Normal / Base Level
    ASSISTED_ONLY = 1          # Level 1: Soft ceiling
    CONSTRAINED_AUTONOMY = 2   # Level 2: Hard ceiling
    FULL_AUTONOMY_ALLOWED = 3  # Level 3: Protective/Emergency or full depending on context


def _clamp_int(x: int, lo: int, hi: int) -> int:
    return lo if x < lo else hi if x > hi else x


def _level_from_float(x: float) -> AutonomyLevel:
    """Quantize to nearest integer level; enforce 0..3"""
    ix = int(round(float(x)))
    ix = _clamp_int(ix, int(AutonomyLevel.MANUAL_ONLY), int(AutonomyLevel.FULL_AUTONOMY_ALLOWED))
    return AutonomyLevel(ix)


# -----------------------------
# Safety Monitor Signals
# -----------------------------

@dataclass(frozen=True)
class SafetyMonitorSignals:
    """
    External safety monitor inputs (e.g., ISO 26262 monitors).
    """
    collision_risk_score: Optional[float] = None  # 0..1, higher = more risk
    path_safety_score: Optional[float] = None     # 0..1, higher = safer
    sensor_fusion_health: Optional[float] = None  # 0..1, health metric
    localization_uncertainty_m: Optional[float] = None  # meters, position uncertainty
    object_detection_health: Optional[float] = None  # 0..1
    emergency_brake_available: bool = True
    redundant_systems_online: bool = True

    def is_safe_for_level(self, level: AutonomyLevel) -> Tuple[bool, List[str]]:
        """Domain-specific safety thresholds per level"""
        issues: List[str] = []

        if not self.emergency_brake_available:
            issues.append("emergency_brake_unavailable")

        if level >= AutonomyLevel.ASSISTED_ONLY:
            if self.sensor_fusion_health is not None and self.sensor_fusion_health < 0.80:
                issues.append(f"sensor_fusion_health={self.sensor_fusion_health:.2f} below 0.80")

        if level >= AutonomyLevel.CONSTRAINED_AUTONOMY:
            if self.collision_risk_score is not None and self.collision_risk_score > 0.30:
                issues.append(f"collision_risk={self.collision_risk_score:.2f} exceeds 0.30")

            if self.path_safety_score is not None and self.path_safety_score < 0.70:
                issues.append(f"path_safety={self.path_safety_score:.2f} below 0.70")

            if self.localization_uncertainty_m is not None and self.localization_uncertainty_m > 2.0:
                issues.append(f"localization_uncertainty={self.localization_uncertainty_m:.1f}m exceeds 2.0m")

            if self.object_detection_health is not None and self.object_detection_health < 0.85:
                issues.append(f"object_detection_health={self.object_detection_health:.2f} below 0.85")

        if level >= AutonomyLevel.FULL_AUTONOMY_ALLOWED:
            if not self.redundant_systems_online:
                issues.append("redundant_systems_offline")

            if self.collision_risk_score is not None and self.collision_risk_score > 0.10:
                issues.append(f"collision_risk={self.collision_risk_score:.2f} exceeds 0.10")

            if self.localization_uncertainty_m is not None and self.localization_uncertainty_m > 0.5:
                issues.append(f"localization_uncertainty={self.localization_uncertainty_m:.1f}m exceeds 0.5m")

        return len(issues) == 0, issues


# -----------------------------
# Operational Design Domain
# -----------------------------

@dataclass(frozen=True)
class OperationalDesignDomain:
    """ODD constraints"""
    geofence_authorized: bool = True
    hd_map_available: bool = True
    distance_to_boundary_m: Optional[float] = None
    weather_code: str = "clear"
    weather_trend: str = "stable"
    visibility_m: Optional[float] = None
    road_type: str = "highway"
    road_surface: str = "dry"
    construction_zone: bool = False
    time_of_day: str = "day"
    traffic_density: str = "moderate"

    def max_safe_level(self) -> AutonomyLevel:
        if not self.geofence_authorized:
            return AutonomyLevel.MANUAL_ONLY

        if not self.hd_map_available and self.road_type in ("highway", "urban"):
            return AutonomyLevel.ASSISTED_ONLY

        if self.weather_code in ("heavy_rain", "snow", "fog", "ice"):
            return AutonomyLevel.ASSISTED_ONLY

        if self.road_surface == "ice":
            return AutonomyLevel.MANUAL_ONLY

        if self.road_surface == "snow":
            return AutonomyLevel.ASSISTED_ONLY

        if self.visibility_m is not None:
            if self.visibility_m < 30.0:
                return AutonomyLevel.MANUAL_ONLY
            if self.visibility_m < 100.0:
                return AutonomyLevel.ASSISTED_ONLY

        if self.construction_zone:
            return AutonomyLevel.ASSISTED_ONLY

        return AutonomyLevel.FULL_AUTONOMY_ALLOWED

    def get_warning_distance_m(self) -> float:
        if self.road_type == "highway":
            return 1000.0
        elif self.road_type == "urban":
            return 300.0
        return 500.0


# -----------------------------
# Driver State
# -----------------------------

@dataclass(frozen=True)
class DriverState:
    readiness: float  # 0..1
    attention_level: Optional[float] = None
    distraction_detected: bool = False
    drowsiness_detected: bool = False
    hands_on_wheel: Optional[bool] = None
    eyes_on_road: Optional[bool] = None
    response_time_ms: Optional[float] = None
    last_manual_input_ts: Optional[float] = None

    def is_ready_for_handoff(self, current_ts: float) -> Tuple[bool, str]:
        if self.readiness < 0.70:
            return False, f"readiness={self.readiness:.2f} below 0.70"
        if self.attention_level is not None and self.attention_level < 0.50:
            return False, f"attention={self.attention_level:.2f} below 0.50"
        if self.distraction_detected:
            return False, "distraction_detected"
        if self.drowsiness_detected:
            return False, "drowsiness_detected"
        return True, "driver_ready"


# -----------------------------
# System Health
# -----------------------------

@dataclass(frozen=True)
class SystemHealth:
    latency_ms: Optional[float] = None
    sensor_faults: int = 0
    compute_load: Optional[float] = None
    memory_available_mb: Optional[float] = None
    gpu_temperature_c: Optional[float] = None
    can_bus_errors: int = 0


# -----------------------------
# Domain Inputs
# -----------------------------

@dataclass(frozen=True)
class AutonomySignals:
    proposed_level: AutonomyLevel
    model_confidence: float  # 0..1
    peer_recommended_levels: Tuple[AutonomyLevel, ...] = ()
    safety_monitors: Optional[SafetyMonitorSignals] = None
    odd: Optional[OperationalDesignDomain] = None
    driver_state: Optional[DriverState] = None
    system_health: Optional[SystemHealth] = None
    environment: Dict[str, Any] = field(default_factory=dict)
    current_scenario: Optional[str] = None
    timestamp: Optional[float] = None


# -----------------------------
# Domain Policy & Governor
# -----------------------------

@dataclass(frozen=True)
class AutonomyGovernancePolicy:
    min_driver_readiness_for_autonomy: float = 0.60
    min_driver_readiness_for_full_autonomy: float = 0.75
    max_latency_ms_for_autonomy: float = 150.0
    max_latency_ms_for_full_autonomy: float = 100.0
    max_sensor_faults: int = 0
    max_can_bus_errors: int = 5
    min_seconds_between_escalations: float = 10.0
    min_seconds_between_downgrades: float = 0.0
    max_allowed_level: AutonomyLevel = AutonomyLevel.FULL_AUTONOMY_ALLOWED


class AileeAutomotiveDomain:
    """
    Automotive Domain integration class bridging EV horsepower governance and AILEE trust checks.
    """

    def __init__(self, policy: Optional[AutonomyGovernancePolicy] = None):
        self.policy = policy or AutonomyGovernancePolicy()

    def evaluate_signals(self, signals: Dict[str, Any]) -> GovernanceDecision:
        """
        Evaluate raw EV signals and produce a GovernanceDecision object.
        """
        try:
            signals = _validated_signals(signals)
        except (TypeError, ValueError, OverflowError) as err:
            return _rejected_decision(f"Invalid automotive governance evidence: {err}")
        hp_mech = float(signals.get("hp_mech", 0.0))
        hp_elec = float(signals.get("hp_elec", 0.0))
        consistency = float(signals.get("hp_consistency_score", 1.0))
        soc = float(signals.get("soc", 100.0))
        soh = float(signals.get("soh", 100.0))
        temp_c = float(signals.get("temp_c", 25.0))
        max_hp = max(hp_mech, hp_elec, 1.0)
        max_torque = float(signals.get("max_torque_nm", 400.0))
        max_current = float(signals.get("max_current_a", 500.0))

        reasons = []

        # Base trust score from signals
        trust = 1.0
        if consistency < 0.85:
            penalty = (0.85 - consistency) * 1.5
            trust -= penalty
            reasons.append(f"HP consistency mismatch (score={consistency:.2f})")

        if temp_c > 45.0:
            penalty = (temp_c - 45.0) * 0.025
            trust -= penalty
            reasons.append(f"Elevated temperature ({temp_c:.1f}°C)")

        if soc < 20.0:
            penalty = (20.0 - soc) * 0.02
            trust -= penalty
            reasons.append(f"Low SOC ({soc:.1f}%)")

        if soh < 85.0:
            penalty = (85.0 - soh) * 0.015
            trust -= penalty
            reasons.append(f"Reduced battery health SOH ({soh:.1f}%)")

        trust = max(0.0, min(1.0, trust))

        # Determine level and limits
        if temp_c >= 60.0 or soc <= 5.0 or trust < 0.50:
            level = 3
            derate_factor = 0.25
            reason = "Level 3 Protective Mode: Critical thermal/battery stress or low trust score."
        elif temp_c >= 50.0 or soc <= 15.0 or trust < 0.70:
            level = 2
            derate_factor = 0.65
            reason = f"Level 2 Hard Ceiling: Hard derating applied ({derate_factor*100:.0f}% HP)."
        elif temp_c >= 42.0 or soc <= 25.0 or trust < 0.88 or consistency < 0.90:
            level = 1
            derate_factor = 0.90
            reason = f"Level 1 Soft Ceiling: Soft derating applied ({derate_factor*100:.0f}% HP)."
        else:
            level = 0
            derate_factor = 1.0
            reason = "Level 0 Normal: Full performance envelope authorized."

        if not reasons:
            reasons.append("All safety and consistency checks passed successfully.")

        return GovernanceDecision(
            level=level,
            governed_hp=max_hp * derate_factor,
            governed_torque=max_torque * derate_factor,
            governed_discharge_current=max_current * derate_factor,
            trust_score=trust,
            hp_consistency_score=consistency,
            reason=reason,
            used_fallback=(level == 3),
            reasons=reasons,
        )


__all__ = [
    "AutonomyLevel",
    "SafetyMonitorSignals",
    "OperationalDesignDomain",
    "DriverState",
    "SystemHealth",
    "AutonomySignals",
    "AutonomyGovernancePolicy",
    "AileeAutomotiveDomain",
]
