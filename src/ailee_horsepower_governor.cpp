/*
 * ============================================================================
 * AILEE TRUST LAYER AUTOMOTIVE DOMAIN HORSEPOWER GOVERNOR IMPLEMENTATION
 * ============================================================================
 *
 * LICENSE: Copyright (c) Don Michael Feeney Jr. Licensed under the MIT License.
 * ============================================================================
 */

#include "ailee_horsepower_governor.hpp"

namespace {
bool validSignals(const RawSignals& signals) {
    return signals.ctx.sensor_valid && std::isfinite(signals.torque_nm) &&
        signals.torque_nm >= 0.0 && std::isfinite(signals.rpm) && signals.rpm >= 0.0 &&
        std::isfinite(signals.v_batt) && signals.v_batt > 0.0 &&
        std::isfinite(signals.i_batt) && signals.i_batt >= 0.0 &&
        std::isfinite(signals.ctx.soc) && signals.ctx.soc >= 0.0 && signals.ctx.soc <= 100.0 &&
        std::isfinite(signals.ctx.soh) && signals.ctx.soh >= 0.0 && signals.ctx.soh <= 100.0 &&
        std::isfinite(signals.ctx.temp_c) &&
        std::isfinite(AileeHorsepowerGovernor::computeMechanicalHp(signals.torque_nm, signals.rpm)) &&
        std::isfinite(AileeHorsepowerGovernor::computeElectricalHp(signals.v_batt, signals.i_batt));
}

GovernanceDecisionCpp rejectedDecision(const std::string& reason) {
    GovernanceDecisionCpp decision;
    decision.level = 3;
    decision.trust_score = 0.0;
    decision.hp_consistency_score = 0.0;
    decision.used_fallback = true;
    decision.reason = reason;
    return decision;
}
}

#ifdef DS_ENABLE_PYTHON_GOVERNOR
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
#include <pybind11/embed.h>
#include <pybind11/pybind11.h>
#pragma GCC diagnostic pop

#include <iostream>
#include <algorithm>
#include <cmath>

namespace py = pybind11;
#endif

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
struct AileeHorsepowerGovernor::Impl {
#ifdef DS_ENABLE_PYTHON_GOVERNOR
    bool py_initialized = false;
    py::object py_module;
    py::object py_eval_func;

    Impl() {
        try {
            if (!Py_IsInitialized()) {
                py::initialize_interpreter();
            }
            py::module_ sys = py::module_::import("sys");
            py::list path = sys.attr("path");

            // Add ds_core/python to sys.path if not present
            path.append("ds_core/python");
            path.append("../ds_core/python");
            path.append("../../ds_core/python");

            py_module = py::module_::import("ds_ev_enhancer");
            py_eval_func = py_module.attr("evaluate_governance");
            py_initialized = true;
        } catch (const std::exception& e) {
            py_initialized = false;
        }
    }
#else
    bool py_initialized = false;
    Impl() = default;
#endif

    GovernanceDecisionCpp fallbackEvaluate(const RawSignals& signals) const {
        if (!validSignals(signals)) {
            return rejectedDecision("C++ Fallback Level 3: Sensor validity or numeric-domain check failed.");
        }
        GovernanceDecisionCpp dec;
        dec.hp_mech = AileeHorsepowerGovernor::computeMechanicalHp(signals.torque_nm, signals.rpm);
        dec.hp_elec = AileeHorsepowerGovernor::computeElectricalHp(signals.v_batt, signals.i_batt);
        dec.hp_consistency_score = AileeHorsepowerGovernor::hpConsistencyScore(dec.hp_mech, dec.hp_elec);

        double max_hp = std::max({dec.hp_mech, dec.hp_elec, 1.0});
        double max_torque = signals.torque_nm > 0.0 ? signals.torque_nm : 400.0;
        double max_current = signals.i_batt > 0.0 ? signals.i_batt : 500.0;

        double trust = 1.0;
        if (dec.hp_consistency_score < 0.85) {
            trust -= (0.85 - dec.hp_consistency_score) * 1.5;
        }
        if (signals.ctx.temp_c > 45.0) {
            trust -= (signals.ctx.temp_c - 45.0) * 0.025;
        }
        if (signals.ctx.soc < 20.0) {
            trust -= (20.0 - signals.ctx.soc) * 0.02;
        }
        if (signals.ctx.soh < 85.0) {
            trust -= (85.0 - signals.ctx.soh) * 0.015;
        }
        trust = std::clamp(trust, 0.0, 1.0);
        dec.trust_score = trust;

        if (signals.ctx.temp_c >= 60.0 || signals.ctx.soc <= 5.0 || trust < 0.50) {
            dec.level = 3;
            dec.governed_hp = max_hp * 0.25;
            dec.governed_torque = max_torque * 0.25;
            dec.governed_discharge_current = max_current * 0.25;
            dec.reason = "C++ Fallback Level 3 Protective Mode: Thermal/battery critical limits reached.";
            dec.used_fallback = true;
        } else if (signals.ctx.temp_c >= 50.0 || signals.ctx.soc <= 15.0 || trust < 0.70) {
            dec.level = 2;
            dec.governed_hp = max_hp * 0.65;
            dec.governed_torque = max_torque * 0.65;
            dec.governed_discharge_current = max_current * 0.65;
            dec.reason = "C++ Fallback Level 2 Hard Ceiling applied (65%).";
        } else if (signals.ctx.temp_c >= 42.0 || signals.ctx.soc <= 25.0 || trust < 0.88 || dec.hp_consistency_score < 0.90) {
            dec.level = 1;
            dec.governed_hp = max_hp * 0.90;
            dec.governed_torque = max_torque * 0.90;
            dec.governed_discharge_current = max_current * 0.90;
            dec.reason = "C++ Fallback Level 1 Soft Ceiling applied (90%).";
        } else {
            dec.level = 0;
            dec.governed_hp = max_hp;
            dec.governed_torque = max_torque;
            dec.governed_discharge_current = max_current;
            dec.reason = "C++ Fallback Level 0 Normal operation.";
        }

        return dec;
    }
};
#pragma GCC diagnostic pop

