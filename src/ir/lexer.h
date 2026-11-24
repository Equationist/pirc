/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PIR_IR_LEXER_H
#define PIR_IR_LEXER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/arena.h"
#include "common/diag.h"

typedef enum {
    TOK_EOF = 0,
    TOK_NEWLINE,
    TOK_LOCAL,      /* %name or %0 */
    TOK_GLOBAL,     /* @name or @"quoted" */
    TOK_LABEL,      /* .name (attached=true for type/op suffix like add.i32) */
    TOK_IDENT,      /* bareword */
    TOK_INT,        /* 128-bit integer literal */
    TOK_FLOAT,      /* floating-point literal (or -inf) */
    TOK_STRING,     /* "..." decoded bytes */
    TOK_LBRACE,     /* { */
    TOK_RBRACE,     /* } */
    TOK_LPAREN,     /* ( */
    TOK_RPAREN,     /* ) */
    TOK_LBRACKET,   /* [ */
    TOK_RBRACKET,   /* ] */
    TOK_COMMA,      /* , */
    TOK_COLON,      /* : */
    TOK_EQ,         /* = */
    TOK_ARROW,      /* -> */
    TOK_ELLIPSIS,   /* ... */
    TOK_PLUS,       /* + */
    TOK_MINUS,      /* - */
    TOK_ERR
} PirTokKind;

typedef struct {
    PirTokKind     kind;
    uint32_t       line;
    uint32_t       col;
    bool           attached;  /* No whitespace preceded this token */
    const char    *text;      /* Arena-allocated NUL-terminated text/bytes */
    size_t         len;       /* Byte length (important for TOK_STRING with \0) */
    uint64_t       int_lo;
    uint64_t       int_hi;
    bool           int_neg;
    double         float_val;
} PirToken;

typedef struct {
    const char *src;
    size_t      len;
    size_t      pos;
    uint32_t    line;
    uint32_t    col;
    PirArena   *arena;
    PirDiag    *diag;
} PirLexer;

void     pir_lexer_init(PirLexer *lex, const char *src, size_t len, PirArena *arena, PirDiag *diag);
PirToken pir_lexer_next(PirLexer *lex);

#endif /* PIR_IR_LEXER_H */
