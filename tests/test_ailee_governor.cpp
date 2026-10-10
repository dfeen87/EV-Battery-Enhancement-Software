/*
 * ============================================================================
 * UNIT TEST: AILEE HORSEPOWER GOVERNOR & DS TORQUE MANAGER
 * ============================================================================
 *
 * LICENSE: Copyright (c) Don Michael Feeney Jr. Licensed under the MIT License.
 * ============================================================================
 */

#include "ailee_horsepower_governor.hpp"
#include "ds_torque_manager.hpp"
#include <iostream>
#include <cassert>
#include <cmath>
#include <limits>

#ifdef DS_TEST_PYTHON_GOVERNOR
#include <pybind11/embed.h>
#endif

namespace {
RawSignals valid_signals() {
    RawSignals signals;
    signals.torque_nm = 400.0;
    signals.rpm = 4000.0;
    signals.v_batt = 400.0;
    signals.i_batt = 400.0;
    signals.ctx.soc = 80.0;
    signals.ctx.soh = 90.0;
    signals.ctx.temp_c = 30.0;
    return signals;
}

void assert_rejected(const GovernanceDecisionCpp& decision) {
    assert(decision.level == 3);
    assert(decision.trust_score == 0.0);
    assert(decision.used_fallback);
    for (double value : {decision.governed_hp, decision.governed_torque,
                         decision.governed_discharge_current, decision.hp_mech,
                         decision.hp_elec, decision.hp_consistency_score}) {
        assert(std::isfinite(value));
        assert(value == 0.0);
    }
}
}

void test_invalid_governance_evidence() {
    AileeHorsepowerGovernor governor;
    auto signals = valid_signals();
    signals.ctx.sensor_valid = false;
    assert_rejected(governor.evaluate(signals));

    for (double invalid : {std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity(),
                           -std::numeric_limits<double>::infinity()}) {
        for (int field = 0; field < 7; ++field) {
            signals = valid_signals();
            double* values[] = {&signals.torque_nm, &signals.rpm, &signals.v_batt,
                                &signals.i_batt, &signals.ctx.soc, &signals.ctx.soh,
                                &signals.ctx.temp_c};
            *values[field] = invalid;
            assert_rejected(governor.evaluate(signals));
        }
    }

    for (int field = 0; field < 6; ++field) {
        signals = valid_signals();
        double* values[] = {&signals.torque_nm, &signals.rpm, &signals.v_batt,
                            &signals.i_batt, &signals.ctx.soc, &signals.ctx.soh};
        *values[field] = -1.0;
        assert_rejected(governor.evaluate(signals));
    }
    signals = valid_signals();
    signals.v_batt = 0.0;
    assert_rejected(governor.evaluate(signals));
    signals = valid_signals();
    signals.ctx.soc = 100.01;
    assert_rejected(governor.evaluate(signals));
    signals = valid_signals();
    signals.ctx.soh = 100.01;
    assert_rejected(governor.evaluate(signals));

    // Finite inputs can still overflow the derived horsepower calculations.
    signals = valid_signals();
    signals.torque_nm = std::numeric_limits<double>::max();
    signals.rpm = std::numeric_limits<double>::max();
    signals.i_batt = std::numeric_limits<double>::max();
    assert_rejected(governor.evaluate(signals));
    assert(governor.governHorsepower(std::numeric_limits<double>::infinity(),
                                    200.0, valid_signals().ctx) == 0.0);

    // Valid critical evidence retains the configured 25% policy.
    signals = valid_signals();
    signals.ctx.temp_c = 60.0;
    const auto thermal = governor.evaluate(signals);
    assert(thermal.level == 3);
    assert(thermal.governed_torque == 100.0);
    assert(thermal.governed_discharge_current == 100.0);
}

void test_rejected_command_updates_audit_decision() {
    ds::drive::DSAileeTorqueManager manager;
    ds::drive::TorqueCommand command;
    command.requested_torque_nm = 400.0;
    command.motor_rpm = 4000.0;
    command.i_batt = 400.0;
    command.ctx = valid_signals().ctx;
    assert(manager.processTorqueCommand(command).governance_level == 0);
    assert(manager.getLastGovernanceDecision().level == 0);
    command.ctx.sensor_valid = false;
    const auto rejected = manager.processTorqueCommand(command);
    assert(rejected.governance_level == 3);
    assert(rejected.applied_torque_nm == 0.0);
    assert(rejected.applied_hp == 0.0);
    assert(rejected.max_allowed_current_a == 0.0);
    assert_rejected(manager.getLastGovernanceDecision());
    assert(manager.getLastGovernanceDecision().reason == rejected.reason);
}

