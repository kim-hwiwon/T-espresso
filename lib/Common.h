#pragma once

#include <stdint.h>

// Reference type definition
typedef struct {
  uint8_t *allocs;
  uint8_t *commits;
  uint8_t *records;
  uint32_t slot_size;
} traceinfo_t;

typedef struct record_t {
  int64_t desc;
  int64_t addr;
  int64_t cta;
} record_t;

// Size of a record in bytes, contents of a record:
// 32 bit meta info, 32bit size, 64 bit address, 64 bit cta id
#define RECORD_SIZE (24)
// 6M buffer, devided into 4 parallel slots.
// Buffers: SLOTS_NUM * SLOTS_SIZE * RECORD_SIZE
// Absolute minimum is the warp size, all threads in a warp must collectively
// wait or be able to write a record
//#define SLOTS_SIZE (256 * 1024)
#define SLOTS_SIZE (64)
// Number of slots must be power of two!
#define SLOTS_NUM (4)

#define CACHELINE (64)

#ifdef INCLUDE_LLVM_MEMTRACE_STUFF

// functions need to be static because we link it into both host and device
// instrumentation

#include "llvm/IR/DerivedTypes.h"
#include "llvm/ADT/Twine.h"

static
llvm::StructType* getTraceInfoType(llvm::LLVMContext &ctx) {
  using llvm::Type;
  using llvm::StructType;

  Type *fields[] = {
    Type::getInt8PtrTy(ctx),
    Type::getInt8PtrTy(ctx),
    Type::getInt8PtrTy(ctx),
    Type::getInt32Ty(ctx),
  };

  return StructType::create(fields, "traceinfo_t");
}

static
std::string getSymbolNameForKernel(const llvm::Twine &kernelName) {
  return ("__" + kernelName + "_trace").str();
}

#endif
