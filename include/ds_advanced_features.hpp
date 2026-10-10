/*
 * ============================================================================
 * DS BATTERY ENHANCEMENT - ADVANCED FEATURES MODULE
 * ============================================================================
 * 
 * Advanced capabilities extending the core DS Battery Enhancement Library:
 *   - Chemistry-specific parameter tuning (LFP, NMC, NCA, LTO)
 *   - Multi-cell pack support with cell-to-cell variance modeling
 *   - Kalman filter integration for state estimation
 *   - ML hybrid models (DS physics + data-driven learning)
 *   - Fleet-wide learning and anonymized data aggregation
 *   - GPU acceleration for large battery packs
 * 
 * This module maintains the core DS physics while adding production-ready
 * features for real-world EV deployment.
 * 
 * ============================================================================
 */

#ifndef DS_ADVANCED_FEATURES_HPP
#define DS_ADVANCED_FEATURES_HPP

#include "ds_battery_core.hpp"
#include "ds_battery_enhancement.hpp"
#include <vector>
#include <array>
#include <map>
#include <string>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <memory>
#include <functional>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ds {
namespace advanced {

// ============================================================================
// CHEMISTRY-SPECIFIC PARAMETER SETS
// ============================================================================

enum class ChemistryType {
    LFP,    // Lithium Iron Phosphate (LiFePO4)
    NMC,    // Nickel Manganese Cobalt (NCM)
    NCA,    // Nickel Cobalt Aluminum
    LTO,    // Lithium Titanate
    CUSTOM  // User-defined parameters
};

struct ChemistryProfile {
    ChemistryType type;
    std::string name;
    
    // DS-specific parameters tuned for chemistry
    double lambda;                  // Coupling strength
    double phi_decay_rate;          // Information decay
    double entropy_weight;          // Thermal sensitivity
    
    // Degradation characteristics
    double cycle_degradation_base;  // Base degradation per cycle
    double calendar_aging_rate;     // Time-based aging
    double thermal_sensitivity;     // Temperature impact factor
    
    // Operating constraints
    double max_charge_rate;         // C-rate limit
    double max_discharge_rate;      // C-rate limit
    double optimal_temperature;     // Target operating temp (°C)
    double max_safe_temperature;    // Thermal runaway threshold
    
    // Voltage characteristics
    double nominal_voltage_per_cell; // V
    double max_voltage_per_cell;     // V
    double min_voltage_per_cell;     // V
};

class ChemistryLibrary {
private:
    std::map<ChemistryType, ChemistryProfile> profiles_;
    
