#pragma once

#include "../result_types.hpp"
#include "hid_device.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

using namespace std::string_view_literals;

namespace headsetcontrol {

/**
 * @brief HyperX Cloud III S Wireless Gaming Headset
 *
 * Protocol-aligned with the original Rust implementation:
 * - Query packet (write):  0c 02 03 01 00 <cmd> ...
 * - Feature packet (set):  0c 02 03 00 00 <cmd> ...
 * - Response packet:       0c ...
 * - Notification packet:   0d ...
 * - Mic packet:            05 ...
 * - Consumer control:      0f ...
 *
 * Supported here:
 * - Battery query
 * - Charging state query
 * - Auto-shutdown set/query
 * - 10-band equalizer set
 */
class HyperXCloudIIISWireless : public HIDDevice {
public:
    static constexpr uint16_t VENDOR_HP = 0x03f0;
    static constexpr std::array<uint16_t, 1> SUPPORTED_PRODUCT_IDS {
        0x06be
    };

    // Original Rust sizes
    static constexpr int QUERY_PACKET_SIZE   = 62;
    static constexpr int FEATURE_PACKET_SIZE = 64;

    // Read timing strategy:
    // first read can wait long enough for the real query response,
    // subsequent reads are shorter in case async packets are interleaved.
    static constexpr int FIRST_READ_TIMEOUT_MS      = 2000;
    static constexpr int FOLLOWUP_READ_TIMEOUT_MS   = 250;
    static constexpr int MAX_INTERLEAVED_READS      = 8;

    // Packet headers / report ids
    static constexpr uint8_t RESPONSE_ID                = 0x0c;
    static constexpr uint8_t NOTIFICATION_ID            = 0x0d;
    static constexpr uint8_t MIC_HEADER                 = 0x05;
    static constexpr uint8_t CONSUMER_CONTROL_HEADER    = 0x0f;

    // Shared query/set payload prefix
    static constexpr uint8_t QUERY_1 = 0x02;
    static constexpr uint8_t QUERY_2 = 0x03;

    // Commands from Rust
    static constexpr uint8_t CMD_GET_DONGLE_CONNECTED   = 0x02;
    static constexpr uint8_t CMD_GET_MIC_MUTE           = 0x04;
    static constexpr uint8_t CMD_GET_BATTERY            = 0x06;
    static constexpr uint8_t CMD_GET_VOICE_PROMPT       = 0x14;
    static constexpr uint8_t CMD_GET_SIDE_TONE          = 0x16;
    static constexpr uint8_t CMD_GET_CHARGING           = 0x48;
    static constexpr uint8_t CMD_SET_INACTIVE           = 0x4a;
    static constexpr uint8_t CMD_GET_INACTIVE           = 0x4b;
    static constexpr uint8_t CMD_GET_COLOR              = 0x4d;
    static constexpr uint8_t CMD_SET_EQ_BAND            = 0x5f;

    constexpr uint16_t getVendorId() const override
    {
        return VENDOR_HP;
    }

    std::vector<uint16_t> getProductIds() const override
    {
        return { SUPPORTED_PRODUCT_IDS.begin(), SUPPORTED_PRODUCT_IDS.end() };
    }

    std::string_view getDeviceName() const override
    {
        return "HyperX Cloud III S Wireless"sv;
    }

    constexpr int getCapabilities() const override
    {
        return B(CAP_BATTERY_STATUS) | B(CAP_INACTIVE_TIME) | B(CAP_EQUALIZER);
    }

    std::optional<EqualizerInfo> getEqualizerInfo() const override
    {
        return EqualizerInfo {
            .bands_count    = 10,
            .bands_baseline = 0,
            .bands_step     = 0.1f,
            .bands_min      = -12,
            .bands_max      = 12
        };
    }

    Result<BatteryResult> getBattery(hid_device* device_handle) override
    {
        auto level_res = queryCommand(device_handle, CMD_GET_BATTERY);
        if (!level_res) {
            return level_res.error();
        }

        auto charge_res = queryCommand(device_handle, CMD_GET_CHARGING);
        if (!charge_res) {
            return charge_res.error();
        }

        // Original Rust:
        // BATTERY_COMMAND_ID => DeviceEvent::BatterLevel(response[6])
        int level = static_cast<int>((*level_res)[6]);

        // Original Rust used ChargingStatus::from(response[6]).
        // If your framework only has BATTERY_AVAILABLE / BATTERY_CHARGING,
        // this is the closest loss-minimized mapping.
        enum battery_status status = mapBatteryStatus((*charge_res)[6]);

        return BatteryResult {
            .level_percent = level,
            .status        = status,
            .raw_data      = std::vector<uint8_t> { level_res->begin(), level_res->end() }
        };
    }

    Result<InactiveTimeResult> setInactiveTime(hid_device* device_handle, uint8_t minutes) override
    {
        auto packet = buildFeaturePacket(CMD_SET_INACTIVE);

        // Original Rust:
        // let seconds = (minutes * 60) as u16;
        // packet[6] = high, packet[7] = low
        const uint16_t seconds = static_cast<uint16_t>(minutes) * 60u;
        packet[6] = static_cast<uint8_t>((seconds >> 8) & 0xff);
        packet[7] = static_cast<uint8_t>(seconds & 0xff);

        if (auto sf = sendFeatureReport(device_handle, packet); !sf) {
            return sf.error();
        }

        return InactiveTimeResult {
            .minutes     = minutes,
            .min_minutes = 0,
            .max_minutes = 255
        };
    }

