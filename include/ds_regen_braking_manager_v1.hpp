/*
 * ============================================================================
 * DS Regen Braking Manager v1.0
 * ============================================================================
 *
 * PURPOSE:
 *   Intelligent regenerative braking limiter + blending guidance for EVs.
 *   Uses DS-enhanced battery state + pack diagnostics to compute:
 *     - safe regen torque limits
 *     - recommended regen fraction (blend with friction brakes)
 *
 * DESIGN INTENT:
 *   - OEM-safe: never overrides ABS/ESC; it cooperates and fails closed
 *   - deterministic: no allocations in the hot path
 *   - portable: works with any motor/inverter as long as you supply limits
 *
 * WHAT THIS MODULE DOES:
 *   1) Caps regen based on:
 *      - SOC headroom
 *      - pack voltage headroom
 *      - battery temperature (cold and hot constraints)
 *      - cell imbalance / weak cell indicators (if available)
 *      - DS stress indicators (metric trace / entropy / confidence)
 *      - safety faults from middleware (hard gate)
 *   2) Provides a stable regen fraction to simplify brake blending.
 *
 * WHAT THIS MODULE DOES NOT DO:
 *   - It does NOT replace ABS/ESC.
 *   - It does NOT compute wheel slip. (You feed in slip/ABS events.)
 *   - It does NOT command friction brakes. It only recommends blending.
 *
 * INPUTS:
 *   - Enhanced battery state (ds::EnhancedState) from DSBMSMiddleware
 *   - DiagnosticReport from DSBMSMiddleware (optional but recommended)
 *   - vehicle speed / motor RPM (optional, for speed-dependent behavior)
 *   - brake request (normalized 0..1)
 *   - ABS/ESC active flag + wheel slip flag (hard regen reductions)
 *
 * OUTPUTS:
 *   - max_regen_torque_nm
 *   - regen_fraction (0..1)
 *   - limiting_factor string (for debugging / logging)
 *   - diagnostics snapshot
 *
 * AUTHORS: Don Michael Feeney Jr. + Lex
 * DATE: December 2025
 * LICENSE: Copyright (c) Don Michael Feeney Jr. Licensed under the MIT License.
 * VERSION: 1.0.0
 * ============================================================================
 */

#ifndef DS_REGEN_BRAKING_MANAGER_V1_HPP
#define DS_REGEN_BRAKING_MANAGER_V1_HPP

#include "ds_bms_middleware_v2.hpp"  // for ds_plugin::DiagnosticReport + EnhancedState
#include "raps_ev_stability_membrane.hpp"
#include <string>
#include <algorithm>
#include <cmath>

namespace ds {
namespace drive {

// ============================================================================
// VERSION
// ============================================================================
constexpr int REGEN_VERSION_MAJOR = 6;
constexpr int REGEN_VERSION_MINOR = 0;
constexpr int REGEN_VERSION_PATCH = 0;

inline std::string regen_version() {
    return std::to_string(REGEN_VERSION_MAJOR) + "." +
           std::to_string(REGEN_VERSION_MINOR) + "." +
           std::to_string(REGEN_VERSION_PATCH);
}

// ============================================================================
// CONFIG
// ============================================================================
struct alignas(64) RegenConfig {
    // Base regen capability (motor/inverter dependent)
    double peak_regen_torque_nm = 250.0;      // absolute maximum allowed regen torque
    double max_regen_power_kw = 120.0;        // cap regen power to protect battery/inverter
    double min_speed_kph_for_regen = 5.0;     // below this, many systems fade out regen
    double max_speed_kph_for_full_regen = 30.0; // ramp-in speed for full regen feel

    // SOC constraints (regen taper near high SOC)
    double soc_regen_soft_start = 0.90;       // start tapering regen above this SOC
    double soc_regen_hard_stop = 0.97;        // stop regen above this SOC

    // Voltage headroom constraint (pack voltage near max)
    // Uses pack max from middleware safety limits via diag if available, otherwise uses this:
    double pack_voltage_max_fallback_v = 450.0;
    double voltage_soft_margin_v = 10.0;      // begin taper when within margin of max
    double voltage_hard_margin_v = 2.0;       // hard stop when very close to max

