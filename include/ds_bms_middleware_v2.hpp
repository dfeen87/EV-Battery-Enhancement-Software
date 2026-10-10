#ifndef DS_BMS_MIDDLEWARE_V2_HPP
#define DS_BMS_MIDDLEWARE_V2_HPP

#include "ds_battery_core.hpp"
#include "ds_battery_enhancement.hpp"
#include "ds_advanced_features.hpp"
#include "ds_energy_telemetry.hpp"

#include <vector>
#include <array>
#include <string>
#include <memory>
#include <chrono>
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <map>

namespace ds_plugin {

/* ================= VERSION ================= */

constexpr int MIDDLEWARE_VERSION_MAJOR = 2;
constexpr int MIDDLEWARE_VERSION_MINOR = 0;
constexpr int MIDDLEWARE_VERSION_PATCH = 1;

inline std::string get_middleware_version() {
    return std::to_string(MIDDLEWARE_VERSION_MAJOR) + "." +
           std::to_string(MIDDLEWARE_VERSION_MINOR) + "." +
           std::to_string(MIDDLEWARE_VERSION_PATCH);
}

/* ================= DIAGNOSTICS ================= */

struct DiagnosticReport {
    double pack_soh_percent = 100.0;
    double pack_soc_percent = 100.0;
    double estimated_range_km = 0.0;
    double cycles_to_eol = 0.0;
    double pack_health_percent = 100.0;
    double estimated_remaining_cycles = 0.0;

    int total_cells = 1;
    int weak_cell_count = 0;
    std::vector<int> weak_cell_ids;

    double min_cell_voltage = 0.0;
    double max_cell_voltage = 0.0;
    double voltage_imbalance_mv = 0.0;

    double average_cell_temp_c = 25.0;
    double max_cell_temp_c = 25.0;
    double min_cell_temp_c = 25.0;

    double ds_metric_trace = 2.0;
    double ds_entropy = 0.0;
    double ds_phi_magnitude = 0.0;
    double ds_confidence = 1.0;

    bool weak_cell_warning = false;
    bool thermal_warning = false;
    bool voltage_warning = false;
    bool degradation_warning = false;
    bool balancing_required = false;
    bool safety_fault = false;
    bool soc_warning = false;
    bool low_temp_warning = false;

    double instantaneous_power_kw = 0.0;
    double average_efficiency = 0.95;
    double energy_throughput_kwh = 0.0;
    double last_update_time_ms = 0.0;
    double average_update_time_ms = 0.0;
    double degradation_rate_per_cycle = 0.0;
    double metric_trace = 2.0;
    double phi_magnitude = 0.0;
    double entropy_level = 0.0;

    double time_since_init_s = 0.0;
    int update_count = 0;
};

/* ================= SAFETY LIMITS ================= */

struct SafetyLimits {
    double min_cell_voltage = 2.5;
    double max_cell_voltage = 4.3;
    double min_pack_voltage = 300.0;
    double max_pack_voltage = 450.0;

    double max_discharge_current = 600.0;
    double max_charge_current = 250.0;

    double min_operating_temp = -20.0;
    double max_operating_temp = 60.0;
    double max_cell_temp = 65.0;

    double min_soc = 0.05;
    double max_soc = 0.95;

    double max_voltage_imbalance_mv = 100.0;
    double min_health_percent = 70.0;
};

/* ================= CONFIG ================= */

struct MiddlewareConfig {
    enum class Mode { SIMPLE, SINGLE_BATTERY = SIMPLE, MULTI_CELL_PACK, ADVANCED_ML };

    Mode mode = Mode::SIMPLE;
    ds::advanced::ChemistryType chemistry = ds::advanced::ChemistryType::NMC;

    double nominal_capacity_ah = 75.0;
    double nominal_voltage = 400.0;

    int series_cells = 96;

    ds::DSConfig ds_config;

    bool enable_kalman_filter = false;
    bool enable_ml_hybrid = false;
    bool enable_cell_balancing = false;

