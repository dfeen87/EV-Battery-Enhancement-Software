#include "ds_regen_braking_manager_v1.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
int failures = 0;

void require(bool condition, const char* invariant, double actual) {
    if (!condition) {
        std::cerr << "FAIL: " << invariant << " (actual=" << actual << ")\n";
        ++failures;
    }
}

ds::EnhancedState healthy_state() {
    ds::EnhancedState state{};
    state.state.voltage = 400.0;
    state.state.current = 0.0;
    state.state.temperature = 25.0;
    state.state.state_of_charge = 0.5;
    state.health.remaining_capacity_percent = 100.0;
    state.ds_confidence = 1.0;
    state.numerical_stability = true;
    return state;
}

void require_denied(const ds::drive::RegenResult& result, const char* invariant) {
    require(std::isfinite(result.max_regen_torque_nm) && result.max_regen_torque_nm == 0.0,
            invariant, result.max_regen_torque_nm);
    require(std::isfinite(result.regen_fraction) && result.regen_fraction == 0.0,
            "denied regen has zero finite blending fraction", result.regen_fraction);
    for (double value : {result.diag.f_speed, result.diag.f_soc, result.diag.f_voltage,
                         result.diag.f_temp, result.diag.f_cell, result.diag.f_ds,
                         result.diag.f_stability, result.diag.f_raps_membrane}) {
        require(std::isfinite(value), "denied regen diagnostics remain finite", value);
    }
}

void test_mechanical_power_ceiling() {
    ds::drive::RegenConfig config;
    config.max_regen_power_kw = 1.0;
    for (double rpm : {0.0, 1e-9, 50.0, 3000.0, 30000.0,
                       std::numeric_limits<double>::max()}) {
        ds::drive::DSRegenBrakingManager manager(config);
        auto result = manager.compute_regen_limit(
            healthy_state(), nullptr, 1.0, 50.0, rpm, false, false, 1.0);
        const double omega = rpm * (2.0 * std::acos(-1.0) / 60.0);
        const double power_kw = result.max_regen_torque_nm * (omega / 1000.0);
        require(std::isfinite(power_kw) && power_kw <= config.max_regen_power_kw + 1e-9,
                "mechanical regen power respects configured kilowatt ceiling", power_kw);
        require(result.max_regen_torque_nm <= config.peak_regen_torque_nm,
                "power conversion preserves peak torque ceiling", result.max_regen_torque_nm);
        if (rpm == 0.0) {
            require(result.max_regen_torque_nm > 0.0,
                    "known zero RPM has zero power without division by zero", result.max_regen_torque_nm);
        }
    }
    config.max_regen_power_kw = 0.0;
    ds::drive::DSRegenBrakingManager disabled(config);
    require_denied(disabled.compute_regen_limit(
        healthy_state(), nullptr, 1.0, 50.0, 0.0, false, false, 1.0),
        "zero configured power disables regen at zero RPM too");

    ds::drive::DSRegenBrakingManager rising_rpm(ds::drive::RegenConfig{});
    rising_rpm.compute_regen_limit(healthy_state(), nullptr, 1.0, 50.0, 3000.0, false, false, 1.0);
    auto reduced = rising_rpm.compute_regen_limit(
        healthy_state(), nullptr, 1.0, 50.0, 30000.0, false, false, 0.001);
    require(reduced.max_regen_torque_nm * (30000.0 * 2.0 * std::acos(-1.0) / 60000.0) <= 120.0 + 1e-9,
            "higher RPM power ceiling applies before recovery slew", reduced.max_regen_torque_nm);
}

void test_under_resolved_positive_speed_denies_regen() {
    for (double rpm : {1e-320, 7e-320}) {
        ds::drive::RegenConfig config;
        config.max_regen_power_kw = 2.0 * std::numeric_limits<double>::denorm_min();
        ds::drive::DSRegenBrakingManager manager(config), reference(config);
        ds::drive::RegenResult result;
        for (int step = 0; step < 1000; ++step) {
            result = manager.compute_regen_limit(
                healthy_state(), nullptr, 1.0, 50.0, rpm, false, false, 1.0);
        }
        const double actual_power_kw = (result.max_regen_torque_nm * rpm) *
            (2.0 * std::acos(-1.0) / 60000.0);
        require(actual_power_kw <= config.max_regen_power_kw,
                "under-resolved positive speed cannot bypass power cap", actual_power_kw);
        require_denied(result, "under-resolved positive speed gives zero authority");
        require(result.limiting_factor == "NUMERICAL_FAILURE",
                "under-resolved speed is identified as numerical rejection", result.max_regen_torque_nm);
        const auto recovered = manager.compute_regen_limit(
            healthy_state(), nullptr, 1.0, 50.0, 3000.0, false, false, 0.001);
        const auto expected = reference.compute_regen_limit(
            healthy_state(), nullptr, 1.0, 50.0, 3000.0, false, false, 0.001);
        require(recovered.max_regen_torque_nm == expected.max_regen_torque_nm &&
                recovered.diag.f_raps_membrane == expected.diag.f_raps_membrane,
                "under-resolved speed leaves accepted filter history unchanged", recovered.max_regen_torque_nm);
    }
}