    void initialize_profiles() {
        // LFP: Long life, thermal stability, lower energy density
        profiles_[ChemistryType::LFP] = {
            ChemistryType::LFP, "Lithium Iron Phosphate",
            5e-7,    // lambda (lower coupling, more stable)
            0.0005,  // phi_decay_rate
            0.3,     // entropy_weight (thermally stable)
            0.00005, // cycle_degradation_base (excellent cycle life)
            0.0001,  // calendar_aging_rate
            0.8,     // thermal_sensitivity (low)
            2.0,     // max_charge_rate (2C)
            3.0,     // max_discharge_rate (3C)
            35.0,    // optimal_temperature
            70.0,    // max_safe_temperature
            3.2,     // nominal_voltage_per_cell
            3.65,    // max_voltage_per_cell
            2.5      // min_voltage_per_cell
        };
        
        // NMC: High energy density, moderate cycle life
        profiles_[ChemistryType::NMC] = {
            ChemistryType::NMC, "Nickel Manganese Cobalt",
            1.5e-6,  // lambda (higher coupling, more sensitive)
            0.0015,  // phi_decay_rate
            0.6,     // entropy_weight (thermal sensitive)
            0.0001,  // cycle_degradation_base
            0.0002,  // calendar_aging_rate
            1.3,     // thermal_sensitivity (higher)
            1.5,     // max_charge_rate (1.5C)
            2.0,     // max_discharge_rate (2C)
            30.0,    // optimal_temperature
            55.0,    // max_safe_temperature
            3.7,     // nominal_voltage_per_cell
            4.2,     // max_voltage_per_cell
            3.0      // min_voltage_per_cell
        };
        
        // NCA: Highest energy density, requires careful management
        profiles_[ChemistryType::NCA] = {
            ChemistryType::NCA, "Nickel Cobalt Aluminum",
            2e-6,    // lambda (highest coupling)
            0.002,   // phi_decay_rate
            0.7,     // entropy_weight (very thermal sensitive)
            0.00012, // cycle_degradation_base
            0.00025, // calendar_aging_rate
            1.5,     // thermal_sensitivity (highest)
            1.0,     // max_charge_rate (1C, careful charging)
            2.0,     // max_discharge_rate (2C)
            25.0,    // optimal_temperature
            50.0,    // max_safe_temperature
            3.6,     // nominal_voltage_per_cell
            4.2,     // max_voltage_per_cell
            3.0      // min_voltage_per_cell
        };
        
        // LTO: Ultra-long life, lower energy density, fast charging
        profiles_[ChemistryType::LTO] = {
            ChemistryType::LTO, "Lithium Titanate",
            3e-7,    // lambda (very low coupling, ultra-stable)
            0.0003,  // phi_decay_rate
            0.2,     // entropy_weight (minimal thermal sensitivity)
            0.00002, // cycle_degradation_base (20k+ cycles)
            0.00005, // calendar_aging_rate
            0.5,     // thermal_sensitivity (very low)
            5.0,     // max_charge_rate (5C, fast charging capable)
            10.0,    // max_discharge_rate (10C)
            40.0,    // optimal_temperature
            75.0,    // max_safe_temperature
            2.4,     // nominal_voltage_per_cell
            2.8,     // max_voltage_per_cell
            1.5      // min_voltage_per_cell
        };
    }
    
public:
    ChemistryLibrary() {
        initialize_profiles();
    }
    
    const ChemistryProfile& get_profile(ChemistryType type) const {
        return profiles_.at(type);
    }
    
    DSConfig create_config(ChemistryType type,
                           double capacity_ah, 
                           int series_cells) const {
        const auto& profile = get_profile(type);
        
        DSConfig config;
        config.lambda = profile.lambda;
        config.phi_decay_rate = profile.phi_decay_rate;
        config.entropy_weight = profile.entropy_weight;
        config.nominal_capacity_ah = capacity_ah;
        config.nominal_voltage = profile.nominal_voltage_per_cell * series_cells;
        config.max_temperature = profile.max_safe_temperature;
        
        return config;
    }
};

// ============================================================================
// MULTI-CELL PACK SUPPORT
// ============================================================================

struct CellState {
    int cell_id;
    DSState state;
    double relative_capacity;  // Normalized to pack average
    double internal_resistance; // Ohms
    bool is_weak_cell;         // Flagged as problematic
};

// ============================================================================
// GPU ACCELERATION INTERFACE (for large packs)
// ============================================================================

#ifdef DS_USE_GPU
// Placeholder for GPU acceleration
// In production: use CUDA/OpenCL for parallel cell updates
namespace gpu {

class GPUAccelerator {
public:
    void update_cells_parallel(std::vector<CellState>& cells,
                               DSCoupling& coupling,
                               double dt) {
        // GPU kernel placeholder with CPU fallback
        // Pseudocode:
        // - Copy cell states to GPU memory
        // - Launch kernel: one thread per cell
        // - Each thread runs DSCoupling::update()
        // - Copy results back to CPU
        for (auto& cell : cells) {
            coupling.update(cell.state, dt);
        }
    }
};

} // namespace gpu
#endif

class MultiCellPack {
private:
    std::vector<CellState> cells_;
    ChemistryProfile chemistry_;
    DSCoupling coupling_;
    
