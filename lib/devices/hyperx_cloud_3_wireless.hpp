#pragma once

#include "../result_types.hpp"
#include "hid_device.hpp"
#include <array>
#include <optional>
#include <string_view>

using namespace std::string_view_literals;

namespace headsetcontrol {

/**
 * @brief HyperX Cloud III Wireless Gaming Headset
 *
 * Features:
 * - Battery status
 * - Inactive time/auto-shutoff
 * - Sidetone control
 */
class HyperXCloudIIIWireless : public HIDDevice {
public:
    static constexpr uint16_t VENDOR_HP = 0x03f0;
    static constexpr std::array<uint16_t, 2> SUPPORTED_PRODUCT_IDS {
        0x05b7,
        0x0c9d
    };

    static constexpr int MSG_SIZE   = 62;
    static constexpr int TIMEOUT_MS = 2000;

    static constexpr uint8_t PACKET_HEADER = 0x66;

    static constexpr uint8_t CMD_GET_CHARGING      = 138;
    static constexpr uint8_t CMD_GET_BATTERY       = 137;
    static constexpr uint8_t CMD_SET_AUTO_SHUTDOWN = 2;
    static constexpr uint8_t CMD_SET_SIDETONE_ON   = 1;
    static constexpr uint8_t CMD_SET_SIDETONE_VOL  = 5;

    static constexpr uint8_t RESP_CHARGING = 12;
    static constexpr uint8_t RESP_BATTERY  = 13;

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
        return "HyperX Cloud III Wireless"sv;
    }

    constexpr int getCapabilities() const override
    {
        return B(CAP_BATTERY_STATUS) | B(CAP_INACTIVE_TIME) | B(CAP_SIDETONE);
    }

    Result<BatteryResult> getBattery(hid_device* device_handle) override
    {
        auto level_res = sendCommand(device_handle, CMD_GET_BATTERY);
        if (!level_res) {
            return level_res.error();
        }

        auto charge_res = sendCommand(device_handle, CMD_GET_CHARGING);
        if (!charge_res) {
            return charge_res.error();
        }

        int level = -1;
        if (((*level_res)[1] == CMD_GET_BATTERY || (*level_res)[1] == RESP_BATTERY) && ((*level_res)[2] != 0 || (*level_res)[3] != 0)) {
            level = (*level_res)[4];
        }

        enum battery_status status = BATTERY_AVAILABLE;
        if ((*charge_res)[1] == CMD_GET_CHARGING || (*charge_res)[1] == RESP_CHARGING) {
            status = ((*charge_res)[2] == 1) ? BATTERY_CHARGING : BATTERY_AVAILABLE;
        }

        return BatteryResult {
            .level_percent = level,
            .status        = status,
            .raw_data      = std::vector<uint8_t> { level_res->begin(), level_res->end() }
        };
    }

    Result<SidetoneResult> setSidetone(hid_device* device_handle, uint8_t level) override
    {
        auto toggle = sendCommand(device_handle, CMD_SET_SIDETONE_ON, level > 0 ? 1 : 0);
        if (!toggle) {
            return toggle.error();
        }

        if (level > 0) {
            auto vol = sendCommand(device_handle, CMD_SET_SIDETONE_VOL, level);
            if (!vol) {
                return vol.error();
            }
        }

        return SidetoneResult {
            .current_level = level,
            .min_level     = 0,
            .max_level     = 128,
            .device_min    = 0,
            .device_max    = 128
        };
    }

    Result<InactiveTimeResult> setInactiveTime(hid_device* device_handle, uint8_t minutes) override
    {
        auto res = sendCommand(device_handle, CMD_SET_AUTO_SHUTDOWN, minutes);
        if (!res) {
            return res.error();
        }

        return InactiveTimeResult {
            .minutes     = minutes,
            .min_minutes = 0,
            .max_minutes = 255
        };
    }

private:
    Result<std::array<uint8_t, MSG_SIZE>> sendCommand(hid_device* device_handle, uint8_t command, std::optional<uint8_t> value = std::nullopt)
    {
        std::array<uint8_t, MSG_SIZE> request {};
        request[0] = PACKET_HEADER;
        request[1] = command;
        if (value.has_value()) {
            request[2] = *value;
        }

        if (auto wr = writeHID(device_handle, request); !wr) {
            return wr.error();
        }

        std::array<uint8_t, MSG_SIZE> response {};
        auto rd = readHIDTimeout(device_handle, response, TIMEOUT_MS);
        if (!rd) {
            return rd.error();
        }
        if (*rd < 2 || response[0] != PACKET_HEADER) {
            return DeviceError::protocolError("Invalid Cloud III Wireless response");
        }

        return response;
    }
};

} // namespace headsetcontrol
