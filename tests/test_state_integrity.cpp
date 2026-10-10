// PB-01/PB-02/PB-03: committed model epochs survive rejected transactions.
#include "ds_advanced_features.hpp"
#include "ailee_trust_layer/ailee_adapters.hpp"
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
int checks = 0;
int failures = 0;
void check(bool condition, const std::string& label) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}
bool same(double a, double b) {
    return std::memcmp(&a, &b, sizeof(double)) == 0;
}
std::vector<double> fields(const ds::DSState& s) {
    std::vector<double> v = {s.voltage, s.current, s.temperature, s.state_of_charge,
        s.entropy, s.cycle_count, s.degradation, s.phi_magnitude, s.lambda,
        s.energy_psi, s.energy_phi, s.energy_metric, s.energy_total,
        s.charge_throughput_ah, s.capacity_fade, s.time, s.last_update};
    v.insert(v.end(), s.grad_phi.begin(), s.grad_phi.end());
    for (const auto& row : s.g_eff.data) v.insert(v.end(), row.begin(), row.end());
    return v;
}
void equal_state(const ds::DSState& a, const ds::DSState& b, const std::string& label) {
    const auto x = fields(a), y = fields(b);
    for (size_t i = 0; i < x.size(); ++i) check(same(x[i], y[i]), label + " field " + std::to_string(i));
}
template<class F> void rejected(F operation, const std::string& label, bool argument_only = false) {
    bool caught = false;
    try { operation(); } catch (const std::invalid_argument&) { caught = true; }
    catch (const std::runtime_error&) { caught = !argument_only; }
    check(caught, label + " rejected");
}
const double nan = std::numeric_limits<double>::quiet_NaN();
const double inf = std::numeric_limits<double>::infinity();
const double maximum = std::numeric_limits<double>::max();

void coupling_tests() {
    ds::DSCoupling engine;
    ds::DSState s;
    s.voltage = 400; s.current = 10; s.state_of_charge = 0.8;
    engine.update(s, 1);
    const auto before = s;
    // Original PB-02 reproduction must preserve every numerical/history field.
    rejected([&] { engine.update(s, maximum); }, "PB-02 DBL_MAX timestep");
    equal_state(s, before, "PB-02 overflow atomicity");
    s = before;
    for (double dt : {0.0, -1.0, nan, inf}) {
        rejected([&] { engine.update(s, dt); }, "PB-02 invalid dt");
        equal_state(s, before, "PB-02 invalid dt atomicity");
    }
    for (size_t field = 0; field < 3; ++field) {
        auto malformed = before;
        if (field == 0) malformed.voltage = nan;
        if (field == 1) malformed.charge_throughput_ah = inf;
        if (field == 2) malformed.grad_phi[2] = nan;
        const auto snapshot = malformed;
        rejected([&] { engine.update(malformed, 1); }, "PB-02 invalid state");
        equal_state(malformed, snapshot, "PB-02 malformed state atomicity");
    }
    auto reference = before;
    engine.update(s, 1); engine.update(reference, 1);
    equal_state(s, reference, "PB-02 recovery");
    s.current = -0.0;
    const auto signed_zero = s;
    rejected([&] { engine.update(s, inf); }, "PB-02 signed-zero rejection");
    equal_state(s, signed_zero, "PB-02 signed-zero preserved");
}

