#include "PyRandom.hpp"

#include <cmath>
#include <numbers>
#include <vector>

namespace awo::py {

namespace {

constexpr int kN = 624;
constexpr int kM = 397;
constexpr std::uint32_t kMatrixA = 0x9908b0dfU;
constexpr std::uint32_t kUpperMask = 0x80000000U;
constexpr std::uint32_t kLowerMask = 0x7fffffffU;
// (Python's TWOPI, rounded once as it is)
constexpr double kTwoPi = 2.0 * std::numbers::pi;

}  // namespace

Random::Random(std::uint64_t seed) {
    this->seed(seed);
}

void Random::initGenrand(std::uint32_t s) {
    mt_[0] = s;
    for (int i = 1; i < kN; ++i) {
        mt_[i] = 1812433253U * (mt_[i - 1] ^ (mt_[i - 1] >> 30)) + static_cast<std::uint32_t>(i);
    }
    index_ = kN;
}

void Random::initByArray(const std::uint32_t* key, int length) {
    initGenrand(19650218U);
    int i = 1, j = 0;
    for (int k = kN > length ? kN : length; k; --k) {
        mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1664525U)) + key[j] + static_cast<std::uint32_t>(j);
        ++i;
        ++j;
        if (i >= kN) {
            mt_[0] = mt_[kN - 1];
            i = 1;
        }
        if (j >= length) {
            j = 0;
        }
    }
    for (int k = kN - 1; k; --k) {
        mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1566083941U)) - static_cast<std::uint32_t>(i);
        ++i;
        if (i >= kN) {
            mt_[0] = mt_[kN - 1];
            i = 1;
        }
    }
    mt_[0] = 0x80000000U;
}

// (random.seed(n) for an int: its absolute value split into 32 bit words, least significant first)
void Random::seed(std::uint64_t seed) {
    std::vector<std::uint32_t> key;
    do {
        key.push_back(static_cast<std::uint32_t>(seed & 0xffffffffU));
        seed >>= 32;
    } while (seed);
    initByArray(key.data(), static_cast<int>(key.size()));
    gaussNext_.reset();
}

std::uint32_t Random::genrand() {
    static constexpr std::uint32_t mag01[2] = {0x0U, kMatrixA};
    if (index_ >= kN) {
        int kk = 0;
        for (; kk < kN - kM; ++kk) {
            const std::uint32_t y = (mt_[kk] & kUpperMask) | (mt_[kk + 1] & kLowerMask);
            mt_[kk] = mt_[kk + kM] ^ (y >> 1) ^ mag01[y & 0x1U];
        }
        for (; kk < kN - 1; ++kk) {
            const std::uint32_t y = (mt_[kk] & kUpperMask) | (mt_[kk + 1] & kLowerMask);
            mt_[kk] = mt_[kk + (kM - kN)] ^ (y >> 1) ^ mag01[y & 0x1U];
        }
        const std::uint32_t y = (mt_[kN - 1] & kUpperMask) | (mt_[0] & kLowerMask);
        mt_[kN - 1] = mt_[kM - 1] ^ (y >> 1) ^ mag01[y & 0x1U];
        index_ = 0;
    }
    std::uint32_t y = mt_[index_++];
    y ^= (y >> 11);
    y ^= (y << 7) & 0x9d2c5680U;
    y ^= (y << 15) & 0xefc60000U;
    y ^= (y >> 18);
    return y;
}

double Random::random() {
    const std::uint32_t a = genrand() >> 5;
    const std::uint32_t b = genrand() >> 6;
    return (a * 67108864.0 + b) * (1.0 / 9007199254740992.0);
}

// (Python's: a pair of normals from two uniforms, the second kept for the next call)
double Random::gauss(double mu, double sigma) {
    double z;
    if (gaussNext_) {
        z = *gaussNext_;
        gaussNext_.reset();
    } else {
        const double x2pi = random() * kTwoPi;
        const double g2rad = std::sqrt(-2.0 * std::log(1.0 - random()));
        z = std::cos(x2pi) * g2rad;
        gaussNext_ = std::sin(x2pi) * g2rad;
    }
    return mu + z * sigma;
}

}  // namespace awo::py
