#pragma once

// The numpy operations MusicCadence.py's analysis uses, computed in the same order as numpy 1.26 does them (with its
// OpenBLAS on this kind of CPU), so the C++ app's tempo analysis comes out the same as the Python app's, not just close:
//   sums and means: numpy's pairwise summation (blocks of 8, from 0), in the array's own precision
//   np.convolve(a, v, "same"): numpy's correlate, its middle by small_correlate for kernels of up to 11 (plain sums
//     of products) and everything else by OpenBLAS's Skylake-X ddot (AVX-512, fused multiply-adds)
//   np.interp, np.median, np.percentile (linear), np.hanning, np.fft.rfft / irfft (numpy's pocketfft)
// (Checked against numpy itself in tests/tst_music.cpp and the golden analyses.)

#include <cstddef>
#include <vector>

namespace awo::music::np {

using Array = std::vector<double>;
using FloatArray = std::vector<float>;

double sum(const double* a, std::size_t n);
float sum(const float* a, std::size_t n);
inline double sum(const Array& a) {
    return sum(a.data(), a.size());
}
double mean(const Array& a);
double stddev(const Array& a);
// (a copy is sorted: a's own order is kept)
double median(const double* a, std::size_t n);
inline double median(const Array& a) {
    return median(a.data(), a.size());
}
double percentile(const Array& a, double q);
// np.interp(x, xp, fp) for one x
double interp(double x, const Array& xp, const Array& fp);
Array hanning(std::size_t m);
// np.convolve(a, v, mode="same")
Array convolveSame(const Array& a, const Array& v);
// numpy's dot of two double rows (OpenBLAS ddot, see above)
double dot(const double* x, const double* y, std::size_t n);

// np.fft.rfft(a, n): (re, im) pairs, n/2 + 1 of them
Array rfft(const double* a, std::size_t length, std::size_t n);
// np.fft.irfft(c): c is (re, im) pairs, m of them; n = 2*(m - 1)
Array irfft(const Array& c);

// numpy's round half to even (Python's round on a numpy float)
double roundHalfEven(double value);
// np.argmax: the first of the largest
std::size_t argmax(const double* a, std::size_t n);

}  // namespace awo::music::np
