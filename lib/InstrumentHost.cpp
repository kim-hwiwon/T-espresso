/*******************************************************************************
 * This host instrumentation pass inserts calls into the host support library.
 * The host support library is used to set up queues for traces that are sinked
 * into a thread that writes them into a file.
 * A kernel launch is split into two major parts:
 * 1. cudaConfigureCall()
 * 2. <wrapper>() -> cudaLaunch()
 * The function cudaConfigurCall sets up the execution grid and stream to
 * execute in and the wrapper function sets up kernel arguments and launches
 * the kernel.
 * Instrumentation requires the stream, set in cudaConfigureCall, as well as the
 * kernel name, implicitly "set" by the wrapper function.
 * This pass defines the location of a kernel launch as the call to
 * cudaConfigureCall, which the module is searched for.
 *
 * Finding the kernel name boils down to following the execution path assuming
 * no errors occur during config and argument setup until we find:
 * 1. a call cudaLaunch and return the name of the first operand, OR
 * 2. a call to something other than cudaSetupArguent and return its name
 *
 */

#define INCLUDE_LLVM_MEMTRACE_STUFF
#include "Common.h"
#undef INCLUDE_LLVM_MEMTRACE_STUFF

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Operator.h"

#include "llvm/Pass.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "memtrace-host"

using namespace llvm;

struct KernelCallInfo {
  std::string kernelName;
    
};

struct InstrumentHost : public ModulePass {
    static char ID;
    InstrumentHost() : ModulePass(ID) {}

    Type* TraceInfoType = nullptr;

    Constant *TraceFillInfo = nullptr;
    Constant *TraceCopyToSymbol = nullptr;
    Constant *TraceTouch = nullptr;
    Constant *TraceStart = nullptr;
    Constant *TraceStop = nullptr;

    /** Sets up pointers to (and inserts prototypes of) the utility functions
     * from the host-support library.
     * We're pretending all pointer types are identical, linker does not
     * complain in tests.
     *
     * Reference:
     * void __trace_fill_info(const void *info, cudaStream_t stream)
     * void __trace_copy_to_symbol(cudaStream_t stream, const char* symbol, const void *info)
     * void __trace_start(cudaStream_t stream, const char *kernel_name);
     * void __trace_stop(cudaStream_t stream);
     */
    void findOrInsertFunctions(Module &M) {
      LLVMContext &ctx = M.getContext();
      Type* cuStreamTy = Type::getInt8PtrTy(ctx);
      //Type* cuErrTy = Type::getInt32Ty(ctx);
      Type* voidPtrTy = Type::getInt8PtrTy(ctx);
      Type* stringTy = Type::getInt8PtrTy(ctx);
      Type* voidTy = Type::getVoidTy(ctx);

      TraceFillInfo = M.getOrInsertFunction("__trace_fill_info",
          voidTy, voidPtrTy, cuStreamTy);
      TraceCopyToSymbol = M.getOrInsertFunction("__trace_copy_to_symbol",
          voidTy, cuStreamTy, stringTy, voidPtrTy);
      TraceStart = M.getOrInsertFunction("__trace_start",
          voidTy, cuStreamTy, stringTy);
      TraceTouch = M.getOrInsertFunction("__trace_touch",
          voidTy, cuStreamTy);
      TraceStop = M.getOrInsertFunction("__trace_stop",
          voidTy, cuStreamTy);
    }

    /** Find the kernel launch or wrapper function belonging to a
     * cudaConfigureCall. Must handle inlined and non-inlined cases.
     */
    CallInst* searchKernelLaunchFor(Instruction *inst) {
      while (inst != nullptr) {
        // on conditional branches, assume everything went okay
        if (auto *br = dyn_cast<BranchInst>(inst)) {
          if (br->isUnconditional()) {
            inst = inst->getNextNode();
            continue;
          } else {
            auto *BB = br->getSuccessor(0);
            StringRef name = BB->getName();
            if (name.startswith("kcall.configok") || name.startswith("setup.next")) {
              inst = BB->getFirstNonPHI();
              continue;
            }
            BB = br->getSuccessor(1);
            name = BB->getName();
            if (name.startswith("kcall.configok") || name.startswith("setup.next")) {
              inst = BB->getFirstNonPHI();
              continue;
            }
            // unrecognized branch
            return nullptr;
          }
        }

        StringRef callee = "";
        if (auto *call = dyn_cast<CallInst>(inst)) {
          callee = call->getCalledFunction()->getName();
          // blacklist helper functions
          if (callee == "cudaSetupArgument" || callee == "cudaConfigureCall"
              || callee.startswith("llvm.lifetime")) {
            inst = inst->getNextNode();
            continue;
          }
          // this either cudaLaunch or a wrapper, break and return
          break;
        }

        // uninteresting, get next
        inst = inst->getNextNode();
        continue;
      }
      return dyn_cast_or_null<CallInst>(inst);
    }

