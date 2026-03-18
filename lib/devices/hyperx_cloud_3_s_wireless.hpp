#pragma once

#include "../result_types.hpp"
#include "hid_device.hpp"
#include <algorithm>
#include <array>
#include <string_view>

using namespace std::string_view_literals;

namespace headsetcontrol {

/**
 * @brief HyperX Cloud III S Wireless Gaming Headset
 *
 * Features:
 * - Battery status
 * - Inactive time/auto-shutoff
 * - 10-band equalizer
 */
class HyperXCloudIIISWireless : public HIDDevice {
public:
    static constexpr uint16_t VENDOR_HP = 0x03f0;
    static constexpr std::array<uint16_t, 1> SUPPORTED_PRODUCT_IDS {
        0x06be
    };

    static constexpr int QUERY_PACKET_SIZE   = 62;
    static constexpr int FEATURE_PACKET_SIZE = 64;
    static constexpr int TIMEOUT_MS          = 2000;

    static constexpr uint8_t REPORT_ID = 0x0c;
    static constexpr uint8_t QUERY_1   = 0x02;
    static constexpr uint8_t QUERY_2   = 0x03;

    static constexpr uint8_t CMD_GET_BATTERY  = 0x06;
    static constexpr uint8_t CMD_GET_CHARGING = 0x48;
    static constexpr uint8_t CMD_SET_INACTIVE = 0x4a;
    static constexpr uint8_t CMD_SET_EQ_BAND  = 0x5f;

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

        int level = -1;
        if ((*level_res)[0] == REPORT_ID && (*level_res)[5] == CMD_GET_BATTERY) {
            level = (*level_res)[6];
        }

        enum battery_status status = BATTERY_AVAILABLE;
        if ((*charge_res)[0] == REPORT_ID && (*charge_res)[5] == CMD_GET_CHARGING) {
            status = ((*charge_res)[6] == 1) ? BATTERY_CHARGING : BATTERY_AVAILABLE;
        }

        return BatteryResult {
            .level_percent = level,
            .status        = status,
            .raw_data      = std::vector<uint8_t> { level_res->begin(), level_res->end() }
        };
    }

    Result<InactiveTimeResult> setInactiveTime(hid_device* device_handle, uint8_t minutes) override
    {
        std::array<uint8_t, FEATURE_PACKET_SIZE> packet {};
        packet[0] = REPORT_ID;
        packet[1] = QUERY_1;
        packet[2] = QUERY_2;
        packet[3] = 0x00;
        packet[4] = 0x00;
        packet[5] = CMD_SET_INACTIVE;

        uint16_t seconds = static_cast<uint16_t>(minutes) * 60;
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

    Result<EqualizerResult> setEqualizer(hid_device* device_handle, const EqualizerSettings& settings) override
    {
        if (settings.bands.size() != 10) {
            return DeviceError::invalidParameter("Cloud III S Wireless requires exactly 10 EQ bands");
        }

        for (size_t i = 0; i < settings.bands.size(); i++) {
            std::array<uint8_t, FEATURE_PACKET_SIZE> packet {};
            packet[0] = REPORT_ID;
            packet[1] = QUERY_1;
            packet[2] = QUERY_2;
            packet[3] = 0x00;
            packet[4] = 0x00;
            packet[5] = CMD_SET_EQ_BAND;
            packet[6] = static_cast<uint8_t>(i);

            float clamped = std::clamp(settings.bands[i], -12.0f, 12.0f);
            int16_t db100 = static_cast<int16_t>(clamped * 100.0f);
            auto raw = static_cast<uint16_t>(db100);
            packet[7] = static_cast<uint8_t>((raw >> 8) & 0xff);
            packet[8] = static_cast<uint8_t>(raw & 0xff);

            if (auto sf = sendFeatureReport(device_handle, packet); !sf) {
                return sf.error();
            }
        }

        return EqualizerResult {};
    }

private:
    Result<std::array<uint8_t, QUERY_PACKET_SIZE>> queryCommand(hid_device* device_handle, uint8_t command)
    {
        std::array<uint8_t, QUERY_PACKET_SIZE> packet {};
        packet[0] = REPORT_ID;
        packet[1] = QUERY_1;
        packet[2] = QUERY_2;
        packet[3] = 0x01;
        packet[4] = 0x00;
        packet[5] = command;

        if (auto wr = writeHID(device_handle, packet); !wr) {
            return wr.error();
        }

        std::array<uint8_t, QUERY_PACKET_SIZE> response {};
        auto rd = readHIDTimeout(device_handle, response, TIMEOUT_MS);
        if (!rd) {
            return rd.error();
        }
        if (*rd < 7 || response[0] != REPORT_ID) {
            return DeviceError::protocolError("Invalid Cloud III S Wireless response");
        }

        return response;
    }
};

} // namespace headsetcontrol