    // Pack-level statistics
    double pack_voltage_;
    double pack_current_;
    double average_temperature_;
    double min_cell_voltage_;
    double max_cell_voltage_;
    double voltage_imbalance_;
    double min_cell_temperature_;
    double max_cell_temperature_;
    double temperature_spread_;
    
    void update_pack_statistics() {
        pack_voltage_ = 0.0;
        average_temperature_ = 0.0;
        min_cell_voltage_ = 1e6;
        max_cell_voltage_ = -1e6;
        min_cell_temperature_ = 1e6;
        max_cell_temperature_ = -1e6;
        
        for (const auto& cell : cells_) {
            pack_voltage_ += cell.state.voltage;
            average_temperature_ += cell.state.temperature;
            min_cell_voltage_ = std::min(min_cell_voltage_, cell.state.voltage);
            max_cell_voltage_ = std::max(max_cell_voltage_, cell.state.voltage);
            min_cell_temperature_ = std::min(min_cell_temperature_, cell.state.temperature);
            max_cell_temperature_ = std::max(max_cell_temperature_, cell.state.temperature);
        }
        
        average_temperature_ /= cells_.size();
        voltage_imbalance_ = max_cell_voltage_ - min_cell_voltage_;
        temperature_spread_ = max_cell_temperature_ - min_cell_temperature_;
    }
    
    void detect_weak_cells() {
        // Calculate average degradation
        double avg_degradation = 0.0;
        for (const auto& cell : cells_) {
            avg_degradation += cell.state.degradation;
        }
        avg_degradation /= cells_.size();
        
        // Flag cells significantly above average
        double threshold = avg_degradation * 1.3; // 30% worse than average
        for (auto& cell : cells_) {
            cell.is_weak_cell = (cell.state.degradation > threshold);
        }
    }
    
public:
    MultiCellPack(int num_cells, const ChemistryProfile& chemistry)
        : chemistry_(chemistry),
          coupling_(DSConfig()),
          pack_voltage_(0.0),
          pack_current_(0.0),
          average_temperature_(0.0),
          min_cell_voltage_(0.0),
          max_cell_voltage_(0.0),
          voltage_imbalance_(0.0),
          min_cell_temperature_(0.0),
          max_cell_temperature_(0.0),
          temperature_spread_(0.0) {
        cells_.resize(num_cells);
        for (int i = 0; i < num_cells; ++i) {
            cells_[i].cell_id = i;
            cells_[i].relative_capacity = 1.0;
            cells_[i].internal_resistance = 0.01; // 10 mΩ typical
            cells_[i].is_weak_cell = false;
        }
    }
    
