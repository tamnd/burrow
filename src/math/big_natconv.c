/* math/big: natural numbers to and from text, natconv.go.
 *
 * Copyright 2015 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "big_internal.h"

#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/math.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/sync.h"

static const char big_digits[] =
    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

#define BIG_MAX_BASE_SMALL (10 + ('z' - 'a' + 1))

BURROW_SENTINEL_ERROR(big_err_no_digits, "number has no digits");
BURROW_SENTINEL_ERROR(big_err_inval_sep, "'_' must separate successive digits");

/* maxPow: (b**n, n) for the largest n with b**n <= _M, which is how many
 * digits in base b fit in a word. */
static BigWord big_max_pow(BigWord b, int *n) {
    BigWord p = b;
    *n = 1;
    for (BigWord max = BIG_M / b; p <= max;) {
        p *= b;
        (*n)++;
    }
    return p;
}

/* pow: x**n for n > 0, and 1 otherwise. */
static BigWord big_pow_w(BigWord x, int n) {
    BigWord p = 1;
    while (n > 0) {
        if ((n & 1) != 0)
            p *= x;
        x *= x;
        n >>= 1;
    }
    return p;
}

/* ---------------------------------------------------------------- scanner */

void big_scanner_init(BigScanner *r, Str s) {
    r->p = s.p;
    r->len = s.len;
    r->pos = 0;
    r->st = NULL;
}

void big_scanner_init_state(BigScanner *r, FmtScanState *st) {
    r->p = NULL;
    r->len = 0;
    r->pos = 0;
    r->st = st;
}

/* From a ScanState a byte is a rune, and a rune that is not one byte is an
 * error, the same as Go's byteReader. */
Error big_scanner_read(BigScanner *r, Byte *c) {
    if (r->st != NULL) {
        Int size = 0;
        Error err = BURROW_NO_ERROR;
        Rune ch = r->st->vt->read_rune(r->st->data, &size, &err);
        if (size != 1 && BURROW_OK(err))
            err = fmt_errorf_v("invalid rune %#U", ch);
        *c = (Byte)ch;
        return err;
    }
    if (r->pos >= r->len) {
        *c = 0;
        return io_eof;
    }
    *c = r->p[r->pos++];
    return BURROW_NO_ERROR;
}

void big_scanner_unread(BigScanner *r) {
    if (r->st != NULL) {
        r->st->vt->unread_rune(r->st->data);
        return;
    }
    if (r->pos > 0)
        r->pos--;
}

/* ------------------------------------------------------------------- scan */

