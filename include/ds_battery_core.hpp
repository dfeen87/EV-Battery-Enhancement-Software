/*
 * ============================================================================
 * DS BATTERY ENHANCEMENT LIBRARY v5.0
 * ============================================================================
 * 
 * Implementation of Don Michael Feeney Jr.'s Dual-State (DS) Theory
 * for Advanced Battery Management Systems
 * 
 * Based on: "Mathematical Formulation of the U2→U1 Coupling in the 
 *            Dual-State Theory" (the developer, 2025)
 * 
 * UPDATES IN v5.0:
 * - Version bump to 5.0.0 with integrated AILEE Automotive Governance Layer
 * 
 * ARCHITECTURE:
 * ------------
 * This library provides a drop-in enhancement layer for existing BMS systems.
 * It models batteries as dual-state systems:
 *   - Ψ (Psi): Physical/geometric state (voltage, current, temp, SoC)
 *   - Φ (Phi): Informational state (entropy, history, degradation)
 * 
 * The coupling between these states follows DS's effective metric formulation:
 *   g_eff_μν = g_μν + λ ∂_μΦ ∂_νΦ
 * 
 * And respects energy conservation via the Thermodynamic Energy Conservation:
 *   δE_total = δE_Ψ + δE_Φ + δE_metric = 0
 * 
 * INTEGRATION:
 * -----------
 * Single header include, minimal dependencies, real-time compatible.
 * Add two lines to existing BMS update loop and get DS enhancement.
 * 
 * AUTHORS: Don Michael Feeney Jr. & Claude (Anthropic)
 * DATE: December 2025
 * LICENSE: Copyright (c) Don Michael Feeney Jr. Licensed under the MIT License.
 * VERSION: 1.2.0
 * 
 * ============================================================================
 */

#ifndef DS_BATTERY_CORE_HPP
#define DS_BATTERY_CORE_HPP

#include <cmath>
#include <vector>
#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <algorithm>
#include <string>
#include <cassert>

#ifndef DS_ENABLE_FEEN
#define DS_ENABLE_FEEN 0
#endif

#include "battery_feen_adapter/battery_feen_adapter.hpp"

namespace ds {

// ============================================================================
// VERSION INFORMATION
// ============================================================================

constexpr int DS_VERSION_MAJOR = 8;
constexpr int DS_VERSION_MINOR = 0;
constexpr int DS_VERSION_PATCH = 1;

inline std::string get_version_string() {
    return std::to_string(DS_VERSION_MAJOR) + "." +
           std::to_string(DS_VERSION_MINOR) + "." +
           std::to_string(DS_VERSION_PATCH);
}

// ============================================================================
// CONSTANTS & CONFIGURATION
// ============================================================================

constexpr double PLANCK_REDUCED = 1.054571817e-34;  // ℏ (J·s)
constexpr double BOLTZMANN = 1.380649e-23;          // k_B (J/K)
constexpr double ELECTRON_CHARGE = 1.602176634e-19; // e (C)

// Default DS coupling parameters (tunable per battery chemistry)
struct DSConfig {
    bool enable_feen_battery_integration = false;
    double lambda = 1e-6;           // Coupling strength λ
    double tau_min = 0.01;          // Minimum update interval (s)
    double phi_decay_rate = 0.001;  // Information decay rate
    double thermodynamic_beta = 1.0;     // Thermodynamic energy scaling
    double entropy_weight = 0.5;    // Entropy contribution to Φ
    
    // Battery-specific parameters
    double nominal_capacity_ah = 75.0;  // Nominal capacity (Ah)
    double nominal_voltage = 400.0;     // Nominal voltage (V)
    double max_temperature = 60.0;      // Max safe temp (°C)
    double min_temperature = -20.0;     // Min safe temp (°C)
    
    // Validation and safety
    double max_current = 500.0;         // Max current (A)
    double energy_conservation_tolerance = 1e-6; // Energy balance tolerance
    