    void update_all_cells(const std::vector<double>& cell_voltages,
                         const std::vector<double>& cell_temperatures,
                         double pack_current,
                         double dt) {
        if (cell_voltages.size() != cells_.size() ||
            cell_temperatures.size() != cells_.size()) {
            throw std::runtime_error("Cell data size mismatch");
        }
        if (cells_.empty() || !std::isfinite(pack_current) ||
            !std::isfinite(dt) || dt <= 0.0) {
            throw std::invalid_argument("Pack cycle must be nonempty with finite current and positive dt");
        }
        if (dt < coupling_.get_config().tau_min) {
            throw std::invalid_argument("Pack interval is below tau_min");
        }
        const double voltage_span = chemistry_.max_voltage_per_cell - chemistry_.min_voltage_per_cell;
        if (!std::isfinite(chemistry_.min_voltage_per_cell) ||
            !std::isfinite(voltage_span) || voltage_span <= 0.0) {
            throw std::invalid_argument("Invalid cell voltage range");
        }
        // Validate the complete sensor set before doing any cell work.
        for (size_t i = 0; i < cells_.size(); ++i) {
            if (!std::isfinite(cell_voltages[i]) || cell_voltages[i] < 0.0 ||
                !std::isfinite(cell_temperatures[i])) {
                throw std::invalid_argument("Cell measurements must be finite with nonnegative voltage");
            }
        }

        // Copy cells and statistics together. No externally visible field changes
        // until every cell and all aggregate arithmetic have succeeded.
        MultiCellPack candidate = *this;
        candidate.pack_current_ = pack_current;
        for (size_t i = 0; i < cells_.size(); ++i) {
            candidate.cells_[i].state.voltage = cell_voltages[i];
            candidate.cells_[i].state.temperature = cell_temperatures[i];
            candidate.cells_[i].state.current = pack_current; // Series connection
            
            // Estimate SoC from voltage (simplified - use proper OCV curve)
            double v_norm = (cell_voltages[i] - chemistry_.min_voltage_per_cell) /
                           voltage_span;
            if (!std::isfinite(v_norm)) {
                throw std::runtime_error("Invalid cell SOC calculation");
            }
            candidate.cells_[i].state.state_of_charge = std::clamp(v_norm, 0.0, 1.0);
        }

#ifdef DS_USE_GPU
        gpu::GPUAccelerator accelerator;
        accelerator.update_cells_parallel(candidate.cells_, candidate.coupling_, dt);
#else
        for (auto& cell : candidate.cells_) {
            candidate.coupling_.update(cell.state, dt);
        }
#endif
        
        candidate.update_pack_statistics();
        candidate.detect_weak_cells();
        const double statistics[] = {candidate.pack_voltage_, candidate.pack_current_,
            candidate.average_temperature_, candidate.min_cell_voltage_, candidate.max_cell_voltage_,
            candidate.voltage_imbalance_, candidate.min_cell_temperature_, candidate.max_cell_temperature_,
            candidate.temperature_spread_};
        for (double value : statistics) {
            if (!std::isfinite(value)) {
                throw std::runtime_error("Invalid pack statistics after update");
            }
        }
        // Vector swap and scalar assignments cannot throw; commit the whole cycle.
        cells_.swap(candidate.cells_);
        pack_voltage_ = candidate.pack_voltage_;
        pack_current_ = candidate.pack_current_;
        average_temperature_ = candidate.average_temperature_;
        min_cell_voltage_ = candidate.min_cell_voltage_;
        max_cell_voltage_ = candidate.max_cell_voltage_;
        voltage_imbalance_ = candidate.voltage_imbalance_;
        min_cell_temperature_ = candidate.min_cell_temperature_;
        max_cell_temperature_ = candidate.max_cell_temperature_;
        temperature_spread_ = candidate.temperature_spread_;
    }
    
    const std::vector<CellState>& get_cells() const { return cells_; }
    
    double get_pack_voltage() const { return pack_voltage_; }
    double get_pack_current() const { return pack_current_; }
    double get_min_cell_voltage() const { return min_cell_voltage_; }
    double get_max_cell_voltage() const { return max_cell_voltage_; }
    double get_voltage_imbalance() const { return voltage_imbalance_; }
    double get_average_temperature() const { return average_temperature_; }
    double get_temperature_spread() const { return temperature_spread_; }
    double get_min_cell_temperature() const { return min_cell_temperature_; }
    double get_max_cell_temperature() const { return max_cell_temperature_; }
    
    std::vector<int> get_weak_cell_ids() const {
        std::vector<int> weak_ids;
        for (const auto& cell : cells_) {
            if (cell.is_weak_cell) {
                weak_ids.push_back(cell.cell_id);
            }
        }
        return weak_ids;
    }
    
    HealthPrediction get_pack_health(double cycles_ahead) const {
        // Use worst cell to predict pack EOL
        const CellState* worst_cell = &cells_[0];
        for (const auto& cell : cells_) {
            if (cell.state.degradation > worst_cell->state.degradation) {
                worst_cell = &cell;
            }
        }
        
        return coupling_.predict_health(worst_cell->state, cycles_ahead);
    }
};

// ============================================================================
// KALMAN FILTER INTEGRATION
// ============================================================================

class KalmanDSFilter {
private:
    // State vector: [SoC, degradation, phi_magnitude, entropy]
    std::array<double, 4> x_;     // State estimate
    std::array<double, 4> P_;     // State covariance (diagonal)
    
