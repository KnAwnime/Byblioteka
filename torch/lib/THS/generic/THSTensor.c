#ifndef THS_GENERIC_FILE
#define THS_GENERIC_FILE "generic/THSTensor.c"
#else

/**** access methods ****/
int THSTensor_(nDimension)(const THSTensor *self)
{
  return self->nDimension;
}

long THSTensor_(size)(const THSTensor *self, int dim)
{
  THArgCheck((dim >= 0) && (dim < self->nDimension), 1, "dimension %d out of range of %dD tensor",
      dim+1, THSTensor_(nDimension)(self));
  return self->size[dim];
}

long THSTensor_(nnz)(const THSTensor *self) {
  return self->nnz;
}

THLongStorage *THSTensor_(newSizeOf)(THSTensor *self)
{
  THLongStorage *size = THLongStorage_newWithSize(self->nDimension);
  THLongStorage_rawCopy(size, self->size);
  return size;
}

/*** TODO: watch out for memory leaks ***/
THLongTensor *THSTensor_(indicies)(const THSTensor *self) {
  return THLongTensor_newNarrow(self->indicies, 1, 0, self->nnz);
}

THTensor *THSTensor_(values)(const THSTensor *self) {
  return THTensor_(newNarrow)(self->values, 0, 0, self->nnz);
}

THSTensor *THSTensor_(set)(THSTensor *self, THLongTensor *indicies, THTensor *values) {
  THArgCheck(THLongTensor_nDimension(indicies) == 2, 1,
      "indicies must be nDim x nnz");
  THArgCheck(THTensor_(nDimension)(values) == 1, 2, "values must nnz vector");
  THArgCheck(THLongTensor_size(indicies, 1) == THTensor_(size)(values, 0), 1,
      "indicies and values must have same nnz");
  THFree(self->indicies);
  THFree(self->values);
  self->indicies = THLongTensor_newClone(indicies);
  self->values = THTensor_(newClone)(values);
  self->nnz = THTensor_(size)(values, 0);
}

int THSTensor_(isContiguous)(const THSTensor *self) {
  return self->contiguous;
}


void THSTensor_(reorder)(THSTensor *self) {
  /* TODO: We do an insertion sort here, should change to quicksort or shellsort
   */
  if (self->nnz < 2) return;
  long d, i, j, p, cmp, ndim, indskip, tmplong;
  real tmpreal;
  THLongTensor *indicies_ = self->indicies;
  THTensor *values_ = self->values;
  long *indicies = THLongTensor_data(indicies_);
  real *values = THTensor_(data)(values_);
  indskip = THLongTensor_size(indicies_, 1); // To index indicies
  ndim = THSTensor_(nDimension)(self);

#define IND(i, d) indicies[d * indskip + i]
  for (i = 1; i < self->nnz; i++) {
    for (j = i-1; j >= 0; j--) {
      cmp = 0;
      for (d = 0; d < ndim; d++) {
        if (IND(j+1, d) < IND(j, d))
          cmp = 1;
        if (IND(j+1, d) != IND(j, d)) break;
      }
      if (cmp) {
        tmpreal = values[j+1]; values[j+1] = values[j]; values[j] = tmpreal;
        for (d = 0; d < ndim; d++) {
          tmplong = IND(j+1, d); IND(j+1, d) = IND(j, d); IND(j, d) = tmplong;
        }
      } else break;
    }
  }

  i = 0;
  for (j = 1; j < self->nnz; j++) {
    cmp = 1;
    for (d = 0; d < ndim; d++)
      if (IND(i, d) != IND(j, d)) {
        cmp = 0;
        break;
      }
    if (cmp) values[i] += values[j];
    else {
      values[++i] = values[j];
      for (d = 0; d < ndim; d++) IND(i, d) = IND(j, d);
    }
  }
  self->nnz = i + 1;
#undef IND
}

void THSTensor_(contiguous)(THSTensor *self) {
  if (self->contiguous) return;
  THSTensor_(reorder)(self);
  self->contiguous = 1;
}

/**** creation methods ****/

static void THSTensor_(rawInit)(THSTensor *self)
{
  self->size = NULL;
  self->indicies = NULL;
  self->values = NULL;
  self->nDimension = 0;
  self->contiguous = 0;
  self->nnz = 0;
  // self->flag = TH_TENSOR_REFCOUNTED;
}