Nat nat_scan(Nat z, BigScanner *r, int base, bool frac_ok, int *res_base,
             Int *res_count, Error *res_err) {
    /* Reject invalid bases. */
    bool base_ok = base == 0 || (!frac_ok && 2 <= base && base <= BIG_MAX_BASE) ||
                   (frac_ok && (base == 2 || base == 8 || base == 10 || base == 16));
    if (!base_ok)
        panic_str(fmt_sprintf_v(error_allocator(), "invalid number base %d", base));

    /* prev is the previous character: '_', '0' for a digit, or '.' for
     * anything else. A separator may only follow a digit, and only in base
     * 0. */
    int prev = '.';
    bool inval_sep = false;
    Int count = 0;

    /* one character of look ahead */
    Byte ch;
    Error err = big_scanner_read(r, &ch);

    /* The actual base. */
    int b = base;
    int prefix = 0;
    if (base == 0) {
        /* 10 unless there is a prefix */
        b = 10;
        if (BURROW_OK(err) && ch == '0') {
            prev = '0';
            count = 1;
            err = big_scanner_read(r, &ch);
            if (BURROW_OK(err)) {
                /* possibly one of 0b, 0B, 0o, 0O, 0x, 0X */
                switch (ch) {
                case 'b':
                case 'B':
                    b = 2;
                    prefix = 'b';
                    break;
                case 'o':
                case 'O':
                    b = 8;
                    prefix = 'o';
                    break;
                case 'x':
                case 'X':
                    b = 16;
                    prefix = 'x';
                    break;
                default:
                    if (!frac_ok) {
                        b = 8;
                        prefix = '0';
                    }
                }
                if (prefix != 0) {
                    count = 0; /* the prefix is not counted */
                    if (prefix != '0')
                        err = big_scanner_read(r, &ch);
                }
            }
        }
    }

    /* Collect digits in groups of at most n in di. Bases 2, 4 and 16 pack
     * into words exactly, so their groups are appended as they are and put
     * in order at the end (bn == 0 marks this). The other bases shift z up a
     * group with mulAddWW for every group. */
    z = nat_to(z, 0);
    BigWord b1 = (BigWord)b;
    BigWord bn = 0; /* b1**n, or 0 for the bit packing bases */
    int n = 0;      /* the most digits that fit in a word */
    switch (b) {
    case 2:
        n = (int)BIG_W;
        break;
    case 4:
        n = (int)BIG_W / 2;
        break;
    case 16:
        n = (int)BIG_W / 4;
        break;
    default:
        bn = big_max_pow(b1, &n);
    }
    BigWord di = 0; /* 0 <= di < b1**i < bn */
    int i = 0;      /* 0 <= i < n */
    Int dp = -1;    /* where the point is */
    while (BURROW_OK(err)) {
        if (ch == '.' && frac_ok) {
            frac_ok = false;
            if (prev == '_')
                inval_sep = true;
            prev = '.';
            dp = count;
        } else if (ch == '_' && base == 0) {
            if (prev != '0')
                inval_sep = true;
            prev = '_';
        } else {
            /* the digit's value */
            BigWord d1;
            if ('0' <= ch && ch <= '9')
                d1 = (BigWord)(ch - '0');
            else if ('a' <= ch && ch <= 'z')
                d1 = (BigWord)(ch - 'a') + 10;
            else if ('A' <= ch && ch <= 'Z')
                d1 = b <= BIG_MAX_BASE_SMALL ? (BigWord)(ch - 'A' + 10)
                                             : (BigWord)(ch - 'A' + BIG_MAX_BASE_SMALL);
            else
                d1 = BIG_MAX_BASE + 1;
            if (d1 >= b1) {
                big_scanner_unread(r); /* ch is not part of the number */
                break;
            }
            prev = '0';
            count++;

            /* collect d1 in di */
            di = di * b1 + d1;
            i++;

            /* a full group goes into the result */
            if (i == n) {
                if (bn == 0) {
                    /* append(z, di) */
                    Int len = z.len;
                    if (len < z.cap) {
                        z.len++;
                    } else {
                        Nat t = nat_make(NAT_NIL, len + 1);
                        nat_copy(t, z);
                        z = t;
                    }
                    z.p[len] = di;
                } else {
                    z = nat_mul_add_ww(z, z, bn, di);
                }
                di = 0;
                i = 0;
            }
        }

        err = big_scanner_read(r, &ch);
    }

    if (errors_is(err, io_eof))
        err = BURROW_NO_ERROR;

    /* other errors come before a misplaced separator */
    if (BURROW_OK(err) && (inval_sep || prev == '_'))
        err = big_err_inval_sep;

    if (count == 0) {
        /* no digits */
        if (prefix == '0') {
            /* Only the octal prefix 0, maybe followed by separators and
             * digits past 7: that is a decimal zero. */
            *res_base = 10;
            *res_count = 1;
            *res_err = err;
            return nat_to(z, 0);
        }
        err = big_err_no_digits; /* and the result is 0 */
    }

    if (bn == 0) {
        if (i > 0) {
            /* The last group, left justified, and shifted back down after
             * the words are put in order. */
            Int len = z.len;
            if (len < z.cap) {
                z.len++;
            } else {
                Nat t = nat_make(NAT_NIL, len + 1);
                nat_copy(t, z);
                z = t;
            }
            z.p[len] = di * big_pow_w(b1, n - i);
        }
        for (Int lo = 0, hi = z.len - 1; lo < hi; lo++, hi--) {
            BigWord t = z.p[lo];
            z.p[lo] = z.p[hi];
            z.p[hi] = t;
        }
        z = nat_norm(z);
        if (i > 0)
            z = nat_rsh(z, z, (Uint)(n - i) * (BIG_W / (Uint)n));
    } else {
        if (i > 0)
            z = nat_mul_add_ww(z, z, big_pow_w(b1, i), di);
    }

    /* the count for a fraction */
    if (dp >= 0)
        count = dp - count; /* 0 <= dp <= count */

    *res_base = b;
    *res_count = count;
    *res_err = err;
    return z;
}

