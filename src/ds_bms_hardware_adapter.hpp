#ifndef DS_BMS_HARDWARE_ADAPTER_HPP
#define DS_BMS_HARDWARE_ADAPTER_HPP
/*
 * ============================================================================
 * DS BMS Hardware Adapter – Production-lean Reference Implementation
 * ============================================================================
 *
 * PURPOSE:
 *   This file is a manufacturer-friendly reference adapter showing how to bridge:
 *     - Real BMS hardware (CAN / ADC / cell monitoring ICs)
 *     - DS middleware interfaces (IPackSensors, ICellSensors, IBatteryActuators)
 *
 * DESIGN GOALS:
 *   - Minimal vendor lock-in: OEMs implement ICANTransport + mapping table.
 *   - Safety-first: stale-data detection, bounds checks, conservative behavior.
 *   - Clear plugin points: where a manufacturer wires their hardware stack.
 *
 * WHAT THIS IS:
 *   - A production-lean skeleton with real patterns (not a “dummy”).
 *
 * WHAT THIS IS NOT:
 *   - A complete product CAN database (DBC) or a full vehicle integration.
 *
 * OEM INTEGRATION MODEL:
 *   1) Implement ICANTransport for your platform (SocketCAN, Vector, Kvaser, etc.)
 *   2) Fill in the CAN mapping IDs and signal scaling factors below
 *   3) If cell data comes from SPI/I2C monitor ICs, implement ICellTelemetryBackend
 *   4) Instantiate DSHardwareAdapter in your main ECU/BMS loop
 *
 * ============================================================================
 */

#include "ds_bms_interfaces.hpp"

#include <cstdint>
#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include <optional>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ds_plugin {

/* ============================================================================
 * CAN TRANSPORT CONTRACT (OEM implements this)
 * ============================================================================
 *
 * Why this contract exists:
 *   Every manufacturer has a different CAN stack (driver, RTOS, buffering).
 *   This interface keeps your middleware portable across vendors.
 */

struct CANFrame {
    uint32_t id = 0;           // arbitration ID (11-bit or 29-bit)
    bool is_extended = false;  // 29-bit if true
    uint8_t dlc = 8;
    uint8_t data[8] = {0};
};

class ICANTransport {
public:
    virtual ~ICANTransport() = default;

    // Send a CAN frame immediately (non-blocking preferred)
    virtual bool send(const CANFrame& frame) = 0;

    // Poll/receive one frame (non-blocking). Return std::nullopt if none.
    virtual std::optional<CANFrame> receive() = 0;
};

/* ============================================================================
 * OPTIONAL CELL TELEMETRY BACKEND (OEM implements if cells not on CAN)
 * ============================================================================
 *
 * Many packs do NOT stream all cell voltages on CAN due to bandwidth.
 * Instead, a BMS MCU reads cell monitor ICs via SPI/I2C.
 *
 * If your architecture reads cells over SPI/I2C, implement this backend.
 */

class ICellTelemetryBackend {
public:
    virtual ~ICellTelemetryBackend() = default;
    virtual bool read_cell_voltages(std::vector<double>& out_voltages) = 0;     // volts
    virtual bool read_cell_temperatures(std::vector<double>& out_temps) = 0;   // °C
};

/* ============================================================================
 * OEM SIGNAL MAP (edit these for your DBC/hardware)
 * ============================================================================
 *
 * NOTE: IDs and scalings below are placeholders. OEM must align with their DBC.
 * Keep the names stable so vendors can diff easily.
 */

struct OEMSignalMap {
    // Pack signals (CAN)
    uint32_t pack_voltage_id = 0x180;     // Example ID
    uint32_t pack_current_id = 0x181;     // Example ID
    uint32_t pack_temp_id    = 0x182;     // Example ID
    uint32_t pack_soc_id     = 0x183;     // Example ID

    // Actuators (CAN)
    uint32_t contactor_cmd_id = 0x200;    // Example ID
    uint32_t balancing_cmd_id = 0x201;    // Example ID

    // Scaling factors (example)
    // Pack voltage might be encoded as uint16 where value = raw * 0.1 V
    double pack_voltage_scale = 0.1;      // V per LSB
    double pack_current_scale = 0.1;      // A per LSB (signed)
    double pack_temp_scale    = 0.1;      // °C per LSB (signed)
    double pack_soc_scale     = 0.1;      // % per LSB

    // Staleness thresholds
    double max_signal_age_s = 0.50;       // conservative default: 500ms

    // Optional range validation (disabled by default)
    bool enable_range_validation = false;
    double pack_voltage_min = 0.0;        // V
    double pack_voltage_max = 1000.0;     // V
    double pack_current_min = -2000.0;    // A
    double pack_current_max = 2000.0;     // A
    double pack_temp_min = -40.0;         // °C
    double pack_temp_max = 120.0;         // °C
    double pack_soc_min = 0.0;            // %
    double pack_soc_max = 100.0;          // %
};

/* ============================================================================
 * INTERNAL CACHED TELEMETRY
 * ============================================================================
 */

struct CachedSignal {
    double value = 0.0;
    double timestamp_s = -1.0;
    bool valid = false;
};

