#include "ds_advanced_features.hpp"

#include <cmath>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

// Exact hexadecimal output allows byte-for-byte comparison with clean main.
// This probe only uses cycles at or above the configured minimum interval.
void print_state(const std::string& label, const ds::DSState& s) {
    std::cout << label << " " << s.voltage << " " << s.current << " "
              << s.temperature << " " << s.state_of_charge << " "
              << s.entropy << " " << s.cycle_count << " " << s.degradation
              << " " << s.phi_magnitude << " " << s.lambda;
    for (const auto& row : s.g_eff.data) {
        for (double element : row) std::cout << " " << element;
    }
    for (double gradient : s.grad_phi) std::cout << " " << gradient;
    std::cout << " " << s.energy_psi << " " << s.energy_phi << " "
              << s.energy_metric << " " << s.energy_total << " "
              << s.charge_throughput_ah << " " << s.capacity_fade << " "
              << s.time << " " << s.last_update << " " << s.is_valid() << "\n";
}

void print_health(const std::string& label, const ds::HealthPrediction& h) {
    std::cout << label << " " << h.remaining_capacity_percent << " "
              << h.cycles_to_80_percent << " " << h.estimated_eol_cycles << " "
              << h.confidence << " " << h.warning_triggered << " "
              << h.degradation_rate << " " << h.time_to_80_percent_years << "\n";
}

void print_charging(const std::string& label, const ds::OptimalChargingProfile& c) {
    std::cout << label << " " << c.recommended_current_limit << " "
              << c.recommended_voltage_limit << " " << c.recommended_temperature
              << " " << c.estimated_charge_time << " " << c.degradation_impact
              << " " << c.max_safe_current << " " << c.max_safe_voltage << "\n";
}

void print_enhanced(const std::string& label, const ds::EnhancedState& e) {
    print_state(label + ".state", e.state);
    print_health(label + ".health", e.health);
    print_charging(label + ".charging", e.charging);
    std::cout << label << ".enhanced " << e.feen_trust_metric << " "
              << e.degradation_warning << " " << e.ds_confidence << " "
              << e.input_clamped << " " << e.energy_conservation_error << " "
              << e.numerical_stability << "\n";
}

void direct_coupling_probe() {
    ds::DSConfig config;
    config.lambda = 2e-6;
    config.nominal_capacity_ah = 120.0;
    config.nominal_voltage = 410.0;
    config.entropy_weight = 0.6;
    config.phi_decay_rate = 0.0015;
    config.thermodynamic_beta = 1.25;
    ds::DSCoupling coupling(config);
    ds::DSState state;
    state.entropy = 0.17;
    state.charge_throughput_ah = 83.0;
    state.cycle_count = state.charge_throughput_ah / (2.0 * config.nominal_capacity_ah);
    state.degradation = 0.08;
    state.capacity_fade = 0.08;
    state.phi_magnitude = std::sqrt(0.17 * 0.17 + 0.08 * 0.08);
    state.time = 20.0;
    state.last_update = 20.0;
    const double dt_values[] = {config.tau_min, 0.1, 1.0, 10.0, 3600.0};
    for (int cycle = 0; cycle < 15; ++cycle) {
        state.voltage = 300.0 + 8.0 * cycle;
        state.current = (cycle % 2 == 0 ? 1.0 : -1.0) * (10.0 + 20.0 * cycle);
        state.temperature = -10.0 + 5.0 * cycle;
        state.state_of_charge = 0.05 + 0.06 * cycle;
        coupling.update(state, dt_values[cycle % 5]);
        const std::string label = "direct." + std::to_string(cycle);
        print_state(label, state);
        print_health(label + ".health", coupling.predict_health(state, 100.0));
        print_charging(label + ".charging", coupling.optimize_charging(state));
    }
}

void enhancement_probe() {
    ds::DSEnhancement enhancement;
    enhancement.init();
    struct Input { double voltage, current, temperature, soc, dt; };
    const Input cases[] = {
        {360.0, 10.0, 25.0, 0.8, 0.01},
        {370.0, -80.0, 31.0, 0.6, 0.1},
        {400.0, 900.0, 90.0, 1.1, 1.0},
        {310.0, -900.0, -50.0, -0.2, 0.5},
        {390.0, 0.0, 28.0, 0.95, 10.0},
        {385.0, 60.0, 60.0, 0.88, 3600.0},
    };
    for (unsigned cycle = 0; cycle < sizeof(cases) / sizeof(cases[0]); ++cycle) {
        const auto& i = cases[cycle];
        const std::string label = "enhancement." + std::to_string(cycle);
        print_enhanced(label, enhancement.enhance(i.voltage, i.current, i.temperature,
                                                 i.soc, i.dt));
        std::cout << label << ".cumulative_error "
                  << enhancement.check_energy_conservation() << "\n";
    }
}

void pack_probe(int count) {
    ds::advanced::ChemistryLibrary chemistry;
    ds::advanced::MultiCellPack pack(count,
        chemistry.get_profile(ds::advanced::ChemistryType::NMC));
    std::vector<double> voltages(count), temperatures(count);
    const double dt_values[] = {0.01, 0.1, 1.0, 60.0};
    for (int cycle = 0; cycle < 4; ++cycle) {
        for (int cell = 0; cell < count; ++cell) {
            voltages[cell] = 3.05 + 0.01 * (cell % 96) + 0.025 * cycle;
            temperatures[cell] = 18.0 + 0.3 * (cell % 96) + 1.5 * cycle;
        }
        pack.update_all_cells(voltages, temperatures, cycle % 2 ? -45.0 : 30.0,
                              dt_values[cycle]);
        const std::string label = "pack" + std::to_string(count) + "." + std::to_string(cycle);
        for (const auto& cell : pack.get_cells()) {
            const std::string cell_label = label + ".cell" + std::to_string(cell.cell_id);
            print_state(cell_label, cell.state);
            std::cout << cell_label << ".metadata " << cell.cell_id << " "
                      << cell.relative_capacity << " " << cell.internal_resistance
                      << " " << cell.is_weak_cell << "\n";
        }
        std::cout << label << ".statistics " << pack.get_pack_voltage() << " "
                  << pack.get_voltage_imbalance() << " " << pack.get_average_temperature()
                  << " " << pack.get_temperature_spread() << " "
                  << pack.get_min_cell_temperature() << " "
                  << pack.get_max_cell_temperature() << "\n";
        std::cout << label << ".weak_ids";
        for (int cell_id : pack.get_weak_cell_ids()) std::cout << " " << cell_id;
        std::cout << "\n";
        print_health(label + ".health", pack.get_pack_health(100.0));
    }
}

int main() {
    std::cout << std::hexfloat;
    direct_coupling_probe();
    enhancement_probe();
    pack_probe(2);
    pack_probe(96);
}