void test_configured_voltage_precedence() {
    ds::drive::RegenConfig config;
    config.pack_voltage_max_fallback_v = 410.0;
    ds_plugin::DiagnosticReport diagnostic;
    for (double voltage : {399.0, 405.0, 408.0, 409.0}) {
        auto state = healthy_state();
        state.state.voltage = voltage;
        ds::drive::DSRegenBrakingManager without_diag(config), with_diag(config);
        auto first = without_diag.compute_regen_limit(state, nullptr, 1.0, 50.0, 3000.0, false, false, 1.0);
        auto second = with_diag.compute_regen_limit(state, &diagnostic, 1.0, 50.0, 3000.0, false, false, 1.0);
        require(first.max_regen_torque_nm == second.max_regen_torque_nm,
                "diagnostics without voltage limit cannot replace configured policy", second.max_regen_torque_nm);
        require(first.diag.f_voltage == second.diag.f_voltage,
                "voltage factor is identical with ordinary diagnostics", second.diag.f_voltage);
        if (voltage >= 408.0) require_denied(second, "configured voltage hard stop applies with diagnostics");
    }
    config.pack_voltage_max_fallback_v = 0.5;
    config.voltage_soft_margin_v = 0.2;
    config.voltage_hard_margin_v = 0.1;
    ds::drive::DSRegenBrakingManager restrictive(config);
    require_denied(restrictive.compute_regen_limit(
        healthy_state(), nullptr, 1.0, 50.0, 3000.0, false, false, 1.0),
        "positive configured voltage maximum is never silently bypassed");
}

void test_invalid_input_and_recovery() {
    for (double invalid : {std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity(),
                           -std::numeric_limits<double>::infinity()}) {
        for (int field = 0; field < 10; ++field) {
            auto state = healthy_state();
            ds_plugin::DiagnosticReport diagnostic;
            double brake = 1.0, speed = 50.0, rpm = 3000.0, dt = 1.0;
            double* values[] = {&state.state.voltage, &state.state.current, &state.state.temperature,
                &state.state.state_of_charge, &brake, &speed, &rpm, &dt,
                &diagnostic.ds_confidence, &diagnostic.voltage_imbalance_mv};
            *values[field] = invalid;
            ds::drive::DSRegenBrakingManager manager;
            require_denied(manager.compute_regen_limit(
                state, &diagnostic, brake, speed, rpm, true, true, dt),
                "malformed regen evidence authorizes zero torque");
            ds::drive::DSRegenBrakingManager fresh;
            auto recovery = manager.compute_regen_limit(
                healthy_state(), nullptr, 1.0, 50.0, 3000.0, false, false, 0.001);
            auto reference = fresh.compute_regen_limit(
                healthy_state(), nullptr, 1.0, 50.0, 3000.0, false, false, 0.001);
            require(recovery.max_regen_torque_nm == reference.max_regen_torque_nm,
                    "rejected input does not contaminate filters or ABS recovery", recovery.max_regen_torque_nm);
            require(recovery.diag.f_raps_membrane == reference.diag.f_raps_membrane,
                    "RAPS history remains unchanged after rejected regen evidence", recovery.diag.f_raps_membrane);
        }
    }
    for (int field = 0; field < 8; ++field) {
        auto state = healthy_state();
        double brake = 1.0, speed = 50.0, rpm = 3000.0, dt = 1.0;
        double* values[] = {&state.state.voltage, &state.state.state_of_charge,
                            &brake, &speed, &rpm, &dt};
        if (field < 6) *values[field] = field == 0 || field == 5 ? 0.0 : -1.0;
        if (field == 6) state.state.state_of_charge = 1.01;
        if (field == 7) brake = 1.01;
        ds::drive::DSRegenBrakingManager manager;
        require_denied(manager.compute_regen_limit(
            state, nullptr, brake, speed, rpm, false, false, dt),
            "out-of-domain regen evidence authorizes zero torque");
    }
    ds::drive::RegenConfig config;
    config.torque_slew_nm_per_s = std::numeric_limits<double>::max();
    ds::drive::DSRegenBrakingManager overflow(config);
    require_denied(overflow.compute_regen_limit(
        healthy_state(), nullptr, 1.0, 50.0, 3000.0, false, false, 2.0),
        "finite inputs with overflowing slew arithmetic are rejected");

    ds::drive::DSRegenBrakingManager manager;
    manager.compute_regen_limit(healthy_state(), nullptr, 1.0, 50.0, 3000.0, false, false, 1.0);
    auto malformed = healthy_state();
    malformed.state.temperature = std::numeric_limits<double>::quiet_NaN();
    require_denied(manager.compute_regen_limit(
        malformed, nullptr, 1.0, 50.0, 3000.0, false, false, 0.001),
        "malformed evidence stops established regen immediately");
    auto recovery = manager.compute_regen_limit(
        healthy_state(), nullptr, 1.0, 50.0, 3000.0, false, false, 0.001);
    require(recovery.max_regen_torque_nm <= 5.0 + 1e-9,
            "recovery starts at emitted zero after malformed evidence", recovery.max_regen_torque_nm);
}

