#include "raps_ev_stability_membrane.hpp"
#include "torque_enhancement.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
int failures = 0;
int checks = 0;
const double quiet_nan = std::numeric_limits<double>::quiet_NaN();
const double inf = std::numeric_limits<double>::infinity();
const double extreme = std::numeric_limits<double>::max();

void check(bool passed, const char* message) {
    ++checks;
    if (!passed) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

ds::EnhancedState healthy() {
    ds::EnhancedState value{};
    value.state.voltage = 400.0;
    value.state.current = 0.0;
    value.state.temperature = 25.0;
    value.state.state_of_charge = 0.5;
    value.health.remaining_capacity_percent = 100.0;
    value.ds_confidence = 1.0;
    return value;
}

bool denied(const raps::ev::StabilityState& state) {
    return state.dsm_tripped && state.overall_membrane_stability == 0.0 &&
        state.stability_boost_allowance == 0.0 &&
        std::isfinite(state.filtered_current_a) &&
        std::isfinite(state.filtered_regen_current_a) &&
        std::isfinite(state.last_temperature_c) &&
        std::isfinite(state.temp_derivative_c_per_s);
}

bool denied(const ds::drive::TorqueResult& result) {
    return result.max_drive_torque_nm == 0.0 && result.max_regen_torque_nm == 0.0 &&
        result.max_power_kw == 0.0 && result.overall_scaling == 0.0 &&
        result.limp_mode_active && result.confidence_level == 0.0 &&
        result.limiting_factor != "";
}

template<class Action> bool rejects_configuration(Action action) {
    try { action(); } catch (const std::invalid_argument&) { return true; }
    return false;
}

void test_raps_invalid_input_and_history() {
    for (double bad : {quiet_nan, inf, -inf}) {
        for (int field = 0; field < 4; ++field) {
            raps::ev::RapsEVStabilityMembrane membrane, reference;
            membrane.evaluate(400, -40, 26, nullptr, 0.01);
            reference.evaluate(400, -40, 26, nullptr, 0.01);
            const auto prior = membrane.getState();
            double voltage = 400, current = 0, temperature = 25, dt = 0.01;
            if (field == 0) voltage = bad;
            if (field == 1) current = bad;
            if (field == 2) temperature = bad;
            if (field == 3) dt = bad;
            const auto failed = membrane.evaluate(voltage, current, temperature, nullptr, dt);
            check(denied(failed), "RAPS malformed evidence emits finite zero authority");
            check(failed.filtered_current_a == prior.filtered_current_a &&
                  failed.filtered_regen_current_a == prior.filtered_regen_current_a &&
                  failed.last_temperature_c == prior.last_temperature_c &&
                  failed.temp_derivative_c_per_s == prior.temp_derivative_c_per_s,
                  "RAPS malformed evidence preserves filter history");
            const auto recovered = membrane.evaluate(400, -45, 27, nullptr, 0.01);
            const auto expected = reference.evaluate(400, -45, 27, nullptr, 0.01);
            check(recovered.overall_membrane_stability == expected.overall_membrane_stability &&
                  recovered.filtered_current_a == expected.filtered_current_a &&
                  recovered.temp_derivative_c_per_s == expected.temp_derivative_c_per_s,
                  "RAPS recovers with uncontaminated numeric history");
        }
    }
    for (double bad_dt : {0.0, -0.01}) {
        raps::ev::RapsEVStabilityMembrane membrane;
        check(denied(membrane.evaluate(400, 0, 25, nullptr, bad_dt)), "RAPS requires positive timestep");
    }
    raps::ev::RapsEVStabilityMembrane membrane;
    ds_plugin::DiagnosticReport diagnostic;
    diagnostic.voltage_imbalance_mv = quiet_nan;
    check(denied(membrane.evaluate(400, 0, 25, &diagnostic)), "RAPS rejects malformed consumed diagnostics");
    check(denied(membrane.evaluate(400, 0, -extreme)), "RAPS rejects overflowed thermal arithmetic");
    membrane.evaluate(400, extreme, 25, nullptr, 1);
    const auto prior = membrane.getState();
    const auto failed = membrane.evaluate(400, -extreme, 25, nullptr, 1);
    check(denied(failed), "RAPS rejects overflow from opposite extreme finite currents");
    check(failed.filtered_current_a == prior.filtered_current_a,
          "RAPS overflow does not commit a damaged filter");
    raps::ev::StabilityConfig long_filter;
    long_filter.thermal_oscillation_tau_s = extreme;
    raps::ev::RapsEVStabilityMembrane long_membrane(long_filter);
    check(denied(long_membrane.evaluate(400, 0, 25, nullptr, extreme)),
          "RAPS rejects overflowed finite timestep and filter-time sum");
}

void test_raps_configuration() {
    raps::ev::StabilityConfig config;
    double* fields[] = {&config.min_voltage_threshold_v, &config.max_voltage_threshold_v,
        &config.voltage_sag_warning_v, &config.voltage_sag_damping_factor,
        &config.max_discharge_current_a, &config.max_charge_current_a, &config.current_spike_alpha,
        &config.max_regen_surge_rate_a_per_s, &config.regen_surge_damping_gain,
        &config.temp_soft_limit_c, &config.temp_hard_limit_c, &config.thermal_oscillation_tau_s,
        &config.imbalance_soft_mv, &config.imbalance_hard_mv, &config.cell_drift_compensation_gain};
    for (double* field : fields) {
        const double previous = *field;
        for (double bad : {quiet_nan, inf, -inf}) {
            *field = bad;
            check(rejects_configuration([&] { raps::ev::RapsEVStabilityMembrane invalid(config); }),
                  "RAPS constructor rejects nonfinite configuration");
        }
        *field = previous;
    }
    raps::ev::RapsEVStabilityMembrane membrane;
    membrane.evaluate(400, 20, 26);
    const auto prior = membrane.getState();
    config.temp_hard_limit_c = config.temp_soft_limit_c;
    check(rejects_configuration([&] { membrane.init(config); }), "RAPS rejects unordered thermal thresholds");
    check(membrane.getConfig().temp_hard_limit_c == 60.0 &&
          membrane.getState().filtered_current_a == prior.filtered_current_a,
          "RAPS rejected initialization preserves active configuration and history");
}

void test_direct_torque_inputs() {
    for (bool raps_enabled : {false, true}) {
        for (double bad : {quiet_nan, inf, -inf}) {
            for (int field = 0; field < 11; ++field) {
                ds::drive::TorqueConfig config;
                config.enable_raps_stability_membrane = raps_enabled;
                ds::drive::DSTorqueManager manager(config);
                auto state = healthy();
                double rpm = 3000, dt = 0.01;
                if (field == 0) state.state.voltage = bad;
                if (field == 1) state.state.current = bad;
                if (field == 2) state.state.temperature = bad;
                if (field == 3) state.state.state_of_charge = bad;
                if (field == 4) state.state.entropy = bad;
                if (field == 5) state.state.phi_magnitude = bad;
                if (field == 6) state.state.degradation = bad;
                if (field == 7) state.health.remaining_capacity_percent = bad;
                if (field == 8) state.ds_confidence = bad;
                if (field == 9) rpm = bad;
                if (field == 10) dt = bad;
                check(denied(manager.compute_torque_limit(state, rpm, dt)),
                      "direct torque rejects nonfinite consumed evidence with RAPS on or off");
            }
        }
        ds::drive::TorqueConfig config;
        config.enable_raps_stability_membrane = raps_enabled;
        for (double bad_dt : {0.0, -0.01, extreme}) {
            ds::drive::DSTorqueManager manager(config);
            check(denied(manager.compute_torque_limit(healthy(), 3000, bad_dt)),
                  "direct torque rejects invalid or overflowed timestep");
        }
        ds::drive::DSTorqueManager manager(config);
        check(denied(manager.compute_torque_limit(healthy(), extreme)), "direct torque rejects overflowed RPM arithmetic");
        for (int field = 0; field < 12; ++field) {
            auto invalid = healthy();
            double rpm = 3000.0;
            switch (field) {
                case 0: invalid.state.voltage = 0.0; break;
                case 1: invalid.state.state_of_charge = -0.01; break;
                case 2: invalid.state.state_of_charge = 1.01; break;
                case 3: invalid.state.entropy = -0.01; break;
                case 4: invalid.state.entropy = 1.01; break;
                case 5: invalid.state.phi_magnitude = -0.01; break;
                case 6: invalid.state.degradation = -0.01; break;
                case 7: invalid.state.degradation = 1.01; break;
                case 8: invalid.health.remaining_capacity_percent = -0.01; break;
                case 9: invalid.health.remaining_capacity_percent = 100.01; break;
                case 10: invalid.ds_confidence = 1.01; break;
                case 11: rpm = -0.01; break;
            }
            check(denied(manager.compute_torque_limit(invalid, rpm)),
                  "direct torque rejects finite consumed values outside their domains");
        }
        auto state = healthy();
        state.state.g_eff(0, 0) = quiet_nan;
        check(denied(manager.compute_torque_limit(state, 3000)), "direct torque rejects malformed consumed metric");
        state = healthy();
        state.state.g_eff(0, 0) = extreme;
        state.state.g_eff(1, 1) = extreme;
        check(denied(manager.compute_torque_limit(state, 3000)), "direct torque rejects overflowed metric trace");
        ds_plugin::DiagnosticReport diagnostic;
        diagnostic.voltage_imbalance_mv = quiet_nan;
        check(denied(manager.compute_torque_limit(healthy(), 3000, 0.01, &diagnostic)),
              "direct torque rejects malformed consumed diagnostics");
    }
}

void test_direct_configuration_and_rejected_history() {
    ds::drive::TorqueConfig config;
    double* fields[] = {&config.drivetrain.rear_motor.peak_torque_nm,
        &config.drivetrain.rear_motor.base_speed_rpm, &config.drivetrain.rear_motor.max_speed_rpm,
        &config.drivetrain.rear_motor.efficiency_peak_rpm, &config.drivetrain.rear_motor.peak_power_kw,
        &config.drivetrain.rear_motor.max_motor_temp_c, &config.drivetrain.rear_motor.max_inverter_temp_c,
        &config.drivetrain.rear_motor.thermal_derating_start_c, &config.drivetrain.rear_motor.inverter_derating_start_c,
        &config.drivetrain.rear_motor.torque_rise_rate_nm_per_s, &config.drivetrain.rear_motor.torque_fall_rate_nm_per_s,
        &config.drivetrain.front_weight_dist, &config.battery.max_discharge_power_kw, &config.battery.max_charge_power_kw,
        &config.battery.temp_soft_limit_c, &config.battery.temp_hard_limit_c, &config.battery.temp_cold_limit_c,
        &config.battery.soc_min_normal, &config.battery.soc_min_critical, &config.battery.soc_max_regen,
        &config.battery.soc_max_full, &config.ds_weights.health_influence, &config.ds_weights.entropy_influence,
        &config.ds_weights.metric_stress_influence, &config.ds_weights.eco_power_fraction,
        &config.ds_weights.normal_power_fraction, &config.ds_weights.sport_power_fraction,
        &config.ds_weights.min_torque_fraction, &config.ds_weights.max_ds_derate,
        &config.overboost_duration_s, &config.overboost_power_multiplier};
    for (double* field : fields) {
        const double previous = *field;
        for (double bad : {quiet_nan, inf, -inf}) {
            *field = bad;
            check(rejects_configuration([&] { ds::drive::DSTorqueManager manager(config); }),
                  "direct torque rejects nonfinite consumed configuration");
        }
        *field = previous;
    }
    config = ds::drive::TorqueConfig{};
    config.battery.soc_min_normal = config.battery.soc_min_critical;
    check(rejects_configuration([&] { ds::drive::DSTorqueManager manager(config); }),
          "direct torque rejects unordered SOC thresholds");
    config = ds::drive::TorqueConfig{};
    ds::drive::DSTorqueManager manager(config);
    manager.compute_torque_limit(healthy(), 3000, 1);
    const auto prior = manager.get_diagnostics();
    const double motor_temperature = manager.get_motor_temperature();
    config.battery.temp_hard_limit_c = quiet_nan;
    check(rejects_configuration([&] { manager.init(config); }), "direct torque rejects malformed reinitialization");
    check(manager.get_config().battery.temp_hard_limit_c == 60.0 &&
          manager.get_diagnostics().update_count == prior.update_count,
          "direct torque rejected init preserves active configuration and diagnostics");
    auto malformed = healthy();
    malformed.state.temperature = quiet_nan;
    check(denied(manager.compute_torque_limit(malformed, 3000, 1)), "direct torque rejects temperature NaN");
    check(manager.get_diagnostics().update_count == prior.update_count &&
          manager.get_diagnostics().total_time_s == prior.total_time_s &&
          manager.get_motor_temperature() == motor_temperature,
          "direct torque rejection leaves thermal and diagnostic histories unchanged");
    const auto recovered = manager.compute_torque_limit(healthy(), 3000, 0.001);
    check(recovered.max_drive_torque_nm <= 2.0 + 1e-9,
          "direct torque recovery ramps from the emitted denied limit");

    // Finite configuration is not enough if a consumed derived value overflows.
    config = ds::drive::TorqueConfig{};
    config.battery.max_charge_power_kw = extreme;
    ds::drive::DSTorqueManager overflow(config);
    check(denied(overflow.compute_torque_limit(healthy(), 3000, 1)),
          "direct torque rejects overflowed configured-power conversion");
    check(overflow.get_diagnostics().update_count == 0 && overflow.get_motor_temperature() == 25.0,
          "direct torque derived failure commits no thermal or diagnostic state");
}
}

int main() {
    const auto exact_raps = raps::ev::RapsEVStabilityMembrane{}.evaluate(400, 0, quiet_nan);
    auto state = healthy(); state.state.temperature = quiet_nan;
    const auto exact_torque = ds::drive::DSTorqueManager{ds::drive::TorqueConfig{}}.compute_torque_limit(state, 3000, 1);
    std::cout << "PB04 temperature NaN: RAPS trip=" << exact_raps.dsm_tripped
              << " stability=" << exact_raps.overall_membrane_stability
              << "; drive=" << exact_torque.max_drive_torque_nm << " Nm\n";
    test_raps_invalid_input_and_history();
    test_raps_configuration();
    test_direct_torque_inputs();
    test_direct_configuration_and_rejected_history();
    if (failures) { std::cerr << failures << " failed checks\n"; return 1; }
    std::cout << "Direct numeric boundary regressions passed (" << checks << " checks)\n";
}
