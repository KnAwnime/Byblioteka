#ifndef THC_GENERIC_FILE
#define THC_GENERIC_FILE "generic/THCTensorMasked.h"
#else

THC_API void THCTensor_(maskedFill)(THCState *state,
                                    THCTensor *tensor,
                                    THCudaByteTensor *mask,
                                    scalar_t value);

// FIXME: remove now that we have THCudaByteTensor?
THC_API void THCTensor_(maskedFillByte)(THCState *state,
                                        THCTensor *tensor,
                                        THTensor *mask,
                                        scalar_t value);

THC_API void THCTensor_(maskedCopy)(THCState *state,
                                    THCTensor *tensor,
                                    THCudaByteTensor *mask,
                                    THCTensor *src);

// FIXME: remove now that we have THCudaByteTensor?
THC_API void THCTensor_(maskedCopyByte)(THCState *state,
                                        THCTensor *tensor,
                                        THTensor *mask,
                                        THCTensor *src);

THC_API void THCTensor_(maskedSelect)(THCState *state,
                                      THCTensor *tensor,
                                      THCTensor *src,
                                      THCudaByteTensor *mask);

// FIXME: remove now that we have THCudaByteTensor?
THC_API void THCTensor_(maskedSelectByte)(THCState *state,
                                          THCTensor *tensor,
                                          THCTensor *src,
                                          THTensor *mask);

#endif