AileeHorsepowerGovernor::AileeHorsepowerGovernor()
    : impl_(std::make_unique<Impl>()) {}

AileeHorsepowerGovernor::~AileeHorsepowerGovernor() = default;

double AileeHorsepowerGovernor::computeMechanicalHp(double torque_nm, double rpm) {
    if (torque_nm < 0.0 || rpm < 0.0) return 0.0;
    return (torque_nm * rpm) / 7121.23;
}

double AileeHorsepowerGovernor::computeElectricalHp(double v_batt, double i_batt) {
    if (v_batt < 0.0 || i_batt < 0.0) return 0.0;
    double p_elec_kw = (v_batt * i_batt) / 1000.0;
    return p_elec_kw * 1.34102;
}

double AileeHorsepowerGovernor::hpConsistencyScore(double hp_mech, double hp_elec) {
    if (hp_mech <= 0.0 || hp_elec <= 0.0) return 0.0;
    return std::min(hp_mech, hp_elec) / std::max(hp_mech, hp_elec);
}

double AileeHorsepowerGovernor::governHorsepower(double raw_hp_mech, double raw_hp_elec, const GovernanceContext& ctx) {
    RawSignals signals;
    signals.torque_nm = (raw_hp_mech * 7121.23) / 4000.0; // Assume 4000 RPM base
    signals.rpm = 4000.0;
    signals.v_batt = 400.0;
    signals.i_batt = ((raw_hp_elec / 1.34102) * 1000.0) / 400.0;
    signals.ctx = ctx;

    GovernanceDecisionCpp dec = evaluate(signals);
    return dec.governed_hp;
}

GovernanceDecisionCpp AileeHorsepowerGovernor::evaluate(const RawSignals& signals) {
    if (!validSignals(signals)) {
        return impl_->fallbackEvaluate(signals);
    }
    if (!impl_->py_initialized) {
        return impl_->fallbackEvaluate(signals);
    }

#ifdef DS_ENABLE_PYTHON_GOVERNOR
    try {
        py::dict py_res = impl_->py_eval_func(
            signals.torque_nm,
            signals.rpm,
            signals.v_batt,
            signals.i_batt,
            signals.ctx.soc,
            signals.ctx.soh,
            signals.ctx.temp_c,
            signals.ctx.sensor_valid,
            signals.torque_nm > 0.0 ? signals.torque_nm : 400.0,
            signals.i_batt > 0.0 ? signals.i_batt : 500.0
        );

        for (const char* key : {"level", "governed_hp", "governed_torque",
                               "governed_discharge_current", "trust_score",
                               "hp_consistency_score", "hp_mech", "hp_elec"}) {
            if (py::isinstance<py::bool_>(py_res[key])) {
                return rejectedDecision("Level 3: Python governor returned boolean numeric evidence.");
            }
        }
        GovernanceDecisionCpp dec;
        dec.level = py_res["level"].cast<int>();
        dec.governed_hp = py_res["governed_hp"].cast<double>();
        dec.governed_torque = py_res["governed_torque"].cast<double>();
        dec.governed_discharge_current = py_res["governed_discharge_current"].cast<double>();
        dec.trust_score = py_res["trust_score"].cast<double>();
        dec.hp_consistency_score = py_res["hp_consistency_score"].cast<double>();
        dec.hp_mech = py_res["hp_mech"].cast<double>();
        dec.hp_elec = py_res["hp_elec"].cast<double>();
        dec.reason = py_res["reason"].cast<std::string>();
        dec.used_fallback = py_res["used_fallback"].cast<bool>();

        const bool valid_decision = dec.level >= 0 && dec.level <= 3 &&
            std::isfinite(dec.governed_hp) && dec.governed_hp >= 0.0 &&
            std::isfinite(dec.governed_torque) && dec.governed_torque >= 0.0 &&
            std::isfinite(dec.governed_discharge_current) && dec.governed_discharge_current >= 0.0 &&
            std::isfinite(dec.trust_score) && dec.trust_score >= 0.0 && dec.trust_score <= 1.0 &&
            std::isfinite(dec.hp_consistency_score) && dec.hp_consistency_score >= 0.0 &&
            dec.hp_consistency_score <= 1.0 && std::isfinite(dec.hp_mech) && dec.hp_mech >= 0.0 &&
            std::isfinite(dec.hp_elec) && dec.hp_elec >= 0.0;
        if (!valid_decision) {
            return rejectedDecision("Level 3: Python governor returned an invalid decision.");
        }

        return dec;
    } catch (const std::exception& e) {
        return rejectedDecision("Level 3: Python governor evaluation failed.");
    }
#else
    return impl_->fallbackEvaluate(signals);
#endif
}
