#include "../src/ds_bms_hardware_adapter.hpp"
#include <iostream>
#include <cassert>
#include <limits>
#include <optional>
#include <vector>

class FakeCANTransport : public ds_plugin::ICANTransport {
    std::vector<ds_plugin::CANFrame> queue;
public:
    bool send(const ds_plugin::CANFrame&) override {
        throw std::runtime_error("Fake transport does not send");
    }
    std::optional<ds_plugin::CANFrame> receive() override {
        if (queue.empty()) return std::nullopt;
        auto f = queue.front();
        queue.erase(queue.begin());
        return f;
    }
    void push(const ds_plugin::CANFrame& f) {
        queue.push_back(f);
    }
};

void encode_u16_le(ds_plugin::CANFrame& f, uint16_t v) {
    f.data[0] = v & 0xFF;
    f.data[1] = (v >> 8) & 0xFF;
}

bool test_invalid_max_age_config() {
    std::cout << "Testing invalid max_signal_age_s config..." << std::flush;
    FakeCANTransport fake_can;
    ds_plugin::OEMSignalMap map;

    map.max_signal_age_s = -1.0;
    try { ds_plugin::DSHardwareAdapter adapter(fake_can, map, 1); assert(false); } catch (const std::invalid_argument&) {}

    map.max_signal_age_s = std::numeric_limits<double>::quiet_NaN();
    try { ds_plugin::DSHardwareAdapter adapter(fake_can, map, 1); assert(false); } catch (const std::invalid_argument&) {}

    map.max_signal_age_s = std::numeric_limits<double>::infinity();
    try { ds_plugin::DSHardwareAdapter adapter(fake_can, map, 1); assert(false); } catch (const std::invalid_argument&) {}

    std::cout << " PASS\n";
    return true;
}

bool test_clock_freshness() {
    std::cout << "Testing clock freshness evidence (PB-08)..." << std::flush;
    FakeCANTransport fake_can;
    ds_plugin::OEMSignalMap map;
    map.pack_voltage_id = 0x180;
    map.pack_voltage_scale = 1.0;
    map.max_signal_age_s = 0.5;

    ds_plugin::DSHardwareAdapter adapter(fake_can, map, 1);

    // push a frame at timestamp 100s
    adapter.set_now_seconds(100.0);
    ds_plugin::CANFrame f;
    f.id = 0x180;
    f.dlc = 8;
    encode_u16_le(f, 360);
    fake_can.push(f);
    adapter.poll_can();

    // Case 1: Fresh reading
    adapter.set_now_seconds(100.2); // age = 0.2
    assert(adapter.read_pack_voltage() == 360.0);

    // Case 2: Exact boundary
    adapter.set_now_seconds(100.5); // age = 0.5
    assert(adapter.read_pack_voltage() == 360.0);

    // Case 3: Expired signal
    adapter.set_now_seconds(100.6); // age = 0.6
    try { adapter.read_pack_voltage(); assert(false); } catch (const std::runtime_error&) {}

    // Case 4: Future timestamp / backward clock
    adapter.set_now_seconds(0.0); // age = -100
    try { adapter.read_pack_voltage(); assert(false); } catch (const std::runtime_error&) {}

    // Case 5: NaN clock
    adapter.set_now_seconds(std::numeric_limits<double>::quiet_NaN());
    try { adapter.read_pack_voltage(); assert(false); } catch (const std::runtime_error&) {}

    // Case 6: Infinity clock
    adapter.set_now_seconds(std::numeric_limits<double>::infinity());
    try { adapter.read_pack_voltage(); assert(false); } catch (const std::runtime_error&) {}

    // Case 7: Negative infinity as configured max age
    try {
        ds_plugin::OEMSignalMap neg_inf_map;
        neg_inf_map.max_signal_age_s = -std::numeric_limits<double>::infinity();
        ds_plugin::DSHardwareAdapter adapter_neg_inf(fake_can, neg_inf_map, 1);
        assert(false);
    } catch (const std::invalid_argument&) {}

    // Case 8: Zero maximum age with exact timestamp match
    ds_plugin::OEMSignalMap map_zero;
    map_zero.pack_voltage_id = 0x180;
    map_zero.pack_voltage_scale = 1.0;
    map_zero.max_signal_age_s = 0.0;
    ds_plugin::DSHardwareAdapter adapter_zero(fake_can, map_zero, 1);
    adapter_zero.set_now_seconds(200.0);
    ds_plugin::CANFrame f2; f2.id = 0x180; f2.dlc = 8; encode_u16_le(f2, 360);
    fake_can.push(f2);
    adapter_zero.poll_can();
    assert(adapter_zero.read_pack_voltage() == 360.0);

    // Case 9: Zero maximum age with positive elapsed time
    adapter_zero.set_now_seconds(200.0001);
    try { adapter_zero.read_pack_voltage(); assert(false); } catch (const std::runtime_error&) {}

    // Case 10: Non-finite cached timestamp
    adapter.set_now_seconds(std::numeric_limits<double>::quiet_NaN());
    fake_can.push(f);
    adapter.poll_can();
    adapter.set_now_seconds(110.0);
    try { adapter.read_pack_voltage(); assert(false); } catch (const std::runtime_error&) {}

    // Case 11: Arbitrary clock rollback (yielding positive age but undetected internally)
    adapter.set_now_seconds(100.0);
    fake_can.push(f);
    adapter.poll_can();
    adapter.set_now_seconds(100.4);
    assert(adapter.read_pack_voltage() == 360.0);
    // rollback
    adapter.set_now_seconds(100.2);
    assert(adapter.read_pack_voltage() == 360.0); // positive age, not automatically detected!

    std::cout << " PASS\n";
    return true;
}


int main() {
    std::cout << "============================================================================\n";
    std::cout << "DS HARDWARE ADAPTER TESTS\n";
    std::cout << "============================================================================\n\n";

    try {
        bool all_passed = true;
        all_passed &= test_invalid_max_age_config();
        all_passed &= test_clock_freshness();

        std::cout << "\n============================================================================\n";
        if (all_passed) {
            std::cout << "✓ All hardware adapter tests PASSED\n";
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