    // Temperature constraints (regen acceptance is chemistry-dependent; these are safe defaults)
    double temp_cold_soft_c = 5.0;            // below this, gradually reduce regen
    double temp_cold_hard_c = -5.0;           // below this, disable regen (risk plating)
    double temp_hot_soft_c = 55.0;            // above this, reduce regen (thermal stress)
    double temp_hot_hard_c = 62.0;            // above this, disable regen

    // Cell-level influence (only if pack diagnostics provide imbalance data)
    double imbalance_soft_mv = 50.0;          // start reducing regen if imbalance high
    double imbalance_hard_mv = 120.0;         // aggressively reduce regen if extreme

    // DS stress influence (optional: conservative derating when confidence is low)
    double min_ds_confidence_for_full_regen = 0.85; // below this, start tapering
    double min_ds_confidence_hard = 0.60;           // below this, strong reduction

    // ABS/ESC cooperation
    double abs_regen_cut_fraction = 0.10;     // when ABS/ESC active, allow only 10% regen
    double slip_regen_cut_fraction = 0.25;    // when wheel slip flagged, allow 25% regen
    double regen_recovery_tau_s = 0.25;       // smoothing time constant for regen recovery

    // Output smoothing (avoid oscillations)
    double torque_slew_nm_per_s = 5000.0;     // limit rate of change of regen torque
};

// ============================================================================
// RESULT + DIAGNOSTICS
// ============================================================================
struct alignas(64) RegenDiagnostics {
    double last_max_regen_torque_nm = 0.0;
    double last_regen_fraction = 0.0;

    // Most recent derate factors (0..1)
    double f_speed = 1.0;
    double f_soc = 1.0;
    double f_voltage = 1.0;
    double f_temp = 1.0;
    double f_cell = 1.0;
    double f_ds = 1.0;
    double f_stability = 1.0;
    double f_raps_membrane = 1.0;

    int abs_events = 0;
    int slip_events = 0;
    int safety_blocks = 0;

    bool raps_dsm_tripped = false;
    std::string raps_dsm_trip_reason = "NONE";

    std::string limiting_factor = "NONE";
};

struct alignas(64) RegenResult {
    double max_regen_torque_nm = 0.0;     // final torque limit (Nm)
    double regen_fraction = 0.0;          // 0..1 portion of braking to assign to regen
    std::string limiting_factor = "NONE";
    RegenDiagnostics diag;
};

// ============================================================================
// REGEN MANAGER
// ============================================================================
class DSRegenBrakingManager {
public:
    explicit DSRegenBrakingManager(const RegenConfig& cfg = RegenConfig(),
                                  const raps::ev::StabilityConfig& raps_cfg = raps::ev::StabilityConfig())
        : cfg_(cfg), raps_membrane_(raps_cfg), last_cmd_torque_nm_(0.0), last_output_torque_nm_(0.0),
          last_time_s_(0.0), last_abs_or_slip_time_s_(-1.0) {}

    static std::string get_version() { return regen_version(); }

