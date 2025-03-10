//------------------------------------------------------------------------------
//! @file Function.h
//! @brief Function-related utilities
//
// SPDX-FileCopyrightText: Michael Popoloski
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace slang {

/// An efficient, type-erasing, non-owning reference to a callable. This is
/// intended for use as the type of a function parameter that is not used
/// after the function in question returns.
///
/// This class does not own the callable, so it is not in general safe to store
/// a function_ref.
///
/// NOTE: This is based on the type of the same name from the LLVM project.
template<typename Fn>
class function_ref;

template<typename Ret, typename... Params>
class function_ref<Ret(Params...)> {
    Ret (*callback)(intptr_t callable, Params... params) = nullptr;
    intptr_t callable;

    template<typename Callable>
    static Ret callback_fn(intptr_t callable, Params... params) {
        return (*reinterpret_cast<Callable*>(callable))(std::forward<Params>(params)...);
    }

public:
    function_ref() = default;
    function_ref(std::nullptr_t) {}

    template<typename Callable>
    function_ref(Callable&& callable,
                 std::enable_if_t<!std::is_same<std::remove_cv_t<std::remove_reference_t<Callable>>,
                                                function_ref>::value>* = nullptr) :
        callback(callback_fn<typename std::remove_reference<Callable>::type>),
        callable(reinterpret_cast<intptr_t>(&callable)) {}

    /// Invokes the function with the given parameters.
    Ret operator()(Params... params) const {
        return callback(callable, std::forward<Params>(params)...);
    }

    /// @return true if the function_ref points to a valid function; otherwise, false
    /// if the function is null / empty.
    explicit operator bool() const { return callback; }
};

} // namespace slang
