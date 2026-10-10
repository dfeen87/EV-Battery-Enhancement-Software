/*
 * ============================================================================
 * DS BATTERY ENHANCEMENT - MIDDLEWARE TESTS
 * ============================================================================
 * 
 * Tests for BMS middleware integration layer
 * 
 * ============================================================================
 */

#include "ds_bms_middleware_v2.hpp"
#include "ailee_trust_layer/ailee_adapters.hpp"
#include <iostream>
#include <cassert>
#include <limits>

bool test_middleware_initialization() {
    std::cout << "Testing middleware initialization..." << std::flush;
    
    ds_plugin::DSBMSMiddleware middleware;
    middleware.init(75.0, 400.0);
    
    std::cout << " PASS\n";
    return true;
}

bool test_middleware_enhance_cycle() {
    std::cout << "Testing middleware enhance cycle..." << std::flush;
    
    ds_plugin::DSBMSMiddleware middleware;
    middleware.init(75.0, 400.0);
    
    auto result = middleware.enhance_cycle(360.0, 50.0, 25.0, 0.8, 1.0);
    (void)result;
    
    assert(result.state.voltage > 0);
    assert(result.ds_confidence >= 0 && result.ds_confidence <= 1.0);
    
    std::cout << " PASS\n";
    return true;
}

bool test_middleware_diagnostics() {
    std::cout << "Testing middleware diagnostics..." << std::flush;
    
    ds_plugin::DSBMSMiddleware middleware;
    middleware.init(75.0, 400.0);
    
    middleware.enhance_cycle(360.0, 50.0, 25.0, 0.8, 1.0);
    
    auto diag = middleware.diagnostics();
    
    assert(diag.pack_soh_percent > 0);
    assert(diag.pack_soc_percent >= 0 && diag.pack_soc_percent <= 100);
    assert(diag.update_count > 0);
    
    std::cout << " PASS\n";
    return true;
}

bool test_middleware_safety() {
    std::cout << "Testing middleware safety monitoring..." << std::flush;
    
    ds_plugin::DSBMSMiddleware middleware;
    
    ds_plugin::MiddlewareConfig config;
    config.nominal_capacity_ah = 75.0;
    config.nominal_voltage = 400.0;
    config.enable_safety_monitoring = true;
    config.safety_limits.max_discharge_current = 100.0;
    
    middleware.init_advanced(config);
    
    // Normal operation - should pass
    middleware.enhance_cycle(360.0, 50.0, 25.0, 0.8, 1.0);
    
    auto diag = middleware.diagnostics();
    assert(!diag.safety_fault); // Should not have safety fault
    
    std::cout << " PASS\n";
    return true;
}

