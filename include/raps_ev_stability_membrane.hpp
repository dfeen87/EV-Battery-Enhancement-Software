/*
 * ============================================================================
 * RAPS EV STABILITY MEMBRANE ENGINE
 * ============================================================================
 *
 * Physics-informed stability layer adapting RAPS (Robust Aerospace & Propulsion
 * Safety / Predictive Digital Twin & Deterministic Safety Monitor) principles
 * for Electric Vehicle powertrain, battery health, torque management, and regen.
 *
 * STABILITY TARGETS:
 *   1. Voltage Sag Mitigation & Pressure Envelope Enforcement
 *   2. Discharge/Regen Current Spike Damping
 *   3. Regenerative Braking Surge Damping
 *   4. Thermal Oscillation Stabilization
 *   5. Cell Imbalance Drift Compensation
 *
 * ARCHITECTURE:
 *   - Deterministic Safety Monitor (DSM): Inviolable physical boundaries
 *   - Predictive Digital Twin (PDT): Short-horizon state trajectory forecasting
 *   - Stability Membrane: Dampens dynamic stress fluctuations & maintains coherent output
 *
 * AUTHORS: Don Michael Feeney Jr. & Jules
 * LICENSE: Copyright (c) Don Michael Feeney Jr. Licensed under the MIT License.
 * VERSION: 8.0.1
 * ============================================================================
 */

#ifndef RAPS_EV_STABILITY_MEMBRANE_HPP
#define RAPS_EV_STABILITY_MEMBRANE_HPP

#include "ds_battery_core.hpp"
#include "ds_bms_middleware_v2.hpp"
#include <algorithm>
#include <cmath>
#include <string>

namespace raps {
namespace ev {

constexpr int RAPS_EV_VERSION_MAJOR = 8;
constexpr int RAPS_EV_VERSION_MINOR = 0;
constexpr int RAPS_EV_VERSION_PATCH = 0;

inline std::string get_raps_ev_version() {
    return std::to_string(RAPS_EV_VERSION_MAJOR) + "." +
           std::to_string(RAPS_EV_VERSION_MINOR) + "." +
           std::to_string(RAPS_EV_VERSION_PATCH);
}

enum class DsmTripReason {
    NONE,
    CRITICAL_UNDERVOLTAGE_SAG,
    CRITICAL_OVERVOLTAGE_SURGE,
    CRITICAL_OVERTEMPERATURE,
    BMS_SAFETY_FAULT
};

inline const char* dsm_trip_reason_to_string(DsmTripReason reason) {
    switch (reason) {
        case DsmTripReason::CRITICAL_UNDERVOLTAGE_SAG: return "CRITICAL_UNDERVOLTAGE_SAG";
        case DsmTripReason::CRITICAL_OVERVOLTAGE_SURGE: return "CRITICAL_OVERVOLTAGE_SURGE";
        case DsmTripReason::CRITICAL_OVERTEMPERATURE: return "CRITICAL_OVERTEMPERATURE";
        case DsmTripReason::BMS_SAFETY_FAULT: return "BMS_SAFETY_FAULT";
        default: return "NONE";
    }
}

struct alignas(64) StabilityConfig {
    // Voltage Sag Protection
    double min_voltage_threshold_v = 320.0;
    double max_voltage_threshold_v = 450.0;
    double voltage_sag_warning_v = 340.0;
    double voltage_sag_damping_factor = 0.5;

    // Current Spike Protection
    double max_discharge_current_a = 600.0;
    double max_charge_current_a = 250.0;
    double current_spike_alpha = 0.15; // Low-pass filter smoothing coefficient

    // Regen Surge Protection
    double max_regen_surge_rate_a_per_s = 1500.0;
    double regen_surge_damping_gain = 0.8;

    // Thermal Oscillation Damping
    double temp_soft_limit_c = 45.0;
    double temp_hard_limit_c = 60.0;
    double thermal_oscillation_tau_s = 5.0; // Filter window for temp rate of change