    // Process and measurement noise
    double process_noise_;
    double measurement_noise_;
    
    DSState last_ds_state_;
    
public:
    KalmanDSFilter(double process_noise = 1e-5,
                   double measurement_noise = 1e-3)
        : x_{1.0, 0.0, 0.0, 0.0},
          P_{0.01, 0.01, 0.01, 0.01},
          process_noise_(process_noise),
          measurement_noise_(measurement_noise),
          last_ds_state_{} {
    }
    
    // Predict step using DS dynamics
    void predict(const DSState& ds_state, double dt) {
        // Use DS coupling as the state transition model
        // x_k|k-1 = f(x_k-1, u_k)
        
        // Simple Euler integration of DS dynamics
        x_[0] = ds_state.state_of_charge; // SoC from DS
        x_[1] = ds_state.degradation;     // Degradation from DS
        x_[2] = ds_state.phi_magnitude;   // Phi from DS
        x_[3] = ds_state.entropy;         // Entropy from DS
        
        // Increase uncertainty (process noise)
        for (int i = 0; i < 4; ++i) {
            P_[i] += process_noise_ * dt;
        }
        
        last_ds_state_ = ds_state;
    }
    
    // Update step with measurements
    void update(double measured_soc, double measured_capacity_fade) {
        // Measurement vector: [SoC, degradation_proxy]
        // H = [1, 0, 0, 0]  for SoC
        //     [0, 1, 0, 0]  for degradation
        
        // SoC update
        double innovation_soc = measured_soc - x_[0];
        double S_soc = P_[0] + measurement_noise_;
        double K_soc = P_[0] / S_soc;
        x_[0] += K_soc * innovation_soc;
        P_[0] *= (1.0 - K_soc);
        
        // Degradation update
        double innovation_deg = measured_capacity_fade - x_[1];
        double S_deg = P_[1] + measurement_noise_;
        double K_deg = P_[1] / S_deg;
        x_[1] += K_deg * innovation_deg;
        P_[1] *= (1.0 - K_deg);
        
        // Clamp to physical bounds
        x_[0] = std::clamp(x_[0], 0.0, 1.0);
        x_[1] = std::clamp(x_[1], 0.0, 1.0);
    }
    
    std::array<double, 4> get_state() const { return x_; }
    std::array<double, 4> get_covariance() const { return P_; }
    
    double get_filtered_soc() const { return x_[0]; }
    double get_filtered_degradation() const { return x_[1]; }
    double get_uncertainty() const { 
        return std::sqrt(P_[0] + P_[1] + P_[2] + P_[3]);
    }
};

// ============================================================================
// ML HYBRID MODEL (DS + Neural Network)
// ============================================================================

struct MLFeatures {
    // Input features for ML model
    double soc;
    double voltage;
    double current;
    double temperature;
    double cycle_count;
    double ds_metric_trace;
    double ds_phi_magnitude;
    double ds_entropy;
    double ds_predicted_degradation;
    
    std::vector<double> to_vector() const {
        return {soc, voltage, current, temperature, cycle_count,
                ds_metric_trace, ds_phi_magnitude, ds_entropy,
                ds_predicted_degradation};
    }
};

class MLHybridModel {
private:
    // Simple 2-layer neural network for demonstration
    // In production, use TensorFlow/PyTorch models
    std::vector<std::vector<double>> layer1_weights_;
    std::vector<double> layer1_bias_;
    std::vector<std::vector<double>> layer2_weights_;
    std::vector<double> layer2_bias_;
    
    int input_size_;
    int hidden_size_;
    int output_size_;
    
    double relu(double x) const { return std::max(0.0, x); }
    