void equal_pack(const ds::advanced::MultiCellPack& a, const ds::advanced::MultiCellPack& b,
                const std::string& label) {
    const auto& x = a.get_cells(); const auto& y = b.get_cells();
    check(x.size() == y.size(), label + " count");
    for (size_t i = 0; i < x.size(); ++i) {
        equal_state(x[i].state, y[i].state, label + " cell " + std::to_string(i));
        check(x[i].cell_id == y[i].cell_id && x[i].relative_capacity == y[i].relative_capacity &&
              x[i].internal_resistance == y[i].internal_resistance && x[i].is_weak_cell == y[i].is_weak_cell,
              label + " cell metadata");
    }
    check(same(a.get_pack_voltage(), b.get_pack_voltage()), label + " voltage");
    check(same(a.get_pack_current(), b.get_pack_current()), label + " current");
    check(same(a.get_min_cell_voltage(), b.get_min_cell_voltage()), label + " min voltage");
    check(same(a.get_max_cell_voltage(), b.get_max_cell_voltage()), label + " max voltage");
    check(same(a.get_voltage_imbalance(), b.get_voltage_imbalance()), label + " imbalance");
    check(same(a.get_average_temperature(), b.get_average_temperature()), label + " average temp");
    check(same(a.get_temperature_spread(), b.get_temperature_spread()), label + " spread");
    check(same(a.get_min_cell_temperature(), b.get_min_cell_temperature()), label + " min temp");
    check(same(a.get_max_cell_temperature(), b.get_max_cell_temperature()), label + " max temp");
    check(a.get_weak_cell_ids() == b.get_weak_cell_ids(), label + " weak IDs");
    const auto p = a.get_pack_health(100), q = b.get_pack_health(100);
    check(p.remaining_capacity_percent == q.remaining_capacity_percent && p.cycles_to_80_percent == q.cycles_to_80_percent &&
          p.estimated_eol_cycles == q.estimated_eol_cycles && p.confidence == q.confidence &&
          p.warning_triggered == q.warning_triggered && p.degradation_rate == q.degradation_rate &&
          p.time_to_80_percent_years == q.time_to_80_percent_years, label + " health");
}
void pack_tests() {
    const auto profile = ds::advanced::ChemistryLibrary().get_profile(ds::advanced::ChemistryType::NMC);
    ds::advanced::MultiCellPack two(2, profile);
    two.update_all_cells({3.7, 3.7}, {25, 25}, 10, 1);
    const auto two_before = two;
    rejected([&] { two.update_all_cells({3.8, nan}, {30, 30}, 20, 1); }, "PB-01 original two-cell");
    equal_pack(two, two_before, "PB-01 two-cell atomicity");
    ds::advanced::MultiCellPack pack(3, profile);
    pack.update_all_cells({3.6, 3.7, 3.8}, {25, 30, 45}, 10, 1);
    check(pack.get_min_cell_voltage() == 3.6, "PB-01 accepted minimum voltage");
    check(pack.get_max_cell_voltage() == 3.8, "PB-01 accepted maximum voltage");
    const auto before = pack;
    const auto attempt = [&](std::vector<double> v, std::vector<double> t, double current, double dt, const std::string& label) {
        auto candidate = before;
        rejected([&] { candidate.update_all_cells(v, t, current, dt); }, label);
        equal_pack(candidate, before, label);

    };
    for (size_t i = 0; i < 3; ++i) {
        for (double bad : {nan, inf, -1.0, maximum}) {
            std::vector<double> v(3, 3.9), t(3, 35);
            v[i] = bad;
            attempt(v, t, 20, 1, "PB-01 voltage position " + std::to_string(i));
        }
        for (double bad : {nan, inf}) {
            std::vector<double> t(3, 35); t[i] = bad;
            attempt({3.9, 3.9, 3.9}, t, 20, 1, "PB-01 temperature position " + std::to_string(i));
        }
    }
    for (double bad : {nan, inf}) attempt({3.9,3.9,3.9}, {35,35,35}, bad, 1, "PB-01 current");
    attempt({3.9,3.9,3.9}, {35,35,35}, maximum, 10, "PB-01 current arithmetic overflow");
    for (double bad : {0.0, -1.0, nan, inf, maximum}) attempt({3.9,3.9,3.9}, {35,35,35}, 20, bad, "PB-01 dt");
    attempt({3.9,3.9}, {35,35,35}, 20, 1, "PB-01 size");
    attempt({3.9,3.9,3.9}, {maximum/2,maximum/2,maximum/2}, 0, 1, "PB-01 aggregate overflow");
    attempt({3.9,3.9,3.9}, {maximum,-maximum,25}, 0, 1, "PB-01 temperature spread overflow");
    rejected([&] { pack.update_all_cells({3.9,3.9,maximum}, {35,35,35}, 20, 1); }, "PB-01 late arithmetic rejection");
    auto reference = before;
    pack.update_all_cells({3.8,3.7,3.6}, {30,35,40}, -20, 0.01);
    reference.update_all_cells({3.8,3.7,3.6}, {30,35,40}, -20, 0.01);
    equal_pack(pack, reference, "PB-01 recovery");
    // DBL_MAX * 1 is representable here: preserve the pre-existing finite policy.
    auto extreme = before;
    extreme.update_all_cells({3.9,3.9,3.9}, {35,35,35}, maximum, 1);
    check(extreme.get_pack_current() == maximum && extreme.get_cells()[0].state.is_valid(),
          "PB-01 representable extreme current retains valid policy");
    const auto extreme_before = extreme;
    rejected([&] { extreme.update_all_cells({3.9,3.9,3.9}, {35,35,35}, maximum, 10); },
             "PB-01 overflowing extreme current rejected");
    equal_pack(extreme, extreme_before, "PB-01 extreme-history preservation");
}
void energy_consistent(const ds::EnhancedState& r, const ds::DSConfig& config) {
    const auto& s = r.state;
    const double expected = s.voltage * s.state_of_charge * config.nominal_capacity_ah * 3600.0;
    check(s.energy_psi == expected, "PB-03 physical energy uses published inputs");
    check(s.energy_total == s.energy_psi + s.energy_phi + s.energy_metric,
          "PB-03 total energy uses current epoch");
    check(s.time == s.last_update && r.numerical_stability && r.energy_conservation_error == 0,
          "PB-03 accepted epoch is stable and complete");
}
void short_tests() {
    ds::DSConfig config;
    ds::DSEnhancement owner;
    owner.init(config);
    const auto initial = owner.get_state();
    rejected([&] { owner.enhance(400, 0, 25, 0.5, 0.001); }, "PB-03 initial short cycle", true);
    equal_state(owner.get_state(), initial, "PB-03 initial rejection");
    auto accepted = owner.enhance(360, 0, 25, 0.8, 1);
    energy_consistent(accepted, config);
    const auto before = owner.get_state();
    const double error_before = owner.check_energy_conservation();
    for (double dt : {0.001, std::nextafter(config.tau_min, 0.0)}) {
        for (int i = 0; i < 12; ++i) {
            rejected([&] { owner.enhance(400, 20, 30, 0.5, dt); }, "PB-03 repeated short cycle", true);
            equal_state(owner.get_state(), before, "PB-03 no mixed epoch or accumulation");
            check(owner.check_energy_conservation() == error_before, "PB-03 residual preserved");
        }
    }
    ds::DSEnhancement reference; reference.init(config);
    reference.enhance(360, 0, 25, 0.8, 1);
    for (double dt : {config.tau_min, std::nextafter(config.tau_min, inf)}) {
        accepted = owner.enhance(400, 0, 25, 0.5, dt);
        const auto expected = reference.enhance(400, 0, 25, 0.5, dt);
        equal_state(accepted.state, expected.state, "PB-03 supported-step recovery");
        energy_consistent(accepted, config);
        check(accepted.state.energy_psi == 54000000.0, "PB-03 changed voltage/SOC energy");
        check(accepted.state.time > before.time, "PB-03 supported cycle advances model time");
    }
    config.tau_min = 0.001;
    ds::DSEnhancement fast; fast.init(config);
    energy_consistent(fast.enhance(400, 0, 25, 0.5, config.tau_min), config);
    const auto fast_before = fast.get_state();
    rejected([&] { fast.enhance(360, 0, 25, 0.8, 0.0005); }, "PB-03 configured minimum rejection", true);
    equal_state(fast.get_state(), fast_before, "PB-03 configured minimum preservation");
    ds::DSEnhancement fast_reference; fast_reference.init(config);
    fast_reference.enhance(400, 0, 25, 0.5, config.tau_min);
    const auto fast_recovered = fast.enhance(360, 0, 25, 0.8, config.tau_min);
    const auto fast_expected = fast_reference.enhance(360, 0, 25, 0.8, config.tau_min);
    equal_state(fast_recovered.state, fast_expected.state, "PB-03 configured minimum recovery");
    energy_consistent(fast_recovered, config);
    ds::DSCoupling coupling;
    auto direct = before;
    rejected([&] { coupling.update(direct, 0.001); }, "PB-03 direct short cycle", true);
    equal_state(direct, before, "PB-03 direct rejection");
    const auto profile = ds::advanced::ChemistryLibrary().get_profile(ds::advanced::ChemistryType::NMC);
    ds::advanced::MultiCellPack pack(2, profile);
    pack.update_all_cells({3.7,3.7}, {25,25}, 10, 1);
    const auto pack_before = pack;
    rejected([&] { pack.update_all_cells({3.8,3.9}, {30,35}, 20, 0.001); }, "PB-03 pack short cycle", true);
    equal_pack(pack, pack_before, "PB-03 pack epoch preserved");
    ds_plugin::DSBMSMiddleware middleware; middleware.init(75,400);
    middleware.enhance_cycle(360, 0, 25, 0.8, 1);
    const auto diag = middleware.diagnostics();
    rejected([&] { middleware.enhance_cycle(400, 20, 30, 0.5, 0.001); }, "PB-03 middleware short cycle", true);
    const auto after = middleware.diagnostics();
    check(after.update_count == diag.update_count && after.time_since_init_s == diag.time_since_init_s &&
          after.energy_throughput_kwh == diag.energy_throughput_kwh && after.pack_soc_percent == diag.pack_soc_percent &&
          after.ds_entropy == diag.ds_entropy && after.last_update_time_ms == diag.last_update_time_ms,
          "PB-03 middleware accepted history preserved");
    ailee::ev::adapters::BMSMiddlewareAdapter adapter; adapter.initialize();
    adapter.update_cycle(360, 0, 25, 0.8, 1);
    rejected([&] { adapter.update_cycle(400, 20, 30, 0.5, 0.001); }, "PB-03 adapter short cycle", true);
    const auto telemetry = adapter.evaluate(0.1);
    check(telemetry.trust_score == 0 && telemetry.anomaly_detected &&
          telemetry.recommended_level == ailee::ev::GovernanceLevel::LEVEL_3_PROTECTIVE,
          "PB-03 rejected short cycle supplies no current authorization evidence");
}

}
int main(int argc, char** argv) {
    const std::string group = argc > 1 ? argv[1] : "all";
    const auto run = [](void (*test)()) {
        try { test(); } catch (const std::exception& e) {
            check(false, std::string("Unexpected exception: ") + e.what());
        }
    };
    if (group == "coupling" || group == "all") run(coupling_tests);
    if (group == "pack" || group == "all") run(pack_tests);
    if (group == "short" || group == "all") run(short_tests);
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
