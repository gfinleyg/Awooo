#include "NumpyLike.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <numbers>

#include "pocketfft.h"

namespace awo::music::np {

namespace {

// numpy's @TYPE@_pairwise_sum (loops_utils.h.src): T the array's own type, accumulated in it
template <typename T>
T pairwiseSum(const T* a, std::size_t n) {
    if (n < 8) {
        T res = 0;
        for (std::size_t i = 0; i < n; ++i) {
            res += a[i];
        }
        return res;
    }
    if (n <= 128) {
        T r[8];
        for (int j = 0; j < 8; ++j) {
            r[j] = a[j];
        }
        std::size_t i = 8;
        for (; i < n - (n % 8); i += 8) {
            for (int j = 0; j < 8; ++j) {
                r[j] += a[i + j];
            }
        }
        T res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
        for (; i < n; ++i) {
            res += a[i];
        }
        return res;
    }
    std::size_t n2 = n / 2;
    n2 -= n2 % 8;
    return pairwiseSum(a, n2) + pairwiseSum(a + n2, n - n2);
}

// numpy's small_correlate: a kernel of up to 11, each output a plain sum of products from 0
bool smallCorrelate(const double* d, std::size_t nd, const double* k, std::size_t nk, double* out) {
    if (nk > 11) {
        return false;
    }
    for (std::size_t i = 0; i < nd; ++i) {
        double s = 0;
        for (std::size_t j = 0; j < nk; ++j) {
            s += d[i + j] * k[j];
        }
        out[i] = s;
    }
    return true;
}

}  // namespace

double sum(const double* a, std::size_t n) {
    return pairwiseSum(a, n);
}

float sum(const float* a, std::size_t n) {
    return pairwiseSum(a, n);
}

double mean(const Array& a) {
    return sum(a) / static_cast<double>(a.size());
}

// numpy's _var: the mean, the squared differences summed, over n, its root
double stddev(const Array& a) {
    const double m = mean(a);
    Array squares(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double x = a[i] - m;
        squares[i] = x * x;
    }
    return std::sqrt(sum(squares) / static_cast<double>(a.size()));
}

double median(const double* a, std::size_t n) {
    std::vector<double> sorted(a, a + n);
    std::sort(sorted.begin(), sorted.end());
    if (n % 2 == 1) {
        return sorted[n / 2];
    }
    // (np.mean of the middle two: their sum, halved)
    return (sorted[n / 2 - 1] + sorted[n / 2]) / 2;
}

// numpy 1.26's quantile, method "linear": (n - 1) q, and its _lerp
double percentile(const Array& a, double q) {
    std::vector<double> sorted(a);
    std::sort(sorted.begin(), sorted.end());
    const std::size_t n = sorted.size();
    const double virtualIndex = static_cast<double>(n - 1) * (q / 100);
    if (virtualIndex >= static_cast<double>(n - 1)) {
        return sorted[n - 1];
    }
    if (virtualIndex < 0) {
        return sorted[0];
    }
    const double previous = std::floor(virtualIndex);
    const double gamma = virtualIndex - previous;
    const double x = sorted[static_cast<std::size_t>(previous)];
    const double y = sorted[static_cast<std::size_t>(previous) + 1];
    const double diff = y - x;
    return gamma >= 0.5 ? y - diff * (1 - gamma) : x + diff * gamma;
}

// numpy's arr_interp for one point (binary_search_with_guess's answer is the same as a plain search's)
double interp(double x, const Array& xp, const Array& fp) {
    const std::size_t n = xp.size();
    if (x < xp[0]) {
        return fp[0];
    }
    if (x > xp[n - 1]) {
        return fp[n - 1];
    }
    // the last j with xp[j] <= x
    const std::size_t j = static_cast<std::size_t>(std::upper_bound(xp.begin(), xp.end(), x) - xp.begin()) - 1;
    if (j == n - 1 || xp[j] == x) {
        return fp[j];
    }
    const double slope = (fp[j + 1] - fp[j]) / (xp[j + 1] - xp[j]);
    double result = slope * (x - xp[j]) + fp[j];
    if (std::isnan(result)) {
        result = slope * (x - xp[j + 1]) + fp[j + 1];
        if (std::isnan(result) && fp[j] == fp[j + 1]) {
            result = fp[j];
        }
    }
    return result;
}

Array hanning(std::size_t m) {
    if (m < 1) {
        return {};
    }
    if (m == 1) {
        return {1.0};
    }
    Array window(m);
    for (std::size_t i = 0; i < m; ++i) {
        const double n = 1.0 - static_cast<double>(m) + 2.0 * static_cast<double>(i);
        window[i] = 0.5 + 0.5 * std::cos(std::numbers::pi * n / static_cast<double>(m - 1));
    }
    return window;
}

// OpenBLAS 0.3.23's ddot for Skylake-X (ddot.c + dot_microk_skylakex-2.c, GCC contracting its multiply-adds):
// whole 16s by the kernel (32 at a time on four 8 wide accumulators, then 16 at a time on four 4 wide ones, folded
// together), the rest one by one
double dot(const double* x, const double* y, std::size_t n) {
    const std::size_t n1 = n & ~std::size_t(15);
    double result = 0.0;
    if (n1) {
        std::size_t i = 0;
        const std::size_t n32 = n1 & ~std::size_t(31);
        double acc4[4][4] = {};
        if (n32) {
            double acc8[4][8] = {};
            for (; i < n32; i += 32) {
                for (int a = 0; a < 4; ++a) {
                    for (int l = 0; l < 8; ++l) {
                        const std::size_t k = i + a * 8 + l;
                        acc8[a][l] = std::fma(x[k], y[k], acc8[a][l]);
                    }
                }
            }
            for (int a = 0; a < 4; ++a) {
                for (int l = 0; l < 4; ++l) {
                    acc4[a][l] = acc8[a][l] + acc8[a][l + 4];
                }
            }
        }
        for (; i < n1; i += 16) {
            for (int a = 0; a < 4; ++a) {
                for (int l = 0; l < 4; ++l) {
                    const std::size_t k = i + a * 4 + l;
                    acc4[a][l] = std::fma(x[k], y[k], acc4[a][l]);
                }
            }
        }
        double s[4];
        for (int l = 0; l < 4; ++l) {
            s[l] = ((acc4[0][l] + acc4[1][l]) + acc4[2][l]) + acc4[3][l];
        }
        result = (s[0] + s[2]) + (s[1] + s[3]);
    }
    for (std::size_t k = n1; k < n; ++k) {
        result = std::fma(y[k], x[k], result);
    }
    // (numpy's DOUBLE_dot: sum = 0. + the BLAS dot)
    return 0.0 + result;
}

// np.convolve: the longer one first, the shorter reversed, through numpy's _pyarray_correlate (mode 1, "same")
Array convolveSame(const Array& aIn, const Array& vIn) {
    const Array& a = vIn.size() > aIn.size() ? vIn : aIn;
    const Array& v = vIn.size() > aIn.size() ? aIn : vIn;
    const std::size_t n1 = a.size();
    std::size_t n = v.size();
    Array k(v.rbegin(), v.rend());
    Array out(n1);
    const std::size_t nLeft = n / 2;
    const std::size_t nRight = n - nLeft - 1;
    const double* ip1 = a.data();
    const double* ip2 = k.data() + nLeft;
    double* op = out.data();
    n = n - nLeft;
    for (std::size_t i = 0; i < nLeft; ++i) {
        *op++ = dot(ip1, ip2, n);
        ++n;
        --ip2;
    }
    if (smallCorrelate(ip1, n1 - n + 1, ip2, n, op)) {
        ip1 += n1 - n + 1;
        op += n1 - n + 1;
    } else {
        for (std::size_t i = 0; i < n1 - n + 1; ++i) {
            *op++ = dot(ip1, ip2, n);
            ++ip1;
        }
    }
    for (std::size_t i = 0; i < nRight; ++i) {
        --n;
        *op++ = dot(ip1, ip2, n);
        ++ip1;
    }
    return out;
}

Array rfft(const double* a, std::size_t length, std::size_t n) {
    // (numpy pads with zeros, or cuts, to n)
    Array in(n, 0.0);
    std::copy(a, a + std::min(length, n), in.begin());
    Array out((n / 2 + 1) * 2);
    if (awo_rfft_forward(in.data(), n, out.data(), 1.0) != 0) {
        throw std::bad_alloc();
    }
    return out;
}

Array irfft(const Array& c) {
    const std::size_t m = c.size() / 2;
    const std::size_t n = 2 * (m - 1);
    Array out(n);
    if (awo_rfft_backward(c.data(), n, out.data(), 1.0 / static_cast<double>(n)) != 0) {
        throw std::bad_alloc();
    }
    return out;
}

double roundHalfEven(double value) {
    return std::nearbyint(value);
}

std::size_t argmax(const double* a, std::size_t n) {
    std::size_t best = 0;
    for (std::size_t i = 1; i < n; ++i) {
        if (a[i] > a[best]) {
            best = i;
        }
    }
    return best;
}

}  // namespace awo::music::np