void test_raps_numerical_rejection_updates_audit_decision() {
    ds::drive::DSAileeTorqueManager manager;
    ds::drive::TorqueCommand command;
    command.requested_torque_nm = 300.0;
    command.motor_rpm = 4000.0;
    command.v_batt = 380.0;
    command.i_batt = 350.0;
    command.ctx.soc = 85.0;
    command.ctx.soh = 95.0;
    command.ctx.temp_c = 30.0;
    assert(manager.processTorqueCommand(command).governance_level == 0);

    // Raw fields and horsepower are finite; the RAPS thermal derivative overflows.
    command.ctx.temp_c = -std::numeric_limits<double>::max();
    const auto rejected = manager.processTorqueCommand(command);
    assert(rejected.governance_level == 3);
    assert(rejected.trust_score == 0.0);
    assert(rejected.applied_torque_nm == 0.0);
    assert(rejected.applied_hp == 0.0);
    assert(rejected.max_allowed_current_a == 0.0);
    assert(rejected.raps_dsm_trip_reason == "NUMERICAL_FAILURE");
    assert_rejected(manager.getLastGovernanceDecision());
    assert(manager.getLastGovernanceDecision().reason == rejected.reason);

    command.ctx.temp_c = 30.0;
    const auto recovered = manager.processTorqueCommand(command);
    assert(recovered.governance_level == 0);
    assert(recovered.applied_torque_nm > 0.0);
    assert(!recovered.raps_dsm_tripped);
}

void test_governed_boost_overflow_rejects_candidate() {
    ds::drive::DSAileeTorqueManager manager, reference;
    raps::ev::RapsEVStabilityMembrane shadow;
    ds::drive::TorqueCommand command;
    command.requested_torque_nm = std::numeric_limits<double>::max();
    command.motor_rpm = 1.0;
    command.v_batt = 400.0;
    const double hp = AileeHorsepowerGovernor::computeMechanicalHp(command.requested_torque_nm, 1.0);
    command.i_batt = hp / (0.4 * 1.34102);
    command.ctx.soc = 80.0;
    command.ctx.soh = 90.0;
    command.ctx.temp_c = 25.0;
    for (int step = 0; step < 1000; ++step) {
        assert(std::isfinite(manager.processTorqueCommand(command).max_allowed_torque_nm));
        reference.processTorqueCommand(command);
        shadow.evaluate(command.v_batt, command.i_batt, command.ctx.temp_c);
    }
    command.i_batt = shadow.getState().filtered_current_a;
    command.ctx.temp_c = 27.0; // Would change accepted thermal history if rejection committed.
    const auto rejected = manager.processTorqueCommand(command);
    assert(rejected.governance_level == 3);
    assert(rejected.trust_score == 0.0);
    assert(rejected.max_allowed_torque_nm == 0.0);
    assert(rejected.max_allowed_current_a == 0.0);
    assert(rejected.applied_torque_nm == 0.0);
    assert(rejected.applied_hp == 0.0);
    assert_rejected(manager.getLastGovernanceDecision());
    assert(manager.getLastGovernanceDecision().reason == rejected.reason);

    command.requested_torque_nm = 300.0;
    command.motor_rpm = 4000.0;
    command.i_batt = 350.0;
    command.v_batt = 380.0;
    command.ctx.temp_c = 27.6;
    const auto recovered = manager.processTorqueCommand(command);
    const auto expected = reference.processTorqueCommand(command);
    assert(recovered.raps_membrane_stability == expected.raps_membrane_stability);
    assert(recovered.applied_torque_nm == expected.applied_torque_nm);
    assert(std::isfinite(recovered.max_allowed_torque_nm));
}

#ifdef DS_TEST_PYTHON_GOVERNOR
void test_python_governor_result_validation() {
    AileeHorsepowerGovernor initializer;
    namespace py = pybind11;
    py::gil_scoped_acquire gil;
    auto module = py::module_::import("ds_ev_enhancer");
    py::object original = module.attr("evaluate_governance");
    const char* numeric_keys[] = {"level", "governed_hp", "governed_torque",
        "governed_discharge_current", "trust_score", "hp_consistency_score",
        "hp_mech", "hp_elec"};
    for (const char* key : numeric_keys) {
        for (bool use_boolean : {true, false}) {
            py::dict result;
            result["level"] = 0;
            result["governed_hp"] = 224.68;
            result["governed_torque"] = 400.0;
            result["governed_discharge_current"] = 400.0;
            result["trust_score"] = 1.0;
            result["hp_consistency_score"] = 0.95;
            result["hp_mech"] = 224.68;
            result["hp_elec"] = 214.56;
            result["reason"] = "TEST_INJECTED_DECISION";
            result["used_fallback"] = false;
            if (use_boolean) {
                result[key] = true;
            } else {
                result[key] = std::numeric_limits<double>::quiet_NaN();
            }
            module.attr("evaluate_governance") = py::cpp_function(
                [result](py::args) { return result; });
            AileeHorsepowerGovernor governor;
            assert_rejected(governor.evaluate(valid_signals()));
        }
    }
    module.attr("evaluate_governance") = py::cpp_function(
        [](py::args) { return py::dict(); });
    {
        AileeHorsepowerGovernor governor;
        assert_rejected(governor.evaluate(valid_signals()));
    }
    module.attr("evaluate_governance") = original;
}
#endif

