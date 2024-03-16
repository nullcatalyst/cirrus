#pragma once

#include <cstdint>
#include <string_view>

#include "rain/lang/lex/location.hpp"

namespace rain::lang::lex {

enum TokenKind : uint32_t {
    Undefined,
    EndOfFile,

    // Atoms and literals
    Identifier,
    Integer,
    Float,

    // Keywords
    True,
    False,
    If,
    Else,
    Loop,
    While,
    For,
    Return,
    Break,
    Continue,
    Export,
    Struct,
    Interface,
    Self,
    Fn,
    Let,

    // Operators
    Hash,
    Period,
    Comma,
    Colon,
    Semicolon,
    RArrow,

    At,
    Exclaim,
    Ampersand,
    Pipe,
    Caret,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    LessLess,
    GreaterGreater,

    Equal,
    EqualEqual,
    ExclaimEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,

    // Yes, we all agree that these are bad names for these tokens, that there are "more correct"
    // names, but can we also agree that we all know (unamibiguously) what these names refer to?
    LRoundBracket,
    RRoundBracket,
    LSquareBracket,
    RSquareBracket,
    LCurlyBracket,
    RCurlyBracket,

    Count,
};

struct Token {
    TokenKind kind = TokenKind::Undefined;
    Location  location;

    constexpr std::string_view text() const { return location.text(); }
};

}  // namespace rain::lang::lex