    SafetyLimits safety_limits;
    bool enable_safety_monitoring = true;

    bool enable_logging = true;
    double logging_interval_s = 1.0;
};

/* ================= SAFETY MONITOR ================= */

class SafetyMonitor {
    SafetyLimits limits_;
    bool fault_active_ = false;
    std::vector<std::string> faults_;
    std::vector<std::string> advisories_;
    bool soc_warning_ = false;
    bool low_temp_warning_ = false;

public:
    explicit SafetyMonitor(const SafetyLimits& limits = {}) : limits_(limits) {}

    bool check(const ds::DSState& s, const DiagnosticReport& d) {
        faults_.clear();
        advisories_.clear();
        fault_active_ = false;
        soc_warning_ = false;
        low_temp_warning_ = false;

        if (s.current > limits_.max_discharge_current)
            faults_.push_back("Discharge current exceeded");

        if (s.current < -limits_.max_charge_current)
            faults_.push_back("Charge current exceeded");

        if (s.voltage < limits_.min_pack_voltage || s.voltage > limits_.max_pack_voltage)
            faults_.push_back("Pack voltage out of bounds");

        if (s.temperature > limits_.max_operating_temp)
            faults_.push_back("Temperature too high");

        if (d.max_cell_temp_c > limits_.max_cell_temp)
            faults_.push_back("Cell temperature critical");

        if (s.state_of_charge < limits_.min_soc) {
            soc_warning_ = true;
            if (s.current > 0.0) {
                faults_.push_back("SOC below minimum operating limit while discharging");
            } else if (s.current == 0.0) {
                advisories_.push_back("SOC below minimum operating limit while resting");
            }
        }

        if (s.state_of_charge > limits_.max_soc) {
            soc_warning_ = true;
            if (s.current < 0.0) {
                faults_.push_back("SOC above maximum operating limit while charging");
            } else if (s.current == 0.0) {
                advisories_.push_back("SOC above maximum operating limit while resting");
            }
        }

        if (s.temperature < limits_.min_operating_temp) {
            low_temp_warning_ = true;
            if (s.current != 0.0) {
                faults_.push_back("Temperature below minimum operating limit while active");
            } else {
                advisories_.push_back("Temperature below minimum operating limit while resting");
            }
        }

        if (!faults_.empty()) fault_active_ = true;
        return !fault_active_;
    }

    bool has_fault() const { return fault_active_; }
    const std::vector<std::string>& faults() const { return faults_; }

    const std::vector<std::string>& advisories() const { return advisories_; }
    bool has_soc_warning() const { return soc_warning_; }
    bool has_low_temp_warning() const { return low_temp_warning_; }
};

/* ================= MIDDLEWARE ================= */

class DSBMSMiddleware {
    MiddlewareConfig config_;

    std::unique_ptr<ds::DSEnhancement> ds_core_;
    std::unique_ptr<ds::advanced::MultiCellPack> pack_;
    std::unique_ptr<ds::advanced::KalmanDSFilter> kalman_;

    SafetyMonitor safety_;
    DiagnosticReport diag_ = {};
    ds::EnhancedState enhanced_ = {};

    bool initialized_ = false;
    double time_s_ = 0.0;
    int updates_ = 0;
    double total_update_time_ms_ = 0.0;

    double energy_in_kwh_ = 0.0;
    double energy_out_kwh_ = 0.0;

public:
    DSBMSMiddleware() = default;

    void init(double capacity_ah, double voltage_v) {
        config_.nominal_capacity_ah = capacity_ah;
        config_.nominal_voltage = voltage_v;
        config_.ds_config.nominal_capacity_ah = capacity_ah;
        config_.ds_config.nominal_voltage = voltage_v;

        ds_core_ = std::make_unique<ds::DSEnhancement>();
        ds_core_->init(config_.ds_config);

        safety_ = SafetyMonitor(config_.safety_limits);
        initialized_ = true;
    }

