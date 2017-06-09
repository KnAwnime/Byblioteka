#pragma once

#include <atomic>

#include "TensorLib/Scalar.h"
#include "TensorLib/Type.h"
#include <iostream>
namespace tlib {

class Type;
struct TensorImpl {
  TensorImpl(Type * type)
  : type_(type), refcount(1) {}
  Type & type() const {
    return *type_;
  }

  virtual const char * toString() const = 0;
  virtual IntList sizes() = 0;
  virtual IntList strides() = 0;
  void retain() {
    ++refcount;
  }
  virtual void release() {
    if(--refcount == 0) {
      delete this;
    }
  }
  virtual ~TensorImpl() {}

  friend class Type;
private:
  std::atomic<int> refcount;
  Type * type_;
};

}
