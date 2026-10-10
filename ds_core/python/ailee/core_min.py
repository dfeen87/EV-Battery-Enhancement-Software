# Copyright (c) Don Michael Feeney Jr.
# Licensed under the MIT License.
"""
AILEE Core Minimal - Self-contained trust pipeline orchestration, domain registration,
and governance structures for automotive HP and EV control.
"""

from __future__ import annotations

import time
import math
from dataclasses import dataclass, field
from datetime import datetime, timezone
from enum import Enum, IntEnum
from typing import Any, Dict, List, Optional, Tuple


class GovernanceLevel(IntEnum):
    """Governance levels (0..3) for HP and dynamic vehicle control constraints"""
    LEVEL_0_NORMAL = 0
    LEVEL_1_SOFT_CEILING = 1
    LEVEL_2_HARD_CEILING = 2
    LEVEL_3_PROTECTIVE = 3


# Alias for backward/domain compatibility
AutonomyLevel = GovernanceLevel


class SafetyStatus(str, Enum):
    ACCEPTED = "ACCEPTED"
    BORDERLINE = "BORDERLINE"
    REJECTED = "REJECTED"
    FALLBACK = "FALLBACK"


class GraceStatus(str, Enum):
    PASSED = "PASSED"
    FAILED = "FAILED"
    NOT_APPLIED = "NOT_APPLIED"


class ConsensusStatus(str, Enum):
    AGREED = "AGREED"
    DISAGREED = "DISAGREED"
    NO_QUORUM = "NO_QUORUM"


@dataclass
class AileeConfig:
    """Configuration options for AILEE Trust Pipeline"""
    accept_threshold: float = 0.85
    borderline_low: float = 0.65
    borderline_high: float = 0.85

    w_stability: float = 0.50
    w_agreement: float = 0.30
    w_likelihood: float = 0.20

    history_window: int = 50
    forecast_window: int = 10

    grace_peer_delta: float = 1.0
    grace_min_peer_agreement_ratio: float = 0.60
    grace_forecast_epsilon: float = 0.30
    grace_max_abs_z: float = 3.0

    consensus_quorum: int = 3
    consensus_delta: float = 1.0
    consensus_pass_ratio: float = 0.60

    fallback_mode: str = "last_good"
    fallback_clamp_min: float = 0.0
    fallback_clamp_max: float = 3.0

    hard_min: float = 0.0
    hard_max: float = 3.0

    enable_grace: bool = True
    enable_consensus: bool = True
    enable_audit_metadata: bool = True


@dataclass
class DecisionResult:
    """Structure representing lower-level AILEE pipeline outputs"""
    value: float
    confidence_score: float = 1.0
    safety_status: SafetyStatus = SafetyStatus.ACCEPTED
    grace_status: GraceStatus = GraceStatus.PASSED
    consensus_status: ConsensusStatus = ConsensusStatus.AGREED
    used_fallback: bool = False
    reasons: List[str] = field(default_factory=list)
    metadata: Dict[str, Any] = field(default_factory=dict)


@dataclass
class GovernanceDecision:
    """
    Primary governance decision structure governing horsepower, torque,
    and discharge current limits.
    """
    level: int  # 0..3
    governed_hp: float
    governed_torque: float
    governed_discharge_current: float
    trust_score: float
    hp_consistency_score: float
    reason: str
    timestamp: str = field(default_factory=lambda: datetime.now(timezone.utc).isoformat())
    used_fallback: bool = False
    reasons: List[str] = field(default_factory=list)
    metadata: Dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> Dict[str, Any]:
        return {
            "timestamp": self.timestamp,
            "level": self.level,
            "governed_hp": self.governed_hp,
            "governed_torque": self.governed_torque,
            "governed_discharge_current": self.governed_discharge_current,
            "trust_score": self.trust_score,
            "hp_consistency_score": self.hp_consistency_score,
            "reason": self.reason,
            "used_fallback": self.used_fallback,
            "reasons": self.reasons,
        }


def _validated_signals(signals: Dict[str, Any]) -> Dict[str, Any]:
    """Validate evidence before comparisons, arithmetic, or fallback authorization."""
    if not isinstance(signals, dict) or signals.get("sensor_valid") is not True:
        raise ValueError("Missing or invalid sensor validity evidence")
    normalized = dict(signals)
    required = ("hp_mech", "hp_elec", "hp_consistency_score", "soc", "soh", "temp_c")
    optional = {"max_torque_nm": 400.0, "max_current_a": 500.0}
    numeric_keys = required + tuple(optional) + tuple(
        key for key in ("torque_nm", "rpm", "v_batt", "i_batt") if key in signals
    )
    for key in numeric_keys:
        value = signals.get(key, optional.get(key))
        if isinstance(value, bool):
            raise ValueError(f"Invalid numeric evidence: {key}")
        try:
            value = float(value)
        except (TypeError, ValueError, OverflowError) as err:
            raise ValueError(f"Missing or invalid numeric evidence: {key}") from err
        if not math.isfinite(value):
            raise ValueError(f"Nonfinite numeric evidence: {key}")
        normalized[key] = value
    for key in ("hp_mech", "hp_elec", "max_torque_nm", "max_current_a",
                "torque_nm", "rpm", "i_batt"):
        if key in normalized and normalized[key] < 0.0:
            raise ValueError(f"Negative numeric evidence: {key}")
    for key, upper in (("soc", 100.0), ("soh", 100.0), ("hp_consistency_score", 1.0)):
        if not 0.0 <= normalized[key] <= upper:
            raise ValueError(f"Out-of-domain numeric evidence: {key}")
    if "v_batt" in normalized and normalized["v_batt"] <= 0.0:
        raise ValueError("Nonpositive voltage evidence")
    return normalized