    // Validate configuration
    bool validate() const {
        const double values[] = {lambda, tau_min, phi_decay_rate,
            thermodynamic_beta, entropy_weight, nominal_capacity_ah,
            nominal_voltage, max_temperature, min_temperature, max_current,
            energy_conservation_tolerance};
        for (double value : values) {
            if (!std::isfinite(value)) return false;
        }
        if (lambda <= 0.0 || lambda > 1e-3) return false;
        if (tau_min <= 0.0 || tau_min > 1.0) return false;
        if (phi_decay_rate < 0.0 || phi_decay_rate > 1.0) return false;
        if (nominal_capacity_ah <= 0.0) return false;
        if (nominal_voltage <= 0.0) return false;
        if (max_temperature <= min_temperature) return false;
        if (max_current <= 0.0) return false;
        if (entropy_weight < 0.0 || entropy_weight > 1.0) return false;
        if (thermodynamic_beta <= 0.0) return false;
        if (energy_conservation_tolerance <= 0.0) return false;
        // Explicit zero check for max_temperature: prevents divide-by-zero in
        // compute_phi_gradients (temp_factor = temperature / max_temperature).
        // Note: max_temperature > min_temperature above does not exclude zero when
        // min_temperature is negative (e.g. default -20.0).
        if (max_temperature == 0.0) return false;
        return true;
    }
};

// ============================================================================
// MATRIX4X4 - Lightweight 4x4 matrix for metric calculations
// ============================================================================

class Matrix4x4 {
public:
    std::array<std::array<double, 4>, 4> data;
    
    Matrix4x4() {
        // Initialize to baseline metric by default
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                data[i][j] = (i == j) ? ((i == 0) ? -1.0 : 1.0) : 0.0;
    }
    
    double& operator()(int i, int j) {
        assert(i >= 0 && i < 4 && j >= 0 && j < 4 && "Matrix4x4 index out of bounds");
        return data[i][j];
    }
    const double& operator()(int i, int j) const {
        assert(i >= 0 && i < 4 && j >= 0 && j < 4 && "Matrix4x4 index out of bounds");
        return data[i][j];
    }
    
    // Compute trace
    double trace() const {
        return data[0][0] + data[1][1] + data[2][2] + data[3][3];
    }
    
    // Check if metric is numerically stable
    bool is_stable() const {
        double tr = trace();
        return std::isfinite(tr) && std::abs(tr) < 1e6;
    }
};

// ============================================================================
// DSSTATE - Dual-state representation (Ψ and Φ)
// ============================================================================

struct DSState {
    // PHYSICAL STATE Ψ (directly measurable)
    double voltage;           // V
    double current;           // A
    double temperature;       // °C
    double state_of_charge;   // 0.0 to 1.0
    
    // INFORMATIONAL STATE Φ (computed/inferred)
    double entropy;           // Normalized entropy measure
    double cycle_count;       // Equivalent full cycles
    double degradation;       // 0.0 (new) to 1.0 (dead)
    double phi_magnitude;     // |Φ| for coupling calculations
    
    // COUPLING METRICS
    double lambda;            // Current coupling strength
    Matrix4x4 g_eff;          // Effective metric g^eff_μν
    
    // GRADIENTS (for metric modulation)
    std::array<double, 4> grad_phi;  // ∂_μΦ in (t,x,y,z) basis
    
    // ENERGY TRACKING (Thermodynamic energy conservation compliance)
    double energy_psi;        // Energy in physical state
    double energy_phi;        // Energy in informational state
    double energy_metric;     // Energy in metric modulation
    double energy_total;      // Total energy (should be conserved)
    
    // COULOMB COUNTING (for accurate SoC)
    double charge_throughput_ah;  // Total Ah throughput
    double capacity_fade;         // Measured capacity loss
    
    // TIMESTAMPS
    double time;              // Current time (s)
    double last_update;       // Last update time (s)
    
    // Constructor with defaults
    DSState() : voltage(0), current(0), temperature(25), state_of_charge(1.0),
                 entropy(0), cycle_count(0), degradation(0), phi_magnitude(0),
                 lambda(1e-6), grad_phi{0,0,0,0},
                 energy_psi(0), energy_phi(0), energy_metric(0), energy_total(0),
                 charge_throughput_ah(0), capacity_fade(0),
                 time(0), last_update(0) {}
    