/* ------------------------------------------------------------------- itoa */

/* One entry of the table of divisors the recursive conversion splits by. */
typedef struct BigDivisor {
    Nat bbb;     /* the divisor */
    Int nbits;   /* its length in bits */
    Int ndigits; /* its length in digits of the output base */
} BigDivisor;

/* Blocks longer than this many words are split in two. Go keeps it in a
 * variable so its tests can change it; here it is fixed at Go's value. */
#define BIG_LEAF_SIZE 8

/* The divisors for base 10, which almost every conversion uses, built once
 * and kept for the life of the process. The words are on the heap and an
 * entry is never changed once its ndigits is set, so a table handed out stays
 * good after the lock is dropped. */
static SyncMutex big_cache10_mu;
static BigDivisor big_cache10[64];

static void big_convert_words(Nat q, Byte *s, Int slen, BigWord b, int ndigits,
                              BigWord bb, const BigDivisor *table, Int ntable);

Nat nat_exp_ww(Nat z, BigWord x, BigWord y) {
    return nat_exp_nn(z, nat_set_word(NAT_NIL, x), nat_set_word(NAT_NIL, y), NAT_NIL,
                      false);
}

/* A copy of x on the heap, for the cache. */
static Nat big_nat_keep(Nat x) {
    Nat r = {NULL, x.len, x.len};
    if (x.len > 0) {
        r.p = mem_alloc_array(heap_allocator(), (size_t)x.len, sizeof(BigWord),
                              _Alignof(BigWord));
        if (r.p == NULL)
            big_oom();
        memcpy(r.p, x.p, (size_t)x.len * sizeof(BigWord));
    }
    return r;
}

/* divisors: the table of the powers of bb**leafSize by repeated squaring, up
 * to about the square root of an m word number. NULL when m is small enough
 * for the iterative conversion. */
static const BigDivisor *big_divisors(Int m, BigWord b, int ndigits, BigWord bb,
                                      Int *ntable) {
    *ntable = 0;
    if (m <= BIG_LEAF_SIZE)
        return NULL;

    /* the k where (bb**leafSize)**(2**k) >= sqrt(x) */
    Int k = 1;
    for (Int words = BIG_LEAF_SIZE; words < m >> 1 && k < 64; words <<= 1)
        k++;

    /* Base 10 extends and reuses the cache, anything else makes its own. */
    BigDivisor *table;
    bool cached = b == 10;
    if (cached) {
        sync_mutex_lock(&big_cache10_mu);
        table = big_cache10;
    } else {
        table = (BigDivisor *)big_alloc(
            (Int)((sizeof(BigDivisor) * (size_t)k + sizeof(BigWord) - 1) /
                  sizeof(BigWord)));
        memset(table, 0, sizeof(BigDivisor) * (size_t)k);
    }

    /* add entries as needed */
    if (table[k - 1].ndigits == 0) {
        for (Int i = 0; i < k; i++) {
            if (table[i].ndigits != 0)
                continue;
            Nat bbb;
            Int nd;
            if (i == 0) {
                bbb = nat_exp_ww(NAT_NIL, bb, BIG_LEAF_SIZE);
                nd = (Int)ndigits * BIG_LEAF_SIZE;
            } else {
                bbb = nat_sqr(NAT_NIL, table[i - 1].bbb);
                nd = 2 * table[i - 1].ndigits;
            }

            /* Use up the spare bits at the top of the block. */
            Nat larger = nat_set(NAT_NIL, bbb);
            while (big_mul_add_vww(larger, larger, b, 0) == 0) {
                bbb = nat_set(bbb, larger);
                nd++;
            }

            table[i].bbb = cached ? big_nat_keep(bbb) : bbb;
            table[i].nbits = nat_bit_len(bbb);
            table[i].ndigits = nd;
        }
    }

    if (cached)
        sync_mutex_unlock(&big_cache10_mu);
    *ntable = k;
    return table;
}