def _rejected_decision(reason: str, timestamp: Optional[str] = None) -> GovernanceDecision:
    """Malformed evidence authorizes no output, independent of the percentage policy."""
    return GovernanceDecision(
        level=int(GovernanceLevel.LEVEL_3_PROTECTIVE),
        governed_hp=0.0, governed_torque=0.0, governed_discharge_current=0.0,
        trust_score=0.0, hp_consistency_score=0.0,
        reason=f"PROTECTIVE MODE ACTIVATED (Level 3 Rejection): {reason}",
        timestamp=timestamp or datetime.now(timezone.utc).isoformat(),
        used_fallback=True, reasons=[reason],
    )


class DomainRegistry:
    """Registry for AILEE governance domains"""
    _domains: Dict[str, Any] = {}

    @classmethod
    def register(cls, name: str, domain_cls: Any) -> None:
        cls._domains[name] = domain_cls

    @classmethod
    def get(cls, name: str) -> Optional[Any]:
        return cls._domains.get(name)


class AileeTrustPipeline:
    """
    Minimal AILEE Trust Pipeline orchestrator for governance evaluation and fallback handling.
    """

    def __init__(self, domain: Optional[Any] = None, cfg: Optional[AileeConfig] = None):
        if isinstance(domain, AileeConfig) and cfg is None:
            cfg = domain
            domain = None
        self.cfg = cfg or AileeConfig()
        self.domain = domain
        self._history: List[float] = []

    def process_raw(
        self,
        raw_value: float,
        raw_confidence: float = 1.0,
        peer_values: Optional[List[float]] = None,
        timestamp: Optional[float] = None,
        context: Optional[Dict[str, Any]] = None,
    ) -> DecisionResult:
        """Low level pipeline evaluation matching AILEE core interface"""
        timestamp = timestamp or time.time()
        peer_values = peer_values or []

        reasons = []
        used_fallback = False
        confidence = float(raw_confidence)

        if confidence < self.cfg.borderline_low:
            used_fallback = True
            reasons.append(f"Confidence {confidence:.2f} below minimum threshold {self.cfg.borderline_low:.2f}")
            status = SafetyStatus.FALLBACK
            val = float(self.cfg.fallback_clamp_min)
        elif confidence < self.cfg.accept_threshold:
            status = SafetyStatus.BORDERLINE
            reasons.append(f"Confidence {confidence:.2f} borderline")
            val = min(float(raw_value), self.cfg.fallback_clamp_max)
        else:
            status = SafetyStatus.ACCEPTED
            val = float(raw_value)

        val = max(self.cfg.hard_min, min(self.cfg.hard_max, val))
        self._history.append(val)
        if len(self._history) > self.cfg.history_window:
            self._history.pop(0)

        return DecisionResult(
            value=val,
            confidence_score=confidence,
            safety_status=status,
            used_fallback=used_fallback,
            reasons=reasons,
            metadata=context or {},
        )

    def process(self, signals: Dict[str, Any]) -> GovernanceDecision:
        """
        Orchestrate governance evaluation over input signals dict.
        Applies the configured percentage fallback for valid low-trust evidence;
        malformed evidence or domain failures receive zero authorization.
        """
        now_iso = datetime.now(timezone.utc).isoformat()

        try:
            signals = _validated_signals(signals)
            if self.domain and hasattr(self.domain, "evaluate_signals"):
                decision = self.domain.evaluate_signals(signals)
                if isinstance(decision, GovernanceDecision):
                    valid_limits = all(
                        not isinstance(value, bool) and math.isfinite(value) and value >= 0.0 for value in (
                            decision.governed_hp, decision.governed_torque,
                            decision.governed_discharge_current,
                        )
                    )
                    valid_scores = all(
                        not isinstance(value, bool) and math.isfinite(value) and 0.0 <= value <= 1.0 for value in (
                            decision.trust_score, decision.hp_consistency_score,
                        )
                    )
                    if (not isinstance(decision.level, int) or isinstance(decision.level, bool)
                            or not 0 <= decision.level <= 3 or not valid_limits or not valid_scores):
                        return _rejected_decision("Domain returned an invalid governance decision", now_iso)
                    if (decision.level == 3 and decision.used_fallback and decision.trust_score == 0.0
                            and decision.governed_hp == 0.0 and decision.governed_torque == 0.0
                            and decision.governed_discharge_current == 0.0):
                        return decision
                    if decision.trust_score < self.cfg.borderline_low:
                        return self._create_fallback_decision(
                            signals,
                            f"Trust score {decision.trust_score:.2f} below threshold {self.cfg.borderline_low:.2f}",
                            now_iso,
                        )
                    return decision
                return _rejected_decision("Domain returned no governance decision", now_iso)

            # Fallback inline evaluation logic if no explicit domain handler
            hp_mech = float(signals.get("hp_mech", 0.0))
            hp_elec = float(signals.get("hp_elec", 0.0))
            consistency = float(signals.get("hp_consistency_score", 1.0))
            soc = float(signals.get("soc", 100.0))
            soh = float(signals.get("soh", 100.0))
            temp_c = float(signals.get("temp_c", 25.0))
            sensor_valid = bool(signals.get("sensor_valid", True))

            if not sensor_valid:
                return self._create_fallback_decision(signals, "Sensor validity check failed", now_iso)

            # Compute trust score
            trust = 1.0
            reasons = []

            if consistency < 0.8:
                trust -= (0.8 - consistency) * 1.5
                reasons.append(f"HP consistency mismatch (score={consistency:.2f})")

            if temp_c > 55.0:
                trust -= (temp_c - 55.0) * 0.03
                reasons.append(f"High battery temperature ({temp_c:.1f}°C)")

            if soc < 15.0:
                trust -= (15.0 - soc) * 0.02
                reasons.append(f"Low battery SOC ({soc:.1f}%)")

            if soh < 80.0:
                trust -= (80.0 - soh) * 0.015
                reasons.append(f"Degraded battery SOH ({soh:.1f}%)")

            trust = max(0.0, min(1.0, trust))

            if trust < self.cfg.borderline_low:
                return self._create_fallback_decision(
                    signals,
                    f"Computed trust score ({trust:.2f}) below threshold ({self.cfg.borderline_low:.2f}): "
                    + "; ".join(reasons),
                    now_iso,
                )

            # Determine governance level
            max_hp = max(hp_mech, hp_elec, 1.0)
            max_torque = float(signals.get("max_torque_nm", 400.0))
            max_current = float(signals.get("max_current_a", 500.0))

            if trust >= 0.90 and temp_c <= 45.0 and soc >= 20.0:
                level = GovernanceLevel.LEVEL_0_NORMAL
                governed_hp = max_hp
                governed_torque = max_torque
                governed_current = max_current
                reason_str = "Normal operation; full performance envelope allowed."
            elif trust >= 0.75 and temp_c <= 52.0 and soc >= 10.0:
                level = GovernanceLevel.LEVEL_1_SOFT_CEILING
                governed_hp = max_hp * 0.90
                governed_torque = max_torque * 0.90
                governed_current = max_current * 0.90
                reason_str = "Soft ceiling applied (90% envelope) due to minor warning indicators."
            elif trust >= 0.60:
                level = GovernanceLevel.LEVEL_2_HARD_CEILING
                governed_hp = max_hp * 0.65
                governed_torque = max_torque * 0.65
                governed_current = max_current * 0.65
                reason_str = "Hard ceiling applied (65% envelope) due to elevated system stress."
            else:
                return self._create_fallback_decision(signals, "Trust score insufficient for normal modes", now_iso)

            return GovernanceDecision(
                level=int(level),
                governed_hp=governed_hp,
                governed_torque=governed_torque,
                governed_discharge_current=governed_current,
                trust_score=trust,
                hp_consistency_score=consistency,
                reason=reason_str,
                timestamp=now_iso,
                used_fallback=False,
                reasons=reasons,
            )

        except Exception as err:
            return _rejected_decision(f"Trust pipeline error: {str(err)}", now_iso)

    def _create_fallback_decision(self, signals: Dict[str, Any], reason: str, timestamp: str) -> GovernanceDecision:
        try:
            signals = _validated_signals(signals)
        except (TypeError, ValueError, OverflowError) as err:
            return _rejected_decision(f"Invalid fallback evidence: {err}", timestamp)
        max_hp = max(float(signals.get("hp_mech", 0.0)), float(signals.get("hp_elec", 0.0)), 100.0)
        max_torque = float(signals.get("max_torque_nm", 400.0))
        max_current = float(signals.get("max_current_a", 500.0))

        return GovernanceDecision(
            level=int(GovernanceLevel.LEVEL_3_PROTECTIVE),
            governed_hp=max_hp * 0.25,
            governed_torque=max_torque * 0.25,
            governed_discharge_current=max_current * 0.25,
            trust_score=0.0,
            hp_consistency_score=float(signals.get("hp_consistency_score", 0.0)),
            reason=f"PROTECTIVE MODE ACTIVATED (Level 3 Fallback): {reason}",
            timestamp=timestamp,
            used_fallback=True,
            reasons=[reason],
        )
