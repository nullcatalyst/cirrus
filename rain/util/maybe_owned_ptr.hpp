#pragma once

#include <cstddef>
#include <memory>

#include "absl/base/nullability.h"
#include "rain/util/console.hpp"
#include "rain/util/unreachable.hpp"

namespace rain::util {

template <typename T>
class MaybeOwnedPtr {
    enum class State {
        None,
        Ptr,
        OwnedPtr,
    };

    union {
        absl::Nonnull<T*>  _ptr;
        std::unique_ptr<T> _owned_ptr;
    };
    State _type = State::None;

  public:
    MaybeOwnedPtr() : _type(State::None) {}
    ~MaybeOwnedPtr() { _reset(); }

    // Copying is not allowed.
    //
    // This is because we don't know if the pointer is owned or not, and we don't want to make a
    // copy of the owned pointer, then accidentally free the original (owned version) when there
    // still exists an unowned copy referring to the same object.
    //
    // So unfortunately, this means that you as the developer have to stop and think about if you
    // want to make a copy, instead of accidentally doing it.
    MaybeOwnedPtr(const MaybeOwnedPtr& other)            = delete;
    MaybeOwnedPtr& operator=(const MaybeOwnedPtr& other) = delete;

    MaybeOwnedPtr(MaybeOwnedPtr&& other) {
        switch (other._type) {
            case State::None:
                _type = State::None;
                break;
            case State::Ptr:
                _type = State::Ptr;
                _ptr  = other._ptr;
                break;
            case State::OwnedPtr:
                _type = State::OwnedPtr;
                new (&_owned_ptr) std::unique_ptr<T>(std::move(other._owned_ptr));
                break;
        }

        other._reset();
    }
    MaybeOwnedPtr& operator=(MaybeOwnedPtr&& other) {
        _reset();

        switch (other._type) {
            case State::None:
                _type = State::None;
                break;
            case State::Ptr:
                _type = State::Ptr;
                _ptr  = other._ptr;
                break;
            case State::OwnedPtr:
                _type = State::OwnedPtr;
                new (&_owned_ptr) std::unique_ptr<T>(std::move(other._owned_ptr));
                break;
        }

        other._reset();
        return *this;
    }

    constexpr MaybeOwnedPtr(std::nullptr_t) : _type(State::None) {}
    MaybeOwnedPtr& operator=(std::nullptr_t) {
        _reset();
        _type = State::None;
        return *this;
    }

    MaybeOwnedPtr(absl::Nullable<T*> ptr)
        : _ptr(ptr), _type(ptr != nullptr ? State::Ptr : State::None) {}
    MaybeOwnedPtr& operator=(absl::Nonnull<T*> ptr) {
        _reset();
        _ptr  = ptr;
        _type = State::Ptr;
        return *this;
    }

    MaybeOwnedPtr(std::unique_ptr<T> owned_ptr)
        : _owned_ptr(std::move(owned_ptr)), _type(State::OwnedPtr) {}
    MaybeOwnedPtr& operator=(std::unique_ptr<T> owned_ptr) {
        _reset();
        new (&_owned_ptr) std::unique_ptr<T>(std::move(owned_ptr));
        _type = State::OwnedPtr;
        return *this;
    }

    constexpr bool operator==(const std::nullptr_t) const {
        switch (_type) {
            case State::None:
                return true;
            case State::Ptr:
                return _ptr == nullptr;
            case State::OwnedPtr:
                return _owned_ptr == nullptr;
        }
        util::unreachable();
    }

    constexpr bool operator!=(const std::nullptr_t) const {
        switch (_type) {
            case State::None:
                return false;
            case State::Ptr:
                return _ptr != nullptr;
            case State::OwnedPtr:
                return _owned_ptr != nullptr;
        }
        util::unreachable();
    }

    T* operator->() const {
        switch (_type) {
            case State::None:
                std::abort();
            case State::Ptr:
                return _ptr;
            case State::OwnedPtr:
                return _owned_ptr.get();
        }
        util::unreachable();
    }

    absl::Nullable<T*> get() const {
        switch (_type) {
            case State::None:
                return nullptr;
            case State::Ptr:
                return _ptr;
            case State::OwnedPtr:
                return _owned_ptr.get();
        }
        util::unreachable();
    }

    absl::Nullable<T*> get_nonnull() const {
        switch (_type) {
            case State::None:
                util::panic("trying to get a nonnull value from a null ptr");
            case State::Ptr:
                return _ptr;
            case State::OwnedPtr:
                return _owned_ptr.get();
        }
        util::unreachable();
    }

    std::unique_ptr<T> take() {
        switch (_type) {
            case State::None:
            case State::Ptr:
                return std::unique_ptr<T>();
            case State::OwnedPtr:
                return std::move(_owned_ptr);
        }
        util::unreachable();
    }

    [[nodiscard]] constexpr bool is_owned() const { return _type == State::OwnedPtr; }

  private:
    void _reset() {
        if (_type == State::OwnedPtr) {
            _owned_ptr.~unique_ptr<T>();
        }
        _type = State::None;
    }
};

}  // namespace rain::util