    std::vector<double> forward(const std::vector<double>& input) const {
        // Layer 1
        std::vector<double> hidden(hidden_size_, 0.0);
        for (int i = 0; i < hidden_size_; ++i) {
            for (int j = 0; j < input_size_; ++j) {
                hidden[i] += layer1_weights_[i][j] * input[j];
            }
            hidden[i] = relu(hidden[i] + layer1_bias_[i]);
        }
        
        // Layer 2
        std::vector<double> output(output_size_, 0.0);
        for (int i = 0; i < output_size_; ++i) {
            for (int j = 0; j < hidden_size_; ++j) {
                output[i] += layer2_weights_[i][j] * hidden[j];
            }
            output[i] += layer2_bias_[i];
        }
        
        return output;
    }
    
public:
    MLHybridModel(int input_size = 9, int hidden_size = 16, int output_size = 3)
        : input_size_(input_size), 
          hidden_size_(hidden_size),
          output_size_(output_size) {
        
        // Initialize weights (random small values in practice)
        layer1_weights_.resize(hidden_size_, std::vector<double>(input_size_, 0.1));
        layer1_bias_.resize(hidden_size_, 0.0);
        layer2_weights_.resize(output_size_, std::vector<double>(hidden_size_, 0.1));
        layer2_bias_.resize(output_size_, 0.0);
    }
    
    // Predict: [degradation_correction, cycles_to_eol_correction, confidence]
    std::vector<double> predict(const MLFeatures& features) const {
        return forward(features.to_vector());
    }
    
    // In production: load pre-trained weights from file
    void load_weights(const std::string& path) {
        std::ifstream input(path);
        if (!input.is_open()) {
            throw std::runtime_error("Unable to open ML weights file: " + path);
        }

        std::vector<double> values;
        std::string line;
        while (std::getline(input, line)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            std::istringstream stream(line);
            double value = 0.0;
            while (stream >> value) {
                values.push_back(value);
            }
        }

        const size_t expected =
            static_cast<size_t>(hidden_size_ * input_size_) +
            static_cast<size_t>(hidden_size_) +
            static_cast<size_t>(output_size_ * hidden_size_) +
            static_cast<size_t>(output_size_);

        if (values.size() != expected) {
            throw std::runtime_error("Unexpected ML weights size");
        }

        size_t index = 0;
        for (int i = 0; i < hidden_size_; ++i) {
            for (int j = 0; j < input_size_; ++j) {
                layer1_weights_[i][j] = values[index++];
            }
        }
        for (int i = 0; i < hidden_size_; ++i) {
            layer1_bias_[i] = values[index++];
        }
        for (int i = 0; i < output_size_; ++i) {
            for (int j = 0; j < hidden_size_; ++j) {
                layer2_weights_[i][j] = values[index++];
            }
        }
        for (int i = 0; i < output_size_; ++i) {
            layer2_bias_[i] = values[index++];
        }
    }
};

// ============================================================================
// FLEET-WIDE LEARNING (Anonymized Data Aggregation)
// ============================================================================

struct AnonymizedBatteryData {
    ChemistryType chemistry;
    double nominal_capacity;
    int cycle_count;
    double average_temperature;
    double degradation_rate;
    double ds_metric_trace_avg;
    // NO personally identifiable information
};

class FleetLearningAggregator {
private:
    std::vector<AnonymizedBatteryData> fleet_data_;
    std::map<ChemistryType, std::vector<AnonymizedBatteryData>> chemistry_groups_;
    
public:
    void add_battery_data(const AnonymizedBatteryData& data) {
        fleet_data_.push_back(data);
        chemistry_groups_[data.chemistry].push_back(data);
    }
    
