#pragma once

#include <string_view>

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Value.h"
#include "rain/lang/ast/var/variable.hpp"
#include "rain/util/maybe_owned_ptr.hpp"

namespace rain::lang::ast {

class BlockVariable : public Variable {
    std::string_view          _name;
    util::MaybeOwnedPtr<Type> _type;
    bool                      _mutable = false;

    lex::Location _location;

  public:
    BlockVariable(std::string_view name, util::MaybeOwnedPtr<Type> type, bool mutable_,
                  lex::Location location)
        : _name(name), _type(std::move(type)), _mutable(mutable_), _location(location) {
        assert(_type != nullptr);
    }
    ~BlockVariable() override = default;

    [[nodiscard]] serial::VariableKind kind() const noexcept override {
        return serial::VariableKind::Local;
    }
    [[nodiscard]] std::string_view     name() const noexcept override { return _name; }
    [[nodiscard]] absl::Nonnull<Type*> type() const noexcept override {
        return _type.get_nonnull();
    }
    [[nodiscard]] bool          mutable_() const noexcept override { return _mutable; }
    [[nodiscard]] lex::Location location() const noexcept { return _location; }

    [[nodiscard]] util::Result<void> validate(Options& options, Scope& scope) noexcept override;
};

}  // namespace rain::lang::ast
