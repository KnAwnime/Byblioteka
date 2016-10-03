#ifndef THS_GENERIC_FILE
#define THS_GENERIC_FILE "generic/THSTensor.h"
#else

typedef struct THSTensor
{  // Stored in COO format, indicies + values
    long *size;
    long nnz;
    int nDimension;

    THLongTensor *indicies;  // 2-D tensor of nDim x nnz of indicies
    THTensor *values;
    // Math operations can only be performed on ordered sparse tensors
    int contiguous;
    /*
    long storageOffset;
    int refcount;

    char flag;
    */

} THSTensor;

/**** access methods ****/
TH_API int THSTensor_(nDimension)(const THSTensor *self);
TH_API long THSTensor_(size)(const THSTensor *self, int dim);
TH_API long THSTensor_(nnz)(const THSTensor *self);
TH_API THLongStorage *THSTensor_(newSizeOf)(THSTensor *self);
TH_API THLongTensor *THSTensor_(indicies)(const THSTensor *self);
TH_API THTensor *THSTensor_(values)(const THSTensor *self);
TH_API THSTensor *THSTensor_(set)(THSTensor *self, THLongTensor *indicies, THTensor *values);

TH_API int THSTensor_(isContiguous)(const THSTensor *self);
TH_API void THSTensor_(contiguous)(THSTensor *self);


/**** creation methods ****/
TH_API THSTensor *THSTensor_(new)(void);
TH_API THSTensor *THSTensor_(newWithTensor)(THLongTensor *indicies, THTensor *values);
TH_API THSTensor *THSTensor_(newWithTensorAndSize)(THLongTensor *indicies, THTensor *values, THLongTensor *sizes);

TH_API THSTensor *THSTensor_(newWithSize)(THLongStorage *size_);
TH_API THSTensor *THSTensor_(newWithSize1d)(long size0_);
TH_API THSTensor *THSTensor_(newWithSize2d)(long size0_, long size1_);
TH_API THSTensor *THSTensor_(newWithSize3d)(long size0_, long size1_, long size2_);
TH_API THSTensor *THSTensor_(newWithSize4d)(long size0_, long size1_, long size2_, long size3_);

TH_API THTensor *THSTensor_(toDense)(THSTensor *self);

TH_API void THSTensor_(free)(THSTensor *self);

/* TODO (check function signatures too, might be wrong)
TH_API void THSTensor_(freeCopyTo)(THSTensor *self, THSTensor *dst);

TH_API void THSTensor_(narrow)(THSTensor *self, THSTensor *src, int dimension_, long firstIndex_, long size_);
TH_API void THSTensor_(select)(THSTensor *self, THSTensor *src, int dimension_, long sliceIndex_);
TH_API void THSTensor_(transpose)(THSTensor *self, THSTensor *src, int dimension1_, int dimension2_);
*/

#endif