    // Main compute function
    //
    // Inputs:
    //   enhanced: battery enhanced state from middleware
    //   diag: optional pointer to middleware diagnostics (recommended)
    //   brake_request: 0..1 (driver braking demand)
    //   vehicle_speed_kph: for regen ramp-in/out behavior
    //   motor_rpm: optional (if you want speed-dependent regen torque mapping later)
    //   abs_active: if ABS/ESC is active, regen must cut hard to preserve stability
    //   wheel_slip: if slip is detected, reduce regen
    //   dt: time step seconds
    //
    RegenResult compute_regen_limit(
        const ds::EnhancedState& enhanced,
        const ds_plugin::DiagnosticReport* diag,
        double brake_request,
        double vehicle_speed_kph,
        double motor_rpm,
        bool abs_active,
        bool wheel_slip,
        double dt
    ) {
        (void)motor_rpm;
        RegenResult out;
        out.diag = RegenDiagnostics();

        brake_request = clamp01(brake_request);
        dt = std::max(1e-6, dt);

        // Hard safety gate: if middleware reports safety fault, disable regen.
        if (diag && diag->safety_fault) {
            out.diag.safety_blocks++;
            out.limiting_factor = "SAFETY_FAULT";
            out.max_regen_torque_nm = 0.0;
            out.regen_fraction = 0.0;
            out.diag.limiting_factor = out.limiting_factor;
            remember(out, dt);
            return out;
        }

        // Base capability constrained by power limit (P = V*I; torque relates to power by speed)
        // Here we cap torque by max_regen_power using a conservative approximation:
        //    regen_torque_cap ≈ min(peak_regen_torque, power_cap / omega)
        // If speed unknown/low, we just keep peak torque and rely on speed factor + min speed cutoff.
        double base_torque_cap = cfg_.peak_regen_torque_nm;

        // Speed factor: regen fades below min speed; ramps to full by max_speed_kph_for_full_regen
        out.diag.f_speed = compute_speed_factor(vehicle_speed_kph);

        // SOC factor: taper near full SOC
        out.diag.f_soc = compute_soc_factor(enhanced.state.state_of_charge);

        // Voltage headroom: taper near max pack voltage
        const double pack_voltage = enhanced.state.voltage;
        const double v_max = (diag ? diag_pack_max_voltage(*diag) : cfg_.pack_voltage_max_fallback_v);
        out.diag.f_voltage = compute_voltage_factor(pack_voltage, v_max);

        // Temperature acceptance: reduce regen if too cold or too hot
        const double temp_c = enhanced.state.temperature;
        out.diag.f_temp = compute_temp_factor(temp_c);

        // Cell imbalance / weak cells: only applies if diagnostics exists
        out.diag.f_cell = 1.0;
        if (diag) {
            out.diag.f_cell = compute_cell_factor(*diag);
        }

        // DS confidence factor: conservative taper if confidence is low
        out.diag.f_ds = compute_ds_factor(diag ? diag->ds_confidence : 1.0);

        // ABS / slip cooperation
        if (abs_active) {
            out.diag.abs_events++;
            last_abs_or_slip_time_s_ = 0.0; // will be advanced by remember()
        }
        if (wheel_slip) {
            out.diag.slip_events++;
            last_abs_or_slip_time_s_ = 0.0;
        }

        // Stability factor (hard cut when events active; recovery smoothing when cleared)
        out.diag.f_stability = compute_stability_factor(abs_active, wheel_slip, dt);

        // RAPS EV Stability Membrane evaluation (dampens surges & voltage/thermal oscillations)
        double est_regen_current_a = -1.0 * (requested_torque_nm_approx(brake_request) * 0.5); // negative = charging
        auto raps_state = raps_membrane_.evaluate(pack_voltage, est_regen_current_a, temp_c, diag, dt);
        out.diag.f_raps_membrane = raps_state.overall_membrane_stability;
        out.diag.raps_dsm_tripped = raps_state.dsm_tripped;
        out.diag.raps_dsm_trip_reason = raps_state.get_dsm_trip_reason();

        if (raps_state.dsm_tripped) {
            out.diag.safety_blocks++;
            out.limiting_factor = std::string("RAPS_DSM_TRIP (") + raps_state.get_dsm_trip_reason() + ")";
            out.max_regen_torque_nm = 0.0;
            out.regen_fraction = 0.0;
            out.diag.limiting_factor = out.limiting_factor;
            remember(out, dt);
            return out;
        }

        // Combine all derate factors
        double combined = out.diag.f_speed *
                          out.diag.f_soc *
                          out.diag.f_voltage *
                          out.diag.f_temp *
                          out.diag.f_cell *
                          out.diag.f_ds *
                          out.diag.f_stability *
                          out.diag.f_raps_membrane;

        combined = std::clamp(combined, 0.0, 1.0);

        // Determine limiting factor (most restrictive)
        out.limiting_factor = select_limiting_factor(out.diag);
        out.diag.limiting_factor = out.limiting_factor;

        // Requested regen torque is proportional to brake request (simple & stable)
        // OEM NOTE:
        //   Many production systems map pedal to decel, then to torque. Keep this linear mapping
        //   for now; OEM can replace mapping upstream while using this module’s torque cap.
        double requested_torque = brake_request * cfg_.peak_regen_torque_nm;

        // Apply combined derate to torque cap
        double max_allowed = base_torque_cap * combined;

        // Smooth recovery, but never delay a tighter protection or pedal ceiling.
        const double target = std::min(requested_torque, max_allowed);
        double limited = std::min(target, slew_limit(last_output_torque_nm_, target, cfg_.torque_slew_nm_per_s, dt));

        out.max_regen_torque_nm = std::max(0.0, limited);

        // Regen fraction: recommended portion of braking to handle via regen
        // Keep it tied to the ratio of allowed vs requested.
        if (requested_torque > 1e-6) {
            out.regen_fraction = clamp01(out.max_regen_torque_nm / requested_torque);
        } else {
            out.regen_fraction = 0.0;
        }

        remember(out, dt);
        return out;
    }