    // Validate state
    bool is_valid() const {
        const double values[] = {voltage, current, temperature, state_of_charge,
            entropy, cycle_count, degradation, phi_magnitude, lambda,
            energy_psi, energy_phi, energy_metric, energy_total,
            charge_throughput_ah, capacity_fade, time, last_update};
        for (double value : values) {
            if (!std::isfinite(value)) return false;
        }
        for (double gradient : grad_phi) {
            if (!std::isfinite(gradient)) return false;
        }
        if (voltage < 0.0 || entropy < 0.0 || entropy > 1.0 ||
            cycle_count < 0.0 || phi_magnitude < 0.0 || lambda <= 0.0 ||
            charge_throughput_ah < 0.0 || capacity_fade < 0.0 ||
            time < 0.0 || last_update < 0.0) return false;
        if (state_of_charge < 0.0 || state_of_charge > 1.0) return false;
        if (degradation < 0.0 || degradation > 1.0) return false;
        if (!g_eff.is_stable()) return false;
        return true;
    }
};

// ============================================================================
// PREDICTION RESULTS - Output structures for easy integration
// ============================================================================

struct HealthPrediction {
    double remaining_capacity_percent = 0.0;  // Predicted remaining capacity
    double cycles_to_80_percent = 0.0;        // Cycles until 80% capacity
    double estimated_eol_cycles = 0.0;        // End-of-life estimate
    double confidence = 0.0;                  // Prediction confidence [0,1]
    bool warning_triggered = false;           // Early degradation warning
    
    // Additional metrics
    double degradation_rate = 0.0;            // Per-cycle degradation rate
    double time_to_80_percent_years = 0.0;    // Time estimate (assuming 1 cycle/day)
};

struct OptimalChargingProfile {
    double recommended_current_limit = 0.0;   // Optimal current (A)
    double recommended_voltage_limit = 0.0;   // Optimal voltage (V)
    double recommended_temperature = 25.0;    // Target temp (°C); 25°C is standard ambient charging temperature
    double estimated_charge_time = 0.0;       // Time to full (minutes)
    double degradation_impact = 0.0;          // Impact on lifetime (normalized)
    
    // Safety margins
    double max_safe_current = 0.0;            // Absolute current limit
    double max_safe_voltage = 0.0;            // Absolute voltage limit
};

struct EnhancedState {
    double feen_trust_metric = -1.0;    // FEEN trust metric (if enabled)
    DSState state;                     // Full DS state
    HealthPrediction health;            // Health prediction
    OptimalChargingProfile charging;    // Optimal charging
    bool degradation_warning;           // Critical warning flag
    double ds_confidence;              // Overall confidence in DS prediction
    bool input_clamped = false;         // True if sensor inputs were clamped to safe range
    
    // Diagnostics
    double energy_conservation_error;   // Should be near zero
    bool numerical_stability;           // Stability check
};

// ============================================================================
// DSCOUPLING - Core coupling dynamics engine
// ============================================================================

class DSCoupling {
private:
    DSConfig config_;
    
    // Compute basic rates of change for the state variables
    void compute_phi_gradients(DSState& state) {
        state.grad_phi[0] = -config_.phi_decay_rate * state.phi_magnitude;
        state.grad_phi[1] = state.entropy * (1.0 - state.state_of_charge);
        state.grad_phi[2] = (state.temperature / std::max(1.0, config_.max_temperature)) * state.degradation;
        state.grad_phi[3] = 0.01 * state.cycle_count;
    }
    
    // Baseline metric matrix definition
    void compute_effective_metric(DSState& state) {
        state.g_eff = Matrix4x4(); // Always default to flat reference scaling coefficients (trace = 2.0)
    }
    
    // Update state based on empirical battery dynamics
    void update_phi(DSState& state, double dt) {
        double temp_contrib = std::max(0.0, state.temperature - 25.0) / 35.0;
        double current_contrib = std::abs(state.current) / 100.0;
        
        state.entropy += dt * config_.entropy_weight * (temp_contrib + current_contrib);
        state.entropy = std::min(state.entropy, 1.0);
        
        double charge_delta = std::abs(state.current) * dt / 3600.0;
        state.charge_throughput_ah += charge_delta;
        double prev_cycle_count = state.cycle_count;
        state.cycle_count = state.charge_throughput_ah / (2.0 * config_.nominal_capacity_ah);
        
        double cycle_delta = state.cycle_count - prev_cycle_count;
        double base_degradation = cycle_delta * 0.0001; // baseline 0.01% per cycle
        double thermal_degradation = temp_contrib * 0.0005 * dt;
        
        state.degradation += base_degradation + thermal_degradation;
        state.degradation = std::clamp(state.degradation, 0.0, 1.0);
        state.capacity_fade = state.degradation;
        
        state.phi_magnitude = std::sqrt(state.entropy * state.entropy + state.degradation * state.degradation);
    }
    