    // Learn degradation patterns from fleet
    double get_expected_degradation_rate(ChemistryType chemistry,
                                        double cycle_count,
                                        double avg_temperature) const {
        const auto& group = chemistry_groups_.at(chemistry);
        if (group.empty()) return 0.0001; // Default
        
        // Find similar batteries in fleet
        std::vector<double> similar_rates;
        for (const auto& battery : group) {
            if (std::abs(battery.average_temperature - avg_temperature) < 10.0 &&
                std::abs(battery.cycle_count - cycle_count) < 500) {
                similar_rates.push_back(battery.degradation_rate);
            }
        }
        
        if (similar_rates.empty()) return 0.0001;
        
        // Return median degradation rate
        std::sort(similar_rates.begin(), similar_rates.end());
        return similar_rates[similar_rates.size() / 2];
    }
    
    size_t get_fleet_size() const { return fleet_data_.size(); }
    
    // Export anonymized data for ML training
    std::vector<AnonymizedBatteryData> export_training_data() const {
        return fleet_data_;
    }
};

// ============================================================================
// ADVANCED FEATURES INTEGRATION CLASS
// ============================================================================

class AdvancedDSSystem {
private:
    ChemistryLibrary chemistry_lib_;
    std::unique_ptr<MultiCellPack> pack_;
    std::unique_ptr<KalmanDSFilter> kalman_;
    std::unique_ptr<MLHybridModel> ml_model_;
    FleetLearningAggregator fleet_aggregator_;
    
    ChemistryType chemistry_type_;
    bool use_kalman_;
    bool use_ml_;
    bool contribute_to_fleet_;
    double nominal_capacity_ah_;

    double last_metric_trace_;
    double last_phi_magnitude_;
    double last_entropy_;
    double last_ml_confidence_;
    std::vector<double> last_ml_prediction_;
    double last_filtered_soc_;
    double last_filtered_degradation_;
    
public:
    AdvancedDSSystem()
        : chemistry_type_(ChemistryType::NMC),
          use_kalman_(false),
          use_ml_(false),
          contribute_to_fleet_(false),
          nominal_capacity_ah_(0.0),
          last_metric_trace_(0.0),
          last_phi_magnitude_(0.0),
          last_entropy_(0.0),
          last_ml_confidence_(0.0),
          last_filtered_soc_(0.0),
          last_filtered_degradation_(0.0) {}
    
    void init(ChemistryType chemistry, 
             double capacity_ah,
             int series_cells,
             bool enable_kalman = true,
             bool enable_ml = false,
             bool enable_fleet = false) {
        
        chemistry_type_ = chemistry;
        use_kalman_ = enable_kalman;
        use_ml_ = enable_ml;
        contribute_to_fleet_ = enable_fleet;
        nominal_capacity_ah_ = capacity_ah;
        
        // Initialize multi-cell pack
        auto profile = chemistry_lib_.get_profile(chemistry);
        pack_ = std::make_unique<MultiCellPack>(series_cells, profile);
        
        // Initialize Kalman filter
        if (use_kalman_) {
            kalman_ = std::make_unique<KalmanDSFilter>();
        }
        
        // Initialize ML model
        if (use_ml_) {
            ml_model_ = std::make_unique<MLHybridModel>();
        }
    }
    