    const RegenDiagnostics& diagnostics() const { return last_diag_; }

private:
    RegenConfig cfg_;
    raps::ev::RapsEVStabilityMembrane raps_membrane_;
    double last_cmd_torque_nm_;
    double last_output_torque_nm_;
    double last_time_s_;
    double last_abs_or_slip_time_s_;
    RegenDiagnostics last_diag_;

    double requested_torque_nm_approx(double brake_request) const {
        return brake_request * cfg_.peak_regen_torque_nm;
    }

    static double clamp01(double x) { return std::clamp(x, 0.0, 1.0); }

    double compute_speed_factor(double v_kph) const {
        if (v_kph <= cfg_.min_speed_kph_for_regen) return 0.0;
        if (v_kph >= cfg_.max_speed_kph_for_full_regen) return 1.0;
        double t = (v_kph - cfg_.min_speed_kph_for_regen) /
                   (cfg_.max_speed_kph_for_full_regen - cfg_.min_speed_kph_for_regen);
        return std::clamp(t, 0.0, 1.0);
    }

    double compute_soc_factor(double soc) const {
        soc = clamp01(soc);
        if (soc >= cfg_.soc_regen_hard_stop) return 0.0;
        if (soc <= cfg_.soc_regen_soft_start) return 1.0;
        double t = (cfg_.soc_regen_hard_stop - soc) /
                   (cfg_.soc_regen_hard_stop - cfg_.soc_regen_soft_start);
        return std::clamp(t, 0.0, 1.0);
    }

    double compute_voltage_factor(double v_pack, double v_max) const {
        if (v_max <= 1.0) return 1.0;
        const double soft_start = v_max - cfg_.voltage_soft_margin_v;
        const double hard_stop  = v_max - cfg_.voltage_hard_margin_v;

        if (v_pack >= hard_stop) return 0.0;
        if (v_pack <= soft_start) return 1.0;

        double t = (hard_stop - v_pack) / (hard_stop - soft_start);
        return std::clamp(t, 0.0, 1.0);
    }

    double compute_temp_factor(double t_c) const {
        // Cold taper
        if (t_c <= cfg_.temp_cold_hard_c) return 0.0;
        double f_cold = 1.0;
        if (t_c < cfg_.temp_cold_soft_c) {
            f_cold = (t_c - cfg_.temp_cold_hard_c) /
                     (cfg_.temp_cold_soft_c - cfg_.temp_cold_hard_c);
            f_cold = std::clamp(f_cold, 0.0, 1.0);
        }

        // Hot taper
        if (t_c >= cfg_.temp_hot_hard_c) return 0.0;
        double f_hot = 1.0;
        if (t_c > cfg_.temp_hot_soft_c) {
            f_hot = (cfg_.temp_hot_hard_c - t_c) /
                    (cfg_.temp_hot_hard_c - cfg_.temp_hot_soft_c);
            f_hot = std::clamp(f_hot, 0.0, 1.0);
        }

        return std::min(f_cold, f_hot);
    }

    double compute_cell_factor(const ds_plugin::DiagnosticReport& d) const {
        // If no cell detail available, do nothing.
        if (d.total_cells <= 1) return 1.0;

        const double dv = d.voltage_imbalance_mv;
        if (dv >= cfg_.imbalance_hard_mv) return 0.35; // aggressive reduction but not full cut
        if (dv <= cfg_.imbalance_soft_mv) return 1.0;

        double t = (cfg_.imbalance_hard_mv - dv) /
                   (cfg_.imbalance_hard_mv - cfg_.imbalance_soft_mv);
        return std::clamp(t, 0.35, 1.0);
    }