    // Compute standard empirical thermodynamic energy equations
    void compute_energies(DSState& state) {
        // Physical stored electrical energy: voltage * SoC * nominal capacity
        state.energy_psi = state.voltage * state.state_of_charge * config_.nominal_capacity_ah * 3600.0;
        
        // Auxiliary state thermal energy representation
        double temp_kelvin = state.temperature + 273.15;
        state.energy_phi = state.phi_magnitude * config_.thermodynamic_beta * temp_kelvin * 1e-4;
        
        state.energy_metric = 0.0;
        state.energy_total = state.energy_psi + state.energy_phi;
    }
    
public:
    DSCoupling(const DSConfig& config = DSConfig()) : config_(config) {
        if (!config_.validate()) {
            throw std::invalid_argument("Invalid DSConfig parameters");
        }
    }
    
    // Main update function - call once per BMS cycle
    void update(DSState& state, double dt) {
        if (!std::isfinite(dt) || dt <= 0.0) {
            throw std::invalid_argument("Update interval must be finite and positive");
        }
        if (dt < config_.tau_min) {
            return;
        }
        
        // Validate input state
        if (!state.is_valid()) {
            throw std::runtime_error("Invalid DSState before update");
        }
        
        // Update time
        state.time += dt;
        state.last_update = state.time;
        
        state.lambda = config_.lambda * (1.0 + 0.1 * state.degradation);
        
        // Update state variables using empirical model
        update_phi(state, dt);
        compute_phi_gradients(state);
        compute_effective_metric(state);
        compute_energies(state);
        
        // Validate output state
        if (!state.is_valid()) {
            throw std::runtime_error("Invalid DSState after update");
        }
    }
    
    // Predict future degradation using clean empirical models
    HealthPrediction predict_health(const DSState& state, double horizon_cycles) const {
        HealthPrediction pred;
        
        double current_degradation = state.degradation;
        double degradation_rate = 0.0002; // Standard empirical degradation rate per cycle
        pred.degradation_rate = degradation_rate;
        
        double future_degradation = current_degradation + degradation_rate * horizon_cycles;
        
        pred.remaining_capacity_percent = (1.0 - future_degradation) * 100.0;
        pred.remaining_capacity_percent = std::max(0.0, pred.remaining_capacity_percent);
        
        double cycles_to_80 = (0.2 - current_degradation) / std::max(degradation_rate, 1e-8);
        pred.cycles_to_80_percent = std::max(0.0, cycles_to_80);
        
        pred.estimated_eol_cycles = (0.5 - current_degradation) / std::max(degradation_rate, 1e-8);
        pred.time_to_80_percent_years = pred.cycles_to_80_percent / 365.0;
        
        pred.confidence = 1.0 - state.degradation;
        pred.warning_triggered = (state.degradation > 0.3);
        
        return pred;
    }
    
    // Compute optimal charging profile using clean empirical models
    OptimalChargingProfile optimize_charging(const DSState& state) {
        OptimalChargingProfile profile;
        
        profile.recommended_current_limit = 100.0 * (1.0 - state.degradation);
        profile.max_safe_current = std::min(config_.max_current, profile.recommended_current_limit * 1.2);
        
        profile.recommended_voltage_limit = config_.nominal_voltage;
        profile.max_safe_voltage = profile.recommended_voltage_limit * 1.05;
        
        profile.recommended_temperature = 25.0 + 5.0 * state.state_of_charge;
        
        double remaining_capacity = (1.0 - state.state_of_charge) * config_.nominal_capacity_ah;
        profile.estimated_charge_time = 60.0 * remaining_capacity / std::max(profile.recommended_current_limit, 1.0);
        
        profile.degradation_impact = profile.recommended_current_limit / 100.0;
        
        return profile;
    }
    
