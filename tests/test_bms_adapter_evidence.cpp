#include "ailee_trust_layer/ailee_adapters.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

using ailee::ev::CompartmentTelemetry;
using ailee::ev::GovernanceLevel;
using ailee::ev::TrustGate;
using ailee::ev::adapters::BMSMiddlewareAdapter;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void check_unavailable(BMSMiddlewareAdapter& adapter, const char* context) {
    const CompartmentTelemetry telemetry = adapter.evaluate(0.1);
    const auto decision = TrustGate().evaluate_telemetry(&telemetry, 1);
    const bool unavailable = telemetry.health_score == 0.0 &&
        telemetry.trust_score == 0.0 && telemetry.anomaly_detected &&
        telemetry.recommended_level == GovernanceLevel::LEVEL_3_PROTECTIVE &&
        adapter.get_recommended_governance_level() == GovernanceLevel::LEVEL_3_PROTECTIVE;
    check(unavailable, context);
    check(telemetry.status_message.data[0] != '\0', "Unavailable evidence has a diagnostic reason");
    check(decision.level == GovernanceLevel::LEVEL_3_PROTECTIVE &&
        decision.overall_trust_score == 0.0 && !decision.learning_allowed &&
        decision.fallback_active, "Unavailable adapter evidence prevents normal authorization");
    if (!unavailable) {
        std::cerr << "  " << context << ": health=" << telemetry.health_score
                  << ", trust=" << telemetry.trust_score
                  << ", level=" << static_cast<int>(telemetry.recommended_level)
                  << ", gate level=" << static_cast<int>(decision.level)
                  << ", learning=" << decision.learning_allowed << '\n';
    }
}

CompartmentTelemetry check_healthy(BMSMiddlewareAdapter& adapter) {
    const CompartmentTelemetry telemetry = adapter.evaluate(0.1);
    const auto decision = TrustGate().evaluate_telemetry(&telemetry, 1);
    check(telemetry.health_score > 0.99 && telemetry.trust_score > 0.99 &&
        !telemetry.anomaly_detected &&
        telemetry.recommended_level == GovernanceLevel::LEVEL_0_NORMAL &&
        adapter.get_recommended_governance_level() == GovernanceLevel::LEVEL_0_NORMAL,
        "Successful healthy cycle retains normal adapter policy");
    check(decision.level == GovernanceLevel::LEVEL_0_NORMAL && decision.learning_allowed,
        "Successful healthy cycle permits normal trust-gate authorization");
    return telemetry;
}

void test_missing_cycle_and_reset() {
    BMSMiddlewareAdapter adapter;
    check_unavailable(adapter, "Construction does not supply cycle evidence");
    bool rejected = false;
    try {
        adapter.update_cycle(400.0, 10.0, 25.0, 0.5, 1.0);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected, "Uninitialized cycle still propagates middleware rejection");
    check_unavailable(adapter, "Rejected uninitialized cycle supplies no evidence");

    check(adapter.initialize(), "Adapter initialization succeeds");
    check_unavailable(adapter, "Initialization does not supply cycle evidence");
    adapter.update_cycle(400.0, 10.0, 25.0, 0.5, 1.0);
    const auto healthy = check_healthy(adapter);
    const auto repeated = check_healthy(adapter);
    check(healthy.health_score == repeated.health_score &&
        healthy.trust_score == repeated.trust_score,
        "Evaluation preserves successfully observed cycle evidence");

    check(adapter.initialize(), "Repeated initialization succeeds");
    check_unavailable(adapter, "Repeated initialization invalidates previous cycle evidence");
    adapter.update_cycle(400.0, 10.0, 25.0, 0.5, 1.0);
    check_healthy(adapter);
    adapter.reset_to_baseline();
    check_unavailable(adapter, "Baseline reset invalidates previous cycle evidence");
    adapter.update_cycle(400.0, 10.0, 25.0, 0.5, 1.0);
    check_healthy(adapter);
}

void test_rejected_latest_cycle_and_recovery() {
    BMSMiddlewareAdapter adapter;
    BMSMiddlewareAdapter reference;
    adapter.initialize();
    reference.initialize();
    adapter.update_cycle(400.0, 10.0, 25.0, 0.5, 1.0);
    reference.update_cycle(400.0, 10.0, 25.0, 0.5, 1.0);
    check_healthy(adapter);

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    struct InvalidCycle {
        double voltage;
        double current;
        double temperature;
        double soc;
        double dt;
        bool numerical_failure = false;
    };
    const InvalidCycle invalid_cycles[] = {
        {400.0, 10.0, nan, 0.5, 1.0},
        {0.0, 10.0, 25.0, 0.5, 1.0},
        {400.0, infinity, 25.0, 0.5, 1.0},
        {400.0, 10.0, 25.0, infinity, 1.0},
        {400.0, 10.0, 25.0, 0.5, 0.0},
        {400.0, 10.0, 25.0, 0.5, -1.0},
        {400.0, 10.0, 25.0, 0.5, infinity},
        {400.0, 10.0, 25.0, 0.5, std::numeric_limits<double>::max(), true},
    };
    for (const auto& cycle : invalid_cycles) {
        bool rejected = false;
        try {
            adapter.update_cycle(cycle.voltage, cycle.current,
                cycle.temperature, cycle.soc, cycle.dt);
        } catch (const std::invalid_argument&) {
            rejected = !cycle.numerical_failure;
        } catch (const std::runtime_error&) {
            rejected = cycle.numerical_failure;
        }
        check(rejected, "Malformed latest cycle preserves the middleware exception contract");
        check_unavailable(adapter, "Rejected latest cycle invalidates earlier authorization evidence");
        check_unavailable(adapter, "Evaluation cannot restore rejected-cycle authorization evidence");

        adapter.update_cycle(400.0, 10.0, 25.0, 0.5, 1.0);
        reference.update_cycle(400.0, 10.0, 25.0, 0.5, 1.0);
        const auto recovered = check_healthy(adapter);
        const auto expected = check_healthy(reference);
        check(recovered.health_score == expected.health_score &&
            recovered.trust_score == expected.trust_score,
            "Rejected calls preserve the middleware model used by later successful cycles");
    }
}

void test_successful_fault_cycle() {
    BMSMiddlewareAdapter adapter;
    adapter.initialize();
    adapter.update_cycle(400.0, 10.0, 25.0, 0.5, 1.0);
    check_healthy(adapter);
    adapter.update_cycle(400.0, 10.0, 100.0, 0.5, 1.0);
    const auto telemetry = adapter.evaluate(0.1);
    const auto decision = TrustGate().evaluate_telemetry(&telemetry, 1);
    check(telemetry.trust_score == 0.0 && telemetry.anomaly_detected &&
        telemetry.recommended_level == GovernanceLevel::LEVEL_3_PROTECTIVE &&
        adapter.get_recommended_governance_level() == GovernanceLevel::LEVEL_3_PROTECTIVE,
        "Successfully observed raw thermal fault retains protective adapter policy");
    check(decision.level == GovernanceLevel::LEVEL_3_PROTECTIVE && !decision.learning_allowed,
        "Successfully observed fault still denies normal authorization");
    adapter.update_cycle(400.0, 10.0, 25.0, 0.5, 1.0);
    check_healthy(adapter);
}

} // namespace

int main() {
    test_missing_cycle_and_reset();
    test_rejected_latest_cycle_and_recovery();
    test_successful_fault_cycle();
    if (failures != 0) {
        std::cerr << failures << " BMS adapter evidence checks failed\n";
        return 1;
    }
    std::cout << "BMS adapter evidence checks passed\n";
    return 0;
}
