#pragma once

// Python's random.Random, for what has to come out the same as the Python app's: CPython's Mersenne Twister seeded
// from an integer the way random.seed(n) does it, random(), and gauss() with its spare value kept for the next call.
// PreviewRide's simulated rider draws from it, so a simulated ride matches the Python app's for the same seed.

#include <array>
#include <cstdint>
#include <optional>

namespace awo::py {

class Random {
public:
    explicit Random(std::uint64_t seed = 0);

    void seed(std::uint64_t seed);
    // [0, 1) with 53 random bits
    double random();
    double gauss(double mu, double sigma);
    std::uint32_t genrand();

private:
    void initGenrand(std::uint32_t s);
    void initByArray(const std::uint32_t* key, int length);

    std::array<std::uint32_t, 624> mt_{};
    int index_ = 625;
    std::optional<double> gaussNext_;
};

}  // namespace awo::py