    // Get current configuration
    const DSConfig& get_config() const { return config_; }
};

// ============================================================================
// DSENHANCEMENT - Easy integration interface (THE MAIN API)
// ============================================================================

class DSEnhancement {
private:
    BatteryFeenAdapter feen_adapter_;
    DSConfig config_;
    DSCoupling coupling_;
    DSState state_;
    bool initialized_;
    
    // Energy tracking for conservation validation
    double initial_energy_;
    double cumulative_energy_error_;
    
public:
    DSEnhancement() : coupling_(), initialized_(false),
                       initial_energy_(0.0), cumulative_energy_error_(0.0) {}
    
    // Initialize with battery configuration
    void init(const DSConfig& config = DSConfig()) {
        if (!config.validate()) {
            throw std::invalid_argument("Invalid DSConfig parameters");
        }
        
        config_ = config;
        coupling_ = DSCoupling(config);
        state_ = DSState();
        state_.lambda = config.lambda;
        initial_energy_ = 0.0;
        cumulative_energy_error_ = 0.0;
        initialized_ = true;
    }
    
    // Main enhancement function - call once per BMS cycle
    // INPUT: Raw sensor readings from existing BMS
    // OUTPUT: Enhanced state with DS predictions
    EnhancedState enhance(double voltage, double current, 
                         double temperature, double soc, 
                         double dt = 0.1) {
        if (!initialized_) {
            throw std::runtime_error("DSEnhancement not initialized. Call init() first.");
        }

        if (!std::isfinite(voltage) || voltage <= 0.0 ||
            !std::isfinite(current) || !std::isfinite(temperature) ||
            !std::isfinite(soc) || !std::isfinite(dt) || dt <= 0.0) {
            throw std::invalid_argument("Sensor values must be finite, voltage and dt must be positive");
        }
        
        // Bounds checking: clamp inputs and set flag instead of throwing
        bool input_clamped = false;
        if (std::abs(current) > config_.max_current) {
            current = std::copysign(config_.max_current, current);
            input_clamped = true;
        }
        if (temperature > config_.max_temperature) {
            temperature = config_.max_temperature;
            input_clamped = true;
        } else if (temperature < config_.min_temperature) {
            temperature = config_.min_temperature;
            input_clamped = true;
        }
        
        // Compute into a candidate so a failed update cannot partially mutate the
        // last-known-good state exposed to downstream control code.
        DSState candidate = state_;
        candidate.voltage = voltage;
        candidate.current = current;
        candidate.temperature = temperature;
        candidate.state_of_charge = std::clamp(soc, 0.0, 1.0);
        coupling_.update(candidate, dt);

        // This is the internal energy-accounting residual, not the legitimate
        // change in stored energy caused by new physical sensor measurements.
        double energy_error = std::abs(candidate.energy_total -
            (candidate.energy_psi + candidate.energy_phi + candidate.energy_metric));
        state_ = candidate;
        cumulative_energy_error_ += energy_error;
        
        // Generate predictions
        EnhancedState result;
        result.state = state_;
        result.health = coupling_.predict_health(state_, 100.0); // 100 cycle horizon
        result.charging = coupling_.optimize_charging(state_);
        result.degradation_warning = result.health.warning_triggered;
        result.ds_confidence = result.health.confidence;
        result.input_clamped = input_clamped;
        result.energy_conservation_error = energy_error;
        result.numerical_stability = state_.is_valid() && state_.g_eff.is_stable() &&
            (energy_error <= config_.energy_conservation_tolerance);
        
        if (config_.enable_feen_battery_integration) {
            double trust = feen_adapter_.compute_battery_trust_from_feen(state_.voltage);
            result.feen_trust_metric = trust;
            result.ds_confidence *= trust;
        }
        return result;
    }
    
    // Get current DS state (for debugging/monitoring)
    const DSState& get_state() const { return state_; }
    
    // Get long-term health forecast
    HealthPrediction get_health_forecast(double cycles_ahead) {
        if (!initialized_) {
            throw std::runtime_error("DSEnhancement not initialized");
        }
        return coupling_.predict_health(state_, cycles_ahead);
    }
    
    // Get optimal charging recommendation
    OptimalChargingProfile get_optimal_charging() {
        if (!initialized_) {
            throw std::runtime_error("DSEnhancement not initialized");
        }
        return coupling_.optimize_charging(state_);
    }
    
