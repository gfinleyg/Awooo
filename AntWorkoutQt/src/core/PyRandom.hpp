#pragma once

// Python's random.Random, for what has to come out the same as the Python app's: CPython's Mersenne Twister seeded
// from an integer the way random.seed(n) does it, random(), gauss() with its spare value kept for the next call, and
// (CPython 3.10's) getrandbits, shuffle and choice. PreviewRide's simulated rider draws from it, so a simulated ride
// matches the Python app's for the same seed; so does a random pick of songs (MusicCadence.random_selection).

#include <array>
#include <cstdint>
#include <optional>
#include <utility>

namespace awo::py {

class Random {
public:
    explicit Random(std::uint64_t seed = 0);

    void seed(std::uint64_t seed);
    // [0, 1) with 53 random bits
    double random();
    double gauss(double mu, double sigma);
    std::uint32_t genrand();
    // k (1-32) random bits
    std::uint32_t getrandbits(int k);
    // [0, n), as random._randbelow_with_getrandbits
    std::uint32_t randbelow(std::uint32_t n);
    // random.shuffle: in place, last to first
    template <typename List>
    void shuffle(List& list) {
        for (auto i = static_cast<std::uint32_t>(list.size()); i > 1; --i) {
            const std::uint32_t j = randbelow(i);
            std::swap(list[i - 1], list[j]);
        }
    }
    // random.choice's index into a sequence this long
    std::uint32_t choiceIndex(std::uint32_t length) { return randbelow(length); }

private:
    void initGenrand(std::uint32_t s);
    void initByArray(const std::uint32_t* key, int length);

    std::array<std::uint32_t, 624> mt_{};
    int index_ = 625;
    std::optional<double> gaussNext_;
};

}  // namespace awo::py
