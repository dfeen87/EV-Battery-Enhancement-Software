#include "torque_enhancement.hpp"
#include "ds_regen_braking_manager_v1.hpp"

#include <cmath>
#include <iostream>

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

void test_regen_protective_decreases() {
    // Establish prior output above every next-cycle protective ceiling.
    // RAPS remains enabled; its additional derating may only lower these bounds.
    for (int boundary = 0; boundary < 9; ++boundary) {
        ds::drive::RegenConfig config;
        // Exercise the regen temperature gate below the separate RAPS hard gate.
        config.temp_hot_soft_c = 45.0;
        config.temp_hot_hard_c = 50.0;
        ds::drive::DSRegenBrakingManager manager(config);
        auto state = healthy_state();
        auto previous = manager.compute_regen_limit(
            state, nullptr, 1.0, 50.0, 3000.0, false, false, 1.0);
        require(previous.max_regen_torque_nm > 100.0,
                "healthy regen establishes a positive limit", previous.max_regen_torque_nm);

        double brake_request = 1.0;
        double speed = 50.0;
        bool abs_active = false;
        bool wheel_slip = false;
        double ceiling = 0.0;
        const char* invariant = "regen hard stop applies immediately";
        switch (boundary) {
            case 0: state.state.state_of_charge = 1.0; break;
            case 1: state.state.voltage = 448.0; break;
            case 2: state.state.temperature = -5.0; break;
            case 3: state.state.temperature = 50.0; break;
            case 4: speed = 0.0; break;
            case 5: brake_request = 0.0; break;
            case 6:
                abs_active = true;
                ceiling = config.peak_regen_torque_nm * config.abs_regen_cut_fraction;
                invariant = "ABS regen ceiling applies immediately";
                break;
            case 7:
                wheel_slip = true;
                ceiling = config.peak_regen_torque_nm * config.slip_regen_cut_fraction;
                invariant = "slip regen ceiling applies immediately";
                break;
            case 8:
                state.state.state_of_charge = 0.95;
                // The configured linear SOC taper authorizes at most 2/7 peak.
                ceiling = config.peak_regen_torque_nm * (2.0 / 7.0);
                invariant = "SOC taper ceiling applies immediately";
                break;
        }
        auto constrained = manager.compute_regen_limit(
            state, nullptr, brake_request, speed, 3000.0, abs_active, wheel_slip, 0.001);
        require(constrained.max_regen_torque_nm <= ceiling + 1e-9,
                invariant, constrained.max_regen_torque_nm);
    }
}

void test_regen_ramp_and_recovery() {
    ds::drive::RegenConfig config;
    ds::drive::DSRegenBrakingManager manager(config);
    auto state = healthy_state();
    auto first = manager.compute_regen_limit(
        state, nullptr, 1.0, 50.0, 3000.0, false, false, 0.001);
    require(std::abs(first.max_regen_torque_nm - 5.0) < 1e-9,
            "ordinary regen rise remains slew limited", first.max_regen_torque_nm);
    manager.compute_regen_limit(state, nullptr, 1.0, 50.0, 3000.0, false, false, 1.0);
    state.state.state_of_charge = 1.0;
    auto stopped = manager.compute_regen_limit(
        state, nullptr, 1.0, 50.0, 3000.0, false, false, 0.001);
    require(stopped.max_regen_torque_nm == 0.0,
            "full SOC stops regen before recovery", stopped.max_regen_torque_nm);
    state.state.state_of_charge = 0.5;
    auto recovery = manager.compute_regen_limit(
        state, nullptr, 1.0, 50.0, 3000.0, false, false, 0.001);
    require(recovery.max_regen_torque_nm <= 5.0 + 1e-9,
            "regen recovery ramps from last emitted limit", recovery.max_regen_torque_nm);
}

void test_drive_protective_decreases() {
    ds::drive::TorqueConfig config;
    config.enable_raps_stability_membrane = false;
    ds::drive::DSTorqueManager manager(config);
    auto state = healthy_state();
    auto healthy = manager.compute_torque_limit(state, 3000.0, 1.0);
    require(std::abs(healthy.max_drive_torque_nm - 340.0) < 1e-9,
            "normal configured drive envelope is unchanged", healthy.max_drive_torque_nm);
    state.state.state_of_charge = 0.03;
    auto low_soc = manager.compute_torque_limit(state, 3000.0, 0.001);
    require(std::abs(low_soc.max_drive_torque_nm - 85.0) < 1e-9,
            "configured 25 percent low SOC ceiling applies immediately", low_soc.max_drive_torque_nm);
    state.state.state_of_charge = 0.5;
    auto recovery = manager.compute_torque_limit(state, 3000.0, 0.001);
    require(std::abs(recovery.max_drive_torque_nm - 87.0) < 1e-9,
            "drive recovery preserves configured rise slew", recovery.max_drive_torque_nm);
    auto overspeed = manager.compute_torque_limit(state, 16000.0, 0.001);
    require(overspeed.max_drive_torque_nm == 0.0,
            "motor speed hard limit applies immediately", overspeed.max_drive_torque_nm);

    ds::drive::DSTorqueManager thermal_manager(config);
    thermal_manager.compute_torque_limit(state, 3000.0, 1.0);
    state.state.temperature = 60.0;
    auto hot = thermal_manager.compute_torque_limit(state, 3000.0, 0.001);
    require(hot.max_drive_torque_nm <= 136.0 + 1e-9,
            "configured thermal drive ceiling applies immediately", hot.max_drive_torque_nm);
}

void test_drive_ramp_and_hard_gate_history() {
    auto state = healthy_state();
    ds::drive::DSTorqueManager manager{ds::drive::TorqueConfig{}};
    auto first = manager.compute_torque_limit(state, 3000.0, 0.001);
    require(std::abs(first.max_drive_torque_nm - 2.0) < 1e-9,
            "ordinary drive rise remains slew limited", first.max_drive_torque_nm);
    auto second = manager.compute_torque_limit(state, 3000.0, 0.001);
    require(std::abs(second.max_drive_torque_nm - 4.0) < 1e-9,
            "ordinary drive history tracks the emitted limit", second.max_drive_torque_nm);
    manager.compute_torque_limit(state, 3000.0, 1.0);
    ds_plugin::DiagnosticReport diagnostics;
    diagnostics.safety_fault = true;
    auto stopped = manager.compute_torque_limit(state, 3000.0, 0.001, &diagnostics);
    require(stopped.max_drive_torque_nm == 0.0 && stopped.max_regen_torque_nm == 0.0,
            "RAPS safety fault stops drive and regen", stopped.max_drive_torque_nm);
    diagnostics.safety_fault = false;
    auto recovery = manager.compute_torque_limit(state, 3000.0, 0.001, &diagnostics);
    require(recovery.max_drive_torque_nm <= 2.0 + 1e-9,
            "drive recovery ramps from the emitted hard-stop limit", recovery.max_drive_torque_nm);
}
} // namespace

int main() {
    test_regen_protective_decreases();
    test_regen_ramp_and_recovery();
    test_drive_protective_decreases();
    test_drive_ramp_and_hard_gate_history();
    if (failures != 0) return 1;
    std::cout << "Protection envelope regressions passed\n";
    return 0;
}
