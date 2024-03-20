#pragma once

#include <memory>

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Target/TargetMachine.h"

namespace rain::lang::code {

class Module {
    std::unique_ptr<llvm::Module> _llvm_mod;

  public:
    Module()                         = default;
    Module(const Module&)            = delete;
    Module& operator=(const Module&) = delete;
    Module(Module&&)                 = default;
    Module& operator=(Module&&)      = default;

    Module(std::shared_ptr<llvm::LLVMContext> ctx, std::unique_ptr<llvm::Module> mod);
    ~Module() = default;

    [[nodiscard]] const llvm::LLVMContext& llvm_context() const noexcept { return *_llvm_ctx; }
    [[nodiscard]] const llvm::Module&      llvm_module() const noexcept { return *_llvm_mod; }
    [[nodiscard]] llvm::Module&            llvm_module() noexcept { return *_llvm_mod; }

    [[nodiscard]] const Scope& exported_scope() const noexcept { return _exported_scope; }
    [[nodiscard]] Scope&       exported_scope() noexcept { return _exported_scope; }

    void optimize();

    [[nodiscard]] util::Result<std::string> emit_ir() const;
    // [[nodiscard]] util::Result<std::unique_ptr<llvm::MemoryBuffer>> emit_obj() const;
};

}  // namespace rain::lang::code
