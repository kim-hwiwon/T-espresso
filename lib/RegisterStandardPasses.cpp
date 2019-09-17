#include "Passes.h"

#include "llvm/PassRegistry.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"

#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/AST.h"
#include "clang/Frontend/CompilerInstance.h"
#include "llvm/Support/raw_ostream.h"

#include <sstream>

#include "llvm/Support/MemoryBuffer.h"


using namespace llvm;

static InstrumentPassArg pass_args;

namespace clang {
  
  static void registerStandardPasses(const PassManagerBuilder &, legacy::PassManagerBase &);
  

  class PluginEntry : public PluginASTAction {
    
  protected:


    // Register pass according to args
    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                   StringRef strref) override {

      static RegisterStandardPasses RegisterTracePass(
        PassManagerBuilder::EP_ModuleOptimizerEarly, registerStandardPasses);
      static RegisterStandardPasses RegisterTracePass0(
        PassManagerBuilder::EP_EnabledOnOptLevel0, registerStandardPasses);

      return make_unique<ASTConsumer>();
    
    }

    
    // Get args of the plugin
    bool ParseArgs(const CompilerInstance &CI,
                   const std::vector<std::string>& args) override {
      
      pass_args.trace_thread = true;
      pass_args.trace_mem = true;
      
      
      std::stringstream argslist;
      std::copy(args.begin(), args.end(), std::ostream_iterator<std::string>(argslist, ","));

      
      std::string optstr;
      while (getline(argslist, optstr, ',')) {
        size_t equal_pos = optstr.find('=');
        std::string optname = optstr.substr(0, equal_pos);
        std::stringstream optarglist;
        if (equal_pos != std::string::npos) {
          optarglist = std::stringstream(optstr.substr(equal_pos+1));
        }
        
        
        //printf("%s, %s\n", optname.c_str(), optarg.c_str());
        
        if (optname == "thread-only") {
          //errs() << "cuprof: Tracing thread scheduling - Disabled\n";
          pass_args.trace_thread = true;
          pass_args.trace_mem = false;

          
        } else if (optname == "mem-only") {
          //errs() << "cuprof: Tracing memory access - Disabled\n";
          pass_args.trace_thread = false;
          pass_args.trace_mem = true;

          
        } else if (optname == "no-trace") {
          //errs() << "cuprof: Tracing - Disabled\n";
          return false;

          
        } else if (optname == "kernel") {
          std::string optarg;
          while (getline(optarglist, optarg, ' ')) {
            
            pass_args.kernel.push_back(optarg);
          }

          
        } else if (optname == "sm") {
          std::string optarg;
          while (getline(optarglist, optarg, ' ')) {
            
            if (std::all_of(optarg.begin(), optarg.end(), ::isdigit)) {
              uint8_t smid = (uint8_t)(std::stoi(optarg) & 0xFF);
              pass_args.sm.push_back(smid);
            }
          }

          
        } else if (optname == "cta") {
          std::string optarg;
          while (getline(optarglist, optarg, ' ')) {
            
            uint32_t ctaid[3] = {0, 0, 0};
            std::stringstream ctaidlist(optarg);
            std::string ctaid_cur;
            
            for (int i = 0; i < 3 && getline(ctaidlist, ctaid_cur, '/'); i++) {
              if (std::all_of(ctaid_cur.begin(), ctaid_cur.end(), ::isdigit)) {
                ctaid[i] = stoi(ctaid_cur);
              }
            }

            uint64_t ctaid_conv =
              ( (uint64_t)ctaid[0] << 32 ) +
              ( (uint64_t)(ctaid[1] & 0xFFFF) << 16 ) +
              ( (uint64_t)ctaid[2] & 0xFFFF );
            pass_args.cta.push_back(ctaid_conv);
          }
          

        } else if (optname == "warp") {
          std::string optarg;
          while (getline(optarglist, optarg, ' ')) {
            
            if (std::all_of(optarg.begin(), optarg.end(), ::isdigit)) {
              uint32_t warpid = std::stoi(optarg);
              pass_args.warp.push_back(warpid);
            }
          }
          


          
        } else {
          fprintf(stderr, "cuprof: unused argument: %s\n", optstr.c_str());
        }

        
      }


      return true;
    }
    
    // Run the plugin automatically
    ActionType getActionType() override {
      return ActionType::AddBeforeMainAction;
    }

  private:

  };




  
  static void registerStandardPasses(const PassManagerBuilder &, legacy::PassManagerBase &PM) {
    
    PM.add(createMarkAllDeviceForInlinePass());
    PM.add(createAlwaysInlinerLegacyPass());
    PM.add(createLinkDeviceSupportPass());
    PM.add(createInstrumentDevicePass(pass_args));

    PM.add(createInstrumentHostPass(pass_args));
  }
  
  static FrontendPluginRegistry::Add<clang::PluginEntry>
  X("cuprof", "cuda profiler");
}

