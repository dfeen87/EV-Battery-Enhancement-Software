/*
 * ============================================================================
 * TEST RAPS EV STABILITY MEMBRANE
 * ============================================================================
 */

#include "raps_ev_stability_membrane.hpp"
#include "torque_enhancement.hpp"
#include "ds_torque_manager.hpp"
#include "ds_regen_braking_manager_v1.hpp"
#include <cassert>
#include <iostream>

void test_voltage_sag_mitigation() {
    raps::ev::StabilityConfig cfg;
    cfg.voltage_sag_warning_v = 340.0;
    cfg.min_voltage_threshold_v = 320.0;

    raps::ev::RapsEVStabilityMembrane membrane(cfg);
    // Normal voltage
    auto st1 = membrane.evaluate(380.0, 100.0, 25.0);
    assert(!st1.dsm_tripped);
    assert(st1.voltage_sag_ratio == 1.0);
    (void)st1;

    // Sag voltage (330V is between 320 and 340)
    auto st2 = membrane.evaluate(330.0, 100.0, 25.0);
    assert(!st2.dsm_tripped);
    assert(st2.voltage_sag_ratio < 1.0);
    assert(st2.overall_membrane_stability < 1.0);
    (void)st2;

    // Severe sag below threshold -> DSM trip
    auto st3 = membrane.evaluate(310.0, 100.0, 25.0);
    assert(st3.dsm_tripped);
    assert(std::string(st3.get_dsm_trip_reason()) == "CRITICAL_UNDERVOLTAGE_SAG");
    (void)st3;
    std::cout << "[PASS] test_voltage_sag_mitigation\n";
}

void test_current_spike_damping() {
    raps::ev::StabilityConfig cfg;
    raps::ev::RapsEVStabilityMembrane membrane(cfg);

    // Step 1: steady state current
    membrane.evaluate(400.0, 50.0, 25.0);
    // Step 2: sudden huge current spike (50 -> 500 A)
    auto st = membrane.evaluate(400.0, 500.0, 25.0);
    assert(!st.dsm_tripped);
    assert(st.current_spike_ratio < 0.9);
    assert(st.overall_membrane_stability < 1.0);
    (void)st;
    std::cout << "[PASS] test_current_spike_damping\n";
}

void test_regen_surge_damping() {
    raps::ev::StabilityConfig cfg;
    cfg.max_regen_surge_rate_a_per_s = 500.0;
    raps::ev::RapsEVStabilityMembrane membrane(cfg);

    // Step 1: zero current
    membrane.evaluate(400.0, 0.0, 25.0, nullptr, 0.01);
    // Step 2: sudden regen surge (-300 A in 0.01s = 30000 A/s surge rate)
    auto st = membrane.evaluate(400.0, -300.0, 25.0, nullptr, 0.01);
    assert(!st.dsm_tripped);
    assert(st.regen_surge_ratio < 1.0);
    (void)st;
    std::cout << "[PASS] test_regen_surge_damping\n";
}

void test_thermal_oscillation_stabilization() {
    raps::ev::StabilityConfig cfg;
    cfg.temp_soft_limit_c = 45.0;
    raps::ev::RapsEVStabilityMembrane membrane(cfg);

    // Step 1: normal temp
    membrane.evaluate(400.0, 100.0, 25.0, nullptr, 0.1);
    // Step 2: rapid thermal rise (25 -> 48 C in 0.1s)
    auto st = membrane.evaluate(400.0, 100.0, 48.0, nullptr, 0.1);
    assert(!st.dsm_tripped);
    assert(st.thermal_stability_ratio < 1.0);
    (void)st;
    std::cout << "[PASS] test_thermal_oscillation_stabilization\n";
}

void test_cell_imbalance_drift() {
    raps::ev::StabilityConfig cfg;
    cfg.imbalance_soft_mv = 40.0;
    raps::ev::RapsEVStabilityMembrane membrane(cfg);

    ds_plugin::DiagnosticReport diag;
    diag.voltage_imbalance_mv = 80.0;

    auto st = membrane.evaluate(400.0, 100.0, 25.0, &diag);
    assert(!st.dsm_tripped);
    assert(st.cell_drift_ratio < 1.0);
    (void)st;
    std::cout << "[PASS] test_cell_imbalance_drift\n";
}

void test_torque_manager_raps_integration() {
    ds::drive::TorqueConfig cfg;
    cfg.enable_raps_stability_membrane = true;
    ds::drive::DSTorqueManager torque_mgr(cfg);

    ds::EnhancedState enhanced{};
    enhanced.state.voltage = 330.0; // voltage sag
    enhanced.state.current = 200.0;
    enhanced.state.temperature = 30.0;
    enhanced.state.state_of_charge = 0.8;
    enhanced.health.remaining_capacity_percent = 95.0;

    auto result = torque_mgr.compute_torque_limit(enhanced, 3000.0, 0.01);
    assert(result.raps_membrane_derate_active);
    assert(result.raps_membrane_scaling < 1.0);

    // Test AILEE Governor Torque Manager RAPS Boost
    ds::drive::DSAileeTorqueManager ailee_tm;
    ds::drive::TorqueCommand cmd;
    cmd.requested_torque_nm = 300.0;
    cmd.motor_rpm = 4000.0;
    cmd.v_batt = 400.0;
    cmd.i_batt = 50.0; // stable initial step; surge damping is tested separately
    cmd.ctx.soc = 90.0;
    cmd.ctx.soh = 98.0;
    cmd.ctx.temp_c = 25.0;
    cmd.ctx.sensor_valid = true;

    auto gov_out = ailee_tm.processTorqueCommand(cmd);
    assert(!gov_out.raps_dsm_tripped);
    assert(gov_out.raps_membrane_stability > 0.90);
    assert(gov_out.raps_boost_multiplier > 1.0);
    std::cout << "[PASS] test_torque_manager_raps_integration\n";
}

void test_regen_braking_raps_integration() {
    ds::drive::DSRegenBrakingManager regen_mgr;
    ds::EnhancedState enhanced{};
    enhanced.state.voltage = 400.0;
    enhanced.state.temperature = 25.0;
    enhanced.state.state_of_charge = 0.6;

    // Evaluate regen with 80% pedal input
    auto res = regen_mgr.compute_regen_limit(enhanced, nullptr, 0.8, 50.0, 3000.0, false, false, 0.01);
    assert(!res.diag.raps_dsm_tripped);
    assert(res.diag.f_raps_membrane <= 1.0);
    std::cout << "[PASS] test_regen_braking_raps_integration\n";
}

int main() {
    std::cout << "=== Running RAPS EV Stability Membrane Unit Tests ===\n";
    test_voltage_sag_mitigation();
    test_current_spike_damping();
    test_regen_surge_damping();
    test_thermal_oscillation_stabilization();
    test_cell_imbalance_drift();
    test_torque_manager_raps_integration();
    test_regen_braking_raps_integration();
    std::cout << "=== All RAPS EV Stability Tests Passed Successfully! ===\n";
    return 0;
}