void test_invalid_configuration() {
    for (int field = 0; field < 22; ++field) {
        ds::drive::RegenConfig config;
        double* values[] = {&config.peak_regen_torque_nm, &config.max_regen_power_kw,
            &config.min_speed_kph_for_regen, &config.max_speed_kph_for_full_regen,
            &config.soc_regen_soft_start, &config.soc_regen_hard_stop,
            &config.pack_voltage_max_fallback_v, &config.voltage_soft_margin_v,
            &config.voltage_hard_margin_v, &config.temp_cold_soft_c, &config.temp_cold_hard_c,
            &config.temp_hot_soft_c, &config.temp_hot_hard_c, &config.imbalance_soft_mv,
            &config.imbalance_hard_mv, &config.min_ds_confidence_for_full_regen,
            &config.min_ds_confidence_hard, &config.abs_regen_cut_fraction,
            &config.slip_regen_cut_fraction, &config.regen_recovery_tau_s,
            &config.torque_slew_nm_per_s};
        if (field < 21) *values[field] = std::numeric_limits<double>::quiet_NaN();
        else config.soc_regen_hard_stop = config.soc_regen_soft_start;
        bool rejected = false;
        try {
            ds::drive::DSRegenBrakingManager manager(config);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "invalid regen configuration rejected during construction", field);
    }
}

void test_rejection_preserves_stability_history() {
    ds::drive::DSRegenBrakingManager manager, reference;
    auto state = healthy_state();
    manager.compute_regen_limit(state, nullptr, 1.0, 50.0, 3000.0, true, false, 0.01);
    reference.compute_regen_limit(state, nullptr, 1.0, 50.0, 3000.0, true, false, 0.01);
    auto invalid = state;
    invalid.state.temperature = std::numeric_limits<double>::quiet_NaN();
    require_denied(manager.compute_regen_limit(
        invalid, nullptr, 1.0, 50.0, 3000.0, false, false, 0.1),
        "invalid cycle between ABS recovery steps is denied");
    const auto recovery = manager.compute_regen_limit(
        state, nullptr, 1.0, 50.0, 3000.0, false, false, 0.01);
    const auto expected = reference.compute_regen_limit(
        state, nullptr, 1.0, 50.0, 3000.0, false, false, 0.01);
    require(recovery.diag.f_stability == expected.diag.f_stability,
            "invalid cycle preserves accepted ABS recovery factor", recovery.diag.f_stability);

    ds::drive::DSRegenBrakingManager numerical, fresh;
    invalid = state;
    invalid.state.temperature = -std::numeric_limits<double>::max();
    const auto denied = numerical.compute_regen_limit(
        invalid, nullptr, 1.0, 50.0, 3000.0, true, false, 1e-300);
    require_denied(denied, "RAPS numerical rejection authorizes zero regen");
    require(denied.diag.raps_dsm_trip_reason == "NUMERICAL_FAILURE",
            "extreme finite evidence reports the RAPS numerical rejection", denied.max_regen_torque_nm);
    const auto after = numerical.compute_regen_limit(
        state, nullptr, 1.0, 50.0, 3000.0, false, false, 0.001);
    const auto clean = fresh.compute_regen_limit(
        state, nullptr, 1.0, 50.0, 3000.0, false, false, 0.001);
    require(after.diag.f_stability == clean.diag.f_stability,
            "RAPS rejected candidate does not start ABS recovery history", after.diag.f_stability);
    require(after.max_regen_torque_nm == clean.max_regen_torque_nm,
            "RAPS rejected candidate leaves accepted filter history unchanged", after.max_regen_torque_nm);
}
} // namespace

int main() {
    test_mechanical_power_ceiling();
    test_under_resolved_positive_speed_denies_regen();
    test_configured_voltage_precedence();
    test_invalid_input_and_recovery();
    test_invalid_configuration();
    test_rejection_preserves_stability_history();
    if (failures != 0) {
        std::cerr << "Regen boundary failures: " << failures << '\n';
        return 1;
    }
    std::cout << "Regen power, voltage and evidence boundaries passed\n";
    return 0;
}