    /** Given a "kernel launch" differentiate whether it is a cudaLaunch or
     * wrapper function call and return the appropriate name.
     */
    StringRef getKernelNameOfLaunch(CallInst *launch) {
      if (launch == nullptr) {
        return "anonymous";
      }

      errs() << "launch: " << *launch << "\n";
      StringRef calledFunctionName = launch->getCalledFunction()->getName();

      // for kernel launch, return name of first operand
      if (calledFunctionName == "cudaLaunch") {
        auto *op = launch->getArgOperand(0);
        while (auto *cast = dyn_cast<BitCastOperator>(op)) {
          op = cast->getOperand(0);
        }
        return op->getName();
      }

      // otherwise return name of called function itself
      return calledFunctionName;
    }

    /** Updates kernel calls to set up tracing infrastructure on host and device
     * before starting the kernel and tearing everything down afterwards.
     */
    void patchKernelCall(CallInst *configureCall) {
      auto *launch = searchKernelLaunchFor(configureCall);
      assert(launch != nullptr && "did not find kernel launch");

      StringRef kernelName = getKernelNameOfLaunch(launch);
      assert(configureCall->getNumArgOperands() == 6);
      auto *stream = configureCall->getArgOperand(5);
      std::string kernelSymbolName = getSymbolNameForKernel(kernelName);

      errs() << "patching kernel launch: " << *configureCall << "\n";
      errs() << "  name: "   << kernelName << "\n";
      errs() << "  stream: " << *stream << "\n";
      errs() << "  symbolName: " << kernelSymbolName << "\n";

      // insert preparational steps directly after cudaConfigureCall
      // 0. touch consumer to create new one if necessary
      // 1. start/prepare trace consumer for stream
      // 2. get trace consumer info
      // 3. copy trace consumer info to device

      IRBuilder<> IRB(configureCall->getNextNode());

      Type* i8Ty = IRB.getInt8Ty();

      Value* kernelNameVal = IRB.CreateGlobalStringPtr(kernelName);
      Value* kernelInfoSymbolVal = IRB.CreateGlobalStringPtr(kernelSymbolName);
      Value* streamPtr = IRB.CreatePointerCast(stream, IRB.getInt8PtrTy());

      IRB.CreateCall(TraceTouch, {streamPtr});
      IRB.CreateCall(TraceStart, {streamPtr, kernelNameVal});

      const DataLayout &DL = configureCall->getParent()->getParent()->getParent()->getDataLayout();
      size_t bufSize = DL.getTypeStoreSize(TraceInfoType);

      Value* infoBuf = IRB.CreateAlloca(i8Ty, IRB.getInt32(bufSize));
      IRB.CreateCall(TraceFillInfo, {infoBuf, streamPtr});
      IRB.CreateCall(TraceCopyToSymbol, {streamPtr, kernelInfoSymbolVal, infoBuf});


      // insert finishing steps after kernel launch was issued
      // 1. stop trace consumer
      IRB.SetInsertPoint(launch->getNextNode());
      IRB.CreateCall(TraceStop, {streamPtr});

    }

    bool runOnModule(Module &M) override {
      if (M.getTargetTriple().find("nvptx") != std::string::npos) {
        return false;
      }

      Function* cudaConfigureCall = M.getFunction("cudaConfigureCall");
      if (cudaConfigureCall == nullptr) {
        errs() << "no configure calls\n";
        return false;
      }

      TraceInfoType = getTraceInfoType(M.getContext());
      findOrInsertFunctions(M);

      for (auto* user : cudaConfigureCall->users()) {
        if (auto *call = dyn_cast<CallInst>(user)) {
          patchKernelCall(call);
        }

        //if (auto *call = dyn_cast<InvokeInst>(user)) {
        //  patchKernelCall(call);
        //}
      }

      return true;
    }
};

char InstrumentHost::ID = 0;

static RegisterPass<InstrumentHost> X("memtrace-host", "inserts host-side instrumentation for mem-traces", false, false);