static void THSTensor_(rawResize)(THSTensor *self, int nDim, long *size) {
  // Only resize valid sizes into tensor.
  self->size = THRealloc(self->size, sizeof(long)*nDim);

  long d, nDim_ = 0;
  for (d = 0; d < nDim; d++)
    if (size[d] > 0)
      self->size[nDim_++] = size[d];
  self->nDimension = nDim_;
}


/* Empty init */
THSTensor *THSTensor_(new)(void)
{
  THSTensor *self = THAlloc(sizeof(THSTensor));
  THSTensor_(rawInit)(self);
  return self;
}

/* Pointer-copy init */
THSTensor *THSTensor_(newWithTensor)(THLongTensor *indicies, THTensor *values)
{
  return THSTensor_(newWithTensorAndSize)(indicies, values, NULL);
}

THSTensor *THSTensor_(newWithTensorAndSize)(THLongTensor *indicies, THTensor *values, THLongTensor *sizes)
{  // If sizes are not given, it is inferred as max index of each dim.
  long nDim;
  THLongTensor *ignore;

  THSTensor *self = THAlloc(sizeof(THSTensor));
  THSTensor_(rawInit)(self);
  THSTensor_(set)(self, indicies, values);

  nDim = THLongTensor_size(indicies, 0);
  if (!sizes) {
    ignore = THLongTensor_new();
    sizes = THLongTensor_new();
    THLongTensor_max(sizes, ignore, indicies, 0);
    THFree(sizes);
    THFree(ignore);
  }

  THSTensor_(rawResize)(self, nDim, THLongTensor_data(sizes));
  return self;
}

THSTensor *THSTensor_(newWithSize)(THLongStorage *size)
{
  THSTensor *self = THAlloc(sizeof(THSTensor));
  THSTensor_(rawInit)(self);
  THSTensor_(rawResize)(self, size->size, size->data);
}

THSTensor *THSTensor_(newWithSize1d)(long size0)
{
  return THSTensor_(newWithSize4d)(size0, -1, -1, -1);
}

THSTensor *THSTensor_(newWithSize2d)(long size0, long size1)
{
  return THSTensor_(newWithSize4d)(size0, size1, -1, -1);
}

THSTensor *THSTensor_(newWithSize3d)(long size0, long size1, long size2)
{
  return THSTensor_(newWithSize4d)(size0, size1, size2, -1);
}

THSTensor *THSTensor_(newWithSize4d)(long size0, long size1, long size2, long size3)
{
  long size[4] = {size0, size1, size2, size3};

  THSTensor *self = THAlloc(sizeof(THSTensor));
  THSTensor_(rawInit)(self);
  THSTensor_(rawResize)(self, 4, size);

  return self;
}

THTensor *THSTensor_(toDense)(THSTensor *self) {
  int d, k, index;
  long nnz, ndim, indskip;
  long *sizes;
  THLongStorage *storage;

  THTensor *other_, *values_;
  real *other, *values;
  THLongTensor *indicies_;
  long *indicies;

  // set up the new tensor
  storage = THSTensor_(newSizeOf)(self);
  other_ = THTensor_(newWithSize)(storage, NULL);
  THTensor_(zero)(other_);
  other = THTensor_(data)(other_);

  // Some necessary dimensions and sizes
  nnz = THSTensor_(nnz)(self);
  ndim = THSTensor_(nDimension)(self);
  sizes = storage->data;

  // These should be contiguous...
  values_ = THSTensor_(values)(self);
  indicies_ = self->indicies;
  values = THTensor_(data)(values_);
  indicies = THLongTensor_data(indicies_);
  indskip = THLongTensor_size(indicies_, 1); // To index indicies

  #pragma omp parallel for private(k, index)
  for (k = 0; k < nnz; k++) {
    for (d = 0, index = 0; d < ndim; d++)
      index = sizes[d] * index + indicies[d * indskip + k];
    other[index] += values[k];
  }

  THFree(values_);
  THLongStorage_free(storage);
  return other_;
}

void THSTensor_(free)(THSTensor *self)
{
  if(!self)
    return;

  THFree(self->indicies);
  THFree(self->values);
  THFree(self);
}

#endif
