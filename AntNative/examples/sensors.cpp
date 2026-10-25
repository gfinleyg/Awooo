// Finds the ANT+ sensors in range and shows their readings once a second, through the C++ manager API - what a
// game would read each frame. Moves them onto their own channels after a few seconds (as AntWorkout does once
// sensors are chosen) and runs the watchdog.
//
// Build (VS 2022 Developer prompt, from AntNative):
//   cl /nologo /W4 /EHsc /std:c++17 /Iinclude examples\sensors.cpp build\Release\antnative.lib
// Run with build\Release\antnative.dll beside it or on PATH, passing the ANT+ network key as 16 hex digits (it
// comes with the ANT+ Adopter agreement, so it isn't written into this example):
//   sensors.exe <network key> [seconds] [stick number]

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

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

const char* type_name(int device_type) {
    switch (device_type) {
        case ANT_DEVICE_TYPE_HEART_RATE: return "Heart rate";
        case ANT_DEVICE_TYPE_POWER: return "Power";
        case ANT_DEVICE_TYPE_FITNESS_EQUIPMENT: return "Trainer";
        case ANT_DEVICE_TYPE_SPEED_CADENCE: return "Speed/cad";
        case ANT_DEVICE_TYPE_CADENCE: return "Cadence";
        case ANT_DEVICE_TYPE_SPEED: return "Speed";
        default: return "Other";
    }
}

void print(const ant_sensor& s) {
    static const char* const states[] = {"active", "stale", "lost"};
    std::printf("  %-9s %5u  ch %2d  %-6s", type_name(s.device_type), s.device_number, s.channel, states[s.state]);
    if (s.fields & ANT_READING_HEART_RATE) std::printf("  %3d bpm", s.heart_rate);
    if (s.fields & ANT_READING_RR_INTERVAL) std::printf("  rr %.3f s", s.rr_interval);
    if (s.fields & ANT_READING_POWER) std::printf("  %4d W", s.power_watts);
    if (s.fields & ANT_READING_CADENCE) std::printf("  %5.1f rpm", s.cadence_rpm);
    if (s.fields & ANT_READING_SPEED) std::printf("  %5.1f km/h", s.speed_mps * 3.6);
    if (s.fields & ANT_READING_BATTERY) std::printf("  battery %d", s.battery_status);
    std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
    antnative::Payload key{};
    if (argc < 2 || !parse_key(argv[1], key)) {
        std::fprintf(stderr, "usage: sensors <ANT+ network key as 16 hex digits> [seconds] [stick number]\n");
        return 2;
    }
    const int seconds = argc > 2 ? std::atoi(argv[2]) : 30;
    const int stick = argc > 3 ? std::atoi(argv[3]) : -1;

    try {
        antnative::Manager ant;
        if (!ant.open(stick)) {
            std::fprintf(stderr, "Couldn't open an ANT stick (none attached, or all in use?)\n");
            return 1;
        }
        ant.configure_scan(key);
        ant.start();
        ant.start_recovery();

        bool moved = false;
        const auto start = std::chrono::steady_clock::now();
        for (int second = 1; second <= seconds; ++second) {
            std::this_thread::sleep_until(start + std::chrono::seconds(second));
            const std::vector<ant_sensor> sensors = ant.sensors();

            // After 5 s of scanning, give every sensor found its own channel
            if (!moved && second >= 5 && !sensors.empty()) {
                std::vector<ant_sensor_id> ids;
                for (const ant_sensor& s : sensors) {
                    ids.push_back({s.device_type, s.device_number, s.transmission_type});
                }
                ant.open_device_channels(ids);
                ant.watch(ids);
                moved = true;
                std::printf("-- moved %zu sensor(s) to their own channels\n", ids.size());
            }

            std::printf("[%2d s] %zu sensor(s)\n", second, sensors.size());
            for (const ant_sensor& s : sensors) {
                print(s);
            }
            while (std::optional<std::string> line = ant.next_log()) {
                std::printf("  log: %s\n", line->c_str());
            }
        }
    } catch (const antnative::Error& error) {
        std::fprintf(stderr, "AntNative error %d: %s\n", error.code(), error.what());
        return 1;
    }
    return 0;
}
