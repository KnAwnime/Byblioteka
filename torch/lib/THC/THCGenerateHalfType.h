#ifndef THC_GENERIC_FILE
#error "You must define THC_GENERIC_FILE before including THGenerateHalfType.h"
#endif

#include "THCHalf.h"

#ifdef CUDA_HALF_TENSOR

#define real half
#define accreal float
#define Real Half
#define CReal CudaHalf
#define THC_REAL_IS_HALF
#line 1 THC_GENERIC_FILE
#include THC_GENERIC_FILE
#undef real
#undef accreal
#undef Real
#undef CReal
#undef THC_REAL_IS_HALF

#endif // CUDA_HALF_TENSOR

#ifndef THCGenerateAllTypes
#undef THC_GENERIC_FILE
#endif