/* ============================================================================
 * DSHardwareAdapter
 * ============================================================================
 *
 * Implements:
 *   - IPackSensors
 *   - ICellSensors
 *   - IBatteryActuators
 *   - ITimeSource
 *
 * Thread model:
 *   - Either call poll_can() in your main loop
 *   - Or call poll_can() in a dedicated thread (with mutex protection)
 */

class DSHardwareAdapter final
    : public IPackSensors
    , public ICellSensors
    , public IBatteryActuators
    , public ITimeSource
{
public:
    DSHardwareAdapter(
        ICANTransport& can,
        OEMSignalMap map,
        int series_cells,
        ICellTelemetryBackend* cell_backend = nullptr
    )
        : can_(can),
          map_(map),
          series_cells_(series_cells),
          cell_backend_(cell_backend)
    {
        if (std::isnan(map_.max_signal_age_s) || std::isinf(map_.max_signal_age_s) || map_.max_signal_age_s < 0.0) {
            throw std::invalid_argument("max_signal_age_s must be finite and nonnegative");
        }
        cell_voltages_.assign(std::max(1, series_cells_), 0.0);
        cell_temps_.assign(std::max(1, series_cells_), 25.0);
    }

    /* ===================== TIME SOURCE ===================== */

    // OEM NOTE:
    // Replace this if you have a system time service. The default uses a simple
    // monotonic counter driven externally via set_now_seconds().
    double now_seconds() const override {
        return now_s_.load(std::memory_order_relaxed);
    }

    void set_now_seconds(double t) {
        now_s_.store(t, std::memory_order_relaxed);
    }

    /* ===================== CAN POLLING ===================== */

    // Call this frequently (e.g., 100–500 Hz) to keep cache fresh.
    // In a production ECU, this often runs on its own thread.
    void poll_can() {
        while (true) {
            auto frame_opt = can_.receive();
            if (!frame_opt.has_value()) break;
            handle_frame(*frame_opt);
        }
    }

    /* ===================== PACK SENSORS ===================== */

    double read_pack_voltage() const override {
        return get_fresh_or_throw(pack_voltage_, "pack_voltage");
    }

    double read_pack_current() const override {
        return get_fresh_or_throw(pack_current_, "pack_current");
    }

    double read_pack_temperature() const override {
        return get_fresh_or_throw(pack_temp_, "pack_temperature");
    }

    double estimate_soc() const override {
        // SOC is optional on some systems. If not present, OEM can:
        //   - derive from coulomb counting
        //   - derive from OCV model
        //   - supply from BMS internal estimate
        // This implementation expects SOC on CAN as percent.
        double soc_percent = get_fresh_or_throw(pack_soc_, "pack_soc");
        return std::clamp(soc_percent / 100.0, 0.0, 1.0);
    }

    /* ===================== CELL SENSORS ===================== */

    std::vector<double> read_cell_voltages() const override {
        std::lock_guard<std::mutex> lk(cell_mutex_);
        return cell_voltages_;
    }

    std::vector<double> read_cell_temperatures() const override {
        std::lock_guard<std::mutex> lk(cell_mutex_);
        return cell_temps_;
    }

    // OEM NOTE:
    // If your cell telemetry is NOT on CAN, call this each loop.
    // It pulls from the cell_backend_ (SPI/I2C monitor ICs).
    bool refresh_cells_from_backend() {
        if (!cell_backend_) return false;
        std::vector<double> v, t;
        if (!cell_backend_->read_cell_voltages(v)) return false;
        if (!cell_backend_->read_cell_temperatures(t)) return false;

        // Basic validation & clamp
        if (static_cast<int>(v.size()) != series_cells_) return false;
        if (static_cast<int>(t.size()) != series_cells_) return false;

        std::lock_guard<std::mutex> lk(cell_mutex_);
        cell_voltages_ = std::move(v);
        cell_temps_ = std::move(t);
        return true;
    }

    /* ===================== ACTUATORS ===================== */

    void enable_discharge(bool enable) override {
        // OEM NOTE:
        // Many systems control discharge/charge via contactors + inverter handshake.
        // This sends a reference command. OEM should map to their real safety logic.
        send_contactor_command(enable, charge_enabled_.load(), "enable_discharge");
        discharge_enabled_.store(enable);
    }

    void enable_charge(bool enable) override {
        send_contactor_command(discharge_enabled_.load(), enable, "enable_charge");
        charge_enabled_.store(enable);
    }

    void request_cell_balancing(const std::vector<int>& cell_ids) override {
        // OEM NOTE:
        // Balancing command schemes vary. Some use bitmasks, others send a list.
        // Here we implement a compact bitmask for up to 64 cells (extendable).
        // If disturbed by this assumption, OEM should replace pack message encoding.
        CANFrame f;
        f.id = map_.balancing_cmd_id;
        f.is_extended = false;
        f.dlc = 8;

        uint64_t mask = 0;
        for (int id : cell_ids) {
            if (id >= 0 && id < 64) mask |= (1ULL << static_cast<uint64_t>(id));
        }

        for (int b = 0; b < 8; ++b) {
            f.data[b] = static_cast<uint8_t>((mask >> (8 * b)) & 0xFF);
        }

        if (!can_.send(f)) {
            can_send_failures_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    uint32_t can_send_failures() const {
        return can_send_failures_.load(std::memory_order_relaxed);
    }

private:
    /* ===================== FRAME HANDLING ===================== */

    void handle_frame(const CANFrame& f) {
        if (f.dlc < 2) {
            return;
        }
        const double t = now_seconds();

        // OEM NOTE:
        // You’ll decode signals according to your DBC. This reference assumes:
        //   - uint16 little-endian for pack voltage
        //   - int16 little-endian for signed current/temp
        // Modify decode_* to match your DBC.
        if (f.id == map_.pack_voltage_id) {
            double value = decode_u16_le(f) * map_.pack_voltage_scale;
            set_signal(pack_voltage_, apply_range_limits(value, map_.pack_voltage_min, map_.pack_voltage_max), t);
        } else if (f.id == map_.pack_current_id) {
            double value = decode_i16_le(f) * map_.pack_current_scale;
            set_signal(pack_current_, apply_range_limits(value, map_.pack_current_min, map_.pack_current_max), t);
        } else if (f.id == map_.pack_temp_id) {
            double value = decode_i16_le(f) * map_.pack_temp_scale;
            set_signal(pack_temp_, apply_range_limits(value, map_.pack_temp_min, map_.pack_temp_max), t);
        } else if (f.id == map_.pack_soc_id) {
            double value = decode_u16_le(f) * map_.pack_soc_scale;
            set_signal(pack_soc_, apply_range_limits(value, map_.pack_soc_min, map_.pack_soc_max), t);
        }

        // OEM NOTE:
        // If your architecture streams cell voltages/temps on CAN, decode here.
        // Many OEMs instead use SPI/I2C telemetry, so this is left as a plug point.
    }

    double apply_range_limits(double value, double min_value, double max_value) const {
        if (!map_.enable_range_validation) {
            return value;
        }
        return std::clamp(value, min_value, max_value);
    }

    static uint16_t decode_u16_le(const CANFrame& f) {
        return static_cast<uint16_t>(f.data[0] | (static_cast<uint16_t>(f.data[1]) << 8));
    }

    static int16_t decode_i16_le(const CANFrame& f) {
        return static_cast<int16_t>(f.data[0] | (static_cast<int16_t>(f.data[1]) << 8));
    }

    void set_signal(CachedSignal& s, double value, double t) {
        std::lock_guard<std::mutex> lk(sig_mutex_);
        s.value = value;
        s.timestamp_s = t;
        s.valid = true;
    }

    double get_fresh_or_throw(const CachedSignal& s, const char* name) const {
        std::lock_guard<std::mutex> lk(sig_mutex_);
        if (!s.valid) {
            throw std::runtime_error(std::string("Missing required signal: ") + name);
        }

        const double current_time = now_seconds();
        if (std::isnan(current_time) || std::isinf(current_time)) {
            throw std::runtime_error(std::string("Invalid clock: ") + name);
        }
        if (std::isnan(s.timestamp_s) || std::isinf(s.timestamp_s)) {
            throw std::runtime_error(std::string("Invalid timestamp: ") + name);
        }

        const double age = current_time - s.timestamp_s;

        if (std::isnan(age) || std::isinf(age)) {
            throw std::runtime_error(std::string("Invalid age calculation: ") + name);
        }

        if (age < 0.0) {
            throw std::runtime_error(std::string("Future timestamp or backward clock: ") + name);
        }

        if (std::isnan(map_.max_signal_age_s) || std::isinf(map_.max_signal_age_s) || map_.max_signal_age_s < 0.0) {
            throw std::runtime_error(std::string("Invalid max_signal_age_s config: ") + name);
        }

        if (age > map_.max_signal_age_s) {
            throw std::runtime_error(std::string("Stale signal: ") + name);
        }
        return s.value;
    }

    void send_contactor_command(bool discharge, bool charge, const char* /*reason*/) {
        CANFrame f;
        f.id = map_.contactor_cmd_id;
        f.is_extended = false;
        f.dlc = 2;
        f.data[0] = discharge ? 1 : 0;
        f.data[1] = charge ? 1 : 0;
        if (!can_.send(f)) {
            can_send_failures_.fetch_add(1, std::memory_order_relaxed);
        }
    }

private:
    ICANTransport& can_;
    OEMSignalMap map_;
    int series_cells_;
    ICellTelemetryBackend* cell_backend_;

    mutable std::mutex sig_mutex_;
    CachedSignal pack_voltage_;
    CachedSignal pack_current_;
    CachedSignal pack_temp_;
    CachedSignal pack_soc_;

    mutable std::mutex cell_mutex_;
    std::vector<double> cell_voltages_;
    std::vector<double> cell_temps_;

    std::atomic<double> now_s_{0.0};
    std::atomic<bool> discharge_enabled_{true};
    std::atomic<bool> charge_enabled_{true};
    std::atomic<uint32_t> can_send_failures_{0};
};

} // namespace ds_plugin

#endif // DS_BMS_HARDWARE_ADAPTER_HPP