    // Cell Imbalance Drift Protection
    double imbalance_soft_mv = 40.0;
    double imbalance_hard_mv = 120.0;
    double cell_drift_compensation_gain = 0.75;
};

struct alignas(64) StabilityState {
    double filtered_current_a = 0.0;
    double filtered_regen_current_a = 0.0;
    double temp_derivative_c_per_s = 0.0;
    double last_temperature_c = 25.0;
    double voltage_sag_ratio = 1.0;
    double current_spike_ratio = 1.0;
    double regen_surge_ratio = 1.0;
    double thermal_stability_ratio = 1.0;
    double cell_drift_ratio = 1.0;
    double overall_membrane_stability = 1.0;
    double stability_boost_allowance = 1.0; // Multiplier for extra boost when stable
    bool dsm_tripped = false;
    DsmTripReason trip_code = DsmTripReason::NONE;

    const char* get_dsm_trip_reason() const {
        return dsm_trip_reason_to_string(trip_code);
    }
};

class RapsEVStabilityMembrane {
private:
    StabilityConfig config_;
    StabilityState state_;

public:
    explicit RapsEVStabilityMembrane(const StabilityConfig& config = StabilityConfig())
        : config_(config) {}

    void init(const StabilityConfig& config) {
        config_ = config;
        state_ = StabilityState();
    }

    const StabilityState& getState() const { return state_; }
    const StabilityConfig& getConfig() const { return config_; }