    void init_advanced(const MiddlewareConfig& cfg) {
        config_ = cfg;

        ds_core_ = std::make_unique<ds::DSEnhancement>();
        ds_core_->init(config_.ds_config);

        if (cfg.mode == MiddlewareConfig::Mode::MULTI_CELL_PACK ||
            cfg.mode == MiddlewareConfig::Mode::ADVANCED_ML) {
            pack_ = std::make_unique<ds::advanced::MultiCellPack>(
                cfg.series_cells,
                ds::advanced::ChemistryLibrary().get_profile(cfg.chemistry));
        }

        if (cfg.enable_kalman_filter) {
            kalman_ = std::make_unique<ds::advanced::KalmanDSFilter>();
        }

        safety_ = SafetyMonitor(cfg.safety_limits);
        initialized_ = true;
    }

    ds::EnhancedState enhance_cycle(double v, double i, double t, double soc, double dt) {
        if (!initialized_) throw std::runtime_error("Middleware not initialized");

        // Let the core validate the original readings before normalization.
        // In particular, clamping SOC here would turn infinity into valid data.
        enhanced_ = ds_core_->enhance(v, i, t, soc, dt);

        // DS bounds its model inputs. Safety policy must evaluate the original
        // evidence so those bounds cannot conceal a thermal or current fault.
        ds::DSState observed = enhanced_.state;
        observed.voltage = v;
        observed.current = i;
        observed.temperature = t;
        observed.state_of_charge = soc;

        if (kalman_) {
            kalman_->predict(enhanced_.state, dt);
            kalman_->update(std::clamp(soc, 0.0, 1.0), i);
            auto f = kalman_->get_state();
            enhanced_.state.state_of_charge = std::clamp(f[0], 0.0, 1.0);
        }

        double power_kw = (v * i) / 1000.0;
        double e = power_kw * dt / 3600.0;

        if (i > 0) energy_out_kwh_ += e;
        else energy_in_kwh_ += std::abs(e);

        update_diagnostics(dt, observed);
        return enhanced_;
    }

    const DiagnosticReport& diagnostics() const { return diag_; }
    const DiagnosticReport* diagnostics_ptr() const { return &diag_; }
    DiagnosticReport get_diagnostics() const { return diag_; }

    ds::HealthPrediction get_health_forecast(double cycles_ahead = 100.0) {
        if (!initialized_) throw std::runtime_error("Middleware not initialized");

        if (config_.mode == MiddlewareConfig::Mode::SIMPLE ||
            config_.mode == MiddlewareConfig::Mode::SINGLE_BATTERY) {
            return ds_core_->get_health_forecast(cycles_ahead);
        }

        if (!pack_) {
            throw std::runtime_error("Multi-cell pack not initialized");
        }

        return pack_->get_pack_health(cycles_ahead);
    }