void test_hp_calculations() {
    std::cout << "[TEST] Mechanical & Electrical HP Calculations..." << std::endl;
    // Torque 400 Nm at 4000 RPM -> (400 * 4000) / 7121.23 = 224.681 HP
    double hp_mech = AileeHorsepowerGovernor::computeMechanicalHp(400.0, 4000.0);
    assert(std::abs(hp_mech - 224.681) < 0.1);

    // Battery 400V, 400A -> 160 kW -> 160 * 1.34102 = 214.563 HP
    double hp_elec = AileeHorsepowerGovernor::computeElectricalHp(400.0, 400.0);
    assert(std::abs(hp_elec - 214.563) < 0.1);

    double consistency = AileeHorsepowerGovernor::hpConsistencyScore(hp_mech, hp_elec);
    assert(consistency > 0.90);
    std::cout << "  -> PASSED! hp_mech=" << hp_mech << ", hp_elec=" << hp_elec << ", consistency=" << consistency << std::endl;
}

void test_ailee_governor_levels() {
    std::cout << "[TEST] AILEE Horsepower Governor Evaluation..." << std::endl;
    AileeHorsepowerGovernor governor;

    // Normal conditions
    RawSignals normal_sig;
    normal_sig.torque_nm = 300.0;
    normal_sig.rpm = 4000.0;
    normal_sig.v_batt = 380.0;
    normal_sig.i_batt = 350.0;
    normal_sig.ctx.soc = 85.0;
    normal_sig.ctx.soh = 95.0;
    normal_sig.ctx.temp_c = 30.0;
    normal_sig.ctx.sensor_valid = true;

    auto dec_normal = governor.evaluate(normal_sig);
    assert(dec_normal.level == 0);
    assert(dec_normal.trust_score >= 0.85);
    std::cout << "  -> Normal Mode PASSED! Level: " << dec_normal.level << ", Governed HP: " << dec_normal.governed_hp << std::endl;

    // Elevated Temperature -> Level 1 or 2
    RawSignals hot_sig = normal_sig;
    hot_sig.ctx.temp_c = 52.0;
    auto dec_hot = governor.evaluate(hot_sig);
    assert(dec_hot.level >= 1);
    assert(dec_hot.governed_hp < dec_normal.governed_hp);
    std::cout << "  -> Hot Battery Derating PASSED! Level: " << dec_hot.level << ", Governed HP: " << dec_hot.governed_hp << std::endl;

    // Sensor Fault -> Level 3 Protective
    RawSignals fault_sig = normal_sig;
    fault_sig.ctx.sensor_valid = false;
    auto dec_fault = governor.evaluate(fault_sig);
    assert(dec_fault.level == 3);
    assert(dec_fault.used_fallback == true);
    std::cout << "  -> Sensor Fault Protective Mode PASSED! Level: " << dec_fault.level << std::endl;
}

void test_ds_torque_manager() {
    std::cout << "[TEST] DS Torque Manager Integration..." << std::endl;
    ds::drive::DSAileeTorqueManager torque_mgr;

    ds::drive::TorqueCommand cmd;
    cmd.requested_torque_nm = 400.0;
    cmd.motor_rpm = 4000.0;
    cmd.v_batt = 400.0;
    cmd.i_batt = 400.0;
    cmd.ctx.soc = 80.0;
    cmd.ctx.soh = 90.0;
    cmd.ctx.temp_c = 30.0;
    cmd.ctx.sensor_valid = true;

    auto out = torque_mgr.processTorqueCommand(cmd);
    assert(out.applied_torque_nm <= 400.0);
    assert(out.governance_level == 0);
    std::cout << "  -> Normal Torque Command PASSED! Applied Torque: " << out.applied_torque_nm << " Nm" << std::endl;

    // Command under Level 2 Hard Ceiling
    cmd.ctx.temp_c = 55.0; // High temp
    auto out_derated = torque_mgr.processTorqueCommand(cmd);
    assert(out_derated.governance_level >= 2);
    assert(out_derated.applied_torque_nm < out.applied_torque_nm);
    assert(out_derated.derating_active == true);
    std::cout << "  -> Derated Torque Command PASSED! Governed Torque: " << out_derated.applied_torque_nm << " Nm" << std::endl;
}

int main() {
    std::cout << "===========================================" << std::endl;
    std::cout << "RUNNING AILEE GOVERNOR & TORQUE MANAGER TESTS" << std::endl;
    std::cout << "===========================================" << std::endl;

    try {
        test_hp_calculations();
        test_ailee_governor_levels();
        test_ds_torque_manager();
        test_invalid_governance_evidence();
        test_rejected_command_updates_audit_decision();
        test_raps_numerical_rejection_updates_audit_decision();
        test_governed_boost_overflow_rejects_candidate();
#ifdef DS_TEST_PYTHON_GOVERNOR
        test_python_governor_result_validation();
#endif
        std::cout << "===========================================" << std::endl;
        std::cout << "ALL C++ TESTS PASSED SUCCESSFULLY!" << std::endl;
        std::cout << "===========================================" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