    // Optional helper: aligns with Rust GET_AUTO_POWER_OFF_COMMAND_ID = 0x4B
    // Keep/remove depending on whether your framework exposes a getter API.
    Result<InactiveTimeResult> getInactiveTime(hid_device* device_handle)
    {
        auto response = queryCommand(device_handle, CMD_GET_INACTIVE);
        if (!response) {
            return response.error();
        }

        const uint16_t seconds =
            (static_cast<uint16_t>((*response)[6]) << 8) |
            static_cast<uint16_t>((*response)[7]);

        const uint16_t minutes = static_cast<uint16_t>(seconds / 60u);

        return InactiveTimeResult {
            .minutes     = static_cast<uint8_t>(std::min<uint16_t>(minutes, 255)),
            .min_minutes = 0,
            .max_minutes = 255
        };
    }

    Result<EqualizerResult> setEqualizer(hid_device* device_handle, const EqualizerSettings& settings) override
    {
        // Original Rust only accepts band_index 0..9
        if (settings.bands.size() != 10) {
            return DeviceError::invalidParameter("Cloud III S Wireless requires exactly 10 EQ bands");
        }

        for (size_t i = 0; i < settings.bands.size(); ++i) {
            auto packet = buildFeaturePacket(CMD_SET_EQ_BAND);
            packet[6] = static_cast<uint8_t>(i);

            // Original Rust:
            // let value_int = (db_value * 100.0).clamp(-1200.0, 1200.0) as i16;
            const float clamped = std::clamp(settings.bands[i], -12.0f, 12.0f);
            const int16_t value_db_x100 = static_cast<int16_t>(clamped * 100.0f);

            // Send as signed 16-bit big-endian
            const auto raw = static_cast<uint16_t>(value_db_x100);
            packet[7] = static_cast<uint8_t>((raw >> 8) & 0xff);
            packet[8] = static_cast<uint8_t>(raw & 0xff);

            if (auto sf = sendFeatureReport(device_handle, packet); !sf) {
                return sf.error();
            }
        }

        return EqualizerResult {};
    }

private:
    static constexpr std::array<uint8_t, QUERY_PACKET_SIZE> buildQueryPacket(uint8_t command)
    {
        std::array<uint8_t, QUERY_PACKET_SIZE> packet {};
        packet[0] = RESPONSE_ID; // 0x0c
        packet[1] = QUERY_1;     // 0x02
        packet[2] = QUERY_2;     // 0x03
        packet[3] = 0x01;
        packet[4] = 0x00;
        packet[5] = command;
        return packet;
    }

    static constexpr std::array<uint8_t, FEATURE_PACKET_SIZE> buildFeaturePacket(uint8_t command)
    {
        std::array<uint8_t, FEATURE_PACKET_SIZE> packet {};
        packet[0] = RESPONSE_ID; // 0x0c
        packet[1] = QUERY_1;     // 0x02
        packet[2] = QUERY_2;     // 0x03
        packet[3] = 0x00;
        packet[4] = 0x00;
        packet[5] = command;
        return packet;
    }

    static constexpr bool isAsyncOrUnrelatedHeader(uint8_t header)
    {
        return header == NOTIFICATION_ID
            || header == MIC_HEADER
            || header == CONSUMER_CONTROL_HEADER;
    }

    static enum battery_status mapBatteryStatus(uint8_t raw)
    {
        // Rust used ChargingStatus::from(raw), which may preserve more states.
        // If your framework later grows a richer battery enum, expand this switch.
        switch (raw) {
        case 1:
            return BATTERY_CHARGING;
        default:
            return BATTERY_AVAILABLE;
        }
    }

    Result<std::array<uint8_t, QUERY_PACKET_SIZE>> queryCommand(hid_device* device_handle, uint8_t command)
    {
        const auto packet = buildQueryPacket(command);

        if (auto wr = writeHID(device_handle, packet); !wr) {
            return wr.error();
        }

        // We keep reading until we get the exact matching RESPONSE_ID packet
        // for the command we just sent. This mirrors the Rust parser behavior
        // better than "read once and hope".
        for (int read_index = 0; read_index < MAX_INTERLEAVED_READS; ++read_index) {
            std::array<uint8_t, QUERY_PACKET_SIZE> response {};
            const int timeout = (read_index == 0) ? FIRST_READ_TIMEOUT_MS : FOLLOWUP_READ_TIMEOUT_MS;

            auto rd = readHIDTimeout(device_handle, response, timeout);
            if (!rd) {
                return rd.error();
            }

            if (*rd <= 0) {
                continue;
            }

            const uint8_t header = response[0];

            // Rust parser accepts that the device may emit async packets
            // interleaved with query responses.
            if (isAsyncOrUnrelatedHeader(header)) {
                continue;
            }

            if (header != RESPONSE_ID) {
                continue;
            }

            // Need enough bytes to inspect command/value positions
            if (*rd < 7) {
                continue;
            }

            // Rust parse_response(): if response[6] == 0xFF => ignore / invalid
            if (response[6] == 0xff) {
                return DeviceError::protocolError("Cloud III S Wireless returned invalid sentinel response (0xFF)");
            }

            // Must be the response to the command we sent, not just any 0x0c packet
            if (response[5] != command) {
                continue;
            }

            return response;
        }

        return DeviceError::protocolError("Timed out waiting for matching Cloud III S Wireless response");
    }
};

} // namespace headsetcontrol