    /**
     * @brief Process powertrain state through RAPS Deterministic Safety Monitor (DSM)
     *        and Predictive Digital Twin (PDT) stability membrane filters.
     */
    StabilityState evaluate(
        double pack_voltage,
        double pack_current, // positive = discharge, negative = charge / regen
        double pack_temperature,
        const ds_plugin::DiagnosticReport* diag = nullptr,
        double dt = 0.01) {

        dt = std::max(1e-5, dt);

        // --- 1. Deterministic Safety Monitor (DSM) Hard Bounds ---
        state_.dsm_tripped = false;
        state_.trip_code = DsmTripReason::NONE;

        if (pack_voltage < config_.min_voltage_threshold_v) {
            state_.dsm_tripped = true;
            state_.trip_code = DsmTripReason::CRITICAL_UNDERVOLTAGE_SAG;
        } else if (pack_voltage > config_.max_voltage_threshold_v) {
            state_.dsm_tripped = true;
            state_.trip_code = DsmTripReason::CRITICAL_OVERVOLTAGE_SURGE;
        } else if (pack_temperature >= config_.temp_hard_limit_c) {
            state_.dsm_tripped = true;
            state_.trip_code = DsmTripReason::CRITICAL_OVERTEMPERATURE;
        } else if (diag && diag->safety_fault) {
            state_.dsm_tripped = true;
            state_.trip_code = DsmTripReason::BMS_SAFETY_FAULT;
        }

        if (state_.dsm_tripped) {
            state_.voltage_sag_ratio = 0.0;
            state_.current_spike_ratio = 0.0;
            state_.regen_surge_ratio = 0.0;
            state_.thermal_stability_ratio = 0.0;
            state_.cell_drift_ratio = 0.0;
            state_.overall_membrane_stability = 0.0;
            state_.stability_boost_allowance = 0.0;
            return state_;
        }

        // --- 2. Voltage Sag Mitigation Membrane ---
        if (pack_voltage < config_.voltage_sag_warning_v) {
            double sag_depth = (config_.voltage_sag_warning_v - pack_voltage) /
                               (config_.voltage_sag_warning_v - config_.min_voltage_threshold_v);
            sag_depth = std::clamp(sag_depth, 0.0, 1.0);
            state_.voltage_sag_ratio = 1.0 - (config_.voltage_sag_damping_factor * sag_depth);
        } else {
            state_.voltage_sag_ratio = 1.0;
        }

        // --- 3. Current Spike Filter (Exponential Damping) ---
        state_.filtered_current_a = state_.filtered_current_a +
            config_.current_spike_alpha * (pack_current - state_.filtered_current_a);

        double spike_magnitude = std::abs(pack_current - state_.filtered_current_a);
        double max_current_ref = (pack_current >= 0.0) ? config_.max_discharge_current_a
                                                        : config_.max_charge_current_a;
        double spike_ratio_raw = 1.0 - (spike_magnitude / std::max(1.0, max_current_ref));
        state_.current_spike_ratio = std::clamp(spike_ratio_raw, 0.2, 1.0);

        // --- 4. Regenerative Braking Surge Protection ---
        if (pack_current < 0.0) { // Charging / Regen phase
            double regen_curr = std::abs(pack_current);
            // Low pass filter regen current to avoid step-function derivative artifacts
            double alpha_regen = dt / (dt + 0.05); // 50ms smoothing window
            double prev_filtered = state_.filtered_regen_current_a;
            state_.filtered_regen_current_a += alpha_regen * (regen_curr - state_.filtered_regen_current_a);

            double regen_rate = (state_.filtered_regen_current_a - prev_filtered) / dt;

            if (regen_rate > config_.max_regen_surge_rate_a_per_s) {
                double excess_surge = (regen_rate - config_.max_regen_surge_rate_a_per_s) /
                                      config_.max_regen_surge_rate_a_per_s;
                excess_surge = std::clamp(excess_surge, 0.0, 1.0);
                state_.regen_surge_ratio = 1.0 - (config_.regen_surge_damping_gain * excess_surge);
            } else {
                state_.regen_surge_ratio = 1.0;
            }
        } else {
            state_.filtered_regen_current_a = 0.0;
            state_.regen_surge_ratio = 1.0;
        }

        // --- 5. Thermal Oscillation Stabilization ---
        double dT = (pack_temperature - state_.last_temperature_c) / dt;
        state_.last_temperature_c = pack_temperature;

        // Low-pass filter temperature derivative
        double dT_alpha = dt / (dt + config_.thermal_oscillation_tau_s);
        state_.temp_derivative_c_per_s += dT_alpha * (dT - state_.temp_derivative_c_per_s);

        double thermal_factor = 1.0;
        if (pack_temperature > config_.temp_soft_limit_c) {
            double temp_taper = (pack_temperature - config_.temp_soft_limit_c) /
                               (config_.temp_hard_limit_c - config_.temp_soft_limit_c);
            thermal_factor -= 0.5 * std::clamp(temp_taper, 0.0, 1.0);
        }
        // Dampen rapid thermal spikes
        if (std::abs(state_.temp_derivative_c_per_s) > 0.5) { // >0.5 °C/s rate of change
            double rate_penalty = std::min(0.3, 0.1 * std::abs(state_.temp_derivative_c_per_s));
            thermal_factor -= rate_penalty;
        }
        state_.thermal_stability_ratio = std::clamp(thermal_factor, 0.2, 1.0);

        // --- 6. Cell Imbalance Drift Compensation ---
        if (diag && diag->voltage_imbalance_mv > config_.imbalance_soft_mv) {
            double imb_ratio = (diag->voltage_imbalance_mv - config_.imbalance_soft_mv) /
                               (config_.imbalance_hard_mv - config_.imbalance_soft_mv);
            imb_ratio = std::clamp(imb_ratio, 0.0, 1.0);
            state_.cell_drift_ratio = 1.0 - (config_.cell_drift_compensation_gain * imb_ratio);
        } else {
            state_.cell_drift_ratio = 1.0;
        }

        // --- 7. Overall Membrane Stability Synthesis ---
        state_.overall_membrane_stability = state_.voltage_sag_ratio *
                                             state_.current_spike_ratio *
                                             state_.regen_surge_ratio *
                                             state_.thermal_stability_ratio *
                                             state_.cell_drift_ratio;
        state_.overall_membrane_stability = std::clamp(state_.overall_membrane_stability, 0.1, 1.0);

        // --- 8. AILEE Extra Boost Allowance Calculation ---
        // High stability (overall > 0.90) provides headroom for boost (1.0 to 1.2x)
        if (state_.overall_membrane_stability >= 0.90 && pack_temperature < config_.temp_soft_limit_c) {
            state_.stability_boost_allowance = 1.0 + 0.20 * ((state_.overall_membrane_stability - 0.90) / 0.10);
        } else {
            state_.stability_boost_allowance = state_.overall_membrane_stability;
        }

        return state_;
    }
};

} // namespace ev
} // namespace raps

#endif // RAPS_EV_STABILITY_MEMBRANE_HPP