    void update(const std::vector<double>& cell_voltages,
               const std::vector<double>& cell_temperatures,
               double pack_current,
               double dt) {
        
        // Update all cells with DS dynamics
        pack_->update_all_cells(cell_voltages, cell_temperatures,
                                pack_current, dt);

        const auto& cells = pack_->get_cells();
        DSState average_state;
        average_state.temperature = 0.0;
        average_state.voltage = 0.0;
        average_state.current = 0.0;
        average_state.state_of_charge = 0.0;
        average_state.entropy = 0.0;
        average_state.cycle_count = 0.0;
        average_state.degradation = 0.0;
        average_state.phi_magnitude = 0.0;
        average_state.capacity_fade = 0.0;

        last_metric_trace_ = 0.0;
        for (const auto& cell : cells) {
            average_state.temperature += cell.state.temperature;
            average_state.voltage += cell.state.voltage;
            average_state.current += cell.state.current;
            average_state.state_of_charge += cell.state.state_of_charge;
            average_state.entropy += cell.state.entropy;
            average_state.cycle_count += cell.state.cycle_count;
            average_state.degradation += cell.state.degradation;
            average_state.phi_magnitude += cell.state.phi_magnitude;
            average_state.capacity_fade += cell.state.capacity_fade;
            last_metric_trace_ += cell.state.g_eff.trace();
        }

        const double cell_count = static_cast<double>(cells.size());
        if (cell_count > 0.0) {
            average_state.temperature /= cell_count;
            average_state.voltage /= cell_count;
            average_state.current /= cell_count;
            average_state.state_of_charge /= cell_count;
            average_state.entropy /= cell_count;
            average_state.cycle_count /= cell_count;
            average_state.degradation /= cell_count;
            average_state.phi_magnitude /= cell_count;
            average_state.capacity_fade /= cell_count;
            last_metric_trace_ /= cell_count;
        }
        last_phi_magnitude_ = average_state.phi_magnitude;
        last_entropy_ = average_state.entropy;

        if (use_kalman_ && kalman_) {
            kalman_->predict(average_state, dt);
            kalman_->update(average_state.state_of_charge,
                            average_state.capacity_fade);
            last_filtered_soc_ = kalman_->get_filtered_soc();
            last_filtered_degradation_ = kalman_->get_filtered_degradation();
        }

        if (use_ml_ && ml_model_) {
            MLFeatures features{
                average_state.state_of_charge,
                average_state.voltage,
                average_state.current,
                average_state.temperature,
                average_state.cycle_count,
                last_metric_trace_,
                average_state.phi_magnitude,
                average_state.entropy,
                average_state.degradation
            };
            last_ml_prediction_ = ml_model_->predict(features);
            last_ml_confidence_ =
                (last_ml_prediction_.size() >= 3) ? last_ml_prediction_[2] : 0.0;
        }

        if (contribute_to_fleet_) {
            auto health = pack_->get_pack_health(100.0);
            AnonymizedBatteryData data{
                chemistry_type_,
                nominal_capacity_ah_,
                static_cast<int>(std::round(average_state.cycle_count)),
                average_state.temperature,
                health.degradation_rate,
                last_metric_trace_
            };
            fleet_aggregator_.add_battery_data(data);
        }
    }
    
    HealthPrediction get_pack_health(double cycles_ahead = 100.0) const {
        auto health = pack_->get_pack_health(cycles_ahead);
        if (use_ml_ && last_ml_prediction_.size() >= 2) {
            const double degradation_correction = last_ml_prediction_[0];
            const double cycles_correction = last_ml_prediction_[1];
            health.degradation_rate = std::max(0.0,
                health.degradation_rate + degradation_correction);
            health.remaining_capacity_percent = std::clamp(
                health.remaining_capacity_percent - degradation_correction * 100.0,
                0.0, 100.0);
            health.cycles_to_80_percent = std::max(0.0,
                health.cycles_to_80_percent + cycles_correction);
        }
        return health;
    }
    
    std::vector<int> get_weak_cells() const {
        return pack_->get_weak_cell_ids();
    }

    double get_voltage_imbalance() const {
        return pack_->get_voltage_imbalance();
    }

    double get_temperature_spread() const {
        return pack_->get_temperature_spread();
    }

    double get_average_temperature() const {
        return pack_->get_average_temperature();
    }

    double get_max_cell_temperature() const {
        return pack_->get_max_cell_temperature();
    }

    double get_metric_trace() const { return last_metric_trace_; }
    double get_phi_magnitude() const { return last_phi_magnitude_; }
    double get_entropy_level() const { return last_entropy_; }
    double get_ml_confidence() const { return last_ml_confidence_; }
    double get_filtered_soc() const { return last_filtered_soc_; }
    double get_filtered_degradation() const { return last_filtered_degradation_; }
};

} // namespace advanced
} // namespace ds

#endif // DS_ADVANCED_FEATURES_HPP