bool test_raw_sensor_safety_evidence() {
    std::cout << "Testing raw sensor safety evidence..." << std::flush;
    ds_plugin::DSBMSMiddleware middleware;
    middleware.init(75.0, 400.0);

    // DS normalization bounds model inputs; it must not conceal the original
    // thermal/current violation from the independent safety monitor.
    auto hot = middleware.enhance_cycle(360.0, 10.0, 100.0, 0.8, 1.0);
    assert(hot.state.temperature == 60.0);
    assert(hot.input_clamped);
    assert(middleware.diagnostics().safety_fault);
    assert(middleware.diagnostics().thermal_warning);

    auto discharge = middleware.enhance_cycle(360.0, 2000.0, 25.0, 0.8, 1.0);
    assert(discharge.state.current == 500.0);
    assert(discharge.input_clamped);
    assert(middleware.diagnostics().safety_fault);
    auto charge = middleware.enhance_cycle(360.0, -2000.0, 25.0, 0.8, 1.0);
    assert(charge.state.current == -500.0);
    assert(middleware.diagnostics().safety_fault);

    // The configured safety policy still owns its exact boundaries and recovery.
    middleware.enhance_cycle(360.0, 600.0, 60.0, 0.8, 1.0);
    assert(!middleware.diagnostics().safety_fault);
    assert(!middleware.diagnostics().thermal_warning);
    middleware.enhance_cycle(360.0, -250.0, 25.0, 0.8, 1.0);
    assert(!middleware.diagnostics().safety_fault);

    ds_plugin::MiddlewareConfig config;
    config.ds_config.max_current = 100.0;
    config.safety_limits.max_discharge_current = 200.0;
    config.safety_limits.max_operating_temp = 50.0;
    middleware.init_advanced(config);
    auto stricter = middleware.enhance_cycle(360.0, 250.0, 55.0, 0.8, 1.0);
    assert(stricter.state.current == 100.0);
    assert(middleware.diagnostics().safety_fault);
    assert(middleware.diagnostics().thermal_warning);
    middleware.enhance_cycle(360.0, 200.0, 50.0, 0.8, 1.0);
    assert(!middleware.diagnostics().safety_fault);

    ailee::ev::adapters::BMSMiddlewareAdapter adapter;
    assert(adapter.initialize());
    adapter.update_cycle(360.0, 10.0, 100.0, 0.8, 1.0);
    const auto telemetry = adapter.evaluate(1.0);
    assert(telemetry.trust_score == 0.0);
    assert(telemetry.anomaly_detected);
    assert(telemetry.recommended_level == ailee::ev::GovernanceLevel::LEVEL_3_PROTECTIVE);
    assert(adapter.get_recommended_governance_level() == ailee::ev::GovernanceLevel::LEVEL_3_PROTECTIVE);
    adapter.update_cycle(360.0, 10.0, 25.0, 0.8, 1.0);
    assert(adapter.get_recommended_governance_level() == ailee::ev::GovernanceLevel::LEVEL_0_NORMAL);
    std::cout << " PASS\n";
    return true;
}

bool test_non_finite_soc_rejection_is_atomic() {
    std::cout << "Testing original SOC evidence rejection..." << std::flush;
    ds_plugin::DSBMSMiddleware middleware;
    middleware.init(75.0, 400.0);
    middleware.enhance_cycle(360.0, 10.0, 25.0, 0.8, 1.0);
    const auto before = middleware.diagnostics();
    const double invalid[] = {std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN()};
    for (double soc : invalid) {
        bool rejected = false;
        try {
            middleware.enhance_cycle(400.0, 20.0, 30.0, soc, 1.0);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        assert(rejected);
        assert(middleware.diagnostics().update_count == before.update_count);
        assert(middleware.diagnostics().time_since_init_s == before.time_since_init_s);
        assert(middleware.diagnostics().energy_throughput_kwh == before.energy_throughput_kwh);
        assert(middleware.diagnostics().pack_soc_percent == before.pack_soc_percent);
    }
    // Finite out-of-domain measurements retain the established normalization.
    assert(middleware.enhance_cycle(360.0, 10.0, 25.0, 2.0, 1.0).state.state_of_charge == 1.0);
    assert(middleware.enhance_cycle(360.0, 10.0, 25.0, -1.0, 1.0).state.state_of_charge == 0.0);
    std::cout << " PASS\n";
    return true;
}

int main() {
    std::cout << "============================================================================\n";
    std::cout << "DS MIDDLEWARE TESTS\n";
    std::cout << "============================================================================\n\n";
    
    try {
        bool all_passed = true;
        
        all_passed &= test_middleware_initialization();
        all_passed &= test_middleware_enhance_cycle();
        all_passed &= test_middleware_diagnostics();
        all_passed &= test_middleware_safety();
        all_passed &= test_raw_sensor_safety_evidence();
        all_passed &= test_non_finite_soc_rejection_is_atomic();
        
        std::cout << "\n============================================================================\n";
        if (all_passed) {
            std::cout << "✓ All middleware tests PASSED\n";
        } else {
            std::cout << "✗ Some tests FAILED\n";
            return 1;
        }
        std::cout << "============================================================================\n";
        
    } catch (const std::exception& e) {
        std::cerr << "\n❌ Error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
