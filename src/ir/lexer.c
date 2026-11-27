/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ir/lexer.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool is_ident_start(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool is_ident_cont(int c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static int hex_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char peek(const PirLexer *lex, size_t off) {
    return (lex->pos + off < lex->len) ? lex->src[lex->pos + off] : '\0';
}

static char advance(PirLexer *lex) {
    if (lex->pos >= lex->len) {
        return '\0';
    }
    char c = lex->src[lex->pos++];
    if (c == '\n') {
        lex->line++;
        lex->col = 1u;
    } else {
        lex->col++;
    }
    return c;
}

static bool u128_add(uint64_t *hi, uint64_t *lo, uint64_t ahi, uint64_t alo) {
    uint64_t nlo = *lo + alo;
    uint64_t carry = (nlo < *lo) ? 1u : 0u;
    uint64_t nhi = *hi + ahi + carry;
    bool ovf = (nhi < *hi) || (carry && nhi == *hi);
    *lo = nlo;
    *hi = nhi;
    return !ovf;
}

static bool u128_shl(uint64_t *hi, uint64_t *lo, unsigned shift) {
    if (shift == 0u) return true;
    uint64_t lost = *hi >> (64u - shift);
    *hi = (*hi << shift) | (*lo >> (64u - shift));
    *lo = *lo << shift;
    return lost == 0u;
}

static bool lex_string_body(PirLexer *lex, uint32_t start_line, uint32_t start_col,
                            char **out_buf, size_t *out_len) {
    /* Worst-case decoded length <= remaining bytes in source. */
    size_t start_pos = lex->pos;
    size_t cap = 64u;
    char *tmp = (char *)malloc(cap);
    if (!tmp) abort();
    size_t len = 0u;

    while (lex->pos < lex->len) {
        char c = advance(lex);
        if (c == '"') {
            char *res = (char *)pir_arena_alloc_aligned(lex->arena, len + 1u, 1u);
            if (len > 0u) memcpy(res, tmp, len);
            res[len] = '\0';
            free(tmp);
            *out_buf = res;
            *out_len = len;
            return true;
        }
        if (c == '\n' || c == '\r') {
            free(tmp);
            pir_diag_error(lex->diag, start_line, start_col, "unescaped newline in string literal");
            return false;
        }
        unsigned char byte = (unsigned char)c;
        if (c == '\\') {
            if (lex->pos >= lex->len) {
                free(tmp);
                pir_diag_error(lex->diag, start_line, start_col, "unterminated escape in string");
                return false;
            }
            char esc = advance(lex);
            switch (esc) {
                case 'n':  byte = '\n'; break;
                case 'r':  byte = '\r'; break;
                case 't':  byte = '\t'; break;
                case '\\': byte = '\\'; break;
                case '"':  byte = '"';  break;
                case '0':  byte = '\0'; break;
                case 'x': {
                    int h1 = hex_val((unsigned char)peek(lex, 0));
                    int h2 = hex_val((unsigned char)peek(lex, 1));
                    if (h1 < 0 || h2 < 0) {
                        free(tmp);
                        pir_diag_error(lex->diag, lex->line, lex->col, "invalid \\xNN escape");
                        return false;
                    }
                    advance(lex);
                    advance(lex);
                    byte = (unsigned char)((h1 << 4) | h2);
                    break;
                }
                default:
                    free(tmp);
                    pir_diag_error(lex->diag, lex->line, lex->col,
                                   "unknown escape sequence '\\%c'", esc);
                    return false;
            }
        }
        if (len + 1u >= cap) {
            cap *= 2u;
            tmp = (char *)realloc(tmp, cap);
            if (!tmp) abort();
        }
        tmp[len++] = (char)byte;
    }
    (void)start_pos;
    free(tmp);
    pir_diag_error(lex->diag, start_line, start_col, "unterminated string literal");
    return false;
}

static PirToken lex_number(PirLexer *lex, bool neg, uint32_t line, uint32_t col, bool attached) {
    PirToken tok;
    memset(&tok, 0, sizeof(tok));
    tok.line     = line;
    tok.col      = col;
    tok.attached = attached;
    tok.int_neg  = neg;

    size_t num_start = lex->pos;
    bool is_hex = false;
    bool is_bin = false;
    bool is_float = false;

    if (peek(lex, 0) == '0' && (peek(lex, 1) == 'x' || peek(lex, 1) == 'X')) {
        is_hex = true;
        advance(lex);
        advance(lex);
    } else if (peek(lex, 0) == '0' && (peek(lex, 1) == 'b' || peek(lex, 1) == 'B')) {
        is_bin = true;
        advance(lex);
        advance(lex);
    }

    size_t digits_start = lex->pos;
    if (is_hex) {
        bool has_dot = false;
        bool has_exp = false;
        while (hex_val((unsigned char)peek(lex, 0)) >= 0) advance(lex);
        if (peek(lex, 0) == '.') {
            has_dot = true;
            is_float = true;
            advance(lex);
            while (hex_val((unsigned char)peek(lex, 0)) >= 0) advance(lex);
        }
        if (peek(lex, 0) == 'p' || peek(lex, 0) == 'P') {
            has_exp = true;
            is_float = true;
            advance(lex);
            if (peek(lex, 0) == '+' || peek(lex, 0) == '-') advance(lex);
            size_t exp_start = lex->pos;
            while (isdigit((unsigned char)peek(lex, 0))) advance(lex);
            if (lex->pos == exp_start) {
                pir_diag_error(lex->diag, line, col, "hexadecimal float literal missing exponent digits");
                tok.kind = TOK_ERR;
                return tok;
            }
        }
        if (has_dot && !has_exp) {
            pir_diag_error(lex->diag, line, col, "hexadecimal float literal requires 'p'/'P' exponent");
            tok.kind = TOK_ERR;
            return tok;
        }
    } else if (is_bin) {
        while (peek(lex, 0) == '0' || peek(lex, 0) == '1') advance(lex);
    } else {
        while (isdigit((unsigned char)peek(lex, 0))) advance(lex);
        if (peek(lex, 0) == '.' && isdigit((unsigned char)peek(lex, 1))) {
            is_float = true;
            advance(lex);
            while (isdigit((unsigned char)peek(lex, 0))) advance(lex);
        }
        if (peek(lex, 0) == 'e' || peek(lex, 0) == 'E') {
            is_float = true;
            advance(lex);
            if (peek(lex, 0) == '+' || peek(lex, 0) == '-') advance(lex);
            while (isdigit((unsigned char)peek(lex, 0))) advance(lex);
        }
    }

    if (is_ident_cont((unsigned char)peek(lex, 0))) {
        pir_diag_error(lex->diag, line, col, "invalid suffix on numeric literal");
        tok.kind = TOK_ERR;
        return tok;
    }

    if (is_float) {
        size_t flen = lex->pos - num_start;
        char *buf = (char *)malloc(flen + 2u);
        if (!buf) abort();
        size_t off = 0u;
        if (neg) buf[off++] = '-';
        memcpy(buf + off, lex->src + num_start, flen);
        buf[off + flen] = '\0';
        char *endp = NULL;
        errno = 0;
        tok.float_val = strtod(buf, &endp);
        int err_d = errno;
        bool ok = (endp == buf + off + flen);
        int err_f = 0;
        if (ok) {
            char *endp_f = NULL;
            errno = 0;
            tok.float_val_f32 = strtof(buf, &endp_f);
            err_f = errno;
            if (endp_f != buf + off + flen) ok = false;
        }
        free(buf);
        if (!ok) {
            pir_diag_error(lex->diag, line, col, "invalid float literal");
            tok.kind = TOK_ERR;
            return tok;
        }
        if (err_d == ERANGE && isinf(tok.float_val)) {
            pir_diag_error(lex->diag, line, col, "float literal overflows f64 range");
            tok.kind = TOK_ERR;
            return tok;
        }
        tok.float_f32_overflow = (err_f == ERANGE && isinf(tok.float_val_f32));
        tok.kind = TOK_FLOAT;
        return tok;
    }

    if (lex->pos == digits_start) {
        pir_diag_error(lex->diag, line, col, "expected digits in integer literal");
        tok.kind = TOK_ERR;
        return tok;
    }

    uint64_t hi = 0u, lo = 0u;
    bool ok = true;
    for (size_t i = digits_start; i < lex->pos; ++i) {
        int d = hex_val((unsigned char)lex->src[i]);
        if (is_hex) {
            ok = ok && u128_shl(&hi, &lo, 4u) && u128_add(&hi, &lo, 0u, (uint64_t)d);
        } else if (is_bin) {
            ok = ok && u128_shl(&hi, &lo, 1u) && u128_add(&hi, &lo, 0u, (uint64_t)d);
        } else {
            uint64_t h1 = hi, l1 = lo;
            uint64_t h3 = hi, l3 = lo;
            ok = ok && u128_shl(&h1, &l1, 1u) && u128_shl(&h3, &l3, 3u);
            hi = h3; lo = l3;
            ok = ok && u128_add(&hi, &lo, h1, l1) && u128_add(&hi, &lo, 0u, (uint64_t)d);
        }
    }
    if (!ok) {
        pir_diag_error(lex->diag, line, col, "integer literal overflows 128 bits");
        tok.kind = TOK_ERR;
        return tok;
    }
    tok.kind   = TOK_INT;
    tok.int_lo = lo;
    tok.int_hi = hi;
    return tok;
}

void pir_lexer_init(PirLexer *lex, const char *src, size_t len, PirArena *arena, PirDiag *diag) {
    lex->src   = src;
    lex->len   = len;
    lex->pos   = 0u;
    lex->line  = 1u;
    lex->col   = 1u;
    lex->arena = arena;
    lex->diag  = diag;
}

PirToken pir_lexer_next(PirLexer *lex) {
    bool skipped_ws = false;
    while (lex->pos < lex->len) {
        char c = peek(lex, 0);
        if (c == ' ' || c == '\t' || c == '\r') {
            skipped_ws = true;
            advance(lex);
        } else if (c == '/' && peek(lex, 1) == '/') {
            skipped_ws = true;
            while (lex->pos < lex->len && peek(lex, 0) != '\n') {
                advance(lex);
            }
        } else {
            break;
        }
    }

    PirToken tok;
    memset(&tok, 0, sizeof(tok));
    tok.line     = lex->line;
    tok.col      = lex->col;
    tok.attached = !skipped_ws && lex->pos > 0u;

    if (lex->pos >= lex->len) {
        tok.kind = TOK_EOF;
        return tok;
    }

    char c = peek(lex, 0);
    if (c == '\n') {
        advance(lex);
        tok.kind = TOK_NEWLINE;
        return tok;
    }

    if (c == '{') { advance(lex); tok.kind = TOK_LBRACE;   return tok; }
    if (c == '}') { advance(lex); tok.kind = TOK_RBRACE;   return tok; }
    if (c == '(') { advance(lex); tok.kind = TOK_LPAREN;   return tok; }
    if (c == ')') { advance(lex); tok.kind = TOK_RPAREN;   return tok; }
    if (c == '[') { advance(lex); tok.kind = TOK_LBRACKET; return tok; }
    if (c == ']') { advance(lex); tok.kind = TOK_RBRACKET; return tok; }
    if (c == ',') { advance(lex); tok.kind = TOK_COMMA;    return tok; }
    if (c == ':') { advance(lex); tok.kind = TOK_COLON;    return tok; }
    if (c == '=') { advance(lex); tok.kind = TOK_EQ;       return tok; }
    if (c == '+') { advance(lex); tok.kind = TOK_PLUS;     return tok; }

    if (c == '-') {
        if (peek(lex, 1) == '>') {
            advance(lex);
            advance(lex);
            tok.kind = TOK_ARROW;
            return tok;
        }
        if (isdigit((unsigned char)peek(lex, 1))) {
            advance(lex);
            return lex_number(lex, true, tok.line, tok.col, tok.attached);
        }
        if (peek(lex, 1) == 'i' && peek(lex, 2) == 'n' && peek(lex, 3) == 'f' &&
            !is_ident_cont((unsigned char)peek(lex, 4))) {
            advance(lex); advance(lex); advance(lex); advance(lex);
            tok.kind          = TOK_FLOAT;
            tok.float_val     = -HUGE_VAL;
            tok.float_val_f32 = -HUGE_VALF;
            return tok;
        }
        if (peek(lex, 1) == 'n' && peek(lex, 2) == 'a' && peek(lex, 3) == 'n' &&
            !is_ident_cont((unsigned char)peek(lex, 4))) {
            advance(lex); advance(lex); advance(lex); advance(lex);
            tok.kind          = TOK_FLOAT;
            tok.float_val     = -(double)NAN;
            tok.float_val_f32 = -(float)NAN;
            return tok;
        }
        advance(lex);
        tok.kind = TOK_MINUS;
        return tok;
    }

    if (c == '.') {
        if (peek(lex, 1) == '.' && peek(lex, 2) == '.') {
            advance(lex); advance(lex); advance(lex);
            tok.kind = TOK_ELLIPSIS;
            return tok;
        }
        advance(lex);
        size_t start = lex->pos;
        if (!is_ident_start((unsigned char)peek(lex, 0))) {
            pir_diag_error(lex->diag, tok.line, tok.col, "expected identifier after '.'");
            tok.kind = TOK_ERR;
            return tok;
        }
        while (is_ident_cont((unsigned char)peek(lex, 0))) advance(lex);
        tok.kind = TOK_LABEL;
        tok.len  = lex->pos - start;
        tok.text = pir_arena_strndup(lex->arena, lex->src + start, tok.len);
        return tok;
    }

    if (c == '%') {
        advance(lex);
        size_t start = lex->pos;
        if (is_ident_start((unsigned char)peek(lex, 0))) {
            while (is_ident_cont((unsigned char)peek(lex, 0))) advance(lex);
        } else if (isdigit((unsigned char)peek(lex, 0))) {
            while (isdigit((unsigned char)peek(lex, 0))) advance(lex);
            if (is_ident_cont((unsigned char)peek(lex, 0))) {
                pir_diag_error(lex->diag, tok.line, tok.col, "invalid numeric local name after '%%'");
                tok.kind = TOK_ERR;
                return tok;
            }
        } else {
            pir_diag_error(lex->diag, tok.line, tok.col, "expected local name after '%%'");
            tok.kind = TOK_ERR;
            return tok;
        }
        tok.kind = TOK_LOCAL;
        tok.len  = lex->pos - start;
        tok.text = pir_arena_strndup(lex->arena, lex->src + start, tok.len);
        return tok;
    }

    if (c == '@') {
        advance(lex);
        if (peek(lex, 0) == '"') {
            advance(lex);
            char *s = NULL;
            size_t slen = 0u;
            if (!lex_string_body(lex, tok.line, tok.col, &s, &slen)) {
                tok.kind = TOK_ERR;
                return tok;
            }
            if (slen == 0u) {
                pir_diag_error(lex->diag, tok.line, tok.col, "symbol name cannot be empty");
                tok.kind = TOK_ERR;
                return tok;
            }
            if (memchr(s, '\0', slen) != NULL) {
                pir_diag_error(lex->diag, tok.line, tok.col, "symbol name cannot contain embedded NUL byte");
                tok.kind = TOK_ERR;
                return tok;
            }
            if (memchr(s, '\n', slen) != NULL || memchr(s, '\r', slen) != NULL) {
                pir_diag_error(lex->diag, tok.line, tok.col, "symbol name cannot contain newline");
                tok.kind = TOK_ERR;
                return tok;
            }
            tok.kind = TOK_GLOBAL;
            tok.text = s;
            tok.len  = slen;
            return tok;
        }
        size_t start = lex->pos;
        if (!is_ident_start((unsigned char)peek(lex, 0))) {
            pir_diag_error(lex->diag, tok.line, tok.col, "expected symbol name after '@'");
            tok.kind = TOK_ERR;
            return tok;
        }
        while (is_ident_cont((unsigned char)peek(lex, 0))) advance(lex);
        tok.kind = TOK_GLOBAL;
        tok.len  = lex->pos - start;
        tok.text = pir_arena_strndup(lex->arena, lex->src + start, tok.len);
        return tok;
    }

    if (c == '"') {
        advance(lex);
        char *s = NULL;
        size_t slen = 0u;
        if (!lex_string_body(lex, tok.line, tok.col, &s, &slen)) {
            tok.kind = TOK_ERR;
            return tok;
        }
        tok.kind = TOK_STRING;
        tok.text = s;
        tok.len  = slen;
        return tok;
    }

    if (isdigit((unsigned char)c)) {
        return lex_number(lex, false, tok.line, tok.col, tok.attached);
    }

    if (is_ident_start((unsigned char)c)) {
        size_t start = lex->pos;
        while (is_ident_cont((unsigned char)peek(lex, 0))) advance(lex);
        tok.kind = TOK_IDENT;
        tok.len  = lex->pos - start;
        tok.text = pir_arena_strndup(lex->arena, lex->src + start, tok.len);
        return tok;
    }

    advance(lex);
    pir_diag_error(lex->diag, tok.line, tok.col, "unexpected character '%c'", c);
    tok.kind = TOK_ERR;
    return tok;
}
