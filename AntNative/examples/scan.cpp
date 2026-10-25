// Opens the first ANT stick, scans for ANT+ sensors and prints their readings, through the C++ API - the loop a
// game would run each frame.
//
// Build (VS 2022 Developer prompt, from AntNative):
//   cl /nologo /W4 /EHsc /std:c++17 /Iinclude examples\scan.cpp build\Release\antnative.lib
// Run with build\Release\antnative.dll beside it or on PATH, passing the ANT+ network key as 16 hex digits (it
// comes with the ANT+ Adopter agreement, so it isn't written into this example):
//   scan.exe <network key> [seconds]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "antnative.hpp"

namespace {

bool parse_key(const std::string& text, antnative::Payload& key) {
    if (text.size() != 16) {
        return false;
    }
    for (std::size_t i = 0; i < 8; ++i) {
        char* end = nullptr;
        const std::string pair = text.substr(i * 2, 2);
        key[i] = static_cast<std::uint8_t>(std::strtoul(pair.c_str(), &end, 16));
        if (end != pair.c_str() + 2) {
            return false;
        }
    }
    return true;
}

void print(const ant_message& m) {
    if (m.fields & ANT_FIELD_HEART_RATE) {
        std::printf("HR    %5u  %3u bpm\n", m.device_number, m.computed_heart_rate);
    } else if (m.fields & ANT_FIELD_POWER) {
        std::printf("Power %5u  %4u W\n", m.device_number, m.instantaneous_power_watts);
    } else if (m.fields & ANT_FIELD_SPEED_CADENCE) {
        std::printf("S&C   %5u  crank revs %u, wheel revs %u\n", m.device_number,
                    m.cadence_revolution_count, m.speed_revolution_count);
    } else if (m.fields & ANT_FIELD_SPEED_OR_CADENCE) {
        std::printf("%s %5u  revs %u\n", m.device_type == ANT_DEVICE_TYPE_SPEED ? "Speed" : "Cad. ",
                    m.device_number, m.revolution_count);
    } else if ((m.fields & ANT_FIELD_DEVICE_ID) && m.device_type == ANT_DEVICE_TYPE_FITNESS_EQUIPMENT) {
        std::printf("FE-C  %5u  page 0x%02X\n", m.device_number, m.payload[0]);
    }
}

}  // namespace

int main(int argc, char** argv) {
    antnative::Payload key{};
    if (argc < 2 || !parse_key(argv[1], key)) {
        std::fprintf(stderr, "usage: scan <ANT+ network key as 16 hex digits> [seconds]\n");
        return 2;
    }
    const int seconds = argc > 2 ? std::atoi(argv[2]) : 30;

    try {
        if (antnative::list_devices().empty()) {
            std::fprintf(stderr, "No ANT stick attached\n");
            return 1;
        }

        antnative::Device stick;
        if (!stick.open()) {
            std::fprintf(stderr, "Couldn't open the stick (is another program using it?)\n");
            return 1;
        }
        // The scan setup ANTManager.configure_scan uses: one channel listening to every device, with the
        // extended data that says which device each message came from
        const bool configured = stick.reset() &&
                                stick.set_network_key(0, key) &&
                                stick.assign_channel(0, 1, 0) &&
                                stick.set_channel_id(0, 0, 0, 0) &&
                                stick.set_channel_rf_frequency(0, 57) &&
                                stick.set_rx_extended_messages_enabled(true) &&
                                stick.open_rx_scan_mode();
        if (!configured) {
            std::fprintf(stderr, "The stick refused the scan setup\n");
            return 1;
        }

        stick.start_messages();
        std::printf("Scanning for %d s...\n", seconds);
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        int received = 0;
        while (std::chrono::steady_clock::now() < end) {
            for (const ant_message& m : stick.poll()) {
                ++received;
                print(m);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));  // a game's frame
        }
        std::printf("%d messages, %llu dropped\n", received,
                    static_cast<unsigned long long>(stick.dropped_messages()));
    } catch (const antnative::Error& error) {
        std::fprintf(stderr, "AntNative error %d: %s\n", error.code(), error.what());
        return 1;
    }
    return 0;
}