    std::string get_status_summary() const {
        std::string status = "DS BMS Status:\n";
        status += "  Mode: " + std::string(
            config_.mode == MiddlewareConfig::Mode::SIMPLE ||
            config_.mode == MiddlewareConfig::Mode::SINGLE_BATTERY
                ? "Single Battery"
                : "Multi-Cell Pack"
        ) + "\n";
        status += "  Health: " + std::to_string(diag_.pack_health_percent) + "%\n";
        status += "  Remaining Cycles: " + std::to_string(diag_.estimated_remaining_cycles) + "\n";
        status += "  Update Count: " + std::to_string(diag_.update_count) + "\n";

        if (diag_.degradation_warning || diag_.thermal_warning ||
            diag_.weak_cell_warning || diag_.voltage_warning || diag_.safety_fault) {
            status += "  ⚠️  WARNINGS ACTIVE\n";
        }

        return status;
    }

// --------------------------------------------------------------------
    // Optional Telemetry Snapshot (Non-Control, Read-Only)
    // --------------------------------------------------------------------
    ds::DSEnergyTelemetry snapshot() const {
        ds::DSEnergyTelemetry t;

        t.soc_percent = diag_.pack_soc_percent;
        t.soh_percent = diag_.pack_soh_percent;

        t.pack_power_kw = diag_.instantaneous_power_kw;
        t.regen_power_kw = (diag_.instantaneous_power_kw < 0.0)
                             ? -diag_.instantaneous_power_kw
                             : 0.0;

        t.recovered_energy_kwh = energy_in_kwh_;
        t.ds_metric_trace = diag_.ds_metric_trace;
        t.ds_entropy = diag_.ds_entropy;
        t.ds_confidence = diag_.ds_confidence;

        t.limiting_factor = diag_.safety_fault ? "SAFETY" : "NONE";

        return t;
    }

private:
    void update_diagnostics(double dt, const ds::DSState& observed) {
        auto& s = enhanced_.state;

        diag_.pack_soc_percent = s.state_of_charge * 100.0;
        diag_.pack_soh_percent = (1.0 - s.degradation) * 100.0;
        diag_.pack_health_percent = diag_.pack_soh_percent;
        diag_.instantaneous_power_kw = (s.voltage * s.current) / 1000.0;
        diag_.ds_metric_trace = s.g_eff.trace();
        diag_.ds_entropy = s.entropy;
        diag_.ds_phi_magnitude = s.phi_magnitude;
        diag_.ds_confidence = enhanced_.ds_confidence;
        diag_.metric_trace = diag_.ds_metric_trace;
        diag_.phi_magnitude = diag_.ds_phi_magnitude;
        diag_.entropy_level = diag_.ds_entropy;
        diag_.energy_throughput_kwh = energy_in_kwh_ + energy_out_kwh_;
        diag_.last_update_time_ms = dt * 1000.0;
        const double next_update_count = static_cast<double>(updates_ + 1);
        total_update_time_ms_ += diag_.last_update_time_ms;
        diag_.average_update_time_ms = total_update_time_ms_ / std::max(1.0, next_update_count);
        if (s.cycle_count > 0.0) {
            diag_.degradation_rate_per_cycle = s.degradation / s.cycle_count;
        } else {
            diag_.degradation_rate_per_cycle = 0.0;
        }

        diag_.weak_cell_warning = false;
        diag_.thermal_warning = observed.temperature > config_.safety_limits.max_operating_temp;
        diag_.balancing_required = false;
        bool imbalance_out_of_bounds = false;
        const bool pack_voltage_out_of_bounds =
            (s.voltage < config_.safety_limits.min_pack_voltage) ||
            (s.voltage > config_.safety_limits.max_pack_voltage);

        if (pack_) {
            diag_.total_cells = static_cast<int>(pack_->get_cells().size());
            diag_.weak_cell_ids = pack_->get_weak_cell_ids();
            diag_.weak_cell_count = static_cast<int>(diag_.weak_cell_ids.size());
            diag_.voltage_imbalance_mv = pack_->get_voltage_imbalance() * 1000.0;
            diag_.average_cell_temp_c = pack_->get_average_temperature();
            diag_.min_cell_temp_c = pack_->get_min_cell_temperature();
            diag_.max_cell_temp_c = pack_->get_max_cell_temperature();
            diag_.weak_cell_warning = diag_.weak_cell_count > 0;
            diag_.thermal_warning = diag_.thermal_warning ||
                diag_.max_cell_temp_c > config_.safety_limits.max_cell_temp;
            diag_.balancing_required =
                diag_.voltage_imbalance_mv > config_.safety_limits.max_voltage_imbalance_mv;
            imbalance_out_of_bounds = diag_.balancing_required;
        }
        diag_.voltage_warning = pack_voltage_out_of_bounds || imbalance_out_of_bounds;

        diag_.estimated_remaining_cycles = std::max(0.0, (1.0 - s.degradation) * 2000.0);

        if (config_.enable_safety_monitoring) {
            diag_.safety_fault = !safety_.check(observed, diag_);
            diag_.soc_warning = safety_.has_soc_warning();
            diag_.low_temp_warning = safety_.has_low_temp_warning();
        }

        diag_.time_since_init_s += dt;
        diag_.update_count = ++updates_;
    }
};

} // namespace ds_plugin

#endif
