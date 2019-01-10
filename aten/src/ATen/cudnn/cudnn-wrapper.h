#pragma once

#include <cudnn.h>

#define STRINGIFY(x) #x
#define STRING(x) STRINGIFY(x)

#if CUDNN_MAJOR < 6
#pragma message ("CuDNN v" STRING(CUDNN_MAJOR) " found, but need at least CuDNN v6. You can get the latest version of CuDNN from https://developer.nvidia.com/cudnn or disable CuDNN with NO_CUDNN=1")
#error "CuDNN version not supported"
#endif

#undef STRINGIFY
#undef STRING