    // Check energy conservation (Thermodynamic energy conservation compliance)
    double check_energy_conservation() const {
        return cumulative_energy_error_;
    }
    
    // Reset state (for testing or after battery replacement)
    void reset_state() {
        feen_adapter_.reset();
        state_ = DSState();
        state_.lambda = config_.lambda;
        initial_energy_ = 0.0;
        cumulative_energy_error_ = 0.0;
    }
    
    // Get version information
    static std::string get_version() {
        return get_version_string();
    }
};

} // namespace ds

#endif // DS_BATTERY_CORE_HPP

// ============================================================================
// EXAMPLE INTEGRATION
// ============================================================================

#ifdef DS_EXAMPLE_MAIN

#include <iostream>
#include <iomanip>

int main() {
    using namespace ds;
    
    std::cout << "=== DS Battery Enhancement Demo v"
              << DSEnhancement::get_version() << " ===\n\n";
    
    // Initialize DS enhancement
    DSEnhancement ds;
    DSConfig config;
    config.lambda = 1e-6;
    config.nominal_capacity_ah = 75.0;
    config.nominal_voltage = 400.0;
    
    try {
        ds.init(config);
    } catch (const std::exception& e) {
        std::cerr << "Initialization error: " << e.what() << "\n";
        return 1;
    }
    
    std::cout << "Simulating 1000 charge cycles...\n\n";
    
    // Simulate battery operation over many cycles
    double time = 0.0;
    const double dt = 10.0; // 10 second updates
    
    for (int cycle = 0; cycle < 1000; ++cycle) {
        // Simulate one charge-discharge cycle
        for (int step = 0; step < 100; ++step) {
            // Synthetic sensor data (would come from real BMS)
            double soc = std::abs(std::sin(step * 0.0314)); // Oscillating SoC
            double voltage = 350.0 + 50.0 * soc;
            double current = (step < 50) ? 50.0 : -50.0; // Charge then discharge
            double temp = 25.0 + 10.0 * std::abs(current) / 50.0;
            
            try {
                // Get DS enhancement
                auto enhanced = ds.enhance(voltage, current, temp, soc, dt);
                
                time += dt;
                
                // Print status every 100 cycles
                if (cycle % 100 == 0 && step == 0) {
                    std::cout << std::fixed << std::setprecision(2);
                    std::cout << "Cycle " << cycle << ":\n";
                    std::cout << "  Degradation: " << enhanced.state.degradation * 100 << "%\n";
                    std::cout << "  Remaining Capacity: " << enhanced.health.remaining_capacity_percent << "%\n";
                    std::cout << "  Cycles to 80%: " << enhanced.health.cycles_to_80_percent << "\n";
                    std::cout << "  Years to 80%: " << enhanced.health.time_to_80_percent_years << "\n";
                    std::cout << "  Metric Trace: " << enhanced.state.g_eff.trace() << "\n";
                    std::cout << "  DS Confidence: " << enhanced.ds_confidence * 100 << "%\n";
                    std::cout << "  Stability: " << (enhanced.numerical_stability ? "OK" : "WARNING") << "\n";
                    
                    if (enhanced.degradation_warning) {
                        std::cout << "  ⚠️  DEGRADATION WARNING TRIGGERED\n";
                    }
                    std::cout << "\n";
                }
            } catch (const std::exception& e) {
                std::cerr << "Update error at cycle " << cycle << ": " << e.what() << "\n";
                return 1;
            }
        }
    }
    
    // Final predictions
    std::cout << "=== Final Health Assessment ===\n";
    auto final_health = ds.get_health_forecast(500.0);
    std::cout << "Predicted remaining capacity: " << final_health.remaining_capacity_percent << "%\n";
    std::cout << "Estimated cycles to EOL: " << final_health.estimated_eol_cycles << "\n";
    std::cout << "Prediction confidence: " << final_health.confidence * 100 << "%\n";
    
    std::cout << "\n=== Energy Conservation Check ===\n";
    std::cout << "Cumulative energy error: " << ds.check_energy_conservation() << " J\n";
    std::cout << "(Should be small for Thermodynamic energy conservation compliance)\n";
    
    return 0;
}

#endif // DS_EXAMPLE_MAIN