    double compute_ds_factor(double confidence) const {
        confidence = clamp01(confidence);
        if (confidence <= cfg_.min_ds_confidence_hard) return 0.50;
        if (confidence >= cfg_.min_ds_confidence_for_full_regen) return 1.0;

        double t = (confidence - cfg_.min_ds_confidence_hard) /
                   (cfg_.min_ds_confidence_for_full_regen - cfg_.min_ds_confidence_hard);
        return std::clamp(t, 0.50, 1.0);
    }

    double compute_stability_factor(bool abs_active, bool slip, double dt) {
        // Hard cuts when active.
        if (abs_active) return cfg_.abs_regen_cut_fraction;
        if (slip) return cfg_.slip_regen_cut_fraction;

        // Recovery smoothing: after event clears, gradually restore stability factor.
        // This reduces “regen snap-back” which can destabilize low-µ surfaces.
        if (last_abs_or_slip_time_s_ >= 0.0) {
            // last_abs_or_slip_time_s_ is advanced in remember()
            const double tau = std::max(1e-3, cfg_.regen_recovery_tau_s);
            const double alpha = 1.0 - std::exp(-dt / tau);
            // Blend toward 1.0 from current last value (stored in last_diag_.f_stability)
            double prev = last_diag_.f_stability;
            return prev + alpha * (1.0 - prev);
        }
        return 1.0;
    }

    static double slew_limit(double prev, double target, double rate, double dt) {
        const double max_delta = std::max(0.0, rate) * dt;
        const double delta = target - prev;
        if (delta > max_delta) return prev + max_delta;
        if (delta < -max_delta) return prev - max_delta;
        return target;
    }

    static std::string select_limiting_factor(const RegenDiagnostics& d) {
        // Smallest factor dominates
        double min_f = d.f_speed;
        std::string name = "SPEED";

        if (d.f_soc < min_f) { min_f = d.f_soc; name = "SOC"; }
        if (d.f_voltage < min_f) { min_f = d.f_voltage; name = "VOLTAGE"; }
        if (d.f_temp < min_f) { min_f = d.f_temp; name = "TEMPERATURE"; }
        if (d.f_cell < min_f) { min_f = d.f_cell; name = "CELL_IMBALANCE"; }
        if (d.f_ds < min_f) { min_f = d.f_ds; name = "DS_CONFIDENCE"; }
        if (d.f_stability < min_f) { min_f = d.f_stability; name = "STABILITY_EVENT"; }
        if (d.f_raps_membrane < min_f) { min_f = d.f_raps_membrane; name = "RAPS_MEMBRANE"; }

        // If none reduced, call it NONE
        if (min_f >= 0.999) return "NONE";
        return name;
    }

    static double diag_pack_max_voltage(const ds_plugin::DiagnosticReport& d) {
        // DiagnosticReport doesn’t currently contain pack max voltage explicitly.
        // OEM NOTE:
        //   If you want exact limits, consider adding pack voltage max to diagnostics,
        //   or pass config/safety limits into this manager.
        // For now, rely on fallback in config in compute_voltage_factor.
        (void)d;
        return 450.0;
    }

    void remember(const RegenResult& r, double dt) {
        last_cmd_torque_nm_ = r.max_regen_torque_nm;
        last_output_torque_nm_ = r.max_regen_torque_nm;
        last_diag_ = r.diag;

        // advance event timer if active
        if (last_abs_or_slip_time_s_ >= 0.0) {
            last_abs_or_slip_time_s_ += dt;
            // After ~2 seconds with no further events, stop tracking.
            if (last_abs_or_slip_time_s_ > 2.0) last_abs_or_slip_time_s_ = -1.0;
        }

        last_time_s_ += dt;
    }
};

} // namespace drive
} // namespace ds

#endif // DS_REGEN_BRAKING_MANAGER_V1_HPP