Str nat_itoa(Nat x, bool neg, int base) {
    if (base < 2 || base > BIG_MAX_BASE)
        panic_str(BURROW_S("invalid base"));

    /* x == 0 */
    if (x.len == 0)
        return BURROW_S("0");
    /* len(x) > 0 */

    /* the buffer, one too long at most */
    Int i = (Int)((double)nat_bit_len(x) / math_log2((double)base)) + 1;
    if (neg)
        i++;
    Int slen = i;
    Byte *s = (Byte *)big_alloc((slen + BIG_S - 1) / BIG_S);

    BigWord b = (BigWord)base;
    if (b == (b & (0 - b))) {
        /* A power of two: the digits are groups of bits. */
        Uint shift = (Uint)bits_trailing_zeros(b); /* shift > 0 because b >= 2 */
        BigWord mask = ((BigWord)1 << shift) - 1;
        BigWord w = x.p[0]; /* the current word */
        Uint nbits = BIG_W; /* how many bits of w are left */

        /* the lower words, leading zeros included */
        for (Int k = 1; k < x.len; k++) {
            /* the whole digits */
            while (nbits >= shift) {
                i--;
                s[i] = (Byte)big_digits[w & mask];
                w >>= shift;
                nbits -= shift;
            }

            /* a digit split across two words, then the next word */
            if (nbits == 0) {
                w = x.p[k];
                nbits = BIG_W;
            } else {
                w |= x.p[k] << nbits;
                i--;
                s[i] = (Byte)big_digits[w & mask];
                w = x.p[k] >> (shift - nbits);
                nbits = BIG_W - (shift - nbits);
            }
        }

        /* the top word, without leading zeros */
        while (w != 0) {
            i--;
            s[i] = (Byte)big_digits[w & mask];
            w >>= shift;
        }
    } else {
        int ndigits;
        BigWord bb = big_max_pow(b, &ndigits);

        /* the table of squares of bb**leafSize to split by, when x is long */
        Int ntable;
        const BigDivisor *table = big_divisors(x.len, b, ndigits, bb, &ntable);

        /* q is a copy, since the conversion destroys it */
        Nat q = nat_set(NAT_NIL, x);
        big_convert_words(q, s, slen, b, ndigits, bb, table, ntable);

        /* Strip leading zeros. x is not zero, so there is a digit that is
         * not. */
        i = 0;
        while (s[i] == '0')
            i++;
    }

    if (neg) {
        i--;
        s[i] = '-';
    }

    Str r = {s + i, slen - i};
    return r;
}

Str nat_utoa(Nat x, int base) {
    return nat_itoa(x, false, base);
}

/* convertWords: the digits of q into s, right aligned with zeros in front. A
 * long q is split in two by the divisor nearest its square root and the two
 * halves converted on their own; a short one takes a word of digits at a time
 * off the bottom. */
static void big_convert_words(Nat q, Byte *s, Int slen, BigWord b, int ndigits,
                              BigWord bb, const BigDivisor *table, Int ntable) {
    /* split long blocks */
    if (table != NULL) {
        /* len(q) > leafSize > 0 */
        Nat r = NAT_NIL;
        Int index = ntable - 1;
        while (q.len > BIG_LEAF_SIZE) {
            /* a divisor near sqrt(q), and in any case less than q */
            Int max_length = nat_bit_len(q);
            Int min_length = max_length >> 1;
            while (index > 0 && table[index - 1].nbits > min_length)
                index--;
            if (table[index].nbits >= max_length && nat_cmp(table[index].bbb, q) >= 0) {
                index--;
                if (index < 0)
                    panic_str(BURROW_S("internal inconsistency"));
            }

            /* q = q'*bbb + r, and the two convert on their own */
            q = nat_div(q, r, q, table[index].bbb, &r);

            Int h = slen - table[index].ndigits;
            big_convert_words(r, s + h, slen - h, b, ndigits, bb, table, index);
            slen = h;
        }
    }

    /* what is left, a word at a time */
    Int i = slen;
    BigWord r;
    if (b == 10) {
        /* 10 by name, so that / and % are by a constant */
        while (q.len > 0) {
            q = nat_div_w(q, q, bb, &r);
            for (int j = 0; j < ndigits && i > 0; j++) {
                i--;
                BigWord t = r / 10;
                s[i] = (Byte)('0' + (r - t * 10));
                r = t;
            }
        }
    } else {
        while (q.len > 0) {
            q = nat_div_w(q, q, bb, &r);
            for (int j = 0; j < ndigits && i > 0; j++) {
                i--;
                s[i] = (Byte)big_digits[r % b];
                r /= b;
            }
        }
    }

    /* the zeros in front */
    while (i > 0) {
        i--;
        s[i] = '0';
    }
}
