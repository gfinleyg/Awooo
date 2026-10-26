#ifndef AWO_POCKETFFT_H
#define AWO_POCKETFFT_H

/* pocketfft (numpy 1.26.4's), real transforms only: numpy's np.fft.rfft / irfft of one row, before numpy's own
   padding/truncating to n and its norm (forward: fct 1, backward: fct 1/n). 0 on success */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int awo_rfft_forward(const double *in, size_t n, double *out, double fct);
int awo_rfft_backward(const double *in, size_t n, double *out, double fct);

#ifdef __cplusplus
}
#endif

#endif
