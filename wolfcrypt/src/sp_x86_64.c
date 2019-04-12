/* sp.c
 *
 * Copyright (C) 2006-2019 wolfSSL Inc.
 *
 * This file is part of wolfSSL.
 *
 * wolfSSL is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * wolfSSL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335, USA
 */

/* Implementation by Sean Parkinson. */

#ifdef HAVE_CONFIG_H
    #include <config.h>
#endif

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <wolfssl/wolfcrypt/cpuid.h>
#ifdef NO_INLINE
    #include <wolfssl/wolfcrypt/misc.h>
#else
    #define WOLFSSL_MISC_INCLUDED
    #include <wolfcrypt/src/misc.c>
#endif

#if defined(WOLFSSL_HAVE_SP_RSA) || defined(WOLFSSL_HAVE_SP_DH) || \
                                    defined(WOLFSSL_HAVE_SP_ECC)

#ifdef RSA_LOW_MEM
#ifndef SP_RSA_PRIVATE_EXP_D
#define SP_RSA_PRIVATE_EXP_D
#endif

#ifndef WOLFSSL_SP_SMALL
#define WOLFSSL_SP_SMALL
#endif
#endif

#include <wolfssl/wolfcrypt/sp.h>

#ifdef WOLFSSL_SP_X86_64_ASM
#if defined(WOLFSSL_HAVE_SP_RSA) || defined(WOLFSSL_HAVE_SP_DH)
#ifndef WOLFSSL_SP_NO_2048
/* Read big endian unsigned byte aray into r.
 *
 * r  A single precision integer.
 * a  Byte array.
 * n  Number of bytes in array to read.
 */
static void sp_2048_from_bin(sp_digit* r, int max, const byte* a, int n)
{
    int i, j = 0, s = 0;

    r[0] = 0;
    for (i = n-1; i >= 0; i--) {
        r[j] |= ((sp_digit)a[i]) << s;
        if (s >= 56) {
            r[j] &= 0xffffffffffffffffl;
            s = 64 - s;
            if (j + 1 >= max)
                break;
            r[++j] = a[i] >> s;
            s = 8 - s;
        }
        else
            s += 8;
    }

    for (j++; j < max; j++)
        r[j] = 0;
}

/* Convert an mp_int to an array of sp_digit.
 *
 * r  A single precision integer.
 * a  A multi-precision integer.
 */
static void sp_2048_from_mp(sp_digit* r, int max, mp_int* a)
{
#if DIGIT_BIT == 64
    int j;

    XMEMCPY(r, a->dp, sizeof(sp_digit) * a->used);

    for (j = a->used; j < max; j++)
        r[j] = 0;
#elif DIGIT_BIT > 64
    int i, j = 0, s = 0;

    r[0] = 0;
    for (i = 0; i < a->used && j < max; i++) {
        r[j] |= a->dp[i] << s;
        r[j] &= 0xffffffffffffffffl;
        s = 64 - s;
        if (j + 1 >= max)
            break;
        r[++j] = a->dp[i] >> s;
        while (s + 64 <= DIGIT_BIT) {
            s += 64;
            r[j] &= 0xffffffffffffffffl;
            if (j + 1 >= max)
                break;
            if (s < DIGIT_BIT)
                r[++j] = a->dp[i] >> s;
            else
                r[++j] = 0;
        }
        s = DIGIT_BIT - s;
    }

    for (j++; j < max; j++)
        r[j] = 0;
#else
    int i, j = 0, s = 0;

    r[0] = 0;
    for (i = 0; i < a->used && j < max; i++) {
        r[j] |= ((sp_digit)a->dp[i]) << s;
        if (s + DIGIT_BIT >= 64) {
            r[j] &= 0xffffffffffffffffl;
            if (j + 1 >= max)
                break;
            s = 64 - s;
            if (s == DIGIT_BIT) {
                r[++j] = 0;
                s = 0;
            }
            else {
                r[++j] = a->dp[i] >> s;
                s = DIGIT_BIT - s;
            }
        }
        else
            s += DIGIT_BIT;
    }

    for (j++; j < max; j++)
        r[j] = 0;
#endif
}

/* Write r as big endian to byte aray.
 * Fixed length number of bytes written: 256
 *
 * r  A single precision integer.
 * a  Byte array.
 */
static void sp_2048_to_bin(sp_digit* r, byte* a)
{
    int i, j, s = 0, b;

    j = 2048 / 8 - 1;
    a[j] = 0;
    for (i=0; i<32 && j>=0; i++) {
        b = 0;
        a[j--] |= r[i] << s; b += 8 - s;
        if (j < 0)
            break;
        while (b < 64) {
            a[j--] = r[i] >> b; b += 8;
            if (j < 0)
                break;
        }
        s = 8 - (b - 64);
        if (j >= 0)
            a[j] = 0;
        if (s != 0)
            j++;
    }
}

extern void sp_2048_mul_16(sp_digit* r, const sp_digit* a, const sp_digit* b);
extern void sp_2048_sqr_16(sp_digit* r, const sp_digit* a);
extern void sp_2048_mul_avx2_16(sp_digit* r, const sp_digit* a, const sp_digit* b);
extern void sp_2048_sqr_avx2_16(sp_digit* r, const sp_digit* a);
extern sp_digit sp_2048_add_16(sp_digit* r, const sp_digit* a, const sp_digit* b);
extern sp_digit sp_2048_sub_in_place_32(sp_digit* a, const sp_digit* b);
extern sp_digit sp_2048_add_32(sp_digit* r, const sp_digit* a, const sp_digit* b);
/* AND m into each word of a and store in r.
 *
 * r  A single precision integer.
 * a  A single precision integer.
 * m  Mask to AND against each digit.
 */
static void sp_2048_mask_16(sp_digit* r, sp_digit* a, sp_digit m)
{
#ifdef WOLFSSL_SP_SMALL
    int i;

    for (i=0; i<16; i++)
        r[i] = a[i] & m;
#else
    int i;

    for (i = 0; i < 16; i += 8) {
        r[i+0] = a[i+0] & m;
        r[i+1] = a[i+1] & m;
        r[i+2] = a[i+2] & m;
        r[i+3] = a[i+3] & m;
        r[i+4] = a[i+4] & m;
        r[i+5] = a[i+5] & m;
        r[i+6] = a[i+6] & m;
        r[i+7] = a[i+7] & m;
    }
#endif
}

/* Multiply a and b into r. (r = a * b)
 *
 * r  A single precision integer.
 * a  A single precision integer.
 * b  A single precision integer.
 */
SP_NOINLINE static void sp_2048_mul_32(sp_digit* r, const sp_digit* a,
        const sp_digit* b)
{
    sp_digit* z0 = r;
    sp_digit z1[32];
    sp_digit a1[16];
    sp_digit b1[16];
    sp_digit z2[32];
    sp_digit u, ca, cb;

    ca = sp_2048_add_16(a1, a, &a[16]);
    cb = sp_2048_add_16(b1, b, &b[16]);
    u  = ca & cb;
    sp_2048_mul_16(z1, a1, b1);
    sp_2048_mul_16(z2, &a[16], &b[16]);
    sp_2048_mul_16(z0, a, b);
    sp_2048_mask_16(r + 32, a1, 0 - cb);
    sp_2048_mask_16(b1, b1, 0 - ca);
    u += sp_2048_add_16(r + 32, r + 32, b1);
    u += sp_2048_sub_in_place_32(z1, z2);
    u += sp_2048_sub_in_place_32(z1, z0);
    u += sp_2048_add_32(r + 16, r + 16, z1);
    r[48] = u;
    XMEMSET(r + 48 + 1, 0, sizeof(sp_digit) * (16 - 1));
    sp_2048_add_32(r + 32, r + 32, z2);
}

/* Square a and put result in r. (r = a * a)
 *
 * r  A single precision integer.
 * a  A single precision integer.
 */
SP_NOINLINE static void sp_2048_sqr_32(sp_digit* r, const sp_digit* a)
{
    sp_digit* z0 = r;
    sp_digit z2[32];
    sp_digit z1[32];
    sp_digit a1[16];
    sp_digit u;

    u = sp_2048_add_16(a1, a, &a[16]);
    sp_2048_sqr_16(z1, a1);
    sp_2048_sqr_16(z2, &a[16]);
    sp_2048_sqr_16(z0, a);
    sp_2048_mask_16(r + 32, a1, 0 - u);
    u += sp_2048_add_16(r + 32, r + 32, r + 32);
    u += sp_2048_sub_in_place_32(z1, z2);
    u += sp_2048_sub_in_place_32(z1, z0);
    u += sp_2048_add_32(r + 16, r + 16, z1);
    r[48] = u;
    XMEMSET(r + 48 + 1, 0, sizeof(sp_digit) * (16 - 1));
    sp_2048_add_32(r + 32, r + 32, z2);
}

#ifdef HAVE_INTEL_AVX2
/* Multiply a and b into r. (r = a * b)
 *
 * r  A single precision integer.
 * a  A single precision integer.
 * b  A single precision integer.
 */
SP_NOINLINE static void sp_2048_mul_avx2_32(sp_digit* r, const sp_digit* a,
        const sp_digit* b)
{
    sp_digit* z0 = r;
    sp_digit z1[32];
    sp_digit a1[16];
    sp_digit b1[16];
    sp_digit z2[32];
    sp_digit u, ca, cb;

    ca = sp_2048_add_16(a1, a, &a[16]);
    cb = sp_2048_add_16(b1, b, &b[16]);
    u  = ca & cb;
    sp_2048_mul_avx2_16(z1, a1, b1);
    sp_2048_mul_avx2_16(z2, &a[16], &b[16]);
    sp_2048_mul_avx2_16(z0, a, b);
    sp_2048_mask_16(r + 32, a1, 0 - cb);
    sp_2048_mask_16(b1, b1, 0 - ca);
    u += sp_2048_add_16(r + 32, r + 32, b1);
    u += sp_2048_sub_in_place_32(z1, z2);
    u += sp_2048_sub_in_place_32(z1, z0);
    u += sp_2048_add_32(r + 16, r + 16, z1);
    r[48] = u;
    XMEMSET(r + 48 + 1, 0, sizeof(sp_digit) * (16 - 1));
    sp_2048_add_32(r + 32, r + 32, z2);
}
#endif /* HAVE_INTEL_AVX2 */

#ifdef HAVE_INTEL_AVX2
/* Square a and put result in r. (r = a * a)
 *
 * r  A single precision integer.
 * a  A single precision integer.
 */
SP_NOINLINE static void sp_2048_sqr_avx2_32(sp_digit* r, const sp_digit* a)
{
    sp_digit* z0 = r;
    sp_digit z2[32];
    sp_digit z1[32];
    sp_digit a1[16];
    sp_digit u;

    u = sp_2048_add_16(a1, a, &a[16]);
    sp_2048_sqr_avx2_16(z1, a1);
    sp_2048_sqr_avx2_16(z2, &a[16]);
    sp_2048_sqr_avx2_16(z0, a);
    sp_2048_mask_16(r + 32, a1, 0 - u);
    u += sp_2048_add_16(r + 32, r + 32, r + 32);
    u += sp_2048_sub_in_place_32(z1, z2);
    u += sp_2048_sub_in_place_32(z1, z0);
    u += sp_2048_add_32(r + 16, r + 16, z1);
    r[48] = u;
    XMEMSET(r + 48 + 1, 0, sizeof(sp_digit) * (16 - 1));
    sp_2048_add_32(r + 32, r + 32, z2);
}
#endif /* HAVE_INTEL_AVX2 */

#if (defined(WOLFSSL_HAVE_SP_RSA) || defined(WOLFSSL_HAVE_SP_DH)) && !defined(WOLFSSL_RSA_PUBLIC_ONLY)
#endif /* (WOLFSSL_HAVE_SP_RSA || WOLFSSL_HAVE_SP_DH) && !WOLFSSL_RSA_PUBLIC_ONLY */

/* Caclulate the bottom digit of -1/a mod 2^n.
 *
 * a    A single precision number.
 * rho  Bottom word of inverse.
 */
static void sp_2048_mont_setup(sp_digit* a, sp_digit* rho)
{
    sp_digit x, b;

    b = a[0];
    x = (((b + 2) & 4) << 1) + b; /* here x*a==1 mod 2**4 */
    x *= 2 - b * x;               /* here x*a==1 mod 2**8 */
    x *= 2 - b * x;               /* here x*a==1 mod 2**16 */
    x *= 2 - b * x;               /* here x*a==1 mod 2**32 */
    x *= 2 - b * x;               /* here x*a==1 mod 2**64 */

    /* rho = -1/m mod b */
    *rho = -x;
}

extern void sp_2048_mul_d_32(sp_digit* r, const sp_digit* a, const sp_digit b);
#if (defined(WOLFSSL_HAVE_SP_RSA) || defined(WOLFSSL_HAVE_SP_DH)) && !defined(WOLFSSL_RSA_PUBLIC_ONLY)
extern sp_digit sp_2048_sub_in_place_16(sp_digit* a, const sp_digit* b);
/* r = 2^n mod m where n is the number of bits to reduce by.
 * Given m must be 2048 bits, just need to subtract.
 *
 * r  A single precision number.
 * m  A signle precision number.
 */
static void sp_2048_mont_norm_16(sp_digit* r, sp_digit* m)
{
    XMEMSET(r, 0, sizeof(sp_digit) * 16);

    /* r = 2^n mod m */
    sp_2048_sub_in_place_16(r, m);
}

extern sp_digit sp_2048_cond_sub_16(sp_digit* r, sp_digit* a, sp_digit* b, sp_digit m);
extern void sp_2048_mont_reduce_16(sp_digit* a, sp_digit* m, sp_digit mp);
/* Multiply two Montogmery form numbers mod the modulus (prime).
 * (r = a * b mod m)
 *
 * r   Result of multiplication.
 * a   First number to multiply in Montogmery form.
 * b   Second number to multiply in Montogmery form.
 * m   Modulus (prime).
 * mp  Montogmery mulitplier.
 */
static void sp_2048_mont_mul_16(sp_digit* r, sp_digit* a, sp_digit* b,
        sp_digit* m, sp_digit mp)
{
    sp_2048_mul_16(r, a, b);
    sp_2048_mont_reduce_16(r, m, mp);
}

/* Square the Montgomery form number. (r = a * a mod m)
 *
 * r   Result of squaring.
 * a   Number to square in Montogmery form.
 * m   Modulus (prime).
 * mp  Montogmery mulitplier.
 */
static void sp_2048_mont_sqr_16(sp_digit* r, sp_digit* a, sp_digit* m,
        sp_digit mp)
{
    sp_2048_sqr_16(r, a);
    sp_2048_mont_reduce_16(r, m, mp);
}

extern void sp_2048_mul_d_16(sp_digit* r, const sp_digit* a, const sp_digit b);
extern void sp_2048_mul_d_avx2_16(sp_digit* r, const sp_digit* a, const sp_digit b);
/* Divide the double width number (d1|d0) by the dividend. (d1|d0 / div)
 *
 * d1   The high order half of the number to divide.
 * d0   The low order half of the number to divide.
 * div  The dividend.
 * returns the result of the division.
 */
static WC_INLINE sp_digit div_2048_word_16(sp_digit d1, sp_digit d0,
        sp_digit div)
{
    register sp_digit r asm("rax");
    __asm__ __volatile__ (
        "divq %3"
        : "=a" (r)
        : "d" (d1), "a" (d0), "r" (div)
        :
    );
    return r;
}
extern int64_t sp_2048_cmp_16(sp_digit* a, sp_digit* b);
/* Divide d in a and put remainder into r (m*d + r = a)
 * m is not calculated as it is not needed at this time.
 *
 * a  Nmber to be divided.
 * d  Number to divide with.
 * m  Multiplier result.
 * r  Remainder from the division.
 * returns MP_OKAY indicating success.
 */
static WC_INLINE int sp_2048_div_16(sp_digit* a, sp_digit* d, sp_digit* m,
        sp_digit* r)
{
    sp_digit t1[32], t2[17];
    sp_digit div, r1;
    int i;
#ifdef HAVE_INTEL_AVX2
    word32 cpuid_flags = cpuid_get_flags();
#endif

    (void)m;

    div = d[15];
    XMEMCPY(t1, a, sizeof(*t1) * 2 * 16);
    for (i=15; i>=0; i--) {
        r1 = div_2048_word_16(t1[16 + i], t1[16 + i - 1], div);

#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
            sp_2048_mul_d_avx2_16(t2, d, r1);
        else
#endif
            sp_2048_mul_d_16(t2, d, r1);
        t1[16 + i] += sp_2048_sub_in_place_16(&t1[i], t2);
        t1[16 + i] -= t2[16];
        sp_2048_mask_16(t2, d, t1[16 + i]);
        t1[16 + i] += sp_2048_add_16(&t1[i], &t1[i], t2);
        sp_2048_mask_16(t2, d, t1[16 + i]);
        t1[16 + i] += sp_2048_add_16(&t1[i], &t1[i], t2);
    }

    r1 = sp_2048_cmp_16(t1, d) >= 0;
    sp_2048_cond_sub_16(r, t1, d, (sp_digit)0 - r1);

    return MP_OKAY;
}

/* Reduce a modulo m into r. (r = a mod m)
 *
 * r  A single precision number that is the reduced result.
 * a  A single precision number that is to be reduced.
 * m  A single precision number that is the modulus to reduce with.
 * returns MP_OKAY indicating success.
 */
static WC_INLINE int sp_2048_mod_16(sp_digit* r, sp_digit* a, sp_digit* m)
{
    return sp_2048_div_16(a, m, NULL, r);
}

/* Modular exponentiate a to the e mod m. (r = a^e mod m)
 *
 * r     A single precision number that is the result of the operation.
 * a     A single precision number being exponentiated.
 * e     A single precision number that is the exponent.
 * bits  The number of bits in the exponent.
 * m     A single precision number that is the modulus.
 * returns 0 on success and MEMORY_E on dynamic memory allocation failure.
 */
static int sp_2048_mod_exp_16(sp_digit* r, sp_digit* a, sp_digit* e,
        int bits, sp_digit* m, int reduceA)
{
#ifndef WOLFSSL_SMALL_STACK
    sp_digit t[32][32];
#else
    sp_digit* t[32];
    sp_digit* td;
#endif
    sp_digit* norm;
    sp_digit mp = 1;
    sp_digit n;
    sp_digit mask;
    int i;
    int c, y;
    int err = MP_OKAY;

#ifdef WOLFSSL_SMALL_STACK
    td = (sp_digit*)XMALLOC(sizeof(sp_digit) * 32 * 32, NULL,
                            DYNAMIC_TYPE_TMP_BUFFER);
    if (td == NULL)
        err = MEMORY_E;

    if (err == MP_OKAY) {
        for (i=0; i<32; i++)
            t[i] = td + i * 32;
        norm = t[0];
    }
#else
    norm = t[0];
#endif

    if (err == MP_OKAY) {
        sp_2048_mont_setup(m, &mp);
        sp_2048_mont_norm_16(norm, m);

        XMEMSET(t[1], 0, sizeof(sp_digit) * 16);
        if (reduceA) {
            err = sp_2048_mod_16(t[1] + 16, a, m);
            if (err == MP_OKAY)
                err = sp_2048_mod_16(t[1], t[1], m);
        }
        else {
            XMEMCPY(t[1] + 16, a, sizeof(sp_digit) * 16);
            err = sp_2048_mod_16(t[1], t[1], m);
        }
    }

    if (err == MP_OKAY) {
        sp_2048_mont_sqr_16(t[ 2], t[ 1], m, mp);
        sp_2048_mont_mul_16(t[ 3], t[ 2], t[ 1], m, mp);
        sp_2048_mont_sqr_16(t[ 4], t[ 2], m, mp);
        sp_2048_mont_mul_16(t[ 5], t[ 3], t[ 2], m, mp);
        sp_2048_mont_sqr_16(t[ 6], t[ 3], m, mp);
        sp_2048_mont_mul_16(t[ 7], t[ 4], t[ 3], m, mp);
        sp_2048_mont_sqr_16(t[ 8], t[ 4], m, mp);
        sp_2048_mont_mul_16(t[ 9], t[ 5], t[ 4], m, mp);
        sp_2048_mont_sqr_16(t[10], t[ 5], m, mp);
        sp_2048_mont_mul_16(t[11], t[ 6], t[ 5], m, mp);
        sp_2048_mont_sqr_16(t[12], t[ 6], m, mp);
        sp_2048_mont_mul_16(t[13], t[ 7], t[ 6], m, mp);
        sp_2048_mont_sqr_16(t[14], t[ 7], m, mp);
        sp_2048_mont_mul_16(t[15], t[ 8], t[ 7], m, mp);
        sp_2048_mont_sqr_16(t[16], t[ 8], m, mp);
        sp_2048_mont_mul_16(t[17], t[ 9], t[ 8], m, mp);
        sp_2048_mont_sqr_16(t[18], t[ 9], m, mp);
        sp_2048_mont_mul_16(t[19], t[10], t[ 9], m, mp);
        sp_2048_mont_sqr_16(t[20], t[10], m, mp);
        sp_2048_mont_mul_16(t[21], t[11], t[10], m, mp);
        sp_2048_mont_sqr_16(t[22], t[11], m, mp);
        sp_2048_mont_mul_16(t[23], t[12], t[11], m, mp);
        sp_2048_mont_sqr_16(t[24], t[12], m, mp);
        sp_2048_mont_mul_16(t[25], t[13], t[12], m, mp);
        sp_2048_mont_sqr_16(t[26], t[13], m, mp);
        sp_2048_mont_mul_16(t[27], t[14], t[13], m, mp);
        sp_2048_mont_sqr_16(t[28], t[14], m, mp);
        sp_2048_mont_mul_16(t[29], t[15], t[14], m, mp);
        sp_2048_mont_sqr_16(t[30], t[15], m, mp);
        sp_2048_mont_mul_16(t[31], t[16], t[15], m, mp);

        i = (bits - 1) / 64;
        n = e[i--];
        y = n >> 59;
        n <<= 5;
        c = 59;
        XMEMCPY(r, t[y], sizeof(sp_digit) * 16);
        for (; i>=0 || c>=5; ) {
            if (c == 0) {
                n = e[i--];
                y = n >> 59;
                n <<= 5;
                c = 59;
            }
            else if (c < 5) {
                y = n >> 59;
                n = e[i--];
                c = 5 - c;
                y |= n >> (64 - c);
                n <<= c;
                c = 64 - c;
            }
            else {
                y = (n >> 59) & 0x1f;
                n <<= 5;
                c -= 5;
            }

            sp_2048_mont_sqr_16(r, r, m, mp);
            sp_2048_mont_sqr_16(r, r, m, mp);
            sp_2048_mont_sqr_16(r, r, m, mp);
            sp_2048_mont_sqr_16(r, r, m, mp);
            sp_2048_mont_sqr_16(r, r, m, mp);

            sp_2048_mont_mul_16(r, r, t[y], m, mp);
        }
        y = e[0] & ((1 << c) - 1);
        for (; c > 0; c--)
            sp_2048_mont_sqr_16(r, r, m, mp);
        sp_2048_mont_mul_16(r, r, t[y], m, mp);

        XMEMSET(&r[16], 0, sizeof(sp_digit) * 16);
        sp_2048_mont_reduce_16(r, m, mp);

        mask = 0 - (sp_2048_cmp_16(r, m) >= 0);
        sp_2048_cond_sub_16(r, r, m, mask);
    }

#ifdef WOLFSSL_SMALL_STACK
    if (td != NULL)
        XFREE(td, NULL, DYNAMIC_TYPE_TMP_BUFFER);
#endif

    return err;
}

extern void sp_2048_mont_reduce_avx2_16(sp_digit* a, sp_digit* m, sp_digit mp);
#ifdef HAVE_INTEL_AVX2
/* Multiply two Montogmery form numbers mod the modulus (prime).
 * (r = a * b mod m)
 *
 * r   Result of multiplication.
 * a   First number to multiply in Montogmery form.
 * b   Second number to multiply in Montogmery form.
 * m   Modulus (prime).
 * mp  Montogmery mulitplier.
 */
static void sp_2048_mont_mul_avx2_16(sp_digit* r, sp_digit* a, sp_digit* b,
        sp_digit* m, sp_digit mp)
{
    sp_2048_mul_avx2_16(r, a, b);
    sp_2048_mont_reduce_avx2_16(r, m, mp);
}

#endif /* HAVE_INTEL_AVX2 */
#ifdef HAVE_INTEL_AVX2
/* Square the Montgomery form number. (r = a * a mod m)
 *
 * r   Result of squaring.
 * a   Number to square in Montogmery form.
 * m   Modulus (prime).
 * mp  Montogmery mulitplier.
 */
static void sp_2048_mont_sqr_avx2_16(sp_digit* r, sp_digit* a, sp_digit* m,
        sp_digit mp)
{
    sp_2048_sqr_avx2_16(r, a);
    sp_2048_mont_reduce_avx2_16(r, m, mp);
}

#endif /* HAVE_INTEL_AVX2 */
#ifdef HAVE_INTEL_AVX2
/* Modular exponentiate a to the e mod m. (r = a^e mod m)
 *
 * r     A single precision number that is the result of the operation.
 * a     A single precision number being exponentiated.
 * e     A single precision number that is the exponent.
 * bits  The number of bits in the exponent.
 * m     A single precision number that is the modulus.
 * returns 0 on success and MEMORY_E on dynamic memory allocation failure.
 */
static int sp_2048_mod_exp_avx2_16(sp_digit* r, sp_digit* a, sp_digit* e,
        int bits, sp_digit* m, int reduceA)
{
#ifndef WOLFSSL_SMALL_STACK
    sp_digit t[32][32];
#else
    sp_digit* t[32];
    sp_digit* td;
#endif
    sp_digit* norm;
    sp_digit mp = 1;
    sp_digit n;
    sp_digit mask;
    int i;
    int c, y;
    int err = MP_OKAY;

#ifdef WOLFSSL_SMALL_STACK
    td = (sp_digit*)XMALLOC(sizeof(sp_digit) * 32 * 32, NULL,
                            DYNAMIC_TYPE_TMP_BUFFER);
    if (td == NULL)
        err = MEMORY_E;

    if (err == MP_OKAY) {
        for (i=0; i<32; i++)
            t[i] = td + i * 32;
        norm = t[0];
    }
#else
    norm = t[0];
#endif

    if (err == MP_OKAY) {
        sp_2048_mont_setup(m, &mp);
        sp_2048_mont_norm_16(norm, m);

        XMEMSET(t[1], 0, sizeof(sp_digit) * 16);
        if (reduceA) {
            err = sp_2048_mod_16(t[1] + 16, a, m);
            if (err == MP_OKAY)
                err = sp_2048_mod_16(t[1], t[1], m);
        }
        else {
            XMEMCPY(t[1] + 16, a, sizeof(sp_digit) * 16);
            err = sp_2048_mod_16(t[1], t[1], m);
        }
    }

    if (err == MP_OKAY) {
        sp_2048_mont_sqr_avx2_16(t[ 2], t[ 1], m, mp);
        sp_2048_mont_mul_avx2_16(t[ 3], t[ 2], t[ 1], m, mp);
        sp_2048_mont_sqr_avx2_16(t[ 4], t[ 2], m, mp);
        sp_2048_mont_mul_avx2_16(t[ 5], t[ 3], t[ 2], m, mp);
        sp_2048_mont_sqr_avx2_16(t[ 6], t[ 3], m, mp);
        sp_2048_mont_mul_avx2_16(t[ 7], t[ 4], t[ 3], m, mp);
        sp_2048_mont_sqr_avx2_16(t[ 8], t[ 4], m, mp);
        sp_2048_mont_mul_avx2_16(t[ 9], t[ 5], t[ 4], m, mp);
        sp_2048_mont_sqr_avx2_16(t[10], t[ 5], m, mp);
        sp_2048_mont_mul_avx2_16(t[11], t[ 6], t[ 5], m, mp);
        sp_2048_mont_sqr_avx2_16(t[12], t[ 6], m, mp);
        sp_2048_mont_mul_avx2_16(t[13], t[ 7], t[ 6], m, mp);
        sp_2048_mont_sqr_avx2_16(t[14], t[ 7], m, mp);
        sp_2048_mont_mul_avx2_16(t[15], t[ 8], t[ 7], m, mp);
        sp_2048_mont_sqr_avx2_16(t[16], t[ 8], m, mp);
        sp_2048_mont_mul_avx2_16(t[17], t[ 9], t[ 8], m, mp);
        sp_2048_mont_sqr_avx2_16(t[18], t[ 9], m, mp);
        sp_2048_mont_mul_avx2_16(t[19], t[10], t[ 9], m, mp);
        sp_2048_mont_sqr_avx2_16(t[20], t[10], m, mp);
        sp_2048_mont_mul_avx2_16(t[21], t[11], t[10], m, mp);
        sp_2048_mont_sqr_avx2_16(t[22], t[11], m, mp);
        sp_2048_mont_mul_avx2_16(t[23], t[12], t[11], m, mp);
        sp_2048_mont_sqr_avx2_16(t[24], t[12], m, mp);
        sp_2048_mont_mul_avx2_16(t[25], t[13], t[12], m, mp);
        sp_2048_mont_sqr_avx2_16(t[26], t[13], m, mp);
        sp_2048_mont_mul_avx2_16(t[27], t[14], t[13], m, mp);
        sp_2048_mont_sqr_avx2_16(t[28], t[14], m, mp);
        sp_2048_mont_mul_avx2_16(t[29], t[15], t[14], m, mp);
        sp_2048_mont_sqr_avx2_16(t[30], t[15], m, mp);
        sp_2048_mont_mul_avx2_16(t[31], t[16], t[15], m, mp);

        i = (bits - 1) / 64;
        n = e[i--];
        y = n >> 59;
        n <<= 5;
        c = 59;
        XMEMCPY(r, t[y], sizeof(sp_digit) * 16);
        for (; i>=0 || c>=5; ) {
            if (c == 0) {
                n = e[i--];
                y = n >> 59;
                n <<= 5;
                c = 59;
            }
            else if (c < 5) {
                y = n >> 59;
                n = e[i--];
                c = 5 - c;
                y |= n >> (64 - c);
                n <<= c;
                c = 64 - c;
            }
            else {
                y = (n >> 59) & 0x1f;
                n <<= 5;
                c -= 5;
            }

            sp_2048_mont_sqr_avx2_16(r, r, m, mp);
            sp_2048_mont_sqr_avx2_16(r, r, m, mp);
            sp_2048_mont_sqr_avx2_16(r, r, m, mp);
            sp_2048_mont_sqr_avx2_16(r, r, m, mp);
            sp_2048_mont_sqr_avx2_16(r, r, m, mp);

            sp_2048_mont_mul_avx2_16(r, r, t[y], m, mp);
        }
        y = e[0] & ((1 << c) - 1);
        for (; c > 0; c--)
            sp_2048_mont_sqr_avx2_16(r, r, m, mp);
        sp_2048_mont_mul_avx2_16(r, r, t[y], m, mp);

        XMEMSET(&r[16], 0, sizeof(sp_digit) * 16);
        sp_2048_mont_reduce_avx2_16(r, m, mp);

        mask = 0 - (sp_2048_cmp_16(r, m) >= 0);
        sp_2048_cond_sub_16(r, r, m, mask);
    }

#ifdef WOLFSSL_SMALL_STACK
    if (td != NULL)
        XFREE(td, NULL, DYNAMIC_TYPE_TMP_BUFFER);
#endif

    return err;
}
#endif /* HAVE_INTEL_AVX2 */

#endif /* (WOLFSSL_HAVE_SP_RSA || WOLFSSL_HAVE_SP_DH) && !WOLFSSL_RSA_PUBLIC_ONLY */

#ifdef WOLFSSL_HAVE_SP_DH
/* r = 2^n mod m where n is the number of bits to reduce by.
 * Given m must be 2048 bits, just need to subtract.
 *
 * r  A single precision number.
 * m  A signle precision number.
 */
static void sp_2048_mont_norm_32(sp_digit* r, sp_digit* m)
{
    XMEMSET(r, 0, sizeof(sp_digit) * 32);

    /* r = 2^n mod m */
    sp_2048_sub_in_place_32(r, m);
}

#endif /* WOLFSSL_HAVE_SP_DH */
extern sp_digit sp_2048_cond_sub_32(sp_digit* r, sp_digit* a, sp_digit* b, sp_digit m);
extern void sp_2048_mont_reduce_32(sp_digit* a, sp_digit* m, sp_digit mp);
/* Multiply two Montogmery form numbers mod the modulus (prime).
 * (r = a * b mod m)
 *
 * r   Result of multiplication.
 * a   First number to multiply in Montogmery form.
 * b   Second number to multiply in Montogmery form.
 * m   Modulus (prime).
 * mp  Montogmery mulitplier.
 */
static void sp_2048_mont_mul_32(sp_digit* r, sp_digit* a, sp_digit* b,
        sp_digit* m, sp_digit mp)
{
    sp_2048_mul_32(r, a, b);
    sp_2048_mont_reduce_32(r, m, mp);
}

/* Square the Montgomery form number. (r = a * a mod m)
 *
 * r   Result of squaring.
 * a   Number to square in Montogmery form.
 * m   Modulus (prime).
 * mp  Montogmery mulitplier.
 */
static void sp_2048_mont_sqr_32(sp_digit* r, sp_digit* a, sp_digit* m,
        sp_digit mp)
{
    sp_2048_sqr_32(r, a);
    sp_2048_mont_reduce_32(r, m, mp);
}

#ifndef WOLFSSL_RSA_PUBLIC_ONLY
extern void sp_2048_mul_d_avx2_32(sp_digit* r, const sp_digit* a, const sp_digit b);
/* Divide the double width number (d1|d0) by the dividend. (d1|d0 / div)
 *
 * d1   The high order half of the number to divide.
 * d0   The low order half of the number to divide.
 * div  The dividend.
 * returns the result of the division.
 */
static WC_INLINE sp_digit div_2048_word_32(sp_digit d1, sp_digit d0,
        sp_digit div)
{
    register sp_digit r asm("rax");
    __asm__ __volatile__ (
        "divq %3"
        : "=a" (r)
        : "d" (d1), "a" (d0), "r" (div)
        :
    );
    return r;
}
/* AND m into each word of a and store in r.
 *
 * r  A single precision integer.
 * a  A single precision integer.
 * m  Mask to AND against each digit.
 */
static void sp_2048_mask_32(sp_digit* r, sp_digit* a, sp_digit m)
{
#ifdef WOLFSSL_SP_SMALL
    int i;

    for (i=0; i<32; i++)
        r[i] = a[i] & m;
#else
    int i;

    for (i = 0; i < 32; i += 8) {
        r[i+0] = a[i+0] & m;
        r[i+1] = a[i+1] & m;
        r[i+2] = a[i+2] & m;
        r[i+3] = a[i+3] & m;
        r[i+4] = a[i+4] & m;
        r[i+5] = a[i+5] & m;
        r[i+6] = a[i+6] & m;
        r[i+7] = a[i+7] & m;
    }
#endif
}

extern int64_t sp_2048_cmp_32(sp_digit* a, sp_digit* b);
/* Divide d in a and put remainder into r (m*d + r = a)
 * m is not calculated as it is not needed at this time.
 *
 * a  Nmber to be divided.
 * d  Number to divide with.
 * m  Multiplier result.
 * r  Remainder from the division.
 * returns MP_OKAY indicating success.
 */
static WC_INLINE int sp_2048_div_32(sp_digit* a, sp_digit* d, sp_digit* m,
        sp_digit* r)
{
    sp_digit t1[64], t2[33];
    sp_digit div, r1;
    int i;
#ifdef HAVE_INTEL_AVX2
    word32 cpuid_flags = cpuid_get_flags();
#endif

    (void)m;

    div = d[31];
    XMEMCPY(t1, a, sizeof(*t1) * 2 * 32);
    for (i=31; i>=0; i--) {
        r1 = div_2048_word_32(t1[32 + i], t1[32 + i - 1], div);

#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
            sp_2048_mul_d_avx2_32(t2, d, r1);
        else
#endif
            sp_2048_mul_d_32(t2, d, r1);
        t1[32 + i] += sp_2048_sub_in_place_32(&t1[i], t2);
        t1[32 + i] -= t2[32];
        sp_2048_mask_32(t2, d, t1[32 + i]);
        t1[32 + i] += sp_2048_add_32(&t1[i], &t1[i], t2);
        sp_2048_mask_32(t2, d, t1[32 + i]);
        t1[32 + i] += sp_2048_add_32(&t1[i], &t1[i], t2);
    }

    r1 = sp_2048_cmp_32(t1, d) >= 0;
    sp_2048_cond_sub_32(r, t1, d, (sp_digit)0 - r1);

    return MP_OKAY;
}

/* Reduce a modulo m into r. (r = a mod m)
 *
 * r  A single precision number that is the reduced result.
 * a  A single precision number that is to be reduced.
 * m  A single precision number that is the modulus to reduce with.
 * returns MP_OKAY indicating success.
 */
static WC_INLINE int sp_2048_mod_32(sp_digit* r, sp_digit* a, sp_digit* m)
{
    return sp_2048_div_32(a, m, NULL, r);
}

#endif /* WOLFSSL_RSA_PUBLIC_ONLY */
/* Divide d in a and put remainder into r (m*d + r = a)
 * m is not calculated as it is not needed at this time.
 *
 * a  Nmber to be divided.
 * d  Number to divide with.
 * m  Multiplier result.
 * r  Remainder from the division.
 * returns MP_OKAY indicating success.
 */
static WC_INLINE int sp_2048_div_32_cond(sp_digit* a, sp_digit* d, sp_digit* m,
        sp_digit* r)
{
    sp_digit t1[64], t2[33];
    sp_digit div, r1;
    int i;
#ifdef HAVE_INTEL_AVX2
    word32 cpuid_flags = cpuid_get_flags();
#endif

    (void)m;

    div = d[31];
    XMEMCPY(t1, a, sizeof(*t1) * 2 * 32);
    for (i=31; i>=0; i--) {
        r1 = div_2048_word_32(t1[32 + i], t1[32 + i - 1], div);

#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
            sp_2048_mul_d_avx2_32(t2, d, r1);
        else
#endif
            sp_2048_mul_d_32(t2, d, r1);
        t1[32 + i] += sp_2048_sub_in_place_32(&t1[i], t2);
        t1[32 + i] -= t2[32];
        if (t1[32 + i] != 0) {
            t1[32 + i] += sp_2048_add_32(&t1[i], &t1[i], d);
            if (t1[32 + i] != 0)
                t1[32 + i] += sp_2048_add_32(&t1[i], &t1[i], d);
        }
    }

    r1 = sp_2048_cmp_32(t1, d) >= 0;
    sp_2048_cond_sub_32(r, t1, d, (sp_digit)0 - r1);

    return MP_OKAY;
}

/* Reduce a modulo m into r. (r = a mod m)
 *
 * r  A single precision number that is the reduced result.
 * a  A single precision number that is to be reduced.
 * m  A single precision number that is the modulus to reduce with.
 * returns MP_OKAY indicating success.
 */
static WC_INLINE int sp_2048_mod_32_cond(sp_digit* r, sp_digit* a, sp_digit* m)
{
    return sp_2048_div_32_cond(a, m, NULL, r);
}

#if (defined(SP_RSA_PRIVATE_EXP_D) && !defined(WOLFSSL_RSA_PUBLIC_ONLY)) || defined(WOLFSSL_HAVE_SP_DH)
/* Modular exponentiate a to the e mod m. (r = a^e mod m)
 *
 * r     A single precision number that is the result of the operation.
 * a     A single precision number being exponentiated.
 * e     A single precision number that is the exponent.
 * bits  The number of bits in the exponent.
 * m     A single precision number that is the modulus.
 * returns 0 on success and MEMORY_E on dynamic memory allocation failure.
 */
static int sp_2048_mod_exp_32(sp_digit* r, sp_digit* a, sp_digit* e,
        int bits, sp_digit* m, int reduceA)
{
#ifndef WOLFSSL_SMALL_STACK
    sp_digit t[32][64];
#else
    sp_digit* t[32];
    sp_digit* td;
#endif
    sp_digit* norm;
    sp_digit mp = 1;
    sp_digit n;
    sp_digit mask;
    int i;
    int c, y;
    int err = MP_OKAY;

#ifdef WOLFSSL_SMALL_STACK
    td = (sp_digit*)XMALLOC(sizeof(sp_digit) * 32 * 64, NULL,
                            DYNAMIC_TYPE_TMP_BUFFER);
    if (td == NULL)
        err = MEMORY_E;

    if (err == MP_OKAY) {
        for (i=0; i<32; i++)
            t[i] = td + i * 64;
        norm = t[0];
    }
#else
    norm = t[0];
#endif

    if (err == MP_OKAY) {
        sp_2048_mont_setup(m, &mp);
        sp_2048_mont_norm_32(norm, m);

        XMEMSET(t[1], 0, sizeof(sp_digit) * 32);
        if (reduceA) {
            err = sp_2048_mod_32(t[1] + 32, a, m);
            if (err == MP_OKAY)
                err = sp_2048_mod_32(t[1], t[1], m);
        }
        else {
            XMEMCPY(t[1] + 32, a, sizeof(sp_digit) * 32);
            err = sp_2048_mod_32(t[1], t[1], m);
        }
    }

    if (err == MP_OKAY) {
        sp_2048_mont_sqr_32(t[ 2], t[ 1], m, mp);
        sp_2048_mont_mul_32(t[ 3], t[ 2], t[ 1], m, mp);
        sp_2048_mont_sqr_32(t[ 4], t[ 2], m, mp);
        sp_2048_mont_mul_32(t[ 5], t[ 3], t[ 2], m, mp);
        sp_2048_mont_sqr_32(t[ 6], t[ 3], m, mp);
        sp_2048_mont_mul_32(t[ 7], t[ 4], t[ 3], m, mp);
        sp_2048_mont_sqr_32(t[ 8], t[ 4], m, mp);
        sp_2048_mont_mul_32(t[ 9], t[ 5], t[ 4], m, mp);
        sp_2048_mont_sqr_32(t[10], t[ 5], m, mp);
        sp_2048_mont_mul_32(t[11], t[ 6], t[ 5], m, mp);
        sp_2048_mont_sqr_32(t[12], t[ 6], m, mp);
        sp_2048_mont_mul_32(t[13], t[ 7], t[ 6], m, mp);
        sp_2048_mont_sqr_32(t[14], t[ 7], m, mp);
        sp_2048_mont_mul_32(t[15], t[ 8], t[ 7], m, mp);
        sp_2048_mont_sqr_32(t[16], t[ 8], m, mp);
        sp_2048_mont_mul_32(t[17], t[ 9], t[ 8], m, mp);
        sp_2048_mont_sqr_32(t[18], t[ 9], m, mp);
        sp_2048_mont_mul_32(t[19], t[10], t[ 9], m, mp);
        sp_2048_mont_sqr_32(t[20], t[10], m, mp);
        sp_2048_mont_mul_32(t[21], t[11], t[10], m, mp);
        sp_2048_mont_sqr_32(t[22], t[11], m, mp);
        sp_2048_mont_mul_32(t[23], t[12], t[11], m, mp);
        sp_2048_mont_sqr_32(t[24], t[12], m, mp);
        sp_2048_mont_mul_32(t[25], t[13], t[12], m, mp);
        sp_2048_mont_sqr_32(t[26], t[13], m, mp);
        sp_2048_mont_mul_32(t[27], t[14], t[13], m, mp);
        sp_2048_mont_sqr_32(t[28], t[14], m, mp);
        sp_2048_mont_mul_32(t[29], t[15], t[14], m, mp);
        sp_2048_mont_sqr_32(t[30], t[15], m, mp);
        sp_2048_mont_mul_32(t[31], t[16], t[15], m, mp);

        i = (bits - 1) / 64;
        n = e[i--];
        y = n >> 59;
        n <<= 5;
        c = 59;
        XMEMCPY(r, t[y], sizeof(sp_digit) * 32);
        for (; i>=0 || c>=5; ) {
            if (c == 0) {
                n = e[i--];
                y = n >> 59;
                n <<= 5;
                c = 59;
            }
            else if (c < 5) {
                y = n >> 59;
                n = e[i--];
                c = 5 - c;
                y |= n >> (64 - c);
                n <<= c;
                c = 64 - c;
            }
            else {
                y = (n >> 59) & 0x1f;
                n <<= 5;
                c -= 5;
            }

            sp_2048_mont_sqr_32(r, r, m, mp);
            sp_2048_mont_sqr_32(r, r, m, mp);
            sp_2048_mont_sqr_32(r, r, m, mp);
            sp_2048_mont_sqr_32(r, r, m, mp);
            sp_2048_mont_sqr_32(r, r, m, mp);

            sp_2048_mont_mul_32(r, r, t[y], m, mp);
        }
        y = e[0] & ((1 << c) - 1);
        for (; c > 0; c--)
            sp_2048_mont_sqr_32(r, r, m, mp);
        sp_2048_mont_mul_32(r, r, t[y], m, mp);

        XMEMSET(&r[32], 0, sizeof(sp_digit) * 32);
        sp_2048_mont_reduce_32(r, m, mp);

        mask = 0 - (sp_2048_cmp_32(r, m) >= 0);
        sp_2048_cond_sub_32(r, r, m, mask);
    }

#ifdef WOLFSSL_SMALL_STACK
    if (td != NULL)
        XFREE(td, NULL, DYNAMIC_TYPE_TMP_BUFFER);
#endif

    return err;
}
#endif /* (SP_RSA_PRIVATE_EXP_D && !WOLFSSL_RSA_PUBLIC_ONLY) || WOLFSSL_HAVE_SP_DH */

extern void sp_2048_mont_reduce_avx2_32(sp_digit* a, sp_digit* m, sp_digit mp);
#ifdef HAVE_INTEL_AVX2
/* Multiply two Montogmery form numbers mod the modulus (prime).
 * (r = a * b mod m)
 *
 * r   Result of multiplication.
 * a   First number to multiply in Montogmery form.
 * b   Second number to multiply in Montogmery form.
 * m   Modulus (prime).
 * mp  Montogmery mulitplier.
 */
static void sp_2048_mont_mul_avx2_32(sp_digit* r, sp_digit* a, sp_digit* b,
        sp_digit* m, sp_digit mp)
{
    sp_2048_mul_avx2_32(r, a, b);
    sp_2048_mont_reduce_avx2_32(r, m, mp);
}

#endif /* HAVE_INTEL_AVX2 */
#ifdef HAVE_INTEL_AVX2
/* Square the Montgomery form number. (r = a * a mod m)
 *
 * r   Result of squaring.
 * a   Number to square in Montogmery form.
 * m   Modulus (prime).
 * mp  Montogmery mulitplier.
 */
static void sp_2048_mont_sqr_avx2_32(sp_digit* r, sp_digit* a, sp_digit* m,
        sp_digit mp)
{
    sp_2048_sqr_avx2_32(r, a);
    sp_2048_mont_reduce_avx2_32(r, m, mp);
}

#endif /* HAVE_INTEL_AVX2 */
#if (defined(SP_RSA_PRIVATE_EXP_D) && !defined(WOLFSSL_RSA_PUBLIC_ONLY)) || defined(WOLFSSL_HAVE_SP_DH)
#ifdef HAVE_INTEL_AVX2
/* Modular exponentiate a to the e mod m. (r = a^e mod m)
 *
 * r     A single precision number that is the result of the operation.
 * a     A single precision number being exponentiated.
 * e     A single precision number that is the exponent.
 * bits  The number of bits in the exponent.
 * m     A single precision number that is the modulus.
 * returns 0 on success and MEMORY_E on dynamic memory allocation failure.
 */
static int sp_2048_mod_exp_avx2_32(sp_digit* r, sp_digit* a, sp_digit* e,
        int bits, sp_digit* m, int reduceA)
{
#ifndef WOLFSSL_SMALL_STACK
    sp_digit t[32][64];
#else
    sp_digit* t[32];
    sp_digit* td;
#endif
    sp_digit* norm;
    sp_digit mp = 1;
    sp_digit n;
    sp_digit mask;
    int i;
    int c, y;
    int err = MP_OKAY;

#ifdef WOLFSSL_SMALL_STACK
    td = (sp_digit*)XMALLOC(sizeof(sp_digit) * 32 * 64, NULL,
                            DYNAMIC_TYPE_TMP_BUFFER);
    if (td == NULL)
        err = MEMORY_E;

    if (err == MP_OKAY) {
        for (i=0; i<32; i++)
            t[i] = td + i * 64;
        norm = t[0];
    }
#else
    norm = t[0];
#endif

    if (err == MP_OKAY) {
        sp_2048_mont_setup(m, &mp);
        sp_2048_mont_norm_32(norm, m);

        XMEMSET(t[1], 0, sizeof(sp_digit) * 32);
        if (reduceA) {
            err = sp_2048_mod_32(t[1] + 32, a, m);
            if (err == MP_OKAY)
                err = sp_2048_mod_32(t[1], t[1], m);
        }
        else {
            XMEMCPY(t[1] + 32, a, sizeof(sp_digit) * 32);
            err = sp_2048_mod_32(t[1], t[1], m);
        }
    }

    if (err == MP_OKAY) {
        sp_2048_mont_sqr_avx2_32(t[ 2], t[ 1], m, mp);
        sp_2048_mont_mul_avx2_32(t[ 3], t[ 2], t[ 1], m, mp);
        sp_2048_mont_sqr_avx2_32(t[ 4], t[ 2], m, mp);
        sp_2048_mont_mul_avx2_32(t[ 5], t[ 3], t[ 2], m, mp);
        sp_2048_mont_sqr_avx2_32(t[ 6], t[ 3], m, mp);
        sp_2048_mont_mul_avx2_32(t[ 7], t[ 4], t[ 3], m, mp);
        sp_2048_mont_sqr_avx2_32(t[ 8], t[ 4], m, mp);
        sp_2048_mont_mul_avx2_32(t[ 9], t[ 5], t[ 4], m, mp);
        sp_2048_mont_sqr_avx2_32(t[10], t[ 5], m, mp);
        sp_2048_mont_mul_avx2_32(t[11], t[ 6], t[ 5], m, mp);
        sp_2048_mont_sqr_avx2_32(t[12], t[ 6], m, mp);
        sp_2048_mont_mul_avx2_32(t[13], t[ 7], t[ 6], m, mp);
        sp_2048_mont_sqr_avx2_32(t[14], t[ 7], m, mp);
        sp_2048_mont_mul_avx2_32(t[15], t[ 8], t[ 7], m, mp);
        sp_2048_mont_sqr_avx2_32(t[16], t[ 8], m, mp);
        sp_2048_mont_mul_avx2_32(t[17], t[ 9], t[ 8], m, mp);
        sp_2048_mont_sqr_avx2_32(t[18], t[ 9], m, mp);
        sp_2048_mont_mul_avx2_32(t[19], t[10], t[ 9], m, mp);
        sp_2048_mont_sqr_avx2_32(t[20], t[10], m, mp);
        sp_2048_mont_mul_avx2_32(t[21], t[11], t[10], m, mp);
        sp_2048_mont_sqr_avx2_32(t[22], t[11], m, mp);
        sp_2048_mont_mul_avx2_32(t[23], t[12], t[11], m, mp);
        sp_2048_mont_sqr_avx2_32(t[24], t[12], m, mp);
        sp_2048_mont_mul_avx2_32(t[25], t[13], t[12], m, mp);
        sp_2048_mont_sqr_avx2_32(t[26], t[13], m, mp);
        sp_2048_mont_mul_avx2_32(t[27], t[14], t[13], m, mp);
        sp_2048_mont_sqr_avx2_32(t[28], t[14], m, mp);
        sp_2048_mont_mul_avx2_32(t[29], t[15], t[14], m, mp);
        sp_2048_mont_sqr_avx2_32(t[30], t[15], m, mp);
        sp_2048_mont_mul_avx2_32(t[31], t[16], t[15], m, mp);

        i = (bits - 1) / 64;
        n = e[i--];
        y = n >> 59;
        n <<= 5;
        c = 59;
        XMEMCPY(r, t[y], sizeof(sp_digit) * 32);
        for (; i>=0 || c>=5; ) {
            if (c == 0) {
                n = e[i--];
                y = n >> 59;
                n <<= 5;
                c = 59;
            }
            else if (c < 5) {
                y = n >> 59;
                n = e[i--];
                c = 5 - c;
                y |= n >> (64 - c);
                n <<= c;
                c = 64 - c;
            }
            else {
                y = (n >> 59) & 0x1f;
                n <<= 5;
                c -= 5;
            }

            sp_2048_mont_sqr_avx2_32(r, r, m, mp);
            sp_2048_mont_sqr_avx2_32(r, r, m, mp);
            sp_2048_mont_sqr_avx2_32(r, r, m, mp);
            sp_2048_mont_sqr_avx2_32(r, r, m, mp);
            sp_2048_mont_sqr_avx2_32(r, r, m, mp);

            sp_2048_mont_mul_avx2_32(r, r, t[y], m, mp);
        }
        y = e[0] & ((1 << c) - 1);
        for (; c > 0; c--)
            sp_2048_mont_sqr_avx2_32(r, r, m, mp);
        sp_2048_mont_mul_avx2_32(r, r, t[y], m, mp);

        XMEMSET(&r[32], 0, sizeof(sp_digit) * 32);
        sp_2048_mont_reduce_avx2_32(r, m, mp);

        mask = 0 - (sp_2048_cmp_32(r, m) >= 0);
        sp_2048_cond_sub_32(r, r, m, mask);
    }

#ifdef WOLFSSL_SMALL_STACK
    if (td != NULL)
        XFREE(td, NULL, DYNAMIC_TYPE_TMP_BUFFER);
#endif

    return err;
}
#endif /* HAVE_INTEL_AVX2 */
#endif /* (SP_RSA_PRIVATE_EXP_D && !WOLFSSL_RSA_PUBLIC_ONLY) || WOLFSSL_HAVE_SP_DH */

#ifdef WOLFSSL_HAVE_SP_RSA
/* RSA public key operation.
 *
 * in      Array of bytes representing the number to exponentiate, base.
 * inLen   Number of bytes in base.
 * em      Public exponent.
 * mm      Modulus.
 * out     Buffer to hold big-endian bytes of exponentiation result.
 *         Must be at least 256 bytes long.
 * outLen  Number of bytes in result.
 * returns 0 on success, MP_TO_E when the outLen is too small, MP_READ_E when
 * an array is too long and MEMORY_E when dynamic memory allocation fails.
 */
int sp_RsaPublic_2048(const byte* in, word32 inLen, mp_int* em, mp_int* mm,
    byte* out, word32* outLen)
{
#if !defined(WOLFSSL_SP_SMALL) && !defined(WOLFSSL_SMALL_STACK)
    sp_digit ad[64], md[32], rd[64];
#else
    sp_digit* d = NULL;
#endif
    sp_digit* a;
    sp_digit *ah;
    sp_digit* m;
    sp_digit* r;
    sp_digit e[1];
    int err = MP_OKAY;
#ifdef HAVE_INTEL_AVX2
    word32 cpuid_flags = cpuid_get_flags();
#endif

    if (*outLen < 256)
        err = MP_TO_E;
    if (err == MP_OKAY && (mp_count_bits(em) > 64 || inLen > 256 ||
                                                     mp_count_bits(mm) != 2048))
        err = MP_READ_E;

#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (err == MP_OKAY) {
        d = (sp_digit*)XMALLOC(sizeof(sp_digit) * 32 * 5, NULL,
                                                              DYNAMIC_TYPE_RSA);
        if (d == NULL)
            err = MEMORY_E;
    }

    if (err == MP_OKAY) {
        a = d;
        r = a + 32 * 2;
        m = r + 32 * 2;
        ah = a + 32;
    }
#else
    a = ad;
    m = md;
    r = rd;
    ah = a + 32;
#endif

    if (err == MP_OKAY) {
        sp_2048_from_bin(ah, 32, in, inLen);
#if DIGIT_BIT >= 64
        e[0] = em->dp[0];
#else
        e[0] = em->dp[0];
        if (em->used > 1)
            e[0] |= ((sp_digit)em->dp[1]) << DIGIT_BIT;
#endif
        if (e[0] == 0)
            err = MP_EXPTMOD_E;
    }
    if (err == MP_OKAY) {
        sp_2048_from_mp(m, 32, mm);

        if (e[0] == 0x3) {
#ifdef HAVE_INTEL_AVX2
            if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags)) {
                if (err == MP_OKAY) {
                    sp_2048_sqr_avx2_32(r, ah);
                    err = sp_2048_mod_32_cond(r, r, m);
                }
                if (err == MP_OKAY) {
                    sp_2048_mul_avx2_32(r, ah, r);
                    err = sp_2048_mod_32_cond(r, r, m);
                }
            }
            else
#endif
            {
                if (err == MP_OKAY) {
                    sp_2048_sqr_32(r, ah);
                    err = sp_2048_mod_32_cond(r, r, m);
                }
                if (err == MP_OKAY) {
                    sp_2048_mul_32(r, ah, r);
                    err = sp_2048_mod_32_cond(r, r, m);
                }
            }
        }
        else {
            int i;
            sp_digit mp;

            sp_2048_mont_setup(m, &mp);

            /* Convert to Montgomery form. */
            XMEMSET(a, 0, sizeof(sp_digit) * 32);
            err = sp_2048_mod_32_cond(a, a, m);

            if (err == MP_OKAY) {
                for (i=63; i>=0; i--)
                    if (e[0] >> i)
                        break;

                XMEMCPY(r, a, sizeof(sp_digit) * 32);
#ifdef HAVE_INTEL_AVX2
                if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags)) {
                    for (i--; i>=0; i--) {
                        sp_2048_mont_sqr_avx2_32(r, r, m, mp);
                        if (((e[0] >> i) & 1) == 1)
                            sp_2048_mont_mul_avx2_32(r, r, a, m, mp);
                    }
                    XMEMSET(&r[32], 0, sizeof(sp_digit) * 32);
                    sp_2048_mont_reduce_avx2_32(r, m, mp);
                }
                else
#endif
                {
                    for (i--; i>=0; i--) {
                        sp_2048_mont_sqr_32(r, r, m, mp);
                        if (((e[0] >> i) & 1) == 1)
                            sp_2048_mont_mul_32(r, r, a, m, mp);
                    }
                    XMEMSET(&r[32], 0, sizeof(sp_digit) * 32);
                    sp_2048_mont_reduce_32(r, m, mp);
                }

                for (i = 31; i > 0; i--) {
                    if (r[i] != m[i])
                        break;
                }
                if (r[i] >= m[i])
                    sp_2048_sub_in_place_32(r, m);
            }
        }
    }

    if (err == MP_OKAY) {
        sp_2048_to_bin(r, out);
        *outLen = 256;
    }

#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (d != NULL)
        XFREE(d, NULL, DYNAMIC_TYPE_RSA);
#endif

    return err;
}

/* RSA private key operation.
 *
 * in      Array of bytes representing the number to exponentiate, base.
 * inLen   Number of bytes in base.
 * dm      Private exponent.
 * pm      First prime.
 * qm      Second prime.
 * dpm     First prime's CRT exponent.
 * dqm     Second prime's CRT exponent.
 * qim     Inverse of second prime mod p.
 * mm      Modulus.
 * out     Buffer to hold big-endian bytes of exponentiation result.
 *         Must be at least 256 bytes long.
 * outLen  Number of bytes in result.
 * returns 0 on success, MP_TO_E when the outLen is too small, MP_READ_E when
 * an array is too long and MEMORY_E when dynamic memory allocation fails.
 */
int sp_RsaPrivate_2048(const byte* in, word32 inLen, mp_int* dm,
    mp_int* pm, mp_int* qm, mp_int* dpm, mp_int* dqm, mp_int* qim, mp_int* mm,
    byte* out, word32* outLen)
{
#if !defined(WOLFSSL_SP_SMALL) && !defined(WOLFSSL_SMALL_STACK)
    sp_digit ad[32 * 2];
    sp_digit pd[16], qd[16], dpd[16];
    sp_digit tmpad[32], tmpbd[32];
#else
    sp_digit* t = NULL;
#endif
    sp_digit* a;
    sp_digit* p;
    sp_digit* q;
    sp_digit* dp;
    sp_digit* dq;
    sp_digit* qi;
    sp_digit* tmp;
    sp_digit* tmpa;
    sp_digit* tmpb;
    sp_digit* r;
    sp_digit c;
    int err = MP_OKAY;
#ifdef HAVE_INTEL_AVX2
    word32 cpuid_flags = cpuid_get_flags();
#endif

    (void)dm;
    (void)mm;

    if (*outLen < 256)
        err = MP_TO_E;
    if (err == MP_OKAY && (inLen > 256 || mp_count_bits(mm) != 2048))
        err = MP_READ_E;

#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (err == MP_OKAY) {
        t = (sp_digit*)XMALLOC(sizeof(sp_digit) * 16 * 11, NULL,
                                                              DYNAMIC_TYPE_RSA);
        if (t == NULL)
            err = MEMORY_E;
    }
    if (err == MP_OKAY) {
        a = t;
        p = a + 32 * 2;
        q = p + 16;
        qi = dq = dp = q + 16;
        tmpa = qi + 16;
        tmpb = tmpa + 32;

        tmp = t;
        r = tmp + 32;
    }
#else
    r = a = ad;
    p = pd;
    q = qd;
    qi = dq = dp = dpd;
    tmpa = tmpad;
    tmpb = tmpbd;
    tmp = a + 32;
#endif

    if (err == MP_OKAY) {
        sp_2048_from_bin(a, 32, in, inLen);
        sp_2048_from_mp(p, 16, pm);
        sp_2048_from_mp(q, 16, qm);
        sp_2048_from_mp(dp, 16, dpm);

#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
            err = sp_2048_mod_exp_avx2_16(tmpa, a, dp, 1024, p, 1);
        else
#endif
            err = sp_2048_mod_exp_16(tmpa, a, dp, 1024, p, 1);
    }
    if (err == MP_OKAY) {
        sp_2048_from_mp(dq, 16, dqm);
#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
            err = sp_2048_mod_exp_avx2_16(tmpb, a, dq, 1024, q, 1);
       else
#endif
            err = sp_2048_mod_exp_16(tmpb, a, dq, 1024, q, 1);
    }

    if (err == MP_OKAY) {
        c = sp_2048_sub_in_place_16(tmpa, tmpb);
        sp_2048_mask_16(tmp, p, c);
        sp_2048_add_16(tmpa, tmpa, tmp);

        sp_2048_from_mp(qi, 16, qim);
#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
            sp_2048_mul_avx2_16(tmpa, tmpa, qi);
        else
#endif
            sp_2048_mul_16(tmpa, tmpa, qi);
        err = sp_2048_mod_16(tmpa, tmpa, p);
    }

    if (err == MP_OKAY) {
#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
            sp_2048_mul_avx2_16(tmpa, q, tmpa);
        else
#endif
            sp_2048_mul_16(tmpa, q, tmpa);
        XMEMSET(&tmpb[16], 0, sizeof(sp_digit) * 16);
        sp_2048_add_32(r, tmpb, tmpa);

        sp_2048_to_bin(r, out);
        *outLen = 256;
    }

#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (t != NULL) {
        XMEMSET(t, 0, sizeof(sp_digit) * 16 * 11);
        XFREE(t, NULL, DYNAMIC_TYPE_RSA);
    }
#else
    XMEMSET(tmpad, 0, sizeof(tmpad));
    XMEMSET(tmpbd, 0, sizeof(tmpbd));
    XMEMSET(pd, 0, sizeof(pd));
    XMEMSET(qd, 0, sizeof(qd));
    XMEMSET(dpd, 0, sizeof(dpd));
#endif

    return err;
}
#endif /* WOLFSSL_HAVE_SP_RSA */
#if defined(WOLFSSL_HAVE_SP_DH) || (defined(WOLFSSL_HAVE_SP_RSA) && \
                                              !defined(WOLFSSL_RSA_PUBLIC_ONLY))
/* Convert an array of sp_digit to an mp_int.
 *
 * a  A single precision integer.
 * r  A multi-precision integer.
 */
static int sp_2048_to_mp(sp_digit* a, mp_int* r)
{
    int err;

    err = mp_grow(r, (2048 + DIGIT_BIT - 1) / DIGIT_BIT);
    if (err == MP_OKAY) {
#if DIGIT_BIT == 64
        XMEMCPY(r->dp, a, sizeof(sp_digit) * 32);
        r->used = 32;
        mp_clamp(r);
#elif DIGIT_BIT < 64
        int i, j = 0, s = 0;

        r->dp[0] = 0;
        for (i = 0; i < 32; i++) {
            r->dp[j] |= a[i] << s;
            r->dp[j] &= (1l << DIGIT_BIT) - 1;
            s = DIGIT_BIT - s;
            r->dp[++j] = a[i] >> s;
            while (s + DIGIT_BIT <= 64) {
                s += DIGIT_BIT;
                r->dp[j] &= (1l << DIGIT_BIT) - 1;
                r->dp[++j] = a[i] >> s;
            }
            s = 64 - s;
        }
        r->used = (2048 + DIGIT_BIT - 1) / DIGIT_BIT;
        mp_clamp(r);
#else
        int i, j = 0, s = 0;

        r->dp[0] = 0;
        for (i = 0; i < 32; i++) {
            r->dp[j] |= ((mp_digit)a[i]) << s;
            if (s + 64 >= DIGIT_BIT) {
    #if DIGIT_BIT < 64
                r->dp[j] &= (1l << DIGIT_BIT) - 1;
    #endif
                s = DIGIT_BIT - s;
                r->dp[++j] = a[i] >> s;
                s = 64 - s;
            }
            else
                s += 64;
        }
        r->used = (2048 + DIGIT_BIT - 1) / DIGIT_BIT;
        mp_clamp(r);
#endif
    }

    return err;
}

/* Perform the modular exponentiation for Diffie-Hellman.
 *
 * base  Base. MP integer.
 * exp   Exponent. MP integer.
 * mod   Modulus. MP integer.
 * res   Result. MP integer.
 * returs 0 on success, MP_READ_E if there are too many bytes in an array
 * and MEMORY_E if memory allocation fails.
 */
int sp_ModExp_2048(mp_int* base, mp_int* exp, mp_int* mod, mp_int* res)
{
    int err = MP_OKAY;
    sp_digit b[64], e[32], m[32];
    sp_digit* r = b;
#ifdef HAVE_INTEL_AVX2
    word32 cpuid_flags = cpuid_get_flags();
#endif
    int expBits = mp_count_bits(exp);

    if (mp_count_bits(base) > 2048 || expBits > 2048 ||
                                                   mp_count_bits(mod) != 2048) {
        err = MP_READ_E;
    }

    if (err == MP_OKAY) {
        sp_2048_from_mp(b, 32, base);
        sp_2048_from_mp(e, 32, exp);
        sp_2048_from_mp(m, 32, mod);

#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
            err = sp_2048_mod_exp_avx2_32(r, b, e, expBits, m, 0);
        else
#endif
            err = sp_2048_mod_exp_32(r, b, e, expBits, m, 0);
    }

    if (err == MP_OKAY) {
        err = sp_2048_to_mp(r, res);
    }

    XMEMSET(e, 0, sizeof(e));

    return err;
}

/* Perform the modular exponentiation for Diffie-Hellman.
 *
 * base     Base.
 * exp      Array of bytes that is the exponent.
 * expLen   Length of data, in bytes, in exponent.
 * mod      Modulus.
 * out      Buffer to hold big-endian bytes of exponentiation result.
 *          Must be at least 256 bytes long.
 * outLen   Length, in bytes, of exponentiation result.
 * returs 0 on success, MP_READ_E if there are too many bytes in an array
 * and MEMORY_E if memory allocation fails.
 */
int sp_DhExp_2048(mp_int* base, const byte* exp, word32 expLen,
    mp_int* mod, byte* out, word32* outLen)
{
    int err = MP_OKAY;
    sp_digit b[64], e[32], m[32];
    sp_digit* r = b;
    word32 i;
#ifdef HAVE_INTEL_AVX2
    word32 cpuid_flags = cpuid_get_flags();
#endif

    if (mp_count_bits(base) > 2048 || expLen > 256 ||
                                                   mp_count_bits(mod) != 2048) {
        err = MP_READ_E;
    }

    if (err == MP_OKAY) {
        sp_2048_from_mp(b, 32, base);
        sp_2048_from_bin(e, 32, exp, expLen);
        sp_2048_from_mp(m, 32, mod);

#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
            err = sp_2048_mod_exp_avx2_32(r, b, e, expLen * 8, m, 0);
        else
#endif
            err = sp_2048_mod_exp_32(r, b, e, expLen * 8, m, 0);
    }

    if (err == MP_OKAY) {
        sp_2048_to_bin(r, out);
        *outLen = 256;
        for (i=0; i<256 && out[i] == 0; i++) {
        }
        *outLen -= i;
        XMEMMOVE(out, out + i, *outLen);
    }

    XMEMSET(e, 0, sizeof(e));

    return err;
}
/* Perform the modular exponentiation for Diffie-Hellman.
 *
 * base  Base. MP integer.
 * exp   Exponent. MP integer.
 * mod   Modulus. MP integer.
 * res   Result. MP integer.
 * returs 0 on success, MP_READ_E if there are too many bytes in an array
 * and MEMORY_E if memory allocation fails.
 */
int sp_ModExp_1024(mp_int* base, mp_int* exp, mp_int* mod, mp_int* res)
{
    int err = MP_OKAY;
    sp_digit b[32], e[16], m[16];
    sp_digit* r = b;
#ifdef HAVE_INTEL_AVX2
    word32 cpuid_flags = cpuid_get_flags();
#endif
    int expBits = mp_count_bits(exp);

    if (mp_count_bits(base) > 1024 || expBits > 1024 ||
                                                   mp_count_bits(mod) != 1024) {
        err = MP_READ_E;
    }

    if (err == MP_OKAY) {
        sp_2048_from_mp(b, 16, base);
        sp_2048_from_mp(e, 16, exp);
        sp_2048_from_mp(m, 16, mod);

#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
            err = sp_2048_mod_exp_avx2_16(r, b, e, expBits, m, 0);
        else
#endif
            err = sp_2048_mod_exp_16(r, b, e, expBits, m, 0);
    }

    if (err == MP_OKAY) {
        XMEMSET(r + 16, 0, sizeof(*r) * 16);
        err = sp_2048_to_mp(r, res);
    }

    XMEMSET(e, 0, sizeof(e));

    return err;
}

#endif /* WOLFSSL_HAVE_SP_DH || (WOLFSSL_HAVE_SP_RSA && !WOLFSSL_RSA_PUBLIC_ONLY) */

#endif /* !WOLFSSL_SP_NO_2048 */

#ifndef WOLFSSL_SP_NO_3072
/* Read big endian unsigned byte aray into r.
 *
 * r  A single precision integer.
 * a  Byte array.
 * n  Number of bytes in array to read.
 */
static void sp_3072_from_bin(sp_digit* r, int max, const byte* a, int n)
{
    int i, j = 0, s = 0;

    r[0] = 0;
    for (i = n-1; i >= 0; i--) {
        r[j] |= ((sp_digit)a[i]) << s;
        if (s >= 56) {
            r[j] &= 0xffffffffffffffffl;
            s = 64 - s;
            if (j + 1 >= max)
                break;
            r[++j] = a[i] >> s;
            s = 8 - s;
        }
        else
            s += 8;
    }

    for (j++; j < max; j++)
        r[j] = 0;
}

/* Convert an mp_int to an array of sp_digit.
 *
 * r  A single precision integer.
 * a  A multi-precision integer.
 */
static void sp_3072_from_mp(sp_digit* r, int max, mp_int* a)
{
#if DIGIT_BIT == 64
    int j;

    XMEMCPY(r, a->dp, sizeof(sp_digit) * a->used);

    for (j = a->used; j < max; j++)
        r[j] = 0;
#elif DIGIT_BIT > 64
    int i, j = 0, s = 0;

    r[0] = 0;
    for (i = 0; i < a->used && j < max; i++) {
        r[j] |= a->dp[i] << s;
        r[j] &= 0xffffffffffffffffl;
        s = 64 - s;
        if (j + 1 >= max)
            break;
        r[++j] = a->dp[i] >> s;
        while (s + 64 <= DIGIT_BIT) {
            s += 64;
            r[j] &= 0xffffffffffffffffl;
            if (j + 1 >= max)
                break;
            if (s < DIGIT_BIT)
                r[++j] = a->dp[i] >> s;
            else
                r[++j] = 0;
        }
        s = DIGIT_BIT - s;
    }

    for (j++; j < max; j++)
        r[j] = 0;
#else
    int i, j = 0, s = 0;

    r[0] = 0;
    for (i = 0; i < a->used && j < max; i++) {
        r[j] |= ((sp_digit)a->dp[i]) << s;
        if (s + DIGIT_BIT >= 64) {
            r[j] &= 0xffffffffffffffffl;
            if (j + 1 >= max)
                break;
            s = 64 - s;
            if (s == DIGIT_BIT) {
                r[++j] = 0;
                s = 0;
            }
            else {
                r[++j] = a->dp[i] >> s;
                s = DIGIT_BIT - s;
            }
        }
        else
            s += DIGIT_BIT;
    }

    for (j++; j < max; j++)
        r[j] = 0;
#endif
}

/* Write r as big endian to byte aray.
 * Fixed length number of bytes written: 384
 *
 * r  A single precision integer.
 * a  Byte array.
 */
static void sp_3072_to_bin(sp_digit* r, byte* a)
{
    int i, j, s = 0, b;

    j = 3072 / 8 - 1;
    a[j] = 0;
    for (i=0; i<48 && j>=0; i++) {
        b = 0;
        a[j--] |= r[i] << s; b += 8 - s;
        if (j < 0)
            break;
        while (b < 64) {
            a[j--] = r[i] >> b; b += 8;
            if (j < 0)
                break;
        }
        s = 8 - (b - 64);
        if (j >= 0)
            a[j] = 0;
        if (s != 0)
            j++;
    }
}

extern void sp_3072_mul_24(sp_digit* r, const sp_digit* a, const sp_digit* b);
extern void sp_3072_sqr_24(sp_digit* r, const sp_digit* a);
extern void sp_3072_mul_avx2_24(sp_digit* r, const sp_digit* a, const sp_digit* b);
extern void sp_3072_sqr_avx2_24(sp_digit* r, const sp_digit* a);
extern sp_digit sp_3072_add_24(sp_digit* r, const sp_digit* a, const sp_digit* b);
extern sp_digit sp_3072_sub_in_place_48(sp_digit* a, const sp_digit* b);
extern sp_digit sp_3072_add_48(sp_digit* r, const sp_digit* a, const sp_digit* b);
/* AND m into each word of a and store in r.
 *
 * r  A single precision integer.
 * a  A single precision integer.
 * m  Mask to AND against each digit.
 */
static void sp_3072_mask_24(sp_digit* r, sp_digit* a, sp_digit m)
{
#ifdef WOLFSSL_SP_SMALL
    int i;

    for (i=0; i<24; i++)
        r[i] = a[i] & m;
#else
    int i;

    for (i = 0; i < 24; i += 8) {
        r[i+0] = a[i+0] & m;
        r[i+1] = a[i+1] & m;
        r[i+2] = a[i+2] & m;
        r[i+3] = a[i+3] & m;
        r[i+4] = a[i+4] & m;
        r[i+5] = a[i+5] & m;
        r[i+6] = a[i+6] & m;
        r[i+7] = a[i+7] & m;
    }
#endif
}

/* Multiply a and b into r. (r = a * b)
 *
 * r  A single precision integer.
 * a  A single precision integer.
 * b  A single precision integer.
 */
SP_NOINLINE static void sp_3072_mul_48(sp_digit* r, const sp_digit* a,
        const sp_digit* b)
{
    sp_digit* z0 = r;
    sp_digit z1[48];
    sp_digit a1[24];
    sp_digit b1[24];
    sp_digit z2[48];
    sp_digit u, ca, cb;

    ca = sp_3072_add_24(a1, a, &a[24]);
    cb = sp_3072_add_24(b1, b, &b[24]);
    u  = ca & cb;
    sp_3072_mul_24(z1, a1, b1);
    sp_3072_mul_24(z2, &a[24], &b[24]);
    sp_3072_mul_24(z0, a, b);
    sp_3072_mask_24(r + 48, a1, 0 - cb);
    sp_3072_mask_24(b1, b1, 0 - ca);
    u += sp_3072_add_24(r + 48, r + 48, b1);
    u += sp_3072_sub_in_place_48(z1, z2);
    u += sp_3072_sub_in_place_48(z1, z0);
    u += sp_3072_add_48(r + 24, r + 24, z1);
    r[72] = u;
    XMEMSET(r + 72 + 1, 0, sizeof(sp_digit) * (24 - 1));
    sp_3072_add_48(r + 48, r + 48, z2);
}

/* Square a and put result in r. (r = a * a)
 *
 * r  A single precision integer.
 * a  A single precision integer.
 */
SP_NOINLINE static void sp_3072_sqr_48(sp_digit* r, const sp_digit* a)
{
    sp_digit* z0 = r;
    sp_digit z2[48];
    sp_digit z1[48];
    sp_digit a1[24];
    sp_digit u;

    u = sp_3072_add_24(a1, a, &a[24]);
    sp_3072_sqr_24(z1, a1);
    sp_3072_sqr_24(z2, &a[24]);
    sp_3072_sqr_24(z0, a);
    sp_3072_mask_24(r + 48, a1, 0 - u);
    u += sp_3072_add_24(r + 48, r + 48, r + 48);
    u += sp_3072_sub_in_place_48(z1, z2);
    u += sp_3072_sub_in_place_48(z1, z0);
    u += sp_3072_add_48(r + 24, r + 24, z1);
    r[72] = u;
    XMEMSET(r + 72 + 1, 0, sizeof(sp_digit) * (24 - 1));
    sp_3072_add_48(r + 48, r + 48, z2);
}

#ifdef HAVE_INTEL_AVX2
/* Multiply a and b into r. (r = a * b)
 *
 * r  A single precision integer.
 * a  A single precision integer.
 * b  A single precision integer.
 */
SP_NOINLINE static void sp_3072_mul_avx2_48(sp_digit* r, const sp_digit* a,
        const sp_digit* b)
{
    sp_digit* z0 = r;
    sp_digit z1[48];
    sp_digit a1[24];
    sp_digit b1[24];
    sp_digit z2[48];
    sp_digit u, ca, cb;

    ca = sp_3072_add_24(a1, a, &a[24]);
    cb = sp_3072_add_24(b1, b, &b[24]);
    u  = ca & cb;
    sp_3072_mul_avx2_24(z1, a1, b1);
    sp_3072_mul_avx2_24(z2, &a[24], &b[24]);
    sp_3072_mul_avx2_24(z0, a, b);
    sp_3072_mask_24(r + 48, a1, 0 - cb);
    sp_3072_mask_24(b1, b1, 0 - ca);
    u += sp_3072_add_24(r + 48, r + 48, b1);
    u += sp_3072_sub_in_place_48(z1, z2);
    u += sp_3072_sub_in_place_48(z1, z0);
    u += sp_3072_add_48(r + 24, r + 24, z1);
    r[72] = u;
    XMEMSET(r + 72 + 1, 0, sizeof(sp_digit) * (24 - 1));
    sp_3072_add_48(r + 48, r + 48, z2);
}
#endif /* HAVE_INTEL_AVX2 */

#ifdef HAVE_INTEL_AVX2
/* Square a and put result in r. (r = a * a)
 *
 * r  A single precision integer.
 * a  A single precision integer.
 */
SP_NOINLINE static void sp_3072_sqr_avx2_48(sp_digit* r, const sp_digit* a)
{
    sp_digit* z0 = r;
    sp_digit z2[48];
    sp_digit z1[48];
    sp_digit a1[24];
    sp_digit u;

    u = sp_3072_add_24(a1, a, &a[24]);
    sp_3072_sqr_avx2_24(z1, a1);
    sp_3072_sqr_avx2_24(z2, &a[24]);
    sp_3072_sqr_avx2_24(z0, a);
    sp_3072_mask_24(r + 48, a1, 0 - u);
    u += sp_3072_add_24(r + 48, r + 48, r + 48);
    u += sp_3072_sub_in_place_48(z1, z2);
    u += sp_3072_sub_in_place_48(z1, z0);
    u += sp_3072_add_48(r + 24, r + 24, z1);
    r[72] = u;
    XMEMSET(r + 72 + 1, 0, sizeof(sp_digit) * (24 - 1));
    sp_3072_add_48(r + 48, r + 48, z2);
}
#endif /* HAVE_INTEL_AVX2 */

#if (defined(WOLFSSL_HAVE_SP_RSA) || defined(WOLFSSL_HAVE_SP_DH)) && !defined(WOLFSSL_RSA_PUBLIC_ONLY)
#endif /* (WOLFSSL_HAVE_SP_RSA || WOLFSSL_HAVE_SP_DH) && !WOLFSSL_RSA_PUBLIC_ONLY */

/* Caclulate the bottom digit of -1/a mod 2^n.
 *
 * a    A single precision number.
 * rho  Bottom word of inverse.
 */
static void sp_3072_mont_setup(sp_digit* a, sp_digit* rho)
{
    sp_digit x, b;

    b = a[0];
    x = (((b + 2) & 4) << 1) + b; /* here x*a==1 mod 2**4 */
    x *= 2 - b * x;               /* here x*a==1 mod 2**8 */
    x *= 2 - b * x;               /* here x*a==1 mod 2**16 */
    x *= 2 - b * x;               /* here x*a==1 mod 2**32 */
    x *= 2 - b * x;               /* here x*a==1 mod 2**64 */

    /* rho = -1/m mod b */
    *rho = -x;
}

extern void sp_3072_mul_d_48(sp_digit* r, const sp_digit* a, const sp_digit b);
#if (defined(WOLFSSL_HAVE_SP_RSA) || defined(WOLFSSL_HAVE_SP_DH)) && !defined(WOLFSSL_RSA_PUBLIC_ONLY)
extern sp_digit sp_3072_sub_in_place_24(sp_digit* a, const sp_digit* b);
/* r = 2^n mod m where n is the number of bits to reduce by.
 * Given m must be 3072 bits, just need to subtract.
 *
 * r  A single precision number.
 * m  A signle precision number.
 */
static void sp_3072_mont_norm_24(sp_digit* r, sp_digit* m)
{
    XMEMSET(r, 0, sizeof(sp_digit) * 24);

    /* r = 2^n mod m */
    sp_3072_sub_in_place_24(r, m);
}

extern sp_digit sp_3072_cond_sub_24(sp_digit* r, sp_digit* a, sp_digit* b, sp_digit m);
extern void sp_3072_mont_reduce_24(sp_digit* a, sp_digit* m, sp_digit mp);
/* Multiply two Montogmery form numbers mod the modulus (prime).
 * (r = a * b mod m)
 *
 * r   Result of multiplication.
 * a   First number to multiply in Montogmery form.
 * b   Second number to multiply in Montogmery form.
 * m   Modulus (prime).
 * mp  Montogmery mulitplier.
 */
static void sp_3072_mont_mul_24(sp_digit* r, sp_digit* a, sp_digit* b,
        sp_digit* m, sp_digit mp)
{
    sp_3072_mul_24(r, a, b);
    sp_3072_mont_reduce_24(r, m, mp);
}

/* Square the Montgomery form number. (r = a * a mod m)
 *
 * r   Result of squaring.
 * a   Number to square in Montogmery form.
 * m   Modulus (prime).
 * mp  Montogmery mulitplier.
 */
static void sp_3072_mont_sqr_24(sp_digit* r, sp_digit* a, sp_digit* m,
        sp_digit mp)
{
    sp_3072_sqr_24(r, a);
    sp_3072_mont_reduce_24(r, m, mp);
}

extern void sp_3072_mul_d_24(sp_digit* r, const sp_digit* a, const sp_digit b);
extern void sp_3072_mul_d_avx2_24(sp_digit* r, const sp_digit* a, const sp_digit b);
/* Divide the double width number (d1|d0) by the dividend. (d1|d0 / div)
 *
 * d1   The high order half of the number to divide.
 * d0   The low order half of the number to divide.
 * div  The dividend.
 * returns the result of the division.
 */
static WC_INLINE sp_digit div_3072_word_24(sp_digit d1, sp_digit d0,
        sp_digit div)
{
    register sp_digit r asm("rax");
    __asm__ __volatile__ (
        "divq %3"
        : "=a" (r)
        : "d" (d1), "a" (d0), "r" (div)
        :
    );
    return r;
}
extern int64_t sp_3072_cmp_24(sp_digit* a, sp_digit* b);
/* Divide d in a and put remainder into r (m*d + r = a)
 * m is not calculated as it is not needed at this time.
 *
 * a  Nmber to be divided.
 * d  Number to divide with.
 * m  Multiplier result.
 * r  Remainder from the division.
 * returns MP_OKAY indicating success.
 */
static WC_INLINE int sp_3072_div_24(sp_digit* a, sp_digit* d, sp_digit* m,
        sp_digit* r)
{
    sp_digit t1[48], t2[25];
    sp_digit div, r1;
    int i;
#ifdef HAVE_INTEL_AVX2
    word32 cpuid_flags = cpuid_get_flags();
#endif

    (void)m;

    div = d[23];
    XMEMCPY(t1, a, sizeof(*t1) * 2 * 24);
    for (i=23; i>=0; i--) {
        r1 = div_3072_word_24(t1[24 + i], t1[24 + i - 1], div);

#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
            sp_3072_mul_d_avx2_24(t2, d, r1);
        else
#endif
            sp_3072_mul_d_24(t2, d, r1);
        t1[24 + i] += sp_3072_sub_in_place_24(&t1[i], t2);
        t1[24 + i] -= t2[24];
        sp_3072_mask_24(t2, d, t1[24 + i]);
        t1[24 + i] += sp_3072_add_24(&t1[i], &t1[i], t2);
        sp_3072_mask_24(t2, d, t1[24 + i]);
        t1[24 + i] += sp_3072_add_24(&t1[i], &t1[i], t2);
    }

    r1 = sp_3072_cmp_24(t1, d) >= 0;
    sp_3072_cond_sub_24(r, t1, d, (sp_digit)0 - r1);

    return MP_OKAY;
}

/* Reduce a modulo m into r. (r = a mod m)
 *
 * r  A single precision number that is the reduced result.
 * a  A single precision number that is to be reduced.
 * m  A single precision number that is the modulus to reduce with.
 * returns MP_OKAY indicating success.
 */
static WC_INLINE int sp_3072_mod_24(sp_digit* r, sp_digit* a, sp_digit* m)
{
    return sp_3072_div_24(a, m, NULL, r);
}

/* Modular exponentiate a to the e mod m. (r = a^e mod m)
 *
 * r     A single precision number that is the result of the operation.
 * a     A single precision number being exponentiated.
 * e     A single precision number that is the exponent.
 * bits  The number of bits in the exponent.
 * m     A single precision number that is the modulus.
 * returns 0 on success and MEMORY_E on dynamic memory allocation failure.
 */
static int sp_3072_mod_exp_24(sp_digit* r, sp_digit* a, sp_digit* e,
        int bits, sp_digit* m, int reduceA)
{
#ifndef WOLFSSL_SMALL_STACK
    sp_digit t[32][48];
#else
    sp_digit* t[32];
    sp_digit* td;
#endif
    sp_digit* norm;
    sp_digit mp = 1;
    sp_digit n;
    sp_digit mask;
    int i;
    int c, y;
    int err = MP_OKAY;

#ifdef WOLFSSL_SMALL_STACK
    td = (sp_digit*)XMALLOC(sizeof(sp_digit) * 32 * 48, NULL,
                            DYNAMIC_TYPE_TMP_BUFFER);
    if (td == NULL)
        err = MEMORY_E;

    if (err == MP_OKAY) {
        for (i=0; i<32; i++)
            t[i] = td + i * 48;
        norm = t[0];
    }
#else
    norm = t[0];
#endif

    if (err == MP_OKAY) {
        sp_3072_mont_setup(m, &mp);
        sp_3072_mont_norm_24(norm, m);

        XMEMSET(t[1], 0, sizeof(sp_digit) * 24);
        if (reduceA) {
            err = sp_3072_mod_24(t[1] + 24, a, m);
            if (err == MP_OKAY)
                err = sp_3072_mod_24(t[1], t[1], m);
        }
        else {
            XMEMCPY(t[1] + 24, a, sizeof(sp_digit) * 24);
            err = sp_3072_mod_24(t[1], t[1], m);
        }
    }

    if (err == MP_OKAY) {
        sp_3072_mont_sqr_24(t[ 2], t[ 1], m, mp);
        sp_3072_mont_mul_24(t[ 3], t[ 2], t[ 1], m, mp);
        sp_3072_mont_sqr_24(t[ 4], t[ 2], m, mp);
        sp_3072_mont_mul_24(t[ 5], t[ 3], t[ 2], m, mp);
        sp_3072_mont_sqr_24(t[ 6], t[ 3], m, mp);
        sp_3072_mont_mul_24(t[ 7], t[ 4], t[ 3], m, mp);
        sp_3072_mont_sqr_24(t[ 8], t[ 4], m, mp);
        sp_3072_mont_mul_24(t[ 9], t[ 5], t[ 4], m, mp);
        sp_3072_mont_sqr_24(t[10], t[ 5], m, mp);
        sp_3072_mont_mul_24(t[11], t[ 6], t[ 5], m, mp);
        sp_3072_mont_sqr_24(t[12], t[ 6], m, mp);
        sp_3072_mont_mul_24(t[13], t[ 7], t[ 6], m, mp);
        sp_3072_mont_sqr_24(t[14], t[ 7], m, mp);
        sp_3072_mont_mul_24(t[15], t[ 8], t[ 7], m, mp);
        sp_3072_mont_sqr_24(t[16], t[ 8], m, mp);
        sp_3072_mont_mul_24(t[17], t[ 9], t[ 8], m, mp);
        sp_3072_mont_sqr_24(t[18], t[ 9], m, mp);
        sp_3072_mont_mul_24(t[19], t[10], t[ 9], m, mp);
        sp_3072_mont_sqr_24(t[20], t[10], m, mp);
        sp_3072_mont_mul_24(t[21], t[11], t[10], m, mp);
        sp_3072_mont_sqr_24(t[22], t[11], m, mp);
        sp_3072_mont_mul_24(t[23], t[12], t[11], m, mp);
        sp_3072_mont_sqr_24(t[24], t[12], m, mp);
        sp_3072_mont_mul_24(t[25], t[13], t[12], m, mp);
        sp_3072_mont_sqr_24(t[26], t[13], m, mp);
        sp_3072_mont_mul_24(t[27], t[14], t[13], m, mp);
        sp_3072_mont_sqr_24(t[28], t[14], m, mp);
        sp_3072_mont_mul_24(t[29], t[15], t[14], m, mp);
        sp_3072_mont_sqr_24(t[30], t[15], m, mp);
        sp_3072_mont_mul_24(t[31], t[16], t[15], m, mp);

        i = (bits - 1) / 64;
        n = e[i--];
        y = n >> 59;
        n <<= 5;
        c = 59;
        XMEMCPY(r, t[y], sizeof(sp_digit) * 24);
        for (; i>=0 || c>=5; ) {
            if (c == 0) {
                n = e[i--];
                y = n >> 59;
                n <<= 5;
                c = 59;
            }
            else if (c < 5) {
                y = n >> 59;
                n = e[i--];
                c = 5 - c;
                y |= n >> (64 - c);
                n <<= c;
                c = 64 - c;
            }
            else {
                y = (n >> 59) & 0x1f;
                n <<= 5;
                c -= 5;
            }

            sp_3072_mont_sqr_24(r, r, m, mp);
            sp_3072_mont_sqr_24(r, r, m, mp);
            sp_3072_mont_sqr_24(r, r, m, mp);
            sp_3072_mont_sqr_24(r, r, m, mp);
            sp_3072_mont_sqr_24(r, r, m, mp);

            sp_3072_mont_mul_24(r, r, t[y], m, mp);
        }
        y = e[0] & ((1 << c) - 1);
        for (; c > 0; c--)
            sp_3072_mont_sqr_24(r, r, m, mp);
        sp_3072_mont_mul_24(r, r, t[y], m, mp);

        XMEMSET(&r[24], 0, sizeof(sp_digit) * 24);
        sp_3072_mont_reduce_24(r, m, mp);

        mask = 0 - (sp_3072_cmp_24(r, m) >= 0);
        sp_3072_cond_sub_24(r, r, m, mask);
    }

#ifdef WOLFSSL_SMALL_STACK
    if (td != NULL)
        XFREE(td, NULL, DYNAMIC_TYPE_TMP_BUFFER);
#endif

    return err;
}

extern void sp_3072_mont_reduce_avx2_24(sp_digit* a, sp_digit* m, sp_digit mp);
#ifdef HAVE_INTEL_AVX2
/* Multiply two Montogmery form numbers mod the modulus (prime).
 * (r = a * b mod m)
 *
 * r   Result of multiplication.
 * a   First number to multiply in Montogmery form.
 * b   Second number to multiply in Montogmery form.
 * m   Modulus (prime).
 * mp  Montogmery mulitplier.
 */
static void sp_3072_mont_mul_avx2_24(sp_digit* r, sp_digit* a, sp_digit* b,
        sp_digit* m, sp_digit mp)
{
    sp_3072_mul_avx2_24(r, a, b);
    sp_3072_mont_reduce_avx2_24(r, m, mp);
}

#endif /* HAVE_INTEL_AVX2 */
#ifdef HAVE_INTEL_AVX2
/* Square the Montgomery form number. (r = a * a mod m)
 *
 * r   Result of squaring.
 * a   Number to square in Montogmery form.
 * m   Modulus (prime).
 * mp  Montogmery mulitplier.
 */
static void sp_3072_mont_sqr_avx2_24(sp_digit* r, sp_digit* a, sp_digit* m,
        sp_digit mp)
{
    sp_3072_sqr_avx2_24(r, a);
    sp_3072_mont_reduce_avx2_24(r, m, mp);
}

#endif /* HAVE_INTEL_AVX2 */
#ifdef HAVE_INTEL_AVX2
/* Modular exponentiate a to the e mod m. (r = a^e mod m)
 *
 * r     A single precision number that is the result of the operation.
 * a     A single precision number being exponentiated.
 * e     A single precision number that is the exponent.
 * bits  The number of bits in the exponent.
 * m     A single precision number that is the modulus.
 * returns 0 on success and MEMORY_E on dynamic memory allocation failure.
 */
static int sp_3072_mod_exp_avx2_24(sp_digit* r, sp_digit* a, sp_digit* e,
        int bits, sp_digit* m, int reduceA)
{
#ifndef WOLFSSL_SMALL_STACK
    sp_digit t[32][48];
#else
    sp_digit* t[32];
    sp_digit* td;
#endif
    sp_digit* norm;
    sp_digit mp = 1;
    sp_digit n;
    sp_digit mask;
    int i;
    int c, y;
    int err = MP_OKAY;

#ifdef WOLFSSL_SMALL_STACK
    td = (sp_digit*)XMALLOC(sizeof(sp_digit) * 32 * 48, NULL,
                            DYNAMIC_TYPE_TMP_BUFFER);
    if (td == NULL)
        err = MEMORY_E;

    if (err == MP_OKAY) {
        for (i=0; i<32; i++)
            t[i] = td + i * 48;
        norm = t[0];
    }
#else
    norm = t[0];
#endif

    if (err == MP_OKAY) {
        sp_3072_mont_setup(m, &mp);
        sp_3072_mont_norm_24(norm, m);

        XMEMSET(t[1], 0, sizeof(sp_digit) * 24);
        if (reduceA) {
            err = sp_3072_mod_24(t[1] + 24, a, m);
            if (err == MP_OKAY)
                err = sp_3072_mod_24(t[1], t[1], m);
        }
        else {
            XMEMCPY(t[1] + 24, a, sizeof(sp_digit) * 24);
            err = sp_3072_mod_24(t[1], t[1], m);
        }
    }

    if (err == MP_OKAY) {
        sp_3072_mont_sqr_avx2_24(t[ 2], t[ 1], m, mp);
        sp_3072_mont_mul_avx2_24(t[ 3], t[ 2], t[ 1], m, mp);
        sp_3072_mont_sqr_avx2_24(t[ 4], t[ 2], m, mp);
        sp_3072_mont_mul_avx2_24(t[ 5], t[ 3], t[ 2], m, mp);
        sp_3072_mont_sqr_avx2_24(t[ 6], t[ 3], m, mp);
        sp_3072_mont_mul_avx2_24(t[ 7], t[ 4], t[ 3], m, mp);
        sp_3072_mont_sqr_avx2_24(t[ 8], t[ 4], m, mp);
        sp_3072_mont_mul_avx2_24(t[ 9], t[ 5], t[ 4], m, mp);
        sp_3072_mont_sqr_avx2_24(t[10], t[ 5], m, mp);
        sp_3072_mont_mul_avx2_24(t[11], t[ 6], t[ 5], m, mp);
        sp_3072_mont_sqr_avx2_24(t[12], t[ 6], m, mp);
        sp_3072_mont_mul_avx2_24(t[13], t[ 7], t[ 6], m, mp);
        sp_3072_mont_sqr_avx2_24(t[14], t[ 7], m, mp);
        sp_3072_mont_mul_avx2_24(t[15], t[ 8], t[ 7], m, mp);
        sp_3072_mont_sqr_avx2_24(t[16], t[ 8], m, mp);
        sp_3072_mont_mul_avx2_24(t[17], t[ 9], t[ 8], m, mp);
        sp_3072_mont_sqr_avx2_24(t[18], t[ 9], m, mp);
        sp_3072_mont_mul_avx2_24(t[19], t[10], t[ 9], m, mp);
        sp_3072_mont_sqr_avx2_24(t[20], t[10], m, mp);
        sp_3072_mont_mul_avx2_24(t[21], t[11], t[10], m, mp);
        sp_3072_mont_sqr_avx2_24(t[22], t[11], m, mp);
        sp_3072_mont_mul_avx2_24(t[23], t[12], t[11], m, mp);
        sp_3072_mont_sqr_avx2_24(t[24], t[12], m, mp);
        sp_3072_mont_mul_avx2_24(t[25], t[13], t[12], m, mp);
        sp_3072_mont_sqr_avx2_24(t[26], t[13], m, mp);
        sp_3072_mont_mul_avx2_24(t[27], t[14], t[13], m, mp);
        sp_3072_mont_sqr_avx2_24(t[28], t[14], m, mp);
        sp_3072_mont_mul_avx2_24(t[29], t[15], t[14], m, mp);
        sp_3072_mont_sqr_avx2_24(t[30], t[15], m, mp);
        sp_3072_mont_mul_avx2_24(t[31], t[16], t[15], m, mp);

        i = (bits - 1) / 64;
        n = e[i--];
        y = n >> 59;
        n <<= 5;
        c = 59;
        XMEMCPY(r, t[y], sizeof(sp_digit) * 24);
        for (; i>=0 || c>=5; ) {
            if (c == 0) {
                n = e[i--];
                y = n >> 59;
                n <<= 5;
                c = 59;
            }
            else if (c < 5) {
                y = n >> 59;
                n = e[i--];
                c = 5 - c;
                y |= n >> (64 - c);
                n <<= c;
                c = 64 - c;
            }
            else {
                y = (n >> 59) & 0x1f;
                n <<= 5;
                c -= 5;
            }

            sp_3072_mont_sqr_avx2_24(r, r, m, mp);
            sp_3072_mont_sqr_avx2_24(r, r, m, mp);
            sp_3072_mont_sqr_avx2_24(r, r, m, mp);
            sp_3072_mont_sqr_avx2_24(r, r, m, mp);
            sp_3072_mont_sqr_avx2_24(r, r, m, mp);

            sp_3072_mont_mul_avx2_24(r, r, t[y], m, mp);
        }
        y = e[0] & ((1 << c) - 1);
        for (; c > 0; c--)
            sp_3072_mont_sqr_avx2_24(r, r, m, mp);
        sp_3072_mont_mul_avx2_24(r, r, t[y], m, mp);

        XMEMSET(&r[24], 0, sizeof(sp_digit) * 24);
        sp_3072_mont_reduce_avx2_24(r, m, mp);

        mask = 0 - (sp_3072_cmp_24(r, m) >= 0);
        sp_3072_cond_sub_24(r, r, m, mask);
    }

#ifdef WOLFSSL_SMALL_STACK
    if (td != NULL)
        XFREE(td, NULL, DYNAMIC_TYPE_TMP_BUFFER);
#endif

    return err;
}
#endif /* HAVE_INTEL_AVX2 */

#endif /* (WOLFSSL_HAVE_SP_RSA || WOLFSSL_HAVE_SP_DH) && !WOLFSSL_RSA_PUBLIC_ONLY */

#ifdef WOLFSSL_HAVE_SP_DH
/* r = 2^n mod m where n is the number of bits to reduce by.
 * Given m must be 3072 bits, just need to subtract.
 *
 * r  A single precision number.
 * m  A signle precision number.
 */
static void sp_3072_mont_norm_48(sp_digit* r, sp_digit* m)
{
    XMEMSET(r, 0, sizeof(sp_digit) * 48);

    /* r = 2^n mod m */
    sp_3072_sub_in_place_48(r, m);
}

#endif /* WOLFSSL_HAVE_SP_DH */
extern sp_digit sp_3072_cond_sub_48(sp_digit* r, sp_digit* a, sp_digit* b, sp_digit m);
extern void sp_3072_mont_reduce_48(sp_digit* a, sp_digit* m, sp_digit mp);
/* Multiply two Montogmery form numbers mod the modulus (prime).
 * (r = a * b mod m)
 *
 * r   Result of multiplication.
 * a   First number to multiply in Montogmery form.
 * b   Second number to multiply in Montogmery form.
 * m   Modulus (prime).
 * mp  Montogmery mulitplier.
 */
static void sp_3072_mont_mul_48(sp_digit* r, sp_digit* a, sp_digit* b,
        sp_digit* m, sp_digit mp)
{
    sp_3072_mul_48(r, a, b);
    sp_3072_mont_reduce_48(r, m, mp);
}

/* Square the Montgomery form number. (r = a * a mod m)
 *
 * r   Result of squaring.
 * a   Number to square in Montogmery form.
 * m   Modulus (prime).
 * mp  Montogmery mulitplier.
 */
static void sp_3072_mont_sqr_48(sp_digit* r, sp_digit* a, sp_digit* m,
        sp_digit mp)
{
    sp_3072_sqr_48(r, a);
    sp_3072_mont_reduce_48(r, m, mp);
}

#ifndef WOLFSSL_RSA_PUBLIC_ONLY
extern void sp_3072_mul_d_avx2_48(sp_digit* r, const sp_digit* a, const sp_digit b);
/* Divide the double width number (d1|d0) by the dividend. (d1|d0 / div)
 *
 * d1   The high order half of the number to divide.
 * d0   The low order half of the number to divide.
 * div  The dividend.
 * returns the result of the division.
 */
static WC_INLINE sp_digit div_3072_word_48(sp_digit d1, sp_digit d0,
        sp_digit div)
{
    register sp_digit r asm("rax");
    __asm__ __volatile__ (
        "divq %3"
        : "=a" (r)
        : "d" (d1), "a" (d0), "r" (div)
        :
    );
    return r;
}
/* AND m into each word of a and store in r.
 *
 * r  A single precision integer.
 * a  A single precision integer.
 * m  Mask to AND against each digit.
 */
static void sp_3072_mask_48(sp_digit* r, sp_digit* a, sp_digit m)
{
#ifdef WOLFSSL_SP_SMALL
    int i;

    for (i=0; i<48; i++)
        r[i] = a[i] & m;
#else
    int i;

    for (i = 0; i < 48; i += 8) {
        r[i+0] = a[i+0] & m;
        r[i+1] = a[i+1] & m;
        r[i+2] = a[i+2] & m;
        r[i+3] = a[i+3] & m;
        r[i+4] = a[i+4] & m;
        r[i+5] = a[i+5] & m;
        r[i+6] = a[i+6] & m;
        r[i+7] = a[i+7] & m;
    }
#endif
}

extern int64_t sp_3072_cmp_48(sp_digit* a, sp_digit* b);
/* Divide d in a and put remainder into r (m*d + r = a)
 * m is not calculated as it is not needed at this time.
 *
 * a  Nmber to be divided.
 * d  Number to divide with.
 * m  Multiplier result.
 * r  Remainder from the division.
 * returns MP_OKAY indicating success.
 */
static WC_INLINE int sp_3072_div_48(sp_digit* a, sp_digit* d, sp_digit* m,
        sp_digit* r)
{
    sp_digit t1[96], t2[49];
    sp_digit div, r1;
    int i;
#ifdef HAVE_INTEL_AVX2
    word32 cpuid_flags = cpuid_get_flags();
#endif

    (void)m;

    div = d[47];
    XMEMCPY(t1, a, sizeof(*t1) * 2 * 48);
    for (i=47; i>=0; i--) {
        r1 = div_3072_word_48(t1[48 + i], t1[48 + i - 1], div);

#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
            sp_3072_mul_d_avx2_48(t2, d, r1);
        else
#endif
            sp_3072_mul_d_48(t2, d, r1);
        t1[48 + i] += sp_3072_sub_in_place_48(&t1[i], t2);
        t1[48 + i] -= t2[48];
        sp_3072_mask_48(t2, d, t1[48 + i]);
        t1[48 + i] += sp_3072_add_48(&t1[i], &t1[i], t2);
        sp_3072_mask_48(t2, d, t1[48 + i]);
        t1[48 + i] += sp_3072_add_48(&t1[i], &t1[i], t2);
    }

    r1 = sp_3072_cmp_48(t1, d) >= 0;
    sp_3072_cond_sub_48(r, t1, d, (sp_digit)0 - r1);

    return MP_OKAY;
}

/* Reduce a modulo m into r. (r = a mod m)
 *
 * r  A single precision number that is the reduced result.
 * a  A single precision number that is to be reduced.
 * m  A single precision number that is the modulus to reduce with.
 * returns MP_OKAY indicating success.
 */
static WC_INLINE int sp_3072_mod_48(sp_digit* r, sp_digit* a, sp_digit* m)
{
    return sp_3072_div_48(a, m, NULL, r);
}

#endif /* WOLFSSL_RSA_PUBLIC_ONLY */
/* Divide d in a and put remainder into r (m*d + r = a)
 * m is not calculated as it is not needed at this time.
 *
 * a  Nmber to be divided.
 * d  Number to divide with.
 * m  Multiplier result.
 * r  Remainder from the division.
 * returns MP_OKAY indicating success.
 */
static WC_INLINE int sp_3072_div_48_cond(sp_digit* a, sp_digit* d, sp_digit* m,
        sp_digit* r)
{
    sp_digit t1[96], t2[49];
    sp_digit div, r1;
    int i;
#ifdef HAVE_INTEL_AVX2
    word32 cpuid_flags = cpuid_get_flags();
#endif

    (void)m;

    div = d[47];
    XMEMCPY(t1, a, sizeof(*t1) * 2 * 48);
    for (i=47; i>=0; i--) {
        r1 = div_3072_word_48(t1[48 + i], t1[48 + i - 1], div);

#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
            sp_3072_mul_d_avx2_48(t2, d, r1);
        else
#endif
            sp_3072_mul_d_48(t2, d, r1);
        t1[48 + i] += sp_3072_sub_in_place_48(&t1[i], t2);
        t1[48 + i] -= t2[48];
        if (t1[48 + i] != 0) {
            t1[48 + i] += sp_3072_add_48(&t1[i], &t1[i], d);
            if (t1[48 + i] != 0)
                t1[48 + i] += sp_3072_add_48(&t1[i], &t1[i], d);
        }
    }

    r1 = sp_3072_cmp_48(t1, d) >= 0;
    sp_3072_cond_sub_48(r, t1, d, (sp_digit)0 - r1);

    return MP_OKAY;
}

/* Reduce a modulo m into r. (r = a mod m)
 *
 * r  A single precision number that is the reduced result.
 * a  A single precision number that is to be reduced.
 * m  A single precision number that is the modulus to reduce with.
 * returns MP_OKAY indicating success.
 */
static WC_INLINE int sp_3072_mod_48_cond(sp_digit* r, sp_digit* a, sp_digit* m)
{
    return sp_3072_div_48_cond(a, m, NULL, r);
}

#if (defined(SP_RSA_PRIVATE_EXP_D) && !defined(WOLFSSL_RSA_PUBLIC_ONLY)) || defined(WOLFSSL_HAVE_SP_DH)
/* Modular exponentiate a to the e mod m. (r = a^e mod m)
 *
 * r     A single precision number that is the result of the operation.
 * a     A single precision number being exponentiated.
 * e     A single precision number that is the exponent.
 * bits  The number of bits in the exponent.
 * m     A single precision number that is the modulus.
 * returns 0 on success and MEMORY_E on dynamic memory allocation failure.
 */
static int sp_3072_mod_exp_48(sp_digit* r, sp_digit* a, sp_digit* e,
        int bits, sp_digit* m, int reduceA)
{
#ifndef WOLFSSL_SMALL_STACK
    sp_digit t[32][96];
#else
    sp_digit* t[32];
    sp_digit* td;
#endif
    sp_digit* norm;
    sp_digit mp = 1;
    sp_digit n;
    sp_digit mask;
    int i;
    int c, y;
    int err = MP_OKAY;

#ifdef WOLFSSL_SMALL_STACK
    td = (sp_digit*)XMALLOC(sizeof(sp_digit) * 32 * 96, NULL,
                            DYNAMIC_TYPE_TMP_BUFFER);
    if (td == NULL)
        err = MEMORY_E;

    if (err == MP_OKAY) {
        for (i=0; i<32; i++)
            t[i] = td + i * 96;
        norm = t[0];
    }
#else
    norm = t[0];
#endif

    if (err == MP_OKAY) {
        sp_3072_mont_setup(m, &mp);
        sp_3072_mont_norm_48(norm, m);

        XMEMSET(t[1], 0, sizeof(sp_digit) * 48);
        if (reduceA) {
            err = sp_3072_mod_48(t[1] + 48, a, m);
            if (err == MP_OKAY)
                err = sp_3072_mod_48(t[1], t[1], m);
        }
        else {
            XMEMCPY(t[1] + 48, a, sizeof(sp_digit) * 48);
            err = sp_3072_mod_48(t[1], t[1], m);
        }
    }

    if (err == MP_OKAY) {
        sp_3072_mont_sqr_48(t[ 2], t[ 1], m, mp);
        sp_3072_mont_mul_48(t[ 3], t[ 2], t[ 1], m, mp);
        sp_3072_mont_sqr_48(t[ 4], t[ 2], m, mp);
        sp_3072_mont_mul_48(t[ 5], t[ 3], t[ 2], m, mp);
        sp_3072_mont_sqr_48(t[ 6], t[ 3], m, mp);
        sp_3072_mont_mul_48(t[ 7], t[ 4], t[ 3], m, mp);
        sp_3072_mont_sqr_48(t[ 8], t[ 4], m, mp);
        sp_3072_mont_mul_48(t[ 9], t[ 5], t[ 4], m, mp);
        sp_3072_mont_sqr_48(t[10], t[ 5], m, mp);
        sp_3072_mont_mul_48(t[11], t[ 6], t[ 5], m, mp);
        sp_3072_mont_sqr_48(t[12], t[ 6], m, mp);
        sp_3072_mont_mul_48(t[13], t[ 7], t[ 6], m, mp);
        sp_3072_mont_sqr_48(t[14], t[ 7], m, mp);
        sp_3072_mont_mul_48(t[15], t[ 8], t[ 7], m, mp);
        sp_3072_mont_sqr_48(t[16], t[ 8], m, mp);
        sp_3072_mont_mul_48(t[17], t[ 9], t[ 8], m, mp);
        sp_3072_mont_sqr_48(t[18], t[ 9], m, mp);
        sp_3072_mont_mul_48(t[19], t[10], t[ 9], m, mp);
        sp_3072_mont_sqr_48(t[20], t[10], m, mp);
        sp_3072_mont_mul_48(t[21], t[11], t[10], m, mp);
        sp_3072_mont_sqr_48(t[22], t[11], m, mp);
        sp_3072_mont_mul_48(t[23], t[12], t[11], m, mp);
        sp_3072_mont_sqr_48(t[24], t[12], m, mp);
        sp_3072_mont_mul_48(t[25], t[13], t[12], m, mp);
        sp_3072_mont_sqr_48(t[26], t[13], m, mp);
        sp_3072_mont_mul_48(t[27], t[14], t[13], m, mp);
        sp_3072_mont_sqr_48(t[28], t[14], m, mp);
        sp_3072_mont_mul_48(t[29], t[15], t[14], m, mp);
        sp_3072_mont_sqr_48(t[30], t[15], m, mp);
        sp_3072_mont_mul_48(t[31], t[16], t[15], m, mp);

        i = (bits - 1) / 64;
        n = e[i--];
        y = n >> 59;
        n <<= 5;
        c = 59;
        XMEMCPY(r, t[y], sizeof(sp_digit) * 48);
        for (; i>=0 || c>=5; ) {
            if (c == 0) {
                n = e[i--];
                y = n >> 59;
                n <<= 5;
                c = 59;
            }
            else if (c < 5) {
                y = n >> 59;
                n = e[i--];
                c = 5 - c;
                y |= n >> (64 - c);
                n <<= c;
                c = 64 - c;
            }
            else {
                y = (n >> 59) & 0x1f;
                n <<= 5;
                c -= 5;
            }

            sp_3072_mont_sqr_48(r, r, m, mp);
            sp_3072_mont_sqr_48(r, r, m, mp);
            sp_3072_mont_sqr_48(r, r, m, mp);
            sp_3072_mont_sqr_48(r, r, m, mp);
            sp_3072_mont_sqr_48(r, r, m, mp);

            sp_3072_mont_mul_48(r, r, t[y], m, mp);
        }
        y = e[0] & ((1 << c) - 1);
        for (; c > 0; c--)
            sp_3072_mont_sqr_48(r, r, m, mp);
        sp_3072_mont_mul_48(r, r, t[y], m, mp);

        XMEMSET(&r[48], 0, sizeof(sp_digit) * 48);
        sp_3072_mont_reduce_48(r, m, mp);

        mask = 0 - (sp_3072_cmp_48(r, m) >= 0);
        sp_3072_cond_sub_48(r, r, m, mask);
    }

#ifdef WOLFSSL_SMALL_STACK
    if (td != NULL)
        XFREE(td, NULL, DYNAMIC_TYPE_TMP_BUFFER);
#endif

    return err;
}
#endif /* (SP_RSA_PRIVATE_EXP_D && !WOLFSSL_RSA_PUBLIC_ONLY) || WOLFSSL_HAVE_SP_DH */

extern void sp_3072_mont_reduce_avx2_48(sp_digit* a, sp_digit* m, sp_digit mp);
#ifdef HAVE_INTEL_AVX2
/* Multiply two Montogmery form numbers mod the modulus (prime).
 * (r = a * b mod m)
 *
 * r   Result of multiplication.
 * a   First number to multiply in Montogmery form.
 * b   Second number to multiply in Montogmery form.
 * m   Modulus (prime).
 * mp  Montogmery mulitplier.
 */
static void sp_3072_mont_mul_avx2_48(sp_digit* r, sp_digit* a, sp_digit* b,
        sp_digit* m, sp_digit mp)
{
    sp_3072_mul_avx2_48(r, a, b);
    sp_3072_mont_reduce_avx2_48(r, m, mp);
}

#endif /* HAVE_INTEL_AVX2 */
#ifdef HAVE_INTEL_AVX2
/* Square the Montgomery form number. (r = a * a mod m)
 *
 * r   Result of squaring.
 * a   Number to square in Montogmery form.
 * m   Modulus (prime).
 * mp  Montogmery mulitplier.
 */
static void sp_3072_mont_sqr_avx2_48(sp_digit* r, sp_digit* a, sp_digit* m,
        sp_digit mp)
{
    sp_3072_sqr_avx2_48(r, a);
    sp_3072_mont_reduce_avx2_48(r, m, mp);
}

#endif /* HAVE_INTEL_AVX2 */
#if (defined(SP_RSA_PRIVATE_EXP_D) && !defined(WOLFSSL_RSA_PUBLIC_ONLY)) || defined(WOLFSSL_HAVE_SP_DH)
#ifdef HAVE_INTEL_AVX2
/* Modular exponentiate a to the e mod m. (r = a^e mod m)
 *
 * r     A single precision number that is the result of the operation.
 * a     A single precision number being exponentiated.
 * e     A single precision number that is the exponent.
 * bits  The number of bits in the exponent.
 * m     A single precision number that is the modulus.
 * returns 0 on success and MEMORY_E on dynamic memory allocation failure.
 */
static int sp_3072_mod_exp_avx2_48(sp_digit* r, sp_digit* a, sp_digit* e,
        int bits, sp_digit* m, int reduceA)
{
#ifndef WOLFSSL_SMALL_STACK
    sp_digit t[32][96];
#else
    sp_digit* t[32];
    sp_digit* td;
#endif
    sp_digit* norm;
    sp_digit mp = 1;
    sp_digit n;
    sp_digit mask;
    int i;
    int c, y;
    int err = MP_OKAY;

#ifdef WOLFSSL_SMALL_STACK
    td = (sp_digit*)XMALLOC(sizeof(sp_digit) * 32 * 96, NULL,
                            DYNAMIC_TYPE_TMP_BUFFER);
    if (td == NULL)
        err = MEMORY_E;

    if (err == MP_OKAY) {
        for (i=0; i<32; i++)
            t[i] = td + i * 96;
        norm = t[0];
    }
#else
    norm = t[0];
#endif

    if (err == MP_OKAY) {
        sp_3072_mont_setup(m, &mp);
        sp_3072_mont_norm_48(norm, m);

        XMEMSET(t[1], 0, sizeof(sp_digit) * 48);
        if (reduceA) {
            err = sp_3072_mod_48(t[1] + 48, a, m);
            if (err == MP_OKAY)
                err = sp_3072_mod_48(t[1], t[1], m);
        }
        else {
            XMEMCPY(t[1] + 48, a, sizeof(sp_digit) * 48);
            err = sp_3072_mod_48(t[1], t[1], m);
        }
    }

    if (err == MP_OKAY) {
        sp_3072_mont_sqr_avx2_48(t[ 2], t[ 1], m, mp);
        sp_3072_mont_mul_avx2_48(t[ 3], t[ 2], t[ 1], m, mp);
        sp_3072_mont_sqr_avx2_48(t[ 4], t[ 2], m, mp);
        sp_3072_mont_mul_avx2_48(t[ 5], t[ 3], t[ 2], m, mp);
        sp_3072_mont_sqr_avx2_48(t[ 6], t[ 3], m, mp);
        sp_3072_mont_mul_avx2_48(t[ 7], t[ 4], t[ 3], m, mp);
        sp_3072_mont_sqr_avx2_48(t[ 8], t[ 4], m, mp);
        sp_3072_mont_mul_avx2_48(t[ 9], t[ 5], t[ 4], m, mp);
        sp_3072_mont_sqr_avx2_48(t[10], t[ 5], m, mp);
        sp_3072_mont_mul_avx2_48(t[11], t[ 6], t[ 5], m, mp);
        sp_3072_mont_sqr_avx2_48(t[12], t[ 6], m, mp);
        sp_3072_mont_mul_avx2_48(t[13], t[ 7], t[ 6], m, mp);
        sp_3072_mont_sqr_avx2_48(t[14], t[ 7], m, mp);
        sp_3072_mont_mul_avx2_48(t[15], t[ 8], t[ 7], m, mp);
        sp_3072_mont_sqr_avx2_48(t[16], t[ 8], m, mp);
        sp_3072_mont_mul_avx2_48(t[17], t[ 9], t[ 8], m, mp);
        sp_3072_mont_sqr_avx2_48(t[18], t[ 9], m, mp);
        sp_3072_mont_mul_avx2_48(t[19], t[10], t[ 9], m, mp);
        sp_3072_mont_sqr_avx2_48(t[20], t[10], m, mp);
        sp_3072_mont_mul_avx2_48(t[21], t[11], t[10], m, mp);
        sp_3072_mont_sqr_avx2_48(t[22], t[11], m, mp);
        sp_3072_mont_mul_avx2_48(t[23], t[12], t[11], m, mp);
        sp_3072_mont_sqr_avx2_48(t[24], t[12], m, mp);
        sp_3072_mont_mul_avx2_48(t[25], t[13], t[12], m, mp);
        sp_3072_mont_sqr_avx2_48(t[26], t[13], m, mp);
        sp_3072_mont_mul_avx2_48(t[27], t[14], t[13], m, mp);
        sp_3072_mont_sqr_avx2_48(t[28], t[14], m, mp);
        sp_3072_mont_mul_avx2_48(t[29], t[15], t[14], m, mp);
        sp_3072_mont_sqr_avx2_48(t[30], t[15], m, mp);
        sp_3072_mont_mul_avx2_48(t[31], t[16], t[15], m, mp);

        i = (bits - 1) / 64;
        n = e[i--];
        y = n >> 59;
        n <<= 5;
        c = 59;
        XMEMCPY(r, t[y], sizeof(sp_digit) * 48);
        for (; i>=0 || c>=5; ) {
            if (c == 0) {
                n = e[i--];
                y = n >> 59;
                n <<= 5;
                c = 59;
            }
            else if (c < 5) {
                y = n >> 59;
                n = e[i--];
                c = 5 - c;
                y |= n >> (64 - c);
                n <<= c;
                c = 64 - c;
            }
            else {
                y = (n >> 59) & 0x1f;
                n <<= 5;
                c -= 5;
            }

            sp_3072_mont_sqr_avx2_48(r, r, m, mp);
            sp_3072_mont_sqr_avx2_48(r, r, m, mp);
            sp_3072_mont_sqr_avx2_48(r, r, m, mp);
            sp_3072_mont_sqr_avx2_48(r, r, m, mp);
            sp_3072_mont_sqr_avx2_48(r, r, m, mp);

            sp_3072_mont_mul_avx2_48(r, r, t[y], m, mp);
        }
        y = e[0] & ((1 << c) - 1);
        for (; c > 0; c--)
            sp_3072_mont_sqr_avx2_48(r, r, m, mp);
        sp_3072_mont_mul_avx2_48(r, r, t[y], m, mp);

        XMEMSET(&r[48], 0, sizeof(sp_digit) * 48);
        sp_3072_mont_reduce_avx2_48(r, m, mp);

        mask = 0 - (sp_3072_cmp_48(r, m) >= 0);
        sp_3072_cond_sub_48(r, r, m, mask);
    }

#ifdef WOLFSSL_SMALL_STACK
    if (td != NULL)
        XFREE(td, NULL, DYNAMIC_TYPE_TMP_BUFFER);
#endif

    return err;
}
#endif /* HAVE_INTEL_AVX2 */
#endif /* (SP_RSA_PRIVATE_EXP_D && !WOLFSSL_RSA_PUBLIC_ONLY) || WOLFSSL_HAVE_SP_DH */

#ifdef WOLFSSL_HAVE_SP_RSA
/* RSA public key operation.
 *
 * in      Array of bytes representing the number to exponentiate, base.
 * inLen   Number of bytes in base.
 * em      Public exponent.
 * mm      Modulus.
 * out     Buffer to hold big-endian bytes of exponentiation result.
 *         Must be at least 384 bytes long.
 * outLen  Number of bytes in result.
 * returns 0 on success, MP_TO_E when the outLen is too small, MP_READ_E when
 * an array is too long and MEMORY_E when dynamic memory allocation fails.
 */
int sp_RsaPublic_3072(const byte* in, word32 inLen, mp_int* em, mp_int* mm,
    byte* out, word32* outLen)
{
#if !defined(WOLFSSL_SP_SMALL) && !defined(WOLFSSL_SMALL_STACK)
    sp_digit ad[96], md[48], rd[96];
#else
    sp_digit* d = NULL;
#endif
    sp_digit* a;
    sp_digit *ah;
    sp_digit* m;
    sp_digit* r;
    sp_digit e[1];
    int err = MP_OKAY;
#ifdef HAVE_INTEL_AVX2
    word32 cpuid_flags = cpuid_get_flags();
#endif

    if (*outLen < 384)
        err = MP_TO_E;
    if (err == MP_OKAY && (mp_count_bits(em) > 64 || inLen > 384 ||
                                                     mp_count_bits(mm) != 3072))
        err = MP_READ_E;

#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (err == MP_OKAY) {
        d = (sp_digit*)XMALLOC(sizeof(sp_digit) * 48 * 5, NULL,
                                                              DYNAMIC_TYPE_RSA);
        if (d == NULL)
            err = MEMORY_E;
    }

    if (err == MP_OKAY) {
        a = d;
        r = a + 48 * 2;
        m = r + 48 * 2;
        ah = a + 48;
    }
#else
    a = ad;
    m = md;
    r = rd;
    ah = a + 48;
#endif

    if (err == MP_OKAY) {
        sp_3072_from_bin(ah, 48, in, inLen);
#if DIGIT_BIT >= 64
        e[0] = em->dp[0];
#else
        e[0] = em->dp[0];
        if (em->used > 1)
            e[0] |= ((sp_digit)em->dp[1]) << DIGIT_BIT;
#endif
        if (e[0] == 0)
            err = MP_EXPTMOD_E;
    }
    if (err == MP_OKAY) {
        sp_3072_from_mp(m, 48, mm);

        if (e[0] == 0x3) {
#ifdef HAVE_INTEL_AVX2
            if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags)) {
                if (err == MP_OKAY) {
                    sp_3072_sqr_avx2_48(r, ah);
                    err = sp_3072_mod_48_cond(r, r, m);
                }
                if (err == MP_OKAY) {
                    sp_3072_mul_avx2_48(r, ah, r);
                    err = sp_3072_mod_48_cond(r, r, m);
                }
            }
            else
#endif
            {
                if (err == MP_OKAY) {
                    sp_3072_sqr_48(r, ah);
                    err = sp_3072_mod_48_cond(r, r, m);
                }
                if (err == MP_OKAY) {
                    sp_3072_mul_48(r, ah, r);
                    err = sp_3072_mod_48_cond(r, r, m);
                }
            }
        }
        else {
            int i;
            sp_digit mp;

            sp_3072_mont_setup(m, &mp);

            /* Convert to Montgomery form. */
            XMEMSET(a, 0, sizeof(sp_digit) * 48);
            err = sp_3072_mod_48_cond(a, a, m);

            if (err == MP_OKAY) {
                for (i=63; i>=0; i--)
                    if (e[0] >> i)
                        break;

                XMEMCPY(r, a, sizeof(sp_digit) * 48);
#ifdef HAVE_INTEL_AVX2
                if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags)) {
                    for (i--; i>=0; i--) {
                        sp_3072_mont_sqr_avx2_48(r, r, m, mp);
                        if (((e[0] >> i) & 1) == 1)
                            sp_3072_mont_mul_avx2_48(r, r, a, m, mp);
                    }
                    XMEMSET(&r[48], 0, sizeof(sp_digit) * 48);
                    sp_3072_mont_reduce_avx2_48(r, m, mp);
                }
                else
#endif
                {
                    for (i--; i>=0; i--) {
                        sp_3072_mont_sqr_48(r, r, m, mp);
                        if (((e[0] >> i) & 1) == 1)
                            sp_3072_mont_mul_48(r, r, a, m, mp);
                    }
                    XMEMSET(&r[48], 0, sizeof(sp_digit) * 48);
                    sp_3072_mont_reduce_48(r, m, mp);
                }

                for (i = 47; i > 0; i--) {
                    if (r[i] != m[i])
                        break;
                }
                if (r[i] >= m[i])
                    sp_3072_sub_in_place_48(r, m);
            }
        }
    }

    if (err == MP_OKAY) {
        sp_3072_to_bin(r, out);
        *outLen = 384;
    }

#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (d != NULL)
        XFREE(d, NULL, DYNAMIC_TYPE_RSA);
#endif

    return err;
}

/* RSA private key operation.
 *
 * in      Array of bytes representing the number to exponentiate, base.
 * inLen   Number of bytes in base.
 * dm      Private exponent.
 * pm      First prime.
 * qm      Second prime.
 * dpm     First prime's CRT exponent.
 * dqm     Second prime's CRT exponent.
 * qim     Inverse of second prime mod p.
 * mm      Modulus.
 * out     Buffer to hold big-endian bytes of exponentiation result.
 *         Must be at least 384 bytes long.
 * outLen  Number of bytes in result.
 * returns 0 on success, MP_TO_E when the outLen is too small, MP_READ_E when
 * an array is too long and MEMORY_E when dynamic memory allocation fails.
 */
int sp_RsaPrivate_3072(const byte* in, word32 inLen, mp_int* dm,
    mp_int* pm, mp_int* qm, mp_int* dpm, mp_int* dqm, mp_int* qim, mp_int* mm,
    byte* out, word32* outLen)
{
#if !defined(WOLFSSL_SP_SMALL) && !defined(WOLFSSL_SMALL_STACK)
    sp_digit ad[48 * 2];
    sp_digit pd[24], qd[24], dpd[24];
    sp_digit tmpad[48], tmpbd[48];
#else
    sp_digit* t = NULL;
#endif
    sp_digit* a;
    sp_digit* p;
    sp_digit* q;
    sp_digit* dp;
    sp_digit* dq;
    sp_digit* qi;
    sp_digit* tmp;
    sp_digit* tmpa;
    sp_digit* tmpb;
    sp_digit* r;
    sp_digit c;
    int err = MP_OKAY;
#ifdef HAVE_INTEL_AVX2
    word32 cpuid_flags = cpuid_get_flags();
#endif

    (void)dm;
    (void)mm;

    if (*outLen < 384)
        err = MP_TO_E;
    if (err == MP_OKAY && (inLen > 384 || mp_count_bits(mm) != 3072))
        err = MP_READ_E;

#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (err == MP_OKAY) {
        t = (sp_digit*)XMALLOC(sizeof(sp_digit) * 24 * 11, NULL,
                                                              DYNAMIC_TYPE_RSA);
        if (t == NULL)
            err = MEMORY_E;
    }
    if (err == MP_OKAY) {
        a = t;
        p = a + 48 * 2;
        q = p + 24;
        qi = dq = dp = q + 24;
        tmpa = qi + 24;
        tmpb = tmpa + 48;

        tmp = t;
        r = tmp + 48;
    }
#else
    r = a = ad;
    p = pd;
    q = qd;
    qi = dq = dp = dpd;
    tmpa = tmpad;
    tmpb = tmpbd;
    tmp = a + 48;
#endif

    if (err == MP_OKAY) {
        sp_3072_from_bin(a, 48, in, inLen);
        sp_3072_from_mp(p, 24, pm);
        sp_3072_from_mp(q, 24, qm);
        sp_3072_from_mp(dp, 24, dpm);

#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
            err = sp_3072_mod_exp_avx2_24(tmpa, a, dp, 1536, p, 1);
        else
#endif
            err = sp_3072_mod_exp_24(tmpa, a, dp, 1536, p, 1);
    }
    if (err == MP_OKAY) {
        sp_3072_from_mp(dq, 24, dqm);
#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
            err = sp_3072_mod_exp_avx2_24(tmpb, a, dq, 1536, q, 1);
       else
#endif
            err = sp_3072_mod_exp_24(tmpb, a, dq, 1536, q, 1);
    }

    if (err == MP_OKAY) {
        c = sp_3072_sub_in_place_24(tmpa, tmpb);
        sp_3072_mask_24(tmp, p, c);
        sp_3072_add_24(tmpa, tmpa, tmp);

        sp_3072_from_mp(qi, 24, qim);
#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
            sp_3072_mul_avx2_24(tmpa, tmpa, qi);
        else
#endif
            sp_3072_mul_24(tmpa, tmpa, qi);
        err = sp_3072_mod_24(tmpa, tmpa, p);
    }

    if (err == MP_OKAY) {
#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
            sp_3072_mul_avx2_24(tmpa, q, tmpa);
        else
#endif
            sp_3072_mul_24(tmpa, q, tmpa);
        XMEMSET(&tmpb[24], 0, sizeof(sp_digit) * 24);
        sp_3072_add_48(r, tmpb, tmpa);

        sp_3072_to_bin(r, out);
        *outLen = 384;
    }

#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (t != NULL) {
        XMEMSET(t, 0, sizeof(sp_digit) * 24 * 11);
        XFREE(t, NULL, DYNAMIC_TYPE_RSA);
    }
#else
    XMEMSET(tmpad, 0, sizeof(tmpad));
    XMEMSET(tmpbd, 0, sizeof(tmpbd));
    XMEMSET(pd, 0, sizeof(pd));
    XMEMSET(qd, 0, sizeof(qd));
    XMEMSET(dpd, 0, sizeof(dpd));
#endif

    return err;
}
#endif /* WOLFSSL_HAVE_SP_RSA */
#if defined(WOLFSSL_HAVE_SP_DH) || (defined(WOLFSSL_HAVE_SP_RSA) && \
                                              !defined(WOLFSSL_RSA_PUBLIC_ONLY))
/* Convert an array of sp_digit to an mp_int.
 *
 * a  A single precision integer.
 * r  A multi-precision integer.
 */
static int sp_3072_to_mp(sp_digit* a, mp_int* r)
{
    int err;

    err = mp_grow(r, (3072 + DIGIT_BIT - 1) / DIGIT_BIT);
    if (err == MP_OKAY) {
#if DIGIT_BIT == 64
        XMEMCPY(r->dp, a, sizeof(sp_digit) * 48);
        r->used = 48;
        mp_clamp(r);
#elif DIGIT_BIT < 64
        int i, j = 0, s = 0;

        r->dp[0] = 0;
        for (i = 0; i < 48; i++) {
            r->dp[j] |= a[i] << s;
            r->dp[j] &= (1l << DIGIT_BIT) - 1;
            s = DIGIT_BIT - s;
            r->dp[++j] = a[i] >> s;
            while (s + DIGIT_BIT <= 64) {
                s += DIGIT_BIT;
                r->dp[j] &= (1l << DIGIT_BIT) - 1;
                r->dp[++j] = a[i] >> s;
            }
            s = 64 - s;
        }
        r->used = (3072 + DIGIT_BIT - 1) / DIGIT_BIT;
        mp_clamp(r);
#else
        int i, j = 0, s = 0;

        r->dp[0] = 0;
        for (i = 0; i < 48; i++) {
            r->dp[j] |= ((mp_digit)a[i]) << s;
            if (s + 64 >= DIGIT_BIT) {
    #if DIGIT_BIT < 64
                r->dp[j] &= (1l << DIGIT_BIT) - 1;
    #endif
                s = DIGIT_BIT - s;
                r->dp[++j] = a[i] >> s;
                s = 64 - s;
            }
            else
                s += 64;
        }
        r->used = (3072 + DIGIT_BIT - 1) / DIGIT_BIT;
        mp_clamp(r);
#endif
    }

    return err;
}

/* Perform the modular exponentiation for Diffie-Hellman.
 *
 * base  Base. MP integer.
 * exp   Exponent. MP integer.
 * mod   Modulus. MP integer.
 * res   Result. MP integer.
 * returs 0 on success, MP_READ_E if there are too many bytes in an array
 * and MEMORY_E if memory allocation fails.
 */
int sp_ModExp_3072(mp_int* base, mp_int* exp, mp_int* mod, mp_int* res)
{
    int err = MP_OKAY;
    sp_digit b[96], e[48], m[48];
    sp_digit* r = b;
#ifdef HAVE_INTEL_AVX2
    word32 cpuid_flags = cpuid_get_flags();
#endif
    int expBits = mp_count_bits(exp);

    if (mp_count_bits(base) > 3072 || expBits > 3072 ||
                                                   mp_count_bits(mod) != 3072) {
        err = MP_READ_E;
    }

    if (err == MP_OKAY) {
        sp_3072_from_mp(b, 48, base);
        sp_3072_from_mp(e, 48, exp);
        sp_3072_from_mp(m, 48, mod);

#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
            err = sp_3072_mod_exp_avx2_48(r, b, e, expBits, m, 0);
        else
#endif
            err = sp_3072_mod_exp_48(r, b, e, expBits, m, 0);
    }

    if (err == MP_OKAY) {
        err = sp_3072_to_mp(r, res);
    }

    XMEMSET(e, 0, sizeof(e));

    return err;
}

/* Perform the modular exponentiation for Diffie-Hellman.
 *
 * base     Base.
 * exp      Array of bytes that is the exponent.
 * expLen   Length of data, in bytes, in exponent.
 * mod      Modulus.
 * out      Buffer to hold big-endian bytes of exponentiation result.
 *          Must be at least 384 bytes long.
 * outLen   Length, in bytes, of exponentiation result.
 * returs 0 on success, MP_READ_E if there are too many bytes in an array
 * and MEMORY_E if memory allocation fails.
 */
int sp_DhExp_3072(mp_int* base, const byte* exp, word32 expLen,
    mp_int* mod, byte* out, word32* outLen)
{
    int err = MP_OKAY;
    sp_digit b[96], e[48], m[48];
    sp_digit* r = b;
    word32 i;
#ifdef HAVE_INTEL_AVX2
    word32 cpuid_flags = cpuid_get_flags();
#endif

    if (mp_count_bits(base) > 3072 || expLen > 384 ||
                                                   mp_count_bits(mod) != 3072) {
        err = MP_READ_E;
    }

    if (err == MP_OKAY) {
        sp_3072_from_mp(b, 48, base);
        sp_3072_from_bin(e, 48, exp, expLen);
        sp_3072_from_mp(m, 48, mod);

#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
            err = sp_3072_mod_exp_avx2_48(r, b, e, expLen * 8, m, 0);
        else
#endif
            err = sp_3072_mod_exp_48(r, b, e, expLen * 8, m, 0);
    }

    if (err == MP_OKAY) {
        sp_3072_to_bin(r, out);
        *outLen = 384;
        for (i=0; i<384 && out[i] == 0; i++) {
        }
        *outLen -= i;
        XMEMMOVE(out, out + i, *outLen);
    }

    XMEMSET(e, 0, sizeof(e));

    return err;
}
/* Perform the modular exponentiation for Diffie-Hellman.
 *
 * base  Base. MP integer.
 * exp   Exponent. MP integer.
 * mod   Modulus. MP integer.
 * res   Result. MP integer.
 * returs 0 on success, MP_READ_E if there are too many bytes in an array
 * and MEMORY_E if memory allocation fails.
 */
int sp_ModExp_1536(mp_int* base, mp_int* exp, mp_int* mod, mp_int* res)
{
    int err = MP_OKAY;
    sp_digit b[48], e[24], m[24];
    sp_digit* r = b;
#ifdef HAVE_INTEL_AVX2
    word32 cpuid_flags = cpuid_get_flags();
#endif
    int expBits = mp_count_bits(exp);

    if (mp_count_bits(base) > 1536 || expBits > 1536 ||
                                                   mp_count_bits(mod) != 1536) {
        err = MP_READ_E;
    }

    if (err == MP_OKAY) {
        sp_3072_from_mp(b, 24, base);
        sp_3072_from_mp(e, 24, exp);
        sp_3072_from_mp(m, 24, mod);

#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
            err = sp_3072_mod_exp_avx2_24(r, b, e, expBits, m, 0);
        else
#endif
            err = sp_3072_mod_exp_24(r, b, e, expBits, m, 0);
    }

    if (err == MP_OKAY) {
        XMEMSET(r + 24, 0, sizeof(*r) * 24);
        err = sp_3072_to_mp(r, res);
    }

    XMEMSET(e, 0, sizeof(e));

    return err;
}

#endif /* WOLFSSL_HAVE_SP_DH || (WOLFSSL_HAVE_SP_RSA && !WOLFSSL_RSA_PUBLIC_ONLY) */

#endif /* !WOLFSSL_SP_NO_3072 */

#endif /* WOLFSSL_HAVE_SP_RSA || WOLFSSL_HAVE_SP_DH */
#ifdef WOLFSSL_HAVE_SP_ECC
#ifndef WOLFSSL_SP_NO_256

/* Point structure to use. */
typedef struct sp_point {
    sp_digit x[2 * 4];
    sp_digit y[2 * 4];
    sp_digit z[2 * 4];
    int infinity;
} sp_point;

/* The modulus (prime) of the curve P256. */
static sp_digit p256_mod[4] = {
    0xffffffffffffffffl,0x00000000ffffffffl,0x0000000000000000l,
    0xffffffff00000001l
};
/* The Montogmery normalizer for modulus of the curve P256. */
static sp_digit p256_norm_mod[4] = {
    0x0000000000000001l,0xffffffff00000000l,0xffffffffffffffffl,
    0x00000000fffffffel
};
/* The Montogmery multiplier for modulus of the curve P256. */
static sp_digit p256_mp_mod = 0x0000000000000001;
#if defined(WOLFSSL_VALIDATE_ECC_KEYGEN) || defined(HAVE_ECC_SIGN) || \
                                            defined(HAVE_ECC_VERIFY)
/* The order of the curve P256. */
static sp_digit p256_order[4] = {
    0xf3b9cac2fc632551l,0xbce6faada7179e84l,0xffffffffffffffffl,
    0xffffffff00000000l
};
#endif
/* The order of the curve P256 minus 2. */
static sp_digit p256_order2[4] = {
    0xf3b9cac2fc63254fl,0xbce6faada7179e84l,0xffffffffffffffffl,
    0xffffffff00000000l
};
#if defined(HAVE_ECC_SIGN) || defined(HAVE_ECC_VERIFY)
/* The Montogmery normalizer for order of the curve P256. */
static sp_digit p256_norm_order[4] = {
    0x0c46353d039cdaafl,0x4319055258e8617bl,0x0000000000000000l,
    0x00000000ffffffffl
};
#endif
#if defined(HAVE_ECC_SIGN) || defined(HAVE_ECC_VERIFY)
/* The Montogmery multiplier for order of the curve P256. */
static sp_digit p256_mp_order = 0xccd1c8aaee00bc4fl;
#endif
#ifdef WOLFSSL_SP_SMALL
/* The base point of curve P256. */
static sp_point p256_base = {
    /* X ordinate */
    {
        0xf4a13945d898c296l,0x77037d812deb33a0l,0xf8bce6e563a440f2l,
        0x6b17d1f2e12c4247l
    },
    /* Y ordinate */
    {
        0xcbb6406837bf51f5l,0x2bce33576b315ecel,0x8ee7eb4a7c0f9e16l,
        0x4fe342e2fe1a7f9bl
    },
    /* Z ordinate */
    {
        0x0000000000000001l,0x0000000000000000l,0x0000000000000000l,
        0x0000000000000000l
    },
    /* infinity */
    0
};
#endif /* WOLFSSL_SP_SMALL */
#if defined(HAVE_ECC_CHECK_KEY) || defined(HAVE_COMP_KEY)
static sp_digit p256_b[4] = {
    0x3bce3c3e27d2604bl,0x651d06b0cc53b0f6l,0xb3ebbd55769886bcl,
    0x5ac635d8aa3a93e7l
};
#endif

#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
/* Allocate memory for point and return error. */
#define sp_ecc_point_new(heap, sp, p)                                         \
    ((p = (sp_point*)XMALLOC(sizeof(sp_point), heap, DYNAMIC_TYPE_ECC))       \
                                                 == NULL) ? MEMORY_E : MP_OKAY
#else
/* Set pointer to data and return no error. */
#define sp_ecc_point_new(heap, sp, p)   ((p = &sp) == NULL) ? MEMORY_E : MP_OKAY
#endif

#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
/* If valid pointer then clear point data if requested and free data. */
#define sp_ecc_point_free(p, clear, heap)     \
    do {                                      \
        if (p != NULL) {                      \
            if (clear)                        \
                XMEMSET(p, 0, sizeof(*p));    \
            XFREE(p, heap, DYNAMIC_TYPE_ECC); \
        }                                     \
    }                                         \
    while (0)
#else
/* Clear point data if requested. */
#define sp_ecc_point_free(p, clear, heap) \
    do {                                  \
        if (clear)                        \
            XMEMSET(p, 0, sizeof(*p));    \
    }                                     \
    while (0)
#endif

/* Multiply a number by Montogmery normalizer mod modulus (prime).
 *
 * r  The resulting Montgomery form number.
 * a  The number to convert.
 * m  The modulus (prime).
 */
static int sp_256_mod_mul_norm_4(sp_digit* r, sp_digit* a, sp_digit* m)
{
    int64_t t[8];
    int64_t a32[8];
    int64_t o;

    (void)m;

    a32[0] = a[0] & 0xffffffff;
    a32[1] = a[0] >> 32;
    a32[2] = a[1] & 0xffffffff;
    a32[3] = a[1] >> 32;
    a32[4] = a[2] & 0xffffffff;
    a32[5] = a[2] >> 32;
    a32[6] = a[3] & 0xffffffff;
    a32[7] = a[3] >> 32;

    /*  1  1  0 -1 -1 -1 -1  0 */
    t[0] = 0 + a32[0] + a32[1] - a32[3] - a32[4] - a32[5] - a32[6];
    /*  0  1  1  0 -1 -1 -1 -1 */
    t[1] = 0 + a32[1] + a32[2] - a32[4] - a32[5] - a32[6] - a32[7];
    /*  0  0  1  1  0 -1 -1 -1 */
    t[2] = 0 + a32[2] + a32[3] - a32[5] - a32[6] - a32[7];
    /* -1 -1  0  2  2  1  0 -1 */
    t[3] = 0 - a32[0] - a32[1] + 2 * a32[3] + 2 * a32[4] + a32[5] - a32[7];
    /*  0 -1 -1  0  2  2  1  0 */
    t[4] = 0 - a32[1] - a32[2] + 2 * a32[4] + 2 * a32[5] + a32[6];
    /*  0  0 -1 -1  0  2  2  1 */
    t[5] = 0 - a32[2] - a32[3] + 2 * a32[5] + 2 * a32[6] + a32[7];
    /* -1 -1  0  0  0  1  3  2 */
    t[6] = 0 - a32[0] - a32[1] + a32[5] + 3 * a32[6] + 2 * a32[7];
    /*  1  0 -1 -1 -1 -1  0  3 */
    t[7] = 0 + a32[0] - a32[2] - a32[3] - a32[4] - a32[5] + 3 * a32[7];

    t[1] += t[0] >> 32; t[0] &= 0xffffffff;
    t[2] += t[1] >> 32; t[1] &= 0xffffffff;
    t[3] += t[2] >> 32; t[2] &= 0xffffffff;
    t[4] += t[3] >> 32; t[3] &= 0xffffffff;
    t[5] += t[4] >> 32; t[4] &= 0xffffffff;
    t[6] += t[5] >> 32; t[5] &= 0xffffffff;
    t[7] += t[6] >> 32; t[6] &= 0xffffffff;
    o     = t[7] >> 32; t[7] &= 0xffffffff;
    t[0] += o;
    t[3] -= o;
    t[6] -= o;
    t[7] += o;
    t[1] += t[0] >> 32; t[0] &= 0xffffffff;
    t[2] += t[1] >> 32; t[1] &= 0xffffffff;
    t[3] += t[2] >> 32; t[2] &= 0xffffffff;
    t[4] += t[3] >> 32; t[3] &= 0xffffffff;
    t[5] += t[4] >> 32; t[4] &= 0xffffffff;
    t[6] += t[5] >> 32; t[5] &= 0xffffffff;
    t[7] += t[6] >> 32; t[6] &= 0xffffffff;
    r[0] = (t[1] << 32) | t[0];
    r[1] = (t[3] << 32) | t[2];
    r[2] = (t[5] << 32) | t[4];
    r[3] = (t[7] << 32) | t[6];

    return MP_OKAY;
}

/* Convert an mp_int to an array of sp_digit.
 *
 * r  A single precision integer.
 * a  A multi-precision integer.
 */
static void sp_256_from_mp(sp_digit* r, int max, mp_int* a)
{
#if DIGIT_BIT == 64
    int j;

    XMEMCPY(r, a->dp, sizeof(sp_digit) * a->used);

    for (j = a->used; j < max; j++)
        r[j] = 0;
#elif DIGIT_BIT > 64
    int i, j = 0, s = 0;

    r[0] = 0;
    for (i = 0; i < a->used && j < max; i++) {
        r[j] |= a->dp[i] << s;
        r[j] &= 0xffffffffffffffffl;
        s = 64 - s;
        if (j + 1 >= max)
            break;
        r[++j] = a->dp[i] >> s;
        while (s + 64 <= DIGIT_BIT) {
            s += 64;
            r[j] &= 0xffffffffffffffffl;
            if (j + 1 >= max)
                break;
            if (s < DIGIT_BIT)
                r[++j] = a->dp[i] >> s;
            else
                r[++j] = 0;
        }
        s = DIGIT_BIT - s;
    }

    for (j++; j < max; j++)
        r[j] = 0;
#else
    int i, j = 0, s = 0;

    r[0] = 0;
    for (i = 0; i < a->used && j < max; i++) {
        r[j] |= ((sp_digit)a->dp[i]) << s;
        if (s + DIGIT_BIT >= 64) {
            r[j] &= 0xffffffffffffffffl;
            if (j + 1 >= max)
                break;
            s = 64 - s;
            if (s == DIGIT_BIT) {
                r[++j] = 0;
                s = 0;
            }
            else {
                r[++j] = a->dp[i] >> s;
                s = DIGIT_BIT - s;
            }
        }
        else
            s += DIGIT_BIT;
    }

    for (j++; j < max; j++)
        r[j] = 0;
#endif
}

/* Convert a point of type ecc_point to type sp_point.
 *
 * p   Point of type sp_point (result).
 * pm  Point of type ecc_point.
 */
static void sp_256_point_from_ecc_point_4(sp_point* p, ecc_point* pm)
{
    XMEMSET(p->x, 0, sizeof(p->x));
    XMEMSET(p->y, 0, sizeof(p->y));
    XMEMSET(p->z, 0, sizeof(p->z));
    sp_256_from_mp(p->x, 4, pm->x);
    sp_256_from_mp(p->y, 4, pm->y);
    sp_256_from_mp(p->z, 4, pm->z);
    p->infinity = 0;
}

/* Convert an array of sp_digit to an mp_int.
 *
 * a  A single precision integer.
 * r  A multi-precision integer.
 */
static int sp_256_to_mp(sp_digit* a, mp_int* r)
{
    int err;

    err = mp_grow(r, (256 + DIGIT_BIT - 1) / DIGIT_BIT);
    if (err == MP_OKAY) {
#if DIGIT_BIT == 64
        XMEMCPY(r->dp, a, sizeof(sp_digit) * 4);
        r->used = 4;
        mp_clamp(r);
#elif DIGIT_BIT < 64
        int i, j = 0, s = 0;

        r->dp[0] = 0;
        for (i = 0; i < 4; i++) {
            r->dp[j] |= a[i] << s;
            r->dp[j] &= (1l << DIGIT_BIT) - 1;
            s = DIGIT_BIT - s;
            r->dp[++j] = a[i] >> s;
            while (s + DIGIT_BIT <= 64) {
                s += DIGIT_BIT;
                r->dp[j] &= (1l << DIGIT_BIT) - 1;
                r->dp[++j] = a[i] >> s;
            }
            s = 64 - s;
        }
        r->used = (256 + DIGIT_BIT - 1) / DIGIT_BIT;
        mp_clamp(r);
#else
        int i, j = 0, s = 0;

        r->dp[0] = 0;
        for (i = 0; i < 4; i++) {
            r->dp[j] |= ((mp_digit)a[i]) << s;
            if (s + 64 >= DIGIT_BIT) {
    #if DIGIT_BIT < 64
                r->dp[j] &= (1l << DIGIT_BIT) - 1;
    #endif
                s = DIGIT_BIT - s;
                r->dp[++j] = a[i] >> s;
                s = 64 - s;
            }
            else
                s += 64;
        }
        r->used = (256 + DIGIT_BIT - 1) / DIGIT_BIT;
        mp_clamp(r);
#endif
    }

    return err;
}

/* Convert a point of type sp_point to type ecc_point.
 *
 * p   Point of type sp_point.
 * pm  Point of type ecc_point (result).
 * returns MEMORY_E when allocation of memory in ecc_point fails otherwise
 * MP_OKAY.
 */
static int sp_256_point_to_ecc_point_4(sp_point* p, ecc_point* pm)
{
    int err;

    err = sp_256_to_mp(p->x, pm->x);
    if (err == MP_OKAY)
        err = sp_256_to_mp(p->y, pm->y);
    if (err == MP_OKAY)
        err = sp_256_to_mp(p->z, pm->z);

    return err;
}

extern void sp_256_cond_copy_4(sp_digit* r, sp_digit* a, sp_digit m);
extern int64_t sp_256_cmp_4(sp_digit* a, sp_digit* b);
/* Normalize the values in each word to 64.
 *
 * a  Array of sp_digit to normalize.
 */
#define sp_256_norm_4(a)

extern sp_digit sp_256_cond_sub_4(sp_digit* r, sp_digit* a, sp_digit* b, sp_digit m);
extern sp_digit sp_256_sub_4(sp_digit* r, const sp_digit* a, const sp_digit* b);
#define sp_256_mont_reduce_order_4         sp_256_mont_reduce_4

extern void sp_256_mont_reduce_4(sp_digit* a, sp_digit* m, sp_digit mp);
extern void sp_256_mont_mul_4(sp_digit* r, sp_digit* a, sp_digit* b, sp_digit* m, sp_digit mp);
extern void sp_256_mont_sqr_4(sp_digit* r, sp_digit* a, sp_digit* m, sp_digit mp);
#if !defined(WOLFSSL_SP_SMALL) || defined(HAVE_COMP_KEY)
/* Square the Montgomery form number a number of times. (r = a ^ n mod m)
 *
 * r   Result of squaring.
 * a   Number to square in Montogmery form.
 * n   Number of times to square.
 * m   Modulus (prime).
 * mp  Montogmery mulitplier.
 */
static void sp_256_mont_sqr_n_4(sp_digit* r, sp_digit* a, int n,
        sp_digit* m, sp_digit mp)
{
    sp_256_mont_sqr_4(r, a, m, mp);
    for (; n > 1; n--)
        sp_256_mont_sqr_4(r, r, m, mp);
}

#endif /* !WOLFSSL_SP_SMALL || HAVE_COMP_KEY */
#ifdef WOLFSSL_SP_SMALL
/* Mod-2 for the P256 curve. */
static const uint64_t p256_mod_2[4] = {
    0xfffffffffffffffd,0x00000000ffffffff,0x0000000000000000,
    0xffffffff00000001
};
#endif /* !WOLFSSL_SP_SMALL */

/* Invert the number, in Montgomery form, modulo the modulus (prime) of the
 * P256 curve. (r = 1 / a mod m)
 *
 * r   Inverse result.
 * a   Number to invert.
 * td  Temporary data.
 */
static void sp_256_mont_inv_4(sp_digit* r, sp_digit* a, sp_digit* td)
{
#ifdef WOLFSSL_SP_SMALL
    sp_digit* t = td;
    int i;

    XMEMCPY(t, a, sizeof(sp_digit) * 4);
    for (i=254; i>=0; i--) {
        sp_256_mont_sqr_4(t, t, p256_mod, p256_mp_mod);
        if (p256_mod_2[i / 64] & ((sp_digit)1 << (i % 64)))
            sp_256_mont_mul_4(t, t, a, p256_mod, p256_mp_mod);
    }
    XMEMCPY(r, t, sizeof(sp_digit) * 4);
#else
    sp_digit* t = td;
    sp_digit* t2 = td + 2 * 4;
    sp_digit* t3 = td + 4 * 4;

    /* t = a^2 */
    sp_256_mont_sqr_4(t, a, p256_mod, p256_mp_mod);
    /* t = a^3 = t * a */
    sp_256_mont_mul_4(t, t, a, p256_mod, p256_mp_mod);
    /* t2= a^c = t ^ 2 ^ 2 */
    sp_256_mont_sqr_n_4(t2, t, 2, p256_mod, p256_mp_mod);
    /* t3= a^d = t2 * a */
    sp_256_mont_mul_4(t3, t2, a, p256_mod, p256_mp_mod);
    /* t = a^f = t2 * t */
    sp_256_mont_mul_4(t, t2, t, p256_mod, p256_mp_mod);
    /* t2= a^f0 = t ^ 2 ^ 4 */
    sp_256_mont_sqr_n_4(t2, t, 4, p256_mod, p256_mp_mod);
    /* t3= a^fd = t2 * t3 */
    sp_256_mont_mul_4(t3, t2, t3, p256_mod, p256_mp_mod);
    /* t = a^ff = t2 * t */
    sp_256_mont_mul_4(t, t2, t, p256_mod, p256_mp_mod);
    /* t2= a^ff00 = t ^ 2 ^ 8 */
    sp_256_mont_sqr_n_4(t2, t, 8, p256_mod, p256_mp_mod);
    /* t3= a^fffd = t2 * t3 */
    sp_256_mont_mul_4(t3, t2, t3, p256_mod, p256_mp_mod);
    /* t = a^ffff = t2 * t */
    sp_256_mont_mul_4(t, t2, t, p256_mod, p256_mp_mod);
    /* t2= a^ffff0000 = t ^ 2 ^ 16 */
    sp_256_mont_sqr_n_4(t2, t, 16, p256_mod, p256_mp_mod);
    /* t3= a^fffffffd = t2 * t3 */
    sp_256_mont_mul_4(t3, t2, t3, p256_mod, p256_mp_mod);
    /* t = a^ffffffff = t2 * t */
    sp_256_mont_mul_4(t, t2, t, p256_mod, p256_mp_mod);
    /* t = a^ffffffff00000000 = t ^ 2 ^ 32  */
    sp_256_mont_sqr_n_4(t2, t, 32, p256_mod, p256_mp_mod);
    /* t2= a^ffffffffffffffff = t2 * t */
    sp_256_mont_mul_4(t, t2, t, p256_mod, p256_mp_mod);
    /* t2= a^ffffffff00000001 = t2 * a */
    sp_256_mont_mul_4(t2, t2, a, p256_mod, p256_mp_mod);
    /* t2= a^ffffffff000000010000000000000000000000000000000000000000
     *   = t2 ^ 2 ^ 160 */
    sp_256_mont_sqr_n_4(t2, t2, 160, p256_mod, p256_mp_mod);
    /* t2= a^ffffffff00000001000000000000000000000000ffffffffffffffff
     *   = t2 * t */
    sp_256_mont_mul_4(t2, t2, t, p256_mod, p256_mp_mod);
    /* t2= a^ffffffff00000001000000000000000000000000ffffffffffffffff00000000
     *   = t2 ^ 2 ^ 32 */
    sp_256_mont_sqr_n_4(t2, t2, 32, p256_mod, p256_mp_mod);
    /* r = a^ffffffff00000001000000000000000000000000fffffffffffffffffffffffd
     *   = t2 * t3 */
    sp_256_mont_mul_4(r, t2, t3, p256_mod, p256_mp_mod);
#endif /* WOLFSSL_SP_SMALL */
}

/* Map the Montgomery form projective co-ordinate point to an affine point.
 *
 * r  Resulting affine co-ordinate point.
 * p  Montgomery form projective co-ordinate point.
 * t  Temporary ordinate data.
 */
static void sp_256_map_4(sp_point* r, sp_point* p, sp_digit* t)
{
    sp_digit* t1 = t;
    sp_digit* t2 = t + 2*4;
    int64_t n;

    sp_256_mont_inv_4(t1, p->z, t + 2*4);

    sp_256_mont_sqr_4(t2, t1, p256_mod, p256_mp_mod);
    sp_256_mont_mul_4(t1, t2, t1, p256_mod, p256_mp_mod);

    /* x /= z^2 */
    sp_256_mont_mul_4(r->x, p->x, t2, p256_mod, p256_mp_mod);
    XMEMSET(r->x + 4, 0, sizeof(r->x) / 2);
    sp_256_mont_reduce_4(r->x, p256_mod, p256_mp_mod);
    /* Reduce x to less than modulus */
    n = sp_256_cmp_4(r->x, p256_mod);
    sp_256_cond_sub_4(r->x, r->x, p256_mod, 0 - (n >= 0));
    sp_256_norm_4(r->x);

    /* y /= z^3 */
    sp_256_mont_mul_4(r->y, p->y, t1, p256_mod, p256_mp_mod);
    XMEMSET(r->y + 4, 0, sizeof(r->y) / 2);
    sp_256_mont_reduce_4(r->y, p256_mod, p256_mp_mod);
    /* Reduce y to less than modulus */
    n = sp_256_cmp_4(r->y, p256_mod);
    sp_256_cond_sub_4(r->y, r->y, p256_mod, 0 - (n >= 0));
    sp_256_norm_4(r->y);

    XMEMSET(r->z, 0, sizeof(r->z));
    r->z[0] = 1;

}

extern void sp_256_mont_add_4(sp_digit* r, sp_digit* a, sp_digit* b, sp_digit* m);
extern void sp_256_mont_dbl_4(sp_digit* r, sp_digit* a, sp_digit* m);
extern void sp_256_mont_tpl_4(sp_digit* r, sp_digit* a, sp_digit* m);
extern void sp_256_mont_sub_4(sp_digit* r, sp_digit* a, sp_digit* b, sp_digit* m);
extern void sp_256_div2_4(sp_digit* r, const sp_digit* a, const sp_digit* m);
/* Double the Montgomery form projective point p.
 *
 * r  Result of doubling point.
 * p  Point to double.
 * t  Temporary ordinate data.
 */
static void sp_256_proj_point_dbl_4(sp_point* r, sp_point* p, sp_digit* t)
{
    sp_point* rp[2];
    sp_digit* t1 = t;
    sp_digit* t2 = t + 2*4;
    sp_digit* x;
    sp_digit* y;
    sp_digit* z;
    int i;

    /* When infinity don't double point passed in - constant time. */
    rp[0] = r;
    rp[1] = (sp_point*)t;
    XMEMSET(rp[1], 0, sizeof(sp_point));
    x = rp[p->infinity]->x;
    y = rp[p->infinity]->y;
    z = rp[p->infinity]->z;
    /* Put point to double into result - good for infinty. */
    if (r != p) {
        for (i=0; i<4; i++)
            r->x[i] = p->x[i];
        for (i=0; i<4; i++)
            r->y[i] = p->y[i];
        for (i=0; i<4; i++)
            r->z[i] = p->z[i];
        r->infinity = p->infinity;
    }

    /* T1 = Z * Z */
    sp_256_mont_sqr_4(t1, z, p256_mod, p256_mp_mod);
    /* Z = Y * Z */
    sp_256_mont_mul_4(z, y, z, p256_mod, p256_mp_mod);
    /* Z = 2Z */
    sp_256_mont_dbl_4(z, z, p256_mod);
    /* T2 = X - T1 */
    sp_256_mont_sub_4(t2, x, t1, p256_mod);
    /* T1 = X + T1 */
    sp_256_mont_add_4(t1, x, t1, p256_mod);
    /* T2 = T1 * T2 */
    sp_256_mont_mul_4(t2, t1, t2, p256_mod, p256_mp_mod);
    /* T1 = 3T2 */
    sp_256_mont_tpl_4(t1, t2, p256_mod);
    /* Y = 2Y */
    sp_256_mont_dbl_4(y, y, p256_mod);
    /* Y = Y * Y */
    sp_256_mont_sqr_4(y, y, p256_mod, p256_mp_mod);
    /* T2 = Y * Y */
    sp_256_mont_sqr_4(t2, y, p256_mod, p256_mp_mod);
    /* T2 = T2/2 */
    sp_256_div2_4(t2, t2, p256_mod);
    /* Y = Y * X */
    sp_256_mont_mul_4(y, y, x, p256_mod, p256_mp_mod);
    /* X = T1 * T1 */
    sp_256_mont_mul_4(x, t1, t1, p256_mod, p256_mp_mod);
    /* X = X - Y */
    sp_256_mont_sub_4(x, x, y, p256_mod);
    /* X = X - Y */
    sp_256_mont_sub_4(x, x, y, p256_mod);
    /* Y = Y - X */
    sp_256_mont_sub_4(y, y, x, p256_mod);
    /* Y = Y * T1 */
    sp_256_mont_mul_4(y, y, t1, p256_mod, p256_mp_mod);
    /* Y = Y - T2 */
    sp_256_mont_sub_4(y, y, t2, p256_mod);

}

/* Double the Montgomery form projective point p a number of times.
 *
 * r  Result of repeated doubling of point.
 * p  Point to double.
 * n  Number of times to double
 * t  Temporary ordinate data.
 */
static void sp_256_proj_point_dbl_n_4(sp_point* r, sp_point* p, int n,
        sp_digit* t)
{
    sp_point* rp[2];
    sp_digit* w = t;
    sp_digit* a = t + 2*4;
    sp_digit* b = t + 4*4;
    sp_digit* t1 = t + 6*4;
    sp_digit* t2 = t + 8*4;
    sp_digit* x;
    sp_digit* y;
    sp_digit* z;
    int i;

    rp[0] = r;
    rp[1] = (sp_point*)t;
    XMEMSET(rp[1], 0, sizeof(sp_point));
    x = rp[p->infinity]->x;
    y = rp[p->infinity]->y;
    z = rp[p->infinity]->z;
    if (r != p) {
        for (i=0; i<4; i++)
            r->x[i] = p->x[i];
        for (i=0; i<4; i++)
            r->y[i] = p->y[i];
        for (i=0; i<4; i++)
            r->z[i] = p->z[i];
        r->infinity = p->infinity;
    }

    /* Y = 2*Y */
    sp_256_mont_dbl_4(y, y, p256_mod);
    /* W = Z^4 */
    sp_256_mont_sqr_4(w, z, p256_mod, p256_mp_mod);
    sp_256_mont_sqr_4(w, w, p256_mod, p256_mp_mod);
    while (n--) {
        /* A = 3*(X^2 - W) */
        sp_256_mont_sqr_4(t1, x, p256_mod, p256_mp_mod);
        sp_256_mont_sub_4(t1, t1, w, p256_mod);
        sp_256_mont_tpl_4(a, t1, p256_mod);
        /* B = X*Y^2 */
        sp_256_mont_sqr_4(t2, y, p256_mod, p256_mp_mod);
        sp_256_mont_mul_4(b, t2, x, p256_mod, p256_mp_mod);
        /* X = A^2 - 2B */
        sp_256_mont_sqr_4(x, a, p256_mod, p256_mp_mod);
        sp_256_mont_dbl_4(t1, b, p256_mod);
        sp_256_mont_sub_4(x, x, t1, p256_mod);
        /* Z = Z*Y */
        sp_256_mont_mul_4(z, z, y, p256_mod, p256_mp_mod);
        /* t2 = Y^4 */
        sp_256_mont_sqr_4(t2, t2, p256_mod, p256_mp_mod);
        if (n) {
            /* W = W*Y^4 */
            sp_256_mont_mul_4(w, w, t2, p256_mod, p256_mp_mod);
        }
        /* y = 2*A*(B - X) - Y^4 */
        sp_256_mont_sub_4(y, b, x, p256_mod);
        sp_256_mont_mul_4(y, y, a, p256_mod, p256_mp_mod);
        sp_256_mont_dbl_4(y, y, p256_mod);
        sp_256_mont_sub_4(y, y, t2, p256_mod);
    }
    /* Y = Y/2 */
    sp_256_div2_4(y, y, p256_mod);
}

/* Compare two numbers to determine if they are equal.
 * Constant time implementation.
 *
 * a  First number to compare.
 * b  Second number to compare.
 * returns 1 when equal and 0 otherwise.
 */
static int sp_256_cmp_equal_4(const sp_digit* a, const sp_digit* b)
{
    return ((a[0] ^ b[0]) | (a[1] ^ b[1]) | (a[2] ^ b[2]) | (a[3] ^ b[3])) == 0;
}

/* Add two Montgomery form projective points.
 *
 * r  Result of addition.
 * p  Frist point to add.
 * q  Second point to add.
 * t  Temporary ordinate data.
 */
static void sp_256_proj_point_add_4(sp_point* r, sp_point* p, sp_point* q,
        sp_digit* t)
{
    sp_point* ap[2];
    sp_point* rp[2];
    sp_digit* t1 = t;
    sp_digit* t2 = t + 2*4;
    sp_digit* t3 = t + 4*4;
    sp_digit* t4 = t + 6*4;
    sp_digit* t5 = t + 8*4;
    sp_digit* x;
    sp_digit* y;
    sp_digit* z;
    int i;

    /* Ensure only the first point is the same as the result. */
    if (q == r) {
        sp_point* a = p;
        p = q;
        q = a;
    }

    /* Check double */
    sp_256_sub_4(t1, p256_mod, q->y);
    sp_256_norm_4(t1);
    if (sp_256_cmp_equal_4(p->x, q->x) & sp_256_cmp_equal_4(p->z, q->z) &
        (sp_256_cmp_equal_4(p->y, q->y) | sp_256_cmp_equal_4(p->y, t1))) {
        sp_256_proj_point_dbl_4(r, p, t);
    }
    else {
        rp[0] = r;
        rp[1] = (sp_point*)t;
        XMEMSET(rp[1], 0, sizeof(sp_point));
        x = rp[p->infinity | q->infinity]->x;
        y = rp[p->infinity | q->infinity]->y;
        z = rp[p->infinity | q->infinity]->z;

        ap[0] = p;
        ap[1] = q;
        for (i=0; i<4; i++)
            r->x[i] = ap[p->infinity]->x[i];
        for (i=0; i<4; i++)
            r->y[i] = ap[p->infinity]->y[i];
        for (i=0; i<4; i++)
            r->z[i] = ap[p->infinity]->z[i];
        r->infinity = ap[p->infinity]->infinity;

        /* U1 = X1*Z2^2 */
        sp_256_mont_sqr_4(t1, q->z, p256_mod, p256_mp_mod);
        sp_256_mont_mul_4(t3, t1, q->z, p256_mod, p256_mp_mod);
        sp_256_mont_mul_4(t1, t1, x, p256_mod, p256_mp_mod);
        /* U2 = X2*Z1^2 */
        sp_256_mont_sqr_4(t2, z, p256_mod, p256_mp_mod);
        sp_256_mont_mul_4(t4, t2, z, p256_mod, p256_mp_mod);
        sp_256_mont_mul_4(t2, t2, q->x, p256_mod, p256_mp_mod);
        /* S1 = Y1*Z2^3 */
        sp_256_mont_mul_4(t3, t3, y, p256_mod, p256_mp_mod);
        /* S2 = Y2*Z1^3 */
        sp_256_mont_mul_4(t4, t4, q->y, p256_mod, p256_mp_mod);
        /* H = U2 - U1 */
        sp_256_mont_sub_4(t2, t2, t1, p256_mod);
        /* R = S2 - S1 */
        sp_256_mont_sub_4(t4, t4, t3, p256_mod);
        /* Z3 = H*Z1*Z2 */
        sp_256_mont_mul_4(z, z, q->z, p256_mod, p256_mp_mod);
        sp_256_mont_mul_4(z, z, t2, p256_mod, p256_mp_mod);
        /* X3 = R^2 - H^3 - 2*U1*H^2 */
        sp_256_mont_sqr_4(x, t4, p256_mod, p256_mp_mod);
        sp_256_mont_sqr_4(t5, t2, p256_mod, p256_mp_mod);
        sp_256_mont_mul_4(y, t1, t5, p256_mod, p256_mp_mod);
        sp_256_mont_mul_4(t5, t5, t2, p256_mod, p256_mp_mod);
        sp_256_mont_sub_4(x, x, t5, p256_mod);
        sp_256_mont_dbl_4(t1, y, p256_mod);
        sp_256_mont_sub_4(x, x, t1, p256_mod);
        /* Y3 = R*(U1*H^2 - X3) - S1*H^3 */
        sp_256_mont_sub_4(y, y, x, p256_mod);
        sp_256_mont_mul_4(y, y, t4, p256_mod, p256_mp_mod);
        sp_256_mont_mul_4(t5, t5, t3, p256_mod, p256_mp_mod);
        sp_256_mont_sub_4(y, y, t5, p256_mod);
    }
}

/* Double the Montgomery form projective point p a number of times.
 *
 * r  Result of repeated doubling of point.
 * p  Point to double.
 * n  Number of times to double
 * t  Temporary ordinate data.
 */
static void sp_256_proj_point_dbl_n_store_4(sp_point* r, sp_point* p,
        int n, int m, sp_digit* t)
{
    sp_digit* w = t;
    sp_digit* a = t + 2*4;
    sp_digit* b = t + 4*4;
    sp_digit* t1 = t + 6*4;
    sp_digit* t2 = t + 8*4;
    sp_digit* x = r[2*m].x;
    sp_digit* y = r[(1<<n)*m].y;
    sp_digit* z = r[2*m].z;
    int i;

    for (i=0; i<4; i++)
        x[i] = p->x[i];
    for (i=0; i<4; i++)
        y[i] = p->y[i];
    for (i=0; i<4; i++)
        z[i] = p->z[i];

    /* Y = 2*Y */
    sp_256_mont_dbl_4(y, y, p256_mod);
    /* W = Z^4 */
    sp_256_mont_sqr_4(w, z, p256_mod, p256_mp_mod);
    sp_256_mont_sqr_4(w, w, p256_mod, p256_mp_mod);
    for (i=1; i<=n; i++) {
        /* A = 3*(X^2 - W) */
        sp_256_mont_sqr_4(t1, x, p256_mod, p256_mp_mod);
        sp_256_mont_sub_4(t1, t1, w, p256_mod);
        sp_256_mont_tpl_4(a, t1, p256_mod);
        /* B = X*Y^2 */
        sp_256_mont_sqr_4(t2, y, p256_mod, p256_mp_mod);
        sp_256_mont_mul_4(b, t2, x, p256_mod, p256_mp_mod);
        x = r[(1<<i)*m].x;
        /* X = A^2 - 2B */
        sp_256_mont_sqr_4(x, a, p256_mod, p256_mp_mod);
        sp_256_mont_dbl_4(t1, b, p256_mod);
        sp_256_mont_sub_4(x, x, t1, p256_mod);
        /* Z = Z*Y */
        sp_256_mont_mul_4(r[(1<<i)*m].z, z, y, p256_mod, p256_mp_mod);
        z = r[(1<<i)*m].z;
        /* t2 = Y^4 */
        sp_256_mont_sqr_4(t2, t2, p256_mod, p256_mp_mod);
        if (i != n) {
            /* W = W*Y^4 */
            sp_256_mont_mul_4(w, w, t2, p256_mod, p256_mp_mod);
        }
        /* y = 2*A*(B - X) - Y^4 */
        sp_256_mont_sub_4(y, b, x, p256_mod);
        sp_256_mont_mul_4(y, y, a, p256_mod, p256_mp_mod);
        sp_256_mont_dbl_4(y, y, p256_mod);
        sp_256_mont_sub_4(y, y, t2, p256_mod);

        /* Y = Y/2 */
        sp_256_div2_4(r[(1<<i)*m].y, y, p256_mod);
        r[(1<<i)*m].infinity = 0;
    }
}

/* Add two Montgomery form projective points.
 *
 * ra  Result of addition.
 * rs  Result of subtraction.
 * p   Frist point to add.
 * q   Second point to add.
 * t   Temporary ordinate data.
 */
static void sp_256_proj_point_add_sub_4(sp_point* ra, sp_point* rs,
        sp_point* p, sp_point* q, sp_digit* t)
{
    sp_digit* t1 = t;
    sp_digit* t2 = t + 2*4;
    sp_digit* t3 = t + 4*4;
    sp_digit* t4 = t + 6*4;
    sp_digit* t5 = t + 8*4;
    sp_digit* t6 = t + 10*4;
    sp_digit* x = ra->x;
    sp_digit* y = ra->y;
    sp_digit* z = ra->z;
    sp_digit* xs = rs->x;
    sp_digit* ys = rs->y;
    sp_digit* zs = rs->z;


    XMEMCPY(x, p->x, sizeof(p->x) / 2);
    XMEMCPY(y, p->y, sizeof(p->y) / 2);
    XMEMCPY(z, p->z, sizeof(p->z) / 2);
    ra->infinity = 0;
    rs->infinity = 0;

    /* U1 = X1*Z2^2 */
    sp_256_mont_sqr_4(t1, q->z, p256_mod, p256_mp_mod);
    sp_256_mont_mul_4(t3, t1, q->z, p256_mod, p256_mp_mod);
    sp_256_mont_mul_4(t1, t1, x, p256_mod, p256_mp_mod);
    /* U2 = X2*Z1^2 */
    sp_256_mont_sqr_4(t2, z, p256_mod, p256_mp_mod);
    sp_256_mont_mul_4(t4, t2, z, p256_mod, p256_mp_mod);
    sp_256_mont_mul_4(t2, t2, q->x, p256_mod, p256_mp_mod);
    /* S1 = Y1*Z2^3 */
    sp_256_mont_mul_4(t3, t3, y, p256_mod, p256_mp_mod);
    /* S2 = Y2*Z1^3 */
    sp_256_mont_mul_4(t4, t4, q->y, p256_mod, p256_mp_mod);
    /* H = U2 - U1 */
    sp_256_mont_sub_4(t2, t2, t1, p256_mod);
    /* RS = S2 + S1 */
    sp_256_mont_add_4(t6, t4, t3, p256_mod);
    /* R = S2 - S1 */
    sp_256_mont_sub_4(t4, t4, t3, p256_mod);
    /* Z3 = H*Z1*Z2 */
    /* ZS = H*Z1*Z2 */
    sp_256_mont_mul_4(z, z, q->z, p256_mod, p256_mp_mod);
    sp_256_mont_mul_4(z, z, t2, p256_mod, p256_mp_mod);
    XMEMCPY(zs, z, sizeof(p->z)/2);
    /* X3 = R^2 - H^3 - 2*U1*H^2 */
    /* XS = RS^2 - H^3 - 2*U1*H^2 */
    sp_256_mont_sqr_4(x, t4, p256_mod, p256_mp_mod);
    sp_256_mont_sqr_4(xs, t6, p256_mod, p256_mp_mod);
    sp_256_mont_sqr_4(t5, t2, p256_mod, p256_mp_mod);
    sp_256_mont_mul_4(y, t1, t5, p256_mod, p256_mp_mod);
    sp_256_mont_mul_4(t5, t5, t2, p256_mod, p256_mp_mod);
    sp_256_mont_sub_4(x, x, t5, p256_mod);
    sp_256_mont_sub_4(xs, xs, t5, p256_mod);
    sp_256_mont_dbl_4(t1, y, p256_mod);
    sp_256_mont_sub_4(x, x, t1, p256_mod);
    sp_256_mont_sub_4(xs, xs, t1, p256_mod);
    /* Y3 = R*(U1*H^2 - X3) - S1*H^3 */
    /* YS = -RS*(U1*H^2 - XS) - S1*H^3 */
    sp_256_mont_sub_4(ys, y, xs, p256_mod);
    sp_256_mont_sub_4(y, y, x, p256_mod);
    sp_256_mont_mul_4(y, y, t4, p256_mod, p256_mp_mod);
    sp_256_sub_4(t6, p256_mod, t6);
    sp_256_mont_mul_4(ys, ys, t6, p256_mod, p256_mp_mod);
    sp_256_mont_mul_4(t5, t5, t3, p256_mod, p256_mp_mod);
    sp_256_mont_sub_4(y, y, t5, p256_mod);
    sp_256_mont_sub_4(ys, ys, t5, p256_mod);
}

/* Structure used to describe recoding of scalar multiplication. */
typedef struct ecc_recode {
    /* Index into pre-computation table. */
    uint8_t i;
    /* Use the negative of the point. */
    uint8_t neg;
} ecc_recode;

/* The index into pre-computation table to use. */
static uint8_t recode_index_4_6[66] = {
     0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17,
    16, 15, 14, 13, 12, 11, 10,  9,  8,  7,  6,  5,  4,  3,  2,  1,
     0,  1,
};

/* Whether to negate y-ordinate. */
static uint8_t recode_neg_4_6[66] = {
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
     1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
     0,  0,
};

/* Recode the scalar for multiplication using pre-computed values and
 * subtraction.
 *
 * k  Scalar to multiply by.
 * v  Vector of operations to peform.
 */
static void sp_256_ecc_recode_6_4(sp_digit* k, ecc_recode* v)
{
    int i, j;
    uint8_t y;
    int carry = 0;
    int o;
    sp_digit n;

    j = 0;
    n = k[j];
    o = 0;
    for (i=0; i<43; i++) {
        y = n;
        if (o + 6 < 64) {
            y &= 0x3f;
            n >>= 6;
            o += 6;
        }
        else if (o + 6 == 64) {
            n >>= 6;
            if (++j < 4)
                n = k[j];
            o = 0;
        }
        else if (++j < 4) {
            n = k[j];
            y |= (n << (64 - o)) & 0x3f;
            o -= 58;
            n >>= o;
        }

        y += carry;
        v[i].i = recode_index_4_6[y];
        v[i].neg = recode_neg_4_6[y];
        carry = (y >> 6) + v[i].neg;
    }
}

/* Multiply the point by the scalar and return the result.
 * If map is true then convert result to affine co-ordinates.
 *
 * r     Resulting point.
 * g     Point to multiply.
 * k     Scalar to multiply by.
 * map   Indicates whether to convert result to affine.
 * heap  Heap to use for allocation.
 * returns MEMORY_E when memory allocation fails and MP_OKAY on success.
 */
static int sp_256_ecc_mulmod_win_add_sub_4(sp_point* r, sp_point* g,
        sp_digit* k, int map, void* heap)
{
#if !defined(WOLFSSL_SP_SMALL) && !defined(WOLFSSL_SMALL_STACK)
    sp_point td[33];
    sp_point rtd, pd;
    sp_digit tmpd[2 * 4 * 6];
#endif
    sp_point* t;
    sp_point* rt;
    sp_point* p = NULL;
    sp_digit* tmp;
    sp_digit* negy;
    int i;
    ecc_recode v[43];
    int err;

    (void)heap;

    err = sp_ecc_point_new(heap, rtd, rt);
    if (err == MP_OKAY)
        err = sp_ecc_point_new(heap, pd, p);
#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    t = (sp_point*)XMALLOC(sizeof(sp_point) * 33, heap, DYNAMIC_TYPE_ECC);
    if (t == NULL)
        err = MEMORY_E;
    tmp = (sp_digit*)XMALLOC(sizeof(sp_digit) * 2 * 4 * 6, heap,
                             DYNAMIC_TYPE_ECC);
    if (tmp == NULL)
        err = MEMORY_E;
#else
    t = td;
    tmp = tmpd;
#endif


    if (err == MP_OKAY) {
        /* t[0] = {0, 0, 1} * norm */
        XMEMSET(&t[0], 0, sizeof(t[0]));
        t[0].infinity = 1;
        /* t[1] = {g->x, g->y, g->z} * norm */
        err = sp_256_mod_mul_norm_4(t[1].x, g->x, p256_mod);
    }
    if (err == MP_OKAY)
        err = sp_256_mod_mul_norm_4(t[1].y, g->y, p256_mod);
    if (err == MP_OKAY)
        err = sp_256_mod_mul_norm_4(t[1].z, g->z, p256_mod);

    if (err == MP_OKAY) {
        t[1].infinity = 0;
        /* t[2] ... t[32]  */
    sp_256_proj_point_dbl_n_store_4(t, &t[ 1], 5, 1, tmp);
    sp_256_proj_point_add_4(&t[ 3], &t[ 2], &t[ 1], tmp);
    sp_256_proj_point_dbl_4(&t[ 6], &t[ 3], tmp);
    sp_256_proj_point_add_sub_4(&t[ 7], &t[ 5], &t[ 6], &t[ 1], tmp);
    sp_256_proj_point_dbl_4(&t[10], &t[ 5], tmp);
    sp_256_proj_point_add_sub_4(&t[11], &t[ 9], &t[10], &t[ 1], tmp);
    sp_256_proj_point_dbl_4(&t[12], &t[ 6], tmp);
    sp_256_proj_point_dbl_4(&t[14], &t[ 7], tmp);
    sp_256_proj_point_add_sub_4(&t[15], &t[13], &t[14], &t[ 1], tmp);
    sp_256_proj_point_dbl_4(&t[18], &t[ 9], tmp);
    sp_256_proj_point_add_sub_4(&t[19], &t[17], &t[18], &t[ 1], tmp);
    sp_256_proj_point_dbl_4(&t[20], &t[10], tmp);
    sp_256_proj_point_dbl_4(&t[22], &t[11], tmp);
    sp_256_proj_point_add_sub_4(&t[23], &t[21], &t[22], &t[ 1], tmp);
    sp_256_proj_point_dbl_4(&t[24], &t[12], tmp);
    sp_256_proj_point_dbl_4(&t[26], &t[13], tmp);
    sp_256_proj_point_add_sub_4(&t[27], &t[25], &t[26], &t[ 1], tmp);
    sp_256_proj_point_dbl_4(&t[28], &t[14], tmp);
    sp_256_proj_point_dbl_4(&t[30], &t[15], tmp);
    sp_256_proj_point_add_sub_4(&t[31], &t[29], &t[30], &t[ 1], tmp);

        negy = t[0].y;

        sp_256_ecc_recode_6_4(k, v);

        i = 42;
        XMEMCPY(rt, &t[v[i].i], sizeof(sp_point));
        for (--i; i>=0; i--) {
            sp_256_proj_point_dbl_n_4(rt, rt, 6, tmp);

            XMEMCPY(p, &t[v[i].i], sizeof(sp_point));
            sp_256_sub_4(negy, p256_mod, p->y);
            sp_256_cond_copy_4(p->y, negy, (sp_digit)0 - v[i].neg);
            sp_256_proj_point_add_4(rt, rt, p, tmp);
        }

        if (map)
            sp_256_map_4(r, rt, tmp);
        else
            XMEMCPY(r, rt, sizeof(sp_point));
    }

#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (t != NULL)
        XFREE(t, heap, DYNAMIC_TYPE_ECC);
    if (tmp != NULL)
        XFREE(tmp, heap, DYNAMIC_TYPE_ECC);
#endif
    sp_ecc_point_free(p, 0, heap);
    sp_ecc_point_free(rt, 0, heap);

    return err;
}

#ifdef HAVE_INTEL_AVX2
extern void sp_256_mont_mul_avx2_4(sp_digit* r, sp_digit* a, sp_digit* b, sp_digit* m, sp_digit mp);
extern void sp_256_mont_sqr_avx2_4(sp_digit* r, sp_digit* a, sp_digit* m, sp_digit mp);
#if !defined(WOLFSSL_SP_SMALL) || defined(HAVE_COMP_KEY)
/* Square the Montgomery form number a number of times. (r = a ^ n mod m)
 *
 * r   Result of squaring.
 * a   Number to square in Montogmery form.
 * n   Number of times to square.
 * m   Modulus (prime).
 * mp  Montogmery mulitplier.
 */
static void sp_256_mont_sqr_n_avx2_4(sp_digit* r, sp_digit* a, int n,
        sp_digit* m, sp_digit mp)
{
    sp_256_mont_sqr_avx2_4(r, a, m, mp);
    for (; n > 1; n--)
        sp_256_mont_sqr_avx2_4(r, r, m, mp);
}

#endif /* !WOLFSSL_SP_SMALL || HAVE_COMP_KEY */

/* Invert the number, in Montgomery form, modulo the modulus (prime) of the
 * P256 curve. (r = 1 / a mod m)
 *
 * r   Inverse result.
 * a   Number to invert.
 * td  Temporary data.
 */
static void sp_256_mont_inv_avx2_4(sp_digit* r, sp_digit* a, sp_digit* td)
{
#ifdef WOLFSSL_SP_SMALL
    sp_digit* t = td;
    int i;

    XMEMCPY(t, a, sizeof(sp_digit) * 4);
    for (i=254; i>=0; i--) {
        sp_256_mont_sqr_avx2_4(t, t, p256_mod, p256_mp_mod);
        if (p256_mod_2[i / 64] & ((sp_digit)1 << (i % 64)))
            sp_256_mont_mul_avx2_4(t, t, a, p256_mod, p256_mp_mod);
    }
    XMEMCPY(r, t, sizeof(sp_digit) * 4);
#else
    sp_digit* t = td;
    sp_digit* t2 = td + 2 * 4;
    sp_digit* t3 = td + 4 * 4;

    /* t = a^2 */
    sp_256_mont_sqr_avx2_4(t, a, p256_mod, p256_mp_mod);
    /* t = a^3 = t * a */
    sp_256_mont_mul_avx2_4(t, t, a, p256_mod, p256_mp_mod);
    /* t2= a^c = t ^ 2 ^ 2 */
    sp_256_mont_sqr_n_avx2_4(t2, t, 2, p256_mod, p256_mp_mod);
    /* t3= a^d = t2 * a */
    sp_256_mont_mul_avx2_4(t3, t2, a, p256_mod, p256_mp_mod);
    /* t = a^f = t2 * t */
    sp_256_mont_mul_avx2_4(t, t2, t, p256_mod, p256_mp_mod);
    /* t2= a^f0 = t ^ 2 ^ 4 */
    sp_256_mont_sqr_n_avx2_4(t2, t, 4, p256_mod, p256_mp_mod);
    /* t3= a^fd = t2 * t3 */
    sp_256_mont_mul_avx2_4(t3, t2, t3, p256_mod, p256_mp_mod);
    /* t = a^ff = t2 * t */
    sp_256_mont_mul_avx2_4(t, t2, t, p256_mod, p256_mp_mod);
    /* t2= a^ff00 = t ^ 2 ^ 8 */
    sp_256_mont_sqr_n_avx2_4(t2, t, 8, p256_mod, p256_mp_mod);
    /* t3= a^fffd = t2 * t3 */
    sp_256_mont_mul_avx2_4(t3, t2, t3, p256_mod, p256_mp_mod);
    /* t = a^ffff = t2 * t */
    sp_256_mont_mul_avx2_4(t, t2, t, p256_mod, p256_mp_mod);
    /* t2= a^ffff0000 = t ^ 2 ^ 16 */
    sp_256_mont_sqr_n_avx2_4(t2, t, 16, p256_mod, p256_mp_mod);
    /* t3= a^fffffffd = t2 * t3 */
    sp_256_mont_mul_avx2_4(t3, t2, t3, p256_mod, p256_mp_mod);
    /* t = a^ffffffff = t2 * t */
    sp_256_mont_mul_avx2_4(t, t2, t, p256_mod, p256_mp_mod);
    /* t = a^ffffffff00000000 = t ^ 2 ^ 32  */
    sp_256_mont_sqr_n_avx2_4(t2, t, 32, p256_mod, p256_mp_mod);
    /* t2= a^ffffffffffffffff = t2 * t */
    sp_256_mont_mul_avx2_4(t, t2, t, p256_mod, p256_mp_mod);
    /* t2= a^ffffffff00000001 = t2 * a */
    sp_256_mont_mul_avx2_4(t2, t2, a, p256_mod, p256_mp_mod);
    /* t2= a^ffffffff000000010000000000000000000000000000000000000000
     *   = t2 ^ 2 ^ 160 */
    sp_256_mont_sqr_n_avx2_4(t2, t2, 160, p256_mod, p256_mp_mod);
    /* t2= a^ffffffff00000001000000000000000000000000ffffffffffffffff
     *   = t2 * t */
    sp_256_mont_mul_avx2_4(t2, t2, t, p256_mod, p256_mp_mod);
    /* t2= a^ffffffff00000001000000000000000000000000ffffffffffffffff00000000
     *   = t2 ^ 2 ^ 32 */
    sp_256_mont_sqr_n_avx2_4(t2, t2, 32, p256_mod, p256_mp_mod);
    /* r = a^ffffffff00000001000000000000000000000000fffffffffffffffffffffffd
     *   = t2 * t3 */
    sp_256_mont_mul_avx2_4(r, t2, t3, p256_mod, p256_mp_mod);
#endif /* WOLFSSL_SP_SMALL */
}

/* Map the Montgomery form projective co-ordinate point to an affine point.
 *
 * r  Resulting affine co-ordinate point.
 * p  Montgomery form projective co-ordinate point.
 * t  Temporary ordinate data.
 */
static void sp_256_map_avx2_4(sp_point* r, sp_point* p, sp_digit* t)
{
    sp_digit* t1 = t;
    sp_digit* t2 = t + 2*4;
    int64_t n;

    sp_256_mont_inv_avx2_4(t1, p->z, t + 2*4);

    sp_256_mont_sqr_avx2_4(t2, t1, p256_mod, p256_mp_mod);
    sp_256_mont_mul_avx2_4(t1, t2, t1, p256_mod, p256_mp_mod);

    /* x /= z^2 */
    sp_256_mont_mul_avx2_4(r->x, p->x, t2, p256_mod, p256_mp_mod);
    XMEMSET(r->x + 4, 0, sizeof(r->x) / 2);
    sp_256_mont_reduce_4(r->x, p256_mod, p256_mp_mod);
    /* Reduce x to less than modulus */
    n = sp_256_cmp_4(r->x, p256_mod);
    sp_256_cond_sub_4(r->x, r->x, p256_mod, 0 - (n >= 0));
    sp_256_norm_4(r->x);

    /* y /= z^3 */
    sp_256_mont_mul_avx2_4(r->y, p->y, t1, p256_mod, p256_mp_mod);
    XMEMSET(r->y + 4, 0, sizeof(r->y) / 2);
    sp_256_mont_reduce_4(r->y, p256_mod, p256_mp_mod);
    /* Reduce y to less than modulus */
    n = sp_256_cmp_4(r->y, p256_mod);
    sp_256_cond_sub_4(r->y, r->y, p256_mod, 0 - (n >= 0));
    sp_256_norm_4(r->y);

    XMEMSET(r->z, 0, sizeof(r->z));
    r->z[0] = 1;

}

/* Double the Montgomery form projective point p.
 *
 * r  Result of doubling point.
 * p  Point to double.
 * t  Temporary ordinate data.
 */
static void sp_256_proj_point_dbl_avx2_4(sp_point* r, sp_point* p, sp_digit* t)
{
    sp_point* rp[2];
    sp_digit* t1 = t;
    sp_digit* t2 = t + 2*4;
    sp_digit* x;
    sp_digit* y;
    sp_digit* z;
    int i;

    /* When infinity don't double point passed in - constant time. */
    rp[0] = r;
    rp[1] = (sp_point*)t;
    XMEMSET(rp[1], 0, sizeof(sp_point));
    x = rp[p->infinity]->x;
    y = rp[p->infinity]->y;
    z = rp[p->infinity]->z;
    /* Put point to double into result - good for infinty. */
    if (r != p) {
        for (i=0; i<4; i++)
            r->x[i] = p->x[i];
        for (i=0; i<4; i++)
            r->y[i] = p->y[i];
        for (i=0; i<4; i++)
            r->z[i] = p->z[i];
        r->infinity = p->infinity;
    }

    /* T1 = Z * Z */
    sp_256_mont_sqr_avx2_4(t1, z, p256_mod, p256_mp_mod);
    /* Z = Y * Z */
    sp_256_mont_mul_avx2_4(z, y, z, p256_mod, p256_mp_mod);
    /* Z = 2Z */
    sp_256_mont_dbl_4(z, z, p256_mod);
    /* T2 = X - T1 */
    sp_256_mont_sub_4(t2, x, t1, p256_mod);
    /* T1 = X + T1 */
    sp_256_mont_add_4(t1, x, t1, p256_mod);
    /* T2 = T1 * T2 */
    sp_256_mont_mul_avx2_4(t2, t1, t2, p256_mod, p256_mp_mod);
    /* T1 = 3T2 */
    sp_256_mont_tpl_4(t1, t2, p256_mod);
    /* Y = 2Y */
    sp_256_mont_dbl_4(y, y, p256_mod);
    /* Y = Y * Y */
    sp_256_mont_sqr_avx2_4(y, y, p256_mod, p256_mp_mod);
    /* T2 = Y * Y */
    sp_256_mont_sqr_avx2_4(t2, y, p256_mod, p256_mp_mod);
    /* T2 = T2/2 */
    sp_256_div2_4(t2, t2, p256_mod);
    /* Y = Y * X */
    sp_256_mont_mul_avx2_4(y, y, x, p256_mod, p256_mp_mod);
    /* X = T1 * T1 */
    sp_256_mont_mul_avx2_4(x, t1, t1, p256_mod, p256_mp_mod);
    /* X = X - Y */
    sp_256_mont_sub_4(x, x, y, p256_mod);
    /* X = X - Y */
    sp_256_mont_sub_4(x, x, y, p256_mod);
    /* Y = Y - X */
    sp_256_mont_sub_4(y, y, x, p256_mod);
    /* Y = Y * T1 */
    sp_256_mont_mul_avx2_4(y, y, t1, p256_mod, p256_mp_mod);
    /* Y = Y - T2 */
    sp_256_mont_sub_4(y, y, t2, p256_mod);

}

/* Double the Montgomery form projective point p a number of times.
 *
 * r  Result of repeated doubling of point.
 * p  Point to double.
 * n  Number of times to double
 * t  Temporary ordinate data.
 */
static void sp_256_proj_point_dbl_n_avx2_4(sp_point* r, sp_point* p, int n,
        sp_digit* t)
{
    sp_point* rp[2];
    sp_digit* w = t;
    sp_digit* a = t + 2*4;
    sp_digit* b = t + 4*4;
    sp_digit* t1 = t + 6*4;
    sp_digit* t2 = t + 8*4;
    sp_digit* x;
    sp_digit* y;
    sp_digit* z;
    int i;

    rp[0] = r;
    rp[1] = (sp_point*)t;
    XMEMSET(rp[1], 0, sizeof(sp_point));
    x = rp[p->infinity]->x;
    y = rp[p->infinity]->y;
    z = rp[p->infinity]->z;
    if (r != p) {
        for (i=0; i<4; i++)
            r->x[i] = p->x[i];
        for (i=0; i<4; i++)
            r->y[i] = p->y[i];
        for (i=0; i<4; i++)
            r->z[i] = p->z[i];
        r->infinity = p->infinity;
    }

    /* Y = 2*Y */
    sp_256_mont_dbl_4(y, y, p256_mod);
    /* W = Z^4 */
    sp_256_mont_sqr_avx2_4(w, z, p256_mod, p256_mp_mod);
    sp_256_mont_sqr_avx2_4(w, w, p256_mod, p256_mp_mod);
    while (n--) {
        /* A = 3*(X^2 - W) */
        sp_256_mont_sqr_avx2_4(t1, x, p256_mod, p256_mp_mod);
        sp_256_mont_sub_4(t1, t1, w, p256_mod);
        sp_256_mont_tpl_4(a, t1, p256_mod);
        /* B = X*Y^2 */
        sp_256_mont_sqr_avx2_4(t2, y, p256_mod, p256_mp_mod);
        sp_256_mont_mul_avx2_4(b, t2, x, p256_mod, p256_mp_mod);
        /* X = A^2 - 2B */
        sp_256_mont_sqr_avx2_4(x, a, p256_mod, p256_mp_mod);
        sp_256_mont_dbl_4(t1, b, p256_mod);
        sp_256_mont_sub_4(x, x, t1, p256_mod);
        /* Z = Z*Y */
        sp_256_mont_mul_avx2_4(z, z, y, p256_mod, p256_mp_mod);
        /* t2 = Y^4 */
        sp_256_mont_sqr_avx2_4(t2, t2, p256_mod, p256_mp_mod);
        if (n) {
            /* W = W*Y^4 */
            sp_256_mont_mul_avx2_4(w, w, t2, p256_mod, p256_mp_mod);
        }
        /* y = 2*A*(B - X) - Y^4 */
        sp_256_mont_sub_4(y, b, x, p256_mod);
        sp_256_mont_mul_avx2_4(y, y, a, p256_mod, p256_mp_mod);
        sp_256_mont_dbl_4(y, y, p256_mod);
        sp_256_mont_sub_4(y, y, t2, p256_mod);
    }
    /* Y = Y/2 */
    sp_256_div2_4(y, y, p256_mod);
}

/* Add two Montgomery form projective points.
 *
 * r  Result of addition.
 * p  Frist point to add.
 * q  Second point to add.
 * t  Temporary ordinate data.
 */
static void sp_256_proj_point_add_avx2_4(sp_point* r, sp_point* p, sp_point* q,
        sp_digit* t)
{
    sp_point* ap[2];
    sp_point* rp[2];
    sp_digit* t1 = t;
    sp_digit* t2 = t + 2*4;
    sp_digit* t3 = t + 4*4;
    sp_digit* t4 = t + 6*4;
    sp_digit* t5 = t + 8*4;
    sp_digit* x;
    sp_digit* y;
    sp_digit* z;
    int i;

    /* Ensure only the first point is the same as the result. */
    if (q == r) {
        sp_point* a = p;
        p = q;
        q = a;
    }

    /* Check double */
    sp_256_sub_4(t1, p256_mod, q->y);
    sp_256_norm_4(t1);
    if (sp_256_cmp_equal_4(p->x, q->x) & sp_256_cmp_equal_4(p->z, q->z) &
        (sp_256_cmp_equal_4(p->y, q->y) | sp_256_cmp_equal_4(p->y, t1))) {
        sp_256_proj_point_dbl_4(r, p, t);
    }
    else {
        rp[0] = r;
        rp[1] = (sp_point*)t;
        XMEMSET(rp[1], 0, sizeof(sp_point));
        x = rp[p->infinity | q->infinity]->x;
        y = rp[p->infinity | q->infinity]->y;
        z = rp[p->infinity | q->infinity]->z;

        ap[0] = p;
        ap[1] = q;
        for (i=0; i<4; i++)
            r->x[i] = ap[p->infinity]->x[i];
        for (i=0; i<4; i++)
            r->y[i] = ap[p->infinity]->y[i];
        for (i=0; i<4; i++)
            r->z[i] = ap[p->infinity]->z[i];
        r->infinity = ap[p->infinity]->infinity;

        /* U1 = X1*Z2^2 */
        sp_256_mont_sqr_avx2_4(t1, q->z, p256_mod, p256_mp_mod);
        sp_256_mont_mul_avx2_4(t3, t1, q->z, p256_mod, p256_mp_mod);
        sp_256_mont_mul_avx2_4(t1, t1, x, p256_mod, p256_mp_mod);
        /* U2 = X2*Z1^2 */
        sp_256_mont_sqr_avx2_4(t2, z, p256_mod, p256_mp_mod);
        sp_256_mont_mul_avx2_4(t4, t2, z, p256_mod, p256_mp_mod);
        sp_256_mont_mul_avx2_4(t2, t2, q->x, p256_mod, p256_mp_mod);
        /* S1 = Y1*Z2^3 */
        sp_256_mont_mul_avx2_4(t3, t3, y, p256_mod, p256_mp_mod);
        /* S2 = Y2*Z1^3 */
        sp_256_mont_mul_avx2_4(t4, t4, q->y, p256_mod, p256_mp_mod);
        /* H = U2 - U1 */
        sp_256_mont_sub_4(t2, t2, t1, p256_mod);
        /* R = S2 - S1 */
        sp_256_mont_sub_4(t4, t4, t3, p256_mod);
        /* Z3 = H*Z1*Z2 */
        sp_256_mont_mul_avx2_4(z, z, q->z, p256_mod, p256_mp_mod);
        sp_256_mont_mul_avx2_4(z, z, t2, p256_mod, p256_mp_mod);
        /* X3 = R^2 - H^3 - 2*U1*H^2 */
        sp_256_mont_sqr_avx2_4(x, t4, p256_mod, p256_mp_mod);
        sp_256_mont_sqr_avx2_4(t5, t2, p256_mod, p256_mp_mod);
        sp_256_mont_mul_avx2_4(y, t1, t5, p256_mod, p256_mp_mod);
        sp_256_mont_mul_avx2_4(t5, t5, t2, p256_mod, p256_mp_mod);
        sp_256_mont_sub_4(x, x, t5, p256_mod);
        sp_256_mont_dbl_4(t1, y, p256_mod);
        sp_256_mont_sub_4(x, x, t1, p256_mod);
        /* Y3 = R*(U1*H^2 - X3) - S1*H^3 */
        sp_256_mont_sub_4(y, y, x, p256_mod);
        sp_256_mont_mul_avx2_4(y, y, t4, p256_mod, p256_mp_mod);
        sp_256_mont_mul_avx2_4(t5, t5, t3, p256_mod, p256_mp_mod);
        sp_256_mont_sub_4(y, y, t5, p256_mod);
    }
}

/* Double the Montgomery form projective point p a number of times.
 *
 * r  Result of repeated doubling of point.
 * p  Point to double.
 * n  Number of times to double
 * t  Temporary ordinate data.
 */
static void sp_256_proj_point_dbl_n_store_avx2_4(sp_point* r, sp_point* p,
        int n, int m, sp_digit* t)
{
    sp_digit* w = t;
    sp_digit* a = t + 2*4;
    sp_digit* b = t + 4*4;
    sp_digit* t1 = t + 6*4;
    sp_digit* t2 = t + 8*4;
    sp_digit* x = r[2*m].x;
    sp_digit* y = r[(1<<n)*m].y;
    sp_digit* z = r[2*m].z;
    int i;

    for (i=0; i<4; i++)
        x[i] = p->x[i];
    for (i=0; i<4; i++)
        y[i] = p->y[i];
    for (i=0; i<4; i++)
        z[i] = p->z[i];

    /* Y = 2*Y */
    sp_256_mont_dbl_4(y, y, p256_mod);
    /* W = Z^4 */
    sp_256_mont_sqr_avx2_4(w, z, p256_mod, p256_mp_mod);
    sp_256_mont_sqr_avx2_4(w, w, p256_mod, p256_mp_mod);
    for (i=1; i<=n; i++) {
        /* A = 3*(X^2 - W) */
        sp_256_mont_sqr_avx2_4(t1, x, p256_mod, p256_mp_mod);
        sp_256_mont_sub_4(t1, t1, w, p256_mod);
        sp_256_mont_tpl_4(a, t1, p256_mod);
        /* B = X*Y^2 */
        sp_256_mont_sqr_avx2_4(t2, y, p256_mod, p256_mp_mod);
        sp_256_mont_mul_avx2_4(b, t2, x, p256_mod, p256_mp_mod);
        x = r[(1<<i)*m].x;
        /* X = A^2 - 2B */
        sp_256_mont_sqr_avx2_4(x, a, p256_mod, p256_mp_mod);
        sp_256_mont_dbl_4(t1, b, p256_mod);
        sp_256_mont_sub_4(x, x, t1, p256_mod);
        /* Z = Z*Y */
        sp_256_mont_mul_avx2_4(r[(1<<i)*m].z, z, y, p256_mod, p256_mp_mod);
        z = r[(1<<i)*m].z;
        /* t2 = Y^4 */
        sp_256_mont_sqr_avx2_4(t2, t2, p256_mod, p256_mp_mod);
        if (i != n) {
            /* W = W*Y^4 */
            sp_256_mont_mul_avx2_4(w, w, t2, p256_mod, p256_mp_mod);
        }
        /* y = 2*A*(B - X) - Y^4 */
        sp_256_mont_sub_4(y, b, x, p256_mod);
        sp_256_mont_mul_avx2_4(y, y, a, p256_mod, p256_mp_mod);
        sp_256_mont_dbl_4(y, y, p256_mod);
        sp_256_mont_sub_4(y, y, t2, p256_mod);

        /* Y = Y/2 */
        sp_256_div2_4(r[(1<<i)*m].y, y, p256_mod);
        r[(1<<i)*m].infinity = 0;
    }
}

/* Add two Montgomery form projective points.
 *
 * ra  Result of addition.
 * rs  Result of subtraction.
 * p   Frist point to add.
 * q   Second point to add.
 * t   Temporary ordinate data.
 */
static void sp_256_proj_point_add_sub_avx2_4(sp_point* ra, sp_point* rs,
        sp_point* p, sp_point* q, sp_digit* t)
{
    sp_digit* t1 = t;
    sp_digit* t2 = t + 2*4;
    sp_digit* t3 = t + 4*4;
    sp_digit* t4 = t + 6*4;
    sp_digit* t5 = t + 8*4;
    sp_digit* t6 = t + 10*4;
    sp_digit* x = ra->x;
    sp_digit* y = ra->y;
    sp_digit* z = ra->z;
    sp_digit* xs = rs->x;
    sp_digit* ys = rs->y;
    sp_digit* zs = rs->z;


    XMEMCPY(x, p->x, sizeof(p->x) / 2);
    XMEMCPY(y, p->y, sizeof(p->y) / 2);
    XMEMCPY(z, p->z, sizeof(p->z) / 2);
    ra->infinity = 0;
    rs->infinity = 0;

    /* U1 = X1*Z2^2 */
    sp_256_mont_sqr_avx2_4(t1, q->z, p256_mod, p256_mp_mod);
    sp_256_mont_mul_avx2_4(t3, t1, q->z, p256_mod, p256_mp_mod);
    sp_256_mont_mul_avx2_4(t1, t1, x, p256_mod, p256_mp_mod);
    /* U2 = X2*Z1^2 */
    sp_256_mont_sqr_avx2_4(t2, z, p256_mod, p256_mp_mod);
    sp_256_mont_mul_avx2_4(t4, t2, z, p256_mod, p256_mp_mod);
    sp_256_mont_mul_avx2_4(t2, t2, q->x, p256_mod, p256_mp_mod);
    /* S1 = Y1*Z2^3 */
    sp_256_mont_mul_avx2_4(t3, t3, y, p256_mod, p256_mp_mod);
    /* S2 = Y2*Z1^3 */
    sp_256_mont_mul_avx2_4(t4, t4, q->y, p256_mod, p256_mp_mod);
    /* H = U2 - U1 */
    sp_256_mont_sub_4(t2, t2, t1, p256_mod);
    /* RS = S2 + S1 */
    sp_256_mont_add_4(t6, t4, t3, p256_mod);
    /* R = S2 - S1 */
    sp_256_mont_sub_4(t4, t4, t3, p256_mod);
    /* Z3 = H*Z1*Z2 */
    /* ZS = H*Z1*Z2 */
    sp_256_mont_mul_avx2_4(z, z, q->z, p256_mod, p256_mp_mod);
    sp_256_mont_mul_avx2_4(z, z, t2, p256_mod, p256_mp_mod);
    XMEMCPY(zs, z, sizeof(p->z)/2);
    /* X3 = R^2 - H^3 - 2*U1*H^2 */
    /* XS = RS^2 - H^3 - 2*U1*H^2 */
    sp_256_mont_sqr_avx2_4(x, t4, p256_mod, p256_mp_mod);
    sp_256_mont_sqr_avx2_4(xs, t6, p256_mod, p256_mp_mod);
    sp_256_mont_sqr_avx2_4(t5, t2, p256_mod, p256_mp_mod);
    sp_256_mont_mul_avx2_4(y, t1, t5, p256_mod, p256_mp_mod);
    sp_256_mont_mul_avx2_4(t5, t5, t2, p256_mod, p256_mp_mod);
    sp_256_mont_sub_4(x, x, t5, p256_mod);
    sp_256_mont_sub_4(xs, xs, t5, p256_mod);
    sp_256_mont_dbl_4(t1, y, p256_mod);
    sp_256_mont_sub_4(x, x, t1, p256_mod);
    sp_256_mont_sub_4(xs, xs, t1, p256_mod);
    /* Y3 = R*(U1*H^2 - X3) - S1*H^3 */
    /* YS = -RS*(U1*H^2 - XS) - S1*H^3 */
    sp_256_mont_sub_4(ys, y, xs, p256_mod);
    sp_256_mont_sub_4(y, y, x, p256_mod);
    sp_256_mont_mul_avx2_4(y, y, t4, p256_mod, p256_mp_mod);
    sp_256_sub_4(t6, p256_mod, t6);
    sp_256_mont_mul_avx2_4(ys, ys, t6, p256_mod, p256_mp_mod);
    sp_256_mont_mul_avx2_4(t5, t5, t3, p256_mod, p256_mp_mod);
    sp_256_mont_sub_4(y, y, t5, p256_mod);
    sp_256_mont_sub_4(ys, ys, t5, p256_mod);
}

/* Multiply the point by the scalar and return the result.
 * If map is true then convert result to affine co-ordinates.
 *
 * r     Resulting point.
 * g     Point to multiply.
 * k     Scalar to multiply by.
 * map   Indicates whether to convert result to affine.
 * heap  Heap to use for allocation.
 * returns MEMORY_E when memory allocation fails and MP_OKAY on success.
 */
static int sp_256_ecc_mulmod_win_add_sub_avx2_4(sp_point* r, sp_point* g,
        sp_digit* k, int map, void* heap)
{
#if !defined(WOLFSSL_SP_SMALL) && !defined(WOLFSSL_SMALL_STACK)
    sp_point td[33];
    sp_point rtd, pd;
    sp_digit tmpd[2 * 4 * 6];
#endif
    sp_point* t;
    sp_point* rt;
    sp_point* p = NULL;
    sp_digit* tmp;
    sp_digit* negy;
    int i;
    ecc_recode v[43];
    int err;

    (void)heap;

    err = sp_ecc_point_new(heap, rtd, rt);
    if (err == MP_OKAY)
        err = sp_ecc_point_new(heap, pd, p);
#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    t = (sp_point*)XMALLOC(sizeof(sp_point) * 33, heap, DYNAMIC_TYPE_ECC);
    if (t == NULL)
        err = MEMORY_E;
    tmp = (sp_digit*)XMALLOC(sizeof(sp_digit) * 2 * 4 * 6, heap,
                             DYNAMIC_TYPE_ECC);
    if (tmp == NULL)
        err = MEMORY_E;
#else
    t = td;
    tmp = tmpd;
#endif


    if (err == MP_OKAY) {
        /* t[0] = {0, 0, 1} * norm */
        XMEMSET(&t[0], 0, sizeof(t[0]));
        t[0].infinity = 1;
        /* t[1] = {g->x, g->y, g->z} * norm */
        err = sp_256_mod_mul_norm_4(t[1].x, g->x, p256_mod);
    }
    if (err == MP_OKAY)
        err = sp_256_mod_mul_norm_4(t[1].y, g->y, p256_mod);
    if (err == MP_OKAY)
        err = sp_256_mod_mul_norm_4(t[1].z, g->z, p256_mod);

    if (err == MP_OKAY) {
        t[1].infinity = 0;
        /* t[2] ... t[32]  */
    sp_256_proj_point_dbl_n_store_avx2_4(t, &t[ 1], 5, 1, tmp);
    sp_256_proj_point_add_avx2_4(&t[ 3], &t[ 2], &t[ 1], tmp);
    sp_256_proj_point_dbl_avx2_4(&t[ 6], &t[ 3], tmp);
    sp_256_proj_point_add_sub_avx2_4(&t[ 7], &t[ 5], &t[ 6], &t[ 1], tmp);
    sp_256_proj_point_dbl_avx2_4(&t[10], &t[ 5], tmp);
    sp_256_proj_point_add_sub_avx2_4(&t[11], &t[ 9], &t[10], &t[ 1], tmp);
    sp_256_proj_point_dbl_avx2_4(&t[12], &t[ 6], tmp);
    sp_256_proj_point_dbl_avx2_4(&t[14], &t[ 7], tmp);
    sp_256_proj_point_add_sub_avx2_4(&t[15], &t[13], &t[14], &t[ 1], tmp);
    sp_256_proj_point_dbl_avx2_4(&t[18], &t[ 9], tmp);
    sp_256_proj_point_add_sub_avx2_4(&t[19], &t[17], &t[18], &t[ 1], tmp);
    sp_256_proj_point_dbl_avx2_4(&t[20], &t[10], tmp);
    sp_256_proj_point_dbl_avx2_4(&t[22], &t[11], tmp);
    sp_256_proj_point_add_sub_avx2_4(&t[23], &t[21], &t[22], &t[ 1], tmp);
    sp_256_proj_point_dbl_avx2_4(&t[24], &t[12], tmp);
    sp_256_proj_point_dbl_avx2_4(&t[26], &t[13], tmp);
    sp_256_proj_point_add_sub_avx2_4(&t[27], &t[25], &t[26], &t[ 1], tmp);
    sp_256_proj_point_dbl_avx2_4(&t[28], &t[14], tmp);
    sp_256_proj_point_dbl_avx2_4(&t[30], &t[15], tmp);
    sp_256_proj_point_add_sub_avx2_4(&t[31], &t[29], &t[30], &t[ 1], tmp);

        negy = t[0].y;

        sp_256_ecc_recode_6_4(k, v);

        i = 42;
        XMEMCPY(rt, &t[v[i].i], sizeof(sp_point));
        for (--i; i>=0; i--) {
            sp_256_proj_point_dbl_n_avx2_4(rt, rt, 6, tmp);

            XMEMCPY(p, &t[v[i].i], sizeof(sp_point));
            sp_256_sub_4(negy, p256_mod, p->y);
            sp_256_cond_copy_4(p->y, negy, (sp_digit)0 - v[i].neg);
            sp_256_proj_point_add_avx2_4(rt, rt, p, tmp);
        }

        if (map)
            sp_256_map_avx2_4(r, rt, tmp);
        else
            XMEMCPY(r, rt, sizeof(sp_point));
    }

#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (t != NULL)
        XFREE(t, heap, DYNAMIC_TYPE_ECC);
    if (tmp != NULL)
        XFREE(tmp, heap, DYNAMIC_TYPE_ECC);
#endif
    sp_ecc_point_free(p, 0, heap);
    sp_ecc_point_free(rt, 0, heap);

    return err;
}

#endif /* HAVE_INTEL_AVX2 */
/* A table entry for pre-computed points. */
typedef struct sp_table_entry {
    sp_digit x[4];
    sp_digit y[4];
    byte infinity;
} sp_table_entry;

#if defined(FP_ECC) || defined(WOLFSSL_SP_SMALL)
#endif /* FP_ECC || WOLFSSL_SP_SMALL */
/* Add two Montgomery form projective points. The second point has a q value of
 * one.
 * Only the first point can be the same pointer as the result point.
 *
 * r  Result of addition.
 * p  Frist point to add.
 * q  Second point to add.
 * t  Temporary ordinate data.
 */
static void sp_256_proj_point_add_qz1_4(sp_point* r, sp_point* p,
        sp_point* q, sp_digit* t)
{
    sp_point* ap[2];
    sp_point* rp[2];
    sp_digit* t1 = t;
    sp_digit* t2 = t + 2*4;
    sp_digit* t3 = t + 4*4;
    sp_digit* t4 = t + 6*4;
    sp_digit* t5 = t + 8*4;
    sp_digit* x;
    sp_digit* y;
    sp_digit* z;
    int i;

    /* Check double */
    sp_256_sub_4(t1, p256_mod, q->y);
    sp_256_norm_4(t1);
    if (sp_256_cmp_equal_4(p->x, q->x) & sp_256_cmp_equal_4(p->z, q->z) &
        (sp_256_cmp_equal_4(p->y, q->y) | sp_256_cmp_equal_4(p->y, t1))) {
        sp_256_proj_point_dbl_4(r, p, t);
    }
    else {
        rp[0] = r;
        rp[1] = (sp_point*)t;
        XMEMSET(rp[1], 0, sizeof(sp_point));
        x = rp[p->infinity | q->infinity]->x;
        y = rp[p->infinity | q->infinity]->y;
        z = rp[p->infinity | q->infinity]->z;

        ap[0] = p;
        ap[1] = q;
        for (i=0; i<4; i++)
            r->x[i] = ap[p->infinity]->x[i];
        for (i=0; i<4; i++)
            r->y[i] = ap[p->infinity]->y[i];
        for (i=0; i<4; i++)
            r->z[i] = ap[p->infinity]->z[i];
        r->infinity = ap[p->infinity]->infinity;

        /* U2 = X2*Z1^2 */
        sp_256_mont_sqr_4(t2, z, p256_mod, p256_mp_mod);
        sp_256_mont_mul_4(t4, t2, z, p256_mod, p256_mp_mod);
        sp_256_mont_mul_4(t2, t2, q->x, p256_mod, p256_mp_mod);
        /* S2 = Y2*Z1^3 */
        sp_256_mont_mul_4(t4, t4, q->y, p256_mod, p256_mp_mod);
        /* H = U2 - X1 */
        sp_256_mont_sub_4(t2, t2, x, p256_mod);
        /* R = S2 - Y1 */
        sp_256_mont_sub_4(t4, t4, y, p256_mod);
        /* Z3 = H*Z1 */
        sp_256_mont_mul_4(z, z, t2, p256_mod, p256_mp_mod);
        /* X3 = R^2 - H^3 - 2*X1*H^2 */
        sp_256_mont_sqr_4(t1, t4, p256_mod, p256_mp_mod);
        sp_256_mont_sqr_4(t5, t2, p256_mod, p256_mp_mod);
        sp_256_mont_mul_4(t3, x, t5, p256_mod, p256_mp_mod);
        sp_256_mont_mul_4(t5, t5, t2, p256_mod, p256_mp_mod);
        sp_256_mont_sub_4(x, t1, t5, p256_mod);
        sp_256_mont_dbl_4(t1, t3, p256_mod);
        sp_256_mont_sub_4(x, x, t1, p256_mod);
        /* Y3 = R*(X1*H^2 - X3) - Y1*H^3 */
        sp_256_mont_sub_4(t3, t3, x, p256_mod);
        sp_256_mont_mul_4(t3, t3, t4, p256_mod, p256_mp_mod);
        sp_256_mont_mul_4(t5, t5, y, p256_mod, p256_mp_mod);
        sp_256_mont_sub_4(y, t3, t5, p256_mod);
    }
}

#ifdef FP_ECC
/* Convert the projective point to affine.
 * Ordinates are in Montgomery form.
 *
 * a  Point to convert.
 * t  Temprorary data.
 */
static void sp_256_proj_to_affine_4(sp_point* a, sp_digit* t)
{
    sp_digit* t1 = t;
    sp_digit* t2 = t + 2 * 4;
    sp_digit* tmp = t + 4 * 4;

    sp_256_mont_inv_4(t1, a->z, tmp);

    sp_256_mont_sqr_4(t2, t1, p256_mod, p256_mp_mod);
    sp_256_mont_mul_4(t1, t2, t1, p256_mod, p256_mp_mod);

    sp_256_mont_mul_4(a->x, a->x, t2, p256_mod, p256_mp_mod);
    sp_256_mont_mul_4(a->y, a->y, t1, p256_mod, p256_mp_mod);
    XMEMCPY(a->z, p256_norm_mod, sizeof(p256_norm_mod));
}

/* Generate the pre-computed table of points for the base point.
 *
 * a      The base point.
 * table  Place to store generated point data.
 * tmp    Temprorary data.
 * heap  Heap to use for allocation.
 */
static int sp_256_gen_stripe_table_4(sp_point* a,
        sp_table_entry* table, sp_digit* tmp, void* heap)
{
#if !defined(WOLFSSL_SP_SMALL) && !defined(WOLFSSL_SMALL_STACK)
    sp_point td, s1d, s2d;
#endif
    sp_point* t;
    sp_point* s1 = NULL;
    sp_point* s2 = NULL;
    int i, j;
    int err;

    (void)heap;

    err = sp_ecc_point_new(heap, td, t);
    if (err == MP_OKAY)
        err = sp_ecc_point_new(heap, s1d, s1);
    if (err == MP_OKAY)
        err = sp_ecc_point_new(heap, s2d, s2);

    if (err == MP_OKAY)
        err = sp_256_mod_mul_norm_4(t->x, a->x, p256_mod);
    if (err == MP_OKAY)
        err = sp_256_mod_mul_norm_4(t->y, a->y, p256_mod);
    if (err == MP_OKAY)
        err = sp_256_mod_mul_norm_4(t->z, a->z, p256_mod);
    if (err == MP_OKAY) {
        t->infinity = 0;
        sp_256_proj_to_affine_4(t, tmp);

        XMEMCPY(s1->z, p256_norm_mod, sizeof(p256_norm_mod));
        s1->infinity = 0;
        XMEMCPY(s2->z, p256_norm_mod, sizeof(p256_norm_mod));
        s2->infinity = 0;

        /* table[0] = {0, 0, infinity} */
        XMEMSET(&table[0], 0, sizeof(sp_table_entry));
        table[0].infinity = 1;
        /* table[1] = Affine version of 'a' in Montgomery form */
        XMEMCPY(table[1].x, t->x, sizeof(table->x));
        XMEMCPY(table[1].y, t->y, sizeof(table->y));
        table[1].infinity = 0;

        for (i=1; i<8; i++) {
            sp_256_proj_point_dbl_n_4(t, t, 32, tmp);
            sp_256_proj_to_affine_4(t, tmp);
            XMEMCPY(table[1<<i].x, t->x, sizeof(table->x));
            XMEMCPY(table[1<<i].y, t->y, sizeof(table->y));
            table[1<<i].infinity = 0;
        }

        for (i=1; i<8; i++) {
            XMEMCPY(s1->x, table[1<<i].x, sizeof(table->x));
            XMEMCPY(s1->y, table[1<<i].y, sizeof(table->y));
            for (j=(1<<i)+1; j<(1<<(i+1)); j++) {
                XMEMCPY(s2->x, table[j-(1<<i)].x, sizeof(table->x));
                XMEMCPY(s2->y, table[j-(1<<i)].y, sizeof(table->y));
                sp_256_proj_point_add_qz1_4(t, s1, s2, tmp);
                sp_256_proj_to_affine_4(t, tmp);
                XMEMCPY(table[j].x, t->x, sizeof(table->x));
                XMEMCPY(table[j].y, t->y, sizeof(table->y));
                table[j].infinity = 0;
            }
        }
    }

    sp_ecc_point_free(s2, 0, heap);
    sp_ecc_point_free(s1, 0, heap);
    sp_ecc_point_free( t, 0, heap);

    return err;
}

#endif /* FP_ECC */
#if defined(FP_ECC) || defined(WOLFSSL_SP_SMALL)
/* Multiply the point by the scalar and return the result.
 * If map is true then convert result to affine co-ordinates.
 *
 * r     Resulting point.
 * k     Scalar to multiply by.
 * map   Indicates whether to convert result to affine.
 * heap  Heap to use for allocation.
 * returns MEMORY_E when memory allocation fails and MP_OKAY on success.
 */
static int sp_256_ecc_mulmod_stripe_4(sp_point* r, sp_point* g,
        sp_table_entry* table, sp_digit* k, int map, void* heap)
{
#if !defined(WOLFSSL_SP_SMALL) && !defined(WOLFSSL_SMALL_STACK)
    sp_point rtd;
    sp_point pd;
    sp_digit td[2 * 4 * 5];
#endif
    sp_point* rt;
    sp_point* p = NULL;
    sp_digit* t;
    int i, j;
    int y, x;
    int err;

    (void)g;
    (void)heap;

    err = sp_ecc_point_new(heap, rtd, rt);
    if (err == MP_OKAY)
        err = sp_ecc_point_new(heap, pd, p);
#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    t = (sp_digit*)XMALLOC(sizeof(sp_digit) * 2 * 4 * 5, heap,
                           DYNAMIC_TYPE_ECC);
    if (t == NULL)
        err = MEMORY_E;
#else
    t = td;
#endif

    if (err == MP_OKAY) {
        XMEMCPY(p->z, p256_norm_mod, sizeof(p256_norm_mod));
        XMEMCPY(rt->z, p256_norm_mod, sizeof(p256_norm_mod));

        y = 0;
        for (j=0,x=31; j<8; j++,x+=32)
            y |= ((k[x / 64] >> (x % 64)) & 1) << j;
        XMEMCPY(rt->x, table[y].x, sizeof(table[y].x));
        XMEMCPY(rt->y, table[y].y, sizeof(table[y].y));
        rt->infinity = table[y].infinity;
        for (i=30; i>=0; i--) {
            y = 0;
            for (j=0,x=i; j<8; j++,x+=32)
                y |= ((k[x / 64] >> (x % 64)) & 1) << j;

            sp_256_proj_point_dbl_4(rt, rt, t);
            XMEMCPY(p->x, table[y].x, sizeof(table[y].x));
            XMEMCPY(p->y, table[y].y, sizeof(table[y].y));
            p->infinity = table[y].infinity;
            sp_256_proj_point_add_qz1_4(rt, rt, p, t);
        }

        if (map)
            sp_256_map_4(r, rt, t);
        else
            XMEMCPY(r, rt, sizeof(sp_point));
    }

#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (t != NULL)
        XFREE(t, heap, DYNAMIC_TYPE_ECC);
#endif
    sp_ecc_point_free(p, 0, heap);
    sp_ecc_point_free(rt, 0, heap);

    return err;
}

#endif /* FP_ECC || WOLFSSL_SP_SMALL */
#ifdef FP_ECC
#ifndef FP_ENTRIES
    #define FP_ENTRIES 16
#endif

typedef struct sp_cache_t {
    sp_digit x[4];
    sp_digit y[4];
    sp_table_entry table[256];
    uint32_t cnt;
    int set;
} sp_cache_t;

static THREAD_LS_T sp_cache_t sp_cache[FP_ENTRIES];
static THREAD_LS_T int sp_cache_last = -1;
static THREAD_LS_T int sp_cache_inited = 0;

#ifndef HAVE_THREAD_LS
    static volatile int initCacheMutex = 0;
    static wolfSSL_Mutex sp_cache_lock;
#endif

static void sp_ecc_get_cache(sp_point* g, sp_cache_t** cache)
{
    int i, j;
    uint32_t least;

    if (sp_cache_inited == 0) {
        for (i=0; i<FP_ENTRIES; i++) {
            sp_cache[i].set = 0;
        }
        sp_cache_inited = 1;
    }

    /* Compare point with those in cache. */
    for (i=0; i<FP_ENTRIES; i++) {
        if (!sp_cache[i].set)
            continue;

        if (sp_256_cmp_equal_4(g->x, sp_cache[i].x) &
                           sp_256_cmp_equal_4(g->y, sp_cache[i].y)) {
            sp_cache[i].cnt++;
            break;
        }
    }

    /* No match. */
    if (i == FP_ENTRIES) {
        /* Find empty entry. */
        i = (sp_cache_last + 1) % FP_ENTRIES;
        for (; i != sp_cache_last; i=(i+1)%FP_ENTRIES) {
            if (!sp_cache[i].set) {
                break;
            }
        }

        /* Evict least used. */
        if (i == sp_cache_last) {
            least = sp_cache[0].cnt;
            for (j=1; j<FP_ENTRIES; j++) {
                if (sp_cache[j].cnt < least) {
                    i = j;
                    least = sp_cache[i].cnt;
                }
            }
        }

        XMEMCPY(sp_cache[i].x, g->x, sizeof(sp_cache[i].x));
        XMEMCPY(sp_cache[i].y, g->y, sizeof(sp_cache[i].y));
        sp_cache[i].set = 1;
        sp_cache[i].cnt = 1;
    }

    *cache = &sp_cache[i];
    sp_cache_last = i;
}
#endif /* FP_ECC */

/* Multiply the base point of P256 by the scalar and return the result.
 * If map is true then convert result to affine co-ordinates.
 *
 * r     Resulting point.
 * g     Point to multiply.
 * k     Scalar to multiply by.
 * map   Indicates whether to convert result to affine.
 * heap  Heap to use for allocation.
 * returns MEMORY_E when memory allocation fails and MP_OKAY on success.
 */
static int sp_256_ecc_mulmod_4(sp_point* r, sp_point* g, sp_digit* k,
        int map, void* heap)
{
#ifndef FP_ECC
    return sp_256_ecc_mulmod_win_add_sub_4(r, g, k, map, heap);
#else
    sp_digit tmp[2 * 4 * 5];
    sp_cache_t* cache;
    int err = MP_OKAY;

#ifndef HAVE_THREAD_LS
    if (initCacheMutex == 0) {
         wc_InitMutex(&sp_cache_lock);
         initCacheMutex = 1;
    }
    if (wc_LockMutex(&sp_cache_lock) != 0)
       err = BAD_MUTEX_E;
#endif /* HAVE_THREAD_LS */

    if (err == MP_OKAY) {
        sp_ecc_get_cache(g, &cache);
        if (cache->cnt == 2)
            sp_256_gen_stripe_table_4(g, cache->table, tmp, heap);

#ifndef HAVE_THREAD_LS
        wc_UnLockMutex(&sp_cache_lock);
#endif /* HAVE_THREAD_LS */

        if (cache->cnt < 2) {
            err = sp_256_ecc_mulmod_win_add_sub_4(r, g, k, map, heap);
        }
        else {
            err = sp_256_ecc_mulmod_stripe_4(r, g, cache->table, k,
                    map, heap);
        }
    }

    return err;
#endif
}

#ifdef HAVE_INTEL_AVX2
#if defined(FP_ECC) || defined(WOLFSSL_SP_SMALL)
#endif /* FP_ECC || WOLFSSL_SP_SMALL */
/* Add two Montgomery form projective points. The second point has a q value of
 * one.
 * Only the first point can be the same pointer as the result point.
 *
 * r  Result of addition.
 * p  Frist point to add.
 * q  Second point to add.
 * t  Temporary ordinate data.
 */
static void sp_256_proj_point_add_qz1_avx2_4(sp_point* r, sp_point* p,
        sp_point* q, sp_digit* t)
{
    sp_point* ap[2];
    sp_point* rp[2];
    sp_digit* t1 = t;
    sp_digit* t2 = t + 2*4;
    sp_digit* t3 = t + 4*4;
    sp_digit* t4 = t + 6*4;
    sp_digit* t5 = t + 8*4;
    sp_digit* x;
    sp_digit* y;
    sp_digit* z;
    int i;

    /* Check double */
    sp_256_sub_4(t1, p256_mod, q->y);
    sp_256_norm_4(t1);
    if (sp_256_cmp_equal_4(p->x, q->x) & sp_256_cmp_equal_4(p->z, q->z) &
        (sp_256_cmp_equal_4(p->y, q->y) | sp_256_cmp_equal_4(p->y, t1))) {
        sp_256_proj_point_dbl_4(r, p, t);
    }
    else {
        rp[0] = r;
        rp[1] = (sp_point*)t;
        XMEMSET(rp[1], 0, sizeof(sp_point));
        x = rp[p->infinity | q->infinity]->x;
        y = rp[p->infinity | q->infinity]->y;
        z = rp[p->infinity | q->infinity]->z;

        ap[0] = p;
        ap[1] = q;
        for (i=0; i<4; i++)
            r->x[i] = ap[p->infinity]->x[i];
        for (i=0; i<4; i++)
            r->y[i] = ap[p->infinity]->y[i];
        for (i=0; i<4; i++)
            r->z[i] = ap[p->infinity]->z[i];
        r->infinity = ap[p->infinity]->infinity;

        /* U2 = X2*Z1^2 */
        sp_256_mont_sqr_avx2_4(t2, z, p256_mod, p256_mp_mod);
        sp_256_mont_mul_avx2_4(t4, t2, z, p256_mod, p256_mp_mod);
        sp_256_mont_mul_avx2_4(t2, t2, q->x, p256_mod, p256_mp_mod);
        /* S2 = Y2*Z1^3 */
        sp_256_mont_mul_avx2_4(t4, t4, q->y, p256_mod, p256_mp_mod);
        /* H = U2 - X1 */
        sp_256_mont_sub_4(t2, t2, x, p256_mod);
        /* R = S2 - Y1 */
        sp_256_mont_sub_4(t4, t4, y, p256_mod);
        /* Z3 = H*Z1 */
        sp_256_mont_mul_avx2_4(z, z, t2, p256_mod, p256_mp_mod);
        /* X3 = R^2 - H^3 - 2*X1*H^2 */
        sp_256_mont_sqr_avx2_4(t1, t4, p256_mod, p256_mp_mod);
        sp_256_mont_sqr_avx2_4(t5, t2, p256_mod, p256_mp_mod);
        sp_256_mont_mul_avx2_4(t3, x, t5, p256_mod, p256_mp_mod);
        sp_256_mont_mul_avx2_4(t5, t5, t2, p256_mod, p256_mp_mod);
        sp_256_mont_sub_4(x, t1, t5, p256_mod);
        sp_256_mont_dbl_4(t1, t3, p256_mod);
        sp_256_mont_sub_4(x, x, t1, p256_mod);
        /* Y3 = R*(X1*H^2 - X3) - Y1*H^3 */
        sp_256_mont_sub_4(t3, t3, x, p256_mod);
        sp_256_mont_mul_avx2_4(t3, t3, t4, p256_mod, p256_mp_mod);
        sp_256_mont_mul_avx2_4(t5, t5, y, p256_mod, p256_mp_mod);
        sp_256_mont_sub_4(y, t3, t5, p256_mod);
    }
}

#ifdef FP_ECC
/* Convert the projective point to affine.
 * Ordinates are in Montgomery form.
 *
 * a  Point to convert.
 * t  Temprorary data.
 */
static void sp_256_proj_to_affine_avx2_4(sp_point* a, sp_digit* t)
{
    sp_digit* t1 = t;
    sp_digit* t2 = t + 2 * 4;
    sp_digit* tmp = t + 4 * 4;

    sp_256_mont_inv_avx2_4(t1, a->z, tmp);

    sp_256_mont_sqr_avx2_4(t2, t1, p256_mod, p256_mp_mod);
    sp_256_mont_mul_avx2_4(t1, t2, t1, p256_mod, p256_mp_mod);

    sp_256_mont_mul_avx2_4(a->x, a->x, t2, p256_mod, p256_mp_mod);
    sp_256_mont_mul_avx2_4(a->y, a->y, t1, p256_mod, p256_mp_mod);
    XMEMCPY(a->z, p256_norm_mod, sizeof(p256_norm_mod));
}

/* Generate the pre-computed table of points for the base point.
 *
 * a      The base point.
 * table  Place to store generated point data.
 * tmp    Temprorary data.
 * heap  Heap to use for allocation.
 */
static int sp_256_gen_stripe_table_avx2_4(sp_point* a,
        sp_table_entry* table, sp_digit* tmp, void* heap)
{
#if !defined(WOLFSSL_SP_SMALL) && !defined(WOLFSSL_SMALL_STACK)
    sp_point td, s1d, s2d;
#endif
    sp_point* t;
    sp_point* s1 = NULL;
    sp_point* s2 = NULL;
    int i, j;
    int err;

    (void)heap;

    err = sp_ecc_point_new(heap, td, t);
    if (err == MP_OKAY)
        err = sp_ecc_point_new(heap, s1d, s1);
    if (err == MP_OKAY)
        err = sp_ecc_point_new(heap, s2d, s2);

    if (err == MP_OKAY)
        err = sp_256_mod_mul_norm_4(t->x, a->x, p256_mod);
    if (err == MP_OKAY)
        err = sp_256_mod_mul_norm_4(t->y, a->y, p256_mod);
    if (err == MP_OKAY)
        err = sp_256_mod_mul_norm_4(t->z, a->z, p256_mod);
    if (err == MP_OKAY) {
        t->infinity = 0;
        sp_256_proj_to_affine_avx2_4(t, tmp);

        XMEMCPY(s1->z, p256_norm_mod, sizeof(p256_norm_mod));
        s1->infinity = 0;
        XMEMCPY(s2->z, p256_norm_mod, sizeof(p256_norm_mod));
        s2->infinity = 0;

        /* table[0] = {0, 0, infinity} */
        XMEMSET(&table[0], 0, sizeof(sp_table_entry));
        table[0].infinity = 1;
        /* table[1] = Affine version of 'a' in Montgomery form */
        XMEMCPY(table[1].x, t->x, sizeof(table->x));
        XMEMCPY(table[1].y, t->y, sizeof(table->y));
        table[1].infinity = 0;

        for (i=1; i<8; i++) {
            sp_256_proj_point_dbl_n_avx2_4(t, t, 32, tmp);
            sp_256_proj_to_affine_avx2_4(t, tmp);
            XMEMCPY(table[1<<i].x, t->x, sizeof(table->x));
            XMEMCPY(table[1<<i].y, t->y, sizeof(table->y));
            table[1<<i].infinity = 0;
        }

        for (i=1; i<8; i++) {
            XMEMCPY(s1->x, table[1<<i].x, sizeof(table->x));
            XMEMCPY(s1->y, table[1<<i].y, sizeof(table->y));
            for (j=(1<<i)+1; j<(1<<(i+1)); j++) {
                XMEMCPY(s2->x, table[j-(1<<i)].x, sizeof(table->x));
                XMEMCPY(s2->y, table[j-(1<<i)].y, sizeof(table->y));
                sp_256_proj_point_add_qz1_avx2_4(t, s1, s2, tmp);
                sp_256_proj_to_affine_avx2_4(t, tmp);
                XMEMCPY(table[j].x, t->x, sizeof(table->x));
                XMEMCPY(table[j].y, t->y, sizeof(table->y));
                table[j].infinity = 0;
            }
        }
    }

    sp_ecc_point_free(s2, 0, heap);
    sp_ecc_point_free(s1, 0, heap);
    sp_ecc_point_free( t, 0, heap);

    return err;
}

#endif /* FP_ECC */
#if defined(FP_ECC) || defined(WOLFSSL_SP_SMALL)
/* Multiply the point by the scalar and return the result.
 * If map is true then convert result to affine co-ordinates.
 *
 * r     Resulting point.
 * k     Scalar to multiply by.
 * map   Indicates whether to convert result to affine.
 * heap  Heap to use for allocation.
 * returns MEMORY_E when memory allocation fails and MP_OKAY on success.
 */
static int sp_256_ecc_mulmod_stripe_avx2_4(sp_point* r, sp_point* g,
        sp_table_entry* table, sp_digit* k, int map, void* heap)
{
#if !defined(WOLFSSL_SP_SMALL) && !defined(WOLFSSL_SMALL_STACK)
    sp_point rtd;
    sp_point pd;
    sp_digit td[2 * 4 * 5];
#endif
    sp_point* rt;
    sp_point* p = NULL;
    sp_digit* t;
    int i, j;
    int y, x;
    int err;

    (void)g;
    (void)heap;

    err = sp_ecc_point_new(heap, rtd, rt);
    if (err == MP_OKAY)
        err = sp_ecc_point_new(heap, pd, p);
#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    t = (sp_digit*)XMALLOC(sizeof(sp_digit) * 2 * 4 * 5, heap,
                           DYNAMIC_TYPE_ECC);
    if (t == NULL)
        err = MEMORY_E;
#else
    t = td;
#endif

    if (err == MP_OKAY) {
        XMEMCPY(p->z, p256_norm_mod, sizeof(p256_norm_mod));
        XMEMCPY(rt->z, p256_norm_mod, sizeof(p256_norm_mod));

        y = 0;
        for (j=0,x=31; j<8; j++,x+=32)
            y |= ((k[x / 64] >> (x % 64)) & 1) << j;
        XMEMCPY(rt->x, table[y].x, sizeof(table[y].x));
        XMEMCPY(rt->y, table[y].y, sizeof(table[y].y));
        rt->infinity = table[y].infinity;
        for (i=30; i>=0; i--) {
            y = 0;
            for (j=0,x=i; j<8; j++,x+=32)
                y |= ((k[x / 64] >> (x % 64)) & 1) << j;

            sp_256_proj_point_dbl_avx2_4(rt, rt, t);
            XMEMCPY(p->x, table[y].x, sizeof(table[y].x));
            XMEMCPY(p->y, table[y].y, sizeof(table[y].y));
            p->infinity = table[y].infinity;
            sp_256_proj_point_add_qz1_avx2_4(rt, rt, p, t);
        }

        if (map)
            sp_256_map_avx2_4(r, rt, t);
        else
            XMEMCPY(r, rt, sizeof(sp_point));
    }

#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (t != NULL)
        XFREE(t, heap, DYNAMIC_TYPE_ECC);
#endif
    sp_ecc_point_free(p, 0, heap);
    sp_ecc_point_free(rt, 0, heap);

    return err;
}

#endif /* FP_ECC || WOLFSSL_SP_SMALL */
/* Multiply the base point of P256 by the scalar and return the result.
 * If map is true then convert result to affine co-ordinates.
 *
 * r     Resulting point.
 * g     Point to multiply.
 * k     Scalar to multiply by.
 * map   Indicates whether to convert result to affine.
 * heap  Heap to use for allocation.
 * returns MEMORY_E when memory allocation fails and MP_OKAY on success.
 */
static int sp_256_ecc_mulmod_avx2_4(sp_point* r, sp_point* g, sp_digit* k,
        int map, void* heap)
{
#ifndef FP_ECC
    return sp_256_ecc_mulmod_win_add_sub_avx2_4(r, g, k, map, heap);
#else
    sp_digit tmp[2 * 4 * 5];
    sp_cache_t* cache;
    int err = MP_OKAY;

#ifndef HAVE_THREAD_LS
    if (initCacheMutex == 0) {
         wc_InitMutex(&sp_cache_lock);
         initCacheMutex = 1;
    }
    if (wc_LockMutex(&sp_cache_lock) != 0)
       err = BAD_MUTEX_E;
#endif /* HAVE_THREAD_LS */

    if (err == MP_OKAY) {
        sp_ecc_get_cache(g, &cache);
        if (cache->cnt == 2)
            sp_256_gen_stripe_table_avx2_4(g, cache->table, tmp, heap);

#ifndef HAVE_THREAD_LS
        wc_UnLockMutex(&sp_cache_lock);
#endif /* HAVE_THREAD_LS */

        if (cache->cnt < 2) {
            err = sp_256_ecc_mulmod_win_add_sub_avx2_4(r, g, k, map, heap);
        }
        else {
            err = sp_256_ecc_mulmod_stripe_avx2_4(r, g, cache->table, k,
                    map, heap);
        }
    }

    return err;
#endif
}

#endif /* HAVE_INTEL_AVX2 */
/* Multiply the point by the scalar and return the result.
 * If map is true then convert result to affine co-ordinates.
 *
 * km    Scalar to multiply by.
 * p     Point to multiply.
 * r     Resulting point.
 * map   Indicates whether to convert result to affine.
 * heap  Heap to use for allocation.
 * returns MEMORY_E when memory allocation fails and MP_OKAY on success.
 */
int sp_ecc_mulmod_256(mp_int* km, ecc_point* gm, ecc_point* r, int map,
        void* heap)
{
#if !defined(WOLFSSL_SP_SMALL) && !defined(WOLFSSL_SMALL_STACK)
    sp_point p;
    sp_digit kd[4];
#endif
    sp_point* point;
    sp_digit* k = NULL;
    int err = MP_OKAY;
#ifdef HAVE_INTEL_AVX2
    word32 cpuid_flags = cpuid_get_flags();
#endif

    err = sp_ecc_point_new(heap, p, point);
#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (err == MP_OKAY) {
        k = (sp_digit*)XMALLOC(sizeof(sp_digit) * 4, heap,
                                                              DYNAMIC_TYPE_ECC);
        if (k == NULL)
            err = MEMORY_E;
    }
#else
    k = kd;
#endif
    if (err == MP_OKAY) {
        sp_256_from_mp(k, 4, km);
        sp_256_point_from_ecc_point_4(point, gm);

#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
            err = sp_256_ecc_mulmod_avx2_4(point, point, k, map, heap);
        else
#endif
            err = sp_256_ecc_mulmod_4(point, point, k, map, heap);
    }
    if (err == MP_OKAY)
        err = sp_256_point_to_ecc_point_4(point, r);

#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (k != NULL)
        XFREE(k, heap, DYNAMIC_TYPE_ECC);
#endif
    sp_ecc_point_free(point, 0, heap);

    return err;
}

#ifdef WOLFSSL_SP_SMALL
static sp_table_entry p256_table[256] = {
    /* 0 */
    { { 0x00, 0x00, 0x00, 0x00 },
      { 0x00, 0x00, 0x00, 0x00 },
      1 },
    /* 1 */
    { { 0x79e730d418a9143cl,0x75ba95fc5fedb601l,0x79fb732b77622510l,
        0x18905f76a53755c6l },
      { 0xddf25357ce95560al,0x8b4ab8e4ba19e45cl,0xd2e88688dd21f325l,
        0x8571ff1825885d85l },
      0 },
    /* 2 */
    { { 0x202886024147519al,0xd0981eac26b372f0l,0xa9d4a7caa785ebc8l,
        0xd953c50ddbdf58e9l },
      { 0x9d6361ccfd590f8fl,0x72e9626b44e6c917l,0x7fd9611022eb64cfl,
        0x863ebb7e9eb288f3l },
      0 },
    /* 3 */
    { { 0x7856b6235cdb6485l,0x808f0ea22f0a2f97l,0x3e68d9544f7e300bl,
        0x00076055b5ff80a0l },
      { 0x7634eb9b838d2010l,0x54014fbb3243708al,0xe0e47d39842a6606l,
        0x8308776134373ee0l },
      0 },
    /* 4 */
    { { 0x4f922fc516a0d2bbl,0x0d5cc16c1a623499l,0x9241cf3a57c62c8bl,
        0x2f5e6961fd1b667fl },
      { 0x5c15c70bf5a01797l,0x3d20b44d60956192l,0x04911b37071fdb52l,
        0xf648f9168d6f0f7bl },
      0 },
    /* 5 */
    { { 0x9e566847e137bbbcl,0xe434469e8a6a0becl,0xb1c4276179d73463l,
        0x5abe0285133d0015l },
      { 0x92aa837cc04c7dabl,0x573d9f4c43260c07l,0x0c93156278e6cc37l,
        0x94bb725b6b6f7383l },
      0 },
    /* 6 */
    { { 0xbbf9b48f720f141cl,0x6199b3cd2df5bc74l,0xdc3f6129411045c4l,
        0xcdd6bbcb2f7dc4efl },
      { 0xcca6700beaf436fdl,0x6f647f6db99326bel,0x0c0fa792014f2522l,
        0xa361bebd4bdae5f6l },
      0 },
    /* 7 */
    { { 0x28aa2558597c13c7l,0xc38d635f50b7c3e1l,0x07039aecf3c09d1dl,
        0xba12ca09c4b5292cl },
      { 0x9e408fa459f91dfdl,0x3af43b66ceea07fbl,0x1eceb0899d780b29l,
        0x53ebb99d701fef4bl },
      0 },
    /* 8 */
    { { 0x4fe7ee31b0e63d34l,0xf4600572a9e54fabl,0xc0493334d5e7b5a4l,
        0x8589fb9206d54831l },
      { 0xaa70f5cc6583553al,0x0879094ae25649e5l,0xcc90450710044652l,
        0xebb0696d02541c4fl },
      0 },
    /* 9 */
    { { 0x4616ca15ac1647c5l,0xb8127d47c4cf5799l,0xdc666aa3764dfbacl,
        0xeb2820cbd1b27da3l },
      { 0x9406f8d86a87e008l,0xd87dfa9d922378f3l,0x56ed2e4280ccecb2l,
        0x1f28289b55a7da1dl },
      0 },
    /* 10 */
    { { 0xabbaa0c03b89da99l,0xa6f2d79eb8284022l,0x27847862b81c05e8l,
        0x337a4b5905e54d63l },
      { 0x3c67500d21f7794al,0x207005b77d6d7f61l,0x0a5a378104cfd6e8l,
        0x0d65e0d5f4c2fbd6l },
      0 },
    /* 11 */
    { { 0xd9d09bbeb5275d38l,0x4268a7450be0a358l,0xf0762ff4973eb265l,
        0xc23da24252f4a232l },
      { 0x5da1b84f0b94520cl,0x09666763b05bd78el,0x3a4dcb8694d29ea1l,
        0x19de3b8cc790cff1l },
      0 },
    /* 12 */
    { { 0x183a716c26c5fe04l,0x3b28de0b3bba1bdbl,0x7432c586a4cb712cl,
        0xe34dcbd491fccbfdl },
      { 0xb408d46baaa58403l,0x9a69748682e97a53l,0x9e39012736aaa8afl,
        0xe7641f447b4e0f7fl },
      0 },
    /* 13 */
    { { 0x7d753941df64ba59l,0xd33f10ec0b0242fcl,0x4f06dfc6a1581859l,
        0x4a12df57052a57bfl },
      { 0xbfa6338f9439dbd0l,0xd3c24bd4bde53e1fl,0xfd5e4ffa21f1b314l,
        0x6af5aa93bb5bea46l },
      0 },
    /* 14 */
    { { 0xda10b69910c91999l,0x0a24b4402a580491l,0x3e0094b4b8cc2090l,
        0x5fe3475a66a44013l },
      { 0xb0f8cabdf93e7b4bl,0x292b501a7c23f91al,0x42e889aecd1e6263l,
        0xb544e308ecfea916l },
      0 },
    /* 15 */
    { { 0x6478c6e916ddfdcel,0x2c329166f89179e6l,0x4e8d6e764d4e67e1l,
        0xe0b6b2bda6b0c20bl },
      { 0x0d312df2bb7efb57l,0x1aac0dde790c4007l,0xf90336ad679bc944l,
        0x71c023de25a63774l },
      0 },
    /* 16 */
    { { 0x62a8c244bfe20925l,0x91c19ac38fdce867l,0x5a96a5d5dd387063l,
        0x61d587d421d324f6l },
      { 0xe87673a2a37173eal,0x2384800853778b65l,0x10f8441e05bab43el,
        0xfa11fe124621efbel },
      0 },
    /* 17 */
    { { 0x1c891f2b2cb19ffdl,0x01ba8d5bb1923c23l,0xb6d03d678ac5ca8el,
        0x586eb04c1f13bedcl },
      { 0x0c35c6e527e8ed09l,0x1e81a33c1819ede2l,0x278fd6c056c652fal,
        0x19d5ac0870864f11l },
      0 },
    /* 18 */
    { { 0x1e99f581309a4e1fl,0xab7de71be9270074l,0x26a5ef0befd28d20l,
        0xe7c0073f7f9c563fl },
      { 0x1f6d663a0ef59f76l,0x669b3b5420fcb050l,0xc08c1f7a7a6602d4l,
        0xe08504fec65b3c0al },
      0 },
    /* 19 */
    { { 0xf098f68da031b3cal,0x6d1cab9ee6da6d66l,0x5bfd81fa94f246e8l,
        0x78f018825b0996b4l },
      { 0xb7eefde43a25787fl,0x8016f80d1dccac9bl,0x0cea4877b35bfc36l,
        0x43a773b87e94747al },
      0 },
    /* 20 */
    { { 0x62577734d2b533d5l,0x673b8af6a1bdddc0l,0x577e7c9aa79ec293l,
        0xbb6de651c3b266b1l },
      { 0xe7e9303ab65259b3l,0xd6a0afd3d03a7480l,0xc5ac83d19b3cfc27l,
        0x60b4619a5d18b99bl },
      0 },
    /* 21 */
    { { 0xbd6a38e11ae5aa1cl,0xb8b7652b49e73658l,0x0b130014ee5f87edl,
        0x9d0f27b2aeebffcdl },
      { 0xca9246317a730a55l,0x9c955b2fddbbc83al,0x07c1dfe0ac019a71l,
        0x244a566d356ec48dl },
      0 },
    /* 22 */
    { { 0x6db0394aeacf1f96l,0x9f2122a9024c271cl,0x2626ac1b82cbd3b9l,
        0x45e58c873581ef69l },
      { 0xd3ff479da38f9dbcl,0xa8aaf146e888a040l,0x945adfb246e0bed7l,
        0xc040e21cc1e4b7a4l },
      0 },
    /* 23 */
    { { 0x847af0006f8117b6l,0x651969ff73a35433l,0x482b35761d9475ebl,
        0x1cdf5c97682c6ec7l },
      { 0x7db775b411f04839l,0x7dbeacf448de1698l,0xb2921dd1b70b3219l,
        0x046755f8a92dff3dl },
      0 },
    /* 24 */
    { { 0xcc8ac5d2bce8ffcdl,0x0d53c48b2fe61a82l,0xf6f161727202d6c7l,
        0x046e5e113b83a5f3l },
      { 0xe7b8ff64d8007f01l,0x7fb1ef125af43183l,0x045c5ea635e1a03cl,
        0x6e0106c3303d005bl },
      0 },
    /* 25 */
    { { 0x48c7358488dd73b1l,0x7670708f995ed0d9l,0x38385ea8c56a2ab7l,
        0x442594ede901cf1fl },
      { 0xf8faa2c912d4b65bl,0x94c2343b96c90c37l,0xd326e4a15e978d1fl,
        0xa796fa514c2ee68el },
      0 },
    /* 26 */
    { { 0x359fb604823addd7l,0x9e2a6183e56693b3l,0xf885b78e3cbf3c80l,
        0xe4ad2da9c69766e9l },
      { 0x357f7f428e048a61l,0x082d198cc092d9a0l,0xfc3a1af4c03ed8efl,
        0xc5e94046c37b5143l },
      0 },
    /* 27 */
    { { 0x476a538c2be75f9el,0x6fd1a9e8cb123a78l,0xd85e4df0b109c04bl,
        0x63283dafdb464747l },
      { 0xce728cf7baf2df15l,0xe592c4550ad9a7f4l,0xfab226ade834bcc3l,
        0x68bd19ab1981a938l },
      0 },
    /* 28 */
    { { 0xc08ead511887d659l,0x3374d5f4b359305al,0x96986981cfe74fe3l,
        0x495292f53c6fdfd6l },
      { 0x4a878c9e1acec896l,0xd964b210ec5b4484l,0x6696f7e2664d60a7l,
        0x0ec7530d26036837l },
      0 },
    /* 29 */
    { { 0x2da13a05ad2687bbl,0xa1f83b6af32e21fal,0x390f5ef51dd4607bl,
        0x0f6207a664863f0bl },
      { 0xbd67e3bb0f138233l,0xdd66b96c272aa718l,0x8ed0040726ec88ael,
        0xff0db07208ed6dcfl },
      0 },
    /* 30 */
    { { 0x749fa1014c95d553l,0xa44052fd5d680a8al,0x183b4317ff3b566fl,
        0x313b513c88740ea3l },
      { 0xb402e2ac08d11549l,0x071ee10bb4dee21cl,0x26b987dd47f2320el,
        0x2d3abcf986f19f81l },
      0 },
    /* 31 */
    { { 0x4c288501815581a2l,0x9a0a6d56632211afl,0x19ba7a0f0cab2e99l,
        0xc036fa10ded98cdfl },
      { 0x29ae08bac1fbd009l,0x0b68b19006d15816l,0xc2eb32779b9e0d8fl,
        0xa6b2a2c4b6d40194l },
      0 },
    /* 32 */
    { { 0xd433e50f6d3549cfl,0x6f33696ffacd665el,0x695bfdacce11fcb4l,
        0x810ee252af7c9860l },
      { 0x65450fe17159bb2cl,0xf7dfbebe758b357bl,0x2b057e74d69fea72l,
        0xd485717a92731745l },
      0 },
    /* 33 */
    { { 0x11741a8af0cb5a98l,0xd3da8f931f3110bfl,0x1994e2cbab382adfl,
        0x6a6045a72f9a604el },
      { 0x170c0d3fa2b2411dl,0xbe0eb83e510e96e0l,0x3bcc9f738865b3ccl,
        0xd3e45cfaf9e15790l },
      0 },
    /* 34 */
    { { 0xce1f69bbe83f7669l,0x09f8ae8272877d6bl,0x9548ae543244278dl,
        0x207755dee3c2c19cl },
      { 0x87bd61d96fef1945l,0x18813cefb12d28c3l,0x9fbcd1d672df64aal,
        0x48dc5ee57154b00dl },
      0 },
    /* 35 */
    { { 0x123790bff7e5a199l,0xe0efb8cf989ccbb7l,0xc27a2bfe0a519c79l,
        0xf2fb0aeddff6f445l },
      { 0x41c09575f0b5025fl,0x550543d740fa9f22l,0x8fa3c8ad380bfbd0l,
        0xa13e9015db28d525l },
      0 },
    /* 36 */
    { { 0xf9f7a350a2b65cbcl,0x0b04b9722a464226l,0x265ce241e23f07a1l,
        0x2bf0d6b01497526fl },
      { 0xd3d4dd3f4b216fb7l,0xf7d7b867fbdda26al,0xaeb7b83f6708505cl,
        0x42a94a5a162fe89fl },
      0 },
    /* 37 */
    { { 0x5846ad0beaadf191l,0x0f8a489025a268d7l,0xe8603050494dc1f6l,
        0x2c2dd969c65ede3dl },
      { 0x6d02171d93849c17l,0x460488ba1da250ddl,0x4810c7063c3a5485l,
        0xf437fa1f42c56dbcl },
      0 },
    /* 38 */
    { { 0x6aa0d7144a0f7dabl,0x0f0497931776e9acl,0x52c0a050f5f39786l,
        0xaaf45b3354707aa8l },
      { 0x85e37c33c18d364al,0xd40b9b063e497165l,0xf417168115ec5444l,
        0xcdf6310df4f272bcl },
      0 },
    /* 39 */
    { { 0x7473c6238ea8b7efl,0x08e9351885bc2287l,0x419567722bda8e34l,
        0xf0d008bada9e2ff2l },
      { 0x2912671d2414d3b1l,0xb3754985b019ea76l,0x5c61b96d453bcbdbl,
        0x5bd5c2f5ca887b8bl },
      0 },
    /* 40 */
    { { 0xef0f469ef49a3154l,0x3e85a5956e2b2e9al,0x45aaec1eaa924a9cl,
        0xaa12dfc8a09e4719l },
      { 0x26f272274df69f1dl,0xe0e4c82ca2ff5e73l,0xb9d8ce73b7a9dd44l,
        0x6c036e73e48ca901l },
      0 },
    /* 41 */
    { { 0x5cfae12a0f6e3138l,0x6966ef0025ad345al,0x8993c64b45672bc5l,
        0x292ff65896afbe24l },
      { 0xd5250d445e213402l,0xf6580e274392c9fel,0x097b397fda1c72e8l,
        0x644e0c90311b7276l },
      0 },
    /* 42 */
    { { 0xe1e421e1a47153f0l,0xb86c3b79920418c9l,0x93bdce87705d7672l,
        0xf25ae793cab79a77l },
      { 0x1f3194a36d869d0cl,0x9d55c8824986c264l,0x49fb5ea3096e945el,
        0x39b8e65313db0a3el },
      0 },
    /* 43 */
    { { 0x37754200b6fd2e59l,0x35e2c0669255c98fl,0xd9dab21a0e2a5739l,
        0x39122f2f0f19db06l },
      { 0xcfbce1e003cad53cl,0x225b2c0fe65c17e3l,0x72baf1d29aa13877l,
        0x8de80af8ce80ff8dl },
      0 },
    /* 44 */
    { { 0xafbea8d9207bbb76l,0x921c7e7c21782758l,0xdfa2b74b1c0436b1l,
        0x871949062e368c04l },
      { 0xb5f928bba3993df5l,0x639d75b5f3b3d26al,0x011aa78a85b55050l,
        0xfc315e6a5b74fde1l },
      0 },
    /* 45 */
    { { 0x561fd41ae8d6ecfal,0x5f8c44f61aec7f86l,0x98452a7b4924741dl,
        0xe6d4a7adee389088l },
      { 0x60552ed14593c75dl,0x70a70da4dd271162l,0xd2aede937ba2c7dbl,
        0x35dfaf9a9be2ae57l },
      0 },
    /* 46 */
    { { 0x6b956fcdaa736636l,0x09f51d97ae2cab7el,0xfb10bf410f349966l,
        0x1da5c7d71c830d2bl },
      { 0x5c41e4833cce6825l,0x15ad118ff9573c3bl,0xa28552c7f23036b8l,
        0x7077c0fddbf4b9d6l },
      0 },
    /* 47 */
    { { 0xbf63ff8d46b9661cl,0xa1dfd36b0d2cfd71l,0x0373e140a847f8f7l,
        0x53a8632ee50efe44l },
      { 0x0976ff68696d8051l,0xdaec0c95c74f468al,0x62994dc35e4e26bdl,
        0x028ca76d34e1fcc1l },
      0 },
    /* 48 */
    { { 0xd11d47dcfc9877eel,0xc8b36210801d0002l,0xd002c11754c260b6l,
        0x04c17cd86962f046l },
      { 0x6d9bd094b0daddf5l,0xbea2357524ce55c0l,0x663356e672da03b5l,
        0xf7ba4de9fed97474l },
      0 },
    /* 49 */
    { { 0xd0dbfa34ebe1263fl,0x5576373571ae7ce6l,0xd244055382a6f523l,
        0xe31f960052131c41l },
      { 0xd1bb9216ea6b6ec6l,0x37a1d12e73c2fc44l,0xc10e7eac89d0a294l,
        0xaa3a6259ce34d47bl },
      0 },
    /* 50 */
    { { 0xfbcf9df536f3dcd3l,0x6ceded50d2bf7360l,0x491710fadf504f5bl,
        0x2398dd627e79daeel },
      { 0xcf4705a36d09569el,0xea0619bb5149f769l,0xff9c037735f6034cl,
        0x5717f5b21c046210l },
      0 },
    /* 51 */
    { { 0x9fe229c921dd895el,0x8e51850040c28451l,0xfa13d2391d637ecdl,
        0x660a2c560e3c28del },
      { 0x9cca88aed67fcbd0l,0xc84724780ea9f096l,0x32b2f48172e92b4dl,
        0x624ee54c4f522453l },
      0 },
    /* 52 */
    { { 0x09549ce4d897ecccl,0x4d49d1d93f9880aal,0x723c2423043a7c20l,
        0x4f392afb92bdfbc0l },
      { 0x6969f8fa7de44fd9l,0xb66cfbe457b32156l,0xdb2fa803368ebc3cl,
        0x8a3e7977ccdb399cl },
      0 },
    /* 53 */
    { { 0xdde1881f06c4b125l,0xae34e300f6e3ca8cl,0xef6999de5c7a13e9l,
        0x3888d02370c24404l },
      { 0x7628035644f91081l,0x3d9fcf615f015504l,0x1827edc8632cd36el,
        0xa5e62e4718102336l },
      0 },
    /* 54 */
    { { 0x1a825ee32facd6c8l,0x699c635454bcbc66l,0x0ce3edf798df9931l,
        0x2c4768e6466a5adcl },
      { 0xb346ff8c90a64bc9l,0x630a6020e4779f5cl,0xd949d064bc05e884l,
        0x7b5e6441f9e652a0l },
      0 },
    /* 55 */
    { { 0x2169422c1d28444al,0xe996c5d8be136a39l,0x2387afe5fb0c7fcel,
        0xb8af73cb0c8d744al },
      { 0x5fde83aa338b86fdl,0xfee3f158a58a5cffl,0xc9ee8f6f20ac9433l,
        0xa036395f7f3f0895l },
      0 },
    /* 56 */
    { { 0x8c73c6bba10f7770l,0xa6f16d81a12a0e24l,0x100df68251bc2b9fl,
        0x4be36b01875fb533l },
      { 0x9226086e9fb56dbbl,0x306fef8b07e7a4f8l,0xeeaccc0566d52f20l,
        0x8cbc9a871bdc00c0l },
      0 },
    /* 57 */
    { { 0xe131895cc0dac4abl,0xa874a440712ff112l,0x6332ae7c6a1cee57l,
        0x44e7553e0c0835f8l },
      { 0x6d503fff7734002dl,0x9d35cb8b0b34425cl,0x95f702760e8738b5l,
        0x470a683a5eb8fc18l },
      0 },
    /* 58 */
    { { 0x81b761dc90513482l,0x0287202a01e9276al,0xcda441ee0ce73083l,
        0x16410690c63dc6efl },
      { 0xf5034a066d06a2edl,0xdd4d7745189b100bl,0xd914ae72ab8218c9l,
        0xd73479fd7abcbb4fl },
      0 },
    /* 59 */
    { { 0x7edefb165ad4c6e5l,0x262cf08f5b06d04dl,0x12ed5bb18575cb14l,
        0x816469e30771666bl },
      { 0xd7ab9d79561e291el,0xeb9daf22c1de1661l,0xf49827eb135e0513l,
        0x0a36dd23f0dd3f9cl },
      0 },
    /* 60 */
    { { 0x098d32c741d5533cl,0x7c5f5a9e8684628fl,0x39a228ade349bd11l,
        0xe331dfd6fdbab118l },
      { 0x5100ab686bcc6ed8l,0x7160c3bdef7a260el,0x9063d9a7bce850d7l,
        0xd3b4782a492e3389l },
      0 },
    /* 61 */
    { { 0xa149b6e8f3821f90l,0x92edd9ed66eb7aadl,0x0bb669531a013116l,
        0x7281275a4c86a5bdl },
      { 0x503858f7d3ff47e5l,0x5e1616bc61016441l,0x62b0f11a7dfd9bb1l,
        0x2c062e7ece145059l },
      0 },
    /* 62 */
    { { 0xa76f996f0159ac2el,0x281e7736cbdb2713l,0x2ad6d28808e46047l,
        0x282a35f92c4e7ef1l },
      { 0x9c354b1ec0ce5cd2l,0xcf99efc91379c229l,0x992caf383e82c11el,
        0xc71cd513554d2abdl },
      0 },
    /* 63 */
    { { 0x4885de9c09b578f4l,0x1884e258e3affa7al,0x8f76b1b759182f1fl,
        0xc50f6740cf47f3a3l },
      { 0xa9c4adf3374b68eal,0xa406f32369965fe2l,0x2f86a22285a53050l,
        0xb9ecb3a7212958dcl },
      0 },
    /* 64 */
    { { 0x56f8410ef4f8b16al,0x97241afec47b266al,0x0a406b8e6d9c87c1l,
        0x803f3e02cd42ab1bl },
      { 0x7f0309a804dbec69l,0xa83b85f73bbad05fl,0xc6097273ad8e197fl,
        0xc097440e5067adc1l },
      0 },
    /* 65 */
    { { 0x846a56f2c379ab34l,0xa8ee068b841df8d1l,0x20314459176c68efl,
        0xf1af32d5915f1f30l },
      { 0x99c375315d75bd50l,0x837cffbaf72f67bcl,0x0613a41848d7723fl,
        0x23d0f130e2d41c8bl },
      0 },
    /* 66 */
    { { 0x857ab6edf41500d9l,0x0d890ae5fcbeada8l,0x52fe864889725951l,
        0xb0288dd6c0a3faddl },
      { 0x85320f30650bcb08l,0x71af6313695d6e16l,0x31f520a7b989aa76l,
        0xffd3724ff408c8d2l },
      0 },
    /* 67 */
    { { 0x53968e64b458e6cbl,0x992dad20317a5d28l,0x3814ae0b7aa75f56l,
        0xf5590f4ad78c26dfl },
      { 0x0fc24bd3cf0ba55al,0x0fc4724a0c778bael,0x1ce9864f683b674al,
        0x18d6da54f6f74a20l },
      0 },
    /* 68 */
    { { 0xed93e225d5be5a2bl,0x6fe799835934f3c6l,0x4314092622626ffcl,
        0x50bbb4d97990216al },
      { 0x378191c6e57ec63el,0x65422c40181dcdb2l,0x41a8099b0236e0f6l,
        0x2b10011801fe49c3l },
      0 },
    /* 69 */
    { { 0xfc68b5c59b391593l,0xc385f5a2598270fcl,0x7144f3aad19adcbbl,
        0xdd55899983fbae0cl },
      { 0x93b88b8e74b82ff4l,0xd2e03c4071e734c9l,0x9a7a9eaf43c0322al,
        0xe6e4c551149d6041l },
      0 },
    /* 70 */
    { { 0x55f655bb1e9af288l,0x647e1a64f7ada931l,0x43697e4bcb2820e5l,
        0x51e00db107ed56ffl },
      { 0x43d169b8771c327el,0x29cdb20b4a96c2adl,0xc07d51f53deb4779l,
        0xe22f424149829177l },
      0 },
    /* 71 */
    { { 0xcd45e8f4635f1abbl,0x7edc0cb568538874l,0xc9472c1fb5a8034dl,
        0xf709373d52dc48c9l },
      { 0x401966bba8af30d6l,0x95bf5f4af137b69cl,0x3966162a9361c47el,
        0xbd52d288e7275b11l },
      0 },
    /* 72 */
    { { 0xab155c7a9c5fa877l,0x17dad6727d3a3d48l,0x43f43f9e73d189d8l,
        0xa0d0f8e4c8aa77a6l },
      { 0x0bbeafd8cc94f92dl,0xd818c8be0c4ddb3al,0x22cc65f8b82eba14l,
        0xa56c78c7946d6a00l },
      0 },
    /* 73 */
    { { 0x2962391b0dd09529l,0x803e0ea63daddfcfl,0x2c77351f5b5bf481l,
        0xd8befdf8731a367al },
      { 0xab919d42fc0157f4l,0xf51caed7fec8e650l,0xcdf9cb4002d48b0al,
        0x854a68a5ce9f6478l },
      0 },
    /* 74 */
    { { 0xdc35f67b63506ea5l,0x9286c489a4fe0d66l,0x3f101d3bfe95cd4dl,
        0x5cacea0b98846a95l },
      { 0xa90df60c9ceac44dl,0x3db29af4354d1c3al,0x08dd3de8ad5dbabel,
        0xe4982d1235e4efa9l },
      0 },
    /* 75 */
    { { 0x23104a22c34cd55el,0x58695bb32680d132l,0xfb345afa1fa1d943l,
        0x8046b7f616b20499l },
      { 0xb533581e38e7d098l,0xd7f61e8df46f0b70l,0x30dea9ea44cb78c4l,
        0xeb17ca7b9082af55l },
      0 },
    /* 76 */
    { { 0x1751b59876a145b9l,0xa5cf6b0fc1bc71ecl,0xd3e03565392715bbl,
        0x097b00bafab5e131l },
      { 0xaa66c8e9565f69e1l,0x77e8f75ab5be5199l,0x6033ba11da4fd984l,
        0xf95c747bafdbcc9el },
      0 },
    /* 77 */
    { { 0x558f01d3bebae45el,0xa8ebe9f0c4bc6955l,0xaeb705b1dbc64fc6l,
        0x3512601e566ed837l },
      { 0x9336f1e1fa1161cdl,0x328ab8d54c65ef87l,0x4757eee2724f21e5l,
        0x0ef971236068ab6bl },
      0 },
    /* 78 */
    { { 0x02598cf754ca4226l,0x5eede138f8642c8el,0x48963f74468e1790l,
        0xfc16d9333b4fbc95l },
      { 0xbe96fb31e7c800cal,0x138063312678adaal,0x3d6244976ff3e8b5l,
        0x14ca4af1b95d7a17l },
      0 },
    /* 79 */
    { { 0x7a4771babd2f81d5l,0x1a5f9d6901f7d196l,0xd898bef7cad9c907l,
        0x4057b063f59c231dl },
      { 0xbffd82fe89c05c0al,0xe4911c6f1dc0df85l,0x3befccaea35a16dbl,
        0x1c3b5d64f1330b13l },
      0 },
    /* 80 */
    { { 0x5fe14bfe80ec21fel,0xf6ce116ac255be82l,0x98bc5a072f4a5d67l,
        0xfad27148db7e63afl },
      { 0x90c0b6ac29ab05b3l,0x37a9a83c4e251ae6l,0x0a7dc875c2aade7dl,
        0x77387de39f0e1a84l },
      0 },
    /* 81 */
    { { 0x1e9ecc49a56c0dd7l,0xa5cffcd846086c74l,0x8f7a1408f505aecel,
        0xb37b85c0bef0c47el },
      { 0x3596b6e4cc0e6a8fl,0xfd6d4bbf6b388f23l,0xaba453fac39cef4el,
        0x9c135ac8f9f628d5l },
      0 },
    /* 82 */
    { { 0x32aa320284e35743l,0x320d6ab185a3cdefl,0xb821b1761df19819l,
        0x5721361fc433851fl },
      { 0x1f0db36a71fc9168l,0x5f98ba735e5c403cl,0xf64ca87e37bcd8f5l,
        0xdcbac3c9e6bb11bdl },
      0 },
    /* 83 */
    { { 0xf01d99684518cbe2l,0xd242fc189c9eb04el,0x727663c7e47feebfl,
        0xb8c1c89e2d626862l },
      { 0x51a58bddc8e1d569l,0x563809c8b7d88cd0l,0x26c27fd9f11f31ebl,
        0x5d23bbda2f9422d4l },
      0 },
    /* 84 */
    { { 0x0a1c729495c8f8bel,0x2961c4803bf362bfl,0x9e418403df63d4acl,
        0xc109f9cb91ece900l },
      { 0xc2d095d058945705l,0xb9083d96ddeb85c0l,0x84692b8d7a40449bl,
        0x9bc3344f2eee1ee1l },
      0 },
    /* 85 */
    { { 0x0d5ae35642913074l,0x55491b2748a542b1l,0x469ca665b310732al,
        0x29591d525f1a4cc1l },
      { 0xe76f5b6bb84f983fl,0xbe7eef419f5f84e1l,0x1200d49680baa189l,
        0x6376551f18ef332cl },
      0 },
    /* 86 */
    { { 0xbda5f14e562976ccl,0x22bca3e60ef12c38l,0xbbfa30646cca9852l,
        0xbdb79dc808e2987al },
      { 0xfd2cb5c9cb06a772l,0x38f475aafe536dcel,0xc2a3e0227c2b5db8l,
        0x8ee86001add3c14al },
      0 },
    /* 87 */
    { { 0xcbe96981a4ade873l,0x7ee9aa4dc4fba48cl,0x2cee28995a054ba5l,
        0x92e51d7a6f77aa4bl },
      { 0x948bafa87190a34dl,0xd698f75bf6bd1ed1l,0xd00ee6e30caf1144l,
        0x5182f86f0a56aaaal },
      0 },
    /* 88 */
    { { 0xfba6212c7a4cc99cl,0xff609b683e6d9ca1l,0x5dbb27cb5ac98c5al,
        0x91dcab5d4073a6f2l },
      { 0x01b6cc3d5f575a70l,0x0cb361396f8d87fal,0x165d4e8c89981736l,
        0x17a0cedb97974f2bl },
      0 },
    /* 89 */
    { { 0x38861e2a076c8d3al,0x701aad39210f924bl,0x94d0eae413a835d9l,
        0x2e8ce36c7f4cdf41l },
      { 0x91273dab037a862bl,0x01ba9bb760e4c8fal,0xf964538833baf2ddl,
        0xf4ccc6cb34f668f3l },
      0 },
    /* 90 */
    { { 0x44ef525cf1f79687l,0x7c59549592efa815l,0xe1231741a5c78d29l,
        0xac0db4889a0df3c9l },
      { 0x86bfc711df01747fl,0x592b9358ef17df13l,0xe5880e4f5ccb6bb5l,
        0x95a64a6194c974a2l },
      0 },
    /* 91 */
    { { 0x72c1efdac15a4c93l,0x40269b7382585141l,0x6a8dfb1c16cb0badl,
        0x231e54ba29210677l },
      { 0xa70df9178ae6d2dcl,0x4d6aa63f39112918l,0xf627726b5e5b7223l,
        0xab0be032d8a731e1l },
      0 },
    /* 92 */
    { { 0x097ad0e98d131f2dl,0x637f09e33b04f101l,0x1ac86196d5e9a748l,
        0xf1bcc8802cf6a679l },
      { 0x25c69140e8daacb4l,0x3c4e405560f65009l,0x591cc8fc477937a6l,
        0x851694695aebb271l },
      0 },
    /* 93 */
    { { 0xde35c143f1dcf593l,0x78202b29b018be3bl,0xe9cdadc29bdd9d3dl,
        0x8f67d9d2daad55d8l },
      { 0x841116567481ea5fl,0xe7d2dde9e34c590cl,0xffdd43f405053fa8l,
        0xf84572b9c0728b5dl },
      0 },
    /* 94 */
    { { 0x5e1a7a7197af71c9l,0xa14494447a736565l,0xa1b4ae070e1d5063l,
        0xedee2710616b2c19l },
      { 0xb2f034f511734121l,0x1cac6e554a25e9f0l,0x8dc148f3a40c2ecfl,
        0x9fd27e9b44ebd7f4l },
      0 },
    /* 95 */
    { { 0x3cc7658af6e2cb16l,0xe3eb7d2cfe5919b6l,0x5a8c5816168d5583l,
        0xa40c2fb6958ff387l },
      { 0x8c9ec560fedcc158l,0x7ad804c655f23056l,0xd93967049a307e12l,
        0x99bc9bb87dc6decfl },
      0 },
    /* 96 */
    { { 0x84a9521d927dafc6l,0x52c1fb695c09cd19l,0x9d9581a0f9366ddel,
        0x9abe210ba16d7e64l },
      { 0x480af84a48915220l,0xfa73176a4dd816c6l,0xc7d539871681ca5al,
        0x7881c25787f344b0l },
      0 },
    /* 97 */
    { { 0x93399b51e0bcf3ffl,0x0d02cbc5127f74f6l,0x8fb465a2dd01d968l,
        0x15e6e319a30e8940l },
      { 0x646d6e0d3e0e05f4l,0xfad7bddc43588404l,0xbe61c7d1c4f850d3l,
        0x0e55facf191172cel },
      0 },
    /* 98 */
    { { 0x7e9d9806f8787564l,0x1a33172131e85ce6l,0x6b0158cab819e8d6l,
        0xd73d09766fe96577l },
      { 0x424834251eb7206el,0xa519290fc618bb42l,0x5dcbb8595e30a520l,
        0x9250a3748f15a50bl },
      0 },
    /* 99 */
    { { 0xcaff08f8be577410l,0xfd408a035077a8c6l,0xf1f63289ec0a63a4l,
        0x77414082c1cc8c0bl },
      { 0x05a40fa6eb0991cdl,0xc1ca086649fdc296l,0x3a68a3c7b324fd40l,
        0x8cb04f4d12eb20b9l },
      0 },
    /* 100 */
    { { 0xb1c2d0556906171cl,0x9073e9cdb0240c3fl,0xdb8e6b4fd8906841l,
        0xe4e429ef47123b51l },
      { 0x0b8dd53c38ec36f4l,0xf9d2dc01ff4b6a27l,0x5d066e07879a9a48l,
        0x37bca2ff3c6e6552l },
      0 },
    /* 101 */
    { { 0x4cd2e3c7df562470l,0x44f272a2c0964ac9l,0x7c6d5df980c793bel,
        0x59913edc3002b22al },
      { 0x7a139a835750592al,0x99e01d80e783de02l,0xcf8c0375ea05d64fl,
        0x43786e4ab013e226l },
      0 },
    /* 102 */
    { { 0xff32b0ed9e56b5a6l,0x0750d9a6d9fc68f9l,0xec15e845597846a7l,
        0x8638ca98b7e79e7al },
      { 0x2f5ae0960afc24b2l,0x05398eaf4dace8f2l,0x3b765dd0aecba78fl,
        0x1ecdd36a7b3aa6f0l },
      0 },
    /* 103 */
    { { 0x5d3acd626c5ff2f3l,0xa2d516c02873a978l,0xad94c9fad2110d54l,
        0xd85d0f85d459f32dl },
      { 0x9f700b8d10b11da3l,0xd2c22c30a78318c4l,0x556988f49208decdl,
        0xa04f19c3b4ed3c62l },
      0 },
    /* 104 */
    { { 0x087924c8ed7f93bdl,0xcb64ac5d392f51f6l,0x7cae330a821b71afl,
        0x92b2eeea5c0950b0l },
      { 0x85ac4c9485b6e235l,0xab2ca4a92936c0f0l,0x80faa6b3e0508891l,
        0x1ee782215834276cl },
      0 },
    /* 105 */
    { { 0xa60a2e00e63e79f7l,0xf590e7b2f399d906l,0x9021054a6607c09dl,
        0xf3f2ced857a6e150l },
      { 0x200510f3f10d9b55l,0x9d2fcfacd8642648l,0xe5631aa7e8bd0e7cl,
        0x0f56a4543da3e210l },
      0 },
    /* 106 */
    { { 0x5b21bffa1043e0dfl,0x6c74b6cc9c007e6dl,0x1a656ec0d4a8517al,
        0xbd8f17411969e263l },
      { 0x8a9bbb86beb7494al,0x1567d46f45f3b838l,0xdf7a12a7a4e5a79al,
        0x2d1a1c3530ccfa09l },
      0 },
    /* 107 */
    { { 0x192e3813506508dal,0x336180c4a1d795a7l,0xcddb59497a9944b3l,
        0xa107a65eb91fba46l },
      { 0xe6d1d1c50f94d639l,0x8b4af3758a58b7d7l,0x1a7c5584bd37ca1cl,
        0x183d760af87a9af2l },
      0 },
    /* 108 */
    { { 0x29d697110dde59a4l,0xf1ad8d070e8bef87l,0x229b49634f2ebe78l,
        0x1d44179dc269d754l },
      { 0xb32dc0cf8390d30el,0x0a3b27530de8110cl,0x31af1dc52bc0339al,
        0x771f9cc29606d262l },
      0 },
    /* 109 */
    { { 0x99993e7785040739l,0x44539db98026a939l,0xcf40f6f2f5f8fc26l,
        0x64427a310362718el },
      { 0x4f4f2d8785428aa8l,0x7b7adc3febfb49a8l,0x201b2c6df23d01acl,
        0x49d9b7496ae90d6dl },
      0 },
    /* 110 */
    { { 0xcc78d8bc435d1099l,0x2adbcd4e8e8d1a08l,0x02c2e2a02cb68a41l,
        0x9037d81b3f605445l },
      { 0x7cdbac27074c7b61l,0xfe2031ab57bfd72el,0x61ccec96596d5352l,
        0x08c3de6a7cc0639cl },
      0 },
    /* 111 */
    { { 0x20fdd020f6d552abl,0x56baff9805cd81f1l,0x06fb7c3e91351291l,
        0xc690944245796b2fl },
      { 0x17b3ae9c41231bd1l,0x1eac6e875cc58205l,0x208837abf9d6a122l,
        0x3fa3db02cafe3ac0l },
      0 },
    /* 112 */
    { { 0xd75a3e6505058880l,0x7da365ef643943f2l,0x4147861cfab24925l,
        0xc5c4bdb0fdb808ffl },
      { 0x73513e34b272b56bl,0xc8327e9511b9043al,0xfd8ce37df8844969l,
        0x2d56db9446c2b6b5l },
      0 },
    /* 113 */
    { { 0x2461782fff46ac6bl,0xd19f792607a2e425l,0xfafea3c409a48de1l,
        0x0f56bd9de503ba42l },
      { 0x137d4ed1345cda49l,0x821158fc816f299dl,0xe7c6a54aaeb43402l,
        0x4003bb9d1173b5f1l },
      0 },
    /* 114 */
    { { 0x3b8e8189a0803387l,0xece115f539cbd404l,0x4297208dd2877f21l,
        0x53765522a07f2f9el },
      { 0xa4980a21a8a4182dl,0xa2bbd07a3219df79l,0x674d0a2e1a19a2d4l,
        0x7a056f586c5d4549l },
      0 },
    /* 115 */
    { { 0x646b25589d8a2a47l,0x5b582948c3df2773l,0x51ec000eabf0d539l,
        0x77d482f17a1a2675l },
      { 0xb8a1bd9587853948l,0xa6f817bd6cfbffeel,0xab6ec05780681e47l,
        0x4115012b2b38b0e4l },
      0 },
    /* 116 */
    { { 0x3c73f0f46de28cedl,0x1d5da7609b13ec47l,0x61b8ce9e6e5c6392l,
        0xcdf04572fbea0946l },
      { 0x1cb3c58b6c53c3b0l,0x97fe3c10447b843cl,0xfb2b8ae12cb9780el,
        0xee703dda97383109l },
      0 },
    /* 117 */
    { { 0x34515140ff57e43al,0xd44660d3b1b811b8l,0x2b3b5dff8f42b986l,
        0x2a0ad89da162ce21l },
      { 0x64e4a6946bc277bal,0xc788c954c141c276l,0x141aa64ccabf6274l,
        0xd62d0b67ac2b4659l },
      0 },
    /* 118 */
    { { 0x39c5d87b2c054ac4l,0x57005859f27df788l,0xedf7cbf3b18128d6l,
        0xb39a23f2991c2426l },
      { 0x95284a15f0b16ae5l,0x0c6a05b1a136f51bl,0x1d63c137f2700783l,
        0x04ed0092c0674cc5l },
      0 },
    /* 119 */
    { { 0x1f4185d19ae90393l,0x3047b4294a3d64e6l,0xae0001a69854fc14l,
        0xa0a91fc10177c387l },
      { 0xff0a3f01ae2c831el,0xbb76ae822b727e16l,0x8f12c8a15a3075b4l,
        0x084cf9889ed20c41l },
      0 },
    /* 120 */
    { { 0xd98509defca6becfl,0x2fceae807dffb328l,0x5d8a15c44778e8b9l,
        0xd57955b273abf77el },
      { 0x210da79e31b5d4f1l,0xaa52f04b3cfa7a1cl,0xd4d12089dc27c20bl,
        0x8e14ea4202d141f1l },
      0 },
    /* 121 */
    { { 0xeed50345f2897042l,0x8d05331f43402c4al,0xc8d9c194c8bdfb21l,
        0x597e1a372aa4d158l },
      { 0x0327ec1acf0bd68cl,0x6d4be0dcab024945l,0x5b9c8d7ac9fe3e84l,
        0xca3f0236199b4deal },
      0 },
    /* 122 */
    { { 0x592a10b56170bd20l,0x0ea897f16d3f5de7l,0xa3363ff144b2ade2l,
        0xbde7fd7e309c07e4l },
      { 0x516bb6d2b8f5432cl,0x210dc1cbe043444bl,0x3db01e6ff8f95b5al,
        0xb623ad0e0a7dd198l },
      0 },
    /* 123 */
    { { 0xa75bd67560c7b65bl,0xab8c559023a4a289l,0xf8220fd0d7b26795l,
        0xd6aa2e4658ec137bl },
      { 0x10abc00b5138bb85l,0x8c31d121d833a95cl,0xb24ff00b1702a32el,
        0x111662e02dcc513al },
      0 },
    /* 124 */
    { { 0x78114015efb42b87l,0xbd9f5d701b6c4dffl,0x66ecccd7a7d7c129l,
        0xdb3ee1cb94b750f8l },
      { 0xb26f3db0f34837cfl,0xe7eed18bb9578d4fl,0x5d2cdf937c56657dl,
        0x886a644252206a59l },
      0 },
    /* 125 */
    { { 0x3c234cfb65b569eal,0x20011141f72119c1l,0x8badc85da15a619el,
        0xa70cf4eb018a17bcl },
      { 0x224f97ae8c4a6a65l,0x36e5cf270134378fl,0xbe3a609e4f7e0960l,
        0xaa4772abd1747b77l },
      0 },
    /* 126 */
    { { 0x676761317aa60cc0l,0xc79163610368115fl,0xded98bb4bbc1bb5al,
        0x611a6ddc30faf974l },
      { 0x30e78cbcc15ee47al,0x2e8962824e0d96a5l,0x36f35adf3dd9ed88l,
        0x5cfffaf816429c88l },
      0 },
    /* 127 */
    { { 0xc0d54cff9b7a99cdl,0x7bf3b99d843c45a1l,0x038a908f62c739e1l,
        0x6e5a6b237dc1994cl },
      { 0xef8b454e0ba5db77l,0xb7b8807facf60d63l,0xe591c0c676608378l,
        0x481a238d242dabccl },
      0 },
    /* 128 */
    { { 0xe3417bc035d0b34al,0x440b386b8327c0a7l,0x8fb7262dac0362d1l,
        0x2c41114ce0cdf943l },
      { 0x2ba5cef1ad95a0b1l,0xc09b37a867d54362l,0x26d6cdd201e486c9l,
        0x20477abf42ff9297l },
      0 },
    /* 129 */
    { { 0x2f75173c18d65dbfl,0x77bf940e339edad8l,0x7022d26bdcf1001cl,
        0xac66409ac77396b6l },
      { 0x8b0bb36fc6261cc3l,0x213f7bc9190e7e90l,0x6541cebaa45e6c10l,
        0xce8e6975cc122f85l },
      0 },
    /* 130 */
    { { 0x0f121b41bc0a67d2l,0x62d4760a444d248al,0x0e044f1d659b4737l,
        0x08fde365250bb4a8l },
      { 0xaceec3da848bf287l,0xc2a62182d3369d6el,0x3582dfdc92449482l,
        0x2f7e2fd2565d6cd7l },
      0 },
    /* 131 */
    { { 0xae4b92dbc3770fa7l,0x095e8d5c379043f9l,0x54f34e9d17761171l,
        0xc65be92e907702ael },
      { 0x2758a303f6fd0a40l,0xe7d822e3bcce784bl,0x7ae4f5854f9767bfl,
        0x4bff8e47d1193b3al },
      0 },
    /* 132 */
    { { 0xcd41d21f00ff1480l,0x2ab8fb7d0754db16l,0xac81d2efbbe0f3eal,
        0x3e4e4ae65772967dl },
      { 0x7e18f36d3c5303e6l,0x3bd9994b92262397l,0x9ed70e261324c3c0l,
        0x5388aefd58ec6028l },
      0 },
    /* 133 */
    { { 0xad1317eb5e5d7713l,0x09b985ee75de49dal,0x32f5bc4fc74fb261l,
        0x5cf908d14f75be0el },
      { 0x760435108e657b12l,0xbfd421a5b96ed9e6l,0x0e29f51f8970ccc2l,
        0xa698ba4060f00ce2l },
      0 },
    /* 134 */
    { { 0x73db1686ef748fecl,0xe6e755a27e9d2cf9l,0x630b6544ce265effl,
        0xb142ef8a7aebad8dl },
      { 0xad31af9f17d5770al,0x66af3b672cb3412fl,0x6bd60d1bdf3359del,
        0xd1896a9658515075l },
      0 },
    /* 135 */
    { { 0xec5957ab33c41c08l,0x87de94ac5468e2e1l,0x18816b73ac472f6cl,
        0x267b0e0b7981da39l },
      { 0x6e554e5d8e62b988l,0xd8ddc755116d21e7l,0x4610faf03d2a6f99l,
        0xb54e287aa1119393l },
      0 },
    /* 136 */
    { { 0x0a0122b5178a876bl,0x51ff96ff085104b4l,0x050b31ab14f29f76l,
        0x84abb28b5f87d4e6l },
      { 0xd5ed439f8270790al,0x2d6cb59d85e3f46bl,0x75f55c1b6c1e2212l,
        0xe5436f6717655640l },
      0 },
    /* 137 */
    { { 0x53f9025e2286e8d5l,0x353c95b4864453bel,0xd832f5bde408e3a0l,
        0x0404f68b5b9ce99el },
      { 0xcad33bdea781e8e5l,0x3cdf5018163c2f5bl,0x575769600119caa3l,
        0x3a4263df0ac1c701l },
      0 },
    /* 138 */
    { { 0xc2965ecc9aeb596dl,0x01ea03e7023c92b4l,0x4704b4b62e013961l,
        0x0ca8fd3f905ea367l },
      { 0x92523a42551b2b61l,0x1eb7a89c390fcd06l,0xe7f1d2be0392a63el,
        0x96dca2644ddb0c33l },
      0 },
    /* 139 */
    { { 0x203bb43a387510afl,0x846feaa8a9a36a01l,0xd23a57702f950378l,
        0x4363e2123aad59dcl },
      { 0xca43a1c740246a47l,0xb362b8d2e55dd24dl,0xf9b086045d8faf96l,
        0x840e115cd8bb98c4l },
      0 },
    /* 140 */
    { { 0xf12205e21023e8a7l,0xc808a8cdd8dc7a0bl,0xe292a272163a5ddfl,
        0x5e0d6abd30ded6d4l },
      { 0x07a721c27cfc0f64l,0x42eec01d0e55ed88l,0x26a7bef91d1f9db2l,
        0x7dea48f42945a25al },
      0 },
    /* 141 */
    { { 0xabdf6f1ce5060a81l,0xe79f9c72f8f95615l,0xcfd36c5406ac268bl,
        0xabc2a2beebfd16d1l },
      { 0x8ac66f91d3e2eac7l,0x6f10ba63d2dd0466l,0x6790e3770282d31bl,
        0x4ea353946c7eefc1l },
      0 },
    /* 142 */
    { { 0xed8a2f8d5266309dl,0x0a51c6c081945a3el,0xcecaf45a578c5dc1l,
        0x3a76e6891c94ffc3l },
      { 0x9aace8a47d7b0d0fl,0x963ace968f584a5fl,0x51a30c724e697fbel,
        0x8212a10a465e6464l },
      0 },
    /* 143 */
    { { 0xef7c61c3cfab8caal,0x18eb8e840e142390l,0xcd1dff677e9733cal,
        0xaa7cab71599cb164l },
      { 0x02fc9273bc837bd1l,0xc06407d0c36af5d7l,0x17621292f423da49l,
        0x40e38073fe0617c3l },
      0 },
    /* 144 */
    { { 0xf4f80824a7bf9b7cl,0x365d23203fbe30d0l,0xbfbe532097cf9ce3l,
        0xe3604700b3055526l },
      { 0x4dcb99116cc6c2c7l,0x72683708ba4cbee6l,0xdcded434637ad9ecl,
        0x6542d677a3dee15fl },
      0 },
    /* 145 */
    { { 0x3f32b6d07b6c377al,0x6cb03847903448bel,0xd6fdd3a820da8af7l,
        0xa6534aee09bb6f21l },
      { 0x30a1780d1035facfl,0x35e55a339dcb47e6l,0x6ea50fe1c447f393l,
        0xf3cb672fdc9aef22l },
      0 },
    /* 146 */
    { { 0xeb3719fe3b55fd83l,0xe0d7a46c875ddd10l,0x33ac9fa905cea784l,
        0x7cafaa2eaae870e7l },
      { 0x9b814d041d53b338l,0xe0acc0a0ef87e6c6l,0xfb93d10811672b0fl,
        0x0aab13c1b9bd522el },
      0 },
    /* 147 */
    { { 0xddcce278d2681297l,0xcb350eb1b509546al,0x2dc431737661aaf2l,
        0x4b91a602847012e9l },
      { 0xdcff109572f8ddcfl,0x08ebf61e9a911af4l,0x48f4360ac372430el,
        0x49534c5372321cabl },
      0 },
    /* 148 */
    { { 0x83df7d71f07b7e9dl,0xa478efa313cd516fl,0x78ef264b6c047ee3l,
        0xcaf46c4fd65ac5eel },
      { 0xa04d0c7792aa8266l,0xedf45466913684bbl,0x56e65168ae4b16b0l,
        0x14ce9e5704c6770fl },
      0 },
    /* 149 */
    { { 0x99445e3e965e8f91l,0xd3aca1bacb0f2492l,0xd31cc70f90c8a0a0l,
        0x1bb708a53e4c9a71l },
      { 0xd5ca9e69558bdd7al,0x734a0508018a26b1l,0xb093aa714c9cf1ecl,
        0xf9d126f2da300102l },
      0 },
    /* 150 */
    { { 0x749bca7aaff9563el,0xdd077afeb49914a0l,0xe27a0311bf5f1671l,
        0x807afcb9729ecc69l },
      { 0x7f8a9337c9b08b77l,0x86c3a785443c7e38l,0x85fafa59476fd8bal,
        0x751adcd16568cd8cl },
      0 },
    /* 151 */
    { { 0x8aea38b410715c0dl,0xd113ea718f7697f7l,0x665eab1493fbf06dl,
        0x29ec44682537743fl },
      { 0x3d94719cb50bebbcl,0x399ee5bfe4505422l,0x90cd5b3a8d2dedb1l,
        0xff9370e392a4077dl },
      0 },
    /* 152 */
    { { 0x59a2d69bc6b75b65l,0x4188f8d5266651c5l,0x28a9f33e3de9d7d2l,
        0x9776478ba2a9d01al },
      { 0x8852622d929af2c7l,0x334f5d6d4e690923l,0xce6cc7e5a89a51e9l,
        0x74a6313fac2f82fal },
      0 },
    /* 153 */
    { { 0xb2f4dfddb75f079cl,0x85b07c9518e36fbbl,0x1b6cfcf0e7cd36ddl,
        0xab75be150ff4863dl },
      { 0x81b367c0173fc9b7l,0xb90a7420d2594fd0l,0x15fdbf03c4091236l,
        0x4ebeac2e0b4459f6l },
      0 },
    /* 154 */
    { { 0xeb6c5fe75c9f2c53l,0xd25220118eae9411l,0xc8887633f95ac5d8l,
        0xdf99887b2c1baffcl },
      { 0xbb78eed2850aaecbl,0x9d49181b01d6a272l,0x978dd511b1cdbcacl,
        0x27b040a7779f4058l },
      0 },
    /* 155 */
    { { 0x90405db7f73b2eb2l,0xe0df85088e1b2118l,0x501b71525962327el,
        0xb393dd37e4cfa3f5l },
      { 0xa1230e7b3fd75165l,0xd66344c2bcd33554l,0x6c36f1be0f7b5022l,
        0x09588c12d0463419l },
      0 },
    /* 156 */
    { { 0xe086093f02601c3bl,0xfb0252f8cf5c335fl,0x955cf280894aff28l,
        0x81c879a9db9f648bl },
      { 0x040e687cc6f56c51l,0xfed471693f17618cl,0x44f88a419059353bl,
        0xfa0d48f55fc11bc4l },
      0 },
    /* 157 */
    { { 0xbc6e1c9de1608e4dl,0x010dda113582822cl,0xf6b7ddc1157ec2d7l,
        0x8ea0e156b6a367d6l },
      { 0xa354e02f2383b3b4l,0x69966b943f01f53cl,0x4ff6632b2de03ca5l,
        0x3f5ab924fa00b5acl },
      0 },
    /* 158 */
    { { 0x337bb0d959739efbl,0xc751b0f4e7ebec0dl,0x2da52dd6411a67d1l,
        0x8bc768872b74256el },
      { 0xa5be3b7282d3d253l,0xa9f679a1f58d779fl,0xa1cac168e16767bbl,
        0xb386f19060fcf34fl },
      0 },
    /* 159 */
    { { 0x31f3c1352fedcfc2l,0x5396bf6262f8af0dl,0x9a02b4eae57288c2l,
        0x4cb460f71b069c4dl },
      { 0xae67b4d35b8095eal,0x92bbf8596fc07603l,0xe1475f66b614a165l,
        0x52c0d50895ef5223l },
      0 },
    /* 160 */
    { { 0x231c210e15339848l,0xe87a28e870778c8dl,0x9d1de6616956e170l,
        0x4ac3c9382bb09c0bl },
      { 0x19be05516998987dl,0x8b2376c4ae09f4d6l,0x1de0b7651a3f933dl,
        0x380d94c7e39705f4l },
      0 },
    /* 161 */
    { { 0x01a355aa81542e75l,0x96c724a1ee01b9b7l,0x6b3a2977624d7087l,
        0x2ce3e171de2637afl },
      { 0xcfefeb49f5d5bc1al,0xa655607e2777e2b5l,0x4feaac2f9513756cl,
        0x2e6cd8520b624e4dl },
      0 },
    /* 162 */
    { { 0x3685954b8c31c31dl,0x68533d005bf21a0cl,0x0bd7626e75c79ec9l,
        0xca17754742c69d54l },
      { 0xcc6edafff6d2dbb2l,0xfd0d8cbd174a9d18l,0x875e8793aa4578e8l,
        0xa976a7139cab2ce6l },
      0 },
    /* 163 */
    { { 0x0a651f1b93fb353dl,0xd75cab8b57fcfa72l,0xaa88cfa731b15281l,
        0x8720a7170a1f4999l },
      { 0x8c3e8d37693e1b90l,0xd345dc0b16f6dfc3l,0x8ea8d00ab52a8742l,
        0x9719ef29c769893cl },
      0 },
    /* 164 */
    { { 0x820eed8d58e35909l,0x9366d8dc33ddc116l,0xd7f999d06e205026l,
        0xa5072976e15704c1l },
      { 0x002a37eac4e70b2el,0x84dcf6576890aa8al,0xcd71bf18645b2a5cl,
        0x99389c9df7b77725l },
      0 },
    /* 165 */
    { { 0x238c08f27ada7a4bl,0x3abe9d03fd389366l,0x6b672e89766f512cl,
        0xa88806aa202c82e4l },
      { 0x6602044ad380184el,0xa8cb78c4126a8b85l,0x79d670c0ad844f17l,
        0x0043bffb4738dcfel },
      0 },
    /* 166 */
    { { 0x8d59b5dc36d5192el,0xacf885d34590b2afl,0x83566d0a11601781l,
        0x52f3ef01ba6c4866l },
      { 0x3986732a0edcb64dl,0x0a482c238068379fl,0x16cbe5fa7040f309l,
        0x3296bd899ef27e75l },
      0 },
    /* 167 */
    { { 0x476aba89454d81d7l,0x9eade7ef51eb9b3cl,0x619a21cd81c57986l,
        0x3b90febfaee571e9l },
      { 0x9393023e5496f7cbl,0x55be41d87fb51bc4l,0x03f1dd4899beb5cel,
        0x6e88069d9f810b18l },
      0 },
    /* 168 */
    { { 0xce37ab11b43ea1dbl,0x0a7ff1a95259d292l,0x851b02218f84f186l,
        0xa7222beadefaad13l },
      { 0xa2ac78ec2b0a9144l,0x5a024051f2fa59c5l,0x91d1eca56147ce38l,
        0xbe94d523bc2ac690l },
      0 },
    /* 169 */
    { { 0x72f4945e0b226ce7l,0xb8afd747967e8b70l,0xedea46f185a6c63el,
        0x7782defe9be8c766l },
      { 0x760d2aa43db38626l,0x460ae78776f67ad1l,0x341b86fc54499cdbl,
        0x03838567a2892e4bl },
      0 },
    /* 170 */
    { { 0x2d8daefd79ec1a0fl,0x3bbcd6fdceb39c97l,0xf5575ffc58f61a95l,
        0xdbd986c4adf7b420l },
      { 0x81aa881415f39eb7l,0x6ee2fcf5b98d976cl,0x5465475dcf2f717dl,
        0x8e24d3c46860bbd0l },
      0 },
    /* 171 */
    { { 0x749d8e549a587390l,0x12bb194f0cbec588l,0x46e07da4b25983c6l,
        0x541a99c4407bafc8l },
      { 0xdb241692624c8842l,0x6044c12ad86c05ffl,0xc59d14b44f7fcf62l,
        0xc0092c49f57d35d1l },
      0 },
    /* 172 */
    { { 0xd3cc75c3df2e61efl,0x7e8841c82e1b35cal,0xc62d30d1909f29f4l,
        0x75e406347286944dl },
      { 0xe7d41fc5bbc237d0l,0xc9537bf0ec4f01c9l,0x91c51a16282bd534l,
        0x5b7cb658c7848586l },
      0 },
    /* 173 */
    { { 0x964a70848a28ead1l,0x802dc508fd3b47f6l,0x9ae4bfd1767e5b39l,
        0x7ae13eba8df097a1l },
      { 0xfd216ef8eadd384el,0x0361a2d9b6b2ff06l,0x204b98784bcdb5f3l,
        0x787d8074e2a8e3fdl },
      0 },
    /* 174 */
    { { 0xc5e25d6b757fbb1cl,0xe47bddb2ca201debl,0x4a55e9a36d2233ffl,
        0x5c2228199ef28484l },
      { 0x773d4a8588315250l,0x21b21a2b827097c1l,0xab7c4ea1def5d33fl,
        0xe45d37abbaf0f2b0l },
      0 },
    /* 175 */
    { { 0xd2df1e3428511c8al,0xebb229c8bdca6cd3l,0x578a71a7627c39a7l,
        0xed7bc12284dfb9d3l },
      { 0xcf22a6df93dea561l,0x5443f18dd48f0ed1l,0xd8b861405bad23e8l,
        0xaac97cc945ca6d27l },
      0 },
    /* 176 */
    { { 0xeb54ea74a16bd00al,0xd839e9adf5c0bcc1l,0x092bb7f11f9bfc06l,
        0x318f97b31163dc4el },
      { 0xecc0c5bec30d7138l,0x44e8df23abc30220l,0x2bb7972fb0223606l,
        0xfa41faa19a84ff4dl },
      0 },
    /* 177 */
    { { 0x4402d974a6642269l,0xc81814ce9bb783bdl,0x398d38e47941e60bl,
        0x38bb6b2c1d26e9e2l },
      { 0xc64e4a256a577f87l,0x8b52d253dc11fe1cl,0xff336abf62280728l,
        0x94dd0905ce7601a5l },
      0 },
    /* 178 */
    { { 0x156cf7dcde93f92al,0xa01333cb89b5f315l,0x02404df9c995e750l,
        0x92077867d25c2ae9l },
      { 0xe2471e010bf39d44l,0x5f2c902096bb53d7l,0x4c44b7b35c9c3d8fl,
        0x81e8428bd29beb51l },
      0 },
    /* 179 */
    { { 0x6dd9c2bac477199fl,0x8cb8eeee6b5ecdd9l,0x8af7db3fee40fd0el,
        0x1b94ab62dbbfa4b1l },
      { 0x44f0d8b3ce47f143l,0x51e623fc63f46163l,0xf18f270fcc599383l,
        0x06a38e28055590eel },
      0 },
    /* 180 */
    { { 0x2e5b0139b3355b49l,0x20e26560b4ebf99bl,0xc08ffa6bd269f3dcl,
        0xa7b36c2083d9d4f8l },
      { 0x64d15c3a1b3e8830l,0xd5fceae1a89f9c0bl,0xcfeee4a2e2d16930l,
        0xbe54c6b4a2822a20l },
      0 },
    /* 181 */
    { { 0xd6cdb3df8d91167cl,0x517c3f79e7a6625el,0x7105648f346ac7f4l,
        0xbf30a5abeae022bbl },
      { 0x8e7785be93828a68l,0x5161c3327f3ef036l,0xe11b5feb592146b2l,
        0xd1c820de2732d13al },
      0 },
    /* 182 */
    { { 0x043e13479038b363l,0x58c11f546b05e519l,0x4fe57abe6026cad1l,
        0xb7d17bed68a18da3l },
      { 0x44ca5891e29c2559l,0x4f7a03765bfffd84l,0x498de4af74e46948l,
        0x3997fd5e6412cc64l },
      0 },
    /* 183 */
    { { 0xf20746828bd61507l,0x29e132d534a64d2al,0xffeddfb08a8a15e3l,
        0x0eeb89293c6c13e8l },
      { 0xe9b69a3ea7e259f8l,0xce1db7e6d13e7e67l,0x277318f6ad1fa685l,
        0x228916f8c922b6efl },
      0 },
    /* 184 */
    { { 0x959ae25b0a12ab5bl,0xcc11171f957bc136l,0x8058429ed16e2b0cl,
        0xec05ad1d6e93097el },
      { 0x157ba5beac3f3708l,0x31baf93530b59d77l,0x47b55237118234e5l,
        0x7d3141567ff11b37l },
      0 },
    /* 185 */
    { { 0x7bd9c05cf6dfefabl,0xbe2f2268dcb37707l,0xe53ead973a38bb95l,
        0xe9ce66fc9bc1d7a3l },
      { 0x75aa15766f6a02a1l,0x38c087df60e600edl,0xf8947f3468cdc1b9l,
        0xd9650b0172280651l },
      0 },
    /* 186 */
    { { 0x504b4c4a5a057e60l,0xcbccc3be8def25e4l,0xa635320817c1ccbdl,
        0x14d6699a804eb7a2l },
      { 0x2c8a8415db1f411al,0x09fbaf0bf80d769cl,0xb4deef901c2f77adl,
        0x6f4c68410d43598al },
      0 },
    /* 187 */
    { { 0x8726df4e96c24a96l,0x534dbc85fcbd99a3l,0x3c466ef28b2ae30al,
        0x4c4350fd61189abbl },
      { 0x2967f716f855b8dal,0x41a42394463c38a1l,0xc37e1413eae93343l,
        0xa726d2425a3118b5l },
      0 },
    /* 188 */
    { { 0xdae6b3ee948c1086l,0xf1de503dcbd3a2e1l,0x3f35ed3f03d022f3l,
        0x13639e82cc6cf392l },
      { 0x9ac938fbcdafaa86l,0xf45bc5fb2654a258l,0x1963b26e45051329l,
        0xca9365e1c1a335a3l },
      0 },
    /* 189 */
    { { 0x3615ac754c3b2d20l,0x742a5417904e241bl,0xb08521c4cc9d071dl,
        0x9ce29c34970b72a5l },
      { 0x8cc81f736d3e0ad6l,0x8060da9ef2f8434cl,0x35ed1d1a6ce862d9l,
        0x48c4abd7ab42af98l },
      0 },
    /* 190 */
    { { 0xd221b0cc40c7485al,0xead455bbe5274dbfl,0x493c76989263d2e8l,
        0x78017c32f67b33cbl },
      { 0xb9d35769930cb5eel,0xc0d14e940c408ed2l,0xf8b7bf55272f1a4dl,
        0x53cd0454de5c1c04l },
      0 },
    /* 191 */
    { { 0xbcd585fa5d28ccacl,0x5f823e56005b746el,0x7c79f0a1cd0123aal,
        0xeea465c1d3d7fa8fl },
      { 0x7810659f0551803bl,0x6c0b599f7ce6af70l,0x4195a77029288e70l,
        0x1b6e42a47ae69193l },
      0 },
    /* 192 */
    { { 0x2e80937cf67d04c3l,0x1e312be289eeb811l,0x56b5d88792594d60l,
        0x0224da14187fbd3dl },
      { 0x87abb8630c5fe36fl,0x580f3c604ef51f5fl,0x964fb1bfb3b429ecl,
        0x60838ef042bfff33l },
      0 },
    /* 193 */
    { { 0x432cb2f27e0bbe99l,0x7bda44f304aa39eel,0x5f497c7a9fa93903l,
        0x636eb2022d331643l },
      { 0xfcfd0e6193ae00aal,0x875a00fe31ae6d2fl,0xf43658a29f93901cl,
        0x8844eeb639218bacl },
      0 },
    /* 194 */
    { { 0x114171d26b3bae58l,0x7db3df7117e39f3el,0xcd37bc7f81a8eadal,
        0x27ba83dc51fb789el },
      { 0xa7df439ffbf54de5l,0x7277030bb5fe1a71l,0x42ee8e35db297a48l,
        0xadb62d3487f3a4abl },
      0 },
    /* 195 */
    { { 0x9b1168a2a175df2al,0x082aa04f618c32e9l,0xc9e4f2e7146b0916l,
        0xb990fd7675e7c8b2l },
      { 0x0829d96b4df37313l,0x1c205579d0b40789l,0x66c9ae4a78087711l,
        0x81707ef94d10d18dl },
      0 },
    /* 196 */
    { { 0x97d7cab203d6ff96l,0x5b851bfc0d843360l,0x268823c4d042db4bl,
        0x3792daead5a8aa5cl },
      { 0x52818865941afa0bl,0xf3e9e74142d83671l,0x17c825275be4e0a7l,
        0x5abd635e94b001bal },
      0 },
    /* 197 */
    { { 0x727fa84e0ac4927cl,0xe3886035a7c8cf23l,0xa4bcd5ea4adca0dfl,
        0x5995bf21846ab610l },
      { 0xe90f860b829dfa33l,0xcaafe2ae958fc18bl,0x9b3baf4478630366l,
        0x44c32ca2d483411el },
      0 },
    /* 198 */
    { { 0xa74a97f1e40ed80cl,0x5f938cb131d2ca82l,0x53f2124b7c2d6ad9l,
        0x1f2162fb8082a54cl },
      { 0x7e467cc5720b173el,0x40e8a666085f12f9l,0x8cebc20e4c9d65dcl,
        0x8f1d402bc3e907c9l },
      0 },
    /* 199 */
    { { 0x4f592f9cfbc4058al,0xb15e14b6292f5670l,0xc55cfe37bc1d8c57l,
        0xb1980f43926edbf9l },
      { 0x98c33e0932c76b09l,0x1df5279d33b07f78l,0x6f08ead4863bb461l,
        0x2828ad9b37448e45l },
      0 },
    /* 200 */
    { { 0x696722c4c4cf4ac5l,0xf5ac1a3fdde64afbl,0x0551baa2e0890832l,
        0x4973f1275a14b390l },
      { 0xe59d8335322eac5dl,0x5e07eef50bd9b568l,0xab36720fa2588393l,
        0x6dac8ed0db168ac7l },
      0 },
    /* 201 */
    { { 0xf7b545aeeda835efl,0x4aa113d21d10ed51l,0x035a65e013741b09l,
        0x4b23ef5920b9de4cl },
      { 0xe82bb6803c4c7341l,0xd457706d3f58bc37l,0x73527863a51e3ee8l,
        0x4dd71534ddf49a4el },
      0 },
    /* 202 */
    { { 0xbf94467295476cd9l,0x648d072fe31a725bl,0x1441c8b8fc4b67e0l,
        0xfd3170002f4a4dbbl },
      { 0x1cb43ff48995d0e1l,0x76e695d10ef729aal,0xe0d5f97641798982l,
        0x14fac58c9569f365l },
      0 },
    /* 203 */
    { { 0xad9a0065f312ae18l,0x51958dc0fcc93fc9l,0xd9a142408a7d2846l,
        0xed7c765136abda50l },
      { 0x46270f1a25d4abbcl,0x9b5dd8f3f1a113eal,0xc609b0755b51952fl,
        0xfefcb7f74d2e9f53l },
      0 },
    /* 204 */
    { { 0xbd09497aba119185l,0xd54e8c30aac45ba4l,0x492479deaa521179l,
        0x1801a57e87e0d80bl },
      { 0x073d3f8dfcafffb0l,0x6cf33c0bae255240l,0x781d763b5b5fdfbcl,
        0x9f8fc11e1ead1064l },
      0 },
    /* 205 */
    { { 0x1583a1715e69544cl,0x0eaf8567f04b7813l,0x1e22a8fd278a4c32l,
        0xa9d3809d3d3a69a9l },
      { 0x936c2c2c59a2da3bl,0x38ccbcf61895c847l,0x5e65244e63d50869l,
        0x3006b9aee1178ef7l },
      0 },
    /* 206 */
    { { 0x0bb1f2b0c9eead28l,0x7eef635d89f4dfbcl,0x074757fdb2ce8939l,
        0x0ab85fd745f8f761l },
      { 0xecda7c933e5b4549l,0x4be2bb5c97922f21l,0x261a1274b43b8040l,
        0xb122d67511e942c2l },
      0 },
    /* 207 */
    { { 0x3be607be66a5ae7al,0x01e703fa76adcbe3l,0xaf9043014eb6e5c5l,
        0x9f599dc1097dbaecl },
      { 0x6d75b7180ff250edl,0x8eb91574349a20dcl,0x425605a410b227a3l,
        0x7d5528e08a294b78l },
      0 },
    /* 208 */
    { { 0xf0f58f6620c26defl,0x025585ea582b2d1el,0xfbe7d79b01ce3881l,
        0x28ccea01303f1730l },
      { 0xd1dabcd179644ba5l,0x1fc643e806fff0b8l,0xa60a76fc66b3e17bl,
        0xc18baf48a1d013bfl },
      0 },
    /* 209 */
    { { 0x34e638c85dc4216dl,0x00c01067206142acl,0xd453a17195f5064al,
        0x9def809db7a9596bl },
      { 0x41e8642e67ab8d2cl,0xb42404336237a2b6l,0x7d506a6d64c4218bl,
        0x0357f8b068808ce5l },
      0 },
    /* 210 */
    { { 0x8e9dbe644cd2cc88l,0xcc61c28df0b8f39dl,0x4a309874cd30a0c8l,
        0xe4a01add1b489887l },
      { 0x2ed1eeacf57cd8f9l,0x1b767d3ebd594c48l,0xa7295c717bd2f787l,
        0x466d7d79ce10cc30l },
      0 },
    /* 211 */
    { { 0x47d318929dada2c7l,0x4fa0a6c38f9aa27dl,0x90e4fd28820a59e1l,
        0xc672a522451ead1al },
      { 0x30607cc85d86b655l,0xf0235d3bf9ad4af1l,0x99a08680571172a6l,
        0x5e3d64faf2a67513l },
      0 },
    /* 212 */
    { { 0xaa6410c79b3b4416l,0xcd8fcf85eab26d99l,0x5ebff74adb656a74l,
        0x6c8a7a95eb8e42fcl },
      { 0x10c60ba7b02a63bdl,0x6b2f23038b8f0047l,0x8c6c3738312d90b0l,
        0x348ae422ad82ca91l },
      0 },
    /* 213 */
    { { 0x7f4746635ccda2fbl,0x22accaa18e0726d2l,0x85adf782492b1f20l,
        0xc1074de0d9ef2d2el },
      { 0xfcf3ce44ae9a65b3l,0xfd71e4ac05d7151bl,0xd4711f50ce6a9788l,
        0xfbadfbdbc9e54ffcl },
      0 },
    /* 214 */
    { { 0x1713f1cd20a99363l,0xb915658f6cf22775l,0x968175cd24d359b2l,
        0xb7f976b483716fcdl },
      { 0x5758e24d5d6dbf74l,0x8d23bafd71c3af36l,0x48f477600243dfe3l,
        0xf4d41b2ecafcc805l },
      0 },
    /* 215 */
    { { 0x51f1cf28fdabd48dl,0xce81be3632c078a4l,0x6ace2974117146e9l,
        0x180824eae0160f10l },
      { 0x0387698b66e58358l,0x63568752ce6ca358l,0x82380e345e41e6c5l,
        0x67e5f63983cf6d25l },
      0 },
    /* 216 */
    { { 0xf89ccb8dcf4899efl,0x949015f09ebb44c0l,0x546f9276b2598ec9l,
        0x9fef789a04c11fc6l },
      { 0x6d367ecf53d2a071l,0xb10e1a7fa4519b09l,0xca6b3fb0611e2eefl,
        0xbc80c181a99c4e20l },
      0 },
    /* 217 */
    { { 0x972536f8e5eb82e6l,0x1a484fc7f56cb920l,0xc78e217150b5da5el,
        0x49270e629f8cdf10l },
      { 0x1a39b7bbea6b50adl,0x9a0284c1a2388ffcl,0x5403eb178107197bl,
        0xd2ee52f961372f7fl },
      0 },
    /* 218 */
    { { 0xd37cd28588e0362al,0x442fa8a78fa5d94dl,0xaff836e5a434a526l,
        0xdfb478bee5abb733l },
      { 0xa91f1ce7673eede6l,0xa5390ad42b5b2f04l,0x5e66f7bf5530da2fl,
        0xd9a140b408df473al },
      0 },
    /* 219 */
    { { 0x0e0221b56e8ea498l,0x623478293563ee09l,0xe06b8391335d2adel,
        0x760c058d623f4b1al },
      { 0x0b89b58cc198aa79l,0xf74890d2f07aba7fl,0x4e204110fde2556al,
        0x7141982d8f190409l },
      0 },
    /* 220 */
    { { 0x6f0a0e334d4b0f45l,0xd9280b38392a94e1l,0x3af324c6b3c61d5el,
        0x3af9d1ce89d54e47l },
      { 0xfd8f798120930371l,0xeda2664c21c17097l,0x0e9545dcdc42309bl,
        0xb1f815c373957dd6l },
      0 },
    /* 221 */
    { { 0x84faa78e89fec44al,0xc8c2ae473caa4cafl,0x691c807dc1b6a624l,
        0xa41aed141543f052l },
      { 0x424353997d5ffe04l,0x8bacb2df625b6e20l,0x85d660be87817775l,
        0xd6e9c1dd86fb60efl },
      0 },
    /* 222 */
    { { 0x3aa2e97ec6853264l,0x771533b7e2304a0bl,0x1b912bb7b8eae9bel,
        0x9c9c6e10ae9bf8c2l },
      { 0xa2309a59e030b74cl,0x4ed7494d6a631e90l,0x89f44b23a49b79f2l,
        0x566bd59640fa61b6l },
      0 },
    /* 223 */
    { { 0x066c0118c18061f3l,0x190b25d37c83fc70l,0xf05fc8e027273245l,
        0xcf2c7390f525345el },
      { 0xa09bceb410eb30cfl,0xcfd2ebba0d77703al,0xe842c43a150ff255l,
        0x02f517558aa20979l },
      0 },
    /* 224 */
    { { 0x396ef794addb7d07l,0x0b4fc74224455500l,0xfaff8eacc78aa3cel,
        0x14e9ada5e8d4d97dl },
      { 0xdaa480a12f7079e2l,0x45baa3cde4b0800el,0x01765e2d7838157dl,
        0xa0ad4fab8e9d9ae8l },
      0 },
    /* 225 */
    { { 0x0bfb76214a653618l,0x1872813c31eaaa5fl,0x1553e73744949d5el,
        0xbcd530b86e56ed1el },
      { 0x169be85332e9c47bl,0xdc2776feb50059abl,0xcdba9761192bfbb4l,
        0x909283cf6979341dl },
      0 },
    /* 226 */
    { { 0x67b0032476e81a13l,0x9bee1a9962171239l,0x08ed361bd32e19d6l,
        0x35eeb7c9ace1549al },
      { 0x1280ae5a7e4e5bdcl,0x2dcd2cd3b6ceec6el,0x52e4224c6e266bc1l,
        0x9a8b2cf4448ae864l },
      0 },
    /* 227 */
    { { 0xf6471bf209d03b59l,0xc90e62a3b65af2abl,0xff7ff168ebd5eec9l,
        0x6bdb60f4d4491379l },
      { 0xdadafebc8a55bc30l,0xc79ead1610097fe0l,0x42e197414c1e3bddl,
        0x01ec3cfd94ba08a9l },
      0 },
    /* 228 */
    { { 0xba6277ebdc9485c2l,0x48cc9a7922fb10c7l,0x4f61d60f70a28d8al,
        0xd1acb1c0475464f6l },
      { 0xd26902b126f36612l,0x59c3a44ee0618d8bl,0x4df8a813308357eel,
        0x7dcd079d405626c2l },
      0 },
    /* 229 */
    { { 0x5ce7d4d3f05a4b48l,0xadcd295237230772l,0xd18f7971812a915al,
        0x0bf53589377d19b8l },
      { 0x35ecd95a6c68ea73l,0xc7f3bbca823a584dl,0x9fb674c6f473a723l,
        0xd28be4d9e16686fcl },
      0 },
    /* 230 */
    { { 0x5d2b990638fa8e4bl,0x559f186e893fd8fcl,0x3a6de2aa436fb6fcl,
        0xd76007aa510f88cel },
      { 0x2d10aab6523a4988l,0xb455cf4474dd0273l,0x7f467082a3407278l,
        0xf2b52f68b303bb01l },
      0 },
    /* 231 */
    { { 0x0d57eafa9835b4cal,0x2d2232fcbb669cbcl,0x8eeeb680c6643198l,
        0xd8dbe98ecc5aed3al },
      { 0xcba9be3fc5a02709l,0x30be68e5f5ba1fa8l,0xfebd43cdf10ea852l,
        0xe01593a3ee559705l },
      0 },
    /* 232 */
    { { 0xd3e5af50ea75a0a6l,0x512226ac57858033l,0x6fe6d50fd0176406l,
        0xafec07b1aeb8ef06l },
      { 0x7fb9956780bb0a31l,0x6f1af3cc37309aael,0x9153a15a01abf389l,
        0xa71b93546e2dbfddl },
      0 },
    /* 233 */
    { { 0xbf8e12e018f593d2l,0xd1a90428a078122bl,0x150505db0ba4f2adl,
        0x53a2005c628523d9l },
      { 0x07c8b639e7f2b935l,0x2bff975ac182961al,0x86bceea77518ca2cl,
        0xbf47d19b3d588e3dl },
      0 },
    /* 234 */
    { { 0x672967a7dd7665d5l,0x4e3030572f2f4de5l,0x144005ae80d4903fl,
        0x001c2c7f39c9a1b6l },
      { 0x143a801469efc6d6l,0xc810bdaa7bc7a724l,0x5f65670ba78150a4l,
        0xfdadf8e786ffb99bl },
      0 },
    /* 235 */
    { { 0xfd38cb88ffc00785l,0x77fa75913b48eb67l,0x0454d055bf368fbcl,
        0x3a838e4d5aa43c94l },
      { 0x561663293e97bb9al,0x9eb93363441d94d9l,0x515591a60adb2a83l,
        0x3cdb8257873e1da3l },
      0 },
    /* 236 */
    { { 0x137140a97de77eabl,0xf7e1c50d41648109l,0x762dcad2ceb1d0dfl,
        0x5a60cc89f1f57fbal },
      { 0x80b3638240d45673l,0x1b82be195913c655l,0x057284b8dd64b741l,
        0x922ff56fdbfd8fc0l },
      0 },
    /* 237 */
    { { 0x1b265deec9a129a1l,0xa5b1ce57cc284e04l,0x04380c46cebfbe3cl,
        0x72919a7df6c5cd62l },
      { 0x298f453a8fb90f9al,0xd719c00b88e4031bl,0xe32c0e77796f1856l,
        0x5e7917803624089al },
      0 },
    /* 238 */
    { { 0x5c16ec557f63cdfbl,0x8e6a3571f1cae4fdl,0xfce26bea560597cal,
        0x4e0a5371e24c2fabl },
      { 0x276a40d3a5765357l,0x3c89af440d73a2b4l,0xb8f370ae41d11a32l,
        0xf5ff7818d56604eel },
      0 },
    /* 239 */
    { { 0xfbf3e3fe1a09df21l,0x26d5d28ee66e8e47l,0x2096bd0a29c89015l,
        0xe41df0e9533f5e64l },
      { 0x305fda40b3ba9e3fl,0xf2340ceb2604d895l,0x0866e1927f0367c7l,
        0x8edd7d6eac4f155fl },
      0 },
    /* 240 */
    { { 0xc9a1dc0e0bfc8ff3l,0x14efd82be936f42fl,0x67016f7ccca381efl,
        0x1432c1caed8aee96l },
      { 0xec68482970b23c26l,0xa64fe8730735b273l,0xe389f6e5eaef0f5al,
        0xcaef480b5ac8d2c6l },
      0 },
    /* 241 */
    { { 0x5245c97875315922l,0xd82951713063cca5l,0xf3ce60d0b64ef2cbl,
        0xd0ba177e8efae236l },
      { 0x53a9ae8fb1b3af60l,0x1a796ae53d2da20el,0x01d63605df9eef28l,
        0xf31c957c1c54ae16l },
      0 },
    /* 242 */
    { { 0xc0f58d5249cc4597l,0xdc5015b0bae0a028l,0xefc5fc55734a814al,
        0x013404cb96e17c3al },
      { 0xb29e2585c9a824bfl,0xd593185e001eaed7l,0x8d6ee68261ef68acl,
        0x6f377c4b91933e6cl },
      0 },
    /* 243 */
    { { 0x9f93bad1a8333fd2l,0xa89302025a2a95b8l,0x211e5037eaf75acel,
        0x6dba3e4ed2d09506l },
      { 0xa48ef98cd04399cdl,0x1811c66ee6b73adel,0x72f60752c17ecaf3l,
        0xf13cf3423becf4a7l },
      0 },
    /* 244 */
    { { 0xceeb9ec0a919e2ebl,0x83a9a195f62c0f68l,0xcfba3bb67aba2299l,
        0xc83fa9a9274bbad3l },
      { 0x0d7d1b0b62fa1ce0l,0xe58b60f53418efbfl,0xbfa8ef9e52706f04l,
        0xb49d70f45d702683l },
      0 },
    /* 245 */
    { { 0x914c7510fad5513bl,0x05f32eecb1751e2dl,0x6d850418d9fb9d59l,
        0x59cfadbb0c30f1cfl },
      { 0xe167ac2355cb7fd6l,0x249367b8820426a3l,0xeaeec58c90a78864l,
        0x5babf362354a4b67l },
      0 },
    /* 246 */
    { { 0x37c981d1ee424865l,0x8b002878f2e5577fl,0x702970f1b9e0c058l,
        0x6188c6a79026c8f0l },
      { 0x06f9a19bd0f244dal,0x1ecced5cfb080873l,0x35470f9b9f213637l,
        0x993fe475df50b9d9l },
      0 },
    /* 247 */
    { { 0x68e31cdf9b2c3609l,0x84eb19c02c46d4eal,0x7ac9ec1a9a775101l,
        0x81f764664c80616bl },
      { 0x1d7c2a5a75fbe978l,0x6743fed3f183b356l,0x838d1f04501dd2bfl,
        0x564a812a5fe9060dl },
      0 },
    /* 248 */
    { { 0x7a5a64f4fa817d1dl,0x55f96844bea82e0fl,0xb5ff5a0fcd57f9aal,
        0x226bf3cf00e51d6cl },
      { 0xd6d1a9f92f2833cfl,0x20a0a35a4f4f89a8l,0x11536c498f3f7f77l,
        0x68779f47ff257836l },
      0 },
    /* 249 */
    { { 0x79b0c1c173043d08l,0xa54467741fc020fal,0xd3767e289a6d26d0l,
        0x97bcb0d1eb092e0bl },
      { 0x2ab6eaa8f32ed3c3l,0xc8a4f151b281bc48l,0x4d1bf4f3bfa178f3l,
        0xa872ffe80a784655l },
      0 },
    /* 250 */
    { { 0xb1ab7935a32b2086l,0xe1eb710e8160f486l,0x9bd0cd913b6ae6bel,
        0x02812bfcb732a36al },
      { 0xa63fd7cacf605318l,0x646e5d50fdfd6d1dl,0xa1d683982102d619l,
        0x07391cc9fe5396afl },
      0 },
    /* 251 */
    { { 0xc50157f08b80d02bl,0x6b8333d162877f7fl,0x7aca1af878d542ael,
        0x355d2adc7e6d2a08l },
      { 0xb41f335a287386e1l,0xfd272a94f8e43275l,0x286ca2cde79989eal,
        0x3dc2b1e37c2a3a79l },
      0 },
    /* 252 */
    { { 0xd689d21c04581352l,0x0a00c825376782bel,0x203bd5909fed701fl,
        0xc47869103ccd846bl },
      { 0x5dba770824c768edl,0x72feea026841f657l,0x73313ed56accce0el,
        0xccc42968d5bb4d32l },
      0 },
    /* 253 */
    { { 0x94e50de13d7620b9l,0xd89a5c8a5992a56al,0xdc007640675487c9l,
        0xe147eb42aa4871cfl },
      { 0x274ab4eeacf3ae46l,0xfd4936fb50350fbel,0xdf2afe4748c840eal,
        0x239ac047080e96e3l },
      0 },
    /* 254 */
    { { 0x481d1f352bfee8d4l,0xce80b5cffa7b0fecl,0x105c4c9e2ce9af3cl,
        0xc55fa1a3f5f7e59dl },
      { 0x3186f14e8257c227l,0xc5b1653f342be00bl,0x09afc998aa904fb2l,
        0x094cd99cd4f4b699l },
      0 },
    /* 255 */
    { { 0x8a981c84d703bebal,0x8631d15032ceb291l,0xa445f2c9e3bd49ecl,
        0xb90a30b642abad33l },
      { 0xb465404fb4a5abf9l,0x004750c375db7603l,0x6f9a42ccca35d89fl,
        0x019f8b9a1b7924f7l },
      0 },
};

/* Multiply the base point of P256 by the scalar and return the result.
 * If map is true then convert result to affine co-ordinates.
 *
 * r     Resulting point.
 * k     Scalar to multiply by.
 * map   Indicates whether to convert result to affine.
 * heap  Heap to use for allocation.
 * returns MEMORY_E when memory allocation fails and MP_OKAY on success.
 */
static int sp_256_ecc_mulmod_base_4(sp_point* r, sp_digit* k,
        int map, void* heap)
{
    return sp_256_ecc_mulmod_stripe_4(r, &p256_base, p256_table,
                                      k, map, heap);
}

#ifdef HAVE_INTEL_AVX2
/* Multiply the base point of P256 by the scalar and return the result.
 * If map is true then convert result to affine co-ordinates.
 *
 * r     Resulting point.
 * k     Scalar to multiply by.
 * map   Indicates whether to convert result to affine.
 * heap  Heap to use for allocation.
 * returns MEMORY_E when memory allocation fails and MP_OKAY on success.
 */
static int sp_256_ecc_mulmod_base_avx2_4(sp_point* r, sp_digit* k,
        int map, void* heap)
{
    return sp_256_ecc_mulmod_stripe_avx2_4(r, &p256_base, p256_table,
                                      k, map, heap);
}

#endif /* HAVE_INTEL_AVX2 */
#else /* WOLFSSL_SP_SMALL */
/* A table entry for pre-computed points. */
typedef struct sp_table_entry_sum {
    sp_digit x[4];
    sp_digit y[4];
    byte infinity;
} sp_table_entry_sum;

/* Table of pre-computed values for P256 with 3 multiples and width of 8 bits.
 */
static sp_table_entry_sum p256_table[33][58] = {
    {
        /* 0 << 0 */
        { { 0x00, 0x00, 0x00, 0x00 },
          { 0x00, 0x00, 0x00, 0x00 },
          1 },
        /* 1 << 0 */
        { { 0x79e730d418a9143cl,0x75ba95fc5fedb601l,0x79fb732b77622510l,
            0x18905f76a53755c6l },
          { 0xddf25357ce95560al,0x8b4ab8e4ba19e45cl,0xd2e88688dd21f325l,
            0x8571ff1825885d85l },
          0 },
        /* 3 << 0 */
        { { 0xffac3f904eebc127l,0xb027f84a087d81fbl,0x66ad77dd87cbbc98l,
            0x26936a3fb6ff747el },
          { 0xb04c5c1fc983a7ebl,0x583e47ad0861fe1al,0x788208311a2ee98el,
            0xd5f06a29e587cc07l },
          0 },
        /* 4 << 0 */
        { { 0x74b0b50d46918dccl,0x4650a6edc623c173l,0x0cdaacace8100af2l,
            0x577362f541b0176bl },
          { 0x2d96f24ce4cbaba6l,0x17628471fad6f447l,0x6b6c36dee5ddd22el,
            0x84b14c394c5ab863l },
          0 },
        /* 5 << 0 */
        { { 0xbe1b8aaec45c61f5l,0x90ec649a94b9537dl,0x941cb5aad076c20cl,
            0xc9079605890523c8l },
          { 0xeb309b4ae7ba4f10l,0x73c568efe5eb882bl,0x3540a9877e7a1f68l,
            0x73a076bb2dd1e916l },
          0 },
        /* 7 << 0 */
        { { 0x0746354ea0173b4fl,0x2bd20213d23c00f7l,0xf43eaab50c23bb08l,
            0x13ba5119c3123e03l },
          { 0x2847d0303f5b9d4dl,0x6742f2f25da67bddl,0xef933bdc77c94195l,
            0xeaedd9156e240867l },
          0 },
        /* 9 << 0 */
        { { 0x75c96e8f264e20e8l,0xabe6bfed59a7a841l,0x2cc09c0444c8eb00l,
            0xe05b3080f0c4e16bl },
          { 0x1eb7777aa45f3314l,0x56af7bedce5d45e3l,0x2b6e019a88b12f1al,
            0x086659cdfd835f9bl },
          0 },
        /* 10 << 0 */
        { { 0x2c18dbd19dc21ec8l,0x98f9868a0fcf8139l,0x737d2cd648250b49l,
            0xcc61c94724b3428fl },
          { 0x0c2b407880dd9e76l,0xc43a8991383fbe08l,0x5f7d2d65779be5d2l,
            0x78719a54eb3b4ab5l },
          0 },
        /* 11 << 0 */
        { { 0xea7d260a6245e404l,0x9de407956e7fdfe0l,0x1ff3a4158dac1ab5l,
            0x3e7090f1649c9073l },
          { 0x1a7685612b944e88l,0x250f939ee57f61c8l,0x0c0daa891ead643dl,
            0x68930023e125b88el },
          0 },
        /* 13 << 0 */
        { { 0xccc425634b2ed709l,0x0e356769856fd30dl,0xbcbcd43f559e9811l,
            0x738477ac5395b759l },
          { 0x35752b90c00ee17fl,0x68748390742ed2e3l,0x7cd06422bd1f5bc1l,
            0xfbc08769c9e7b797l },
          0 },
        /* 15 << 0 */
        { { 0x72bcd8b7bc60055bl,0x03cc23ee56e27e4bl,0xee337424e4819370l,
            0xe2aa0e430ad3da09l },
          { 0x40b8524f6383c45dl,0xd766355442a41b25l,0x64efa6de778a4797l,
            0x2042170a7079adf4l },
          0 },
        /* 16 << 0 */
        { { 0x808b0b650bc6fb80l,0x5882e0753ffe2e6bl,0xd5ef2f7c2c83f549l,
            0x54d63c809103b723l },
          { 0xf2f11bd652a23f9bl,0x3670c3194b0b6587l,0x55c4623bb1580e9el,
            0x64edf7b201efe220l },
          0 },
        /* 17 << 0 */
        { { 0x97091dcbd53c5c9dl,0xf17624b6ac0a177bl,0xb0f139752cfe2dffl,
            0xc1a35c0a6c7a574el },
          { 0x227d314693e79987l,0x0575bf30e89cb80el,0x2f4e247f0d1883bbl,
            0xebd512263274c3d0l },
          0 },
        /* 19 << 0 */
        { { 0xfea912baa5659ae8l,0x68363aba25e1a16el,0xb8842277752c41acl,
            0xfe545c282897c3fcl },
          { 0x2d36e9e7dc4c696bl,0x5806244afba977c5l,0x85665e9be39508c1l,
            0xf720ee256d12597bl },
          0 },
        /* 21 << 0 */
        { { 0x562e4cecc135b208l,0x74e1b2654783f47dl,0x6d2a506c5a3f3b30l,
            0xecead9f4c16762fcl },
          { 0xf29dd4b2e286e5b9l,0x1b0fadc083bb3c61l,0x7a75023e7fac29a4l,
            0xc086d5f1c9477fa3l },
          0 },
        /* 23 << 0 */
        { { 0xf4f876532de45068l,0x37c7a7e89e2e1f6el,0xd0825fa2a3584069l,
            0xaf2cea7c1727bf42l },
          { 0x0360a4fb9e4785a9l,0xe5fda49c27299f4al,0x48068e1371ac2f71l,
            0x83d0687b9077666fl },
          0 },
        /* 25 << 0 */
        { { 0xa4a319acd837879fl,0x6fc1b49eed6b67b0l,0xe395993332f1f3afl,
            0x966742eb65432a2el },
          { 0x4b8dc9feb4966228l,0x96cc631243f43950l,0x12068859c9b731eel,
            0x7b948dc356f79968l },
          0 },
        /* 27 << 0 */
        { { 0x042c2af497e2feb4l,0xd36a42d7aebf7313l,0x49d2c9eb084ffdd7l,
            0x9f8aa54b2ef7c76al },
          { 0x9200b7ba09895e70l,0x3bd0c66fddb7fb58l,0x2d97d10878eb4cbbl,
            0x2d431068d84bde31l },
          0 },
        /* 28 << 0 */
        { { 0x4b523eb7172ccd1fl,0x7323cb2830a6a892l,0x97082ec0cfe153ebl,
            0xe97f6b6af2aadb97l },
          { 0x1d3d393ed1a83da1l,0xa6a7f9c7804b2a68l,0x4a688b482d0cb71el,
            0xa9b4cc5f40585278l },
          0 },
        /* 29 << 0 */
        { { 0x5e5db46acb66e132l,0xf1be963a0d925880l,0x944a70270317b9e2l,
            0xe266f95948603d48l },
          { 0x98db66735c208899l,0x90472447a2fb18a3l,0x8a966939777c619fl,
            0x3798142a2a3be21bl },
          0 },
        /* 31 << 0 */
        { { 0xe2f73c696755ff89l,0xdd3cf7e7473017e6l,0x8ef5689d3cf7600dl,
            0x948dc4f8b1fc87b4l },
          { 0xd9e9fe814ea53299l,0x2d921ca298eb6028l,0xfaecedfd0c9803fcl,
            0xf38ae8914d7b4745l },
          0 },
        /* 33 << 0 */
        { { 0x871514560f664534l,0x85ceae7c4b68f103l,0xac09c4ae65578ab9l,
            0x33ec6868f044b10cl },
          { 0x6ac4832b3a8ec1f1l,0x5509d1285847d5efl,0xf909604f763f1574l,
            0xb16c4303c32f63c4l },
          0 },
        /* 34 << 0 */
        { { 0xb6ab20147ca23cd3l,0xcaa7a5c6a391849dl,0x5b0673a375678d94l,
            0xc982ddd4dd303e64l },
          { 0xfd7b000b5db6f971l,0xbba2cb1f6f876f92l,0xc77332a33c569426l,
            0xa159100c570d74f8l },
          0 },
        /* 35 << 0 */
        { { 0xfd16847fdec67ef5l,0x742ee464233e76b7l,0x0b8e4134efc2b4c8l,
            0xca640b8642a3e521l },
          { 0x653a01908ceb6aa9l,0x313c300c547852d5l,0x24e4ab126b237af7l,
            0x2ba901628bb47af8l },
          0 },
        /* 36 << 0 */
        { { 0x3d5e58d6a8219bb7l,0xc691d0bd1b06c57fl,0x0ae4cb10d257576el,
            0x3569656cd54a3dc3l },
          { 0xe5ebaebd94cda03al,0x934e82d3162bfe13l,0x450ac0bae251a0c6l,
            0x480b9e11dd6da526l },
          0 },
        /* 37 << 0 */
        { { 0x00467bc58cce08b5l,0xb636458c7f178d55l,0xc5748baea677d806l,
            0x2763a387dfa394ebl },
          { 0xa12b448a7d3cebb6l,0xe7adda3e6f20d850l,0xf63ebce51558462cl,
            0x58b36143620088a8l },
          0 },
        /* 39 << 0 */
        { { 0xa9d89488a059c142l,0x6f5ae714ff0b9346l,0x068f237d16fb3664l,
            0x5853e4c4363186acl },
          { 0xe2d87d2363c52f98l,0x2ec4a76681828876l,0x47b864fae14e7b1cl,
            0x0c0bc0e569192408l },
          0 },
        /* 40 << 0 */
        { { 0xe4d7681db82e9f3el,0x83200f0bdf25e13cl,0x8909984c66f27280l,
            0x462d7b0075f73227l },
          { 0xd90ba188f2651798l,0x74c6e18c36ab1c34l,0xab256ea35ef54359l,
            0x03466612d1aa702fl },
          0 },
        /* 41 << 0 */
        { { 0x624d60492ed22e91l,0x6fdfe0b56f072822l,0xeeca111539ce2271l,
            0x98100a4fdb01614fl },
          { 0xb6b0daa2a35c628fl,0xb6f94d2ec87e9a47l,0xc67732591d57d9cel,
            0xf70bfeec03884a7bl },
          0 },
        /* 43 << 0 */
        { { 0x4ff23ffd248a7d06l,0x80c5bfb4878873fal,0xb7d9ad9005745981l,
            0x179c85db3db01994l },
          { 0xba41b06261a6966cl,0x4d82d052eadce5a8l,0x9e91cd3ba5e6a318l,
            0x47795f4f95b2dda0l },
          0 },
        /* 44 << 0 */
        { { 0xecfd7c1fd55a897cl,0x009194abb29110fbl,0x5f0e2046e381d3b0l,
            0x5f3425f6a98dd291l },
          { 0xbfa06687730d50dal,0x0423446c4b083b7fl,0x397a247dd69d3417l,
            0xeb629f90387ba42al },
          0 },
        /* 45 << 0 */
        { { 0x1ee426ccd5cd79bfl,0x0032940b946c6e18l,0x1b1e8ae057477f58l,
            0xe94f7d346d823278l },
          { 0xc747cb96782ba21al,0xc5254469f72b33a5l,0x772ef6dec7f80c81l,
            0xd73acbfe2cd9e6b5l },
          0 },
        /* 46 << 0 */
        { { 0x4075b5b149ee90d9l,0x785c339aa06e9ebal,0xa1030d5babf825e0l,
            0xcec684c3a42931dcl },
          { 0x42ab62c9c1586e63l,0x45431d665ab43f2bl,0x57c8b2c055f7835dl,
            0x033da338c1b7f865l },
          0 },
        /* 47 << 0 */
        { { 0x283c7513caa76097l,0x0a624fa936c83906l,0x6b20afec715af2c7l,
            0x4b969974eba78bfdl },
          { 0x220755ccd921d60el,0x9b944e107baeca13l,0x04819d515ded93d4l,
            0x9bbff86e6dddfd27l },
          0 },
        /* 48 << 0 */
        { { 0x6b34413077adc612l,0xa7496529bbd803a0l,0x1a1baaa76d8805bdl,
            0xc8403902470343adl },
          { 0x39f59f66175adff1l,0x0b26d7fbb7d8c5b7l,0xa875f5ce529d75e3l,
            0x85efc7e941325cc2l },
          0 },
        /* 49 << 0 */
        { { 0x21950b421ff6acd3l,0xffe7048453dc6909l,0xff4cd0b228766127l,
            0xabdbe6084fb7db2bl },
          { 0x837c92285e1109e8l,0x26147d27f4645b5al,0x4d78f592f7818ed8l,
            0xd394077ef247fa36l },
          0 },
        /* 51 << 0 */
        { { 0x508cec1c3b3f64c9l,0xe20bc0ba1e5edf3fl,0xda1deb852f4318d4l,
            0xd20ebe0d5c3fa443l },
          { 0x370b4ea773241ea3l,0x61f1511c5e1a5f65l,0x99a5e23d82681c62l,
            0xd731e383a2f54c2dl },
          0 },
        /* 52 << 0 */
        { { 0x2692f36e83445904l,0x2e0ec469af45f9c0l,0x905a3201c67528b7l,
            0x88f77f34d0e5e542l },
          { 0xf67a8d295864687cl,0x23b92eae22df3562l,0x5c27014b9bbec39el,
            0x7ef2f2269c0f0f8dl },
          0 },
        /* 53 << 0 */
        { { 0x97359638546c4d8dl,0x5f9c3fc492f24679l,0x912e8beda8c8acd9l,
            0xec3a318d306634b0l },
          { 0x80167f41c31cb264l,0x3db82f6f522113f2l,0xb155bcd2dcafe197l,
            0xfba1da5943465283l },
          0 },
        /* 55 << 0 */
        { { 0x258bbbf9e7305683l,0x31eea5bf07ef5be6l,0x0deb0e4a46c814c1l,
            0x5cee8449a7b730ddl },
          { 0xeab495c5a0182bdel,0xee759f879e27a6b4l,0xc2cf6a6880e518cal,
            0x25e8013ff14cf3f4l },
          0 },
        /* 57 << 0 */
        { { 0x3ec832e77acaca28l,0x1bfeea57c7385b29l,0x068212e3fd1eaf38l,
            0xc13298306acf8cccl },
          { 0xb909f2db2aac9e59l,0x5748060db661782al,0xc5ab2632c79b7a01l,
            0xda44c6c600017626l },
          0 },
        /* 59 << 0 */
        { { 0x69d44ed65c46aa8el,0x2100d5d3a8d063d1l,0xcb9727eaa2d17c36l,
            0x4c2bab1b8add53b7l },
          { 0xa084e90c15426704l,0x778afcd3a837ebeal,0x6651f7017ce477f8l,
            0xa062499846fb7a8bl },
          0 },
        /* 60 << 0 */
        { { 0xdc1e6828ed8a6e19l,0x33fc23364189d9c7l,0x026f8fe2671c39bcl,
            0xd40c4ccdbc6f9915l },
          { 0xafa135bbf80e75cal,0x12c651a022adff2cl,0xc40a04bd4f51ad96l,
            0x04820109bbe4e832l },
          0 },
        /* 61 << 0 */
        { { 0x3667eb1a7f4c04ccl,0x59556621a9404f84l,0x71cdf6537eceb50al,
            0x994a44a69b8335fal },
          { 0xd7faf819dbeb9b69l,0x473c5680eed4350dl,0xb6658466da44bba2l,
            0x0d1bc780872bdbf3l },
          0 },
        /* 63 << 0 */
        { { 0xb8d3d9319ff91fe5l,0x039c4800f0518eedl,0x95c376329182cb26l,
            0x0763a43482fc568dl },
          { 0x707c04d5383e76bal,0xac98b930824e8197l,0x92bf7c8f91230de0l,
            0x90876a0140959b70l },
          0 },
        /* 64 << 0 */
        { { 0xdb6d96f305968b80l,0x380a0913089f73b9l,0x7da70b83c2c61e01l,
            0x95fb8394569b38c7l },
          { 0x9a3c651280edfe2fl,0x8f726bb98faeaf82l,0x8010a4a078424bf8l,
            0x296720440e844970l },
          0 },
        /* 65 << 0 */
        { { 0xdc2306ebfcdbb2b2l,0x79527db7ba66f4b9l,0xbf639ed67765765el,
            0x01628c4706b6090al },
          { 0x66eb62f1b957b4a1l,0x33cb7691ba659f46l,0x2c90d98cf3e055d6l,
            0x7d096ac42f174750l },
          0 },
        /* 71 << 0 */
        { { 0xf19f382e92aa7864l,0x49c7cb94fc05804bl,0xf94aa89b40750d01l,
            0xdd421b5d4a210364l },
          { 0x56cd001e39df3672l,0x030a119fdd4af1ecl,0x11f947e696cd0572l,
            0x574cc7b293786791l },
          0 },
        /* 77 << 0 */
        { { 0x0a2193bfc266f85cl,0x719a87be5a0ec9cel,0x9c30c6422b2f9c49l,
            0xdb15e4963d5baeb1l },
          { 0x83c3139be0d37321l,0x4788522b2e9fdbb2l,0x2b4f0c7877eb94eal,
            0x854dc9d595105f9el },
          0 },
        /* 83 << 0 */
        { { 0x2c9ee62dc3363a22l,0x125d4714ec67199al,0xf87abebf2ab80485l,
            0xcf3086e87a243ca4l },
          { 0x5c52b051c64e09ddl,0x5e9b16125625aad7l,0x0536a39db19c6126l,
            0x97f0013247b64be5l },
          0 },
        /* 89 << 0 */
        { { 0xc1ee6264a7eabe67l,0x62d51e29fd54487dl,0x3ea123446310eb5al,
            0xbd88aca74765b805l },
          { 0xb7b284be14fb691al,0x640388f83b9fffefl,0x7ab49dd209f98f9al,
            0x7150f87e7211e445l },
          0 },
        /* 95 << 0 */
        { { 0x263e039bb308cc40l,0x6684ad762b346fd2l,0x9a127f2bcaa12d0dl,
            0x76a8f9fea974291fl },
          { 0xc802049b68aa19e4l,0x65499c990c5dbba0l,0xee1b1cb5344455a1l,
            0x3f293fda2cd6f439l },
          0 },
        /* 101 << 0 */
        { { 0xb7a96e0a4ea6fdf7l,0xbbe914d3b99cd026l,0x6a610374c569a602l,
            0xe9b1c23914da499el },
          { 0xb5f6f0feadc19a99l,0x731251826f21687cl,0x5a8a14644be77793l,
            0x94ce9e0adba8bfc7l },
          0 },
        /* 107 << 0 */
        { { 0x2ca0ba9c3796f4c7l,0x3571e4d1592ce334l,0x28f9cdebe9f6e877l,
            0xee206023efce1a70l },
          { 0xb2159e08b76369dcl,0x2754e4260a7f687cl,0xe008039e02de2ff1l,
            0xccd7e9418ea700c1l },
          0 },
        /* 113 << 0 */
        { { 0xa125e6c1b7ebcb88l,0x3289e86e10ec0d40l,0xcc3a5ecb98353869l,
            0x734e0d078a2b0d3al },
          { 0xe0d92e9a51933360l,0xfa6bcdb1786076b9l,0xd13cca90747f19ecl,
            0x61d8209d49f3a53dl },
          0 },
        /* 116 << 0 */
        { { 0x87f9793bc9826344l,0x4b3de89bb2f5f79cl,0xc9f08a5659cb1b6el,
            0xd8f1fc5f6a92b9aal },
          { 0x86357f9eb412595el,0x53c30bbe65b80f16l,0xf06c2c8c70549a57l,
            0xa9c8a4b42b9157dal },
          0 },
        /* 119 << 0 */
        { { 0x87af199e6cc47305l,0x062afb7c1e314ddel,0x2be22ba0f3a49fb4l,
            0x6ed0b988157b7f56l },
          { 0x8162cf502d653fd9l,0x17d29c64877b7497l,0xd7e814380f67b514l,
            0xfedf1014fe6ee703l },
          0 },
        /* 125 << 0 */
        { { 0xaab54cfc93740130l,0xf72dab6d225733fal,0x04b76d2d1ed32559l,
            0xa9fe2396bb85b9cbl },
          { 0x128b0d24bf2219f0l,0x2292393b579f3ce2l,0x51dc5fac145ff0d5l,
            0xb16d6af8c3febbc1l },
          0 },
    },
    {
        /* 0 << 8 */
        { { 0x00, 0x00, 0x00, 0x00 },
          { 0x00, 0x00, 0x00, 0x00 },
          1 },
        /* 1 << 8 */
        { { 0x486d8ffa696946fcl,0x50fbc6d8b9cba56dl,0x7e3d423e90f35a15l,
            0x7c3da195c0dd962cl },
          { 0xe673fdb03cfd5d8bl,0x0704b7c2889dfca5l,0xf6ce581ff52305aal,
            0x399d49eb914d5e53l },
          0 },
        /* 3 << 8 */
        { { 0x35d6a53eed4c3717l,0x9f8240cf3d0ed2a3l,0x8c0d4d05e5543aa5l,
            0x45d5bbfbdd33b4b4l },
          { 0xfa04cc73137fd28el,0x862ac6efc73b3ffdl,0x403ff9f531f51ef2l,
            0x34d5e0fcbc73f5a2l },
          0 },
        /* 4 << 8 */
        { { 0x4f7081e144cc3addl,0xd5ffa1d687be82cfl,0x89890b6c0edd6472l,
            0xada26e1a3ed17863l },
          { 0x276f271563483caal,0xe6924cd92f6077fdl,0x05a7fe980a466e3cl,
            0xf1c794b0b1902d1fl },
          0 },
        /* 5 << 8 */
        { { 0x33b2385c08369a90l,0x2990c59b190eb4f8l,0x819a6145c68eac80l,
            0x7a786d622ec4a014l },
          { 0x33faadbe20ac3a8dl,0x31a217815aba2d30l,0x209d2742dba4f565l,
            0xdb2ce9e355aa0fbbl },
          0 },
        /* 7 << 8 */
        { { 0x0c4a58d474a86108l,0xf8048a8fee4c5d90l,0xe3c7c924e86d4c80l,
            0x28c889de056a1e60l },
          { 0x57e2662eb214a040l,0xe8c48e9837e10347l,0x8774286280ac748al,
            0xf1c24022186b06f2l },
          0 },
        /* 9 << 8 */
        { { 0xe8cbf1e5d5923359l,0xdb0cea9d539b9fb0l,0x0c5b34cf49859b98l,
            0x5e583c56a4403cc6l },
          { 0x11fc1a2dd48185b7l,0xc93fbc7e6e521787l,0x47e7a05805105b8bl,
            0x7b4d4d58db8260c8l },
          0 },
        /* 10 << 8 */
        { { 0xb31bd6136339c083l,0x39ff8155dfb64701l,0x7c3388d2e29604abl,
            0x1e19084ba6b10442l },
          { 0x17cf54c0eccd47efl,0x896933854a5dfb30l,0x69d023fb47daf9f6l,
            0x9222840b7d91d959l },
          0 },
        /* 11 << 8 */
        { { 0xc510610939842194l,0xb7e2353e49d05295l,0xfc8c1d5cefb42ee0l,
            0xe04884eb08ce811cl },
          { 0xf1f75d817419f40el,0x5b0ac162a995c241l,0x120921bbc4c55646l,
            0x713520c28d33cf97l },
          0 },
        /* 13 << 8 */
        { { 0x41d04ee21726931al,0x0bbbb2c83660ecfdl,0xa6ef6de524818e18l,
            0xe421cc51e7d57887l },
          { 0xf127d208bea87be6l,0x16a475d3b1cdd682l,0x9db1b684439b63f7l,
            0x5359b3dbf0f113b6l },
          0 },
        /* 15 << 8 */
        { { 0x3a5c752edcc18770l,0x4baf1f2f8825c3a5l,0xebd63f7421b153edl,
            0xa2383e47b2f64723l },
          { 0xe7bf620a2646d19al,0x56cb44ec03c83ffdl,0xaf7267c94f6be9f1l,
            0x8b2dfd7bc06bb5e9l },
          0 },
        /* 16 << 8 */
        { { 0x6772b0e5ab4b35a2l,0x1d8b6001f5eeaacfl,0x728f7ce4795b9580l,
            0x4a20ed2a41fb81dal },
          { 0x9f685cd44fec01e6l,0x3ed7ddcca7ff50adl,0x460fd2640c2d97fdl,
            0x3a241426eb82f4f9l },
          0 },
        /* 17 << 8 */
        { { 0xc503cd33bccd9617l,0x365dede4ba7730a3l,0x798c63555ddb0786l,
            0xa6c3200efc9cd3bcl },
          { 0x060ffb2ce5e35efdl,0x99a4e25b5555a1c1l,0x11d95375f70b3751l,
            0x0a57354a160e1bf6l },
          0 },
        /* 19 << 8 */
        { { 0xc033bdc719803511l,0xa9f97b3b8888c3bel,0x3d68aebc85c6d05el,
            0xc3b88a9d193919ebl },
          { 0x2d300748c48b0ee3l,0x7506bc7c07a746c1l,0xfc48437c6e6d57f3l,
            0x5bd71587cfeaa91al },
          0 },
        /* 21 << 8 */
        { { 0xe40736d3df61bc76l,0x13a619c03f778cdbl,0x6dd921a4c56ea28fl,
            0x76a524332fa647b4l },
          { 0x23591891ac5bdc5dl,0xff4a1a72bac7dc01l,0x9905e26162df8453l,
            0x3ac045dfe63b265fl },
          0 },
        /* 23 << 8 */
        { { 0x8435bd6994b03ed1l,0xd9ad1de3634cc546l,0x2cf423fc00e420cal,
            0xeed26d80a03096ddl },
          { 0xd7f60be7a4db09d2l,0xf47f569d960622f7l,0xe5925fd77296c729l,
            0xeff2db2626ca2715l },
          0 },
        /* 25 << 8 */
        { { 0x5dfee80f83774bddl,0x6313160285734485l,0xa1b524ae914a69a9l,
            0xebc2ffafd4e300d7l },
          { 0x52c93db77cfa46a5l,0x71e6161f21653b50l,0x3574fc57a4bc580al,
            0xc09015dde1bc1253l },
          0 },
        /* 27 << 8 */
        { { 0x9c38ddcceb5b76c1l,0x746f528526fc0ab4l,0x52a63a50d62c269fl,
            0x60049c5599458621l },
          { 0xe7f48f823c2f7c9el,0x6bd99043917d5cf3l,0xeb1317a88701f469l,
            0xbd3fe2ed9a449fe0l },
          0 },
        /* 28 << 8 */
        { { 0xe652533b3cef0d7dl,0xd94f7b182bbb4381l,0x838752be0e80f500l,
            0x8e6e24889e9c9bfbl },
          { 0xc975169716caca6al,0x866c49d838531ad9l,0xc917e2397151ade1l,
            0x2d016ec16037c407l },
          0 },
        /* 29 << 8 */
        { { 0x202f6a9c31c71f7bl,0x01f95aa3296ffe5cl,0x5fc0601453cec3a3l,
            0xeb9912375f498a45l },
          { 0xae9a935e5d91ba87l,0xc6ac62810b564a19l,0x8a8fe81c3bd44e69l,
            0x7c8b467f9dd11d45l },
          0 },
        /* 31 << 8 */
        { { 0x21d3634d39eedbbal,0x35cd2e680455a46dl,0xc8cafe65f9d7eb0cl,
            0xbda3ce9e00cefb3el },
          { 0xddc17a602c9cf7a4l,0x01572ee47bcb8773l,0xa92b2b018c7548dfl,
            0x732fd309a84600e3l },
          0 },
        /* 33 << 8 */
        { { 0x65cf89a2e0600afal,0xcf51482f753c5ceal,0x4f2b2d25a5c2bfc5l,
            0x9381f57187098256l },
          { 0x89210f676e976e4bl,0xe2cf12f489f47a7bl,0xc21a1658e8484050l,
            0xa224dbf82f0fff01l },
          0 },
        /* 34 << 8 */
        { { 0xc28961087282513dl,0x9a78c4296a3f8fb8l,0xddfa56f9a31e24b7l,
            0xb1e14f84fb72611fl },
          { 0x1d0f70ab45078d65l,0xb247aef3819924d8l,0x8d519f9dbb9877c1l,
            0x495c2ece8368c7c9l },
          0 },
        /* 35 << 8 */
        { { 0xca9129a0bdb69d12l,0xbe3e319978f39adfl,0xa88506df5fe49438l,
            0x17ddb7a7aafe894cl },
          { 0x28d1456f6d1d742fl,0xeec09651917d1268l,0xdecb1c700fd5b4c0l,
            0x32d14f6acf2861dbl },
          0 },
        /* 36 << 8 */
        { { 0x903f6e3960e913afl,0xb2b58bee98bf140dl,0x9deff025354890b8l,
            0x155810068d2e924el },
          { 0xb5755db493c95e5bl,0x3fac42f0dae20eb8l,0x9377c8c109b6d8e0l,
            0xa43e2b46ab47ceffl },
          0 },
        /* 37 << 8 */
        { { 0x6c3f5a51cb61e7e7l,0x264aebc80d9c73b2l,0xc404b2114a0d9288l,
            0x5178d3cf8b3a79e9l },
          { 0x4080be5372a420d7l,0xa39396adef026429l,0x22fbb92e8dde4728l,
            0x19e42d8874d949fcl },
          0 },
        /* 39 << 8 */
        { { 0xde352d78387f5557l,0x6770149969367413l,0x255bb8c00b0cc102l,
            0x63cad1be1f4d262el },
          { 0xf34f9a8a3f8f4fb6l,0x32bc13aae03a969fl,0xb29d4336218371cdl,
            0x799d76ab285bd210l },
          0 },
        /* 40 << 8 */
        { { 0x5f57b2fbfacfa459l,0x874b1498c1b5aa6bl,0xb9e89acac4db2092l,
            0x1362bf8ddf4381dal },
          { 0x25d76830b76328a0l,0x38188b7098572ae4l,0xb43e941429132f7dl,
            0x7895a29f22dd42c9l },
          0 },
        /* 41 << 8 */
        { { 0x85bded619e808c05l,0x6e0fc2bcc7ef83bbl,0xed70e0b499bedf77l,
            0x300e777dc1aaffc0l },
          { 0xe2da2359c43e6d2cl,0xacf6d60a275226e0l,0x18ca38f7f82558bdl,
            0xd7b017d475ae2591l },
          0 },
        /* 43 << 8 */
        { { 0xed299e2d7cd92ee2l,0x2c08eb37ad847153l,0x7b372aa712acfd81l,
            0x574d27f5fabda29cl },
          { 0xbd8247f0f2ee6ebcl,0x8bf76710d06be261l,0x26e95b4bcb186d4cl,
            0x4fa3ac1d1ebb4a46l },
          0 },
        /* 44 << 8 */
        { { 0xcbde78dd5e22cbb2l,0xf449c85b76bb4391l,0x4289f357b6a4273bl,
            0x9fce23fd48e84a19l },
          { 0xcfc32730939eb3b4l,0x8b3d982c16c32280l,0x5ac234bad5f1346cl,
            0x781954b470769fc9l },
          0 },
        /* 45 << 8 */
        { { 0xff0d4d30062c7dbdl,0x2c483081e6f9fcf0l,0x22f96316d67e070fl,
            0xdd9be459c0e68c44l },
          { 0xb9c1edffce2edd4dl,0x1a54782021fc538cl,0x93849be49979aee1l,
            0x3f313629a590949el },
          0 },
        /* 46 << 8 */
        { { 0x160b836b266be332l,0x49de38215f340575l,0x782e8f6701edce66l,
            0x83ae008b5df1a93el },
          { 0x85d33a263ed9ffebl,0xae2f9f961e79db97l,0xf64f209b95ae9e34l,
            0x2b6b03455e957d49l },
          0 },
        /* 47 << 8 */
        { { 0x7a24a21a331d6bdal,0xfdba302f6328f742l,0x37a36dd47744dca4l,
            0xda2832ce6fef500fl },
          { 0x23da304a7b49d73al,0xeede2cebc6ad834fl,0xf21a81248dec3c78l,
            0x4bc9469b19b721e3l },
          0 },
        /* 48 << 8 */
        { { 0x6faf68feaae6ee70l,0x78f4cc155602b0c9l,0x7e3321a86e94052al,
            0x2fb3a0d6734d5d80l },
          { 0xf3b98f3bb25a43bal,0x30bf803119ee2951l,0x7ffee43321b0612al,
            0x12f775e42eb821d0l },
          0 },
        /* 49 << 8 */
        { { 0x31cc342913e5c1d6l,0x05deaa3cee54e334l,0x21ea2b61cd5087d8l,
            0x73a1841e70d1b8bcl },
          { 0xd44e2b41b078bf14l,0xc295732fcea2a30el,0x30cdab42954939f7l,
            0xc1b4e43a2dba0b7cl },
          0 },
        /* 51 << 8 */
        { { 0x5f33f618b6a20132l,0xc8d73e3cfbbf3022l,0xf3b9844d47ed4320l,
            0xab5868aa927f00cal },
          { 0x06cb1113077f6e1cl,0x1417b43a5c94faaal,0x7666cb90cf4cd1e9l,
            0x99e009f210900566l },
          0 },
        /* 52 << 8 */
        { { 0x4fdff805f57209b5l,0x9bd65ac3f952ac8dl,0x02a3abd3c7969a6fl,
            0x1359927ef523775fl },
          { 0xe09b463f88d2e861l,0x661d2199623287c3l,0x821e64495a70eb7al,
            0x0afbbb1dd67dc684l },
          0 },
        /* 53 << 8 */
        { { 0x2c5a2b2d55750eb2l,0x54d756c29dc28d9fl,0x798c8d113af97f71l,
            0x54e21ee21f6d1853l },
          { 0x34e0c8bceffc3f8al,0xed3cc4dda96f193fl,0x86436a84fad97110l,
            0x8530ca522c97205el },
          0 },
        /* 55 << 8 */
        { { 0x9b6c8452f7236867l,0x21cf260c777b44fdl,0x659fc99dceb00c52l,
            0xda97098e2439e8dbl },
          { 0x647efe510ed6e14fl,0x37c8ca122a6600f3l,0x53e89b0badf6f4a7l,
            0xd9fc8c716645618al },
          0 },
        /* 57 << 8 */
        { { 0x9cecfb8eee6ebd31l,0x4603994b1ff25529l,0x707bc80af4b141c4l,
            0x3a83d56c07524d3al },
          { 0x7035c746613a3020l,0x7aa766b286626a1cl,0x3af656095ac76c78l,
            0x4039c655171e47d6l },
          0 },
        /* 59 << 8 */
        { { 0x79cb147f0ce33b63l,0xa1328a622d160c61l,0xf99538f3cf7eb87el,
            0x0334d4958e2241d5l },
          { 0x3ad97e02f3e49e48l,0xdcfcc754037c3679l,0x76078ba61a8ff67cl,
            0x8054aa55c2a64964l },
          0 },
        /* 60 << 8 */
        { { 0x5852104b87453b28l,0x073e8128b387344dl,0x300e78e4817cfc08l,
            0x3a82ed4799362088l },
          { 0xe222304c88de46a4l,0x666c94fd57fadf4al,0x40b2d08ea0c8e108l,
            0x4b2955b909e050fal },
          0 },
        /* 61 << 8 */
        { { 0x656078565f814881l,0x0fc3d1ce58466117l,0x0ae377d3c6c1e68al,
            0xe3dd8d5cba566c48l },
          { 0x9404849ec4b63be6l,0x1e22b03ba5be9c92l,0x08145122a8b03e63l,
            0x71248243771fe153l },
          0 },
        /* 63 << 8 */
        { { 0xa80a0e83b41ac541l,0xa77570ea533e5f9bl,0x416a14c0216dc452l,
            0x2a8d728a19f7ee59l },
          { 0x58494c8cd6552eaal,0x4d635acd60145722l,0xa8e9b127327b1cbcl,
            0xb429a62e9f8235f0l },
          0 },
        /* 64 << 8 */
        { { 0xf8d112e76e6485b3l,0x4d3e24db771c52f8l,0x48e3ee41684a2f6dl,
            0x7161957d21d95551l },
          { 0x19631283cdb12a6cl,0xbf3fa8822e50e164l,0xf6254b633166cc73l,
            0x3aefa7aeaee8cc38l },
          0 },
        /* 65 << 8 */
        { { 0xd52d2cb746ef1c7el,0xebd4f7c4d8fb6e07l,0x16f77a48cf6dd2b4l,
            0x6e8f0431e77e4d51l },
          { 0x59d94cc4e9177bf2l,0xb58a578f7a7181a1l,0xeefbc4cde8f6d330l,
            0xa66c85560fe05490l },
          0 },
        /* 71 << 8 */
        { { 0x0e6db7a35d9649dal,0x4d2f25193be3d362l,0xcd891fd5a6b137b5l,
            0xa4b7e4ddacd377a9l },
          { 0x20ccd6f24355f258l,0x842c08673aafb413l,0xdd55db99d6873b88l,
            0x04d15f4fea5a2a55l },
          0 },
        /* 77 << 8 */
        { { 0x679cd93dfae289c2l,0x84cadd61ff92ba1bl,0x548b5a6f2cd734aal,
            0x1827507db8267082l },
          { 0xa903a6010c6d5b4cl,0xde0d96befdfb952bl,0x2fc9419c6a2e24f9l,
            0x27333e3936bb3203l },
          0 },
        /* 83 << 8 */
        { { 0x3eb7f062dde4aa6al,0x40effae07f354cc0l,0xe9a14bc2a066c05el,
            0x7817b11356afc543l },
          { 0x5f0ed1f28bdda262l,0x001e23d2e007ec13l,0x435878a59c57de6al,
            0x84d0e20895ac263cl },
          0 },
        /* 89 << 8 */
        { { 0xedf24aec97a66678l,0xd1f93cf8ccf55671l,0x4ed2ce8a9379a49dl,
            0x64991862c39b0ac9l },
          { 0xc15b24e31ff67e04l,0x4ee8fc76c3c084fel,0x262012b4f64bcd46l,
            0x3b5086732425c622l },
          0 },
        /* 95 << 8 */
        { { 0xaa3e451fe65002f7l,0xf5ff2617eb46d253l,0x918d146e572afca2l,
            0x0a9333b7e56a8553l },
          { 0x9b7e232d94127dc0l,0xcd0687d6831014e6l,0x725ce5baf08e1c71l,
            0x56e26f48cde0e4edl },
          0 },
        /* 101 << 8 */
        { { 0xae78dde8db833460l,0xaf1736fe762cb78al,0x5cd85742eae5ac60l,
            0x7b6c52fe955e981al },
          { 0x9f823e8555599f97l,0xb9ce70d21a4b46b3l,0xb6076175d7d09829l,
            0x21e77d22abf390a4l },
          0 },
        /* 107 << 8 */
        { { 0xf704f09da142ad7el,0xb60ec2e1bab9f5d2l,0x4180314681e54d0dl,
            0x0de50506309335e6l },
          { 0x4135374e05aec64fl,0xb5d31041b556808al,0x0092eb86049033a8l,
            0x5b7a2fa0bde0d737l },
          0 },
        /* 113 << 8 */
        { { 0xc0dfa6bbefb40cfal,0x86a6fe279c5037f3l,0xf153cd37f71155f4l,
            0xf16d6029767664f9l },
          { 0x7441aa54c635aa57l,0x547f82e9e8186b2el,0x330b464bfbf7c7fel,
            0xb5556770a1f6fddel },
          0 },
        /* 116 << 8 */
        { { 0xa0a9c5d1e8f9edf1l,0x9814c26b6946cea3l,0xcbb47a37d8e6a08dl,
            0x517a3d9b2cba11b1l },
          { 0x94edc73dab43c540l,0x4fd0b82a753e552cl,0x419aab8bd14ae853l,
            0x94955f9ca68abad8l },
          0 },
        /* 119 << 8 */
        { { 0x3a162e06ed169150l,0x8c9683a6ba1194a8l,0x53fead66ccc28d04l,
            0xdbb2a85bef09809al },
          { 0x58e677439d3ab018l,0xff9a2046b6e56bd0l,0xf4b8215eb28061e9l,
            0xcf16d9f7b10e358fl },
          0 },
        /* 125 << 8 */
        { { 0x265ceae9a55abe39l,0x9e3783f796a98f84l,0xb799628af0757d99l,
            0xebb5f12665472fb3l },
          { 0xd83619f52ba517d8l,0x5672105f50382bdfl,0x32c5681c4a12ee9fl,
            0x31e6f60d834a9fedl },
          0 },
    },
    {
        /* 0 << 16 */
        { { 0x00, 0x00, 0x00, 0x00 },
          { 0x00, 0x00, 0x00, 0x00 },
          1 },
        /* 1 << 16 */
        { { 0x0f0165fce3779ee3l,0xe00e7f9dbd495d9el,0x1fa4efa220284e7al,
            0x4564bade47ac6219l },
          { 0x90e6312ac4708e8el,0x4f5725fba71e9adfl,0xe95f55ae3d684b9fl,
            0x47f7ccb11e94b415l },
          0 },
        /* 3 << 16 */
        { { 0xbd9b8b1dbe7a2af3l,0xec51caa94fb74a72l,0xb9937a4b63879697l,
            0x7c9a9d20ec2687d5l },
          { 0x1773e44f6ef5f014l,0x8abcf412e90c6900l,0x387bd0228142161el,
            0x50393755fcb6ff2al },
          0 },
        /* 4 << 16 */
        { { 0xfabf770977f7195al,0x8ec86167adeb838fl,0xea1285a8bb4f012dl,
            0xd68835039a3eab3fl },
          { 0xee5d24f8309004c2l,0xa96e4b7613ffe95el,0x0cdffe12bd223ea4l,
            0x8f5c2ee5b6739a53l },
          0 },
        /* 5 << 16 */
        { { 0x3d61333959145a65l,0xcd9bc368fa406337l,0x82d11be32d8a52a0l,
            0xf6877b2797a1c590l },
          { 0x837a819bf5cbdb25l,0x2a4fd1d8de090249l,0x622a7de774990e5fl,
            0x840fa5a07945511bl },
          0 },
        /* 7 << 16 */
        { { 0x26e08c07e3533d77l,0xd7222e6a2e341c99l,0x9d60ec3d8d2dc4edl,
            0xbdfe0d8f7c476cf8l },
          { 0x1fe59ab61d056605l,0xa9ea9df686a8551fl,0x8489941e47fb8d8cl,
            0xfeb874eb4a7f1b10l },
          0 },
        /* 9 << 16 */
        { { 0x9164088d977eab40l,0x51f4c5b62760b390l,0xd238238f340dd553l,
            0x358566c3db1d31c9l },
          { 0x3a5ad69e5068f5ffl,0xf31435fcdaff6b06l,0xae549a5bd6debff0l,
            0x59e5f0b775e01331l },
          0 },
        /* 10 << 16 */
        { { 0x2cc5226138634818l,0x501814f4b44c2e0bl,0xf7e181aa54dfdba3l,
            0xcfd58ff0e759718cl },
          { 0xf90cdb14d3b507a8l,0x57bd478ec50bdad8l,0x29c197e250e5f9aal,
            0x4db6eef8e40bc855l },
          0 },
        /* 11 << 16 */
        { { 0xd5d5cdd35958cd79l,0x3580a1b51d373114l,0xa36e4c91fa935726l,
            0xa38c534def20d760l },
          { 0x7088e40a2ff5845bl,0xe5bb40bdbd78177fl,0x4f06a7a8857f9920l,
            0xe3cc3e50e968f05dl },
          0 },
        /* 13 << 16 */
        { { 0x10595b5696a71cbal,0x944938b2fdcadeb7l,0xa282da4cfccd8471l,
            0x98ec05f30d37bfe1l },
          { 0xe171ce1b0698304al,0x2d69144421bdf79bl,0xd0cd3b741b21dec1l,
            0x712ecd8b16a15f71l },
          0 },
        /* 15 << 16 */
        { { 0xe89f48c85963a46el,0x658ab875a99e61c7l,0x6e296f874b8517b4l,
            0x36c4fcdcfc1bc656l },
          { 0xde5227a1a3906defl,0x9fe95f5762418945l,0x20c91e81fdd96cdel,
            0x5adbe47eda4480del },
          0 },
        /* 16 << 16 */
        { { 0xa7a8746a584c5e20l,0x267e4ea1b9dc7035l,0x593a15cfb9548c9bl,
            0x5e6e21354bd012f3l },
          { 0xdf31cc6a8c8f936el,0x8af84d04b5c241dcl,0x63990a6f345efb86l,
            0x6fef4e61b9b962cbl },
          0 },
        /* 17 << 16 */
        { { 0xaa35809ddfe6e2a0l,0xebb4d7d4356a2222l,0x7d500a6a319f33b7l,
            0x4895a47d4ac99011l },
          { 0x300ab40bdf3812b2l,0xd0764ec88aec8b9fl,0x86b61d95e591b2a7l,
            0xc1b2a0b72ed74603l },
          0 },
        /* 19 << 16 */
        { { 0x6001bf5d3849c680l,0xd7a1a4e4c1d3faccl,0xa0f2776418c5e351l,
            0x0849c0736c29c623l },
          { 0x3317e143ac751c0cl,0x9bcb1f3eda06200bl,0x40a63a75541419b5l,
            0x8fad9c983f62c513l },
          0 },
        /* 21 << 16 */
        { { 0xacff0828d03b2242l,0x5a9375c43abb7389l,0x41b1a318d0192baal,
            0x105bd3100458e97bl },
          { 0x71582dc7ed496315l,0x8ab2884a4d4bda18l,0xb8b638b494bc5bb8l,
            0xb42ed1309500bb04l },
          0 },
        /* 23 << 16 */
        { { 0x73e04f02ad1ed952l,0x680051cadfa5bdb7l,0xbe0bef3c0c7437b9l,
            0x45d6f3a40e65e627l },
          { 0x5295e060c9436a75l,0xbe84ba78d289ba9el,0x350887fd69c09364l,
            0xf27bfd17671c64a7l },
          0 },
        /* 25 << 16 */
        { { 0xc8afbdc3adf6ffc5l,0x4a4fb35876385891l,0xc7fa86424d41453fl,
            0x19490b7672eedd06l },
          { 0xc883e45337d22d6al,0x8e6e38e4a9009f96l,0x44e2811eb1c560c6l,
            0x8a0021bf4439cfcfl },
          0 },
        /* 27 << 16 */
        { { 0xba768f8b7615a327l,0x6c8b320d7b15bbe7l,0x5d8d5bcbaaa9ca64l,
            0x19a2b99f3d13cdfdl },
          { 0x858288a26f172e10l,0x2412a4da37a00f94l,0xfc67fd2edaa7f6c6l,
            0x4aea0eadafa2a5c5l },
          0 },
        /* 28 << 16 */
        { { 0x5c80ccef6cd77b30l,0x49978299ec99b6d0l,0x6bf4485eb939d335l,
            0xc53e61ab86d7c147l },
          { 0xdd948052fb601dddl,0x34c5eb393511dd48l,0x91f5c67600e6f61cl,
            0x33f1b525b1e71f34l },
          0 },
        /* 29 << 16 */
        { { 0xb4cb4a151d2dad36l,0x709a61631e60b60dl,0x2f18f3bd932ece4fl,
            0x70f495a8e92368bel },
          { 0x6e88be2bb7aeaa6fl,0x4efebd9ae1bf1d6el,0x49925e6e44e94993l,
            0x33b7aba0ef0517dcl },
          0 },
        /* 31 << 16 */
        { { 0x69ce1f207afe6c37l,0xe1148ba984f68db5l,0x32668bdc2c594a8al,
            0x2cb60d3063ac4fb3l },
          { 0x5e6efe1dd9e036f8l,0x917cb2a27db4739fl,0x70ea601ded4e0b5el,
            0x5928f068ae7ac8a6l },
          0 },
        /* 33 << 16 */
        { { 0x9e4ad0073f2d96abl,0x51a9697f2d058c03l,0xcd5c0a7522d1e795l,
            0xaa1a121c2ac4f019l },
          { 0xa837c14c3e3631f4l,0x6a997381236a5576l,0xb305e7db2753782bl,
            0xae561b0237243afbl },
          0 },
        /* 34 << 16 */
        { { 0x20176baca787897bl,0x057b8b979a9f67d9l,0xe7d5c4f761e14e09l,
            0x8e4856901e6cd6d0l },
          { 0x3eeffbba9b925d52l,0xe651a5383046927bl,0x02326d1fe92d4352l,
            0xad2d6493d697369fl },
          0 },
        /* 35 << 16 */
        { { 0xe9de299c548c4ca5l,0x66f64ef54be3bde3l,0xcf6d39ebf2d5ebc9l,
            0x665ca727898953e1l },
          { 0x521ec435e33ac1b4l,0x8418fa7534ab2b82l,0x94d6c0c4771a3a87l,
            0x21feb6054859ee22l },
          0 },
        /* 36 << 16 */
        { { 0xde7153f8eed9dd1dl,0xba09ad1152ebcb2el,0xaa41b015e1843fb6l,
            0xf933a2abdd4ce6f0l },
          { 0x777f834313f6b83fl,0x28df7da4db113a75l,0x6d7d1b3c72a5d143l,
            0x6f789698966c6ddfl },
          0 },
        /* 37 << 16 */
        { { 0x57d11ed7a95e704el,0x7d5ac6dc380ad582l,0xb175421d5ab6e377l,
            0x4e383b0ba760dd4dl },
          { 0xde07b81a352b6cb3l,0x342abe825c2e1704l,0x90988de20dd48537l,
            0x4a7fec0544821591l },
          0 },
        /* 39 << 16 */
        { { 0xb0e4d17c90a94eb7l,0x27555067aceb0176l,0x587576e15c38c4e2l,
            0xe647d9dd445f2880l },
          { 0x00beb2f5ca502f83l,0x4e89e638c44767c7l,0xbef361da154a5757l,
            0x2dc632a2dc0675f2l },
          0 },
        /* 40 << 16 */
        { { 0xed439a33a72ba054l,0xa3170a15ead265bal,0xcf7eb903fe99a58el,
            0xcf6db0c633d80c26l },
          { 0xd031255ef613e71al,0x12ccbe5718ca255cl,0xdd21d0537808c40dl,
            0xf5488ebc3af2be6bl },
          0 },
        /* 41 << 16 */
        { { 0x589a125ac10f8157l,0x3c8a15bde1353e49l,0x7d9bbd0c22ce2dd0l,
            0xdfcd019211ac7bb1l },
          { 0x0e1d67151193c5b1l,0xd4de115ab0e8c285l,0x0b3e94c2272c29fel,
            0xea640843c8213581l },
          0 },
        /* 43 << 16 */
        { { 0x7a01aeed6aca2231l,0x8135cf2ace80abbel,0xdc1a41b2ae5fdec9l,
            0xde34ea4da0174364l },
          { 0xa5104e453cf8b845l,0x4b6fd986675ba557l,0x4bc750af29c8cb4al,
            0x8bebb266583f9391l },
          0 },
        /* 44 << 16 */
        { { 0x47110d7c1be3f9c5l,0x12b9e4485eadb4ddl,0x6e8c09870b713d41l,
            0xe1e20356733d56ael },
          { 0xe68d6bab445ea727l,0x9ef4f6eac934a1a4l,0xe0155547f8cef1c3l,
            0xdb5c3909159bdcbfl },
          0 },
        /* 45 << 16 */
        { { 0xef0449cb32fa8a37l,0x95071f5dcd246405l,0x1c56ad776c598891l,
            0x981781de0fa9cd42l },
          { 0x0f93d456d29c0500l,0x43aa7bc1483f52c4l,0xd7c8736666c8abadl,
            0x47552530ea5050efl },
          0 },
        /* 46 << 16 */
        { { 0x40dd9ca9fa9b8d3dl,0xf27b7bc056da41d9l,0x87967f4b66db8845l,
            0xf6918c9444de6bc7l },
          { 0x4d76d51135568d4dl,0x7ab18f9a40e7fa5al,0x069a44bba5bbbdc6l,
            0x19e6c04bb4c8f808l },
          0 },
        /* 47 << 16 */
        { { 0x5fd2501108b2b6c7l,0xcce85a3ec41cad21l,0x90857daffdd70387l,
            0x7a679062c63789f4l },
          { 0x9c462134ef8666e2l,0xcb7dba108c8505bdl,0x7c4a7e2fc610f2e7l,
            0x22906f65d68315f9l },
          0 },
        /* 48 << 16 */
        { { 0xf2efe23d442a8ad1l,0xc3816a7d06b9c164l,0xa9df2d8bdc0aa5e5l,
            0x191ae46f120a8e65l },
          { 0x83667f8700611c5bl,0x83171ed7ff109948l,0x33a2ecf8ca695952l,
            0xfa4a73eef48d1a13l },
          0 },
        /* 49 << 16 */
        { { 0x41dd38c1118de9a0l,0x3485cb3be2d8f6f5l,0xd4bac751b1dcc577l,
            0x2148d93fed12ea6bl },
          { 0xde3504729da8cb18l,0x6046daf89eb85925l,0xddbc357b942b1044l,
            0x248e7afe815b8b7cl },
          0 },
        /* 51 << 16 */
        { { 0xd4bb77b3acb21004l,0xe9f236cf83392035l,0xa9894c5c52133743l,
            0x4d6112749a7b054al },
          { 0xa61675ea4ba2a553l,0x59c199681da6aa78l,0x3988c36590f474del,
            0x73e751bbd001be43l },
          0 },
        /* 52 << 16 */
        { { 0x97cacf846604007dl,0x1e92b4b22d47a9f1l,0x858ae0d6374ed165l,
            0x4c973e6f307aefb8l },
          { 0x6f524a238a10eb72l,0x7b4a92a9eb2849d6l,0x3678bda42fe91eddl,
            0x56092acd7c0fc35cl },
          0 },
        /* 53 << 16 */
        { { 0x93bea99b1b9b43c4l,0x2f6af6f3e145fda2l,0x862f0607278adf0dl,
            0x647be08398456ccal },
          { 0xce79ba1487250c28l,0x1c1c4fc8efedab42l,0x966f612af90caa8dl,
            0xb1a2cf6e72c440f8l },
          0 },
        /* 55 << 16 */
        { { 0x2fca1be45b3b7dd5l,0x453c19853c211bcal,0x313cb21969a46484l,
            0x66082837414bd5dfl },
          { 0xab7a97bf2ac1cdf7l,0x45cd1792676d778fl,0x42fb6c4f6a5b560al,
            0x45747fe30b8f17e9l },
          0 },
        /* 57 << 16 */
        { { 0x38b6db6235db6218l,0xa10cdfe1bb54bacal,0x56fd4a1d610f7f6bl,
            0xc4bea78b76d183d7l },
          { 0xc0e6ca9fbf730d26l,0x1b1e271aed6cf535l,0x6fef275faadbe375l,
            0xfa2e8da903e489bal },
          0 },
        /* 59 << 16 */
        { { 0x6f79d25c7c4626ecl,0xfe27690232d55d6cl,0x3f5c5768afa19ce3l,
            0xa1373777f8834739l },
          { 0x761d67a8a4ce960al,0xb34de1ea459e656al,0x8725b0f09db6f269l,
            0x75316f250dbfe22el },
          0 },
        /* 60 << 16 */
        { { 0x091d5b631a093b40l,0xb85c1c075862f24al,0xc5d74eb53e8f85bfl,
            0xf51c7746cab22456l },
          { 0xc25cb8d9e761da89l,0x2670ec2fc0f028b5l,0x873fd30d2db9af5cl,
            0x3d0f1ea18262565el },
          0 },
        /* 61 << 16 */
        { { 0x8f9492c261c23b3cl,0xd366baeb631688a4l,0x55e759e78093bb07l,
            0xf6d0eaf47218f765l },
          { 0xb8a174ff54ca583bl,0x790f10e0b23d14cel,0xfebe7333be83cbbal,
            0xfeb6dcc5eed67536l },
          0 },
        /* 63 << 16 */
        { { 0x175b3bacce027e5bl,0xe0728a99c48252c4l,0x0be25d4507a39c7cl,
            0xcb9c2d3aba8e8c72l },
          { 0x6185a48d1abd459al,0x27207feadff9a27bl,0xfd92e8231d34393fl,
            0x738511534351d965l },
          0 },
        /* 64 << 16 */
        { { 0xfcde7cc8f43a730fl,0xe89b6f3c33ab590el,0xc823f529ad03240bl,
            0x82b79afe98bea5dbl },
          { 0x568f2856962fe5del,0x0c590adb60c591f3l,0x1fc74a144a28a858l,
            0x3b662498b3203f4cl },
          0 },
        /* 65 << 16 */
        { { 0x8ede0fcdc11682eel,0x41e3faa1b2ab5664l,0x58b2a7dc26a35ff5l,
            0x939bcd6b701b89e9l },
          { 0x55f66fd188e0838fl,0x99d1a77b4ff1f975l,0x103abbf72e060cc5l,
            0x91c77beb6bc4bdbbl },
          0 },
        /* 71 << 16 */
        { { 0xcd048abca380cc72l,0x91cab1bbd0e13662l,0x68115b18686de4cel,
            0x484724e63deccbf5l },
          { 0xf164ba54f176137el,0x5189793662ab2728l,0x6afdecf9b60a5458l,
            0xca40472d0aabafd2l },
          0 },
        /* 77 << 16 */
        { { 0x7a9439183b98d725l,0x1c1763e8ece1ea3cl,0x45c44ef639840476l,
            0x689271e69c009133l },
          { 0xa017405f56a51fe1l,0xd54cc7253e0d0970l,0x212ad075cfe09e8bl,
            0x999f21c37af7bf30l },
          0 },
        /* 83 << 16 */
        { { 0xdc2a2af12bf95f73l,0xb88b4ca76de82cbel,0xa31a21aaecb8e84el,
            0x86d19a601b74f5bel },
          { 0xc68bf64406008019l,0xe52ab50e9431c694l,0x6375463d627ab11cl,
            0xdd3eeaa03c0ef241l },
          0 },
        /* 89 << 16 */
        { { 0x608d9cb323f1caf8l,0x95069450b1700741l,0xe3132bd2bc2fa7aal,
            0xc4f363e7f64e4f06l },
          { 0xb059c4191ca888c2l,0x1004cb1f8d17bf5dl,0x6b6ba6f934ea5711l,
            0x071d94abd79b2c8al },
          0 },
        /* 95 << 16 */
        { { 0xc7ef9b42d147a39dl,0x36dd5d770a10cd5bl,0x3bf6cc77d0eea34bl,
            0x60c84591197479c7l },
          { 0xf95860ac50ba50edl,0xe1c94a8dc4cdc8fal,0x780818d685e24a23l,
            0x1950e3c0c8abbd27l },
          0 },
        /* 101 << 16 */
        { { 0x9908c694ae04778el,0x2e37a6790a0d36ffl,0x212a340f52b067bdl,
            0xec89c9fad080b914l },
          { 0x920dc2f005ab8a23l,0xecff5c78655e8984l,0x80eedd34f66211acl,
            0xa7a56366ef58d4d8l },
          0 },
        /* 107 << 16 */
        { { 0x4f95debe2bca42f0l,0xf0346307844334d2l,0x7003a60521d600aal,
            0x1eb98c6365c5248al },
          { 0x6757b3822fa202cal,0x32765d399fb12f36l,0xe851b476d7b44c9al,
            0x27cd7d1b4e0bab4cl },
          0 },
        /* 113 << 16 */
        { { 0xd0c1f7c9c43ea1a3l,0x73d944f49f42907dl,0xd113f34619352c92l,
            0x86a1ad53b149cdc1l },
          { 0x32c34e8f848d1be4l,0xba8afda7c3d9360bl,0x17e8bc32eea8bf96l,
            0x3174cae499c87febl },
          0 },
        /* 116 << 16 */
        { { 0x4b215f016671b47el,0xb67633ca4a8dae2al,0x2915120f79fd3cdbl,
            0xc1f8a06fb064e6del },
          { 0xf4d5368cc1d57420l,0x6ada51a8e18de475l,0xa0f0d47cc749d4b0l,
            0xabfa2c0074526aa5l },
          0 },
        /* 119 << 16 */
        { { 0xf752f6659e5ce44fl,0x7b97ebfa189d35ecl,0x9540cbb90fc609abl,
            0x19c1dc6999632cc8l },
          { 0x0a957700e08ca9a8l,0xb0cd0ab7a3246a4el,0xca687cfcc8d6a544l,
            0xb6281f0035f82a77l },
          0 },
        /* 125 << 16 */
        { { 0x547027012b818036l,0xf72315f729c8f14cl,0x95f1bc15230e74bel,
            0x2e7c492f1abe20d4l },
          { 0xe1ea8b1cd7e78ab1l,0xc3f6ba59043585adl,0xac404ea9477ac053l,
            0xaa6872914ec6d0e3l },
          0 },
    },
    {
        /* 0 << 24 */
        { { 0x00, 0x00, 0x00, 0x00 },
          { 0x00, 0x00, 0x00, 0x00 },
          1 },
        /* 1 << 24 */
        { { 0xd9d0c8c4868af75dl,0xd7325cff45c8c7eal,0xab471996cc81ecb0l,
            0xff5d55f3611824edl },
          { 0xbe3145411977a0eel,0x5085c4c5722038c6l,0x2d5335bff94bb495l,
            0x894ad8a6c8e2a082l },
          0 },
        /* 3 << 24 */
        { { 0xd1e059b21994ef20l,0x2a653b69638ae318l,0x70d5eb582f699010l,
            0x279739f709f5f84al },
          { 0x5da4663c8b799336l,0xfdfdf14d203c37ebl,0x32d8a9dca1dbfb2dl,
            0xab40cff077d48f9bl },
          0 },
        /* 4 << 24 */
        { { 0xf2369f0b879fbbedl,0x0ff0ae86da9d1869l,0x5251d75956766f45l,
            0x4984d8c02be8d0fcl },
          { 0x7ecc95a6d21008f0l,0x29bd54a03a1a1c49l,0xab9828c5d26c50f3l,
            0x32c0087c51d0d251l },
          0 },
        /* 5 << 24 */
        { { 0xf61790abfbaf50a5l,0xdf55e76b684e0750l,0xec516da7f176b005l,
            0x575553bb7a2dddc7l },
          { 0x37c87ca3553afa73l,0x315f3ffc4d55c251l,0xe846442aaf3e5d35l,
            0x61b911496495ff28l },
          0 },
        /* 7 << 24 */
        { { 0x4bdf3a4956f90823l,0xba0f5080741d777bl,0x091d71c3f38bf760l,
            0x9633d50f9b625b02l },
          { 0x03ecb743b8c9de61l,0xb47512545de74720l,0x9f9defc974ce1cb2l,
            0x774a4f6a00bd32efl },
          0 },
        /* 9 << 24 */
        { { 0x327bc002b0131e5bl,0x1739e6d5cb2514d9l,0xc8cbdafe55a81543l,
            0x5bb1a36ce1137243l },
          { 0x205da3c517325327l,0xc35c1a36515a057el,0xf00f64c942925f9bl,
            0xbd14633cb7d59f7al },
          0 },
        /* 10 << 24 */
        { { 0xae2ad171656e8c3al,0xc0e2a4631acd0705l,0x006f6a8aa0b6055cl,
            0xaf4513d72b65a26el },
          { 0x3f549e14d616d5bcl,0x64ee395571253b1fl,0xe8b10bc1b8ce243al,
            0xbcbeace5913a4e77l },
          0 },
        /* 11 << 24 */
        { { 0x47c1004341f37dbdl,0x96eccae36168ecf6l,0x65bde59d1ca46aa3l,
            0x38a7027ab8698ffal },
          { 0xa2b89dc86dc34437l,0x5a0a118d43a4153fl,0x9e330a861ce22fd8l,
            0x28382af6b3bbd3bcl },
          0 },
        /* 13 << 24 */
        { { 0x0b2e27c0d81e0271l,0xa67a7596117a317cl,0x17f08928a6723d99l,
            0x71a75681485310a3l },
          { 0x90465462afb66ca9l,0x185e97ccfbbe229dl,0x6a1a606addad8fc2l,
            0x2431f316b3c797cfl },
          0 },
        /* 15 << 24 */
        { { 0x4703401193529432l,0x1f106bdd30743462l,0xabfb9964cd66d8cal,
            0x934d9d5ae9bdadd5l },
          { 0x5976d815908e3d22l,0x344a362f28e057bdl,0xf92cdadc5443dfb3l,
            0x001297adf089603bl },
          0 },
        /* 16 << 24 */
        { { 0x7f99824f20151427l,0x206828b692430206l,0xaa9097d7e1112357l,
            0xacf9a2f209e414ecl },
          { 0xdbdac9da27915356l,0x7e0734b7001efee3l,0x54fab5bbd2b288e2l,
            0x4c630fc4f62dd09cl },
          0 },
        /* 17 << 24 */
        { { 0x4a2fce605044066bl,0x904a019cfa3a47f4l,0xba81ea9c0c5c0a60l,
            0xd7e4ea0d96c098bdl },
          { 0xefe700419cd50a02l,0xc0c839d42d7f048cl,0xe2daf264e09b561fl,
            0x0cbc13185034b18bl },
          0 },
        /* 19 << 24 */
        { { 0x11e5f2e388323f7al,0xe07a74c2927584cdl,0x1e774b3495613d2dl,
            0x9c9b52c52c787488l },
          { 0x3cdd3c3ebe421f08l,0x5ff7819e223e3d5fl,0xba8739b2c1da09b9l,
            0x6b7263164e8b491bl },
          0 },
        /* 21 << 24 */
        { { 0xb5afd13ca0943befl,0xd651772957abb1ccl,0x9d5a52dc9b61b5bcl,
            0x85cefaa6806e31cdl },
          { 0xab84257a720a1deal,0x6a60261bced70d35l,0xc023f94db9d6da61l,
            0x947f7eec54a0ae0el },
          0 },
        /* 23 << 24 */
        { { 0xc3b787569f83b787l,0xd6d249263694ddd7l,0x58d248945d70a02el,
            0xac16670e8c278c6al },
          { 0x71a94d58e370b6e6l,0xe4d763840253db05l,0x99b1c98814b32cfel,
            0x4e6bd870cc78cc95l },
          0 },
        /* 25 << 24 */
        { { 0xf5f7ca79c8b63614l,0xf3bfb2158af4903cl,0x2bdb9f5496d47bd3l,
            0xd6e715300e8a63bal },
          { 0x67e90a497a93bec4l,0x8613478b8c1e63eel,0xe36bd9c8f2dde561l,
            0x681486518a768689l },
          0 },
        /* 27 << 24 */
        { { 0xef617a9494aa531cl,0x9ac35e2fd6f4ad87l,0xbcd2a047122468fbl,
            0xbd7a423fef7c5ca6l },
          { 0xab58cb52064c8040l,0x93ef4ed54a644716l,0xf7d17097c32cd48dl,
            0xb249a173d17fcf42l },
          0 },
        /* 28 << 24 */
        { { 0x66fe0fffe298cdf5l,0x3f61bea47b2e51b6l,0x7d372117bad3afa4l,
            0x6521a09cef656e2fl },
          { 0xb3b8c966e8a58fe7l,0x25203a115a47ebc7l,0xfe81588d5c4be573l,
            0x6132e2f31f49a03cl },
          0 },
        /* 29 << 24 */
        { { 0xbbe5c108b7a7ecc4l,0x62a5a78ebfd22e4cl,0xb7974033df188bd2l,
            0xcf11deea4df7d1ael },
          { 0x99cc774a53ace3eal,0xe0373a71105cc1f6l,0xd751987f133d7a20l,
            0xab86ee04ae215871l },
          0 },
        /* 31 << 24 */
        { { 0x2094f9a280cd10e6l,0x045232aa7b8a0da7l,0x969a81b69c03244el,
            0x1293b4ca7e98d955l },
          { 0x1631421dd68f3ab0l,0xa0106422c3738c82l,0xc5f43845f82c4ff9l,
            0xb479acbe1aa0f58fl },
          0 },
        /* 33 << 24 */
        { { 0xf1db0267f67683cfl,0xa6b13c9e44ce009dl,0x04b4eed505884a69l,
            0xf2ff9c16d9087a0bl },
          { 0x2c53699b3e35b4a6l,0x5020c0142369afb8l,0xf83bfe0095be37f1l,
            0xd300d8c553b29d80l },
          0 },
        /* 34 << 24 */
        { { 0x16893055811cf4bbl,0x580dd1e55aeb5027l,0xcaf47fba5ae3c71cl,
            0xde79698129ebbb07l },
          { 0xbed1db33d262cdd3l,0x78315e3748c7313bl,0xfc9561f02fe1368dl,
            0xe0209698ccacacc7l },
          0 },
        /* 35 << 24 */
        { { 0xd61af89a781ece24l,0xf3b90626008f41e9l,0xd715dbf7c5693191l,
            0x8d6c05de6f299edel },
          { 0xf18d62637ca50aacl,0x7987bf5cb0dd5fdcl,0x424136bd2cfa702bl,
            0xaa7e237ded859db2l },
          0 },
        /* 36 << 24 */
        { { 0xde7169e4e5d41796l,0x6700333e33c0a380l,0xe20b95780343a994l,
            0xa745455e1fb3a1c3l },
          { 0x97e0ff88ce029a7fl,0x3b3481c976e384bcl,0x028b339dddad5951l,
            0xa1fdcdbae4b95cfcl },
          0 },
        /* 37 << 24 */
        { { 0xcc9221baed20c6adl,0xf2619a51fa9c73aal,0xfc2cff847d7f55a5l,
            0xd56c23d65f01d4dal },
          { 0x6d20f88cb3d84d5fl,0x048825f75dcc615dl,0x73634d3f85631a6el,
            0xa57a02e3ad7b2e2dl },
          0 },
        /* 39 << 24 */
        { { 0x067a8dcf08aa81ffl,0x62948258c23f3d16l,0xb61bd04316f2fe7bl,
            0xf250f769b6a766b1l },
          { 0x32df97246d0b241el,0xb736e4bb714e5f88l,0x50da15022c1d40d7l,
            0x013e0edebdd285a4l },
          0 },
        /* 40 << 24 */
        { { 0x1b92c3a0181a5d8fl,0x6429531d9adb77c7l,0x629152b53af710eel,
            0x4e3f27370bd5647el },
          { 0xfb7c392b77553c7dl,0xa930abacefe78c87l,0xf80c8cd6a05a6991l,
            0x751469b71be5f6f5l },
          0 },
        /* 41 << 24 */
        { { 0xf89f2b0b3e2f2af0l,0x52f634099eefc39al,0x505005c679906cb6l,
            0x820c2216b2de0b1el },
          { 0x96f0f2831f20ad7al,0xcd33125c718ffcb0l,0xf6130ef278f0c578l,
            0x4cda2471d0b76b95l },
          0 },
        /* 43 << 24 */
        { { 0x611dd83f39485581l,0x96c47051803e1b20l,0xefacc736830f44c7l,
            0x5588d8ce688b12bal },
          { 0x44f4edf3eee70fadl,0x1026dfd8869539f7l,0xa4c146ee8ddb0e00l,
            0x9f4f55816efb41c8l },
          0 },
        /* 44 << 24 */
        { { 0x6036ed0236cbace7l,0x5a70e4abada837ddl,0xf06918aff10b2fefl,
            0x08a8a9f69fd31590l },
          { 0x6c4a1ba6916af88dl,0x4868bc1466016037l,0x06d345af164228a9l,
            0x2c1961d19b550dd9l },
          0 },
        /* 45 << 24 */
        { { 0x8b72775c6851f0acl,0x7827242bd70f5975l,0x2de91f1e34db4a6fl,
            0x586bf3d58538f5eel },
          { 0xf0a15aed25d9a09bl,0x43018e56f74deb46l,0xc2af1ad0f50e0e67l,
            0x49cc9528b10cff6fl },
          0 },
        /* 46 << 24 */
        { { 0x05eb146c9d55c425l,0xe2b557ccbc62261fl,0x2a716301bd077089l,
            0x83a63c81e0527d02l },
          { 0x055ff7f8a0d9203bl,0x05d09f0525bf5a04l,0x2e44545fb3eb0b30l,
            0xed7c57c4d279a1adl },
          0 },
        /* 47 << 24 */
        { { 0x6928f6e45e0ebdd5l,0xd7e44ddf092d233bl,0xe7148066d1b7026fl,
            0xf645a2e53d5f25c3l },
          { 0x6eeb25ee58ff9eb4l,0x60f1fcf737f87ebfl,0x9eaaf1e5c4679c70l,
            0x4609fb13b7b7dc7el },
          0 },
        /* 48 << 24 */
        { { 0xae915f5d5fa067d1l,0x4134b57f9668960cl,0xbd3656d6a48edaacl,
            0xdac1e3e4fc1d7436l },
          { 0x674ff869d81fbb26l,0x449ed3ecb26c33d4l,0x85138705d94203e8l,
            0xccde538bbeeb6f4al },
          0 },
        /* 49 << 24 */
        { { 0x27f317af2b33987fl,0xd2d3cf5d51e59588l,0x333999bd031f27c9l,
            0x6ddfa3f22e0a3306l },
          { 0x23e0e651990041b0l,0xf028aba1585837acl,0x1c6ad72b25226f53l,
            0xf243c991d1fca64al },
          0 },
        /* 51 << 24 */
        { { 0x72b8a13272cbae1fl,0xfe0b1c4fbfdbd64al,0x98bc7876c5e76921l,
            0x51c726bfdb1f5af7l },
          { 0x97e88a842c186e8bl,0x9ed99516ed8eb7b4l,0x3e54a17dafc818ebl,
            0xfcfbf25a1e8f77d8l },
          0 },
        /* 52 << 24 */
        { { 0x7780d7d68f7d5c6el,0x6725b49a454101e6l,0xceddc26586b0770cl,
            0xc26624615666f504l },
          { 0x16b77477ce040f75l,0x13f9113c293f8b45l,0xff0cfa07e2dcc91el,
            0x1948d8bd41c202f5l },
          0 },
        /* 53 << 24 */
        { { 0x4c6ae39a1dfbe13al,0xafb1e5c46be9c200l,0x39e728d168bb08c3l,
            0xc794b905acc9166fl },
          { 0x1cb0dec2d9c7c3e4l,0xc4c3053289f14d65l,0x4af80801a6a9d609l,
            0x79d7e82de0d6ab24l },
          0 },
        /* 55 << 24 */
        { { 0xb905c6af8ad4cf6el,0x785590b0f6d1be13l,0x78f402c2a0ef76bel,
            0x739b22ea5c19a40bl },
          { 0xd4d3262553d596b6l,0x01598eb4d571666bl,0xf8dc150b8173486al,
            0xd8aa43af15e94f09l },
          0 },
        /* 57 << 24 */
        { { 0xcfa387cd984393b5l,0x1645659e21a1bf92l,0xb4ab3966dd46c7eel,
            0xcf8c296d89482623l },
          { 0x72e4d01cf976b4c0l,0x44ad07e8fa0fa5ebl,0xd6c82681b486fdd2l,
            0x2d9074f89b8845b4l },
          0 },
        /* 59 << 24 */
        { { 0x96e4fc08d96862dbl,0xf9e29bb6c50c14b2l,0xfedaad64f8f9be75l,
            0xab6b2d79ae9e1274l },
          { 0x033e3eb58d84dec0l,0xc136904ccbd113e7l,0xb82b0aed6061f289l,
            0x3476d9247b699e25l },
          0 },
        /* 60 << 24 */
        { { 0x8fb5ceeb969231dcl,0xaed13be1686ff6cdl,0x71d7c67bdd69db87l,
            0x49613e08fb53f33al },
          { 0x2899729ead8e802fl,0x83bfde49d1982a1dl,0x675c45ea878239d2l,
            0xb7bf59cd0d8240d3l },
          0 },
        /* 61 << 24 */
        { { 0x853d8cd1baf53b8bl,0x9c73d04cff95fc18l,0xae8a94412d1d6aacl,
            0xd8a15ce901500b70l },
          { 0xaef813499aacba59l,0x2cd2ba0ac493cd8dl,0x01c37ee1f398f034l,
            0xed72d51d0f7299fcl },
          0 },
        /* 63 << 24 */
        { { 0x2c204940e7592fb1l,0xcc1bb19b49366f08l,0x31855e8a7c927935l,
            0x16f7e9a2c590b81dl },
          { 0xa5fbb7c1ed8df240l,0x7b5204122de2d7f5l,0x7eb1eb989a637588l,
            0x5ef4eca89540d2e8l },
          0 },
        /* 64 << 24 */
        { { 0x55d5c68da61a76fal,0x598b441dca1554dcl,0xd39923b9773b279cl,
            0x33331d3c36bf9efcl },
          { 0x2d4c848e298de399l,0xcfdb8e77a1a27f56l,0x94c855ea57b8ab70l,
            0xdcdb9dae6f7879bal },
          0 },
        /* 65 << 24 */
        { { 0x811e14dd9594afb8l,0xaf6c1b10d349124al,0x8488021b6528a642l,
            0xecf6834341cf1447l },
          { 0x7a40acb756924446l,0xd9c11bbed98ec4cfl,0x0cef00bfb2bff163l,
            0xfaaad8015432803bl },
          0 },
        /* 71 << 24 */
        { { 0x5a217d5e6b075cbel,0x7ef88d1dc89b513bl,0xb6d015da0531c93bl,
            0x477b502a6333834al },
          { 0x4655e48b2fb458d5l,0x93f21a7cb7674ca8l,0xa0616786502d1f3al,
            0x82d16d17f26bb6ccl },
          0 },
        /* 77 << 24 */
        { { 0x3d995aa9183c1688l,0xa125906c3766d2e8l,0x23ed7871c5f10d5bl,
            0xdfe1e1cc6df80368l },
          { 0x8bfcb54271eaae2cl,0xe94e6f910945a7bbl,0xd543ef90862f650al,
            0x0dc043b803eed66bl },
          0 },
        /* 83 << 24 */
        { { 0x0c6a5620060d2ccdl,0xcd8200e37a8a03a4l,0x6018d304793867e6l,
            0xad23dd61a74d054dl },
          { 0x5a856faeebc21eb4l,0x66be16714b5cd7dbl,0xe0d0441ec75f8c9dl,
            0xb80ca9ecf90dbc6dl },
          0 },
        /* 89 << 24 */
        { { 0xbd6902ccd24692cbl,0xbcce6bbc21920408l,0x40f120ca55dec4c5l,
            0xd9f1f5ef5361c8b3l },
          { 0x535d368226935dffl,0x9635447b01a9998al,0x8c4ec40d99e36d12l,
            0xbaeef8912b793369l },
          0 },
        /* 95 << 24 */
        { { 0xded3a51c1cd887ebl,0xd43225568376515cl,0xdaf3a2271ca7c097l,
            0x089156fdecd4d90cl },
          { 0x2b354810ca0727c9l,0xb7257c1966c19d8cl,0x5e68a379432d5072l,
            0x75c04c2443e585c7l },
          0 },
        /* 101 << 24 */
        { { 0xb5ba2a8fe5e0952fl,0x2c2d086811040b4el,0x27448bd5f818e253l,
            0x720f677987a92c85l },
          { 0x2c9b2367b9d035fal,0xf18ad8ce16c15ab9l,0xd65a360841bd57eel,
            0xeb4b07c9ff6ae897l },
          0 },
        /* 107 << 24 */
        { { 0xcffb6d71d38589acl,0x812372920fa509d3l,0x94db5ba6e54725e8l,
            0x1ad2b4206cfbb825l },
          { 0x8592c1f238cfb9f2l,0xbe8e917e0eec6a27l,0x53921bfe9d93d42fl,
            0x1aa95e6269454a35l },
          0 },
        /* 113 << 24 */
        { { 0xc25e8934d898049dl,0xeeaf4e6d3bb3d459l,0xc3ac44447d29ad10l,
            0xccdf9fcbcef8fa04l },
          { 0x1d995a3fb9679cb9l,0x3d6c5eab46fabc14l,0xd3849ff066385d4dl,
            0xc0eb21bacff08be2l },
          0 },
        /* 116 << 24 */
        { { 0x8213c71e90d13fd6l,0x114321149bb6b733l,0xaaf8037880ac4902l,
            0xb24e046b555f7557l },
          { 0x5f6ed2881db79832l,0xd493a758ac760e5dl,0xbc30a2a7a1c0f570l,
            0xa5009807161174e3l },
          0 },
        /* 119 << 24 */
        { { 0x9e9b864a6889e952l,0xee908932f352f31al,0xe421f2423166b932l,
            0x6dd4aa3b7ddbdb35l },
          { 0x553cc5639e8b88a4l,0x05457f171f04704dl,0x1dcc3004c9554e6bl,
            0x3a4a3a253f1b61e7l },
          0 },
        /* 125 << 24 */
        { { 0x7ac0a5e7c56e303al,0x7c7bab64037b0a19l,0x11f103fcc8d29a2bl,
            0x7d99dc46cf0b1340l },
          { 0x0481588ceffba92el,0x8a817356b04e77bcl,0x19edf4dbce1b708dl,
            0xa2a1f7a6e6f9d52cl },
          0 },
    },
    {
        /* 0 << 32 */
        { { 0x00, 0x00, 0x00, 0x00 },
          { 0x00, 0x00, 0x00, 0x00 },
          1 },
        /* 1 << 32 */
        { { 0x202886024147519al,0xd0981eac26b372f0l,0xa9d4a7caa785ebc8l,
            0xd953c50ddbdf58e9l },
          { 0x9d6361ccfd590f8fl,0x72e9626b44e6c917l,0x7fd9611022eb64cfl,
            0x863ebb7e9eb288f3l },
          0 },
        /* 3 << 32 */
        { { 0xa18f07e0e90fb21el,0x00fd2b80bba7fca1l,0x20387f2795cd67b5l,
            0x5b89a4e7d39707f7l },
          { 0x8f83ad3f894407cel,0xa0025b946c226132l,0xc79563c7f906c13bl,
            0x5f548f314e7bb025l },
          0 },
        /* 4 << 32 */
        { { 0x0ee6d3a7c35d8794l,0x042e65580356bae5l,0x9f59698d643322fdl,
            0x9379ae1550a61967l },
          { 0x64b9ae62fcc9981el,0xaed3d6316d2934c6l,0x2454b3025e4e65ebl,
            0xab09f647f9950428l },
          0 },
        /* 5 << 32 */
        { { 0xc1b3d3d331b85f09l,0x0f45354aa88ae64al,0xa8b626d32fec50fdl,
            0x1bdcfbd4e828834fl },
          { 0xe45a2866cd522539l,0xfa9d4732810f7ab3l,0xd8c1d6b4c905f293l,
            0x10ac80473461b597l },
          0 },
        /* 7 << 32 */
        { { 0xbbb175146fc627e2l,0xa0569bc591573a51l,0xa7016d9e358243d5l,
            0x0dac0c56ac1d6692l },
          { 0x993833b5da590d5fl,0xa8067803de817491l,0x65b4f2124dbf75d0l,
            0xcc960232ccf80cfbl },
          0 },
        /* 9 << 32 */
        { { 0x35d742806cf3d65bl,0x4b7c790678b28dd9l,0xc4fcdd2f95e1f85fl,
            0xcf6fb7ba591350b6l },
          { 0x9f8e3287edfc26afl,0xe2dd9e73c2d0ed9al,0xeab5d67f24cbb703l,
            0x60c293999a759a5al },
          0 },
        /* 10 << 32 */
        { { 0xcf8625d7708f97cdl,0xfb6c5119ea419de4l,0xe8cb234dc03f9b06l,
            0x5a7822c335e23972l },
          { 0x9b876319a284ff10l,0xefcc49977093fdcel,0xdddfd62a878fe39al,
            0x44bfbe53910aa059l },
          0 },
        /* 11 << 32 */
        { { 0xfb93ca3d7ca53d5fl,0x432649f004379cbfl,0xf506113acba2ff75l,
            0x4594ae2103718b35l },
          { 0x1aa6cee50d044627l,0xc0e0d2b7f5c94aa2l,0x0bf33d3dee4dd3f5l,
            0xaca96e288477c97al },
          0 },
        /* 13 << 32 */
        { { 0x995c068e6861a713l,0xa9ba339463de88dcl,0xab954344689a964fl,
            0x58195aec0f5a0d6cl },
          { 0xc5f207d5c98f8b50l,0x6600cd280c98ccf6l,0x1a680fe339c3e6c2l,
            0xa23f3931660e87c0l },
          0 },
        /* 15 << 32 */
        { { 0x43bc1b42c78440a1l,0x9a07e22632ac6c3fl,0xaf3d7ba10f4bcd15l,
            0x3ad43c9da36814c6l },
          { 0xca11f742a0c9c162l,0xd3e06fc6c90b96ecl,0xeace6e766bf2d03fl,
            0x8bcd98e8f8032795l },
          0 },
        /* 16 << 32 */
        { { 0xe27a6dbe305406ddl,0x8eb7dc7fdd5d1957l,0xf54a6876387d4d8fl,
            0x9c479409c7762de4l },
          { 0xbe4d5b5d99b30778l,0x25380c566e793682l,0x602d37f3dac740e3l,
            0x140deabe1566e4ael },
          0 },
        /* 17 << 32 */
        { { 0x7be3ddb77099ae96l,0x83d6157306e0da6al,0x31bcac5f74bf9870l,
            0x7f7aa3b422b256f1l },
          { 0xff84d63caa212e20l,0x7d636556decdc8b5l,0x8fed824dbf909d62l,
            0x62d70186e5fb1445l },
          0 },
        /* 19 << 32 */
        { { 0x8796989f67d8ab8al,0xa46282253700b772l,0xa353cadf05f799abl,
            0x7a8be2741eeb06bbl },
          { 0xf74a367e4653b134l,0x4e43449660c70340l,0xc99b6d6b72e10b18l,
            0xcf1adf0f1ba636e1l },
          0 },
        /* 21 << 32 */
        { { 0xb0260fb57c6a0958l,0xae791b9c2fc2731el,0xb339f2bf8ce6e575l,
            0x769214a816e2639fl },
          { 0xbaf422e1346da10el,0xc7805fdf7a56f463l,0xf47b6b766f845428l,
            0x8f21369e38492948l },
          0 },
        /* 23 << 32 */
        { { 0x2bac716a17931a90l,0x42a5e27cc8267236l,0xfd4b367c0bafeb78l,
            0x5856e69c6173db02l },
          { 0xfaac7358973d73c4l,0xbfbffcc36768d285l,0x05444ff2be3eb243l,
            0x9f8d3692f3c323fel },
          0 },
        /* 25 << 32 */
        { { 0xac296863221c31a9l,0x46f3a24ef1ca99a9l,0xd927648a7535a864l,
            0xd7e3c47d5848e497l },
          { 0xc19595b782a98ac7l,0x9a9bf627273ff554l,0xe29aa48fb62298a1l,
            0xed3f068ee797e9e3l },
          0 },
        /* 27 << 32 */
        { { 0x8d16a1660eb9227bl,0xe04c6bc58c37c74bl,0xd1be9585cc1ef78cl,
            0xa5cfe1962e929d9bl },
          { 0xc9b0ea21417c1cc6l,0x316352d345b79599l,0xc1502c4dc2d54af7l,
            0xe7f4412990f83445l },
          0 },
        /* 28 << 32 */
        { { 0x0f6704abd95917e8l,0x168dafaeaec6e899l,0xd2833e8cde710027l,
            0x34ea277e68ee3c59l },
          { 0x3689e2350054d4e5l,0x6f3a568d11013943l,0xb5ce1ff69bc2b144l,
            0x705bfe7e72b33a59l },
          0 },
        /* 29 << 32 */
        { { 0x1baa4f02c8e93284l,0xec6b93ea3c97d3e8l,0xb656c149034f8b32l,
            0x3cab9063cd4cc69fl },
          { 0xd8de5989d61031ccl,0xcf85329fc1b1de1dl,0xf18b78b323d8cb9al,
            0x6dc04bc61a6b69eal },
          0 },
        /* 31 << 32 */
        { { 0x79cf86314a1d4f8fl,0xda5ba331aa47394el,0x36f9c0be8ff20527l,
            0xccdc719bbc7097f6l },
          { 0x2304a3ba5cb052bbl,0xab80cdea392f0ab5l,0x0ac1858bf38de03bl,
            0xd6e2119878a8f55dl },
          0 },
        /* 33 << 32 */
        { { 0x6bdebc26584bc618l,0x499f0f1894591499l,0xd35ed50bf4a573dal,
            0x5a622e73ff2792d0l },
          { 0x8510cbce68d41a3bl,0x6610f43c94e919afl,0x4527373dc163c8a1l,
            0x50afb46f280a8a7dl },
          0 },
        /* 34 << 32 */
        { { 0x33e779cd8de7707al,0xf94bbd94438f535bl,0x61159864be144878l,
            0xb6623235f098ce4al },
          { 0x6813b71ba65568d8l,0x6603dd4c2f796451l,0x9a97d88c8b9ee5b2l,
            0xaaa4593549d5926cl },
          0 },
        /* 35 << 32 */
        { { 0x2e01fc75ebe75bf2l,0x8270318d6cbdd09cl,0x534e4f21d3f1a196l,
            0x6c9eaeca9459173el },
          { 0xda454fe0b642a1d4l,0xe45b69bfc4664c4al,0x4724bd423e078dc8l,
            0x39ac8fe603336b81l },
          0 },
        /* 36 << 32 */
        { { 0x0a2e53dd302e9485l,0x75882a19deaa9ff4l,0xe283242eac8de4ddl,
            0x2742105cc678dba7l },
          { 0x9f6f0a88cdb3a8a2l,0x5c9d3338f722e894l,0xf1fa3143c38c31c1l,
            0x22137e2db18c77acl },
          0 },
        /* 37 << 32 */
        { { 0xd821665e368d7835l,0x3300c012b596c6ecl,0xb60da7353557b2ddl,
            0x6c3d9db6fb8cf9ael },
          { 0x092d8b0b8b4b0d34l,0x900a0bf4b3d4107dl,0x75371a245e813ec3l,
            0x91125a17f2ad56d5l },
          0 },
        /* 39 << 32 */
        { { 0x5e6594e2fe0073e6l,0x908a93778be13cb7l,0xa2c3d5c8ac26617cl,
            0xa0bab085c317c6b9l },
          { 0x0bdc183b83664109l,0x6bbba2b468f9dcd9l,0x697a50785814be41l,
            0x12a59b183a5e5f98l },
          0 },
        /* 40 << 32 */
        { { 0xbd9802e6c30fa92bl,0x5a70d96d9a552784l,0x9085c4ea3f83169bl,
            0xfa9423bb06908228l },
          { 0x2ffebe12fe97a5b9l,0x85da604971b99118l,0x9cbc2f7f63178846l,
            0xfd96bc709153218el },
          0 },
        /* 41 << 32 */
        { { 0xb5a85c61bfa70ca6l,0x4edc7f2d4c1f745fl,0x05aea9aa3ded1eb5l,
            0x750385efb82e5918l },
          { 0xdcbc53221fdc5164l,0x32a5721f6794184el,0x5c5b2269ff09c90bl,
            0x96d009115323ca42l },
          0 },
        /* 43 << 32 */
        { { 0x12c73403f43f1440l,0xc94813eb66cc1f50l,0x04d5957b9b035151l,
            0x76011bca4bfaafa8l },
          { 0x56806c13574f1f0al,0x98f63a4697652a62l,0x17c63ef4a3178de9l,
            0xf7ce961a65009a52l },
          0 },
        /* 44 << 32 */
        { { 0x58f92aebe4173516l,0xdc37d99275e42d44l,0x76dcec5b4d48e1bal,
            0x07e0608e25676448l },
          { 0xa1877bcd1d4af36al,0x38b62b3c5a8ccf0cl,0x60522e88aeab7f75l,
            0xbef213ed5e03547al },
          0 },
        /* 45 << 32 */
        { { 0x8acd5ba4e6ed0282l,0x792328f06a04531dl,0xe95de8aa80297e50l,
            0x79d33ce07d60e05cl },
          { 0xcb84646dd827d602l,0xd3421521302a608cl,0x867970a4524f9751l,
            0x05e2f7e347a75734l },
          0 },
        /* 46 << 32 */
        { { 0x64e4de4a01c66263l,0xbcfe16a4d0033d4cl,0x359e23d4817de1dcl,
            0xb01e812ec259449cl },
          { 0x90c9ade2df53499fl,0xabbeaa27288c6862l,0x5a655db4cd1b896fl,
            0x416f10a5a022a3d6l },
          0 },
        /* 47 << 32 */
        { { 0x0d17e1ef98601fd5l,0x9a3f85e0eab76a6fl,0x0b9eaed1510b80a1l,
            0x3282fd747ec30422l },
          { 0xaca5815a70a4a402l,0xfad3121cf2439cb2l,0xba251af81fccabd6l,
            0xb382843fa5c127d5l },
          0 },
        /* 48 << 32 */
        { { 0x958381db1782269bl,0xae34bf792597e550l,0xbb5c60645f385153l,
            0x6f0e96afe3088048l },
          { 0xbf6a021577884456l,0xb3b5688c69310ea7l,0x17c9429504fad2del,
            0xe020f0e517896d4dl },
          0 },
        /* 49 << 32 */
        { { 0x442fdfe920cd1ebel,0xa8317dfa6a250d62l,0x5214576d082d5a2dl,
            0xc1a5d31930803c33l },
          { 0x33eee5b25e4a2cd0l,0x7df181b3b4db8011l,0x249285145b5c6b0bl,
            0x464c1c5828bf8837l },
          0 },
        /* 51 << 32 */
        { { 0x5464da65d55babd1l,0x50eaad2a0048d80fl,0x782ca3dd2b9bce90l,
            0x41107164ab526844l },
          { 0xad3f0602d56e0a5fl,0xc1f0248018455114l,0xe05d8dcab1527931l,
            0x87818cf5bb1295d7l },
          0 },
        /* 52 << 32 */
        { { 0x95aeb5bd483e333al,0x003af31effeaededl,0xfc5532e87efb1e4fl,
            0xb37e0fb52dfa24a5l },
          { 0x485d4cecdc140b08l,0xb81a0d23983bd787l,0xd19928dae8d489fdl,
            0x3fa0312c177b9dbdl },
          0 },
        /* 53 << 32 */
        { { 0xade391470c6d7e88l,0x4fd1e8cd47072c45l,0x145760fed5a65c56l,
            0x198960c7be4887del },
          { 0xfe7974a82640257al,0xf838a19b774febefl,0xb2aecad11b6e988el,
            0x643f44fa448e4a8fl },
          0 },
        /* 55 << 32 */
        { { 0xc35ceffdee756e71l,0x2c1364d88ea932c4l,0xbd594d8d837d2d9fl,
            0x5b334bdac9d74d48l },
          { 0x72dc3e03b8fac08bl,0x38f01de006fdf70fl,0x4bde74b31d298ba4l,
            0x2598d183ad5f42a9l },
          0 },
        /* 57 << 32 */
        { { 0x02c6ba15f62befa2l,0x6399ceb55c8ccee9l,0x3638bd6e08d3473el,
            0xb8f1f13d2f8f4a9cl },
          { 0x50d7560655827a74l,0x8d6e65f33fb4f32cl,0x40a5d21189ee621al,
            0x6d3f9e11c4474716l },
          0 },
        /* 59 << 32 */
        { { 0xcb633a4ce9b2bb8fl,0x0475703f8c529253l,0x61e007b5a8878873l,
            0x342d77ba14504159l },
          { 0x2925175c313578dfl,0x4e631897b6b097f1l,0xe64d138929350e41l,
            0x2fb20608ec7adccdl },
          0 },
        /* 60 << 32 */
        { { 0xa560c234d5c0f5d1l,0x74f84bf62bdef0efl,0x61ed00005cbd3d0bl,
            0xc74262d087fb408bl },
          { 0xad30a6496cc64128l,0x708e3a31a4a8b154l,0xaf21ce2637f82074l,
            0x31d33b38204c9a74l },
          0 },
        /* 61 << 32 */
        { { 0x8f609fe04cc2f575l,0xe44f9784b35488c4l,0x0d464bb6180fa375l,
            0x4f44d5d2de2247b8l },
          { 0xf538eb38141ef077l,0x781f8f6e8fa456a4l,0x67e9a46429b4f39dl,
            0x245d21e8b704c3e9l },
          0 },
        /* 63 << 32 */
        { { 0x45a94ee858ffa7cdl,0x4d38bc6818053549l,0x0b4bc65a499d79f3l,
            0xa81e3ab09159cab7l },
          { 0xf13716efb47898cel,0xb7ee597c2e2d9044l,0x09396b90e6158276l,
            0x5c644dc36a533fcel },
          0 },
        /* 64 << 32 */
        { { 0xcca4428dbbe5a1a9l,0x8187fd5f3126bd67l,0x0036973a48105826l,
            0xa39b6663b8bd61a0l },
          { 0x6d42deef2d65a808l,0x4969044f94636b19l,0xf611ee47dd5d564cl,
            0x7b2f3a49d2873077l },
          0 },
        /* 65 << 32 */
        { { 0xbe4c16c3bf429668l,0xd32f56f0ef35db3bl,0xae8355de9ea4e3f1l,
            0x8f66c4a2a450944el },
          { 0xafab94c8b798fbe2l,0x18c57baff7f3d5cfl,0x692d191c5cfa5c7dl,
            0xc0c25f69a689daebl },
          0 },
        /* 71 << 32 */
        { { 0x15fb3ae398340d4cl,0xa8b9233a7de82134l,0x44971a545fc0dbc6l,
            0xb2b4f0f3a1d3f094l },
          { 0x8d9eaba1b6242bd4l,0xd8aad777787cc557l,0xb1ab8b7870d1a2bbl,
            0x5d20f48cead3bfe3l },
          0 },
        /* 77 << 32 */
        { { 0x4dacbf09a2bf9772l,0x969a4c4357aa8457l,0xadbe673b273ebfc5l,
            0xb85582bb927778c9l },
          { 0x748371855c03752cl,0xc337bc6bc2f60d11l,0x2c3838e4ad456a09l,
            0xaf479c897e381842l },
          0 },
        /* 83 << 32 */
        { { 0x8530ae751b1aea77l,0xf43b923ba8310cb9l,0x9c1a60c6bf4dd6c5l,
            0x11885b863e3aaaa5l },
          { 0x594a8fa90f69821el,0x1eece3d66bc37998l,0x1fd718f518df32bfl,
            0x1c00c7d461d84082l },
          0 },
        /* 89 << 32 */
        { { 0xd67ee3a4c763c3cfl,0x760b128305969234l,0x1a5ff331ec17f2d1l,
            0x25f0392a84fecfefl },
          { 0xb1bc004a3a80d47el,0xf450bf08182fee3bl,0xf11117681e19751el,
            0x5b4127dae28ed23fl },
          0 },
        /* 95 << 32 */
        { { 0x91e00defdaf08f09l,0x7ef41724f4738a07l,0x990fbbceaf1263fcl,
            0x779121e3e6eeb5aal },
          { 0x3e162c7a5a3ecf52l,0x73ae568a51be5faal,0x8bea1bfa451be8a9l,
            0x3e8cd5db90e11097l },
          0 },
        /* 101 << 32 */
        { { 0x90390f7224d27159l,0x685c139efd07e5d4l,0x4e21e44a3bc234a8l,
            0x61b50f34eeb14dacl },
          { 0x7beb0aa087555d58l,0x781326bcc806f0d2l,0xc289537a1eb7199fl,
            0x44a31a037b42766el },
          0 },
        /* 107 << 32 */
        { { 0x7d778206edde4b40l,0x34539fa18eb92fcdl,0x5a0bdd79bf52a552l,
            0x066d3672fdcca75el },
          { 0xd73fa893e28b5a5bl,0xb495135876c38698l,0x44469b0114ae16cfl,
            0xb428c763691d6618l },
          0 },
        /* 113 << 32 */
        { { 0x9022db8b69196353l,0x152ebb7dd7a4afd0l,0xea36fae57fcf1765l,
            0xa8fc00ba0decea8al },
          { 0x1047206a0c0b0414l,0x6607d8ade076df28l,0xf343e19966b8aba1l,
            0x7f03c1ad311e208dl },
          0 },
        /* 116 << 32 */
        { { 0xe6b4c96e888f3870l,0xa21bb618fe544042l,0x7122ee88bd817699l,
            0xcb38ecebfa66e173l },
          { 0x6ed5b3482c9cc05fl,0x591affc84ae0fd9el,0x7cf325ac6e7aaac0l,
            0x2397c053d05e5be0l },
          0 },
        /* 119 << 32 */
        { { 0x95363f61eaa96552l,0xe03bc6b38fb15b73l,0xa5c5808f2c389053l,
            0xcd021e6c11b2030cl },
          { 0x349ca9bdc038e30al,0x0a3368d4165afa2cl,0x043630debbfa1cc6l,
            0xb8c4456ba7cdbf69l },
          0 },
        /* 125 << 32 */
        { { 0x63aa3315fd7d2983l,0xaf4c96afa6a04bedl,0x3a5c0b5410814a74l,
            0x9906f5e30f9b0770l },
          { 0x622be6523676986fl,0x09ac5bc0173e7cb5l,0x1c40e56a502c8b3cl,
            0xabb9a0f7253ce8f6l },
          0 },
    },
    {
        /* 0 << 40 */
        { { 0x00, 0x00, 0x00, 0x00 },
          { 0x00, 0x00, 0x00, 0x00 },
          1 },
        /* 1 << 40 */
        { { 0x889f6d65533ef217l,0x7158c7e4c3ca2e87l,0xfb670dfbdc2b4167l,
            0x75910a01844c257fl },
          { 0xf336bf07cf88577dl,0x22245250e45e2acel,0x2ed92e8d7ca23d85l,
            0x29f8be4c2b812f58l },
          0 },
        /* 3 << 40 */
        { { 0xc51e414351facc61l,0xbaf2647de68a25bcl,0x8f5271a00ff872edl,
            0x8f32ef993d2d9659l },
          { 0xca12488c7593cbd4l,0xed266c5d02b82fabl,0x0a2f78ad14eb3f16l,
            0xc34049484d47afe3l },
          0 },
        /* 4 << 40 */
        { { 0xa6f3d574c005979dl,0xc2072b426a40e350l,0xfca5c1568de2ecf9l,
            0xa8c8bf5ba515344el },
          { 0x97aee555114df14al,0xd4374a4dfdc5ec6bl,0x754cc28f2ca85418l,
            0x71cb9e27d3c41f78l },
          0 },
        /* 5 << 40 */
        { { 0x09c1670209470496l,0xa489a5edebd23815l,0xc4dde4648edd4398l,
            0x3ca7b94a80111696l },
          { 0x3c385d682ad636a4l,0x6702702508dc5f1el,0x0c1965deafa21943l,
            0x18666e16610be69el },
          0 },
        /* 7 << 40 */
        { { 0x45beb4ca2a604b3bl,0x56f651843a616762l,0xf52f5a70978b806el,
            0x7aa3978711dc4480l },
          { 0xe13fac2a0e01fabcl,0x7c6ee8a5237d99f9l,0x251384ee05211ffel,
            0x4ff6976d1bc9d3ebl },
          0 },
        /* 9 << 40 */
        { { 0xdde0492316e043a2l,0x98a452611dd3d209l,0xeaf9f61bd431ebe8l,
            0x00919f4dbaf56abdl },
          { 0xe42417db6d8774b1l,0x5fc5279c58e0e309l,0x64aa40613adf81eal,
            0xef419edabc627c7fl },
          0 },
        /* 10 << 40 */
        { { 0x3919759239ef620fl,0x9d47284074fa29c4l,0x4e428fa39d416d83l,
            0xd1a7c25129f30269l },
          { 0x46076e1cd746218fl,0xf3ad6ee8110d967el,0xfbb5f434a00ae61fl,
            0x3cd2c01980d4c929l },
          0 },
        /* 11 << 40 */
        { { 0xfa24d0537a4af00fl,0x3f938926ca294614l,0x0d700c183982182el,
            0x801334434cc59947l },
          { 0xf0397106ec87c925l,0x62bd59fc0ed6665cl,0xe8414348c7cca8b5l,
            0x574c76209f9f0a30l },
          0 },
        /* 13 << 40 */
        { { 0x95be42e2bb8b6a07l,0x64be74eeca23f86al,0xa73d74fd154ce470l,
            0x1c2d2857d8dc076al },
          { 0xb1fa1c575a887868l,0x38df8e0b3de64818l,0xd88e52f9c34e8967l,
            0x274b4f018b4cc76cl },
          0 },
        /* 15 << 40 */
        { { 0x3f5c05b4f8b7559dl,0x0be4c7acfae29200l,0xdd6d3ef756532accl,
            0xf6c3ed87eea7a285l },
          { 0xe463b0a8f46ec59bl,0x531d9b14ecea6c83l,0x3d6bdbafc2dc836bl,
            0x3ee501e92ab27f0bl },
          0 },
        /* 16 << 40 */
        { { 0x8df275455922ac1cl,0xa7b3ef5ca52b3f63l,0x8e77b21471de57c4l,
            0x31682c10834c008bl },
          { 0xc76824f04bd55d31l,0xb6d1c08617b61c71l,0x31db0903c2a5089dl,
            0x9c092172184e5d3fl },
          0 },
        /* 17 << 40 */
        { { 0x7b1a921ea6b3340bl,0x6d7c4d7d7438a53el,0x2b9ef73c5bf71d8fl,
            0xb5f6e0182b167a7cl },
          { 0x5ada98ab0ce536a3l,0xee0f16f9e1fea850l,0xf6424e9d74f1c0c5l,
            0x4d00de0cd3d10b41l },
          0 },
        /* 19 << 40 */
        { { 0xd542f522a6533610l,0xfdde15a734ec439al,0x696560fedc87dd0dl,
            0x69eab421e01fd05fl },
          { 0xca4febdc95cc5988l,0x839be396c44d92fbl,0x7bedff6daffe543bl,
            0xd2bb97296f6da43al },
          0 },
        /* 21 << 40 */
        { { 0x5bc6dea80b8d0077l,0xb2adf5d1ea9c49efl,0x7104c20eaafe8659l,
            0x1e3604f37866ee7el },
          { 0x0cfc7e7b3075c8c5l,0x5281d9bb639c5a2bl,0xcbdf42494bc44ee3l,
            0x835ab066655e9209l },
          0 },
        /* 23 << 40 */
        { { 0x78fbda4b90b94ffal,0x447e52eb7beb993cl,0x920011bc92620d15l,
            0x7bad6ecf481fd396l },
          { 0xad3bd28ba989a09el,0x20491784a3e62b78l,0xcdcd7096b07bd9efl,
            0x9bf5bb7337d780adl },
          0 },
        /* 25 << 40 */
        { { 0xbe911a71a976c8d4l,0xba0346743fdd778el,0x2359e7434cf87ea1l,
            0x8dccf65f07ebb691l },
          { 0x6c2c18eb09746d87l,0x6a19945fd2ecc8fal,0xc67121ff2ffa0339l,
            0x408c95ba9bd9fc31l },
          0 },
        /* 27 << 40 */
        { { 0xa317204bcaa5da39l,0xd390df7468bf53d7l,0x56de18b2dbd71c0dl,
            0xcb4d3bee75184779l },
          { 0x815a219499d920a5l,0x9e10fb4ecf3d3a64l,0x7fd4901dfe92e1eel,
            0x5d86d10d3ab87b2el },
          0 },
        /* 28 << 40 */
        { { 0x24f2a692840bb336l,0x7c353bdca669fa7bl,0xda20d6fcdec9c300l,
            0x625fbe2fa13a4f17l },
          { 0xa2b1b61adbc17328l,0x008965bfa9515621l,0x49690939c620ff46l,
            0x182dd27d8717e91cl },
          0 },
        /* 29 << 40 */
        { { 0x98e9136c878303e4l,0x2769e74fd1e65efdl,0x6154c545809da56el,
            0x8c5d50a04301638cl },
          { 0x10f3d2068214b763l,0x2da9a2fc44df0644l,0xca912bab588a6fcdl,
            0xe9e82d9b227e1932l },
          0 },
        /* 31 << 40 */
        { { 0xcbdc4d66d080e55bl,0xad3f11e5b8f98d6bl,0x31bea68e18a32480l,
            0xdf1c6fd52c1bcf6el },
          { 0xadcda7ee118a3f39l,0xbd02f857ac060d5fl,0xd2d0265d86631997l,
            0xb866a7d33818f2d4l },
          0 },
        /* 33 << 40 */
        { { 0xfbcce2d31892d98dl,0x2e34bc9507de73dcl,0x3a48d1a94891eec1l,
            0xe64499c24d31060bl },
          { 0xe9674b7149745520l,0xf126ccaca6594a2cl,0x33e5c1a079945342l,
            0x02aa0629066e061fl },
          0 },
        /* 34 << 40 */
        { { 0xdfd7c0ae7af3191el,0x923ec111d68c70d9l,0xb6f1380bb675f013l,
            0x9192a224f23d45bal },
          { 0xbe7890f9524891e3l,0x45b24c47eba996bbl,0x59331e48320447e9l,
            0x0e4d8753ac9afad4l },
          0 },
        /* 35 << 40 */
        { { 0x49e49c38c9f5a6c3l,0x3f5eea44d8ee2a65l,0x02bf3e761c74bbb4l,
            0x50d291cdef565571l },
          { 0xf4edc290a36dd5fal,0x3015df9556dd6b85l,0x4494926aa5549a16l,
            0x5de6c59390399e4al },
          0 },
        /* 36 << 40 */
        { { 0x29be11c6ce800998l,0x72bb1752b90360d9l,0x2c1931975a4ad590l,
            0x2ba2f5489fc1dbc0l },
          { 0x7fe4eebbe490ebe0l,0x12a0a4cd7fae11c0l,0x7197cf81e903ba37l,
            0xcf7d4aa8de1c6dd8l },
          0 },
        /* 37 << 40 */
        { { 0x961fa6317e249e7bl,0x5c4f707796caed50l,0x6b176e62d7e50885l,
            0x4dd5de72f390cbecl },
          { 0x91fa29954b2bd762l,0x80427e6395b8dadel,0xd565bf1de2c34743l,
            0x911da39d16e6c841l },
          0 },
        /* 39 << 40 */
        { { 0x48365465802ff016l,0x6d2a561f71beece6l,0xdd299ce6f9707052l,
            0x62a32698a23407bbl },
          { 0x1d55bdb147004afbl,0xfadec124369b1084l,0x1ce78adf291c89f7l,
            0x9f2eaf03278bc529l },
          0 },
        /* 40 << 40 */
        { { 0x92af6bf43fd5684cl,0x2b26eecf80360aa1l,0xbd960f3000546a82l,
            0x407b3c43f59ad8fel },
          { 0x86cae5fe249c82bal,0x9e0faec72463744cl,0x87f551e894916272l,
            0x033f93446ceb0615l },
          0 },
        /* 41 << 40 */
        { { 0x04658ad212dba0cel,0x9e600624068822f0l,0x84661f11b26d368bl,
            0xbca867d894ebb87al },
          { 0x79506dc42f1bad89l,0x1a8322d3ebcbe7a1l,0xb4f1e102ac197178l,
            0x29a950b779f7198cl },
          0 },
        /* 43 << 40 */
        { { 0x19a6fb0984a3d1d5l,0x6c75c3a2ba5f5307l,0x7983485bf9698447l,
            0x689f41b88b1cdc1el },
          { 0x18f6fbd74c1979d0l,0x3e6be9a27a0b6708l,0x06acb615f63d5a8al,
            0x8a817c098d0f64b1l },
          0 },
        /* 44 << 40 */
        { { 0x1e5eb0d18be82e84l,0x89967f0e7a582fefl,0xbcf687d5a6e921fal,
            0xdfee4cf3d37a09bal },
          { 0x94f06965b493c465l,0x638b9a1c7635c030l,0x7666786466f05e9fl,
            0xccaf6808c04da725l },
          0 },
        /* 45 << 40 */
        { { 0xa9b3479b1b53a173l,0xc041eda3392eddc0l,0xdb8f804755edd7eel,
            0xaf1f7a37ab60683cl },
          { 0x9318603a72c0accbl,0xab1bb9fe401cbf3cl,0xc40e991e88afe245l,
            0x9298a4580d06ac35l },
          0 },
        /* 46 << 40 */
        { { 0x58e127d5036c2fe7l,0x5fe5020555b93361l,0xc1373d850f74a045l,
            0x28cd79dbe8228e4bl },
          { 0x0ae82320c2018d9al,0xf6d0049c78f8016al,0x381b6fe2149b31fbl,
            0x33a0e8adec3cfbcfl },
          0 },
        /* 47 << 40 */
        { { 0x23a6612e9eab5da7l,0xb645fe29d94d6431l,0xe3d74594ca1210c4l,
            0xdc1376bceeca0674l },
          { 0xfd40dfef657f0154l,0x7952a548d52cbac5l,0x0ee189583685ad28l,
            0xd13639409ba9ca46l },
          0 },
        /* 48 << 40 */
        { { 0xca2eb690768fccfcl,0xf402d37db835b362l,0x0efac0d0e2fdfccel,
            0xefc9cdefb638d990l },
          { 0x2af12b72d1669a8bl,0x33c536bc5774ccbdl,0x30b21909fb34870el,
            0xc38fa2f77df25acal },
          0 },
        /* 49 << 40 */
        { { 0x1337902f1c982cd6l,0x222e08fe14ec53eal,0x6c8abd0d330ef3e5l,
            0xeb59e01531f6fd9dl },
          { 0xd74ae554a8532df4l,0xbc010db1ab44c83el,0xe98016561b8f9285l,
            0x65a9612783acc546l },
          0 },
        /* 51 << 40 */
        { { 0x36a8b0a76770cfb1l,0x3338d52f9bb578fcl,0x5136c785f5ed12a4l,
            0x652d47ed87bf129el },
          { 0x9c6c827e6067c2d0l,0x61fc2f410345533al,0x2d7fb182130cea19l,
            0x71a0186330b3ef85l },
          0 },
        /* 52 << 40 */
        { { 0x74c5f02bbf81f3f5l,0x0525a5aeaf7e4581l,0x88d2aaba433c54ael,
            0xed9775db806a56c5l },
          { 0xd320738ac0edb37dl,0x25fdb6ee66cc1f51l,0xac661d1710600d76l,
            0x931ec1f3bdd1ed76l },
          0 },
        /* 53 << 40 */
        { { 0xb81e239161faa569l,0xb379f759bb40eebfl,0x9f2fd1b2a2c54549l,
            0x0a968f4b0d6ba0ael },
          { 0xaa869e6eedfe8c75l,0x0e36b298645ab173l,0x5a76282b0bcdefd7l,
            0x9e949331d05293f2l },
          0 },
        /* 55 << 40 */
        { { 0xc1cfa9a1c59fac6el,0x2648bffcb72747cel,0x5f8a39805f2e2637l,
            0x8bd3a8eb73e65758l },
          { 0xd9c43f1df14381a7l,0xecc1c3b0d6a86c10l,0xffcf4fa8a4a6dc74l,
            0x7304fa834cea0a46l },
          0 },
        /* 57 << 40 */
        { { 0x4460760c34dca952l,0xeac9cf2444c70444l,0xb879297b8493c87el,
            0x295941a54b2dccb7l },
          { 0x1e5cecede58721cdl,0xc8b58db74ca0d12bl,0x1927965c6da1d034l,
            0x7220b02839ed1369l },
          0 },
        /* 59 << 40 */
        { { 0xc38746c83c2e34b6l,0x9f27362e38a51042l,0x26febec02067afebl,
            0xd9c4e15544e7371fl },
          { 0x6035f469f92930d1l,0xe6ed7c08b4431b8bl,0xa25bf5903e16410dl,
            0x147d83368adf4c18l },
          0 },
        /* 60 << 40 */
        { { 0x7f01c9ecaa80ba59l,0x3083411a68538e51l,0x970370f1e88128afl,
            0x625cc3db91dec14bl },
          { 0xfef9666c01ac3107l,0xb2a8d577d5057ac3l,0xb0f2629992be5df7l,
            0xf579c8e500353924l },
          0 },
        /* 61 << 40 */
        { { 0xbd9398d6ca02669fl,0x896e053bf9ad11a1l,0xe024b699a3556f9fl,
            0x23b4b96ad53cbca3l },
          { 0x549d2d6c89733dd6l,0x3dae193f394f3179l,0x8bf7ec1cdfeda825l,
            0xf6a1db7a8a4844b4l },
          0 },
        /* 63 << 40 */
        { { 0x3b5403d56437a027l,0xda32bbd233ed30aal,0xd2ad3baa906de0cal,
            0x3b6df514533f736el },
          { 0x986f1cab5df9b9c4l,0x41cd2088970d330el,0xaae7c2238c20a923l,
            0x52760a6e1e951dc0l },
          0 },
        /* 64 << 40 */
        { { 0xb8fa3d931341ed7al,0x4223272ca7b59d49l,0x3dcb194783b8c4a4l,
            0x4e413c01ed1302e4l },
          { 0x6d999127e17e44cel,0xee86bf7533b3adfbl,0xf6902fe625aa96cal,
            0xb73540e4e5aae47dl },
          0 },
        /* 65 << 40 */
        { { 0x55318a525e34036cl,0xc3acafaaf9884e3fl,0xe5ba15cea042ba04l,
            0x56a1d8960ada550el },
          { 0xa5198cae87b76764l,0xd079d1f0b6fd84fbl,0xb22b637bcbe363edl,
            0xbe8ab7d64499deaal },
          0 },
        /* 71 << 40 */
        { { 0xbe8eba5eb4925f25l,0x00f8bf582e3159d6l,0xb1aa24fa18856070l,
            0x22ea8b74e4c30b22l },
          { 0x512f633e55bbe4e8l,0x82ba62318678aee9l,0xea05da90fdf72b7el,
            0x616b9bc7a4fc65eel },
          0 },
        /* 77 << 40 */
        { { 0xe31ee3b3b7c221e7l,0x10353824e353fa43l,0x9d2f3df69dd2a86fl,
            0x8a12ab9322ccffecl },
          { 0x25c8e326d666f9e5l,0x33ea98a0598da7fbl,0x2fc1de0917f74e17l,
            0x0d0b6c7a35efb211l },
          0 },
        /* 83 << 40 */
        { { 0x22a82c6c804e6ecel,0x824a170b1d8fce9el,0x621802becee65ed0l,
            0x4a4e9e7895ec4285l },
          { 0x8da0988fa8940b7al,0xaff89c5b86445aa5l,0x386fdbdad689cde9l,
            0x3aeaae7d9f5caaccl },
          0 },
        /* 89 << 40 */
        { { 0xe9cb9e68a7b62f4cl,0x515cae0ec3b7092el,0xb8abec354b491f52l,
            0x672673fd01eeabc1l },
          { 0x65e5739f7ad6e8a1l,0xc2da8e003d91b2f9l,0xcc43229cced84319l,
            0x0f8cbf9574ccf2d1l },
          0 },
        /* 95 << 40 */
        { { 0xb03d1cfb1b2f872al,0x88aef4670872b6f7l,0xaafe55e48ea9170cl,
            0xd5cc4875f24aa689l },
          { 0x7e5732908458ce84l,0xef4e143d58bfc16dl,0xc58626efaa222836l,
            0x01c60ec0ca5e0cb8l },
          0 },
        /* 101 << 40 */
        { { 0x123901aa36337c09l,0x1697acadd2f5e675l,0xc0a1ddd022fe2bael,
            0xf68ea88cff0210ddl },
          { 0x665d11e014168709l,0x912a575f45f25321l,0x7e7ed38070c78934l,
            0x663d692cb0a46322l },
          0 },
        /* 107 << 40 */
        { { 0x912ab8bd8642cba4l,0x97fab1a3b6b50b73l,0x76666b3cb86ef354l,
            0x16d41330fa5ecce9l },
          { 0x77c7c138c7da404bl,0xc6508cb78c983fb0l,0xe5881733f9004984l,
            0x76dea7794182c7abl },
          0 },
        /* 113 << 40 */
        { { 0x16db18583556b765l,0x39c18c200263755al,0x7b6691f591c15201l,
            0x4e4c17b168514ea9l },
          { 0xacbe449e06f5f20al,0xeb9119d2541ddfb6l,0x2f6e687bf2eac86fl,
            0xb161471ec14ac508l },
          0 },
        /* 116 << 40 */
        { { 0x58846d32c4744733l,0x40517c71379f9e34l,0x2f65655f130ef6cal,
            0x526e4488f1f3503fl },
          { 0x8467bd177ee4a976l,0x1d9dc913921363d1l,0xd8d24c33b069e041l,
            0x5eb5da0a2cdf7f51l },
          0 },
        /* 119 << 40 */
        { { 0x81c2cc32951ab3e7l,0xc86d9a109b0c7e87l,0x0b7a18bd606ef408l,
            0x099b5bbfe6c2251el },
          { 0x46d627d0bfce880fl,0xbfaddcbbe1c6865al,0xa9ab6183d2bb9a00l,
            0x23cb9a2720ad9789l },
          0 },
        /* 125 << 40 */
        { { 0x1592d0630c25fbebl,0x13869ec24995a3fal,0x6413f494861d0a73l,
            0xa3b782342f9f1b89l },
          { 0x113689e2b6cad351l,0x53be2014a873dcc1l,0xccf405e0c6bb1be7l,
            0x4fff7b4ca9061ca9l },
          0 },
    },
    {
        /* 0 << 48 */
        { { 0x00, 0x00, 0x00, 0x00 },
          { 0x00, 0x00, 0x00, 0x00 },
          1 },
        /* 1 << 48 */
        { { 0xcc7a64880a750c0fl,0x39bacfe34e548e83l,0x3d418c760c110f05l,
            0x3e4daa4cb1f11588l },
          { 0x2733e7b55ffc69ffl,0x46f147bc92053127l,0x885b2434d722df94l,
            0x6a444f65e6fc6b7cl },
          0 },
        /* 3 << 48 */
        { { 0x6d0b16f4bdaedfbdl,0x23fd326086746cedl,0x8bfb1d2fff4b3e17l,
            0xc7f2ec2d019c14c8l },
          { 0x3e0832f245104b0dl,0x5f00dafbadea2b7el,0x29e5cf6699fbfb0fl,
            0x264f972361827cdal },
          0 },
        /* 4 << 48 */
        { { 0x97b14f7ea90567e6l,0x513257b7b6ae5cb7l,0x85454a3c9f10903dl,
            0xd8d2c9ad69bc3724l },
          { 0x38da93246b29cb44l,0xb540a21d77c8cbacl,0x9bbfe43501918e42l,
            0xfffa707a56c3614el },
          0 },
        /* 5 << 48 */
        { { 0x6eb1a2f3e30bc27fl,0xe5f0c05ab0836511l,0x4d741bbf4965ab0el,
            0xfeec41ca83464bbdl },
          { 0x1aca705f99d0b09fl,0xc5d6cc56f42da5fal,0x49964eddcc52b931l,
            0x8ae59615c884d8d8l },
          0 },
        /* 7 << 48 */
        { { 0xf634b57b39f8868al,0xe27f4fd475cc69afl,0xa47e58cbd0d5496el,
            0x8a26793fd323e07fl },
          { 0xc61a9b72fa30f349l,0x94c9d9c9b696d134l,0x792beca85880a6d1l,
            0xbdcc4645af039995l },
          0 },
        /* 9 << 48 */
        { { 0xce7ef8e58c796c3cl,0x9adaae84dd66e57al,0x784ae13e45227f33l,
            0xb046c5b82a85e757l },
          { 0xb7aa50aeec37631fl,0xbedc4fca3b300758l,0x0f82567e0ac9700bl,
            0x1071d9d44ff5f8d2l },
          0 },
        /* 10 << 48 */
        { { 0x61360ee99e240d18l,0x057cdcacb4b94466l,0xe7667cd12fe5325cl,
            0x1fa297b521974e3bl },
          { 0xfa4081e7db083d76l,0x31993be6f206bd15l,0x8949269b14c19f8cl,
            0x21468d72a9d92357l },
          0 },
        /* 11 << 48 */
        { { 0xd09ef6c4e51a2811l,0x39f6862bb8fb66b9l,0x64e77f8d22dfaa99l,
            0x7b10504461b08aacl },
          { 0x71704e4c4a7df332l,0xd09734342ffe015bl,0xab0eaf4408d3020el,
            0x28b1909eed63b97al },
          0 },
        /* 13 << 48 */
        { { 0x2f3fa882cdadcd4fl,0xa4ef68595f631995l,0xe52ca2f9e531766fl,
            0x20af5c3057e2c1d3l },
          { 0x1e4828f6e51e94b8l,0xf900a1751a2f5d4fl,0xe831adb3392c58a0l,
            0x4c5a90ca1b6e5866l },
          0 },
        /* 15 << 48 */
        { { 0x5f3dcba86182827cl,0xd1a448ddbd7e7252l,0x2d8f96fcf493b815l,
            0xba0a4c263b0aa95fl },
          { 0x88a1514063a0007fl,0x9564c25e6a9c5846l,0x5a4d7b0fdc0fcbcal,
            0x2275daa33f8a740el },
          0 },
        /* 16 << 48 */
        { { 0x83f49167ceca9754l,0x426d2cf64b7939a0l,0x2555e355723fd0bfl,
            0xa96e6d06c4f144e2l },
          { 0x4768a8dd87880e61l,0x15543815e508e4d5l,0x09d7e772b1b65e15l,
            0x63439dd6ac302fa0l },
          0 },
        /* 17 << 48 */
        { { 0x159591cc0461086bl,0xb695aa9495e66e51l,0x2d4c946779ded531l,
            0xbd2482ba89c2be79l },
          { 0x8ee2658aa20bbf19l,0xc000528a32247917l,0xd924be4affeae845l,
            0x51312bebed992c8bl },
          0 },
        /* 19 << 48 */
        { { 0x3a01b958dc752bd9l,0x2babdbc20c215d45l,0xe689d79a131641c1l,
            0x48e8f0da80e05ed4l },
          { 0x4b505feb77bb70c4l,0xefbd3e2bb6057ef7l,0x7583e22dce603ca5l,
            0xfbe3b1f22c5c70c7l },
          0 },
        /* 21 << 48 */
        { { 0x8ec1ecf029e5e35al,0x2f3168e58645c2b3l,0xe9297362c7f94cb2l,
            0x4fbf1466d1c90b39l },
          { 0x3e4f7656920bae2al,0x805d04b9f1beb172l,0x729a7208dbdbd4b4l,
            0x1aade45687aeca53l },
          0 },
        /* 23 << 48 */
        { { 0xb0ff1f541934a508l,0x19e1397604bbf31al,0xb2a8e6033717a6b4l,
            0xd601e45d0ef12cb9l },
          { 0x563f0af5b515e98el,0x9b129db633984f9bl,0xe34aba2fa47e4a65l,
            0xb56f82d19e3f9d82l },
          0 },
        /* 25 << 48 */
        { { 0x0203effdb1209b86l,0x21f063edb19d6cbfl,0x59f53476980f275bl,
            0x202456d7b7ac5e80l },
          { 0xe5a8c05f4900edc9l,0x04c08eb470f01e86l,0xf74ac2241dcd98cel,
            0x7e77cc0ce2e830dbl },
          0 },
        /* 27 << 48 */
        { { 0x74e37234a9747edel,0x4fc9fbb1361b1013l,0xe7b533733cf357efl,
            0x6aa2dd2c991c4193l },
          { 0x7887e4d2a770917al,0xdd1809b4c20d24cbl,0x004cd7c38e9c2d3el,
            0xc77c5baba9970abel },
          0 },
        /* 28 << 48 */
        { { 0x20ac0351d598d710l,0x272c4166cb3a4da4l,0xdb82fe1aca71de1fl,
            0x746e79f2d8f54b0fl },
          { 0x6e7fc7364b573e9bl,0x75d03f46fd4b5040l,0x5c1cc36d0b98d87bl,
            0x513ba3f11f472da1l },
          0 },
        /* 29 << 48 */
        { { 0x52927eaac3af237fl,0xfaa06065d7398767l,0x042e72b497c6ce0bl,
            0xdaed0cc40a9f2361l },
          { 0xddc2e11c2fc1bb4al,0x631da5770c1a9ef8l,0x8a4cfe44680272bfl,
            0xc76b9f7262fb5cc3l },
          0 },
        /* 31 << 48 */
        { { 0x248f814538b3aae3l,0xb5345864bc204334l,0x66d6b5bc1d127524l,
            0xe312080d14f572d3l },
          { 0x13ed15a716abafebl,0x6f18ce27dba967bel,0x96c9e826ef08552dl,
            0x2c191b06be2b63e0l },
          0 },
        /* 33 << 48 */
        { { 0xde4be45dc115ca51l,0xa028cafe934dabd6l,0x7e875663d1c0f8c5l,
            0xa8e32ab063d17473l },
          { 0x33f55bd5543199aal,0x79d2c937a2071d6el,0xa6a6758ceff16f28l,
            0x9c5f93ef87d85201l },
          0 },
        /* 34 << 48 */
        { { 0x7f2e440381e9ede3l,0x243c3894caf6df0al,0x7c605bb11c073b11l,
            0xcd06a541ba6a4a62l },
          { 0x2916894949d4e2e5l,0x33649d074af66880l,0xbfc0c885e9a85035l,
            0xb4e52113fc410f4bl },
          0 },
        /* 35 << 48 */
        { { 0xe86f21bc3ad4c81el,0x53b408403a37dcebl,0xaa606087383402cdl,
            0xc248caf185452b1dl },
          { 0x38853772576b57cdl,0xe2798e5441b7a6edl,0x7c2f1eed95ef4a33l,
            0xccd7e776adb1873cl },
          0 },
        /* 36 << 48 */
        { { 0xdca3b70678a6513bl,0x92ea4a2a9edb1943l,0x02642216db6e2dd8l,
            0x9b45d0b49fd57894l },
          { 0x114e70dbc69d11ael,0x1477dd194c57595fl,0xbc2208b4ec77c272l,
            0x95c5b4d7db68f59cl },
          0 },
        /* 37 << 48 */
        { { 0xd978bb791c61030al,0xa47325d2218222f3l,0x65ad4d4832e67d97l,
            0x31e4ed632e0d162al },
          { 0x7308ea317f76da37l,0xcfdffe87d93f35d8l,0xf4b2d60ee6f96cc4l,
            0x8028f3bd0117c421l },
          0 },
        /* 39 << 48 */
        { { 0x7df80cbb9543edb6l,0xa07a54df40b0b3bcl,0xacbd067cc1888488l,
            0x61ad61318a00c721l },
          { 0x67e7599ebe2e6fe6l,0x8349d568f7270e06l,0x5630aabc307bc0c7l,
            0x97210b3f71af442fl },
          0 },
        /* 40 << 48 */
        { { 0xfe541fa47ea67c77l,0x952bd2afe3ea810cl,0x791fef568d01d374l,
            0xa3a1c6210f11336el },
          { 0x5ad0d5a9c7ec6d79l,0xff7038af3225c342l,0x003c6689bc69601bl,
            0x25059bc745e8747dl },
          0 },
        /* 41 << 48 */
        { { 0x58bdabb7ef701b5fl,0x64f987aee00c3a96l,0x533b391e2d585679l,
            0x30ad79d97a862e03l },
          { 0xd941471e8177b261l,0x33f65cb856a9018el,0x985ce9f607759fc4l,
            0x9b085f33aefdbd9el },
          0 },
        /* 43 << 48 */
        { { 0xab2fa51a9c43ee15l,0x457f338263f30575l,0xce8dcd863e75a6e0l,
            0x67a03ab86e70421al },
          { 0xe72c37893e174230l,0x45ffff6c066f4816l,0x3a3dd84879a2d4a7l,
            0xefa4b7e68b76c24cl },
          0 },
        /* 44 << 48 */
        { { 0x9a75c80676cb2566l,0x8f76acb1b24892d9l,0x7ae7b9cc1f08fe45l,
            0x19ef73296a4907d8l },
          { 0x2db4ab715f228bf0l,0xf3cdea39817032d7l,0x0b1f482edcabe3c0l,
            0x3baf76b4bb86325cl },
          0 },
        /* 45 << 48 */
        { { 0xd6be8f00e39e056al,0xb58f87a6232fa3bcl,0xd5cb09dc6b18c772l,
            0x3177256da8e7e17bl },
          { 0x1877fd34230bf92cl,0x6f9031175a36f632l,0x526a288728e2c9d9l,
            0xc373fc94415ec45cl },
          0 },
        /* 46 << 48 */
        { { 0xd49065e010089465l,0x3bab5d298e77c596l,0x7636c3a6193dbd95l,
            0xdef5d294b246e499l },
          { 0xb22c58b9286b2475l,0xa0b93939cd80862bl,0x3002c83af0992388l,
            0x6de01f9beacbe14cl },
          0 },
        /* 47 << 48 */
        { { 0x70fa6e2a2bf5e373l,0x501691739271694cl,0xd6ebb98c5d2ed9f1l,
            0x11fd0b3f225bf92dl },
          { 0x51ffbcea1e3d5520l,0xa7c549875513ad47l,0xe9689750b431d46dl,
            0x6e69fecbb620cb9al },
          0 },
        /* 48 << 48 */
        { { 0x6aac688eadd70482l,0x708de92a7b4a4e8al,0x75b6dd73758a6eefl,
            0xea4bf352725b3c43l },
          { 0x10041f2c87912868l,0xb1b1be95ef09297al,0x19ae23c5a9f3860al,
            0xc4f0f839515dcf4bl },
          0 },
        /* 49 << 48 */
        { { 0xf3c22398e04b5734l,0x4fba59b275f2579dl,0xbf95182d691901b3l,
            0x4c139534eb599496l },
          { 0xf3f821de33b77e8bl,0x66e580743785d42fl,0xe3ba3d5abdc89c2dl,
            0x7ee988bdd19f37b9l },
          0 },
        /* 51 << 48 */
        { { 0xe9ba62ca2ee53eb0l,0x64295ae23401d7dal,0x70ed8be24e493580l,
            0x702caa624502732fl },
          { 0xb1f4e21278d0cedfl,0x130b114bdc97057bl,0x9c5d0bd3c38c77b5l,
            0xd9d641e18bad68e7l },
          0 },
        /* 52 << 48 */
        { { 0xc71e27bf8538a5c6l,0x195c63dd89abff17l,0xfd3152851b71e3dal,
            0x9cbdfda7fa680fa0l },
          { 0x9db876ca849d7eabl,0xebe2764b3c273271l,0x663357e3f208dceal,
            0x8c5bd833565b1b70l },
          0 },
        /* 53 << 48 */
        { { 0x7c2dea1d122aebd4l,0x090bee4a138c1e4dl,0x94a9ffe59e4aca6cl,
            0x8f3212ba5d405c7fl },
          { 0x6618185f180b5e85l,0x76298d46f455ab9fl,0x0c804076476b2d88l,
            0x45ea9d03d5a40b39l },
          0 },
        /* 55 << 48 */
        { { 0xdf325ac76a2ed772l,0x35da47ccb0da2765l,0x94ce6f460bc9b166l,
            0xe0fc82fb5f7f3628l },
          { 0x2b26d588c055f576l,0xb9d37c97ec2bae98l,0xffbbead856908806l,
            0xa8c2df87437f4c84l },
          0 },
        /* 57 << 48 */
        { { 0x47d11c3528430994l,0x0183df71cf13d9d3l,0x98604c89aa138fe5l,
            0xb1432e1c32c09aa1l },
          { 0xf19bc45d99bd5e34l,0xb198be72108e9b89l,0xee500ae9dacde648l,
            0x5936cf98746870a9l },
          0 },
        /* 59 << 48 */
        { { 0x6d8efb98ed1d5a9bl,0x2e0b08e697f778fal,0xda728454dc5e0835l,
            0x2c28a45f8e3651c4l },
          { 0x667fab6f7ee77088l,0xd94429c8f29a94b4l,0xd83d594d9deea5b2l,
            0x2dc08ccbbea58080l },
          0 },
        /* 60 << 48 */
        { { 0xba5514df3fd165e8l,0x499fd6a9061f8811l,0x72cd1fe0bfef9f00l,
            0x120a4bb979ad7e8al },
          { 0xf2ffd0955f4a5ac5l,0xcfd174f195a7a2f0l,0xd42301ba9d17baf1l,
            0xd2fa487a77f22089l },
          0 },
        /* 61 << 48 */
        { { 0xfb5f53ba20a9a01el,0x3adb174fd20d6a9cl,0x6db8bb6d80e0f64fl,
            0x596e428df6a26f76l },
          { 0xbab1f846e6a4e362l,0x8bdb22af9b1becbdl,0x62b48335f31352adl,
            0xd72c26409634f727l },
          0 },
        /* 63 << 48 */
        { { 0xaaa61cb22b1ec1c3l,0x3b5156722cb6f00el,0x67d1be0a8bf83f60l,
            0x88f1627aa4b804bcl },
          { 0xc52b11a7cdade2abl,0xa6a8b71a606a4e9dl,0x04e0e6697b900551l,
            0x35cfa33c8d5ad0d2l },
          0 },
        /* 64 << 48 */
        { { 0xb93452381d531696l,0x57201c0088cdde69l,0xdde922519a86afc7l,
            0xe3043895bd35cea8l },
          { 0x7608c1e18555970dl,0x8267dfa92535935el,0xd4c60a57322ea38bl,
            0xe0bf7977804ef8b5l },
          0 },
        /* 65 << 48 */
        { { 0x375ca189b60f0d5al,0xc9458cf949a78362l,0x61c1c5024262c03al,
            0x299353db4363d5bel },
          { 0xe3565124dac407fel,0x16ea66cd5b93c532l,0xe5c6aec2749df8e3l,
            0x59181317ce3ee4bfl },
          0 },
        /* 71 << 48 */
        { { 0xd46ea34af41c2a3cl,0x9936184916545c98l,0xd7cb800ccf2498b4l,
            0xe71d088d9353fe87l },
          { 0x43443cbeae2e172cl,0x77131656ca905cb3l,0x76471fd1dce63594l,
            0x346b1d1738f5e264l },
          0 },
        /* 77 << 48 */
        { { 0x22b1e639f6d0a419l,0x8bbb1fad7cea278cl,0xf07f6c01370cc86al,
            0x661bd027d39b837fl },
          { 0x042c7a69de606098l,0x93433b154e44eb12l,0x20f44ada88d8bfe8l,
            0xb44f66e64ccbfab6l },
          0 },
        /* 83 << 48 */
        { { 0x1cc32158583d9745l,0x9306223cad1c2201l,0x76aa8d0995748039l,
            0x29425391707e9b59l },
          { 0x8501c0d4487cdf9el,0xbe08e89c205c5611l,0xa950400b04ccc48bl,
            0xb614b69b637e966bl },
          0 },
        /* 89 << 48 */
        { { 0xd9c3c1238ffa5c4bl,0xc65765f7f3593988l,0x9a7e5d2728242119l,
            0x0ad27b5097ad7620l },
          { 0x154cc5eb413a8b23l,0xae93d8de7afa8254l,0x9ce5116cab9907b5l,
            0x9a163d78063103b9l },
          0 },
        /* 95 << 48 */
        { { 0x5c4c299291086d2al,0x42c6ca9de8e2d951l,0xe67ecf93dd353f30l,
            0xba54557fe7167c2el },
          { 0x04a7eb2db734c779l,0x8f345605e300711al,0x4811c1ad67b27de6l,
            0xb7ac8e842731d5f0l },
          0 },
        /* 101 << 48 */
        { { 0xee33a1d8e449ac46l,0x2500ba0aaaebfa2dl,0x8fb914ebc424eff4l,
            0x3a36545d3989255el },
          { 0xd24f2484761235e6l,0x2fc5d5ddd9b2c04bl,0x73660f86070ab0dbl,
            0x2e266d0479d20c7bl },
          0 },
        /* 107 << 48 */
        { { 0x143752d5316d19a3l,0x56a55e01915497b8l,0x44ba4b2609a5fd15l,
            0xe4fc3e7fd9bee4eel },
          { 0x6f9d8609878a9f26l,0xdf36b5bd2ede7a20l,0x8e03e712a9a3e435l,
            0x4ced555b56546d33l },
          0 },
        /* 113 << 48 */
        { { 0x89a6aaab0882717el,0x56a9736b43fa5153l,0xdb07dcc9d0e1fb1al,
            0xe7c986d34145e227l },
          { 0x57be66abb10dad51l,0xa47b964e4aa01ea7l,0xd851d9f36bb837cbl,
            0x9851ab3d652e13f7l },
          0 },
        /* 116 << 48 */
        { { 0x22b88a805616ee30l,0xfb09548fe7ab1083l,0x8ad6ab0d511270cdl,
            0x61f6c57a6924d9abl },
          { 0xa0f7bf7290aecb08l,0x849f87c90df784a4l,0x27c79c15cfaf1d03l,
            0xbbf9f675c463facel },
          0 },
        /* 119 << 48 */
        { { 0x65512fb716dd6ce1l,0xfa76ebc960d53b35l,0x31e5322e19ada3bel,
            0x7e259b75d0ccc3cdl },
          { 0xd36d03f0e025fd69l,0xbefab782eea9e5f3l,0x1569969dd09ce6a7l,
            0x2df5396178c385b0l },
          0 },
        /* 125 << 48 */
        { { 0x4201652fce0ccac7l,0x12f8e93df1d29d2dl,0x6c2ac9b2220f00c1l,
            0x4ee6a685a850baa9l },
          { 0x2c2371f163ee8829l,0xddff16488f464433l,0xeab6cd8869a2c413l,
            0xcae34beb85e4c2a8l },
          0 },
    },
    {
        /* 0 << 56 */
        { { 0x00, 0x00, 0x00, 0x00 },
          { 0x00, 0x00, 0x00, 0x00 },
          1 },
        /* 1 << 56 */
        { { 0xc7913e91991724f3l,0x5eda799c39cbd686l,0xddb595c763d4fc1el,
            0x6b63b80bac4fed54l },
          { 0x6ea0fc697e5fb516l,0x737708bad0f1c964l,0x9628745f11a92ca5l,
            0x61f379589a86967al },
          0 },
        /* 3 << 56 */
        { { 0x46a8c4180d738dedl,0x6f1a5bb0e0de5729l,0xf10230b98ba81675l,
            0x32c6f30c112b33d4l },
          { 0x7559129dd8fffb62l,0x6a281b47b459bf05l,0x77c1bd3afa3b6776l,
            0x0709b3807829973al },
          0 },
        /* 4 << 56 */
        { { 0x8c26b232a3326505l,0x38d69272ee1d41bfl,0x0459453effe32afal,
            0xce8143ad7cb3ea87l },
          { 0x932ec1fa7e6ab666l,0x6cd2d23022286264l,0x459a46fe6736f8edl,
            0x50bf0d009eca85bbl },
          0 },
        /* 5 << 56 */
        { { 0x0b825852877a21ecl,0x300414a70f537a94l,0x3f1cba4021a9a6a2l,
            0x50824eee76943c00l },
          { 0xa0dbfcecf83cba5dl,0xf953814893b4f3c0l,0x6174416248f24dd7l,
            0x5322d64de4fb09ddl },
          0 },
        /* 7 << 56 */
        { { 0xa337c447f1f0ced1l,0x800cc7939492dd2bl,0x4b93151dbea08efal,
            0x820cf3f8de0a741el },
          { 0xff1982dc1c0f7d13l,0xef92196084dde6cal,0x1ad7d97245f96ee3l,
            0x319c8dbe29dea0c7l },
          0 },
        /* 9 << 56 */
        { { 0x0ae1d63b0eb919b0l,0xd74ee51da74b9620l,0x395458d0a674290cl,
            0x324c930f4620a510l },
          { 0x2d1f4d19fbac27d4l,0x4086e8ca9bedeeacl,0x0cdd211b9b679ab8l,
            0x5970167d7090fec4l },
          0 },
        /* 10 << 56 */
        { { 0x3420f2c9faf1fc63l,0x616d333a328c8bb4l,0x7d65364c57f1fe4al,
            0x9343e87755e5c73al },
          { 0x5795176be970e78cl,0xa36ccebf60533627l,0xfc7c738009cdfc1bl,
            0xb39a2afeb3fec326l },
          0 },
        /* 11 << 56 */
        { { 0xb7ff1ba16224408al,0xcc856e92247cfc5el,0x01f102e7c18bc493l,
            0x4613ab742091c727l },
          { 0xaa25e89cc420bf2bl,0x00a5317690337ec2l,0xd2be9f437d025fc7l,
            0x3316fb856e6fe3dcl },
          0 },
        /* 13 << 56 */
        { { 0x67332cfc2064cfd1l,0x339c31deb0651934l,0x719b28d52a3bcbeal,
            0xee74c82b9d6ae5c6l },
          { 0x0927d05ebaf28ee6l,0x82cecf2c9d719028l,0x0b0d353eddb30289l,
            0xfe4bb977fddb2e29l },
          0 },
        /* 15 << 56 */
        { { 0xe10b2ab817a91cael,0xb89aab6508e27f63l,0x7b3074a7dba3ddf9l,
            0x1c20ce09330c2972l },
          { 0x6b9917b45fcf7e33l,0xe6793743945ceb42l,0x18fc22155c633d19l,
            0xad1adb3cc7485474l },
          0 },
        /* 16 << 56 */
        { { 0x646f96796424c49bl,0xf888dfe867c241c9l,0xe12d4b9324f68b49l,
            0x9a6b62d8a571df20l },
          { 0x81b4b26d179483cbl,0x666f96329511fae2l,0xd281b3e4d53aa51fl,
            0x7f96a7657f3dbd16l },
          0 },
        /* 17 << 56 */
        { { 0xa7f8b5bf074a30cel,0xd7f52107005a32e6l,0x6f9e090750237ed4l,
            0x2f21da478096fa2bl },
          { 0xf3e19cb4eec863a0l,0xd18f77fd9527620al,0x9505c81c407c1cf8l,
            0x9998db4e1b6ec284l },
          0 },
        /* 19 << 56 */
        { { 0x794e2d5984ac066cl,0xf5954a92e68c69a0l,0x28c524584fd99dccl,
            0x60e639fcb1012517l },
          { 0xc2e601257de79248l,0xe9ef6404f12fc6d7l,0x4c4f28082a3b5d32l,
            0x865ad32ec768eb8al },
          0 },
        /* 21 << 56 */
        { { 0x4f4ddf91b2f1ac7al,0xf99eaabb760fee27l,0x57f4008a49c228e5l,
            0x090be4401cf713bbl },
          { 0xac91fbe45004f022l,0xd838c2c2569e1af6l,0xd6c7d20b0f1daaa5l,
            0xaa063ac11bbb02c0l },
          0 },
        /* 23 << 56 */
        { { 0x54935fcb81d73c9el,0x6d07e9790a5e97abl,0x4dc7b30acf3a6babl,
            0x147ab1f3170bee11l },
          { 0x0aaf8e3d9fafdee4l,0xfab3dbcb538a8b95l,0x405df4b36ef13871l,
            0xf1f4e9cb088d5a49l },
          0 },
        /* 25 << 56 */
        { { 0x43c01b87459afccdl,0x6bd45143b7432652l,0x8473453055b5d78el,
            0x81088fdb1554ba7dl },
          { 0xada0a52c1e269375l,0xf9f037c42dc5ec10l,0xc066060794bfbc11l,
            0xc0a630bbc9c40d2fl },
          0 },
        /* 27 << 56 */
        { { 0x9a730ed44763eb50l,0x24a0e221c1ab0d66l,0x643b6393648748f3l,
            0x1982daa16d3c6291l },
          { 0x6f00a9f78bbc5549l,0x7a1783e17f36384el,0xe8346323de977f50l,
            0x91ab688db245502al },
          0 },
        /* 28 << 56 */
        { { 0x331ab6b56d0bdd66l,0x0a6ef32e64b71229l,0x1028150efe7c352fl,
            0x27e04350ce7b39d3l },
          { 0x2a3c8acdc1070c82l,0xfb2034d380c9feefl,0x2d729621709f3729l,
            0x8df290bf62cb4549l },
          0 },
        /* 29 << 56 */
        { { 0x02f99f33fc2e4326l,0x3b30076d5eddf032l,0xbb21f8cf0c652fb5l,
            0x314fb49eed91cf7bl },
          { 0xa013eca52f700750l,0x2b9e3c23712a4575l,0xe5355557af30fbb0l,
            0x1ada35167c77e771l },
          0 },
        /* 31 << 56 */
        { { 0xdc9f46fc609e4a74l,0x2a44a143ba667f91l,0xbc3d8b95b4d83436l,
            0xa01e4bd0c7bd2958l },
          { 0x7b18293273483c90l,0xa79c6aa1a7c7b598l,0xbf3983c6eaaac07el,
            0x8f18181e96e0d4e6l },
          0 },
        /* 33 << 56 */
        { { 0x0bfc27eeacee5043l,0xae419e732eb10f02l,0x19c028d18943fb05l,
            0x71f01cf7ff13aa2al },
          { 0x7790737e8887a132l,0x6751330966318410l,0x9819e8a37ddb795el,
            0xfecb8ef5dad100b2l },
          0 },
        /* 34 << 56 */
        { { 0x59f74a223021926al,0xb7c28a496f9b4c1cl,0xed1a733f912ad0abl,
            0x42a910af01a5659cl },
          { 0x3842c6e07bd68cabl,0x2b57fa3876d70ac8l,0x8a6707a83c53aaebl,
            0x62c1c51065b4db18l },
          0 },
        /* 35 << 56 */
        { { 0x8de2c1fbb2d09dc7l,0xc3dfed12266bd23bl,0x927d039bd5b27db6l,
            0x2fb2f0f1103243dal },
          { 0xf855a07b80be7399l,0xed9327ce1f9f27a8l,0xa0bd99c7729bdef7l,
            0x2b67125e28250d88l },
          0 },
        /* 36 << 56 */
        { { 0x784b26e88670ced7l,0xe3dfe41fc31bd3b4l,0x9e353a06bcc85cbcl,
            0x302e290960178a9dl },
          { 0x860abf11a6eac16el,0x76447000aa2b3aacl,0x46ff9d19850afdabl,
            0x35bdd6a5fdb2d4c1l },
          0 },
        /* 37 << 56 */
        { { 0xe82594b07e5c9ce9l,0x0f379e5320af346el,0x608b31e3bc65ad4al,
            0x710c6b12267c4826l },
          { 0x51c966f971954cf1l,0xb1cec7930d0aa215l,0x1f15598986bd23a8l,
            0xae2ff99cf9452e86l },
          0 },
        /* 39 << 56 */
        { { 0xb5a741a76b2515cfl,0x71c416019585c749l,0x78350d4fe683de97l,
            0x31d6152463d0b5f5l },
          { 0x7a0cc5e1fbce090bl,0xaac927edfbcb2a5bl,0xe920de4920d84c35l,
            0x8c06a0b622b4de26l },
          0 },
        /* 40 << 56 */
        { { 0xd34dd58bafe7ddf3l,0x55851fedc1e6e55bl,0xd1395616960696e7l,
            0x940304b25f22705fl },
          { 0x6f43f861b0a2a860l,0xcf1212820e7cc981l,0x121862120ab64a96l,
            0x09215b9ab789383cl },
          0 },
        /* 41 << 56 */
        { { 0x311eb30537387c09l,0xc5832fcef03ee760l,0x30358f5832f7ea19l,
            0xe01d3c3491d53551l },
          { 0x1ca5ee41da48ea80l,0x34e71e8ecf4fa4c1l,0x312abd257af1e1c7l,
            0xe3afcdeb2153f4a5l },
          0 },
        /* 43 << 56 */
        { { 0x2a17747fa6d74081l,0x60ea4c0555a26214l,0x53514bb41f88c5fel,
            0xedd645677e83426cl },
          { 0xd5d6cbec96460b25l,0xa12fd0ce68dc115el,0xc5bc3ed2697840eal,
            0x969876a8a6331e31l },
          0 },
        /* 44 << 56 */
        { { 0x60c36217472ff580l,0xf42297054ad41393l,0x4bd99ef0a03b8b92l,
            0x501c7317c144f4f6l },
          { 0x159009b318464945l,0x6d5e594c74c5c6bel,0x2d587011321a3660l,
            0xd1e184b13898d022l },
          0 },
        /* 45 << 56 */
        { { 0x5ba047524c6a7e04l,0x47fa1e2b45550b65l,0x9419daf048c0a9a5l,
            0x663629537c243236l },
          { 0xcd0744b15cb12a88l,0x561b6f9a2b646188l,0x599415a566c2c0c0l,
            0xbe3f08590f83f09al },
          0 },
        /* 46 << 56 */
        { { 0x9141c5beb92041b8l,0x01ae38c726477d0dl,0xca8b71f3d12c7a94l,
            0xfab5b31f765c70dbl },
          { 0x76ae7492487443e9l,0x8595a310990d1349l,0xf8dbeda87d460a37l,
            0x7f7ad0821e45a38fl },
          0 },
        /* 47 << 56 */
        { { 0xed1d4db61059705al,0xa3dd492ae6b9c697l,0x4b92ee3a6eb38bd5l,
            0xbab2609d67cc0bb7l },
          { 0x7fc4fe896e70ee82l,0xeff2c56e13e6b7e3l,0x9b18959e34d26fcal,
            0x2517ab66889d6b45l },
          0 },
        /* 48 << 56 */
        { { 0xf167b4e0bdefdd4fl,0x69958465f366e401l,0x5aa368aba73bbec0l,
            0x121487097b240c21l },
          { 0x378c323318969006l,0xcb4d73cee1fe53d1l,0x5f50a80e130c4361l,
            0xd67f59517ef5212bl },
          0 },
        /* 49 << 56 */
        { { 0xf145e21e9e70c72el,0xb2e52e295566d2fbl,0x44eaba4a032397f5l,
            0x5e56937b7e31a7del },
          { 0x68dcf517456c61e1l,0xbc2e954aa8b0a388l,0xe3552fa760a8b755l,
            0x03442dae73ad0cdel },
          0 },
        /* 51 << 56 */
        { { 0x3fcbdbce478e2135l,0x7547b5cfbda35342l,0xa97e81f18a677af6l,
            0xc8c2bf8328817987l },
          { 0xdf07eaaf45580985l,0xc68d1f05c93b45cbl,0x106aa2fec77b4cacl,
            0x4c1d8afc04a7ae86l },
          0 },
        /* 52 << 56 */
        { { 0xdb41c3fd9eb45ab2l,0x5b234b5bd4b22e74l,0xda253decf215958al,
            0x67e0606ea04edfa0l },
          { 0xabbbf070ef751b11l,0xf352f175f6f06dcel,0xdfc4b6af6839f6b4l,
            0x53ddf9a89959848el },
          0 },
        /* 53 << 56 */
        { { 0xda49c379c21520b0l,0x90864ff0dbd5d1b6l,0x2f055d235f49c7f7l,
            0xe51e4e6aa796b2d8l },
          { 0xc361a67f5c9dc340l,0x5ad53c37bca7c620l,0xda1d658832c756d0l,
            0xad60d9118bb67e13l },
          0 },
        /* 55 << 56 */
        { { 0xd1183316fd6f7140l,0xf9fadb5bbd8e81f7l,0x701d5e0c5a02d962l,
            0xfdee4dbf1b601324l },
          { 0xbed1740735d7620el,0x04e3c2c3f48c0012l,0x9ee29da73455449al,
            0x562cdef491a836c4l },
          0 },
        /* 57 << 56 */
        { { 0x147ebf01fad097a5l,0x49883ea8610e815dl,0xe44d60ba8a11de56l,
            0xa970de6e827a7a6dl },
          { 0x2be414245e17fc19l,0xd833c65701214057l,0x1375813b363e723fl,
            0x6820bb88e6a52e9bl },
          0 },
        /* 59 << 56 */
        { { 0xe1b6f60c08191224l,0xc4126ebbde4ec091l,0xe1dff4dc4ae38d84l,
            0xde3f57db4f2ef985l },
          { 0x34964337d446a1ddl,0x7bf217a0859e77f6l,0x8ff105278e1d13f5l,
            0xa304ef0374eeae27l },
          0 },
        /* 60 << 56 */
        { { 0xfc6f5e47d19dfa5al,0xdb007de37fad982bl,0x28205ad1613715f5l,
            0x251e67297889529el },
          { 0x727051841ae98e78l,0xf818537d271cac32l,0xc8a15b7eb7f410f5l,
            0xc474356f81f62393l },
          0 },
        /* 61 << 56 */
        { { 0x92dbdc5ac242316bl,0xabe060acdbf4aff5l,0x6e8c38fe909a8ec6l,
            0x43e514e56116cb94l },
          { 0x2078fa3807d784f9l,0x1161a880f4b5b357l,0x5283ce7913adea3dl,
            0x0756c3e6cc6a910bl },
          0 },
        /* 63 << 56 */
        { { 0xa573a4966d17fbc7l,0x0cd1a70a73d2b24el,0x34e2c5cab2676937l,
            0xe7050b06bf669f21l },
          { 0xfbe948b61ede9046l,0xa053005197662659l,0x58cbd4edf10124c5l,
            0xde2646e4dd6c06c8l },
          0 },
        /* 64 << 56 */
        { { 0x332f81088cad38c0l,0x471b7e906bd68ae2l,0x56ac3fb20d8e27a3l,
            0xb54660db136b4b0dl },
          { 0x123a1e11a6fd8de4l,0x44dbffeaa37799efl,0x4540b977ce6ac17cl,
            0x495173a8af60acefl },
          0 },
        /* 65 << 56 */
        { { 0xc48b1478db447d0bl,0xe1b85f5d46104fbbl,0x4ab31e7d991c60b9l,
            0xaa674a9258a0cfd0l },
          { 0x179fc2cd316f4297l,0x90c18642dcccbc82l,0x65d4309e56a4c163l,
            0xf211a9c7145a33ecl },
          0 },
        /* 71 << 56 */
        { { 0x9669170cdc32717fl,0x52d69b5138133e34l,0xaed24e5fb079c3b2l,
            0xaba44a91a21ea3d2l },
          { 0xd6814f1938d40105l,0x38289fe463462e7al,0x1793eefa3a80cbf5l,
            0x05816a0795f29bacl },
          0 },
        /* 77 << 56 */
        { { 0xdca88ad98f850641l,0x8c1152c447999b0dl,0x509f654e654aff33l,
            0x2228550f08a12f14l },
          { 0x60fe99dbb6a0ccdbl,0x80d6829bfc2cddccl,0x190f454dd5617aa4l,
            0x0aea05fe36295d2dl },
          0 },
        /* 83 << 56 */
        { { 0x1de06c8af9bef9a5l,0xe24d85d3fb2d3164l,0x3dbe455e8d203d3el,
            0x439bee4735ea47a9l },
          { 0xcc143432784893d7l,0x9b71073bd9bebd00l,0x6c106b343aa2fe88l,
            0x9df2a42734746f7al },
          0 },
        /* 89 << 56 */
        { { 0x1ad0b3725a8c2168l,0x64e52d6d143f0402l,0xd933c783e320f31fl,
            0x1ccf90a80ff14f52l },
          { 0xd3a3133ee1e6d0c0l,0xfd75a2d5b4acc8cal,0x62659b8e5559d171l,
            0x5087d6e9f13ad52al },
          0 },
        /* 95 << 56 */
        { { 0xb4d647a5deef31a4l,0x95bf4ab180975ea9l,0x2f92d15adf57b03el,
            0x5ee808ab746b26d6l },
          { 0x4341597c1082f261l,0x027795eb40c45e95l,0xcb77744b3b690c30l,
            0xdd87c084af3f88d1l },
          0 },
        /* 101 << 56 */
        { { 0x469f177572109785l,0xf365e55123f84d6cl,0x8006a9c28a046dbbl,
            0x1b9fbe892fa09f52l },
          { 0xac18a88016075e9el,0x4a3069bc1e3fd628l,0x20c61eaa60c61c14l,
            0x315b59daf61f004bl },
          0 },
        /* 107 << 56 */
        { { 0x0a94387f26d04857l,0x952a4ebc43d6de95l,0xb422e15cf14abdfal,
            0x5b7a0153324ef90cl },
          { 0x6aefa20e9826ec5bl,0x0e529886ad2fe161l,0xb710a74ec0d416e8l,
            0x6cf4b0a5fb6c90bcl },
          0 },
        /* 113 << 56 */
        { { 0x822aea4031979d3bl,0xb504eafde215a109l,0xa8761ead84bf2377l,
            0xb55c1e55efb3d942l },
          { 0xd01f9b0212b7f17bl,0x41b62c2a891bfbbfl,0x50800e6b08938149l,
            0x527b50a9b0a55d82l },
          0 },
        /* 116 << 56 */
        { { 0x6bc84d8d1d9ce3c4l,0x53b465072a308df0l,0x6c3da9bfca79c88al,
            0x9636ad9c36372acfl },
          { 0x8840e92c425ef14cl,0x863191f96af3225bl,0xd56d82d0d369b857l,
            0x2053a2527a4c41f9l },
          0 },
        /* 119 << 56 */
        { { 0x20aecd6609ca8805l,0x945d9b31dc818ee6l,0x1424647c2119b44bl,
            0xbe934d7e5a6641f9l },
          { 0xe91d53184559e55el,0xc2fb8e0b4dfbc3d4l,0x9e92e20676cb937fl,
            0x0f5582e4f2932429l },
          0 },
        /* 125 << 56 */
        { { 0xb5fc22a42d31809fl,0x6d582d2b0e35b7b4l,0x5fac415158c5f576l,
            0xdff239371e4cd7c9l },
          { 0x0f62b329ed4d1925l,0x00994a2e6010fb16l,0xb4b91076bd754837l,
            0xfde219463345103al },
          0 },
    },
    {
        /* 0 << 64 */
        { { 0x00, 0x00, 0x00, 0x00 },
          { 0x00, 0x00, 0x00, 0x00 },
          1 },
        /* 1 << 64 */
        { { 0x4f922fc516a0d2bbl,0x0d5cc16c1a623499l,0x9241cf3a57c62c8bl,
            0x2f5e6961fd1b667fl },
          { 0x5c15c70bf5a01797l,0x3d20b44d60956192l,0x04911b37071fdb52l,
            0xf648f9168d6f0f7bl },
          0 },
        /* 3 << 64 */
        { { 0x4090914bb5def996l,0x1cb69c83233dd1e7l,0xc1e9c1d39b3d5e76l,
            0x1f3338edfccf6012l },
          { 0xb1e95d0d2f5378a8l,0xacf4c2c72f00cd21l,0x6e984240eb5fe290l,
            0xd66c038d248088ael },
          0 },
        /* 4 << 64 */
        { { 0x9ad5462bb4d8bc50l,0x181c0b16a9195770l,0xebd4fe1c78412a68l,
            0xae0341bcc0dff48cl },
          { 0xb6bc45cf7003e866l,0xf11a6dea8a24a41bl,0x5407151ad04c24c2l,
            0x62c9d27dda5b7b68l },
          0 },
        /* 5 << 64 */
        { { 0xd4992b30614c0900l,0xda98d121bd00c24bl,0x7f534dc87ec4bfa1l,
            0x4a5ff67437dc34bcl },
          { 0x68c196b81d7ea1d7l,0x38cf289380a6d208l,0xfd56cd09e3cbbd6el,
            0xec72e27e4205a5b6l },
          0 },
        /* 7 << 64 */
        { { 0xe8b97932b88756ddl,0xed4e8652f17e3e61l,0xc2dd14993ee1c4a4l,
            0xc0aaee17597f8c0el },
          { 0x15c4edb96c168af3l,0x6563c7bfb39ae875l,0xadfadb6f20adb436l,
            0xad55e8c99a042ac0l },
          0 },
        /* 9 << 64 */
        { { 0x65c29219909523c8l,0xa62f648fa3a1c741l,0x88598d4f60c9e55al,
            0xbce9141b0e4f347al },
          { 0x9af97d8435f9b988l,0x0210da62320475b6l,0x3c076e229191476cl,
            0x7520dbd944fc7834l },
          0 },
        /* 10 << 64 */
        { { 0x87a7ebd1e0a1b12al,0x1e4ef88d770ba95fl,0x8c33345cdc2ae9cbl,
            0xcecf127601cc8403l },
          { 0x687c012e1b39b80fl,0xfd90d0ad35c33ba4l,0xa3ef5a675c9661c2l,
            0x368fc88ee017429el },
          0 },
        /* 11 << 64 */
        { { 0x664300b07850ec06l,0xac5a38b97d3a10cfl,0x9233188de34ab39dl,
            0xe77057e45072cbb9l },
          { 0xbcf0c042b59e78dfl,0x4cfc91e81d97de52l,0x4661a26c3ee0ca4al,
            0x5620a4c1fb8507bcl },
          0 },
        /* 13 << 64 */
        { { 0x84b9ca1504b6c5a0l,0x35216f3918f0e3a3l,0x3ec2d2bcbd986c00l,
            0x8bf546d9d19228fel },
          { 0xd1c655a44cd623c3l,0x366ce718502b8e5al,0x2cfc84b4eea0bfe7l,
            0xe01d5ceecf443e8el },
          0 },
        /* 15 << 64 */
        { { 0xa75feacabe063f64l,0x9b392f43bce47a09l,0xd42415091ad07acal,
            0x4b0c591b8d26cd0fl },
          { 0x2d42ddfd92f1169al,0x63aeb1ac4cbf2392l,0x1de9e8770691a2afl,
            0xebe79af7d98021dal },
          0 },
        /* 16 << 64 */
        { { 0x58af2010f5b343bcl,0x0f2e400af2f142fel,0x3483bfdea85f4bdfl,
            0xf0b1d09303bfeaa9l },
          { 0x2ea01b95c7081603l,0xe943e4c93dba1097l,0x47be92adb438f3a6l,
            0x00bb7742e5bf6636l },
          0 },
        /* 17 << 64 */
        { { 0x66917ce63b5f1cc4l,0x37ae52eace872e62l,0xbb087b722905f244l,
            0x120770861e6af74fl },
          { 0x4b644e491058edeal,0x827510e3b638ca1dl,0x8cf2b7046038591cl,
            0xffc8b47afe635063l },
          0 },
        /* 19 << 64 */
        { { 0x7677408d6dfafed3l,0x33a0165339661588l,0x3c9c15ec0b726fa0l,
            0x090cfd936c9b56dal },
          { 0xe34f4baea3c40af5l,0x3469eadbd21129f1l,0xcc51674a1e207ce8l,
            0x1e293b24c83b1ef9l },
          0 },
        /* 21 << 64 */
        { { 0x796d3a85825808bdl,0x51dc3cb73fd6e902l,0x643c768a916219d1l,
            0x36cd7685a2ad7d32l },
          { 0xe3db9d05b22922a4l,0x6494c87edba29660l,0xf0ac91dfbcd2ebc7l,
            0x4deb57a045107f8dl },
          0 },
        /* 23 << 64 */
        { { 0xb6c69ac82094cec3l,0x9976fb88403b770cl,0x1dea026c4859590dl,
            0xb6acbb468562d1fdl },
          { 0x7cd6c46144569d85l,0xc3190a3697f0891dl,0xc6f5319548d5a17dl,
            0x7d919966d749abc8l },
          0 },
        /* 25 << 64 */
        { { 0xb53b7de561906373l,0x858dbadeeb999595l,0x8cbb47b2a59e5c36l,
            0x660318b3dcf4e842l },
          { 0xbd161ccd12ba4b7al,0xf399daabf8c8282al,0x1587633aeeb2130dl,
            0xa465311ada38dd7dl },
          0 },
        /* 27 << 64 */
        { { 0x2dae9082be7cf3a6l,0xcc86ba92bc967274l,0xf28a2ce8aea0a8a9l,
            0x404ca6d96ee988b3l },
          { 0xfd7e9c5d005921b8l,0xf56297f144e79bf9l,0xa163b4600d75ddc2l,
            0x30b23616a1f2be87l },
          0 },
        /* 28 << 64 */
        { { 0x19e6125dec3f1decl,0x07b1f040911178dal,0xd93ededa904a6738l,
            0x55187a5a0bebedcdl },
          { 0xf7d04722eb329d41l,0xf449099ef170b391l,0xfd317a69ca99f828l,
            0x50c3db2b34a4976dl },
          0 },
        /* 29 << 64 */
        { { 0x0064d8585499fb32l,0x7b67bad977a8aeb7l,0x1d3eb9772d08eec5l,
            0x5fc047a6cbabae1dl },
          { 0x0577d159e54a64bbl,0x8862201bc43497e4l,0xad6b4e282ce0608dl,
            0x8b687b7d0b167aacl },
          0 },
        /* 31 << 64 */
        { { 0xe9f9669cda94951el,0x4b6af58d66b8d418l,0xfa32107417d426a4l,
            0xc78e66a99dde6027l },
          { 0x0516c0834a53b964l,0xfc659d38ff602330l,0x0ab55e5c58c5c897l,
            0x985099b2838bc5dfl },
          0 },
        /* 33 << 64 */
        { { 0xe7a935fa1684cb3bl,0x571650b5a7d7e69dl,0x6ba9ffa40328c168l,
            0xac43f6bc7e46f358l },
          { 0x54f75e567cb6a779l,0x4e4e2cc8c61320del,0xb94258bc2b8903d0l,
            0xc7f32d57ceecabe0l },
          0 },
        /* 34 << 64 */
        { { 0x34739f16cd7d9d89l,0x6daab4267ca080b5l,0x772086ff40e19f45l,
            0x43caa56118c61b42l },
          { 0x0ba3d4a8dbf365f1l,0xa0db435ee760ad97l,0xfd6f30d56916c59bl,
            0xab34cb5dafe12f5dl },
          0 },
        /* 35 << 64 */
        { { 0x445b86ea02a3260al,0x8c51d6428d689babl,0x183334d65588904cl,
            0xf8a3b84d479d6422l },
          { 0x581acfa0f0833d00l,0xc50827bc3b567d2dl,0x2c935e6daddcf73el,
            0x2a645f7704dd19f2l },
          0 },
        /* 36 << 64 */
        { { 0x78d2e8dfcb564473l,0x4349a97357d5621al,0x9d835d89218f8b24l,
            0x01fe7bc5079b6ee2l },
          { 0xe57f2a2b5b3b5dcel,0x5a8637b75fe55565l,0x83ff34aea41dbae7l,
            0xfce1199c950a7a8fl },
          0 },
        /* 37 << 64 */
        { { 0x0ca5d25bf8e71ce2l,0x204edc4a062685dal,0x06fe407d87678ec2l,
            0xd16936a07defa39al },
          { 0x3b108d84af3d16d0l,0xf2e9616d0305cad0l,0xbc9537e6f27bed97l,
            0x71c2d699ebc9f45cl },
          0 },
        /* 39 << 64 */
        { { 0x203bdd84cdcd3a85l,0x1107b901ade3ccfal,0xa7da89e95533159dl,
            0x8d834005860e8c64l },
          { 0x914bc0eb2a7638f7l,0xc66ce0a6620e8606l,0x11ef98c2e6c12dc0l,
            0x25666b1d7780fc0el },
          0 },
        /* 40 << 64 */
        { { 0x374f541f3e707706l,0x9a4d3638a831d0cfl,0x4ab4f4831518ca04l,
            0x54e3ee5dfe38c318l },
          { 0x383ae36403c8819bl,0xa9d1daa12e17864cl,0x245a97b350eeaa5bl,
            0x5362d00999bf4e83l },
          0 },
        /* 41 << 64 */
        { { 0x6667e89f4ded8a4fl,0xa59161abc36a7795l,0x1c96f6f9331ccf94l,
            0xf2727e879a686d49l },
          { 0x0f94894bb841295fl,0xb0fe8f744a0503d1l,0x60c581c7ef407926l,
            0x1980c8e13edb7e1cl },
          0 },
        /* 43 << 64 */
        { { 0x47948c84c5de1a41l,0xd595d14a48959688l,0x3bfca4be86ff21c9l,
            0xb5ff59b86a4191cal },
          { 0xced1dd1d65094c86l,0xd57b86559dc9d001l,0xbcac6fa3486e51d7l,
            0x8e97e2637b774c1bl },
          0 },
        /* 44 << 64 */
        { { 0xfc0313c29bd43980l,0x9c954b70f172db29l,0x679bdcb7f954a21al,
            0x6b48170954e2e4fcl },
          { 0x318af5f530baf1d0l,0x26ea8a3ccbf92060l,0xc3c69d7ccd5ae258l,
            0xa73ba0470ead07c9l },
          0 },
        /* 45 << 64 */
        { { 0xe82eb003e35dca85l,0xfd0000fa31e39180l,0xbca90f746735f378l,
            0xe6aa783158c943edl },
          { 0x0e94ecd5b6a438d7l,0xc02b60faf9a5f114l,0x4063568b8b1611ebl,
            0x1398bdc1272509ecl },
          0 },
        /* 46 << 64 */
        { { 0xc2ef6a01be3e92d1l,0x1bce9c27282bd5ddl,0xf7e488f3adda0568l,
            0xd4f15fdb1af9bb8bl },
          { 0x8c490ade4da846efl,0x76229da17f0b825el,0xc8b812082a6711c6l,
            0x511f5e23b4c523aal },
          0 },
        /* 47 << 64 */
        { { 0xbdf4e7049970f46el,0x70e220288dadbd1al,0x2b86c97fb1223d26l,
            0x042ad22ecf62f51al },
          { 0x72944339ba2ed2e9l,0x0ba0d10ef94fa61dl,0x3f86164194e68f15l,
            0x1312a74acb86c545l },
          0 },
        /* 48 << 64 */
        { { 0x3a63c39731815e69l,0x6df9cbd6dcdd2802l,0x4c47ed4a15b4f6afl,
            0x62009d826ac0f978l },
          { 0x664d80d28b898fc7l,0x72f1eeda2c17c91fl,0x9e84d3bc7aae6609l,
            0x58c7c19528376895l },
          0 },
        /* 49 << 64 */
        { { 0x640ebf5d5b8d354al,0xa5f3a8fdb396ff64l,0xd53f041d8378ed81l,
            0x1969d61bc1234ad2l },
          { 0x16d7acffeb68bde2l,0x63767a68f23e9368l,0x937a533c38928d95l,
            0xee2190bbbeb0f1f2l },
          0 },
        /* 51 << 64 */
        { { 0xb6860c9a73a4aafbl,0xb2f996290488870dl,0x16ef6232572d9e25l,
            0x5b9eb1bad1383389l },
          { 0xabf713a7ed8d77f8l,0xd2b4a2e9e2b69e64l,0xa1a22cfd6d6f17c2l,
            0x4bfd6f992d604511l },
          0 },
        /* 52 << 64 */
        { { 0xdcff7630d9294f07l,0x89b765d68dba8fd0l,0x553e55de8dbcaccdl,
            0x9b4a009eed702bf8l },
          { 0xf6e534dd27b8ca0dl,0xc4496b346177fd52l,0x378ce6f6c87bb7b7l,
            0x68633d4844cc19f0l },
          0 },
        /* 53 << 64 */
        { { 0xfe550021bc84c625l,0x8d7169986d45e4a3l,0xa09c6ded4c0c66b7l,
            0xe32313aeb9e1d547l },
          { 0x8ce775b4d1e8e0b9l,0xa899f9102654dd15l,0x7c38aa066cc8b2a9l,
            0xe6ebb291d6ce6cc0l },
          0 },
        /* 55 << 64 */
        { { 0x5963df62a6991216l,0x4c17f72246996010l,0x131dc2b840477722l,
            0x78bf50b0d1765a75l },
          { 0x360afd587ceaca12l,0xebc55dbb139cd470l,0x9083e27e4c05541cl,
            0xc10057a3b873d757l },
          0 },
        /* 57 << 64 */
        { { 0x440009c3deed7769l,0xde2fa58a14fd8a44l,0x509e7df35b627596l,
            0x3d76a87cc3bb07a7l },
          { 0x8018fee5b8ef000al,0x71ce33e9823fd4b6l,0x3a1cac37469c0bb1l,
            0x92fe7aeaf3eec8eel },
          0 },
        /* 59 << 64 */
        { { 0x37ad0eb8de64e568l,0x4ac669bca1e3e20el,0x240d0ac22ce944edl,
            0xd532039a3c1b28fbl },
          { 0xa2bb899a23acba6cl,0xd472af671af937e1l,0x04478f7b8851e753l,
            0x74030eef5ea05307l },
          0 },
        /* 60 << 64 */
        { { 0x3559e7b67dc17874l,0xd0caf0ef8195cc2al,0x07c067880cd24dd9l,
            0x01a99ea002857c41l },
          { 0xd86579e490f82f63l,0xb1e0658ae41c9237l,0x075ffafd93fd1e79l,
            0x6e70403547f60b8fl },
          0 },
        /* 61 << 64 */
        { { 0x2246ad76c1d68c31l,0x9126202b0d5c4677l,0x5f40de81638882dcl,
            0xb131988ca3253a7fl },
          { 0x766f1897ba9ae0a8l,0xf0e01dd41d8b5fefl,0x03e28ce3ed7b12c8l,
            0x44b3a2be1fd20e1el },
          0 },
        /* 63 << 64 */
        { { 0xd4c8e8e5f2a5f247l,0x42ffd816c2c7c979l,0x89e1485211093d1al,
            0x98f44a4613871ebbl },
          { 0x374849964b032e2dl,0x28a430f445995a61l,0xf2f9acbad5be16b6l,
            0xac98a5402d8e02aal },
          0 },
        /* 64 << 64 */
        { { 0x0d53f5c7a3e6fcedl,0xe8cbbdd5f45fbdebl,0xf85c01df13339a70l,
            0x0ff71880142ceb81l },
          { 0x4c4e8774bd70437al,0x5fb32891ba0bda6al,0x1cdbebd2f18bd26el,
            0x2f9526f103a9d522l },
          0 },
        /* 65 << 64 */
        { { 0x48334fdcc20b8d30l,0x25f887d749414fddl,0x9ccd513311a2cf0dl,
            0x7e7799e4d08975a4l },
          { 0xb5993a53729b951cl,0x0cf14a5a62dbc6a8l,0xb39ed36efe4d16eel,
            0xb75f3fb681bda63al },
          0 },
        /* 71 << 64 */
        { { 0xac7db8706d4f68b5l,0x819a13c7be49b3a4l,0x646ae2b1418bf1e9l,
            0x25b53a5f69b3a5ccl },
          { 0xd23d94d37de26578l,0x8bb581caecdd138al,0x9e053f67f857b0dal,
            0xe679cc7a255ff474l },
          0 },
        /* 77 << 64 */
        { { 0x4a4b8d990df097f9l,0x0ae1227a0b4173cal,0x0d401778adb72178l,
            0xd29848b43f421e0cl },
          { 0xc5eec6096eb0722dl,0x527d72877e12c028l,0xed12a9e71b5dcc0cl,
            0x26b27344dcf4b4dal },
          0 },
        /* 83 << 64 */
        { { 0x695c502565e4408al,0x2d23768fcbce94e6l,0x1505fa1e5080b88dl,
            0x5c8fbab6855f7cc1l },
          { 0x70d876f275fb125dl,0x456421330a252007l,0xfe99249a8ee05be1l,
            0x0893b620f4bf5490l },
          0 },
        /* 89 << 64 */
        { { 0x2a59df1ed9fe6bdfl,0x96a9c791785e057fl,0x4b0d795f86a1d751l,
            0x196c8e0aec642886l },
          { 0x6df67899bc0e055cl,0x4173204a63007433l,0xb5ee4efec21c9245l,
            0x2f7d4c75c1451bael },
          0 },
        /* 95 << 64 */
        { { 0x2ad7f836b1047b7fl,0x368d431a71f6bfe1l,0xfcd933b103db4667l,
            0xfff77ed3ecb81330l },
          { 0x3677935b44958bd4l,0xa6cfcda8a1d5a9e7l,0xb2b73bc699ff9fael,
            0x1c2cd628f866d3c4l },
          0 },
        /* 101 << 64 */
        { { 0x2756873495031ceel,0xebed373d51091c1bl,0x398fef0819aa2f27l,
            0x2f26174e2c0a9feal },
          { 0xedca72b6b219be3fl,0x001a8fdc80503df8l,0x9a2fadbb6b93f643l,
            0xd48e552cd44cebc3l },
          0 },
        /* 107 << 64 */
        { { 0x6c0dbb68667a7ab6l,0x00490ce757630e91l,0x04976cd57eb2f382l,
            0x9ee486b655dda4a3l },
          { 0x4ea5c9c9cca0d01cl,0xa6e054b639f69c6dl,0xb3b7ac992ecab239l,
            0x80c9f6d17597512el },
          0 },
        /* 113 << 64 */
        { { 0x64dfdd68b942fad9l,0xe7d8e88da5eb3d14l,0xb7281dc2382f6301l,
            0xcfa2ee6dbfe00a7fl },
          { 0x6e617657dc7be39fl,0x22d58dd6591c6e3al,0xd3a4003918318c13l,
            0xcac6c830981b6b72l },
          0 },
        /* 116 << 64 */
        { { 0x009690ffb4fbfaa0l,0x8bbbdab73619c6dbl,0xc6d44273728356e8l,
            0xfd76f0d8e453ec35l },
          { 0x775c2554aac28a29l,0x28f7af9d5c55e4f0l,0xbacf54a688e8ad4dl,
            0x85b018e80aa76ddfl },
          0 },
        /* 119 << 64 */
        { { 0x27893f7983ce88e4l,0x9556c9977785f13dl,0x83d3c38d3a35831el,
            0x3856c829d12f0a1dl },
          { 0xb308d84c93259c1al,0x4ef87ab4691ffd28l,0x76a18d5321a88c58l,
            0xf13cd5d53503cb4dl },
          0 },
        /* 125 << 64 */
        { { 0x669d93dba8cc0db3l,0x403cb9200dfcfcf4l,0x5def4a03e77c3979l,
            0x2a05c9423e2e2522l },
          { 0xd86dca52b5f48bf0l,0x174766de5828a135l,0x116290b40d3a96d0l,
            0xe1999457aeea1193l },
          0 },
    },
    {
        /* 0 << 72 */
        { { 0x00, 0x00, 0x00, 0x00 },
          { 0x00, 0x00, 0x00, 0x00 },
          1 },
        /* 1 << 72 */
        { { 0x0db2fb5ed005832al,0x5f5efd3b91042e4fl,0x8c4ffdc6ed70f8cal,
            0xe4645d0bb52da9ccl },
          { 0x9596f58bc9001d1fl,0x52c8f0bc4e117205l,0xfd4aa0d2e398a084l,
            0x815bfe3a104f49del },
          0 },
        /* 3 << 72 */
        { { 0x524d226ad7ab9a2dl,0x9c00090d7dfae958l,0x0ba5f5398751d8c2l,
            0x8afcbcdd3ab8262dl },
          { 0x57392729e99d043bl,0xef51263baebc943al,0x9feace9320862935l,
            0x639efc03b06c817bl },
          0 },
        /* 4 << 72 */
        { { 0xe839be7d341d81dcl,0xcddb688932148379l,0xda6211a1f7026eadl,
            0xf3b2575ff4d1cc5el },
          { 0x40cfc8f6a7a73ae6l,0x83879a5e61d5b483l,0xc5acb1ed41a50ebcl,
            0x59a60cc83c07d8fal },
          0 },
        /* 5 << 72 */
        { { 0xdec98d4ac3b81990l,0x1cb837229e0cc8fel,0xfe0b0491d2b427b9l,
            0x0f2386ace983a66cl },
          { 0x930c4d1eb3291213l,0xa2f82b2e59a62ae4l,0x77233853f93e89e3l,
            0x7f8063ac11777c7fl },
          0 },
        /* 7 << 72 */
        { { 0x36e607cf02ff6072l,0xa47d2ca98ad98cdcl,0xbf471d1ef5f56609l,
            0xbcf86623f264ada0l },
          { 0xb70c0687aa9e5cb6l,0xc98124f217401c6cl,0x8189635fd4a61435l,
            0xd28fb8afa9d98ea6l },
          0 },
        /* 9 << 72 */
        { { 0x3d4da8c3017025f3l,0xefcf628cfb9579b4l,0x5c4d00161f3716ecl,
            0x9c27ebc46801116el },
          { 0x5eba0ea11da1767el,0xfe15145247004c57l,0x3ace6df68c2373b7l,
            0x75c3dffe5dbc37acl },
          0 },
        /* 10 << 72 */
        { { 0xa2a147dba28a0749l,0x246c20d6ee519165l,0x5068d1b1d3810715l,
            0xb1e7018c748160b9l },
          { 0x03f5b1faf380ff62l,0xef7fb1ddf3cb2c1el,0xeab539a8fc91a7dal,
            0x83ddb707f3f9b561l },
          0 },
        /* 11 << 72 */
        { { 0xb57276d980101b98l,0x760883fdb82f0f66l,0x89d7de754bc3eff3l,
            0x03b606435dc2ab40l },
          { 0xcd6e53dfe05beeacl,0xf2f1e862bc3325cdl,0xdd0f7921774f03c3l,
            0x97ca72214552cc1bl },
          0 },
        /* 13 << 72 */
        { { 0x760cb3b5e224c5d7l,0xfa3baf8c68616919l,0x9fbca1138d142552l,
            0x1ab18bf17669ebf5l },
          { 0x55e6f53e9bdf25ddl,0x04cc0bf3cb6cd154l,0x595bef4995e89080l,
            0xfe9459a8104a9ac1l },
          0 },
        /* 15 << 72 */
        { { 0x694b64c5abb020e8l,0x3d18c18419c4eec7l,0x9c4673ef1c4793e5l,
            0xc7b8aeb5056092e6l },
          { 0x3aa1ca43f0f8c16bl,0x224ed5ecd679b2f6l,0x0d56eeaf55a205c9l,
            0xbfe115ba4b8e028bl },
          0 },
        /* 16 << 72 */
        { { 0x3e22a7b397acf4ecl,0x0426c4005ea8b640l,0x5e3295a64e969285l,
            0x22aabc59a6a45670l },
          { 0xb929714c5f5942bcl,0x9a6168bdfa3182edl,0x2216a665104152bal,
            0x46908d03b6926368l },
          0 },
        /* 17 << 72 */
        { { 0x9b8be0247fcba850l,0x81eb5797820a181el,0xa0f2812230a01211l,
            0x7e9cdc3cae7b8821l },
          { 0x202332cc72ce15e7l,0xcd3cb2bbcb8238d7l,0xe4ab63dfc6e82c43l,
            0x58bd00283183d717l },
          0 },
        /* 19 << 72 */
        { { 0x02d57b7e717ed7b5l,0xd22e5b244dbce1a2l,0x174bd7712a4cdcf5l,
            0xa6fdb801408205bbl },
          { 0x67b4b0695e1387e9l,0x332b19a10591a442l,0x24edd916ccacf366l,
            0xbe34cc4534958a50l },
          0 },
        /* 21 << 72 */
        { { 0xa3f46e1e3e66d391l,0xb4a732cd7d6369b2l,0x99c3b85d402c1022l,
            0x7dccfcbe2b54932el },
          { 0xa6ddaa7b56b1dfe2l,0x31dc78a5e34a82c9l,0x8abeb3da704f3941l,
            0xdf11a36cca55fa98l },
          0 },
        /* 23 << 72 */
        { { 0x6c01f77a16e00c1bl,0x82515490839eaaacl,0x62f3a4ef3470d334l,
            0x5a29a6491c1dcd6cl },
          { 0x46b6782ece997a25l,0x9978fb35d3579953l,0x98f5a9df0960e0cel,
            0x547dc8391f527a4cl },
          0 },
        /* 25 << 72 */
        { { 0x395b15835d9dc24fl,0xa4256932c73ae680l,0x0542960efaa2c8e9l,
            0x2bb3adee71068c6al },
          { 0xa706099b570b4554l,0x85d12bb5f4e278d6l,0xd78af6f664296843l,
            0xc7d3b3888428c633l },
          0 },
        /* 27 << 72 */
        { { 0x34d44f9343b7e597l,0xdde440a7c2530f42l,0x7270a0817856bdb9l,
            0x86a945eb5353032fl },
          { 0x6c2f8e9966d39810l,0x0642a31b9b8b4b6bl,0x51679e62d1509d82l,
            0x0120001c90f8ff16l },
          0 },
        /* 28 << 72 */
        { { 0x50a1c1062e36e34al,0x74e8f58ce024ed1al,0x3f0f1dfa1300d726l,
            0x6680df267b4a2d18l },
          { 0x12b5979d8235b3b7l,0x1d2fafcb8a611493l,0x73ebda968848ece5l,
            0xe996c275a413e399l },
          0 },
        /* 29 << 72 */
        { { 0x46b7d7c7495ff000l,0xe60ed097baed95d1l,0xaa8804ac6e38f9c0l,
            0x92990c0645c6f9bbl },
          { 0xcae6a439c0919851l,0x713dff151bf5e1f2l,0x5d262c302eb38cdbl,
            0xb73d505190df31dfl },
          0 },
        /* 31 << 72 */
        { { 0x921e7b1c32d9268cl,0x34db2b964276fad4l,0x0ec56d34cc44e730l,
            0x59be3a46096545b7l },
          { 0xe9fdbc9766cf3a6al,0x7b2f83edd04e9b53l,0x6d99b3cc8fbae3e7l,
            0x8eb5646c7ada3a40l },
          0 },
        /* 33 << 72 */
        { { 0xa69ab906fc3302bfl,0x49ae6ba7d0872e90l,0xc9e2d6d1f3a1bfc3l,
            0x11dfe85f1a033500l },
          { 0x45189c2998666dbdl,0xba6aab88bbfd13cel,0xcf9c8b43dbd38cd4l,
            0xa0cb581b68009236l },
          0 },
        /* 34 << 72 */
        { { 0xff18c42a16288a7al,0x6363ace430699163l,0x8546d6332a2ce353l,
            0x5e0379ef7b6b3418l },
          { 0x2df2bb463e941bb2l,0xae7c091888e1aacel,0x6bc0982d83f5a37al,
            0x8521bd02676d09e0l },
          0 },
        /* 35 << 72 */
        { { 0x6531dff33d361aacl,0x59b954477c8cac2el,0xcc104df6c5cb7363l,
            0x68b571c519364acdl },
          { 0x7521e962979c3bc0l,0xbe0544c9c4aa1f92l,0x59127fe92a31eabbl,
            0x760ac28593d8b55bl },
          0 },
        /* 36 << 72 */
        { { 0x62ed534c6115164bl,0xaebe9e4cdce84ceal,0xd81c91a1c83f64c3l,
            0x325a8ca8ecacd09al },
          { 0x7ea57ad968b45df1l,0xa555636fd530c5d2l,0x23aff510591cfe32l,
            0x46ff147637bedab9l },
          0 },
        /* 37 << 72 */
        { { 0xa5a7e81ecb2edb3bl,0x9b0dc5f4f8fbe238l,0xc6f258087c66dd34l,
            0xb4a57503a3f8f38al },
          { 0x195b433513571b5bl,0xa32840763ccbc30bl,0x64ae1ffccf99ddd5l,
            0x0dfc8772aa844e76l },
          0 },
        /* 39 << 72 */
        { { 0x8b471afbfb22341dl,0xbf448b43397afdd2l,0x4cb08409682c37edl,
            0xc3acfae6a948f1f6l },
          { 0xf58462549e634707l,0x50161a78bd949f52l,0xf0529e752fe73566l,
            0xe7e3fdef6fda53e0l },
          0 },
        /* 40 << 72 */
        { { 0x56dab1c8321a518cl,0xfd4439a68bce226fl,0xe0b30d194facb9fal,
            0xb5052f307583571bl },
          { 0x1442641012afd476l,0xd02e417203fe624al,0xfc394f65531c92e6l,
            0x16d4bf5ad4bc0b52l },
          0 },
        /* 41 << 72 */
        { { 0xa38ac25eb4ec4f0fl,0x5399c024de72b27dl,0x08318aafd81a3d65l,
            0x1af227a70c20e5d9l },
          { 0x6389cc9a26c54e25l,0x438298bba47dc27fl,0x75386cca1a63fa0el,
            0xc941e84cdf7bc1b0l },
          0 },
        /* 43 << 72 */
        { { 0x81cad748fdfe3faal,0x752107b453ff1988l,0x8d8bb7001a8fd829l,
            0x69838e15ca821d8el },
          { 0x24371ede3b9f6b34l,0x19b4bb24d91e1495l,0x90899ca1e598ded1l,
            0xbbb78b167c14e9e3l },
          0 },
        /* 44 << 72 */
        { { 0xa577e84cbef239aal,0x656d2b6f8904b4d4l,0x2f6defe6ca4007edl,
            0xca6e517737770796l },
          { 0x4c62fcba298b6448l,0x046849660f62e00dl,0x806c2f0390b07d82l,
            0x730855795e8d1e60l },
          0 },
        /* 45 << 72 */
        { { 0x24488802f4703b78l,0x6c9323bee9eaa1e0l,0x242990e2aa94c170l,
            0x3292bc42a15b5886l },
          { 0x60ccb5bc908af203l,0x8fd63583713b09bdl,0x40791ecad693fa28l,
            0xea80abf2941af8a1l },
          0 },
        /* 46 << 72 */
        { { 0xf9c0315071145fe3l,0x80a71b55d7873a7dl,0xd134244b5e10bac7l,
            0x303f7e12ded3a4b4l },
          { 0x58e6f17e803b7a3bl,0xcd6f64130b1ca6b4l,0x25e744ce2ce65aa2l,
            0xf2bbc66b952efa51l },
          0 },
        /* 47 << 72 */
        { { 0xc8b212e75913e1f3l,0xf018ab208d416886l,0x28249e15b617cac4l,
            0x837fcba1693ed09al },
          { 0x9c457e511c15a1bcl,0x9354758756c7f3f1l,0x1afd80348be18306l,
            0xa43d56982256ab14l },
          0 },
        /* 48 << 72 */
        { { 0xce06b88210395755l,0x117ce6345ec1df80l,0xfefae513eff55e96l,
            0xcf36cba6fd7fed1el },
          { 0x7340eca9a40ebf88l,0xe6ec1bcfb3d37e12l,0xca51b64e86bbf9ffl,
            0x4e0dbb588b40e05el },
          0 },
        /* 49 << 72 */
        { { 0xf9c063f62f2be34bl,0x9ca32fa99c20f16bl,0xe02e350d0125a01al,
            0x62d66c54e6516c25l },
          { 0x21b154ad5120bedbl,0xb1077f4e8d6ff9d8l,0xd01a46c300bb4941l,
            0x9d381847d1460588l },
          0 },
        /* 51 << 72 */
        { { 0xf3a9b311581cb57bl,0x65fb3fb649727d13l,0xb8496e3d35131142l,
            0xf7642f554d0cdab9l },
          { 0xe2f66f0e9f6d7e45l,0xbae14cedaa22fcd4l,0x1f769f0e49b2e05al,
            0x08c4d7784ac5191el },
          0 },
        /* 52 << 72 */
        { { 0x86f9108ece4aa825l,0xbe5b2f317e5a5fbfl,0x2772c1b49254bb78l,
            0xae6cdf5f4ff8ac5cl },
          { 0x106cd94bf6b7a12el,0xbe0915d6d1c7a1a5l,0x8bf6bc8d3b40ac5el,
            0xbb89180423ee3acal },
          0 },
        /* 53 << 72 */
        { { 0x76f15eaa618b5ea1l,0xec1ea62e6d4ad0c8l,0x301b60c8168d57fal,
            0x454d5f771edbfb05l },
          { 0xea888e29a936031al,0x01303d3f0174dd17l,0x8b5e06b4244254e7l,
            0x00ebf03509724acfl },
          0 },
        /* 55 << 72 */
        { { 0x66ce3ded8e66d509l,0x368e38d05a488586l,0x7b9ae220c7eedf5el,
            0x67e9ea52bfbf9d62l },
          { 0xe9cbf53d99b7ecb3l,0xfde3e8c0908bf072l,0x288400ab1107e21fl,
            0x24c8856256532667l },
          0 },
        /* 57 << 72 */
        { { 0x0d5f9955ca9d3ad1l,0x545feba13a1daec0l,0xd22972016cb30f23l,
            0x9660175ccef6cf6el },
          { 0xbf3e341a395738dcl,0x74a5efbc80f7cca4l,0xc4f9a07bbebc6a60l,
            0x2f1e3dad4b1f915al },
          0 },
        /* 59 << 72 */
        { { 0xada4423f0d5e2e34l,0x2d31f4920b372358l,0xd7f469370e2d6a8cl,
            0xf5e7ccfe0028e4ael },
          { 0x20fcb1f3928854b2l,0x2a8973c507271bf6l,0xe87de33e5fa88fe1l,
            0xe9af2dce7bd3c2a6l },
          0 },
        /* 60 << 72 */
        { { 0x185a19d959d097b2l,0xb1c72a3a0dea2875l,0x3b371628f9021f08l,
            0x45f1255bfa9d6ac1l },
          { 0x9ff36a90cfd72c0dl,0x8c7315db24fe2376l,0x9aebcde04b34d42cl,
            0x2129ab16923025f3l },
          0 },
        /* 61 << 72 */
        { { 0x341b9dd714b4cf50l,0x7c6e4634d619d00el,0x571d6e2fdf2165ael,
            0xdedf9cd18dbe9db5l },
          { 0x52a152777c5f3dc3l,0x7d27c97ef2901cf7l,0x5e098b54d02a85dfl,
            0x6fce3e13088e3640l },
          0 },
        /* 63 << 72 */
        { { 0xfa95be147a939904l,0xdfcf5b9bb56365ccl,0xdbb546bdd2d66922l,
            0xf26a8b9cda03ca7fl },
          { 0x96a8042d16821c0cl,0xe6729970e88ede60l,0xd028130d1285e303l,
            0x1678b01688b7de75l },
          0 },
        /* 64 << 72 */
        { { 0x96649933aed1d1f7l,0x566eaff350563090l,0x345057f0ad2e39cfl,
            0x148ff65b1f832124l },
          { 0x042e89d4cf94cf0dl,0x319bec84520c58b3l,0x2a2676265361aa0dl,
            0xc86fa3028fbc87adl },
          0 },
        /* 65 << 72 */
        { { 0x5db4884124627d04l,0xf92740766f7e3febl,0xd09eb11773496240l,
            0xd48e51419a6b9ec9l },
          { 0xcbb2ac97b7336e27l,0xe794fb760640bf6cl,0xc0b7f78dc7c7fa3fl,
            0x1355d071fd2edbb9l },
          0 },
        /* 71 << 72 */
        { { 0x575d9724e84e25a3l,0x068690a13d4d8708l,0x8a7b1c6c54dd62d0l,
            0x8c45e1b37f88e231l },
          { 0x38c665466d85afe2l,0x65231642e1d69f1bl,0xb71c53a090687ec1l,
            0xdf8469d777fb5981l },
          0 },
        /* 77 << 72 */
        { { 0xb920b503144fe6bcl,0x54b0f0593914c130l,0x63188d5a8269b650l,
            0x8d7780962fc7064dl },
          { 0xbf7b0eec5e50839al,0xaf8a7ddbe242cd06l,0x93df850809cecdb9l,
            0x4db58a72410659e9l },
          0 },
        /* 83 << 72 */
        { { 0x460d9b383baba3cdl,0x52386e4d2cf860b8l,0xd224fe5da3924b9al,
            0xe4a4be7bcf14d813l },
          { 0xb0759e82ed3774fdl,0x57c064b38d9b6c59l,0x301ab902aee183d0l,
            0xf1c873495ba207c3l },
          0 },
        /* 89 << 72 */
        { { 0xe8245b0a6dd58696l,0x0714eedb61091043l,0x7d9874459101129bl,
            0x4a7f1f03a0b27a21l },
          { 0x282e5cff71ee2045l,0x25c694a3da5c6b41l,0xb3d8e21f5542ca55l,
            0x57d64170e3601af0l },
          0 },
        /* 95 << 72 */
        { { 0x9c8e86c6c6c4fee6l,0x70194db5a596119bl,0xfc6271d30e06050cl,
            0x17d94c89b15f18d2l },
          { 0x76c9e9bd49817224l,0x42621638b989c5bcl,0x1e9c4cbeb769d70cl,
            0x85e227c3b87f2783l },
          0 },
        /* 101 << 72 */
        { { 0x146185d2117e73c5l,0xbf6214696dc38116l,0x9af9d9b5459e72cbl,
            0x7512882fb3930b85l },
          { 0xfe935379d36583b8l,0xb83ad35e7c7fdcdel,0x093ca0ab2658ae4bl,
            0xc9b16d60a756681bl },
          0 },
        /* 107 << 72 */
        { { 0x12c24d9195d3519bl,0x1fc6db1bdb43fd06l,0x1ae49fed25bbde51l,
            0x27072e0b76d2827bl },
          { 0xdcb92e05aeb8c47fl,0x601d414056145f67l,0xcb7002652a39e8f7l,
            0x6ce9facc35620d8cl },
          0 },
        /* 113 << 72 */
        { { 0x5c428a5ebd702c22l,0xcb6863291616129dl,0xe6278994eabcb9a1l,
            0xb409a10b9327e540l },
          { 0x6899f7cb66cf96aal,0xa9225f051c64b545l,0x00c5522ee3feec21l,
            0x35503728e083315cl },
          0 },
        /* 116 << 72 */
        { { 0x1916d88cf1600077l,0x1ac9c238e3a58b2bl,0x3080df8535f3508dl,
            0x86cc18712744912bl },
          { 0x56aec9d5ccd15044l,0x8dd9061a5db0ab17l,0x84d6bc4e2c84171dl,
            0xd569c7d70989a5bdl },
          0 },
        /* 119 << 72 */
        { { 0x24446b2702af35abl,0x071710478eea4565l,0xba4989db728306e6l,
            0x2cd692a85954a558l },
          { 0x644e02763576b32el,0x7efdb65c1f9fe65dl,0x04b2828e8796c048l,
            0xcfd22481187b979bl },
          0 },
        /* 125 << 72 */
        { { 0xa10d104084ea9701l,0x27dd0dcb415e187dl,0xf667c5e939bfe45cl,
            0x3995e4ae55b67506l },
          { 0xb25117d9b5a14801l,0xeee58525fe142e92l,0x100b856a6dbae9f1l,
            0xada7057629586658l },
          0 },
    },
    {
        /* 0 << 80 */
        { { 0x00, 0x00, 0x00, 0x00 },
          { 0x00, 0x00, 0x00, 0x00 },
          1 },
        /* 1 << 80 */
        { { 0xe4050f1cf1c367cal,0x9bc85a9bc90fbc7dl,0xa373c4a2e1a11032l,
            0xb64232b7ad0393a9l },
          { 0xf5577eb0167dad29l,0x1604f30194b78ab2l,0x0baa94afe829348bl,
            0x77fbd8dd41654342l },
          0 },
        /* 3 << 80 */
        { { 0xa2f7932c68af43eel,0x5502468e703d00bdl,0xe5dc978f2fb061f5l,
            0xc9a1904a28c815adl },
          { 0xd3af538d470c56a4l,0x159abc5f193d8cedl,0x2a37245f20108ef3l,
            0xfa17081e223f7178l },
          0 },
        /* 4 << 80 */
        { { 0x1fe2a9b2b4b4b67cl,0xc1d10df0e8020604l,0x9d64abfcbc8058d8l,
            0x8943b9b2712a0fbbl },
          { 0x90eed9143b3def04l,0x85ab3aa24ce775ffl,0x605fd4ca7bbc9040l,
            0x8b34a564e2c75dfbl },
          0 },
        /* 5 << 80 */
        { { 0x5c18acf88e2f7d90l,0xfdbf33d777be32cdl,0x0a085cd7d2eb5ee9l,
            0x2d702cfbb3201115l },
          { 0xb6e0ebdb85c88ce8l,0x23a3ce3c1e01d617l,0x3041618e567333acl,
            0x9dd0fd8f157edb6bl },
          0 },
        /* 7 << 80 */
        { { 0x516ff3a36fa6110cl,0x74fb1eb1fb93561fl,0x6c0c90478457522bl,
            0xcfd321046bb8bdc6l },
          { 0x2d6884a2cc80ad57l,0x7c27fc3586a9b637l,0x3461baedadf4e8cdl,
            0x1d56251a617242f0l },
          0 },
        /* 9 << 80 */
        { { 0x892c81a321175ec1l,0x9159a505ee018109l,0xc70130532d8be316l,
            0x76060c21426fa2e5l },
          { 0x074d2dfc6b6f0f22l,0x9725fc64ca01a671l,0x3f6679b92770bd8el,
            0x8fe6604fd7c9b3fel },
          0 },
        /* 10 << 80 */
        { { 0xce711154b6e00a84l,0xd9fe7e4224890e60l,0xd10bc6c34560988fl,
            0xbdc2ef526859b004l },
          { 0xdcf0d868d5c890eel,0x893115e6119c47dcl,0xe97966fbee714567l,
            0x117813355c85aa53l },
          0 },
        /* 11 << 80 */
        { { 0x71d530cc73204349l,0xc9df473d94a0679cl,0xc572f0014261e031l,
            0x9786b71f22f135fel },
          { 0xed6505fa6b64e56fl,0xe2fb48e905219c46l,0x0dbec45bedf53d71l,
            0xd7d782f2c589f406l },
          0 },
        /* 13 << 80 */
        { { 0x06513c8a446cd7f4l,0x158c423b906d52a6l,0x71503261c423866cl,
            0x4b96f57093c148eel },
          { 0x5daf9cc7239a8523l,0x611b597695ac4b8bl,0xde3981db724bf7f6l,
            0x7e7d0f7867afc443l },
          0 },
        /* 15 << 80 */
        { { 0x3d1ab80c8ce59954l,0x742c5a9478222ac0l,0x3ddacbf894f878ddl,
            0xfc085117e7d54a99l },
          { 0xfb0f1dfa21e38ec2l,0x1c7b59cb16f4ff7fl,0x988752397ea888fel,
            0x705d270cb10dc889l },
          0 },
        /* 16 << 80 */
        { { 0xe5aa692a87dec0e1l,0x010ded8df7b39d00l,0x7b1b80c854cfa0b5l,
            0x66beb876a0f8ea28l },
          { 0x50d7f5313476cd0el,0xa63d0e65b08d3949l,0x1a09eea953479fc6l,
            0x82ae9891f499e742l },
          0 },
        /* 17 << 80 */
        { { 0xd7c89ba1e7d1cefdl,0xcb33553a9a91e03dl,0xa01caaff59f01e54l,
            0x4a71c141de07def7l },
          { 0xe1616a4034d467d1l,0x6f395ab2e8ba8817l,0xf781ea64e45869abl,
            0x8b9513bb7134f484l },
          0 },
        /* 19 << 80 */
        { { 0x0b0ec9035948c135l,0xaee219539a990127l,0x9d15ba0eb185dda1l,
            0xd87bc2fb2c7d6802l },
          { 0x05a480307a82d7f8l,0x7b591ce4e7e11ec3l,0x14d4cc22a0e15fdbl,
            0xf2d4213576def955l },
          0 },
        /* 21 << 80 */
        { { 0xd56d69e4117a5f59l,0xcae6008a01286e97l,0x716a0a282dab13b0l,
            0xc821da99b3a8d2d0l },
          { 0x6898b66239c305e6l,0xe42d3394c8b61142l,0x54c1d2b253b16712l,
            0x3cec3953a01f4be6l },
          0 },
        /* 23 << 80 */
        { { 0x5bd1e3036951b85el,0x1a73f1fb164d79a4l,0x6e77abd39fb22bc3l,
            0x8ae4c181b3d18dfdl },
          { 0xdd4226f5a6a14ed1l,0x620e111feb4e1d92l,0xffce6e59edca4fe8l,
            0x39f5fc053d0a717dl },
          0 },
        /* 25 << 80 */
        { { 0xef8fa78cd91aff44l,0x6f3f9749bdc03be7l,0x171545f8b8596075l,
            0xbe31a73e2af132cel },
          { 0x5b4e174123884e1dl,0x4373357ea9fa75f0l,0x8dba2731bc06f49el,
            0xa09aebc877fa6de8l },
          0 },
        /* 27 << 80 */
        { { 0xd4974e518293e18cl,0x1e4cfc5331ec0e8fl,0x80b4258325d40b1el,
            0x5cfb73a2a85f7588l },
          { 0xe553efd204c0e00bl,0xdaa6750e9a48ac39l,0xf20936b00abda06al,
            0xbfd3c7e4bf85771cl },
          0 },
        /* 28 << 80 */
        { { 0x72669c3c7292495cl,0xa627e2dd82786572l,0xbdbfce5cd39c3e3dl,
            0xba6164927feed3d6l },
          { 0x4eb5f513e77b7318l,0x133f2e834337c2e0l,0xdea20f07f408bec6l,
            0x848a8396e3c87655l },
          0 },
        /* 29 << 80 */
        { { 0x3086643551138f2bl,0x1176d8e6108a36bal,0xd78b3b400d4d4b66l,
            0x99ddd9bd956dbff1l },
          { 0x91dfe72822f08e5fl,0x7fd8cfe6a081ac4el,0x8ebb278ed75285c2l,
            0x2335fe00ef457ac0l },
          0 },
        /* 31 << 80 */
        { { 0xe9d79c50f058191al,0x6749c3b05d3183f8l,0x5edc2708dbfeb1ecl,
            0x2c18f93621275986l },
          { 0x3a093e1f0703389fl,0xdf065e4a3ef60f44l,0x6860e4df87e7c458l,
            0xdb22d96e8bfe4c7dl },
          0 },
        /* 33 << 80 */
        { { 0xb7193811b48dad42l,0x23b9dca320ad0f0cl,0x55511ffb54efb61bl,
            0xac8ed94626f9ce42l },
          { 0xa42b4bc73fc4cbd9l,0x2a4670905c6f8e39l,0xb50040f87eb592del,
            0x6633f81bdc2541f3l },
          0 },
        /* 34 << 80 */
        { { 0xc104e02ed2d6d9c2l,0xa4876e870302517al,0x0263c9b2912f5005l,
            0x902f364a3d89d268l },
          { 0x76070565bb20a5a8l,0xa3a8977452109e98l,0x51fbffec463aa476l,
            0xfa8519625daa1503l },
          0 },
        /* 35 << 80 */
        { { 0xe449dd8f82a9a4f3l,0xa1a2f405797e6b36l,0x76913537787785e8l,
            0x0315a3cfe064481el },
          { 0xc02291ee83df11e2l,0x5b59a0e9bcd178f0l,0xd5e8d10ce6b4c63al,
            0x9eee599f3fc60a82l },
          0 },
        /* 36 << 80 */
        { { 0x051e589759621468l,0xb92c06327293621el,0xee17ea647762e4f2l,
            0x412107a771abd28cl },
          { 0xa083d87bf02d65ebl,0xbd4a3f165594395el,0x1d5694337c8882f3l,
            0xc5eb10c55f9c63cfl },
          0 },
        /* 37 << 80 */
        { { 0x4b196728c8e62c4el,0x03dbd04cb74a757cl,0xe960a65b8520f044l,
            0x9eda0f33f7937337l },
          { 0x06ff0b86b6dc7dfbl,0x3bd276c11fc1ac35l,0x0e67055b1b255c27l,
            0xe43ae552eff899f8l },
          0 },
        /* 39 << 80 */
        { { 0xc64c914d3b156d76l,0x784c1f61d794345dl,0xcda0c77c365d7a50l,
            0xcc5a1e205b32dbd0l },
          { 0x2f4e78bff90b6ac0l,0xbead62f9a2d4862dl,0xa8f67e7dcc346b53l,
            0xa38d7ae947e59dbdl },
          0 },
        /* 40 << 80 */
        { { 0x7dc1605d480aca4dl,0x08c37750ef263aabl,0xd5c6b7c93f166725l,
            0xf99982f30ff2853bl },
          { 0xc61b9583a8ecb64al,0x041211a91b771741l,0x50ba64154e156f97l,
            0xb6595ea871b8954el },
          0 },
        /* 41 << 80 */
        { { 0x4ae760845eb3b4eel,0xcafefdc6c62ed274l,0x4eabeacf113f790bl,
            0x10c2cc88a5ff64c9l },
          { 0xe7b59f8a49965d80l,0xd04884b50df07712l,0x6316ac5ba5f7bab1l,
            0x388111d99e78a075l },
          0 },
        /* 43 << 80 */
        { { 0x8d437128f24804efl,0x12a687dd7b71dd53l,0x8b8f71d96139a60el,
            0xb047fed42a095ec7l },
          { 0xef238041fba59ee8l,0x61b17fac64045514l,0x45b1cf4857afa184l,
            0x8592c50a4bff5fc5l },
          0 },
        /* 44 << 80 */
        { { 0x2830592394b745dcl,0x53e9ec16b09cb993l,0x59d0b57f9a134ed1l,
            0x89d7b439c56ee0ebl },
          { 0xc3656539991e22a2l,0xd27a89372a345043l,0x55dd5341064038eel,
            0xc9ee3f0348cb42efl },
          0 },
        /* 45 << 80 */
        { { 0x08518c631d56c1cbl,0x5650f79f31235521l,0x33fc08d648911017l,
            0xbb8b58538a0a33c8l },
          { 0xb54554f2f869a62al,0x67f8cf48222457e5l,0x46e13911f276cc0dl,
            0x4b3a2ad6943b389el },
          0 },
        /* 46 << 80 */
        { { 0x0e72b816b11a4c9dl,0x919b2738e9028fa4l,0xab80e1117698a5d6l,
            0xcd7950f56cd49adal },
          { 0x0db75c908dfb13a5l,0x2178578770f12cebl,0xfab72d5243486ff6l,
            0x66d55d726a0673ebl },
          0 },
        /* 47 << 80 */
        { { 0xe98014b922667519l,0x7fcab2b3a95da9c0l,0x9bdbccd8438d5060l,
            0xa72fff5455a726b6l },
          { 0x7ae032943a5e769bl,0xf7291e9b559a0734l,0x18ae4f182ce18eeel,
            0x88e49f7328b7b4f0l },
          0 },
        /* 48 << 80 */
        { { 0x90fe7a1d214aeb18l,0x1506af3c741432f7l,0xbb5565f9e591a0c4l,
            0x10d41a77b44f1bc3l },
          { 0xa09d65e4a84bde96l,0x42f060d8f20a6a1cl,0x652a3bfdf27f9ce7l,
            0xb6bdb65c3b3d739fl },
          0 },
        /* 49 << 80 */
        { { 0xc6a2923e60ef9d87l,0xac66cdd8c3a64f1cl,0x069292d26e0bb0ccl,
            0x9e491414451e52a0l },
          { 0x2e76cedf0e0d35b3l,0x311b7ae9af682b84l,0xaa1017a02f90b176l,
            0xac0b43a794feb6e8l },
          0 },
        /* 51 << 80 */
        { { 0x7ddb42f9214e82f5l,0x91c88566f67269d7l,0x1763ed8cdd0ff422l,
            0x045dd690ad284ddfl },
          { 0x5713bbb141e48fe7l,0xdc5bef28f8eb580fl,0x4bd0b288ed2992c2l,
            0x436587faaf5ef2b3l },
          0 },
        /* 52 << 80 */
        { { 0xbbc1a48d6e5822c4l,0x16c3135daacebd02l,0xd0c6c543b56157dfl,
            0xae249a0ef49f44a1l },
          { 0x1f2c23ce72c47341l,0x8f52dc2a25974313l,0x2c99bc0a958e0e6bl,
            0xe57eab6b950cd492l },
          0 },
        /* 53 << 80 */
        { { 0xea66db638934efc0l,0x7bfe479193c6f7c7l,0x78438d535ef90d99l,
            0xe63b87c9c665736dl },
          { 0x6de32d82db49e1bbl,0xbfa877dcd0ad1648l,0xdb2e85de1197806dl,
            0x74e9dbd3cfee7854l },
          0 },
        /* 55 << 80 */
        { { 0xd2c26e2edb6d7e0al,0x9103119a531009cdl,0xb5dc49869a8b9d54l,
            0x4781b83bb408b427l },
          { 0x70d98b2ccb4ba2f7l,0x112ed5d7fa8a36b8l,0x97257bc6fdde1675l,
            0xd2a9c711db211cb7l },
          0 },
        /* 57 << 80 */
        { { 0xe4aa6a06ee79fe8cl,0x06e210233dff8a54l,0x63e11ac5bf50731al,
            0xb8b9944f544125b8l },
          { 0xcba92c41d359aeb0l,0xd201c893249bca36l,0xfe79bd77cb501216l,
            0x694b21488d525ba4l },
          0 },
        /* 59 << 80 */
        { { 0x60c90e11ee3dde2al,0x7df08e17bb36c4a2l,0xb6c3210dcc5b3c17l,
            0xa814180955cec91cl },
          { 0xf4ecbc05a8193dffl,0xf43cdef8da5744fal,0x4895a6c6f12f8a2el,
            0x44282692eb7b910al },
          0 },
        /* 60 << 80 */
        { { 0x1a405e1886d6e13al,0x6a18c91827a7c67cl,0xc34877ebe127bfd7l,
            0x3c9fab08c098e692l },
          { 0xfe2dc65bc2066586l,0xb107603a8f68a0a9l,0x74ef0ef8127cd340l,
            0xfe577b5b86788d87l },
          0 },
        /* 61 << 80 */
        { { 0xdc7ff83c71234c81l,0xee48d9c6d868c82fl,0xb80bac5e37e4f365l,
            0x2bfbe94efcb951c2l },
          { 0x55829049a374d0b0l,0x2a502cada87a5fb4l,0x0742ac9d9ee840bal,
            0x7689bf53eecd05b1l },
          0 },
        /* 63 << 80 */
        { { 0x0e7f459320059c22l,0x47c273e0e49368a2l,0x5ccb960ac6946ee2l,
            0xd8209ec48b3271b6l },
          { 0x7fd5142cdfb9e947l,0x46a89c83ff737ab1l,0xa45f6b0282d875ecl,
            0x19a16e0e34c296d6l },
          0 },
        /* 64 << 80 */
        { { 0xeb5ddcb6ec7fae9fl,0x995f2714efb66e5al,0xdee95d8e69445d52l,
            0x1b6c2d4609e27620l },
          { 0x32621c318129d716l,0xb03909f10958c1aal,0x8c468ef91af4af63l,
            0x162c429ffba5cdf6l },
          0 },
        /* 65 << 80 */
        { { 0x65c93be33607927bl,0x86feaaecdae5411dl,0x4a1686c6dd2e2c3dl,
            0xf78200068acdf51dl },
          { 0xf82c4d0239ed3e50l,0x5ac04047b4c3a4a4l,0xbdd14d7ec34b07a7l,
            0x9911d7027cc12db5l },
          0 },
        /* 71 << 80 */
        { { 0x4ed5dbbd1751abc9l,0xaf374229a23cc54al,0x9b5fa66ea4ed3f9al,
            0xc56dd9613d380643l },
          { 0x7d77897144b38021l,0xdf4712d0d3584508l,0x0018e2eecd7ab168l,
            0xc8a3a166293d29a7l },
          0 },
        /* 77 << 80 */
        { { 0x34681bdb3a5a0214l,0xe188d6f1f718797el,0xaa751de7db761c5fl,
            0x347c50324959a5cel },
          { 0x108705fc338be49cl,0x1dc5eada95abf7a8l,0xb863808f0fc3f0b7l,
            0x529c27c1a05c4d43l },
          0 },
        /* 83 << 80 */
        { { 0xa75f90677f699f79l,0xd01cf9c866356f99l,0xf90f9b73fdfbaae7l,
            0xe0b5f4412c304d2fl },
          { 0x17cbfb11807f3f57l,0xe902d542af8a9eb4l,0x3335285461f89b4al,
            0x3a51c54d3628c0ael },
          0 },
        /* 89 << 80 */
        { { 0xae5fd487c704212dl,0x82dd07a565e2e32cl,0x46d4c9646c19c199l,
            0xe7f428593778eedcl },
          { 0x084a4e9b6dcc5ec9l,0x757e04ba2d0538b7l,0x4ec0a573a3fba4cdl,
            0x2432a4e5c627c2fcl },
          0 },
        /* 95 << 80 */
        { { 0xfde00b3094c8a424l,0x20a57d8cd224c232l,0xd6ace1a170019992l,
            0x1a648d40697e67a3l },
          { 0xed1fb10691338d84l,0x828004a08372bfc8l,0xb93030fefad3bfedl,
            0x883dea23f27369ecl },
          0 },
        /* 101 << 80 */
        { { 0xfbbf36a62a710d73l,0x8db834024b3cc6bbl,0xa60c47cf16d7b1fcl,
            0xf9778fa6cd16ce8fl },
          { 0xd77023086d14a1a6l,0x01f139cb06e8247cl,0xd89af2979770b9c1l,
            0x94bf1ca97d9fb550l },
          0 },
        /* 107 << 80 */
        { { 0xe17e2e6dc2d45f34l,0x5969d8ee26efc6cbl,0x6f175231b9219cfbl,
            0x027f333c189f1175l },
          { 0x5bc60fad54f6da49l,0xc52e09af8ae5c3f3l,0x6c0e3927ed07f46dl,
            0xbfd9e598f39cf16bl },
          0 },
        /* 113 << 80 */
        { { 0x9dffd95b090aefb9l,0x26db7b73637224fel,0xb78a679e92e2aa0cl,
            0xfc7c824ffc8f895dl },
          { 0xdc8287e8e636b3a8l,0x6b3ccc0f28b7a639l,0x38e6e2cc653de56al,
            0x998cf6985392c3cal },
          0 },
        /* 116 << 80 */
        { { 0xe68de79e57f0d6fal,0xe707b252ff9c06f7l,0x5613698a4a061697l,
            0xd83d6453b5390352l },
          { 0x59b007599867c708l,0xcfe24fd7b41ea7adl,0x4692abf3da5b7de6l,
            0xd99a6f3bf0c54e8fl },
          0 },
        /* 119 << 80 */
        { { 0xe8ee870dea4addc3l,0x0d1fb29559841f3el,0xdc05b5581dba2f14l,
            0xb8bf38324e3f4600l },
          { 0x1a909e66fd57c48al,0xb65ca4c24e2d76dfl,0x0b27755ae7c60d89l,
            0x9fcfa75acb9003f6l },
          0 },
        /* 125 << 80 */
        { { 0xbbbdf4c49e5325aal,0x6879fe11d0d1f281l,0x7a400f890633002el,
            0xc3633c779bb79ac9l },
          { 0x15a4cfae93ab9bc3l,0x379bbdea42594603l,0x7c61dfa257d2af3fl,
            0x20190537b51bfb62l },
          0 },
    },
    {
        /* 0 << 88 */
        { { 0x00, 0x00, 0x00, 0x00 },
          { 0x00, 0x00, 0x00, 0x00 },
          1 },
        /* 1 << 88 */
        { { 0xa80d1db6f79588c0l,0xfa52fc69b55768ccl,0x0b4df1ae7f54438al,
            0x0cadd1a7f9b46a4fl },
          { 0xb40ea6b31803dd6fl,0x488e4fa555eaae35l,0x9f047d55382e4e16l,
            0xc9b5b7e02f6e0c98l },
          0 },
        /* 3 << 88 */
        { { 0x4b7d0e0683a7337bl,0x1e3416d4ffecf249l,0x24840eff66a2b71fl,
            0xd0d9a50ab37cc26dl },
          { 0xe21981506fe28ef7l,0x3cc5ef1623324c7fl,0x220f3455769b5263l,
            0xe2ade2f1a10bf475l },
          0 },
        /* 4 << 88 */
        { { 0x9894344f3a29467al,0xde81e949c51eba6dl,0xdaea066ba5e5c2f2l,
            0x3fc8a61408c8c7b3l },
          { 0x7adff88f06d0de9fl,0xbbc11cf53b75ce0al,0x9fbb7accfbbc87d5l,
            0xa1458e267badfde2l },
          0 },
        /* 5 << 88 */
        { { 0x03b6c8c7dacddb7dl,0x92ed50047e1edcadl,0xa0e46c2f54080633l,
            0xcd37663d46dec1cel },
          { 0x396984c5f365b7ccl,0x294e3a2ae79bb95dl,0x9aa17d7727b1d3c1l,
            0x3ffd3cfae49440f5l },
          0 },
        /* 7 << 88 */
        { { 0x26679d11399f9cf3l,0x78e7a48e1e3c4394l,0x08722dea0d98daf1l,
            0x37e7ed5880030ea3l },
          { 0xf3731ad43c8aae72l,0x7878be95ac729695l,0x6a643affbbc28352l,
            0xef8b801b78759b61l },
          0 },
        /* 9 << 88 */
        { { 0xdcdd3709b63afe75l,0xad9d7f0b3f1af8ffl,0xdd6a8045194f4beel,
            0x867724cc2f7d998cl },
          { 0xd51d0aa5837751bel,0x21d6754a959a0658l,0xd2212611695f7e58l,
            0xec4b93c2297363efl },
          0 },
        /* 10 << 88 */
        { { 0x0ac1c5fab6ef26cfl,0xcd8ba0c5a39de8eel,0x11ba7537dd7796e0l,
            0x1215933476d58d6dl },
          { 0xf51eb76f529fda4cl,0x2fd9209ddedaa8a3l,0x555a675615efac65l,
            0xb784c9ca7fd42fe9l },
          0 },
        /* 11 << 88 */
        { { 0x8165ec11b9d1a70fl,0x01347efc384f6cael,0xe95c01a0ab7aeca9l,
            0x459ba1c5c6c99530l },
          { 0x38967a635cf3416bl,0x5c3761fd1e5457e2l,0x43e6077af03e9df6l,
            0xb15d34628bd1c7f6l },
          0 },
        /* 13 << 88 */
        { { 0xad87d3db35a75c49l,0xc69d800961af03c5l,0x31aef61a3a6a6c4cl,
            0xb3292640aa10a993l },
          { 0x959aae80aaee340fl,0xf900528e7f381a3bl,0x44ecf76e853691a3l,
            0xa081663ce749e68el },
          0 },
        /* 15 << 88 */
        { { 0x4f2782136283e34al,0x6f9fcf60fbfa315fl,0x224a2ab99b701364l,
            0xb4b1b418f9fecadcl },
          { 0xbf7280fe50ba1b9al,0x7e68259c33f36db9l,0x8ccb754e154c9fb0l,
            0xf281adb1db2328f1l },
          0 },
        /* 16 << 88 */
        { { 0xf92dda31be24319al,0x03f7d28be095a8e7l,0xa52fe84098782185l,
            0x276ddafe29c24dbcl },
          { 0x80cd54961d7a64ebl,0xe43608897f1dbe42l,0x2f81a8778438d2d5l,
            0x7e4d52a885169036l },
          0 },
        /* 17 << 88 */
        { { 0xc2a950ad2d6608bel,0xab415e2a51c3c2b6l,0xffbd2a65f5c803e7l,
            0x3f81dc3eca908532l },
          { 0x0ec47397c28c04f4l,0xf6c632e8153f58e8l,0xccac35f8efb4a6d8l,
            0x22a1b677ee6d7407l },
          0 },
        /* 19 << 88 */
        { { 0x276662435243c119l,0x79cb8580e707363el,0x5bf5ebf4d01682d6l,
            0x8a980173762811e0l },
          { 0xe2f2be1fc7547d77l,0x21a50fffb925fec6l,0x5e6cf2ef40115509l,
            0xb69beae18faa0fc0l },
          0 },
        /* 21 << 88 */
        { { 0xfa147da8cec36e75l,0xba184e5a42860484l,0xe8ec25df222fb1e6l,
            0xce91dcb18ff8403cl },
          { 0xf1b0e27ead7faa32l,0x097d881d42a3a205l,0xa8865dd43f8f56d4l,
            0x624d7a451aef929dl },
          0 },
        /* 23 << 88 */
        { { 0x3db0238ad01698e8l,0xbb7186dc00306082l,0x542f4377250f830el,
            0x34b8a67dae438c50l },
          { 0xada528a0858d8048l,0x561aa3336b57afc1l,0x8d9188e0fda35f7al,
            0x5838d1211dcad0c5l },
          0 },
        /* 25 << 88 */
        { { 0x4f97d1529f17511dl,0x8b9f012776fdb9ebl,0x53a0a72d4056e6a7l,
            0x5ff937d64e262eeel },
          { 0xaa64a8dc489fbe6dl,0xc19947dfea02bc69l,0x76f0bbb91492c9bel,
            0xe53881098d89cd01l },
          0 },
        /* 27 << 88 */
        { { 0x16083309456057b7l,0x2810c08040a331f6l,0x0561656c3c166929l,
            0x16f0d8d6ed1c3999l },
          { 0x37b6da7294697927l,0xd821c2cc23ca6c9cl,0x42ef1bdb8ca4351cl,
            0x7ca32bad5edfa682l },
          0 },
        /* 28 << 88 */
        { { 0xdc1de17d98119f10l,0x74353c5d488c36a6l,0x14aaf33a3d8e23dfl,
            0x31e075c078baf593l },
          { 0x0f7ca03a46d1ca3cl,0x99c5e3ac47b660c7l,0x70d0241388fe2e59l,
            0x2e9a6be12a7ec005l },
          0 },
        /* 29 << 88 */
        { { 0x4d1f087f184252b1l,0xfd3ace273f5b49c6l,0x6e874447bbb04da2l,
            0x2347e3a1b3767ff0l },
          { 0x990d4010f868966al,0x35320090dd658b5el,0x1105bfb974fe972al,
            0x3961f7dc8e7ad2c6l },
          0 },
        /* 31 << 88 */
        { { 0x100d8b54741e3286l,0x65d9108ef3abc7afl,0x172b450620ef8fbcl,
            0x11bd7db2d81b8a2el },
          { 0xf89210e1e8e41de5l,0x910613f3d98a868bl,0xbfc85241849aa909l,
            0x68a43e21c7d3a7cal },
          0 },
        /* 33 << 88 */
        { { 0x68f891479a4f8293l,0x48262328a5eb9101l,0x7eca2a178fe218b5l,
            0xde6c22dbc733f768l },
          { 0xde7171d108d6084dl,0xd153827a0f0f8092l,0xc7b52d8f85a9252fl,
            0xfa29ca3a5708b31fl },
          0 },
        /* 34 << 88 */
        { { 0x20518ddf9e0ad7e7l,0x33d5d079e8d28b9bl,0x1149b393d13058b0l,
            0x708cc65586d4651dl },
          { 0xd7fefaa694207435l,0xce882c0d96312f8fl,0x2fd5cb2059d091a7l,
            0x4533a88a0e1ece94l },
          0 },
        /* 35 << 88 */
        { { 0xceddd9b5a59c28bcl,0xaa4808f9572e2a5dl,0x38bc191999014a1el,
            0x1aacefdaa6d85686l },
          { 0xa59283d42a573fddl,0x84359db29c387594l,0x79994773dca3acc8l,
            0xe4323e7654cf7653l },
          0 },
        /* 36 << 88 */
        { { 0xac449695241fbd6fl,0x67c9b170081c1223l,0x16868f21b56aac6fl,
            0x34bd8fa3f8bcb721l },
          { 0x06b6bd33b6691c76l,0x6c924766381a7973l,0x6a12444ca54078dbl,
            0xd02e91a96d1051ccl },
          0 },
        /* 37 << 88 */
        { { 0x512f5fb35f30b344l,0xb13ade169d516885l,0x18812e9b2b468802l,
            0xf15d730e6b28979al },
          { 0x5015616f6889348bl,0xe0b02a0a96af0401l,0x3b02007b61204c89l,
            0x9ece2aa7432742a4l },
          0 },
        /* 39 << 88 */
        { { 0xd5f7e09c7c1cc4a1l,0x313ac04218b2d854l,0xbc4fe2a04c253b10l,
            0x25a696a3c7080b5cl },
          { 0x6de3cb6aef811877l,0x4d242fecd15f9644l,0xb9bfa2480ee6a136l,
            0x8122679e9c8d181el },
          0 },
        /* 40 << 88 */
        { { 0x37e5684744ddfa35l,0x9ccfc5c5dab3f747l,0x9ac1df3f1ee96cf4l,
            0x0c0571a13b480b8fl },
          { 0x2fbeb3d54b3a7b3cl,0x35c036695dcdbb99l,0x52a0f5dcb2415b3al,
            0xd57759b44413ed9al },
          0 },
        /* 41 << 88 */
        { { 0xc2c7daec96a8d727l,0x8a11631a17f3abf9l,0x06aba65c0ae8940al,
            0xfca280c7873d3635l },
          { 0x57496889ddb72b87l,0xaa9a3359320793d4l,0x11b6864d43120741l,
            0x1877cd4e51527639l },
          0 },
        /* 43 << 88 */
        { { 0x8b35ce4e6f43dfc6l,0x4114b2fe9a19f3bfl,0x8c4af8024ffa45cal,
            0xa3ab5f869328b847l },
          { 0x0986de3e555f30f0l,0xaae6e3eac8cb84c4l,0x2a7dcdbaa4ba01f7l,
            0xfa32efa729f5dc6cl },
          0 },
        /* 44 << 88 */
        { { 0x077379c00b33d3f8l,0x421883c67064e409l,0x2d0873d76c29c8f6l,
            0xbfa433a3d274c0c8l },
          { 0x56dc778f23a5891el,0xd663bf6535e2de04l,0x488fdb485db517cel,
            0x00bba55e19b226c2l },
          0 },
        /* 45 << 88 */
        { { 0x879b30ead7260d78l,0x04954ba2eac5201fl,0x3210c0e3ff2529d1l,
            0x0743823488b470b3l },
          { 0x8b618de48854cc0dl,0x98270d5e35b795eel,0x0e47d651aa33ca37l,
            0x77d75fda1e87d0cfl },
          0 },
        /* 46 << 88 */
        { { 0x789dbe987803fbf9l,0x940589aa17ede316l,0x032902bd85a1988cl,
            0x43cbc0031c47f7f0l },
          { 0xc6ff73714709148fl,0x769957122d9b8a5el,0xb4520e462597b70el,
            0x00d19f39f67ff3b8l },
          0 },
        /* 47 << 88 */
        { { 0xe2dfcef9b159f403l,0xe8e9e8d8855644afl,0x2796247163fa1068l,
            0x400e992a968a5400l },
          { 0xe2b9d29f56e563c1l,0xed66759c2885fabfl,0x788b6263750abdffl,
            0x30adb00d6cbbdcacl },
          0 },
        /* 48 << 88 */
        { { 0x1fe647d83d30a2c5l,0x0857f77ef78a81dcl,0x11d5a334131a4a9bl,
            0xc0a94af929d393f5l },
          { 0xbc3a5c0bdaa6ec1al,0xba9fe49388d2d7edl,0xbb4335b4bb614797l,
            0x991c4d6872f83533l },
          0 },
        /* 49 << 88 */
        { { 0x5548d3423fa17b28l,0x38587952823ee731l,0x8ee9b90a0a28bcd1l,
            0xcfc029bf6676917el },
          { 0x7e08306d2a212358l,0x66a9488dc88a66bcl,0x7a09db327d7c9e65l,
            0x20eaf4e72cbc1790l },
          0 },
        /* 51 << 88 */
        { { 0xb3095b491f2a9605l,0x7cfc4205f72691c7l,0x1544bf964d889b90l,
            0xdc44d20ba0bbae7al },
          { 0xee369b670b1f0b23l,0xf3ec25e818a7bdcbl,0xf614ab5df47ecf65l,
            0x4869762f80a4a09dl },
          0 },
        /* 52 << 88 */
        { { 0xedbbeee78a058fb6l,0xb9d19ddcfb09121al,0xa41bb45bd34dddcel,
            0x2dbc80b900964bc4l },
          { 0x4ed9137d1d6cb654l,0x1b9016db483d01c5l,0x5fc501bc6528e22el,
            0xb2d2f8816cad646bl },
          0 },
        /* 53 << 88 */
        { { 0xb57aa72a89043e56l,0x8fbca2435c5319fdl,0xe66aef43b13ce900l,
            0x2c7c3927c3382934l },
          { 0x434d9104a835fdf5l,0x419470b81b3b85bel,0xeaec374abeb4d448l,
            0x26a53b51f33cda51l },
          0 },
        /* 55 << 88 */
        { { 0x421f1725bb1db793l,0x20214d4f558c94a9l,0x3371233b7696092cl,
            0x774d3fcb1902ab0el },
          { 0x4ce223ded149aecel,0x174b260e33057bc7l,0xdf70cfa3f6effee4l,
            0x3d8cd01f80880678l },
          0 },
        /* 57 << 88 */
        { { 0x32db21862e59985cl,0x448865abaa1b39e1l,0x250ce79cd89fe98dl,
            0x962710e763e3fb10l },
          { 0xa8fc70561ac10e3el,0x9eed208fa3b132fbl,0xf499d638937051f5l,
            0x27acf7ec21a9f78fl },
          0 },
        /* 59 << 88 */
        { { 0x148e572a4c7b445el,0xdc10a0214dc95a4fl,0xe60e9c2e02237869l,
            0xbfdfcb3aa393c3a4l },
          { 0x8b799db211a64cf0l,0x1ca865ea2e16f59fl,0x865441fbd3a17e46l,
            0x23315b9753409692l },
          0 },
        /* 60 << 88 */
        { { 0x5e76fb2f286bad39l,0xbad9efe39dcad1e2l,0x60e75190edc7e904l,
            0x6a6f063e0fecb5a5l },
          { 0x5150ed85aed8acc3l,0xb56ccfbc6d20af6cl,0x7e0d1e982c69dbfal,
            0xabf5628a7c7e10a9l },
          0 },
        /* 61 << 88 */
        { { 0xb84af2c00df6d61fl,0x02c651c52acbaf4bl,0xfb605754afaaa0bfl,
            0xa03f5257dff61017l },
          { 0x9e3ffb1672762093l,0x4f9a5da0c4f40bd3l,0x37dce5220d26f8e1l,
            0x260f736fc06a1a07l },
          0 },
        /* 63 << 88 */
        { { 0xb92aba79b1077d55l,0xc52f81081a42f5f5l,0x9913f04f86e5aa99l,
            0x6814b0b1f3c7f504l },
          { 0xb7d61fd34d354bdal,0xf27926e39581d25el,0x97724001c2dc21adl,
            0x835778231d5c4788l },
          0 },
        /* 64 << 88 */
        { { 0x77b868cee978a1d3l,0xe3a68b337ab92d04l,0x5102979487a5b862l,
            0x5f0606c33a61d41dl },
          { 0x2814be276f9326f1l,0x2f521c14c6fe3c2el,0x17464d7dacdf7351l,
            0x10f5f9d3777f7e44l },
          0 },
        /* 65 << 88 */
        { { 0x53857462ff9727a2l,0xe6870e7dc68488e7l,0x276da72808c79656l,
            0x1308eb61d86c24ebl },
          { 0x34c43a84db0a3e56l,0x03961b5525335a59l,0xf9bc2d5805689d86l,
            0xfa4d3c01eb29d6d6l },
          0 },
        /* 71 << 88 */
        { { 0xd07dac3037d10ffal,0xb2b0a0fd8bef0a79l,0xa2e804510ec02505l,
            0xf256c18962f55f5fl },
          { 0x0ca3f9b10b39f4f0l,0x7bf4e1cf3bb7c8e9l,0x7a8a43f8ee11f227l,
            0x2ad8431a3e4056ebl },
          0 },
        /* 77 << 88 */
        { { 0xb8cf71ed031c1871l,0x702431806f703102l,0x9a87e1c24ec6f1b0l,
            0xf7e6e5b4664f275dl },
          { 0xc70a8b4e8c76b505l,0x6ba69bf2a002e9cfl,0x33ed74f7a0d8c9bfl,
            0x17f5f4b18d9989del },
          0 },
        /* 83 << 88 */
        { { 0xcd116dcb1b13a4a1l,0x591adb831c369877l,0x697be1aca6b8e80bl,
            0xb2d4baa1b975d781l },
          { 0xd4a9a496b16b48e7l,0x64de2d7af293997dl,0x039ae039af09a492l,
            0x66e31a2665f3a485l },
          0 },
        /* 89 << 88 */
        { { 0x110a8a42fec01a53l,0x1f5fcc1b38affab8l,0x757310ca9941a19el,
            0x11ef95f76c29d6cbl },
          { 0x0756bdb22dd427bal,0x8de8d44af3e16c33l,0xf9d28355e25aec52l,
            0xeb761efc02f36465l },
          0 },
        /* 95 << 88 */
        { { 0xfc83bf7454bfcd7al,0x51d861794837b6bel,0x8165b3f9801a324dl,
            0x3a5972bc634cfd61l },
          { 0xeecfe6d825258ed6l,0x51d968df1451ced0l,0x3010cdb8316aa0ael,
            0xc295b8522900eaf2l },
          0 },
        /* 101 << 88 */
        { { 0x5ad434a3890cc798l,0x4c17ff5e1531bce4l,0x825b5b5a5ea8e26fl,
            0xacca9d5dd66fd7b3l },
          { 0xb647dbde37ae6f92l,0xa5594868f3600416l,0x7b90ac53ab0c5d63l,
            0x4b66ad7ceb43e1d0l },
          0 },
        /* 107 << 88 */
        { { 0x04a211fac09ccbffl,0x9c96ad9ee873d898l,0x9eb1deb69c481f86l,
            0xb3616ce8b2d70298l },
          { 0x67a6fe9b9073726dl,0x5b8aa37d4c9bf744l,0xf558603ebb6aa0efl,
            0x72767f5103d304fbl },
          0 },
        /* 113 << 88 */
        { { 0x787cb8b8d6e9b7e3l,0x8bb30222e079fc68l,0x651a2ea6e3145a0bl,
            0x0254c5da9ab18fa8l },
          { 0x83722ffc12e1611fl,0xb0ddf1ffa7cc61bel,0x7c9c7e10ac0ac8d7l,
            0x8241a8191da12218l },
          0 },
        /* 116 << 88 */
        { { 0x70bb7719bc407e6el,0x231328efd84ceb41l,0x8bca6a1fc104bb20l,
            0xd6f4e425280b9071l },
          { 0xb41b95a292896a82l,0x735cf435fa34df67l,0xbc331a08d9d6d769l,
            0x579786052682747el },
          0 },
        /* 119 << 88 */
        { { 0x048ba499eb3af9a9l,0x43a8c367d50b82cel,0xedf9e2b21e0724d9l,
            0x3098aab3d607140bl },
          { 0xd1f18f1e5ed49eb9l,0xf9c6bb6ae0bb02a2l,0x204f96aa0cd245ddl,
            0xdaadaf4afb011ed5l },
          0 },
        /* 125 << 88 */
        { { 0xb298ce2de50404b1l,0x04dd38c45bf9b581l,0x229deabdfada51e8l,
            0x74bd233f8788a132l },
          { 0x951ba5ecf03e6c30l,0x9da2f5aa45bf1a41l,0x6bec7fea7e52b860l,
            0x76e3778964b0a9ddl },
          0 },
    },
    {
        /* 0 << 96 */
        { { 0x00, 0x00, 0x00, 0x00 },
          { 0x00, 0x00, 0x00, 0x00 },
          1 },
        /* 1 << 96 */
        { { 0x4fe7ee31b0e63d34l,0xf4600572a9e54fabl,0xc0493334d5e7b5a4l,
            0x8589fb9206d54831l },
          { 0xaa70f5cc6583553al,0x0879094ae25649e5l,0xcc90450710044652l,
            0xebb0696d02541c4fl },
          0 },
        /* 3 << 96 */
        { { 0xb99f0e0399375235l,0x7614c847b9917970l,0xfec93ce9524ec067l,
            0xe40e7bf89b122520l },
          { 0xb5670631ee4c4774l,0x6f03847a3b04914cl,0xc96e9429dc9dd226l,
            0x43489b6c8c57c1f8l },
          0 },
        /* 4 << 96 */
        { { 0x0e299d23fe67ba66l,0x9145076093cf2f34l,0xf45b5ea997fcf913l,
            0x5be008438bd7dddal },
          { 0x358c3e05d53ff04dl,0xbf7ccdc35de91ef7l,0xad684dbfb69ec1a0l,
            0x367e7cf2801fd997l },
          0 },
        /* 5 << 96 */
        { { 0x46ffd227cc2338fbl,0x89ff6fa990e26153l,0xbe570779331a0076l,
            0x43d241c506e1f3afl },
          { 0xfdcdb97dde9b62a3l,0x6a06e984a0ae30eal,0xc9bf16804fbddf7dl,
            0x170471a2d36163c4l },
          0 },
        /* 7 << 96 */
        { { 0x361619e455950cc3l,0xc71d665c56b66bb8l,0xea034b34afac6d84l,
            0xa987f832e5e4c7e3l },
          { 0xa07427727a79a6a7l,0x56e5d017e26d6c23l,0x7e50b97638167e10l,
            0xaa6c81efe88aa84el },
          0 },
        /* 9 << 96 */
        { { 0x473959d74d325bbfl,0x2a61beec8d6114b9l,0x25672a94924be2eel,
            0xa48595dbf2c23d0cl },
          { 0xe476848b6a221838l,0xe743e69a35c1b673l,0x2ab42499d8468503l,
            0x62aa0054e9e90ba7l },
          0 },
        /* 10 << 96 */
        { { 0x358d13f1bc482911l,0x685d1971b7fa7f26l,0x3e67a51d2be1aee4l,
            0xe041850998d114a9l },
          { 0x59639f604e052561l,0x32075c49155d0818l,0x2aa2343b67b64b1cl,
            0x1b445e2967f53e6al },
          0 },
        /* 11 << 96 */
        { { 0xbdfb271773a904e0l,0x7ce1e40b28888d73l,0x2e7e35f6eaa97d1bl,
            0xd061772aa9afa097l },
          { 0x434ac7c47a1f7c59l,0x6e21124ae79b7b9al,0x055acff3bb22ecc7l,
            0x8bfd7ac984c858d3l },
          0 },
        /* 13 << 96 */
        { { 0x2fd57df59f1f68adl,0x5ddcc6dbb06470c8l,0x801b6451a9b47307l,
            0x6b51c8e376551bf4l },
          { 0xef0bd1f7d44e1da9l,0x714bcb1d4d4e600cl,0xc57bb9e40c6540c7l,
            0x71bd1ec2327cc644l },
          0 },
        /* 15 << 96 */
        { { 0x9a52cf7e7f4dd81fl,0xa0132be15e69c05el,0x90dab7472a0f4d72l,
            0xc142f911312d6706l },
          { 0xe8d3631f8261998bl,0xf0f42fae615c1c94l,0x2f4e948caec3fa5dl,
            0x242ae7a8a374101el },
          0 },
        /* 16 << 96 */
        { { 0x0f893a5dc8de610bl,0xe8c515fb67e223cel,0x7774bfa64ead6dc5l,
            0x89d20f95925c728fl },
          { 0x7a1e0966098583cel,0xa2eedb9493f2a7d7l,0x1b2820974c304d4al,
            0x0842e3dac077282dl },
          0 },
        /* 17 << 96 */
        { { 0x1fa878cad088be52l,0x89c2cb07a9e1e656l,0x385bc5c3219d62dbl,
            0xd82b676b5fda2752l },
          { 0x2449dc9ee304eafcl,0x1e9e7991632f4ea2l,0x3036e061cdd5e0b9l,
            0x75a6f6ff830825bcl },
          0 },
        /* 19 << 96 */
        { { 0xb10fcddc449dedb4l,0x2c890042d1244acfl,0x9b3072cac7fc7017l,
            0x1acda6859ce8063fl },
          { 0xd243313c7f51e2f5l,0x52a3f1a4d73d9578l,0xda785b7a64f0ce6el,
            0x2e766315442a4c2dl },
          0 },
        /* 21 << 96 */
        { { 0x94f9b004151f111al,0xc7a5035b07dbc5fal,0x53958ea7609e49d7l,
            0x0526b4d79013f4c0l },
          { 0x66de5ebb593e2fbdl,0x6e7cf8b44c2e0c37l,0x6f72fc8b8c983e78l,
            0x6fab9b632348f9d7l },
          0 },
        /* 23 << 96 */
        { { 0xc748a3526a3d8468l,0x3fab479927e38032l,0x91ad3629fa430ce7l,
            0xc5af0b2c71614c44l },
          { 0xcede3fa50c211611l,0x6e6889ba02338083l,0xee0a195977f0fe32l,
            0x01ea905d0f4bbc5al },
          0 },
        /* 25 << 96 */
        { { 0x12cfb25e8193db48l,0xddb4ae633bea708cl,0xdaae102ef181f821l,
            0x9d9d923024a089d9l },
          { 0x71c4122da0876aeal,0x1a63ea3bbbe19c09l,0x3b898076016f8d0cl,
            0xa5cccc5daea6b713l },
          0 },
        /* 27 << 96 */
        { { 0xc3f22baf4a8e2f61l,0x77d29ede176da6a6l,0x40a55f211607da63l,
            0x858b38561452e391l },
          { 0x0dd3c267fe1b3c56l,0x66c04bdd7d55227al,0xfbd2fe55e6404e09l,
            0x5981cf49ea9cfcbcl },
          0 },
        /* 28 << 96 */
        { { 0xe549237f78890732l,0xc443bef953fcb4d9l,0x9884d8a6eb3480d6l,
            0x8a35b6a13048b186l },
          { 0xb4e4471665e9a90al,0x45bf380d653006c0l,0x8f3f820d4fe9ae3bl,
            0x244a35a0979a3b71l },
          0 },
        /* 29 << 96 */
        { { 0xae46a902aea870afl,0xa9b9fcf57cbedc99l,0x74f2ca3f79b7e793l,
            0xadb8f2231dbeeb28l },
          { 0x6302060e6764df85l,0x363320d257ebd554l,0xd9fd573e798d22e1l,
            0x285f85f5ebb67dedl },
          0 },
        /* 31 << 96 */
        { { 0xd86b329211caa2b5l,0x2a26258e39337bd1l,0x4dc5a9b579c8c291l,
            0x16443d87741942e6l },
          { 0x6bc9a2f8f811400cl,0x819c69359eeb4e0el,0xe1be7273ce0c214bl,
            0x429afb8184b61581l },
          0 },
        /* 33 << 96 */
        { { 0xb37e188756af5812l,0xd662bdb485aff83el,0xc89742d07bc63de7l,
            0xea103f9d0279f487l },
          { 0x4d26916a3a6cc639l,0x4eea3a3c7c743b94l,0x6a3e0dc7007376d9l,
            0xdb6ef3cf573f904el },
          0 },
        /* 34 << 96 */
        { { 0x9b1058ecb0b0fb53l,0x8955f5f75f8a9a9fl,0xf5f92e7f9f6f9e6dl,
            0x03f5df6c50ec198bl },
          { 0x6c8741f2b8aedbcel,0x8f4e60cfed8018f7l,0x6ca5297c9fa01f89l,
            0x8591cf7a864995dbl },
          0 },
        /* 35 << 96 */
        { { 0xa126147eb0a11b9bl,0xeedcc9e198900232l,0x15d94f8c2bead119l,
            0x042423cfefc38691l },
          { 0x6ce86fbe77165d91l,0xa07732126b3fd565l,0x8cdc409150b1f9c7l,
            0x7f5ad1af064595acl },
          0 },
        /* 36 << 96 */
        { { 0xed374a6658926dddl,0x138b2d49908015b8l,0x886c6579de1f7ab8l,
            0x888b9aa0c3020b7al },
          { 0xd3ec034e3a96e355l,0xba65b0b8f30fbe9al,0x064c8e50ff21367al,
            0x1f508ea40b04b46el },
          0 },
        /* 37 << 96 */
        { { 0x73644c158f8402a0l,0x0d9b5354f4730eb9l,0x78542af4e94cc278l,
            0xf4dbede3e395f33al },
          { 0x8fe8cbc590c70b00l,0x9c35bb2d7db197f6l,0x229b4973e6599746l,
            0x0817d04e1a84b986l },
          0 },
        /* 39 << 96 */
        { { 0x8ffe34e95ecd09b3l,0x6a7c3de4153b7cael,0xf02713e4a81044b7l,
            0x85ca6158c70545c8l },
          { 0xd3ff392845d88bffl,0x3a251a07f0bafe89l,0x61290e1287cea7f4l,
            0xa360a17efa4808adl },
          0 },
        /* 40 << 96 */
        { { 0x98561a49747c866cl,0xbbb1e5fe0518a062l,0x20ff4e8becdc3608l,
            0x7f55cded20184027l },
          { 0x8d73ec95f38c85f0l,0x5b589fdf8bc3b8c3l,0xbe95dd980f12b66fl,
            0xf5bd1a090e338e01l },
          0 },
        /* 41 << 96 */
        { { 0x2d1751083edf4e2bl,0x30e6e90fa29c10d0l,0xfee1eb14c9c6ccd2l,
            0x244670c756a81453l },
          { 0x90b33eefc5185c22l,0xd77ae4b63db82d28l,0xce5ee034f228f940l,
            0x5d7660847bb47be5l },
          0 },
        /* 43 << 96 */
        { { 0x88b7eec499b9a8c6l,0x56048d9e14e8ef0cl,0xa18f93215c89cf78l,
            0xbd2087616d327e66l },
          { 0x5b187225d9e53e27l,0xa57ca6c7bf4d0317l,0x187731d2e9557736l,
            0xd4ce2f78a874982el },
          0 },
        /* 44 << 96 */
        { { 0x65163ae55e915918l,0x6158d6d986f8a46bl,0x8466b538eeebf99cl,
            0xca8761f6bca477efl },
          { 0xaf3449c29ebbc601l,0xef3b0f41e0c3ae2fl,0xaa6c577d5de63752l,
            0xe916660164682a51l },
          0 },
        /* 45 << 96 */
        { { 0xf5b602bb29f47deal,0x42853c9659ddd679l,0x5c25be4041d7c001l,
            0x8e069399d4a3b307l },
          { 0x1782152e736ce467l,0x2e264109c9cb4f08l,0xf900cb11ab124698l,
            0x1bbed1d02d6e05b1l },
          0 },
        /* 46 << 96 */
        { { 0x9cc3fedc7da08b1fl,0x0f44949361d5ed38l,0xc8cbc4209b991b6bl,
            0xee62a342891c42e1l },
          { 0x11c496bb1a179139l,0x94ece2892eac4d8el,0x35f303a5a98d5570l,
            0x69d4340514a31552l },
          0 },
        /* 47 << 96 */
        { { 0x29d45e50892dfcbal,0x653e613e5c30cee3l,0x7b8c1ae61868a348l,
            0x40ab51654f2c612al },
          { 0x56e977f9891cdc8cl,0xee1ca12a34ca7cd1l,0xa4e283ee17b5ddf8l,
            0x4e36f2fb6f536205l },
          0 },
        /* 48 << 96 */
        { { 0x5a3097befc15aa1el,0x40d12548b54b0745l,0x5bad4706519a5f12l,
            0xed03f717a439dee6l },
          { 0x0794bb6c4a02c499l,0xf725083dcffe71d2l,0x2cad75190f3adcafl,
            0x7f68ea1c43729310l },
          0 },
        /* 49 << 96 */
        { { 0xa3834d85e89ea13fl,0x2ca00f942db803bbl,0x0f378681400ed3dal,
            0x1028af6b54854da3l },
          { 0x3928c2da06400c7fl,0x21119785d82aac92l,0x06618c17724e4af0l,
            0x22b42b161470736bl },
          0 },
        /* 51 << 96 */
        { { 0x7d0cfd48f7f2ac65l,0x46e1ac705f641b60l,0x0ab9566a0fcf0137l,
            0xbd4380e0db460fb8l },
          { 0x4550efbf6db99b55l,0x33846e669764b744l,0xacffa0cae34ca007l,
            0xce642d6a077e646cl },
          0 },
        /* 52 << 96 */
        { { 0xe747c8c7b7ffd977l,0xec104c3580761a22l,0x8395ebaf5a3ffb83l,
            0xfb3261f4e4b63db7l },
          { 0x53544960d883e544l,0x13520d708cc2eeb8l,0x08f6337bd3d65f99l,
            0x83997db2781cf95bl },
          0 },
        /* 53 << 96 */
        { { 0xd89112c47d8037a3l,0xcba48ad3464c2025l,0x3afea8399814a09dl,
            0x69e52260269030b5l },
          { 0x5b7067365c674805l,0x8c3fd33d87343f56l,0xc572c858b1c61edfl,
            0x43d8f4ded06749cbl },
          0 },
        /* 55 << 96 */
        { { 0x04da1f06b4066003l,0xf7d4e52f372749e8l,0x56cd667114b38747l,
            0x1943a22a22eb6d9el },
          { 0xc2c5391990714b0al,0xb6e3abb7d13cf3ael,0xfcd8d671676115cbl,
            0x178ce1a0c06a0d3al },
          0 },
        /* 57 << 96 */
        { { 0x94485b36913508f8l,0x92f87fe36de83b42l,0xedd476f0ed77e666l,
            0xee90fbc68da2cf53l },
          { 0x6f4afc53fc6cf3d9l,0x231bceb9f21f6ecfl,0x6504a11d494c6e9cl,
            0xd3728f032c211461l },
          0 },
        /* 59 << 96 */
        { { 0x09a9b93799562ca2l,0xb7d5c5cf6a5a5aa8l,0x52f5d7b9987b219dl,
            0x33849f9ec38014d4l },
          { 0x299adaf628f23880l,0x738ecc8874875588l,0x39d707adca2af665l,
            0xc8c11f688f4c5f73l },
          0 },
        /* 60 << 96 */
        { { 0x68e4f15e9afdfb3cl,0x49a561435bdfb6dfl,0xa9bc1bd45f823d97l,
            0xbceb5970ea111c2al },
          { 0x366b455fb269bbc4l,0x7cd85e1ee9bc5d62l,0xc743c41c4f18b086l,
            0xa4b4099095294fb9l },
          0 },
        /* 61 << 96 */
        { { 0x2ae046d66aa34757l,0x34db1addaa6d7e9dl,0x2b4b7e017ccf432bl,
            0xfbe0bfa590d319c6l },
          { 0xfb2981687ec7a7f2l,0x346cc46004f5132el,0x782b2e53b40aceddl,
            0x402e1d64e3f0b8b9l },
          0 },
        /* 63 << 96 */
        { { 0x2aa3b21d25a56088l,0xae6ee57543d08962l,0x669e42bff1e22297l,
            0x7b4c635732e3a47al },
          { 0x22b16260ea464a25l,0xad8ca59072d5cd7al,0x7c244266104eb96al,
            0x1def95e28e7c11d2l },
          0 },
        /* 64 << 96 */
        { { 0x9c7c581d26ee8382l,0xcf17dcc5359d638el,0xee8273abb728ae3dl,
            0x1d112926f821f047l },
          { 0x1149847750491a74l,0x687fa761fde0dfb9l,0x2c2580227ea435abl,
            0x6b8bdb9491ce7e3fl },
          0 },
        /* 65 << 96 */
        { { 0x1f04524cdc27e1f7l,0xa0c74f61572eab14l,0xdd5d0cfced272074l,
            0x95533c1d5bfe4f65l },
          { 0x3039d57ecce817cal,0x029967d73b822082l,0x9fca43866c4a10d3l,
            0xf8b2a7f0bb4968ebl },
          0 },
        /* 71 << 96 */
        { { 0x933cd6dcbfbf6407l,0xd08f21504be673f8l,0x0e1c4d0db1140a2el,
            0x0502a092431b270al },
          { 0x5d99f9508768c00al,0xda3ce5079b3ff3c7l,0x1c648b75031c11abl,
            0x5e3de47bf2776305l },
          0 },
        /* 77 << 96 */
        { { 0xe22af9274d2b9de4l,0xf3690f55a69609ecl,0x20260a6e453fbe18l,
            0x8edcb46b42d0b085l },
          { 0xd4ef250b7d9c7f58l,0x5e8578dfc83c3433l,0x9751d9b9e46e320al,
            0xb02bd03cf3c58af6l },
          0 },
        /* 83 << 96 */
        { { 0x0ab299ede1b4d1ccl,0x22e7301cec4d18d2l,0xf2380f2a7b86d4ffl,
            0xca19ef9e40753713l },
          { 0x52bb0d24678c38a1l,0xcc9d6fd499001c02l,0xa2dd6b00bc5876e4l,
            0xfe04b402409fe2b3l },
          0 },
        /* 89 << 96 */
        { { 0x7db986b1ff69f8d3l,0x648865e59d6266b9l,0x7ccfe96183f7dae5l,
            0x0f59a8bd6828379bl },
          { 0xad97e5ef0ac7c4e8l,0xa75914be784e9c18l,0x053e015bb18c1bb8l,
            0x18f6cefcb347043el },
          0 },
        /* 95 << 96 */
        { { 0xb4d641bdf257c38al,0xadcea4d0c1372574l,0x7f8d20be71c8f0d0l,
            0x14a1d24c41dc6344l },
          { 0xe446054e41f35526l,0x4664213823c952ddl,0xfbde483401f6b0acl,
            0xc89eee66d75b6318l },
          0 },
        /* 101 << 96 */
        { { 0x700242937a087392l,0xd42bd3aad5da04del,0xee64cb5b1f803414l,
            0xd6341ecbbab52988l },
          { 0x7ad522f343170a74l,0x5fba22536d61d9del,0x230304c1e845a6e5l,
            0xd69feabfbc9e326bl },
          0 },
        /* 107 << 96 */
        { { 0xef7e49412e8a11d7l,0x4cb8963662c8bae1l,0xecc741198aad5816l,
            0x13490782c7af5175l },
          { 0x10c701f73e91a604l,0xcb8c6c7124cc30c1l,0xce0d479c071eb382l,
            0xa3dc71fb058087d4l },
          0 },
        /* 113 << 96 */
        { { 0xec368492541eb6d1l,0x567735d6e09a94abl,0xb8039ec172350329l,
            0x3bd83a8f4894ddafl },
          { 0x740ef2a39c07063dl,0xba25e72277da7b59l,0xb09e248e3bf42e82l,
            0x7ff36da0b017d037l },
          0 },
        /* 116 << 96 */
        { { 0xca80416651b8d9a3l,0x42531bc90ffb0db1l,0x72ce4718aa82e7cel,
            0x6e199913df574741l },
          { 0xd5f1b13dd5d36946l,0x8255dc65f68f0194l,0xdc9df4cd8710d230l,
            0x3453c20f138c1988l },
          0 },
        /* 119 << 96 */
        { { 0x913f23b9ed08ac04l,0x18e336643590d098l,0xd3f72934e67536dcl,
            0xf949a757ec7ecde9l },
          { 0x37fc6583cf9cbd37l,0xcbe62cc043b1228el,0x777124948a743274l,
            0x3ea3668c716ce6f1l },
          0 },
        /* 125 << 96 */
        { { 0xc89ce010a90d375bl,0x39ac669340503fe3l,0x9036f782d33ecb0el,
            0x5190656841fdc7d1l },
          { 0xbefd136e917d94cdl,0x05fea2f22a511b24l,0x80e62d76f9076e0cl,
            0x8c57635e418ba653l },
          0 },
    },
    {
        /* 0 << 104 */
        { { 0x00, 0x00, 0x00, 0x00 },
          { 0x00, 0x00, 0x00, 0x00 },
          1 },
        /* 1 << 104 */
        { { 0x20d3c982cf7d62d2l,0x1f36e29d23ba8150l,0x48ae0bf092763f9el,
            0x7a527e6b1d3a7007l },
          { 0xb4a89097581a85e3l,0x1f1a520fdc158be5l,0xf98db37d167d726el,
            0x8802786e1113e862l },
          0 },
        /* 3 << 104 */
        { { 0xf6e894d1f4c6b6ecl,0x526b082718b3cd9bl,0x73f952a812117fbfl,
            0x2be864b011945bf5l },
          { 0x86f18ea542099b64l,0x2770b28a07548ce2l,0x97390f28295c1c9cl,
            0x672e6a43cb5206c3l },
          0 },
        /* 4 << 104 */
        { { 0xc37c7dd0c55c4496l,0xa6a9635725bbabd2l,0x5b7e63f2add7f363l,
            0x9dce37822e73f1dfl },
          { 0xe1e5a16ab2b91f71l,0xe44898235ba0163cl,0xf2759c32f6e515adl,
            0xa5e2f1f88615eecfl },
          0 },
        /* 5 << 104 */
        { { 0xcacce2c847c64367l,0x6a496b9f45af4ec0l,0x2a0836f36034042cl,
            0x14a1f3900b6c62eal },
          { 0xe7fa93633ef1f540l,0xd323b30a72a76d93l,0xffeec8b50feae451l,
            0x4eafc172bd04ef87l },
          0 },
        /* 7 << 104 */
        { { 0xe4435a51b3e59b89l,0x136139554133a1c9l,0x87f46973440bee59l,
            0x714710f800c401e4l },
          { 0xc0cf4bced6c446c9l,0xe0aa7fd66c4d5368l,0xde5d811afc68fc37l,
            0x61febd72b7c2a057l },
          0 },
        /* 9 << 104 */
        { { 0x27375fe665f837e2l,0x93f8c68bd882179fl,0x584feadc59b16187l,
            0xe5b50be9483bc162l },
          { 0x7ad9d6f1a2776625l,0xe9d1008004ff457bl,0x5b56d322677618a6l,
            0x036694eae3e68673l },
          0 },
        /* 10 << 104 */
        { { 0x6ca4f87e822e37bel,0x73f237b4253bda4el,0xf747f3a241190aebl,
            0xf06fa36f804cf284l },
          { 0x0a6bbb6efc621c12l,0x5d624b6440b80ec6l,0x4b0724257ba556f3l,
            0x7fa0c3543e2d20a8l },
          0 },
        /* 11 << 104 */
        { { 0x6feaffc51d8a4fd1l,0x59663b205f1ad208l,0xefc93cef24acb46al,
            0x54929de05967118cl },
          { 0x885708009acffb1cl,0x492bbf2b145639ecl,0x71f495a638f0018el,
            0xe24365dbc2792847l },
          0 },
        /* 13 << 104 */
        { { 0x4bedae86a6f29002l,0x7abedb56e034457al,0x8bf3eec6179bff2al,
            0x9d626d57390f4e6bl },
          { 0x653fe0e914dd6ea3l,0x7483715989bd6d08l,0x85fb05b4ebd9b03dl,
            0x7dc3f2214a768bbcl },
          0 },
        /* 15 << 104 */
        { { 0xaacc63f132b0ed8fl,0x041237242bafefd2l,0x0df9a7987e2d2a13l,
            0x09bd13cf9c27591fl },
          { 0xaa5f5e476e1afb50l,0xcd146a42b66eb646l,0x3f07561d1442ec3cl,
            0x7e5471738ae8ec47l },
          0 },
        /* 16 << 104 */
        { { 0x8de2b7bc453cadd6l,0x203900a7bc0bc1f8l,0xbcd86e47a6abd3afl,
            0x911cac128502effbl },
          { 0x2d550242ec965469l,0x0e9f769229e0017el,0x633f078f65979885l,
            0xfb87d4494cf751efl },
          0 },
        /* 17 << 104 */
        { { 0x2c3e61196c0c6cd5l,0x5e01a49a99f4aac8l,0xfa518fc92ef1565el,
            0xf64ff8714f772366l },
          { 0x52fcbc2b726420d0l,0x30fbf6eb76cfa9eel,0x0bd17139fa618268l,
            0x23ed6e122087535dl },
          0 },
        /* 19 << 104 */
        { { 0x76098e38bb4ccb2cl,0x44e88aeeafbad6d1l,0x5c4d286771928778l,
            0xb1df868138534c94l },
          { 0x67eb8f4d77ce9debl,0x2a86d0461a77c55dl,0xc327181e46a6a3e7l,
            0x68fd611b8710e206l },
          0 },
        /* 21 << 104 */
        { { 0xc093f3fc0c82bdf1l,0x21db25894f76c4a6l,0xf3dcb22ee410a7ael,
            0x1db37114f3c22ffel },
          { 0x9bd0a1fb58f6801dl,0x2cab103bd1b55cc8l,0x2ae1a7f5077ba4b2l,
            0x82b46642ce5ab2b3l },
          0 },
        /* 23 << 104 */
        { { 0xc8477ec52546684cl,0xe3f9387702ff02b5l,0xefb72133ae5d04cdl,
            0x644905c339f10d02l },
          { 0x1750c87c13d8d356l,0x0e9b8063b41e7640l,0xc7ece04f5647b05bl,
            0x89a43da7ca9df9c4l },
          0 },
        /* 25 << 104 */
        { { 0x02610ef1920eb7d9l,0x34bd2fc2e1ea1dc0l,0xcb89da255170b890l,
            0xaaa2796461cff827l },
          { 0xc308c9d37103ed6al,0xe82d63d5a467564al,0x94c897c4a0fa7732l,
            0x75eb52fa64c7aa5fl },
          0 },
        /* 27 << 104 */
        { { 0x52582f9cb985fcb6l,0xaaef8d9f8508a691l,0x494c2c346e505131l,
            0x6d062362d55f30f6l },
          { 0x70059e9122e1e32fl,0x1507c3fe9e51abb0l,0xd8aba31b2b7bda72l,
            0x5acbc5f77b753f13l },
          0 },
        /* 28 << 104 */
        { { 0x15bfb8bf5116f937l,0x7c64a586c1268943l,0x71e25cc38419a2c8l,
            0x9fd6b0c48335f463l },
          { 0x4bf0ba3ce8ee0e0el,0x6f6fba60298c21fal,0x57d57b39ae66bee0l,
            0x292d513022672544l },
          0 },
        /* 29 << 104 */
        { { 0x075dc81953952ff6l,0xd4d9eeda20b7384dl,0x8a81c1bfd2d6c6a5l,
            0x319368a0db050f3bl },
          { 0x91f476de31f1cee2l,0x1b38604500d0e17fl,0xed2081889a820384l,
            0x8d00c411a0f1a637l },
          0 },
        /* 31 << 104 */
        { { 0xb029b687a47fd8f0l,0xa531360696371a05l,0x7b84e88c5ab09140l,
            0x87dad7c85eeb1d14l },
          { 0xef0749b9d0edf6f3l,0x29fc7310e2ef198bl,0x01e05df5069ed399l,
            0x121db4ecdf4e2fcal },
          0 },
        /* 33 << 104 */
        { { 0xe730f3f62826bee0l,0xb9bdbe3fce332a8fl,0x1ecad11766ec00aal,
            0x7503d835617a62d1l },
          { 0x9f34e161b862b139l,0xde42194cf30f6a67l,0x5037a953c1e879fel,
            0x62f321f89bda45dbl },
          0 },
        /* 34 << 104 */
        { { 0xe87771d8033f2876l,0xb0186ec67d5cc3dbl,0x58e8bb803bc9bc1dl,
            0x4d1395cc6f6ef60el },
          { 0xa73c62d6186244a0l,0x918e5f23110a5b53l,0xed4878ca741b7eabl,
            0x3038d71adbe03e51l },
          0 },
        /* 35 << 104 */
        { { 0xcbdba27c40234d55l,0x24352b6cb3eb56c9l,0xae681b85a8e9295al,
            0x2a6cfba1f1171664l },
          { 0x49f045838ca40c3cl,0xe56da25c6eb0f8eal,0x8e62f86fc4341a4el,
            0x7f68bdc64c3f947fl },
          0 },
        /* 36 << 104 */
        { { 0x840204b7a93c3246l,0x21ab6069a0b9b4cdl,0xf5fa6e2bb1d64218l,
            0x1de6ad0ef3d56191l },
          { 0x570aaa88ff1929c7l,0xc6df4c6b640e87b5l,0xde8a74f2c65f0cccl,
            0x8b972fd5e6f6cc01l },
          0 },
        /* 37 << 104 */
        { { 0x862013c00bf22173l,0xfd004c834acd8e23l,0x50e422ca310b1649l,
            0xe6d04de65bbe1854l },
          { 0x651f646385761ef3l,0x3b17d38652cf85c9l,0xbdce284a5f54ecc7l,
            0x72efcd3ec7c2106cl },
          0 },
        /* 39 << 104 */
        { { 0x34324b182ff07e3el,0x29938f38f50bcb71l,0xd0e3d7b977e2bcc3l,
            0x8e78f007c0a3292bl },
          { 0xfa28c530005c2c00l,0x6f9c21d51faa0c5al,0x3df01abd7b9c78f3l,
            0x0e5618c1ccaaeb7el },
          0 },
        /* 40 << 104 */
        { { 0xaa6778fce7560b90l,0xb4073e61a7e824cel,0xff0d693cd642eba8l,
            0x7ce2e57a5dccef38l },
          { 0x89c2c7891df1ad46l,0x83a06922098346fdl,0x2d715d72da2fc177l,
            0x7b6dd71d85b6cf1dl },
          0 },
        /* 41 << 104 */
        { { 0x4601a6a492ad3889l,0xdc8e3364d9a0709fl,0x0c687f2b2c260327l,
            0xe882af62e1a79573l },
          { 0x0cfd00ab945d9017l,0xe6df7505d0e3c188l,0xb389a66dbde825a2l,
            0x126d77b6bcd8e14fl },
          0 },
        /* 43 << 104 */
        { { 0xc800acc7db18ec73l,0x0ebecc78d86e99efl,0x675796cdbd05bc5fl,
            0x254498126afd7c7fl },
          { 0x96293b695969b165l,0xd8514d83c162c8dal,0xe174f8b674a15a5cl,
            0x880d687389a2f73cl },
          0 },
        /* 44 << 104 */
        { { 0x53703a328300129fl,0x1f63766268c43bfdl,0xbcbd191300e54051l,
            0x812fcc627bf5a8c5l },
          { 0x3f969d5f29fb85dal,0x72f4e00a694759e8l,0x426b6e52790726b7l,
            0x617bbc873bdbb209l },
          0 },
        /* 45 << 104 */
        { { 0xf536f07cad1deb2el,0x2a13a11ea87a710el,0x0ce2ccab64f4dc96l,
            0x16178694f5a55464l },
          { 0x1496168da2cb3986l,0xb079a5b9d56a93a9l,0x97005e99092893d3l,
            0x55df5ed6e8fcc6c3l },
          0 },
        /* 46 << 104 */
        { { 0x511f8bb997aee317l,0x812a4096e81536a8l,0x137dfe593ac09b9bl,
            0x0682238fba8c9a7al },
          { 0x7072ead6aeccb4bdl,0x6a34e9aa692ba633l,0xc82eaec26fff9d33l,
            0xfb7535121d4d2b62l },
          0 },
        /* 47 << 104 */
        { { 0x821dca8bbf328b1cl,0x24596ddd5a3d6830l,0x061c4c15635b5b4cl,
            0x0e2b3bef4fa3560al },
          { 0xffced37498906c43l,0x10ebd174e26b3784l,0x7cd068c470039bb5l,
            0xc47dda0f88404e59l },
          0 },
        /* 48 << 104 */
        { { 0x1a0445ff1d7aadabl,0x65d38260d5f6a67cl,0x6e62fb0891cfb26fl,
            0xef1e0fa55c7d91d6l },
          { 0x47e7c7ba33db72cdl,0x017cbc09fa7c74b2l,0x3c931590f50a503cl,
            0xcac54f60616baa42l },
          0 },
        /* 49 << 104 */
        { { 0x7ad7d13569185235l,0x19771949fb69e030l,0xd4de9717bc45fb4fl,
            0x5657b076167e5739l },
          { 0x9503a71fdd27449el,0xfa2fabf73cc01347l,0xf8ecef24c83fb301l,
            0x527012bd5a8d5078l },
          0 },
        /* 51 << 104 */
        { { 0x70a550d7e6fc3a32l,0x8e5875841951fe57l,0x5e6d43eaaab9788bl,
            0x1e406fed80599794l },
          { 0xd8164ace9ed2557cl,0xf9648f30ff593e10l,0x53af2fd80c2ff879l,
            0x6705993cc9409bf4l },
          0 },
        /* 52 << 104 */
        { { 0x04b005b6c6458293l,0x36bb5276e8d10af7l,0xacf2dc138ee617b8l,
            0x470d2d35b004b3d4l },
          { 0x06790832feeb1b77l,0x2bb75c3985657f9cl,0xd70bd4edc0f60004l,
            0xfe797ecc219b018bl },
          0 },
        /* 53 << 104 */
        { { 0xeca02ebf0ef19ceel,0xac691fbe2de090a4l,0x1f3866641b374547l,
            0xbd8018c6a12ee85fl },
          { 0x3e851318ee63e0f1l,0x45b0c37a161987d3l,0x67fe36056eb567c4l,
            0x07c291b563200c5bl },
          0 },
        /* 55 << 104 */
        { { 0xc85535ac1a956a8al,0x7bf4d70bc0ade321l,0xaf2efc48237bc56fl,
            0xf9bfe13e31ba97e7l },
          { 0x2ca5fac4cf7c6c65l,0xc23b14ff03ec3e35l,0xc5109923217bcfd2l,
            0xf02f96a1c58f32f3l },
          0 },
        /* 57 << 104 */
        { { 0x3b1f715b0d0aeff4l,0xbe406d62f0d44536l,0xe413843d567bcb38l,
            0x75b7fb43791e705al },
          { 0x5b831d4b224f85e5l,0x3fea6659d9a35eael,0xd6f8bd097c85480bl,
            0x2a9561a34a959267l },
          0 },
        /* 59 << 104 */
        { { 0x4a96a3535a303c10l,0x9aa3ad71c37c8d7el,0x4e2d077fde52014fl,
            0x4d8bec5df8e3964dl },
          { 0xda88ab94e865e142l,0x52df506d10a88091l,0x9aebff0092fc38a2l,
            0xdfc034395608b0a2l },
          0 },
        /* 60 << 104 */
        { { 0xee23fa819966e7eel,0x64ec4aa805b7920dl,0x2d44462d2d90aad4l,
            0xf44dd195df277ad5l },
          { 0x8d6471f1bb46b6a1l,0x1e65d313fd885090l,0x33a800f513a977b4l,
            0xaca9d7210797e1efl },
          0 },
        /* 61 << 104 */
        { { 0xb1557be2a4ea787el,0x59324973019f667fl,0x262ceced5595367cl,
            0x8a676897ec598640l },
          { 0x2df6cebfc7f06f4fl,0xb255723138078f9al,0xad553c46524a0dd1l,
            0xe20bb20a5a68d62al },
          0 },
        /* 63 << 104 */
        { { 0x6f47e3779589e263l,0x7cb83e3d35106bb8l,0x2642d87bcc632fc2l,
            0x4d18f34d8b77eb36l },
          { 0x7de6bf6d19ca4d1cl,0x438e8f02f7e926aal,0xb539021250ac930al,
            0xe34ddfc15b219a9fl },
          0 },
        /* 64 << 104 */
        { { 0x98857ceb1bf4581cl,0xe635e186aca7b166l,0x278ddd22659722acl,
            0xa0903c4c1db68007l },
          { 0x366e458948f21402l,0x31b49c14b96abda2l,0x329c4b09e0403190l,
            0x97197ca3d29f43fel },
          0 },
        /* 65 << 104 */
        { { 0xfe4de13781479db4l,0x307331f012f08ea5l,0x7f59a64758c04c13l,
            0x6b41189abdc9b3c9l },
          { 0xb10f11e5a6f8c5edl,0x757fb7a3f5b0579el,0x456d0a873c90d027l,
            0x7e8bb6bf32361796l },
          0 },
        /* 71 << 104 */
        { { 0x6aa1dc6c9e689d8dl,0xaa5fa015479cdd09l,0x7eb4dbb582fc000al,
            0x4a57b689eff4e701l },
          { 0x7bfe8d2a8e15cd8cl,0xab109b1cc9074e1al,0x5716715fee1619a5l,
            0xf29a51eccdcb40bcl },
          0 },
        /* 77 << 104 */
        { { 0x14c76234ddf03c6el,0xdfb5d388baeb2eddl,0x4bd85da26d413d2dl,
            0x5b0dd9be3ae38469l },
          { 0xe4d8a9d89ab3ae61l,0xb9e37b880ee63951l,0x17f08e9b21a7f30fl,
            0x173db1e8119af788l },
          0 },
        /* 83 << 104 */
        { { 0x2352ad4a170d43f6l,0x098d74f65a0ae4b0l,0x290f5236c3a46c2al,
            0xea9266102dd87e7fl },
          { 0xd7ee90f6848e6911l,0xebe8f4cce0d8886fl,0xa2038320558ff6a0l,
            0x1f716534f37c38cfl },
          0 },
        /* 89 << 104 */
        { { 0x9754209439a4a159l,0xe6135412fed24278l,0xbba62254d70e2cabl,
            0x4ac6a8ac85895130l },
          { 0xc01614fee1a45363l,0x720ad3f8b67294f2l,0x724ea95cb420ea51l,
            0x1f40ab2d712b856cl },
          0 },
        /* 95 << 104 */
        { { 0x708e1c7975f3d30cl,0x423f1535e2172da3l,0x7a29be342a06a0b1l,
            0x9de5c9eb32c68ba2l },
          { 0x70217b0232d48793l,0x3cf3855bac1471cfl,0x6762d03f8321e179l,
            0x06ee12ea236fa7cfl },
          0 },
        /* 101 << 104 */
        { { 0x1718e7428779109bl,0x6188008d0aca350bl,0xbbe227e00594bc15l,
            0x4a7b6423ddbdea35l },
          { 0x06ad632dfa44e1bfl,0xaf9c163d1e97b409l,0x64dafec3c61f2b2fl,
            0xc6759d905525c0c9l },
          0 },
        /* 107 << 104 */
        { { 0x76d6294787517149l,0x2bda339baa77d325l,0x04b1bec067ad1fd1l,
            0x49f63fcc0aec7c73l },
          { 0x005cb459ec1bf494l,0x8fa99c1b1ec6f8bbl,0x70a4e6d78b59dd43l,
            0xfd70bcb313d6594dl },
          0 },
        /* 113 << 104 */
        { { 0x2987a7cb13966c11l,0x74ad0a26a783f283l,0xf011200ae54d27f0l,
            0xbd8632963fb38396l },
          { 0x7ec7fe8c9b86d059l,0xfa94ca76d0cd33a7l,0xf6ad741cdc646993l,
            0x83054a427ebc34e9l },
          0 },
        /* 116 << 104 */
        { { 0xadef8c5a192ef710l,0x88afbd4b3b7431f9l,0x7e1f740764250c9el,
            0x6e31318db58bec07l },
          { 0xfd4fc4b824f89b4el,0x65a5dd8848c36a2al,0x4f1eccfff024baa7l,
            0x22a21cf2cba94650l },
          0 },
        /* 119 << 104 */
        { { 0x7b45865478f39754l,0xcbb8b96c4564e003l,0xb492d2bf69b35752l,
            0x4e6287e065ee5ad3l },
          { 0x07906c14eb1ffe62l,0xf350390c681fcdf8l,0xc351386f6be3eec3l,
            0x8480d00ee5df919dl },
          0 },
        /* 125 << 104 */
        { { 0x399861ecf8a2d5aal,0xb179adeb046f78cbl,0x056a6cd88792f647l,
            0xd3dfc91c3d411820l },
          { 0x4ccf92d179693be1l,0x12ecd9a3f65cb250l,0x58e5d2102538b9e7l,
            0x4e655882ff977ccal },
          0 },
    },
    {
        /* 0 << 112 */
        { { 0x00, 0x00, 0x00, 0x00 },
          { 0x00, 0x00, 0x00, 0x00 },
          1 },
        /* 1 << 112 */
        { { 0x8ce9b6bfc360e25al,0xe6425195075a1a78l,0x9dc756a8481732f4l,
            0x83c0440f5432b57al },
          { 0xc670b3f1d720281fl,0x2205910ed135e051l,0xded14b0edb052be7l,
            0x697b3d27c568ea39l },
          0 },
        /* 3 << 112 */
        { { 0x0b89de9314092ebbl,0xf17256bd428e240cl,0xcf89a7f393d2f064l,
            0x4f57841ee1ed3b14l },
          { 0x4ee14405e708d855l,0x856aae7203f1c3d0l,0xc8e5424fbdd7eed5l,
            0x3333e4ef73ab4270l },
          0 },
        /* 4 << 112 */
        { { 0x3bc77adedda492f8l,0xc11a3aea78297205l,0x5e89a3e734931b4cl,
            0x17512e2e9f5694bbl },
          { 0x5dc349f3177bf8b6l,0x232ea4ba08c7ff3el,0x9c4f9d16f511145dl,
            0xccf109a333b379c3l },
          0 },
        /* 5 << 112 */
        { { 0xe75e7a88a1f25897l,0x7ac6961fa1b5d4d8l,0xe3e1077308f3ed5cl,
            0x208a54ec0a892dfbl },
          { 0xbe826e1978660710l,0x0cf70a97237df2c8l,0x418a7340ed704da5l,
            0xa3eeb9a908ca33fdl },
          0 },
        /* 7 << 112 */
        { { 0xb4323d588434a920l,0xc0af8e93622103c5l,0x667518ef938dbf9al,
            0xa184307383a9cdf2l },
          { 0x350a94aa5447ab80l,0xe5e5a325c75a3d61l,0x74ba507f68411a9el,
            0x10581fc1594f70c5l },
          0 },
        /* 9 << 112 */
        { { 0x5aaa98a7cb0c9c8cl,0x75105f3081c4375cl,0xceee50575ef1c90fl,
            0xb31e065fc23a17bfl },
          { 0x5364d275d4b6d45al,0xd363f3ad62ec8996l,0xb5d212394391c65bl,
            0x84564765ebb41b47l },
          0 },
        /* 10 << 112 */
        { { 0x20d18ecc37107c78l,0xacff3b6b570c2a66l,0x22f975d99bd0d845l,
            0xef0a0c46ba178fa0l },
          { 0x1a41965176b6028el,0xc49ec674248612d4l,0x5b6ac4f27338af55l,
            0x06145e627bee5a36l },
          0 },
        /* 11 << 112 */
        { { 0x33e95d07e75746b5l,0x1c1e1f6dc40c78bel,0x967833ef222ff8e2l,
            0x4bedcf6ab49180adl },
          { 0x6b37e9c13d7a4c8al,0x2748887c6ddfe760l,0xf7055123aa3a5bbcl,
            0x954ff2257bbb8e74l },
          0 },
        /* 13 << 112 */
        { { 0x4e23ca446d3fea55l,0xb4ae9c86f4810568l,0x47bfb91b2a62f27dl,
            0x60deb4c9d9bac28cl },
          { 0xa892d8947de6c34cl,0x4ee682594494587dl,0x914ee14e1a3f8a5bl,
            0xbb113eaa28700385l },
          0 },
        /* 15 << 112 */
        { { 0xef9dc899a7b56eafl,0x00c0e52c34ef7316l,0x5b1e4e24fe818a86l,
            0x9d31e20dc538be47l },
          { 0x22eb932d3ed68974l,0xe44bbc087c4e87c4l,0x4121086e0dde9aefl,
            0x8e6b9cff134f4345l },
          0 },
        /* 16 << 112 */
        { { 0x96892c1f711b0eb9l,0xb905f2c8780ab954l,0xace26309a20792dbl,
            0xec8ac9b30684e126l },
          { 0x486ad8b6b40a2447l,0x60121fc19fe3fb24l,0x5626fccf1a8e3b3fl,
            0x4e5686226ad1f394l },
          0 },
        /* 17 << 112 */
        { { 0xda7aae0d196aa5a1l,0xe0df8c771041b5fbl,0x451465d926b318b7l,
            0xc29b6e557ab136e9l },
          { 0x2c2ab48b71148463l,0xb5738de364454a76l,0x54ccf9a05a03abe4l,
            0x377c02960427d58el },
          0 },
        /* 19 << 112 */
        { { 0x90e4f7c92d7d1413l,0x67e2d6b59834f597l,0x4fd4f4f9a808c3e8l,
            0xaf8237e0d5281ec1l },
          { 0x25ab5fdc84687ceel,0xc5ded6b1a5b26c09l,0x8e4a5aecc8ea7650l,
            0x23b73e5c14cc417fl },
          0 },
        /* 21 << 112 */
        { { 0xb4293fdcf50225f9l,0xc52e175cb0e12b03l,0xf649c3bad0a8bf64l,
            0x745a8fefeb8ae3c6l },
          { 0x30d7e5a358321bc3l,0xb1732be70bc4df48l,0x1f217993e9ea5058l,
            0xf7a71cde3e4fd745l },
          0 },
        /* 23 << 112 */
        { { 0xa188b2502d0f39aal,0x622118bb15a85947l,0x2ebf520ffde0f4fal,
            0xa40e9f294860e539l },
          { 0x7b6a51eb22b57f0fl,0x849a33b97e80644al,0x50e5d16f1cf095fel,
            0xd754b54eec55f002l },
          0 },
        /* 25 << 112 */
        { { 0xcd821dfb988baf01l,0xe6331a7ddbb16647l,0x1eb8ad33094cb960l,
            0x593cca38c91bbca5l },
          { 0x384aac8d26567456l,0x40fa0309c04b6490l,0x97834cd6dab6c8f6l,
            0x68a7318d3f91e55fl },
          0 },
        /* 27 << 112 */
        { { 0xc7bfd486605daaa6l,0x46fd72b7bb9a6c9el,0xe4847fb1a124fb89l,
            0x75959cbda2d8ffbcl },
          { 0x42579f65c8a588eel,0x368c92e6b80b499dl,0xea4ef6cd999a5df1l,
            0xaa73bb7f936fe604l },
          0 },
        /* 28 << 112 */
        { { 0xf347a70d6457d188l,0x86eda86b8b7a388bl,0xb7cdff060ccd6013l,
            0xbeb1b6c7d0053fb2l },
          { 0x0b02238799240a9fl,0x1bbb384f776189b2l,0x8695e71e9066193al,
            0x2eb5009706ffac7el },
          0 },
        /* 29 << 112 */
        { { 0x0654a9c04a7d2caal,0x6f3fb3d1a5aaa290l,0x835db041ff476e8fl,
            0x540b8b0bc42295e4l },
          { 0xa5c73ac905e214f5l,0x9a74075a56a0b638l,0x2e4b1090ce9e680bl,
            0x57a5b4796b8d9afal },
          0 },
        /* 31 << 112 */
        { { 0x2a2bfa7f650006f0l,0xdfd7dad350c0fbb2l,0x92452495ccf9ad96l,
            0x183bf494d95635f9l },
          { 0x02d5df434a7bd989l,0x505385cca5431095l,0xdd98e67dfd43f53el,
            0xd61e1a6c500c34a9l },
          0 },
        /* 33 << 112 */
        { { 0x41d85ea1ef74c45bl,0x2cfbfa66ae328506l,0x98b078f53ada7da9l,
            0xd985fe37ec752fbbl },
          { 0xeece68fe5a0148b4l,0x6f9a55c72d78136dl,0x232dccc4d2b729cel,
            0xa27e0dfd90aafbc4l },
          0 },
        /* 34 << 112 */
        { { 0x9647445212b4603el,0xa876c5516b706d14l,0xdf145fcf69a9d412l,
            0xe2ab75b72d479c34l },
          { 0x12df9a761a23ff97l,0xc61389925d359d10l,0x6e51c7aefa835f22l,
            0x69a79cb1c0fcc4d9l },
          0 },
        /* 35 << 112 */
        { { 0xf57f350d594cc7e1l,0x3079ca633350ab79l,0x226fb6149aff594al,
            0x35afec026d59a62bl },
          { 0x9bee46f406ed2c6el,0x58da17357d939a57l,0x44c504028fd1797el,
            0xd8853e7c5ccea6cal },
          0 },
        /* 36 << 112 */
        { { 0x4065508da35fcd5fl,0x8965df8c495ccaebl,0x0f2da85012e1a962l,
            0xee471b94c1cf1cc4l },
          { 0xcef19bc80a08fb75l,0x704958f581de3591l,0x2867f8b23aef4f88l,
            0x8d749384ea9f9a5fl },
          0 },
        /* 37 << 112 */
        { { 0x1b3855378c9049f4l,0x5be948f37b92d8b6l,0xd96f725db6e2bd6bl,
            0x37a222bc958c454dl },
          { 0xe7c61abb8809bf61l,0x46f07fbc1346f18dl,0xfb567a7ae87c0d1cl,
            0x84a461c87ef3d07al },
          0 },
        /* 39 << 112 */
        { { 0x3ab3d5afbd76e195l,0x478dd1ad6938a810l,0x6ffab3936ee3d5cbl,
            0xdfb693db22b361e4l },
          { 0xf969449651dbf1a7l,0xcab4b4ef08a2e762l,0xe8c92f25d39bba9al,
            0x850e61bcf1464d96l },
          0 },
        /* 40 << 112 */
        { { 0xb7e830e3dc09508bl,0xfaf6d2cf74317655l,0x72606cebdf690355l,
            0x48bb92b3d0c3ded6l },
          { 0x65b754845c7cf892l,0xf6cd7ac9d5d5f01fl,0xc2c30a5996401d69l,
            0x91268650ed921878l },
          0 },
        /* 41 << 112 */
        { { 0x380bf913b78c558fl,0x43c0baebc8afdaa9l,0x377f61d554f169d3l,
            0xf8da07e3ae5ff20bl },
          { 0xb676c49da8a90ea8l,0x81c1ff2b83a29b21l,0x383297ac2ad8d276l,
            0x3001122fba89f982l },
          0 },
        /* 43 << 112 */
        { { 0xbbe1e6a6c93f72d6l,0xd5f75d12cad800eal,0xfa40a09fe7acf117l,
            0x32c8cdd57581a355l },
          { 0x742219927023c499l,0xa8afe5d738ec3901l,0x5691afcba90e83f0l,
            0x41bcaa030b8f8eacl },
          0 },
        /* 44 << 112 */
        { { 0xe38b5ff98d2668d5l,0x0715281a7ad81965l,0x1bc8fc7c03c6ce11l,
            0xcbbee6e28b650436l },
          { 0x06b00fe80cdb9808l,0x17d6e066fe3ed315l,0x2e9d38c64d0b5018l,
            0xab8bfd56844dcaefl },
          0 },
        /* 45 << 112 */
        { { 0x42894a59513aed8bl,0xf77f3b6d314bd07al,0xbbdecb8f8e42b582l,
            0xf10e2fa8d2390fe6l },
          { 0xefb9502262a2f201l,0x4d59ea5050ee32b0l,0xd87f77286da789a8l,
            0xcf98a2cff79492c4l },
          0 },
        /* 46 << 112 */
        { { 0xf9577239720943c2l,0xba044cf53990b9d0l,0x5aa8e82395f2884al,
            0x834de6ed0278a0afl },
          { 0xc8e1ee9a5f25bd12l,0x9259ceaa6f7ab271l,0x7e6d97a277d00b76l,
            0x5c0c6eeaa437832al },
          0 },
        /* 47 << 112 */
        { { 0x5232c20f5606b81dl,0xabd7b3750d991ee5l,0x4d2bfe358632d951l,
            0x78f8514698ed9364l },
          { 0x951873f0f30c3282l,0x0da8ac80a789230bl,0x3ac7789c5398967fl,
            0xa69b8f7fbdda0fb5l },
          0 },
        /* 48 << 112 */
        { { 0xe5db77176add8545l,0x1b71cb6672c49b66l,0xd856073968421d77l,
            0x03840fe883e3afeal },
          { 0xb391dad51ec69977l,0xae243fb9307f6726l,0xc88ac87be8ca160cl,
            0x5174cced4ce355f4l },
          0 },
        /* 49 << 112 */
        { { 0x98a35966e58ba37dl,0xfdcc8da27817335dl,0x5b75283083fbc7bfl,
            0x68e419d4d9c96984l },
          { 0x409a39f402a40380l,0x88940faf1fe977bcl,0xc640a94b8f8edea6l,
            0x1e22cd17ed11547dl },
          0 },
        /* 51 << 112 */
        { { 0x17ba93b1a20ef103l,0xad8591306ba6577bl,0x65c91cf66fa214a0l,
            0xd7d49c6c27990da5l },
          { 0xecd9ec8d20bb569dl,0xbd4b2502eeffbc33l,0x2056ca5a6bed0467l,
            0x7916a1f75b63728cl },
          0 },
        /* 52 << 112 */
        { { 0xd4f9497d53a4f566l,0x8973466497b56810l,0xf8e1da740494a621l,
            0x82546a938d011c68l },
          { 0x1f3acb19c61ac162l,0x52f8fa9cabad0d3el,0x15356523b4b7ea43l,
            0x5a16ad61ae608125l },
          0 },
        /* 53 << 112 */
        { { 0xb0bcb87f4faed184l,0x5f236b1d5029f45fl,0xd42c76070bc6b1fcl,
            0xc644324e68aefce3l },
          { 0x8e191d595c5d8446l,0xc020807713ae1979l,0xadcaee553ba59cc7l,
            0x20ed6d6ba2cb81bal },
          0 },
        /* 55 << 112 */
        { { 0x7392b41a530ccbbdl,0x87c82146ea823525l,0xa52f984c05d98d0cl,
            0x2ae57d735ef6974cl },
          { 0x9377f7bf3042a6ddl,0xb1a007c019647a64l,0xfaa9079a0cca9767l,
            0x3d81a25bf68f72d5l },
          0 },
        /* 57 << 112 */
        { { 0xc110d830b0f2ac95l,0x48d0995aab20e64el,0x0f3e00e17729cd9al,
            0x2a570c20dd556946l },
          { 0x912dbcfd4e86214dl,0x2d014ee2cf615498l,0x55e2b1e63530d76el,
            0xc5135ae4fd0fd6d1l },
          0 },
        /* 59 << 112 */
        { { 0x1854daa5061f1658l,0xc0016df1df0cd2b3l,0xc2a3f23e833d50del,
            0x73b681d2bbbd3017l },
          { 0x2f046dc43ac343c0l,0x9c847e7d85716421l,0xe1e13c910917eed4l,
            0x3fc9eebd63a1b9c6l },
          0 },
        /* 60 << 112 */
        { { 0x0f816a727fe02299l,0x6335ccc2294f3319l,0x3820179f4745c5bel,
            0xe647b782922f066el },
          { 0xc22e49de02cafb8al,0x299bc2fffcc2ecccl,0x9a8feea26e0e8282l,
            0xa627278bfe893205l },
          0 },
        /* 61 << 112 */
        { { 0xa7e197337933e47bl,0xf4ff6b132e766402l,0xa4d8be0a98440d9fl,
            0x658f5c2f38938808l },
          { 0x90b75677c95b3b3el,0xfa0442693137b6ffl,0x077b039b43c47c29l,
            0xcca95dd38a6445b2l },
          0 },
        /* 63 << 112 */
        { { 0x583f3703f9374ab6l,0x864f91956e564145l,0x33bc3f4822526d50l,
            0x9f323c801262a496l },
          { 0xaa97a7ae3f046a9al,0x70da183edf8a039al,0x5b68f71c52aa0ba6l,
            0x9be0fe5121459c2dl },
          0 },
        /* 64 << 112 */
        { { 0xc1e17eb6cbc613e5l,0x33131d55497ea61cl,0x2f69d39eaf7eded5l,
            0x73c2f434de6af11bl },
          { 0x4ca52493a4a375fal,0x5f06787cb833c5c2l,0x814e091f3e6e71cfl,
            0x76451f578b746666l },
          0 },
        /* 65 << 112 */
        { { 0xa700767eabd0cc76l,0xa14ae98015889273l,0x5acf2cc466ea6380l,
            0xb942cc40d08d18b9l },
          { 0x9b5daa763ae45782l,0x61a25e0fb72f0ce0l,0xf94c0e80435fefe3l,
            0x73d552cf1620e1c9l },
          0 },
        /* 71 << 112 */
        { { 0x57130582727185c1l,0x8f2b8ebc163897ecl,0x4a059cc7a04e4a6bl,
            0x4b1de9fe0908a366l },
          { 0xa4f7738688d0fef0l,0x55e3bb1d9ebfc138l,0x9022bbef005ae362l,
            0xf5669edc8741d349l },
          0 },
        /* 77 << 112 */
        { { 0xf192c0f7ede937a4l,0xd2e91d62810c1b1el,0xf2b40b64dcc39c69l,
            0xe125fbd028f03b0el },
          { 0x52966dd78da708f9l,0x92d400a3cc0e7f32l,0x4e35aae36b0842b8l,
            0x0b4fe66ded3ad3cfl },
          0 },
        /* 83 << 112 */
        { { 0x14b81d951f1ff6b5l,0x1d82f132ed9b03b8l,0x52f6f029b4fa4047l,
            0xea653682601e5913l },
          { 0x4e900375edeee046l,0xd22ed267f9428714l,0xb004fb3b1753e873l,
            0xfef061ba245b2c09l },
          0 },
        /* 89 << 112 */
        { { 0x5e2376eaf9deba2bl,0x1ed1e9e5269a18cfl,0x8dffd66dcb1cada8l,
            0xb13239f068369c77l },
          { 0x2fede3a67f25426fl,0xc885cf0c6f90a2a6l,0xd950162d4eeac543l,
            0x53011aa09abc201bl },
          0 },
        /* 95 << 112 */
        { { 0x7a63925d432b798al,0x92e762cfc9bd6da9l,0xf22fb9706a190382l,
            0x19919b847b18a9b3l },
          { 0x16793b803adfde86l,0xf9ce15ace8b1d44cl,0x4bf74144c0a140b8l,
            0x680468616f853f6cl },
          0 },
        /* 101 << 112 */
        { { 0xd4e0d8460db84ba2l,0x9a162a3a360b68bbl,0x7297f3939233146cl,
            0xbc93c2f4ec77412dl },
          { 0x13ddf0a7e07e1065l,0x000a8d45fb5e5131l,0xb4373078cf61d467l,
            0xa4a1fd67bf3bb6f9l },
          0 },
        /* 107 << 112 */
        { { 0x6f2473f9d7585098l,0x45a29448d4f23c1al,0x47fe40f1c22bdc25l,
            0x4e46ed1f31347673l },
          { 0x5e43a8624148898cl,0x4a02ededa993954el,0x83d830b52f8a1847l,
            0x007e3156a7f6a378l },
          0 },
        /* 113 << 112 */
        { { 0x01a39fe7e847ca18l,0xaf2722418fed2772l,0x3104ef891fbb1748l,
            0x5b55331b2b9dd5ffl },
          { 0xe7806e31cec6a787l,0x9f49ed881e9c0af2l,0xf5a66373a3905b36l,
            0x77b5bca9efab75f3l },
          0 },
        /* 116 << 112 */
        { { 0xd4d75f4bf0831932l,0x5e770ac477fe8cc9l,0x52b5e748862e72a2l,
            0xe9a45482501d35fel },
          { 0x8a93e7424a9ab187l,0x5a72506de88ca017l,0xe680dcb201eb2defl,
            0xdc5aa4e6ba68209dl },
          0 },
        /* 119 << 112 */
        { { 0x2defa3dc3d01a344l,0x11fd939b162e459al,0x928453b97313d720l,
            0x08696dc053184a65l },
          { 0xd9f8a69c721f7415l,0x304eb0e079539019l,0xc9b0ca6dbb0c6313l,
            0xa10133eba93dc74el },
          0 },
        /* 125 << 112 */
        { { 0xee0b164004393f1el,0x511547dfe1301979l,0xc00dfc3516d26d87l,
            0x06227c8aab847494l },
          { 0x178ca86748b2fdc7l,0xb51296f01a8ba1dcl,0xf252787731e1dd14l,
            0x7ecb5456c0ba2a1fl },
          0 },
    },
    {
        /* 0 << 120 */
        { { 0x00, 0x00, 0x00, 0x00 },
          { 0x00, 0x00, 0x00, 0x00 },
          1 },
        /* 1 << 120 */
        { { 0x3e0e5c9dd111f8ecl,0xbcc33f8db7c4e760l,0x702f9a91bd392a51l,
            0x7da4a795c132e92dl },
          { 0x1a0b0ae30bb1151bl,0x54febac802e32251l,0xea3a5082694e9e78l,
            0xe58ffec1e4fe40b8l },
          0 },
        /* 3 << 120 */
        { { 0x7b23c513516e19e4l,0x56e2e847c5c4d593l,0x9f727d735ce71ef6l,
            0x5b6304a6f79a44c5l },
          { 0x6638a7363ab7e433l,0x1adea470fe742f83l,0xe054b8545b7fc19fl,
            0xf935381aba1d0698l },
          0 },
        /* 4 << 120 */
        { { 0xb5504f9d918e4936l,0x65035ef6b2513982l,0x0553a0c26f4d9cb9l,
            0x6cb10d56bea85509l },
          { 0x48d957b7a242da11l,0x16a4d3dd672b7268l,0x3d7e637c8502a96bl,
            0x27c7032b730d463bl },
          0 },
        /* 5 << 120 */
        { { 0x55366b7d5846426fl,0xe7d09e89247d441dl,0x510b404d736fbf48l,
            0x7fa003d0e784bd7dl },
          { 0x25f7614f17fd9596l,0x49e0e0a135cb98dbl,0x2c65957b2e83a76al,
            0x5d40da8dcddbe0f8l },
          0 },
        /* 7 << 120 */
        { { 0x9fb3bba354530bb2l,0xbde3ef77cb0869eal,0x89bc90460b431163l,
            0x4d03d7d2e4819a35l },
          { 0x33ae4f9e43b6a782l,0x216db3079c88a686l,0x91dd88e000ffedd9l,
            0xb280da9f12bd4840l },
          0 },
        /* 9 << 120 */
        { { 0xa37f3573f37f5937l,0xeb0f6c7dd1e4fca5l,0x2965a554ac8ab0fcl,
            0x17fbf56c274676acl },
          { 0x2e2f6bd9acf7d720l,0x41fc8f8810224766l,0x517a14b385d53befl,
            0xdae327a57d76a7d1l },
          0 },
        /* 10 << 120 */
        { { 0x515d5c891f5f82dcl,0x9a7f67d76361079el,0xa8da81e311a35330l,
            0xe44990c44b18be1bl },
          { 0xc7d5ed95af103e59l,0xece8aba78dac9261l,0xbe82b0999394b8d3l,
            0x6830f09a16adfe83l },
          0 },
        /* 11 << 120 */
        { { 0x43c41ac194d7d9b1l,0x5bafdd82c82e7f17l,0xdf0614c15fda0fcal,
            0x74b043a7a8ae37adl },
          { 0x3ba6afa19e71734cl,0x15d5437e9c450f2el,0x4a5883fe67e242b1l,
            0x5143bdc22c1953c2l },
          0 },
        /* 13 << 120 */
        { { 0xc676d7f2b1f3390bl,0x9f7a1b8ca5b61272l,0x4ebebfc9c2e127a9l,
            0x4602500c5dd997bfl },
          { 0x7f09771c4711230fl,0x058eb37c020f09c1l,0xab693d4bfee5e38bl,
            0x9289eb1f4653cbc0l },
          0 },
        /* 15 << 120 */
        { { 0x54da9dc7ab952578l,0xb5423df226e84d0bl,0xa8b64eeb9b872042l,
            0xac2057825990f6dfl },
          { 0x4ff696eb21f4c77al,0x1a79c3e4aab273afl,0x29bc922e9436b3f1l,
            0xff807ef8d6d9a27al },
          0 },
        /* 16 << 120 */
        { { 0xc7f3a8f833f6746cl,0x21e46f65fea990cal,0x915fd5c5caddb0a9l,
            0xbd41f01678614555l },
          { 0x346f4434426ffb58l,0x8055943614dbc204l,0xf3dd20fe5a969b7fl,
            0x9d59e956e899a39al },
          0 },
        /* 17 << 120 */
        { { 0xe4ca688fd06f56c0l,0xa48af70ddf027972l,0x691f0f045e9a609dl,
            0xa9dd82cdee61270el },
          { 0x8903ca63a0ef18d3l,0x9fb7ee353d6ca3bdl,0xa7b4a09cabf47d03l,
            0x4cdada011c67de8el },
          0 },
        /* 19 << 120 */
        { { 0xac127dc1e038a675l,0x729deff38c5c6320l,0xb7df8fd4a90d2c53l,
            0x9b74b0ec681e7cd3l },
          { 0x5cb5a623dab407e5l,0xcdbd361576b340c6l,0xa184415a7d28392cl,
            0xc184c1d8e96f7830l },
          0 },
        /* 21 << 120 */
        { { 0x86a9303b2f7e85c3l,0x5fce462171988f9bl,0x5b935bf6c138acb5l,
            0x30ea7d6725661212l },
          { 0xef1eb5f4e51ab9a2l,0x0587c98aae067c78l,0xb3ce1b3c77ca9ca6l,
            0x2a553d4d54b5f057l },
          0 },
        /* 23 << 120 */
        { { 0x2c7156e10b1894a0l,0x92034001d81c68c0l,0xed225d00c8b115b5l,
            0x237f9c2283b907f2l },
          { 0x0ea2f32f4470e2c0l,0xb725f7c158be4e95l,0x0f1dcafab1ae5463l,
            0x59ed51871ba2fc04l },
          0 },
        /* 25 << 120 */
        { { 0xd1b0ccdec9520711l,0x55a9e4ed3c8b84bfl,0x9426bd39a1fef314l,
            0x4f5f638e6eb93f2bl },
          { 0xba2a1ed32bf9341bl,0xd63c13214d42d5a9l,0xd2964a89316dc7c5l,
            0xd1759606ca511851l },
          0 },
        /* 27 << 120 */
        { { 0xedf69feaf8c51187l,0x05bb67ec741e4da7l,0x47df0f3208114345l,
            0x56facb07bb9792b1l },
          { 0xf3e007e98f6229e4l,0x62d103f4526fba0fl,0x4f33bef7b0339d79l,
            0x9841357bb59bfec1l },
          0 },
        /* 28 << 120 */
        { { 0xae1e0b67e28ef5bal,0x2c9a4699cb18e169l,0x0ecd0e331e6bbd20l,
            0x571b360eaf5e81d2l },
          { 0xcd9fea58101c1d45l,0x6651788e18880452l,0xa99726351f8dd446l,
            0x44bed022e37281d0l },
          0 },
        /* 29 << 120 */
        { { 0x830e6eea60dbac1fl,0x23d8c484da06a2f7l,0x896714b050ca535bl,
            0xdc8d3644ebd97a9bl },
          { 0x106ef9fab12177b4l,0xf79bf464534d5d9cl,0x2537a349a6ab360bl,
            0xc7c54253a00c744fl },
          0 },
        /* 31 << 120 */
        { { 0x24d661d168754ab0l,0x801fce1d6f429a76l,0xc068a85fa58ce769l,
            0xedc35c545d5eca2bl },
          { 0xea31276fa3f660d1l,0xa0184ebeb8fc7167l,0x0f20f21a1d8db0ael,
            0xd96d095f56c35e12l },
          0 },
        /* 33 << 120 */
        { { 0x57d2046b59da06ebl,0x3c076d5fa49f6d74l,0x6b4c96e616f82ea0l,
            0xaf7b0f1f90536c0bl },
          { 0x7999f86d204a9b2dl,0x7e420264126c9f87l,0x4c967a1f262ac4e5l,
            0xe8174a09900e79adl },
          0 },
        /* 34 << 120 */
        { { 0xd51687f2cb82516bl,0x8a440cfc040e4670l,0xeafd2bcfe7738d32l,
            0x7071e9162a1e911al },
          { 0xbd3abd44cfea57bbl,0x9c3add16085b19e2l,0xb194c01d6baa5aa6l,
            0x6f3d3faf92f85c64l },
          0 },
        /* 35 << 120 */
        { { 0xe23e0769488a280el,0x8e55a728e63a5904l,0x01690716ab84cccfl,
            0xfe796130b78b3c98l },
          { 0x15cc475b9117f211l,0xbdc178761d1b9d56l,0x8df5594a3e37b9b9l,
            0x97747e341e37e494l },
          0 },
        /* 36 << 120 */
        { { 0xf2a6370ed2f896e1l,0x27100e63802987afl,0xb4db1cff4678ebc7l,
            0x6e5f28d937b4b263l },
          { 0xd29030009711ebc4l,0xf14dcb9ff8712484l,0x7a46ec3eea449146l,
            0x200155e9c1c51179l },
          0 },
        /* 37 << 120 */
        { { 0x8130f007f1968d55l,0x18823e7097ed9803l,0xdc9fec559402762dl,
            0x9e0bd57e278f5abbl },
          { 0xaa41b913c9ebf303l,0x1105ec43a76b9353l,0xf8e4ee4cf4e6c6b5l,
            0x3a630972bd7be696l },
          0 },
        /* 39 << 120 */
        { { 0x5c7da7e16356b3eel,0x951bfe458ccf9b48l,0x6f2c6e91d0555d0cl,
            0x47d7f7b58efd38eel },
          { 0x957256c8af6fd630l,0xa690c65bdc01774cl,0xad52b27c7c8dafdal,
            0x81fbc16af44a145fl },
          0 },
        /* 40 << 120 */
        { { 0x497c3a3481b0493al,0x2b3ab20d71bc8408l,0x0c60226aa03769d1l,
            0x4ac89c7ad10708b0l },
          { 0x62398ea5092f7e6al,0x7f408f54de96d526l,0x025bde6f85bf102cl,
            0xcc2f85120a4aa72el },
          0 },
        /* 41 << 120 */
        { { 0x8a65e0386884a9c3l,0xd2e6ac047bf8c794l,0xc9c5d3d3f7bcdfb9l,
            0x0000ce42a33f2c12l },
          { 0xea1c0a9a7dd13b2bl,0xbfd97d7f0c35c3b1l,0x0ba75cf3347fcefel,
            0xc3c5f28f1333460dl },
          0 },
        /* 43 << 120 */
        { { 0x7810ebf575baa708l,0xe7fa7a0dd7440549l,0x25b813baf0667e4al,
            0x30a46740d15838a8l },
          { 0x13207b1ad04b22f7l,0x09e601ffd1419699l,0xb1038fc77f687b27l,
            0xa4547dc9a127f95bl },
          0 },
        /* 44 << 120 */
        { { 0x83b2e3b3056ecd2cl,0xd17dcdaaf03dfd36l,0xee24a5f81dcef956l,
            0xb6746cd0b7239f16l },
          { 0xed6cb311c8458c48l,0xe8c0fc9805d27da4l,0x4610e9a0a1bf0970l,
            0x1947f01d9906c19el },
          0 },
        /* 45 << 120 */
        { { 0x8b979126217c7cd7l,0x65c57a378050067el,0x6a50c6383f34838cl,
            0x3de617c29b7bc81fl },
          { 0x58488d24253a0ac7l,0x3fe53ec75520ba0bl,0x9156dca763f0607el,
            0xdd08c5705d1fe134l },
          0 },
        /* 46 << 120 */
        { { 0xbfb1d9e1e33ba77fl,0x0985311ccaef6c01l,0xc8b59e9accca8948l,
            0x1256280945416f25l },
          { 0xc90edbc257f53218l,0xcaa08c05125d8fb5l,0x33ea3fd49a1aad3bl,
            0x2aa8bd83d005e8bel },
          0 },
        /* 47 << 120 */
        { { 0xcbd2f1a3c2b22963l,0x0f7bd29c0c8ac2b3l,0xddb932432d405bfdl,
            0xeabd4805328413b5l },
          { 0xcc79d31748ebb6b9l,0x09604f831f521aael,0xd3487fdf4c7d188cl,
            0xd219c318d1552ea9l },
          0 },
        /* 48 << 120 */
        { { 0xef4f115c775d6ecel,0x69d2e3bbe8c0e78dl,0xb0264ef1145cfc81l,
            0x0a41e9fa1b69788bl },
          { 0x0d9233be909a1f0bl,0x150a84520ae76b30l,0xea3375370632bb69l,
            0x15f7b3cfaa25584al },
          0 },
        /* 49 << 120 */
        { { 0xfc4c623e321f7b11l,0xd36c1066f9cbc693l,0x8165235835dc0c0al,
            0xa3ce2e18c824e97el },
          { 0x59ea7cbcc6ff405el,0xced5a94a1e56a1e2l,0x88d744c53ab64b39l,
            0x8963d029073a36e7l },
          0 },
        /* 51 << 120 */
        { { 0x97aa902cb19f3edbl,0x8e605ff9bbf2975bl,0x0536fa8ba6eb299bl,
            0xfd96da4f7cd03ac0l },
          { 0x29c5b5b578f9a265l,0x1f025a6d5fd0bc1bl,0x440486ee58e0f8e1l,
            0x8f191f7d593e49e9l },
          0 },
        /* 52 << 120 */
        { { 0xbddf656baea9c13fl,0x083c5d514c678b37l,0x975431b630878ed4l,
            0x6de13d4608d9cf1cl },
          { 0xfbb639cc02427c45l,0x6190ca0c5a6cd989l,0x35a6aa26c53f11b7l,
            0x73f9e17dddfd86f6l },
          0 },
        /* 53 << 120 */
        { { 0xd30478a317be7689l,0x6fc3f634e358f7a7l,0x4057ece515688d9fl,
            0xb5397495d3d91eefl },
          { 0x62fac49e2f49bde4l,0xeb4a3e1860125c73l,0x15f38be8dabdac55l,
            0x18bf29f7d334d52al },
          0 },
        /* 55 << 120 */
        { { 0xf684162b68777538l,0x3e2a770bbb3381f4l,0x1b7562c1b374577cl,
            0x9eec22dc5cf21688l },
          { 0xc35014b1d472be2cl,0xafe2317035f086fbl,0xb9c9c4c9a1491ce1l,
            0x2df1e669b56792ddl },
          0 },
        /* 57 << 120 */
        { { 0xcf7d36fe1830f624l,0x176c3c12ed0474bdl,0x25b802c8f82b493dl,
            0x683c2a744c78147el },
          { 0x0db99444f8f3e446l,0x437bcac6800a56c7l,0xb4e592264d08b25fl,
            0xcaf1b4142e691ca7l },
          0 },
        /* 59 << 120 */
        { { 0x378bd47b9d231cafl,0xde3aa2f01f4db832l,0xf609d16ab29bd7d5l,
            0x13feab54bdfb54dfl },
          { 0x274abbbc22fc1a12l,0x267febb47d30ef1bl,0xeffa996d80717cd8l,
            0x065a86d1118d0812l },
          0 },
        /* 60 << 120 */
        { { 0xc681a8656a3cb3afl,0x528f25a981751414l,0x6669f07cc7eac946l,
            0x9fb3a53f3cc6cc6bl },
          { 0x2919d92a11ae224al,0xa59141110b170a19l,0xdc16c611e2042f16l,
            0x58ace12decd4180bl },
          0 },
        /* 61 << 120 */
        { { 0x689bb1ec107bb59fl,0x8129702adad2b385l,0x10bd3baeb1630603l,
            0xaadec5d15f23e7cfl },
          { 0x572f234f4586f7fbl,0x13abdec95ec11b32l,0xa462a7ec6191c26al,
            0x4a7d92a06685c8d3l },
          0 },
        /* 63 << 120 */
        { { 0xdd4e2b63b16628eal,0xdf0c8fc8eefa5e86l,0xb0ec710205662720l,
            0x3f4c6956fe81e9dal },
          { 0x5732ad8f52e356f7l,0x045a103968a658f0l,0x9c40b0b6506ba33al,
            0x0a426010cb54258dl },
          0 },
        /* 64 << 120 */
        { { 0x09891641d4c5105fl,0x1ae80f8e6d7fbd65l,0x9d67225fbee6bdb0l,
            0x3b433b597fc4d860l },
          { 0x44e66db693e85638l,0xf7b59252e3e9862fl,0xdb785157665c32ecl,
            0x702fefd7ae362f50l },
          0 },
        /* 65 << 120 */
        { { 0x3902ab14c3254641l,0xa63cfd9fd8c001c8l,0x597d155c52d0af3cl,
            0xc5a2cbc4a0dbe688l },
          { 0xac8a841b249195aal,0xc98f01aaed14426fl,0xeb4a8ce8353905f1l,
            0x4d6668171ecee1b7l },
          0 },
        /* 71 << 120 */
        { { 0xbd66e7d9a94da8cdl,0x7bc04735801ef314l,0x90f3eba1c5cc2904l,
            0x3c7dfed6f71bb36dl },
          { 0x89a50c8da75e3086l,0x88b8b4746f8e3418l,0x26fe17f4a44a5dbdl,
            0x98bf74c16a1e24fel },
          0 },
        /* 77 << 120 */
        { { 0xca7b470679e0db85l,0x7f46c7716fc897fdl,0x9537e7918edfc0f3l,
            0xa46d4b4405e91ddfl },
          { 0x97d21061ee5575e7l,0x1f4f32da59650429l,0x2d1d6af878995129l,
            0x41d6fc228a0e4260l },
          0 },
        /* 83 << 120 */
        { { 0xb30a1a89107d2282l,0x5433d7673a5e1323l,0xb9eeab822abdfeafl,
            0x9579cb46df3e0dbfl },
          { 0x6fc3ff2c7e088e79l,0x94b32360d7314326l,0xd2e82b59e5ad82e4l,
            0x7372dc4a55bc24e3l },
          0 },
        /* 89 << 120 */
        { { 0x355697215f3c03cbl,0x4150adf2a146edcdl,0x16ec1a421a252e1cl,
            0xdf4d0f94424984eal },
          { 0x15142b5f5fabe961l,0xe6a73c29567ec13al,0xe6d370795d12070al,
            0x437743d0206fd7c6l },
          0 },
        /* 95 << 120 */
        { { 0x483b7a95d66bc594l,0xf6a7064e8a6113bbl,0x373ce20f4ed34f72l,
            0x6aa876ab24f429b2l },
          { 0x378d5c25412c3102l,0xe4219a97b493199cl,0x01c7cafaa0b37332l,
            0x9305cc85f7633f7dl },
          0 },
        /* 101 << 120 */
        { { 0x0259b43aaadf2273l,0x869c5bd3cf9dc1c2l,0x4f18a6e4068d6628l,
            0xd110637fec2d4547l },
          { 0x1ae88a791e94aaddl,0xe8b4be39de64f5f9l,0x85cbd9b24dc6b2bbl,
            0xb65091fa1bc352b2l },
          0 },
        /* 107 << 120 */
        { { 0x7c5cea5d20f6a354l,0xe936ff1582f3ed39l,0x54e7a775b779368el,
            0x8ca8a46e3cb17c9el },
          { 0x753ca1fa0138974dl,0x9ce311eba72902ffl,0xcb727e56973f72b6l,
            0xde72538d91685710l },
          0 },
        /* 113 << 120 */
        { { 0xf423569f1bec8f85l,0x23376da5ca844ac4l,0xce7b407a111523f4l,
            0x736fb92dde7aa46dl },
          { 0xd9139edcc7662640l,0x520fbf0656a85e24l,0x14e3b5857e5284b5l,
            0xcbae4e8321d56ef3l },
          0 },
        /* 116 << 120 */
        { { 0x69830a05564470a1l,0x1a1e26cf5b702e8el,0xe5fdf7d9d8fae645l,
            0xe4774f74a9950c66l },
          { 0x18bdda7cd1466825l,0xe6ab4ce6d115218al,0xfcb8c50064528629l,
            0xd705f429e70deed9l },
          0 },
        /* 119 << 120 */
        { { 0x3f992d7ba99df096l,0x08993b4125e78725l,0x79eaad13117c4cafl,
            0x7230594c9fa87285l },
          { 0xac23d7edf2673e27l,0xc9d76fb53b9eb111l,0x7a0a036a9e9db78al,
            0x7c6ec39df9565cffl },
          0 },
        /* 125 << 120 */
        { { 0x956ad1441fd4f7a1l,0x6c511ffecb7546cal,0x11becdaef5ae6ddbl,
            0x67587741946168b2l },
          { 0x99cd45edf54379a7l,0x687f8462e2748decl,0x2b2be1e1837bd066l,
            0x3862659c0c45a5a9l },
          0 },
    },
    {
        /* 0 << 128 */
        { { 0x00, 0x00, 0x00, 0x00 },
          { 0x00, 0x00, 0x00, 0x00 },
          1 },
        /* 1 << 128 */
        { { 0x62a8c244bfe20925l,0x91c19ac38fdce867l,0x5a96a5d5dd387063l,
            0x61d587d421d324f6l },
          { 0xe87673a2a37173eal,0x2384800853778b65l,0x10f8441e05bab43el,
            0xfa11fe124621efbel },
          0 },
        /* 3 << 128 */
        { { 0xc0f734a3b2335834l,0x9526205a90ef6860l,0xcb8be71704e2bb0dl,
            0x2418871e02f383fal },
          { 0xd71776814082c157l,0xcc914ad029c20073l,0xf186c1ebe587e728l,
            0x6fdb3c2261bcd5fdl },
          0 },
        /* 4 << 128 */
        { { 0xb4480f0441c23fa3l,0xb4712eb0c1989a2el,0x3ccbba0f93a29ca7l,
            0x6e205c14d619428cl },
          { 0x90db7957b3641686l,0x0432691d45ac8b4el,0x07a759acf64e0350l,
            0x0514d89c9c972517l },
          0 },
        /* 5 << 128 */
        { { 0xcc7c4c1c2cf9d7c1l,0x1320886aee95e5abl,0xbb7b9056beae170cl,
            0xc8a5b250dbc0d662l },
          { 0x4ed81432c11d2303l,0x7da669121f03769fl,0x3ac7a5fd84539828l,
            0x14dada943bccdd02l },
          0 },
        /* 7 << 128 */
        { { 0x51b90651cbae2f70l,0xefc4bc0593aaa8ebl,0x8ecd8689dd1df499l,
            0x1aee99a822f367a5l },
          { 0x95d485b9ae8274c5l,0x6c14d4457d30b39cl,0xbafea90bbcc1ef81l,
            0x7c5f317aa459a2edl },
          0 },
        /* 9 << 128 */
        { { 0x410dc6a90deeaf52l,0xb003fb024c641c15l,0x1384978c5bc504c4l,
            0x37640487864a6a77l },
          { 0x05991bc6222a77dal,0x62260a575e47eb11l,0xc7af6613f21b432cl,
            0x22f3acc9ab4953e9l },
          0 },
        /* 10 << 128 */
        { { 0x27c8919240be34e8l,0xc7162b3791907f35l,0x90188ec1a956702bl,
            0xca132f7ddf93769cl },
          { 0x3ece44f90e2025b4l,0x67aaec690c62f14cl,0xad74141822e3cc11l,
            0xcf9b75c37ff9a50el },
          0 },
        /* 11 << 128 */
        { { 0x0d0942770c24efc8l,0x0349fd04bef737a4l,0x6d1c9dd2514cdd28l,
            0x29c135ff30da9521l },
          { 0xea6e4508f78b0b6fl,0x176f5dd2678c143cl,0x081484184be21e65l,
            0x27f7525ce7df38c4l },
          0 },
        /* 13 << 128 */
        { { 0x9faaccf5e4652f1dl,0xbd6fdd2ad56157b2l,0xa4f4fb1f6261ec50l,
            0x244e55ad476bcd52l },
          { 0x881c9305047d320bl,0x1ca983d56181263fl,0x354e9a44278fb8eel,
            0xad2dbc0f396e4964l },
          0 },
        /* 15 << 128 */
        { { 0xfce0176788a2ffe4l,0xdc506a3528e169a5l,0x0ea108617af9c93al,
            0x1ed2436103fa0e08l },
          { 0x96eaaa92a3d694e7l,0xc0f43b4def50bc74l,0xce6aa58c64114db4l,
            0x8218e8ea7c000fd4l },
          0 },
        /* 16 << 128 */
        { { 0x6a7091c2e48fb889l,0x26882c137b8a9d06l,0xa24986631b82a0e2l,
            0x844ed7363518152dl },
          { 0x282f476fd86e27c7l,0xa04edaca04afefdcl,0x8b256ebc6119e34dl,
            0x56a413e90787d78bl },
          0 },
        /* 17 << 128 */
        { { 0xd1ffd160deb58b9bl,0x78492428c007273cl,0x47c908048ef06073l,
            0x746cd0dfe48c659el },
          { 0xbd7e8e109d47055bl,0xe070967e39711c04l,0x3d8869c99c9444f6l,
            0x6c67ccc834ac85fcl },
          0 },
        /* 19 << 128 */
        { { 0x8a42d8b087b05be1l,0xef00df8d3e4e1456l,0x148cc8e8fbfc8cd2l,
            0x0288ae4c4878804fl },
          { 0x44e669a73b4f6872l,0xa4a8dbd4aab53c5bl,0x843fa963c9660052l,
            0x128e2d2571c05dd2l },
          0 },
        /* 21 << 128 */
        { { 0x3ea86174a9f1b59bl,0xc747ea076a9a8845l,0x733710b5ab242123l,
            0x6381b546d386a60cl },
          { 0xba0e286366a44904l,0x770f618de9db556cl,0x39e567f828fb198dl,
            0xb5f1bef040147ee8l },
          0 },
        /* 23 << 128 */
        { { 0x1adee1d516391617l,0x962d9184a3315fd9l,0x91c229750c805d59l,
            0x4575eaf2cd9a1877l },
          { 0x83fef163451831b9l,0x829d6bdd6f09e30fl,0x9379272dcc6b4e6al,
            0xd7a049bd95fbee4al },
          0 },
        /* 25 << 128 */
        { { 0x695f70da44ae09c6l,0x79793892bb99de1dl,0xde269352f696b429l,
            0xe37ea97f8104c825l },
          { 0x3166cac6b0e72e63l,0xa82e633ca03ba670l,0x1106e3843e505667l,
            0xc2994f3dffb788b6l },
          0 },
        /* 27 << 128 */
        { { 0xd36a5ab37c53073bl,0xc44a9940ebdc7e35l,0x7dd86c8bf3ded136l,
            0x9fe9879fd5a0eb14l },
          { 0xa210726c9b99bf9cl,0x3faf4456861036afl,0x1661f1c9615d091al,
            0x2c63f630911551bcl },
          0 },
        /* 28 << 128 */
        { { 0x1554d46da670ff1dl,0x24833d88cb97a1ccl,0x8fa6ab3cded97493l,
            0x215e037189926498l },
          { 0x549bd592e56d74ffl,0x58a8caf543b5e1ecl,0x3c6087a323e93cb9l,
            0x8b0549875648b83cl },
          0 },
        /* 29 << 128 */
        { { 0x232974230554f94fl,0x4f445a380f3a7618l,0xb9fb40bee4abefd6l,
            0xfbf3eaf9c15eb07cl },
          { 0xed469c23aca0c8b3l,0xc5209f68846e3f8fl,0x33d51d13d75da468l,
            0x9406e10a3d5c6e29l },
          0 },
        /* 31 << 128 */
        { { 0xb9a44b1f5c6cad21l,0xaa9947751ee60a83l,0xc89af3858c390401l,
            0xef1e450b8dd51056l },
          { 0x5f5f069879ac84d1l,0x68d82982ef57b1afl,0x31f1d90f50849555l,
            0xff9577e57d9fc8f6l },
          0 },
        /* 33 << 128 */
        { { 0xaeebc5c0b430d6a1l,0x39b87a13dc3a9c04l,0xf0c445252db4a631l,
            0xe32d95482c66fcf6l },
          { 0x16f11bafb17849c4l,0xdd1c76615eca71f7l,0x4389ad2e32e6c944l,
            0x727c11a5889a06bbl },
          0 },
        /* 34 << 128 */
        { { 0x38dd1ac021e5781al,0x578318dbfd019ee2l,0x096b677d5f88e574l,
            0xdbec82b216ad9f4fl },
          { 0x348debe23260e8d9l,0x9334126064dfcda1l,0xdc5fb34cefc8faael,
            0x5fa048beb4a6fc25l },
          0 },
        /* 35 << 128 */
        { { 0xe18806fd60b3258cl,0xb7d2926b1364df47l,0xe208300fa107ce99l,
            0x8d2f29fe7918df0el },
          { 0x0b012d77a1244f4cl,0xf01076f4213a11cfl,0x8e623223181c559dl,
            0x9df196ee995a281dl },
          0 },
        /* 36 << 128 */
        { { 0xc431a238013ff83bl,0x7c0018b2fad69d08l,0x99aeb52a4c9589eal,
            0x121f41ab9b1cf19fl },
          { 0x0cfbbcbaef0f5958l,0x8deb3aeb7be8fbdcl,0x12b954081f15aa31l,
            0x5acc09b34c0c06fdl },
          0 },
        /* 37 << 128 */
        { { 0xfaa821383a721940l,0xdd70f54dd0008b83l,0x00decb507d32a52dl,
            0x04563529cdd87deal },
          { 0xb0e7e2a2db81643dl,0x445f4c383a6fef50l,0x5c0ef211df694ae1l,
            0xa5a8fead923d0f1cl },
          0 },
        /* 39 << 128 */
        { { 0xbc0e08b0325b2601l,0xae9e4c6105815b7al,0x07f664faf944a4a1l,
            0x0ad19d29288f83b3l },
          { 0x8615cd677232c458l,0x98edff6e9038e7d1l,0x082e0c4395a4dfccl,
            0x336267afeceee00el },
          0 },
        /* 40 << 128 */
        { { 0x775cbfa86d518ffbl,0xdecee1f6930f124bl,0x9a402804f5e81d0fl,
            0x0e8225c52a0eeb2fl },
          { 0x884a5d39fee9e867l,0x9540428ffb505454l,0xb2bf2e20107a70d1l,
            0xd9917c3ba010b2aal },
          0 },
        /* 41 << 128 */
        { { 0xc88ad4452a29bfdel,0x3072ebfa998368b7l,0xa754cbf7f5384692l,
            0x85f7e16906b13146l },
          { 0x42a7095f6a549fbel,0xef44edf91f7f1f42l,0xbea2989737b0c863l,
            0x13b096d87a1e7fc3l },
          0 },
        /* 43 << 128 */
        { { 0x51add77ce2a3a251l,0x840ca1384d8476adl,0x08d01d26f6096478l,
            0x10d501a532f1662bl },
          { 0xc8d63f811165a955l,0x587aa2e34095046al,0x759506c617af9000l,
            0xd6201fe4a32ab8d2l },
          0 },
        /* 44 << 128 */
        { { 0xa98f42fa3d843d53l,0x33777cc613ef927al,0xc440cdbecb84ca74l,
            0x8c22f9631dc7c5ddl },
          { 0x4bc82b70c8d94708l,0x7e0b43fcc814364fl,0x286d4e2486f59b7el,
            0x1abc895e4d6bf4c4l },
          0 },
        /* 45 << 128 */
        { { 0x7c52500cfc8c9bbdl,0x635563381534d9f7l,0xf55f38cbfd52c990l,
            0xc585ae85058f52e7l },
          { 0xb710a28bf9f19a01l,0x891861bdf0273ca4l,0x38a7aa2b034b0b7cl,
            0xa2ecead52a809fb1l },
          0 },
        /* 46 << 128 */
        { { 0x3df614f1ec3ca8eal,0x6bb24e9f9505bc08l,0x23ba1afbf37ace22l,
            0x2e51b03b3463c261l },
          { 0x59a0fca9c39e6558l,0x819f271ca342ccd9l,0x0c913d54df7ac033l,
            0xba0f83de573257d3l },
          0 },
        /* 47 << 128 */
        { { 0xdf62817ab3b32fbcl,0x616d74b0964670d4l,0xa37bc6270e26020bl,
            0xda46d655b7d40bdal },
          { 0x2840f155b5773f84l,0xbb633777897774b6l,0x59ff1df79a1ed3fal,
            0xf7011ee2bac571f9l },
          0 },
        /* 48 << 128 */
        { { 0x38151e274d559d96l,0x4f18c0d3b8db6c01l,0x49a3aa836f9921afl,
            0xdbeab27b8c046029l },
          { 0x242b9eaa7040bf3bl,0x39c479e51614b091l,0x338ede2b0e4baf5dl,
            0x5bb192b7f0a53945l },
          0 },
        /* 49 << 128 */
        { { 0xd612951861535bb0l,0xbf14364016f6a954l,0x3e0931eedde18024l,
            0x79d791c8139441c0l },
          { 0xba4fe7ecb67b8269l,0x7f30d848224b96c1l,0xa7e0a6abf0341068l,
            0x78db42c37198ea2dl },
          0 },
        /* 51 << 128 */
        { { 0x13354044185ce776l,0x109a6e059ff0100cl,0xafa3b61b03144cb1l,
            0x4e4c814585265586l },
          { 0xa8dafd33edb35364l,0x6691781bfd2606bel,0x2e06a9786182f5ccl,
            0x588784ebe77faeecl },
          0 },
        /* 52 << 128 */
        { { 0x896d572337e440d7l,0x685c5fd9ade23f68l,0xb5b1a26dc2c64918l,
            0xb9390e30dad6580cl },
          { 0x87911c4e7dee5b9bl,0xb90c5053deb04f6el,0x37b942a18f065aa6l,
            0x34acdf2a1ca0928dl },
          0 },
        /* 53 << 128 */
        { { 0xc773f525606f8f04l,0x75ae4a4b41b0a5bbl,0xb2aa058eaf7df93cl,
            0xf15bea4feafed676l },
          { 0xd2967b236a3c4fd7l,0xa698628090e30e7fl,0xf1b5166d316418bdl,
            0x5748682e1c13cb29l },
          0 },
        /* 55 << 128 */
        { { 0xe7b11babfff3605bl,0xdbce1b74cbac080fl,0xa0be39bd6535f082l,
            0x2b6501805f826684l },
          { 0xf90cea2400f5244fl,0xe279f2fadd244a1cl,0xd3fca77c9421c3ael,
            0xe66bc7ee81a5210al },
          0 },
        /* 57 << 128 */
        { { 0x114085dac40c6461l,0xaf78cb47f47d41b8l,0x7a9ae851755b0adbl,
            0x8d2e8c66a0600b6dl },
          { 0x5fb19045389758c0l,0xfa6e2cdabe7c91b2l,0x6472a432663983a2l,
            0xc9370829e0e19363l },
          0 },
        /* 59 << 128 */
        { { 0xd335856ec50bf2ffl,0x89b42295dfa708c2l,0x5dfb42241b201b4el,
            0x6c94d6b94eecbf9cl },
          { 0xabe5a47a7a634097l,0xf3d53b1643febecfl,0xff18619faca9846el,
            0x80ad8629a4066177l },
          0 },
        /* 60 << 128 */
        { { 0x7872e34b3390ff23l,0x968ce4abde7d18efl,0x9b4a745e627fe7b1l,
            0x9607b0a0caff3e2al },
          { 0x1b05818eeb40e3a5l,0x6ac62204c0fa8d7al,0xb5b9058571ed4809l,
            0xb2432ef0f7cb65f2l },
          0 },
        /* 61 << 128 */
        { { 0xc1203418f8a144b7l,0xb3413f808378f901l,0xf6badea161857095l,
            0xcd2816c2b2e93efel },
          { 0x6a8303ea174a0ee6l,0x98b62f29150b28b6l,0x68071bbc9c2a05b6l,
            0xcfcf41a39f00e36el },
          0 },
        /* 63 << 128 */
        { { 0xcaf564f234d6bc29l,0x9e9a6507f3c8edb0l,0x2fb889edd4e5502el,
            0xb70d4ceb6cc9d8edl },
          { 0x0de25356b020f740l,0xa68d9263d11fe5e6l,0xe86400679d85dd77l,
            0xa95dfa7dec2c8c8dl },
          0 },
        /* 64 << 128 */
        { { 0x715c9f973112795fl,0xe8244437984e6ee1l,0x55cb4858ecb66bcdl,
            0x7c136735abaffbeel },
          { 0x546615955dbec38el,0x51c0782c388ad153l,0x9ba4c53ac6e0952fl,
            0x27e6782a1b21dfa8l },
          0 },
        /* 65 << 128 */
        { { 0x3f9bc63ece59397dl,0x3f0f98a93eaa6104l,0x2f82c37c002d9271l,
            0x6ac0495d4985353cl },
          { 0xbde52f629191527bl,0xa3a13fce475aa640l,0x1d71ae17ce673f89l,
            0x2b5cc61529120ec1l },
          0 },
        /* 71 << 128 */
        { { 0xa0ab0f9924318c1cl,0x0cc5ca7da80ca60bl,0x24e27598abb965bal,
            0xc4863198b44d1351l },
          { 0x4d913783a28f04bel,0x404e78088cce8960l,0x2973b4e46286873el,
            0x7b6e0f3219f42b50l },
          0 },
        /* 77 << 128 */
        { { 0x0091a786306a6349l,0x4640ceab2098622dl,0x9928022be8182233l,
            0xf261bee4514d0bedl },
          { 0x70cdcc44c5f64fedl,0x4e19fec4f9eb2dfel,0xd05bdc09058b0b69l,
            0x16f3007ed3bc6190l },
          0 },
        /* 83 << 128 */
        { { 0x8f7f16957f136df1l,0x6d7547019b4f4215l,0xfb22d55eb4cc46a6l,
            0x0b53ef53a8563034l },
          { 0x8b105acc42bc9353l,0xe44c0a396079d59dl,0x78441fee35ee38ddl,
            0x87ad93e43dcc0119l },
          0 },
        /* 89 << 128 */
        { { 0x98a1c55358d9f73al,0xaa0843f0540e2b91l,0x701f8831d0647459l,
            0xc4ae9d0484673005l },
          { 0x9c37bc9f30b3ea20l,0x24cb4e2dbcbfb2b2l,0x8513e6f313cbf070l,
            0x0c4db4334e76c79el },
          0 },
        /* 95 << 128 */
        { { 0x882a2b9cbc8320b8l,0x16e9c11e3ad9e222l,0x24399ac19b23cb1dl,
            0x334c5496799a89c7l },
          { 0x72b6f9b8df3d774cl,0x42955bcbb11b6704l,0x3c4d6021ad2d4eafl,
            0x5416b309afe2b671l },
          0 },
        /* 101 << 128 */
        { { 0x1bbe9e662bf7c2a6l,0x22a3a10ca4acfddbl,0x2424eaab46bae581l,
            0xebec1bbf40d6bdadl },
          { 0xd7e3fa1a5b012aedl,0xc0f82c23f1dc6204l,0x42787c82e319477dl,
            0xca1ae7a14cf57573l },
          0 },
        /* 107 << 128 */
        { { 0x44b7d589d51bbde9l,0x15de755fd6a4cc98l,0x9b6ea8e582fb8e2el,
            0x9d9294f04332bc22l },
          { 0x53c6b2b7d1fa239al,0x286bf536693ca4f1l,0xc3fa754603c00f65l,
            0xc046713af49cdb48l },
          0 },
        /* 113 << 128 */
        { { 0xe356f5f11d82d5d6l,0xa0346a73d035ca0cl,0x14c76adee1884448l,
            0xd8369bdd1c23dde9l },
          { 0x13017862fe025eafl,0x6b5ac5e9a76be1d7l,0x52d621a94933bb6el,
            0xb045b53baa8c1d3fl },
          0 },
        /* 116 << 128 */
        { { 0x242da39e4e40466al,0xc03cb184ac322b07l,0x776b744f9aaa10bfl,
            0xb80d9f14fe7d4beal },
          { 0x75cd14308f9c4908l,0xa4e59ce9087b3d7al,0x3bbdce598cdca614l,
            0x58c57113bc1a5df1l },
          0 },
        /* 119 << 128 */
        { { 0x2a70af1abd79d467l,0x68dc4f23f63e2b73l,0x4345572f1f67b23dl,
            0xc012b08f3a340718l },
          { 0x9458585cc963dbe2l,0x21d84032223a495cl,0x0d54a4ea0dc28159l,
            0xd9549e2c9b927dafl },
          0 },
        /* 125 << 128 */
        { { 0xcd54ebd2d43c8cd2l,0x5ff4ded6a817b9f9l,0x6f59bc31245386d3l,
            0x65b67cb0a2077821l },
          { 0x36407956405ffa07l,0x723e0252d589f27al,0x052004b888e1239el,
            0x8e6d188d69fdf94dl },
          0 },
    },
    {
        /* 0 << 136 */
        { { 0x00, 0x00, 0x00, 0x00 },
          { 0x00, 0x00, 0x00, 0x00 },
          1 },
        /* 1 << 136 */
        { { 0xc16c236e846e364fl,0x7f33527cdea50ca0l,0xc48107750926b86dl,
            0x6c2a36090598e70cl },
          { 0xa6755e52f024e924l,0xe0fa07a49db4afcal,0x15c3ce7d66831790l,
            0x5b4ef350a6cbb0d6l },
          0 },
        /* 3 << 136 */
        { { 0xe2a37598a9d82abfl,0x5f188ccbe6c170f5l,0x816822005066b087l,
            0xda22c212c7155adal },
          { 0x151e5d3afbddb479l,0x4b606b846d715b99l,0x4a73b54bf997cb2el,
            0x9a1bfe433ecd8b66l },
          0 },
        /* 4 << 136 */
        { { 0xe13122f3dbfb894el,0xbe9b79f6ce274b18l,0x85a49de5ca58aadfl,
            0x2495775811487351l },
          { 0x111def61bb939099l,0x1d6a974a26d13694l,0x4474b4ced3fc253bl,
            0x3a1485e64c5db15el },
          0 },
        /* 5 << 136 */
        { { 0x5afddab61430c9abl,0x0bdd41d32238e997l,0xf0947430418042ael,
            0x71f9addacdddc4cbl },
          { 0x7090c016c52dd907l,0xd9bdf44d29e2047fl,0xe6f1fe801b1011a6l,
            0xb63accbcd9acdc78l },
          0 },
        /* 7 << 136 */
        { { 0x0ad7337ac0b7eff3l,0x8552225ec5e48b3cl,0xe6f78b0c73f13a5fl,
            0x5e70062e82349cbel },
          { 0x6b8d5048e7073969l,0x392d2a29c33cb3d2l,0xee4f727c4ecaa20fl,
            0xa068c99e2ccde707l },
          0 },
        /* 9 << 136 */
        { { 0x5b826fcb1b3ec67bl,0xece1b4b041356616l,0x7d5ce77e56a3ab4fl,
            0xf6087f13aa212da0l },
          { 0xe63015054db92129l,0xb8ae4c9940407d11l,0x2b6de222dfab8385l,
            0x9b323022b7d6c3b4l },
          0 },
        /* 10 << 136 */
        { { 0x057ef17a5ae6ad84l,0x9feae00b293a6ae0l,0xd18bb6c154266408l,
            0xd3d3e1209c8e8e48l },
          { 0xba8d4ca80e94fc8fl,0x80262ffc8a8ea0fel,0xac5b2855f71655fdl,
            0xa348f8fae9aced89l },
          0 },
        /* 11 << 136 */
        { { 0x60684b69a5660af3l,0x69aad23b9066d14bl,0x4d9f9b49fa4d020al,
            0xafb54ec1b5cd6a4al },
          { 0x2b25fe1832fd864dl,0xee6945062b6b64d0l,0x954a2a515001d8aal,
            0x5e1008557082b5b3l },
          0 },
        /* 13 << 136 */
        { { 0x20ecf71cbc90eb1bl,0x4234facf651c1df4l,0xc720fce9e681f678l,
            0x680becdda7c007f4l },
          { 0x7c08dc063181afeal,0x75c1b050a34eca91l,0x7d3479d54b9e2333l,
            0xed16640af3951aa3l },
          0 },
        /* 15 << 136 */
        { { 0x911b596264723e54l,0x34384f8c004b327cl,0x06ca5c61b85435f2l,
            0x12e0cd25e2c1075cl },
          { 0xa4b84cb8ac727394l,0x50bd720492b352c1l,0xe85524a49cbd0fb4l,
            0x10b9274be7876024l },
          0 },
        /* 16 << 136 */
        { { 0xef0a3fecfa181e69l,0x9ea02f8130d69a98l,0xb2e9cf8e66eab95dl,
            0x520f2beb24720021l },
          { 0x621c540a1df84361l,0x1203772171fa6d5dl,0x6e3c7b510ff5f6ffl,
            0x817a069babb2bef3l },
          0 },
        /* 17 << 136 */
        { { 0xb7cf93c3aace2c6al,0x017a96e658ff1bbfl,0x3b401301624a8250l,
            0xf5ef158529266518l },
          { 0x3c968bef7585838dl,0x8e97d023853191abl,0x175022e4f6823389l,
            0xb6a3bfc2f6a9b4c1l },
          0 },
        /* 19 << 136 */
        { { 0x515acf174591d77el,0xb393c89e3c3b25b6l,0x291e068e9c95abd7l,
            0x256b72c046c02544l },
          { 0x8172af03915ea92fl,0xc1b324ae4fcd0f03l,0x8abc779215108993l,
            0xe05fe6867ab815ael },
          0 },
        /* 21 << 136 */
        { { 0xca08d4095bc42740l,0xdd2c19d3e26e2e60l,0x27afdeded7c091fal,
            0x3b943b0faf25cb22l },
          { 0x400af8be026047e9l,0x3149b35f772b8ff9l,0x3ddb2c06f17229d9l,
            0xcd604aeadac152fcl },
          0 },
        /* 23 << 136 */
        { { 0xea2275311c0f6803l,0x9ae82d5ea394cc08l,0xc107a2cfbe32080cl,
            0x550f35a76429f6d7l },
          { 0x483c94dacfb70c0cl,0xf26f8e5d90190c94l,0x8574b3cf86bf2620l,
            0xe7258e45df9f482fl },
          0 },
        /* 25 << 136 */
        { { 0x8f8dc582da46f1cfl,0x61d76cf91e1e7427l,0x8aceb48b306c84aal,
            0xecaa142f28ebff98l },
          { 0xac5bd940401d80fel,0x0caacb8fe800cf9el,0x99068da9b3359af5l,
            0x92fdd5795225b8c0l },
          0 },
        /* 27 << 136 */
        { { 0x5a29d1c5ab56a3fbl,0x4e46ffc0a9aab4afl,0xa210472624d83080l,
            0xb5820998007f08b6l },
          { 0x9ce1188e4bc07b3el,0xbf6d0dbe32a19898l,0x5d5c68ea5b2350bal,
            0xd6c794eb3aa20b45l },
          0 },
        /* 28 << 136 */
        { { 0x3de605ba9ec598cfl,0x1933d3ae4d3029ael,0x6bf2fabd9b140516l,
            0x712dfc5559a7d01cl },
          { 0xff3eaae0d2576366l,0x36e407f948701cf8l,0xede21d89b41f4bd4l,
            0xc5292f5c666eefa9l },
          0 },
        /* 29 << 136 */
        { { 0x30045782c3ebcd77l,0xaa0cf3c73fdbe72el,0x719ec58ef8f43b39l,
            0x9716fb9972574d3al },
          { 0x300afc2b0d03ccd6l,0xb60016a34f3fac41l,0x8898910ea3a439f6l,
            0xdc00a99707ca11f5l },
          0 },
        /* 31 << 136 */
        { { 0x291b15ee8ed34662l,0xb780d54b2ee422a7l,0x5b9e3788fcfe4ccbl,
            0x4554cb8cbe8b7c3al },
          { 0xfdaccc2209a85a7fl,0x51f4a8ec555497edl,0x07dc69037da33505l,
            0xa3bc8bfcbc1fc1dbl },
          0 },
        /* 33 << 136 */
        { { 0x661638c151e25257l,0x0a6fd99c53304974l,0x29d8ae165078eec6l,
            0xed7512ad447b73del },
          { 0x0e21de607a4d0e9bl,0x842abd422462be01l,0x3be82afa5cddc709l,
            0x25bb9da99b52797dl },
          0 },
        /* 34 << 136 */
        { { 0x80613af28adc986al,0x4602284935776a41l,0x17d33e0f4665d03cl,
            0xeb12eb6c0df12b50l },
          { 0x0f0effa0ee41527fl,0x8ca2edb680531563l,0x4c354679f28c52c3l,
            0x67f1ba5c2f6df66dl },
          0 },
        /* 35 << 136 */
        { { 0x9c27207a2479fb3fl,0xef6e0f13515fb902l,0x3f7ad9e9d0d9436el,
            0x36eb4ea5893bbcf5l },
          { 0x5c53a2ac02b316b7l,0x10c75ee1f54f7585l,0x29e5879c3c7a4c1bl,
            0x77da3c82f29c67d6l },
          0 },
        /* 36 << 136 */
        { { 0xf2b75d21ef78a852l,0xba38cd34dd31a900l,0x72b3a68658ffe18al,
            0x7464190cbfd95745l },
          { 0x406e532177ed6e81l,0x1af0975bde535eabl,0x66ba22c760c54c82l,
            0x88e3b1ceb00a2fe0l },
          0 },
        /* 37 << 136 */
        { { 0xb6099b7df7e5c69bl,0x84aa1e26ba34ee2fl,0x5952600405c338bbl,
            0xe9a134374951a539l },
          { 0xb12276526ec196bdl,0x26a7be264b6dce36l,0x052e10a4e2a68458l,
            0x475fc74c1f38898bl },
          0 },
        /* 39 << 136 */
        { { 0x120167fc0a3eb4e1l,0xaa94bc70c0c21204l,0x313cd835e1243b75l,
            0x3bb63fb20bfd6a4al },
          { 0xa615dcae21ef05cfl,0x63774c2ec23c3ee5l,0x39365b1fed0dfd65l,
            0xb610e6ff5d2a2d7dl },
          0 },
        /* 40 << 136 */
        { { 0x55b7f977f0337b15l,0x3bc872a30e94973al,0x624ad983770deea0l,
            0xcaab336413a5efdbl },
          { 0x391dd0027a0d4247l,0x39590d5df312aed5l,0x532802c9351365acl,
            0xdd2e824578a2e22al },
          0 },
        /* 41 << 136 */
        { { 0x81b0d7be7f774fb8l,0x62f32bb3aa412425l,0xbe7afe26bbcd2162l,
            0xa6ce167c53c7fa7dl },
          { 0x8deca64fc5c4fc5bl,0x70e546aba6efd2fel,0xf2d8495987ff672al,
            0x2ca551f249c3059el },
          0 },
        /* 43 << 136 */
        { { 0x40b62d528eb99155l,0xe6b048947420a7e0l,0x9ebecb2bc685e58al,
            0x3ea642d8d3c8d2cbl },
          { 0x5340ac6ed489d0dfl,0xf3846d08c2b7588el,0x4cecd8a0611c289bl,
            0xdddc39c50dd71421l },
          0 },
        /* 44 << 136 */
        { { 0x98c6a6a52ebee687l,0xcdf65bfa56c1c731l,0x48e8132772def210l,
            0x4ea119418083b5a5l },
          { 0x3fdcea4fffebb525l,0x55aaea19fb50bf72l,0x5fbedc0a2a85b40cl,
            0x0d6fd954bf44f29fl },
          0 },
        /* 45 << 136 */
        { { 0x83a8302a9db4071el,0x52f104436f8ae934l,0x96de829d175b800al,
            0x20ff5035373e97cel },
          { 0xf58660185f65356al,0x992c15054c8cd782l,0x0b962c8eb57d727fl,
            0xe8a9abc92bba8bc7l },
          0 },
        /* 46 << 136 */
        { { 0x81a85ddd7cf2b565l,0x5e51e6afc34a0305l,0xa8d94ccefbc89faal,
            0x2bfd97c1e68cd288l },
          { 0x16d79c21af2958b8l,0x5e5d989defda7df8l,0x6d2f0ca6ff734c8al,
            0xfa5b8dd32cc9bafel },
          0 },
        /* 47 << 136 */
        { { 0x5787a9934e6ed688l,0x6815f3b5aab42f46l,0x7960f45b093c6c66l,
            0xb2b9829728be10cfl },
          { 0x1d4c7790296568cdl,0xa279a877f048e194l,0xcf7c20f4c6a58b4el,
            0xf0c717afa1f9c00fl },
          0 },
        /* 48 << 136 */
        { { 0x8a10b53189e800cal,0x50fe0c17145208fdl,0x9e43c0d3b714ba37l,
            0x427d200e34189accl },
          { 0x05dee24fe616e2c0l,0x9c25f4c8ee1854c1l,0x4d3222a58f342a73l,
            0x0807804fa027c952l },
          0 },
        /* 49 << 136 */
        { { 0x79730084ba196afcl,0x17d38e98054bd539l,0xc5cfff3918583239l,
            0x4b0db5a2d9adbee6l },
          { 0x9bc9f1e3c2a304e8l,0xbaa61de7de406fa8l,0x8e921ca9e4bec498l,
            0xd9f4e5ae6604ab02l },
          0 },
        /* 51 << 136 */
        { { 0xdf6b97b5b37f2097l,0x7576c3f9b4a5d2b9l,0x6eb697ed3588cabbl,
            0x4d75b38622598d8fl },
          { 0x4e6d93b522ff55e8l,0x4620ec635b8f7edal,0xd5006209f97b7749l,
            0x9e22e3a84da8b464l },
          0 },
        /* 52 << 136 */
        { { 0xbabfb7f82e8f326fl,0xed9cac225625a519l,0xf1109c1a0edae0a9l,
            0x45f80a9858521259l },
          { 0x37a44b075ab71f44l,0x21699eb64a21161bl,0xb523fddf56fe67eel,
            0x9f5c3a2120b9f72el },
          0 },
        /* 53 << 136 */
        { { 0x12c1131508b75673l,0xfa20121823b096d6l,0x839f01aeeacd6537l,
            0x0e592be787df32cal },
          { 0xfe3f65ff8b7dd0fcl,0xed09b4875c1d9a80l,0x8c09dd97b79786d8l,
            0x74eba2806c5bc983l },
          0 },
        /* 55 << 136 */
        { { 0xf917704862987b50l,0xcc84cdc6bc4ac456l,0x8bd2c922ae08fe12l,
            0x09d5f661fc2d06c7l },
          { 0xd10ac6dd9457d47fl,0x65aa30a23668060cl,0x33cddac6745161fcl,
            0xf4c18b5ea51e540fl },
          0 },
        /* 57 << 136 */
        { { 0x591c064ede723c1fl,0x92e5d4e601a4adael,0x3d7ee8a3145716ecl,
            0x0ef4c62061727816l },
          { 0x0e17c576f1bf6d6el,0x173104015ae18045l,0xdad620aae9589b75l,
            0xb10c7e2d0eda4905l },
          0 },
        /* 59 << 136 */
        { { 0xb8020f16aa08df6fl,0x03cf58ffd67054e9l,0x302e003c11fe3d1al,
            0x9c194bc1c638a3ecl },
          { 0x8ed3cb3adefd3f1el,0xc4115e079bf39de4l,0x8dece48bdf46fdf6l,
            0xebd1dbcf30eafeafl },
          0 },
        /* 60 << 136 */
        { { 0x058eb276fba319c5l,0xd33a91127f7fa54al,0xf060c1b4932a2dabl,
            0xce3a224e79c7d9bfl },
          { 0x6fb0388c0ba92823l,0x8d31738a69787881l,0x2d86eb0203cd00b7l,
            0x4e6e44512b69911bl },
          0 },
        /* 61 << 136 */
        { { 0xff2efe1cfdcca1cfl,0x08f22c69b5bb71e3l,0xc63f4a9f7023076el,
            0x88fb2aa0ce0c490el },
          { 0xcc7c97f91f77783cl,0x360026d942ab36b7l,0x547c34ecefd68f70l,
            0xebe7f99efbabfdabl },
          0 },
        /* 63 << 136 */
        { { 0xe7c1c1788613e87al,0xb035d65e60b82654l,0x055a82d03583a254l,
            0x27ce1ffc9b3b22fal },
          { 0x0cf904917ec83cd5l,0xfc6c21805604aa40l,0x1330604099357428l,
            0x9b0982f9ad4818b7l },
          0 },
        /* 64 << 136 */
        { { 0xc222653a4f0d56f3l,0x961e4047ca28b805l,0x2c03f8b04a73434bl,
            0x4c966787ab712a19l },
          { 0xcc196c42864fee42l,0xc1be93da5b0ece5cl,0xa87d9f22c131c159l,
            0x2bb6d593dce45655l },
          0 },
        /* 65 << 136 */
        { { 0x3a6080d9fb56bc3al,0xf1552dcad6212d7el,0x977ac5b59420f4f6l,
            0xef914d370e3cd97fl },
          { 0x807bd6e69c04f768l,0x743a7b552bb803f6l,0x7f5c20804215f4b0l,
            0x41e331288fc6ce42l },
          0 },
        /* 71 << 136 */
        { { 0x5a31c9ac61e6a460l,0x55102e4093e7eeddl,0x969fe0612da6adcel,
            0xe8cddc2f3ffea1d9l },
          { 0xaa26c6b1f0f327c5l,0x9e5b63743544f5e1l,0x5159fa1ddbaa685bl,
            0x9892d03aa7f44b99l },
          0 },
        /* 77 << 136 */
        { { 0x4dfcbf12e2c6fc1fl,0x703f2f5b7535ac29l,0x78f8617e82f7dc0fl,
            0x54b835ff853e792dl },
          { 0x3cc7f000df9f7353l,0x0d7ffd68db5a157al,0x2c1c33691672b21cl,
            0x694b4904ac970ef8l },
          0 },
        /* 83 << 136 */
        { { 0xd655bc42c1d2c45cl,0x572f603cbd22b05fl,0xa7fbf09388e4531al,
            0x8d38bbd91fdde98dl },
          { 0x16cc2aaa73b0fa01l,0x515019a25e8ffb04l,0xb075990611e792ccl,
            0x89df06f399112c90l },
          0 },
        /* 89 << 136 */
        { { 0x26d435c2481b46dal,0x73ab7e96266e9b3al,0x22d5b1db3c613c40l,
            0x9de4021c6727e399l },
          { 0x451ebba56051f8c9l,0xa37f6ec52c281a58l,0x3d7a28fe0e9f4cc5l,
            0x0f45bcd655b64df7l },
          0 },
        /* 95 << 136 */
        { { 0xba2a718c66616fbel,0x4b27810b3369a9acl,0x50b8391a2b426d5fl,
            0x420c88efa626fa05l },
          { 0xe39cef97b9c39a30l,0xcae7cde85e67e5d0l,0x3821f8319a58e521l,
            0xbf474d1941479509l },
          0 },
        /* 101 << 136 */
        { { 0x401bbab58fb15118l,0xb0376892dbf38b39l,0x10e4b9dd3a3ca42al,
            0xa69c2693f8063ffel },
          { 0xe10facdde07cb761l,0x96f4dde831d7759al,0xd702fdecc2cc7f9fl,
            0x9e87e46e1ac0162cl },
          0 },
        /* 107 << 136 */
        { { 0xb6cd60518479ca8fl,0xcca345e60968f6c7l,0x7b57248a64a9afe7l,
            0x5552e3511d0d4db9l },
          { 0x8f749b199dc68aabl,0x0fb86f06db1f7819l,0x23b300963143ac09l,
            0x61c166d8abfbcb9bl },
          0 },
        /* 113 << 136 */
        { { 0x4c96e85a43101165l,0x393a882fcf39bd19l,0xef9e1d42c2df6f33l,
            0xe1775c990278f088l },
          { 0xb1581929a9250d4al,0x582b0608c4168873l,0x0b3ffba3a1e68cd8l,
            0x3f78147ef9490897l },
          0 },
        /* 116 << 136 */
        { { 0x277b5177eb18ff20l,0x48002e9828f06d62l,0xece8d6c30e506d8dl,
            0x5cde0a58cd9ff963l },
          { 0x3b97cdb74e3baa0el,0x50560c0b631238f9l,0xe1c31b35cf79793dl,
            0x95d12f14355e2178l },
          0 },
        /* 119 << 136 */
        { { 0x0143f695bcc31b77l,0x3627aed14c49b65al,0x6e4f7a9ce441c183l,
            0xb708c79de1bfa0a3l },
          { 0xdbf0fc313a0726b8l,0xe04d82a8852d78bbl,0xb859001e3be5d398l,
            0x92dcc20c8e89bd11l },
          0 },
        /* 125 << 136 */
        { { 0x5f2416a3df9026b4l,0xffc01f3afcb29a1bl,0x18d02c9f1d94b20fl,
            0xd93b0f2f81cfdef3l },
          { 0xe6b0fd4713adf5f2l,0xcc9067b7ba06dff3l,0xb48c0cbb2256f842l,
            0xc2ae741dfd34df2fl },
          0 },
    },
    {
        /* 0 << 144 */
        { { 0x00, 0x00, 0x00, 0x00 },
          { 0x00, 0x00, 0x00, 0x00 },
          1 },
        /* 1 << 144 */
        { { 0x80531fe1c63c4962l,0x50541e89981fdb25l,0xdc1291a1fd4c2b6bl,
            0xc0693a17a6df4fcal },
          { 0xb2c4604e0117f203l,0x245f19630a99b8d0l,0xaedc20aac6212c44l,
            0xb1ed4e56520f52a8l },
          0 },
        /* 3 << 144 */
        { { 0x18f37a9c6bdf22dal,0xefbc432f90dc82dfl,0xc52cef8e5d703651l,
            0x82887ba0d99881a5l },
          { 0x7cec9ddab920ec1dl,0xd0d7e8c3ec3e8d3bl,0x445bc3954ca88747l,
            0xedeaa2e09fd53535l },
          0 },
        /* 4 << 144 */
        { { 0xa12b384ece53c2d0l,0x779d897d5e4606dal,0xa53e47b073ec12b0l,
            0x462dbbba5756f1adl },
          { 0x69fe09f2cafe37b6l,0x273d1ebfecce2e17l,0x8ac1d5383cf607fdl,
            0x8035f7ff12e10c25l },
          0 },
        /* 5 << 144 */
        { { 0xb7d4cc0f296c9005l,0x4b9094fa7b0aebdbl,0xe1bf10f1c00ec8d4l,
            0xd807b1c4d667c101l },
          { 0xa9412cdfbe713383l,0x435e063e81142ba1l,0x984c15ecaf0a6bdcl,
            0x592c246092a3dab9l },
          0 },
        /* 7 << 144 */
        { { 0x9365690016e23e9dl,0xcb220c6ba7cc41e1l,0xb36b20c369d6245cl,
            0x2d63c348b62e9a6al },
          { 0xa3473e19cdc0bcb5l,0x70f18b3f8f601b98l,0x8ad7a2c7cde346e4l,
            0xae9f6ec3bd3aaa64l },
          0 },
        /* 9 << 144 */
        { { 0x030223503274c7e1l,0x61ee8c934c4b6c26l,0x3c4397e3199389cel,
            0xe0082600488757cel },
          { 0xaac3a2df06b4dafbl,0x45af0700ddff5b6al,0x0a5974248c1d9fa0l,
            0x1640087d391fc68bl },
          0 },
        /* 10 << 144 */
        { { 0x26a43e41d07fa53dl,0x3154a78a74e35bc5l,0x7b768924e0da2f8cl,
            0xba964a2b23613f9al },
          { 0x5a548d35ba1d16c4l,0x2e1bfed1fb54d057l,0xff992136bc640205l,
            0xf39cb9148156df29l },
          0 },
        /* 11 << 144 */
        { { 0xf4873fcf4e5548bdl,0x8725da3f03ce57f0l,0xd82f5c95ca953258l,
            0xac647f127cf0747el },
          { 0xff2038b02d570bd5l,0xb0c2a767a13ae03fl,0xebaa27cde9932d16l,
            0xa686e3fc1234e901l },
          0 },
        /* 13 << 144 */
        { { 0x9f80435e63261eccl,0x6302a62e4337d6c9l,0x91916a49ca4958a0l,
            0x554958993149d5d3l },
          { 0x378d020b9f91de3cl,0x47b839a34dd25170l,0x2825854138b7f258l,
            0xea5b14f7437e7decl },
          0 },
        /* 15 << 144 */
        { { 0x74f08736b0018f44l,0xf4a03417b446d0f5l,0x66a4aa2fa40ca6b2l,
            0x215679f0badb60edl },
          { 0x3871195a323e4eefl,0x8f0940c320952b16l,0xfe8dac62879d5f7dl,
            0x649cb623c1a6e875l },
          0 },
        /* 16 << 144 */
        { { 0xecaff541338d6e43l,0x56f7dd734541d5ccl,0xb5d426de96bc88cal,
            0x48d94f6b9ed3a2c3l },
          { 0x6354a3bb2ef8279cl,0xd575465b0b1867f2l,0xef99b0ff95225151l,
            0xf3e19d88f94500d8l },
          0 },
        /* 17 << 144 */
        { { 0xa26a9087133ec108l,0x5dc5699f2712bdc0l,0x96903f4dd14224a9l,
            0x3da5992429e47b80l },
          { 0xb717712ff9dbba5al,0x9e52004b756391c9l,0xe669a11dcc9d219cl,
            0x3b6e6b84d1d6c07dl },
          0 },
        /* 19 << 144 */
        { { 0x5feec06a676feadbl,0xfc449bc59d69f322l,0x1d8d7b5e7cda8895l,
            0x5ed54dc11a3314a7l },
          { 0x1a11d2ae6de889c0l,0xb2a979724ced2bd9l,0x6ecf6989306a5ef6l,
            0x1611d57b8cc8a249l },
          0 },
        /* 21 << 144 */
        { { 0x2d9942ba007cbf87l,0x4e62bce6df3fc926l,0xe7eee5b0e4560affl,
            0xe51963bb7cb009b7l },
          { 0xaa5118cee29b37ddl,0x5cd84a4747263903l,0x3050caa6620055d8l,
            0x7ef576a76c4b1e3dl },
          0 },
        /* 23 << 144 */
        { { 0x9026a4dde6008ff1l,0x49e995ad1c8cd96cl,0x80722e73503e589bl,
            0x05bcbce184c2bc26l },
          { 0x255f9abbd4682c2cl,0xc42bcfc2f084d456l,0xa0eae9b0641c0767l,
            0x1b45632d864c9a2dl },
          0 },
        /* 25 << 144 */
        { { 0xcf25793b6ae024e0l,0x1b6607b484b5c4b0l,0x9579fa903f1624c8l,
            0x37fb65be68bd57e8l },
          { 0xd693a55efc39c203l,0x4e267ac4c87252e9l,0xb8d78bb09f899413l,
            0xe4c014070b3b8508l },
          0 },
        /* 27 << 144 */
        { { 0x662906e5bc3f3553l,0xde38d53531459684l,0x8f46a8c634f7280dl,
            0xaaf91b873d24198el },
          { 0xecd5ee115f9b117el,0xce00ffbe50ae8ddal,0x263a3d4e7710a9ael,
            0x0ff3f721f26ba74fl },
          0 },
        /* 28 << 144 */
        { { 0x4a8a4f47f0cefa69l,0xdc8e4cbaa4546866l,0x359ba69b23f603c1l,
            0xdab4d601187b7ac5l },
          { 0xa6ca4337c1ebc8d9l,0x9fa6585452b4074bl,0x1a4b4f81902fb733l,
            0xd2bb5d7aa525deaal },
          0 },
        /* 29 << 144 */
        { { 0xcc287ac2e6b3577al,0xd7528ca7f612003bl,0x8afdb6f12c1400b8l,
            0x103a2ed346a2dd8dl },
          { 0xc8f8c54d2ee21339l,0x8f011b92355a2d20l,0x81c6fc9f1346f2acl,
            0xdb6042f005a6d24bl },
          0 },
        /* 31 << 144 */
        { { 0xfc90e3630da4f996l,0x8ceca49daa6d6fe4l,0x1084affdbdfc619bl,
            0x2029f672c1140b04l },
          { 0x606ec25f136f3e5el,0x6d24149b02224c4al,0xabb0f142cfdfcf4cl,
            0xe40d0419fab1a0edl },
          0 },
        /* 33 << 144 */
        { { 0xcfdd08265cbccb84l,0x2258a16e88ad93c4l,0xb3ac365e728c5ad3l,
            0x0bbf97808560df1fl },
          { 0x42d08a39bad8c7b8l,0x1e3960106d3e8b91l,0xc332b39910274f58l,
            0xe0a84dacce2ea778l },
          0 },
        /* 34 << 144 */
        { { 0x113e1189ff432945l,0x4a0d2c3d04e1106cl,0xcde487744f3597b1l,
            0x853b029174fa26eal },
          { 0x2149e0ff02662e26l,0xb3181eaa5e6a030fl,0x086fc2159b006340l,
            0xa1df84a694a4e0bbl },
          0 },
        /* 35 << 144 */
        { { 0xc2cbd80ac99f8d3dl,0xe24b9d8f50ecf4f4l,0xf18d34728ecb126al,
            0x83966662e1670aael },
          { 0x1cece80fda5f594el,0x545e94ae65f391e0l,0xf3286dff93f98bb7l,
            0xf945e6cdf5abf176l },
          0 },
        /* 36 << 144 */
        { { 0x00ba5995dd95ac33l,0xa4957a40738f3bf4l,0x073539f599438a85l,
            0xcc9c43acc2eb1411l },
          { 0xe27501b5be2ec3d2l,0xa88d4ed057a85458l,0x870ae236755c8777l,
            0x0933c5af89216cbal },
          0 },
        /* 37 << 144 */
        { { 0xb5feea219e40e37fl,0x8c5ccb159e20fd60l,0xaeddc502ce8209a1l,
            0xbdf873cc11e793b3l },
          { 0xbc938103f0de8db5l,0x619fb72fb0e9d3d5l,0x800147cb588ed2adl,
            0x260f92bb7901ced8l },
          0 },
        /* 39 << 144 */
        { { 0x72dd9b089848c699l,0xc6086381185dacc1l,0x9489f11ff7d5a4c8l,
            0xedb41d5628dee90fl },
          { 0x1091db6b09af693cl,0xc7587551ae4b6413l,0x806aefb0768227adl,
            0x4214b83eafb3c88el },
          0 },
        /* 40 << 144 */
        { { 0xddfb02c4c753c45fl,0x18ca81b6f9c840fel,0x846fd09ab0f8a3e6l,
            0xb1162adde7733dbcl },
          { 0x7070ad20236e3ab6l,0xf88cdaf5b2a56326l,0x05fc8719997cbc7al,
            0x442cd4524b665272l },
          0 },
        /* 41 << 144 */
        { { 0x748819f9aa9c0ef5l,0xd7227d8ba458ad48l,0x8d67399f27aef626l,
            0xc6241a1859bf0a4cl },
          { 0xed9b0bfcc31cb9bbl,0x591254f896142555l,0x80e4bab461134151l,
            0x7c5e680243efbd83l },
          0 },
        /* 43 << 144 */
        { { 0x7f3f5a1706b9b7ddl,0x392132e75faeb417l,0x508ac4788fae38a2l,
            0x2b854ead0d3499c3l },
          { 0x26a687d8ef18bf0fl,0x62ff0c4a8ae00b61l,0x84111011f48578f2l,
            0xa879f383cd0fcd3al },
          0 },
        /* 44 << 144 */
        { { 0xeb7615aa202992f0l,0xde0562b38361d0b3l,0x789a302862027ee0l,
            0xe3e3e9921048f899l },
          { 0x07945c246deadab4l,0xeb06a15ec77d894el,0xb825af36bab1416bl,
            0x99083c4df4b4e04fl },
          0 },
        /* 45 << 144 */
        { { 0x4684a8f27b3ad6c3l,0x58238dbd928d9b6bl,0x31865b998da2c495l,
            0xc1ca784fb8e7cda1l },
          { 0xc9605dc71e081572l,0x8f560bcdef8ed104l,0x51f73981bd3feaedl,
            0xc778aa4e4251c88dl },
          0 },
        /* 46 << 144 */
        { { 0x9c0daa63aa502800l,0x73c7959a1e15b9bdl,0xd0447bcb7ab10f6cl,
            0x05b8fbc8b8311bdel },
          { 0xa8a74be1915d5c4el,0x38d41c1e0b7c0351l,0x5bb2d49ff52d6568l,
            0x6c48d8eed5e43593l },
          0 },
        /* 47 << 144 */
        { { 0x387b26d554159498l,0x92e92fad1ec34eb4l,0x0f88705e7a51b635l,
            0x66bcbf4dedca735fl },
          { 0x0a4c6112dcb896ccl,0x148e1dfe6fc72ad9l,0x3de977fd2b4c9585l,
            0x0cd6e65f741e62cal },
          0 },
        /* 48 << 144 */
        { { 0x7807f364b71698f5l,0x6ba418d29f7b605el,0xfd20b00fa03b2cbbl,
            0x883eca37da54386fl },
          { 0xff0be43ff3437f24l,0xe910b432a48bb33cl,0x4963a128329df765l,
            0xac1dd556be2fe6f7l },
          0 },
        /* 49 << 144 */
        { { 0x98ae40d53ce533bal,0x10342e1931fdd9c2l,0x54a255c8abf8b2bfl,
            0x8facc41b15f6fef7l },
          { 0x2e195565bc65b38bl,0xb9f3abaaeaea63cbl,0xede2ab9bf2b7518bl,
            0x5e84102ce9ea3d81l },
          0 },
        /* 51 << 144 */
        { { 0x162abc35113bc262l,0x8012f06829eb3fd4l,0x0e2727eb2c1ccf9cl,
            0x89561ff44b455b20l },
          { 0xc48db835ee3b1fd4l,0x4075ca86095bbfa7l,0x0c498d7d98745182l,
            0x828fb93c5dfb5205l },
          0 },
        /* 52 << 144 */
        { { 0xf95c7a5f0a76333bl,0x07603929cd607927l,0xabde328591028d3el,
            0x55765e8fa032a400l },
          { 0x3041f2cabed17cd7l,0x018a5b7b9a9e5923l,0xca4867975bb9bae3l,
            0x741c802ecc382cb5l },
          0 },
        /* 53 << 144 */
        { { 0x182a10311e5a3d8el,0xc352b8c8986c4d10l,0x7c50a172434c02ebl,
            0x121d728c4420c41cl },
          { 0x0f8eca2a8a51812fl,0xdb6c4a4ea5158430l,0x67944e0b8d8f4144l,
            0x387cc2052405c77al },
          0 },
        /* 55 << 144 */
        { { 0x98b36eb47e95ad76l,0x1973fa7d5f7e5ff7l,0xc4827abc6cc8a25cl,
            0x4263a0d3ec822ae4l },
          { 0x49f113f35217a6f4l,0xf27cc9bb81748aa6l,0x9cb81d97d822e08el,
            0x698d2826b5c360bcl },
          0 },
        /* 57 << 144 */
        { { 0x895f81514eb6d0b8l,0x32ef71df9f786536l,0x032a449430379a79l,
            0xa8c1076218bdb83fl },
          { 0x7a3b0b8fe53a4064l,0x0e724a54e2ce89b7l,0x565baeba7a31f6bcl,
            0x12b9fa6387d18a7bl },
          0 },
        /* 59 << 144 */
        { { 0x027231a3585bcfbdl,0x8690e977dca24269l,0x229c021afc6f1422l,
            0xd98050d044084cabl },
          { 0x6add95d79d4fd09al,0x12484c68c15b24ddl,0xa79a8f4facf4f551l,
            0xf53204e27a83cbecl },
          0 },
        /* 60 << 144 */
        { { 0xbc006413a906f7aal,0x9c8cd648bbeaf464l,0xaf5c7c64fb78cdf2l,
            0xe45839eafabc2375l },
          { 0x1eb89bd150012172l,0x9d0d76194488518cl,0xd55a7238bd534d32l,
            0x48f35d5e95b4fe55l },
          0 },
        /* 61 << 144 */
        { { 0xa6c5574f3e70a35al,0x35c11b5a8df97d97l,0x8f629f6cda85dd27l,
            0x94dab294c218452el },
          { 0xa2e1882e8916c731l,0xc02ce77c8929e350l,0xa7ed351fe4eff8afl,
            0xeb76ef0654c3e1c1l },
          0 },
        /* 63 << 144 */
        { { 0xc31d7cf87e3f5be5l,0x1472af0d3ce7f3a0l,0x226414f8f962e1afl,
            0xd318e3df16f54295l },
          { 0x9a3f6aaf41477cd3l,0x7034172f66ec6b2el,0xbea54eb537413a62l,
            0x79f81262dc515e73l },
          0 },
        /* 64 << 144 */
        { { 0x994f523a626332d5l,0x7bc388335561bb44l,0x005ed4b03d845ea2l,
            0xd39d3ee1c2a1f08al },
          { 0x6561fdd3e7676b0dl,0x620e35fffb706017l,0x36ce424ff264f9a8l,
            0xc4c3419fda2681f7l },
          0 },
        /* 65 << 144 */
        { { 0xb71a52b8b6bf8719l,0x0c7701f73196db36l,0xff1b936f53141cf4l,
            0x684d8a3c1b94a31cl },
          { 0xe555633ab52386e1l,0x9353a2af91450578l,0xc53db6fab99b14bcl,
            0x1f2d42adcf619d36l },
          0 },
        /* 71 << 144 */
        { { 0xbeb535ef3851c573l,0x3105fff585589843l,0xbe9f62a1d47aaf06l,
            0x6bb2ee5d107e1131l },
          { 0x82530247a4a7699fl,0x3fb475e144872afbl,0x8ad43fd73c4c49f2l,
            0x3f7632882e045fc4l },
          0 },
        /* 77 << 144 */
        { { 0x48440beb2924d7b2l,0x234163809c88fc57l,0xdc1d23d54ab08c2bl,
            0x576400b6e70feab0l },
          { 0x3b8afb8ba66da779l,0x7a7e3bf445468f16l,0x1976ddf3231f79dfl,
            0xbe61c170b8531a9el },
          0 },
        /* 83 << 144 */
        { { 0xf8d2dc768bf191b2l,0x3269e68813a39eb9l,0x104bb84be755eccfl,
            0xb8d1330f2868f807l },
          { 0x2b29c74cb06c6059l,0x3648baa1a6440a26l,0x5dfae323f1e6b2c9l,
            0x9d0319b79330ac0al },
          0 },
        /* 89 << 144 */
        { { 0x526ba3770e708bb2l,0x95c21ba327565dd9l,0x7071f46d48a0a873l,
            0xe4b9959efed6cc74l },
          { 0x1b16bfd1e08a5afal,0xc87fec98d1789782l,0x200186e946cfd068l,
            0x88ea35a7280bf3ebl },
          0 },
        /* 95 << 144 */
        { { 0x9e31943d42ac0e6cl,0xe61374cf1db8e40fl,0xbe27ea35a27db609l,
            0x7c5b91d67bf192e9l },
          { 0xc2af846defd0a24bl,0x1b2efc37669b647al,0xbfc3c38e5e58ef8al,
            0xb6afb167e13ab5a2l },
          0 },
        /* 101 << 144 */
        { { 0x08612d29b9f2aad4l,0x43c41330ad09dd17l,0xa45cb84a9f740519l,
            0x0a9ea9a7512ec031l },
          { 0x6e90dccaee747f35l,0xe4388bd1f0a1479bl,0x966140c4e20a9029l,
            0x1bb1f65d7dd956abl },
          0 },
        /* 107 << 144 */
        { { 0x066d206ea8f12bb3l,0xc9023b1b4325ec13l,0x1f56c72c96ead8ddl,
            0x454050fd8003e4c2l },
          { 0x9ca258a58917aa9dl,0xfe24b282d94593cfl,0xea66c203752741cfl,
            0x5714268c295a895el },
          0 },
        /* 113 << 144 */
        { { 0x72a9fbecc177d694l,0x38bb9387d68454d3l,0xa3d347bf590bc7d2l,
            0xcb6e292605ccc234l },
          { 0x588abfcf0d393c01l,0xf053dadf539e5568l,0xad7480fef2a8b157l,
            0xff28c8bb018cac8fl },
          0 },
        /* 116 << 144 */
        { { 0x12f1a00e7f5b8821l,0x0afa44e489b4b0cel,0x2dcaad8f6006338el,
            0x79c022cdba41242bl },
          { 0x7f6ef7e17871d350l,0x946c2a91674253adl,0xf686d137a9cbbdd9l,
            0xa47ce2eaf7d4f9f2l },
          0 },
        /* 119 << 144 */
        { { 0x1824991b205d40d6l,0x49cca1c085046a90l,0x7e23c1acd005e3c2l,
            0x093a9ae6d102c8ffl },
          { 0xf4791082d2f40843l,0xe456021811645483l,0x8a59c3b0fd3a6b39l,
            0x39130e7f820de158l },
          0 },
        /* 125 << 144 */
        { { 0xf7eef88d83b90783l,0xff60762af336d581l,0xf64f2d5dd801f5a0l,
            0x672b6ee7d6b3b8b9l },
          { 0xa2a2dceb08034d69l,0x3eca27f635638218l,0xe7065986fa17fefdl,
            0xf1b74445f5803af1l },
          0 },
    },
    {
        /* 0 << 152 */
        { { 0x00, 0x00, 0x00, 0x00 },
          { 0x00, 0x00, 0x00, 0x00 },
          1 },
        /* 1 << 152 */
        { { 0x32670d2f7189e71fl,0xc64387485ecf91e7l,0x15758e57db757a21l,
            0x427d09f8290a9ce5l },
          { 0x846a308f38384a7al,0xaac3acb4b0732b99l,0x9e94100917845819l,
            0x95cba111a7ce5e03l },
          0 },
        /* 3 << 152 */
        { { 0x37a01e48a105fc8el,0x769d754a289ba48cl,0xc08c6fe1d51c2180l,
            0xb032dd33b7bd1387l },
          { 0x953826db020b0aa6l,0x05137e800664c73cl,0xc66302c4660cf95dl,
            0x99004e11b2cef28al },
          0 },
        /* 4 << 152 */
        { { 0x214bc9a7d298c241l,0xe3b697ba56807cfdl,0xef1c78024564eadbl,
            0xdde8cdcfb48149c5l },
          { 0x946bf0a75a4d2604l,0x27154d7f6c1538afl,0x95cc9230de5b1fccl,
            0xd88519e966864f82l },
          0 },
        /* 5 << 152 */
        { { 0x1013e4f796ea6ca1l,0x567cdc2a1f792871l,0xadb728705c658d45l,
            0xf7c1ff4ace600e98l },
          { 0xa1ba86574b6cad39l,0x3d58d634ba20b428l,0xc0011cdea2e6fdfbl,
            0xa832367a7b18960dl },
          0 },
        /* 7 << 152 */
        { { 0x1ecc032af416448dl,0x4a7e8c10ec76d971l,0x854f9805b90b6eael,
            0xfd0b15324bed0594l },
          { 0x89f71848d98b5ca3l,0xd01fe5fcf039b3efl,0x4481332e627bda2el,
            0xe67cecd7a5073e41l },
          0 },
        /* 9 << 152 */
        { { 0x2ab0bce94595a859l,0x4d8c2da082084ee7l,0x21ff8be5acca3d3cl,
            0xd8b805337827f633l },
          { 0xf74e8c026becabbfl,0x9fae4dbefede4828l,0xd3885a5b3cc46bcfl,
            0x2d535e2b6e6ad144l },
          0 },
        /* 10 << 152 */
        { { 0x63d3444507d9e240l,0x6fbadf4338cff7e6l,0x8717624a959c9461l,
            0xd7d951c411fb775bl },
          { 0x4049161af6fc3a2bl,0x0dfa2547a1a8e98dl,0xeca780d439c2139cl,
            0xd8c2d8cbd73ea8efl },
          0 },
        /* 11 << 152 */
        { { 0x3aa1974f07605b28l,0x4f3d82a71e296255l,0xbbe5ea03b4e23f16l,
            0x8f5c6c6b4e654193l },
          { 0x27181182d3e8ab01l,0xc68bb231f3ba6bc2l,0x90a244d820af1fd7l,
            0x605abc055b713f4fl },
          0 },
        /* 13 << 152 */
        { { 0xca5fe19bd221991al,0x271ff066f05f400el,0x9d46ec4c9cf09896l,
            0xdcaa8dfdec4febc3l },
          { 0xaa3995a0adf19d04l,0xc98634239da573a6l,0x378058b2f2465b2bl,
            0x20d389f9b4c31612l },
          0 },
        /* 15 << 152 */
        { { 0xd7d199c7b7631c9dl,0x1322c2b8bb123942l,0xe662b68fbe8b6848l,
            0xc970faf2cde99b14l },
          { 0x61b27134b06655e5l,0xadcef8f781365d89l,0x917b5ab521b851aal,
            0x4f4472121cf694a7l },
          0 },
        /* 16 << 152 */
        { { 0x488f1185ca8d9d1al,0xadf2c77dd987ded2l,0x5f3039f060c46124l,
            0xe5d70b7571e095f4l },
          { 0x82d586506260e70fl,0x39d75ea7f750d105l,0x8cf3d0b175bac364l,
            0xf3a7564d21d01329l },
          0 },
        /* 17 << 152 */
        { { 0x241e3907fe44e547l,0x42d464c36b992187l,0xeaa8fa989ba72f28l,
            0x965a8b8f6afbb81fl },
          { 0x69356a7a8b375ea5l,0x22501ec741bdcc83l,0xf80f4e1445fb180cl,
            0xc0b12e95f5e1b822l },
          0 },
        /* 19 << 152 */
        { { 0x977234e05483dc02l,0x0167430c13d8dcb2l,0xa9971278049912edl,
            0xab044b18ca40fa39l },
          { 0xac9587449ff3896cl,0x75bb32eb860d1240l,0xf807071f6b958654l,
            0x67d2d3dc7121b4b6l },
          0 },
        /* 21 << 152 */
        { { 0x3b61e67722f9f017l,0x9c593eb1a8541696l,0xbeba950050eda653l,
            0x07b5a48f5e673f6al },
          { 0x748dca0013257aa3l,0x6bbddf9a7372e942l,0xc012f4badde83977l,
            0x6e59b327392ddb53l },
          0 },
        /* 23 << 152 */
        { { 0xb2f3fff641356603l,0x50e63537545f042bl,0x55e5149770eb530dl,
            0x5a7383c310860c3bl },
          { 0x7be30382ea669a09l,0xfdf735d289cc1c7fl,0x6e51ed844e0607cfl,
            0xdab566df4893795el },
          0 },
        /* 25 << 152 */
        { { 0x20e3be0f8920690dl,0x98db80eaac279c05l,0x4cd5c60a44b8a4f8l,
            0xeda7e91c7b0335f4l },
          { 0x45c1302a41ee5713l,0x1f6455fe588508d0l,0x82cb7311163d2fc3l,
            0xe866b90322f10b71l },
          0 },
        /* 27 << 152 */
        { { 0xc217a2e259b4041el,0x85b96ce274526cbfl,0xcbfc4f5473f12687l,
            0x097caa5fd40225e7l },
          { 0x0871ad406e91293fl,0x5f2ea207033b98ecl,0x0b3b8fac1f27d37al,
            0x7d72dd4c7f03876cl },
          0 },
        /* 28 << 152 */
        { { 0xb51a40a51e6a75c1l,0x24327c760ea7d817l,0x0663018207774597l,
            0xd6fdbec397fa7164l },
          { 0x20c99dfb13c90f48l,0xd6ac5273686ef263l,0xc6a50bdcfef64eebl,
            0xcd87b28186fdfc32l },
          0 },
        /* 29 << 152 */
        { { 0x2f0c49ac95861439l,0xcdcb051b2e36e38al,0x459474080ae20c0cl,
            0x374baad2dddf0aabl },
          { 0x291abc85d5d104a4l,0x0758001958a0657cl,0xd0f428e1a905ea13l,
            0x12599ddcf7241dbfl },
          0 },
        /* 31 << 152 */
        { { 0x16222ce81bc3c403l,0xbacc1508fc13ca02l,0xfa98db4d920ee8e9l,
            0xe5fc39c4df12a359l },
          { 0x4e8c9b90188733e8l,0x04283dd81394936cl,0x93b3db51cd130432l,
            0x33bfe3163c93ce31l },
          0 },
        /* 33 << 152 */
        { { 0xb48591e9840b1724l,0x1009559f5885ec6fl,0x45ee51121b077620l,
            0x848f9800f1f4cc8al },
          { 0x6ec1e0f74e97bceal,0x953bc23a98e80642l,0x9f0d1e8194ce7181l,
            0xeb3e6b9700eec596l },
          0 },
        /* 34 << 152 */
        { { 0x6d34b39bff7514dal,0x29ffe49825be3634l,0x63e56598f28c8b82l,
            0x78b99133aab41bcel },
          { 0x11febd5a52563180l,0xa3be94c5c356a8c0l,0x5e9b422e0d61f864l,
            0x2bf4ca1278fd259el },
          0 },
        /* 35 << 152 */
        { { 0x8f60e40266914514l,0x6d9e280fef178167l,0x2ff7aec9e2949a48l,
            0x422389ce72d37511l },
          { 0xe9b156f3307ac1d2l,0x1cb581a78518e79fl,0x56d43f302185cf82l,
            0x8d46c5aade59562cl },
          0 },
        /* 36 << 152 */
        { { 0x50fc0711745edc11l,0x9dd9ad7d3dc87558l,0xce6931fbb49d1e64l,
            0x6c77a0a2c98bd0f9l },
          { 0x62b9a6296baf7cb1l,0xcf065f91ccf72d22l,0x7203cce979639071l,
            0x09ae4885f9cb732fl },
          0 },
        /* 37 << 152 */
        { { 0xd007d682e4b35428l,0x80c162315bcdc0d6l,0xe55a86bd36fce9b2l,
            0x16772edb969a87cfl },
          { 0xff323a2d3f370c94l,0x8d3c8028bf3c1afcl,0x4e1591e73b0c3fafl,
            0xfbd6475cb981ce83l },
          0 },
        /* 39 << 152 */
        { { 0xcf414ae3315b2471l,0xf54abf8033168de6l,0x6883efc5df5cdb24l,
            0x3eca788c8efe81acl },
          { 0xdb58c6c778eeccadl,0x3c77939082fecfb7l,0x5736cdd9c9b513f3l,
            0xab7e6ea57b02aaf2l },
          0 },
        /* 40 << 152 */
        { { 0x5e7c3becee8314f3l,0x1c068aeddbea298fl,0x08d381f17c80acecl,
            0x03b56be8e330495bl },
          { 0xaeffb8f29222882dl,0x95ff38f6c4af8bf7l,0x50e32d351fc57d8cl,
            0x6635be5217b444f0l },
          0 },
        /* 41 << 152 */
        { { 0x2cec7ba64805d895l,0x4c8399870ac78e7cl,0x031ad6c7f79416c5l,
            0x1b2f2621f1838d2fl },
          { 0x60835eac91447f90l,0x59147af1f9bab5d9l,0x7a3005d6f393f175l,
            0x8cf3c468c4120ba2l },
          0 },
        /* 43 << 152 */
        { { 0xeccffc7d8a2c1f08l,0x308916d37e384bd4l,0x6b8c2ff55e366384l,
            0xf4b2850d03e4747cl },
          { 0xe839c569e96c1488l,0xa46ff7f956c9cb10l,0xd968c74c362fd172l,
            0x2aa7fe4cad6bb601l },
          0 },
        /* 44 << 152 */
        { { 0x04d15276a5177900l,0x4e1dbb47f6858752l,0x5b475622c615796cl,
            0xa6fa0387691867bfl },
          { 0xed7f5d562844c6d0l,0xc633cf9b03a2477dl,0xf6be5c402d3721d6l,
            0xaf312eb7e9fd68e6l },
          0 },
        /* 45 << 152 */
        { { 0xf3b8164eec04c847l,0xa305ca93fe65816cl,0xa65f9963c7e2ce52l,
            0xc448005198882cfcl },
          { 0x46a998df05c165bbl,0xc38f4edf9dfe1e98l,0xb96ec43f8739f77al,
            0x10a23af9313b40bfl },
          0 },
        /* 46 << 152 */
        { { 0xe476c3e3ee668e0cl,0xcec6a984478197c2l,0xc9fa1d68897147c1l,
            0x4e6aec0ea6465793l },
          { 0xedca9db76b219c3bl,0xa2cd57942e508d3bl,0x38b384663936e02al,
            0x0b8d3b4ca54ce90fl },
          0 },
        /* 47 << 152 */
        { { 0x66e06537af08e0fcl,0x70fe0f2a907f1a93l,0x8c25245285ec1647l,
            0x0b8b2964d5560eddl },
          { 0xda45a326f3ef8e14l,0xf3adf9a6abc3494bl,0xbbdd93c11eda0d92l,
            0x1b5e12c609912773l },
          0 },
        /* 48 << 152 */
        { { 0x242792d2e7417ce1l,0xff42bc71970ee7f5l,0x1ff4dc6d5c67a41el,
            0x77709b7b20882a58l },
          { 0x3554731dbe217f2cl,0x2af2a8cd5bb72177l,0x58eee769591dd059l,
            0xbb2930c94bba6477l },
          0 },
        /* 49 << 152 */
        { { 0x5d9d507551d01848l,0x53dadb405b600d1el,0x7ba5b4dc5cb0a9a3l,
            0xdb85b04c6795e547l },
          { 0x480e7443f0354843l,0xc7efe6e813012322l,0x479b674a2aeee1e6l,
            0xf5481f19704f4ea3l },
          0 },
        /* 51 << 152 */
        { { 0x76a38d6978c7816el,0xe020c87df84ec554l,0x99af2f78f9818010l,
            0x31cf103d988136eal },
          { 0x6b095a114816a5aal,0x5a4cd2a4eff0a4afl,0x543041a5892e5e04l,
            0x460f94c30aab9ee1l },
          0 },
        /* 52 << 152 */
        { { 0x863ee0477d930cfcl,0x4c262ad1396fd1f4l,0xf4765bc8039af7e1l,
            0x2519834b5ba104f6l },
          { 0x7cd61b4cd105f961l,0xa5415da5d63bca54l,0x778280a088a1f17cl,
            0xc49689492329512cl },
          0 },
        /* 53 << 152 */
        { { 0x282d92b48cd3948al,0x95d219dfe168205bl,0xf6111a6f87bf3abcl,
            0x910f8ce655fee9f2l },
          { 0xb6c806f74f71ac89l,0xd0cc300fb7235f73l,0xfe37ccb47d0d45bbl,
            0x5b2445f6952f0eaal },
          0 },
        /* 55 << 152 */
        { { 0x03870be447141962l,0x8b79033f4a2b3f7fl,0xb6983b5ed2e5e274l,
            0x2a2f8018501ed99cl },
          { 0x07a92eb9feb49656l,0x063f0a9e482e2972l,0x413be27a57435832l,
            0x56363c5f6f9d3de1l },
          0 },
        /* 57 << 152 */
        { { 0xd247153163b50214l,0x32b435eeb2b897del,0xc49f0b01b05df4del,
            0x97b6aa40b7df9b91l },
          { 0x58ff34ec8ec39d78l,0xab0889005e0114a3l,0x6872b4de4822b7b8l,
            0x7614c0d0ab239073l },
          0 },
        /* 59 << 152 */
        { { 0x81891d378aa5d80al,0xf48ca24292e45f2cl,0xba711b6c0d04904cl,
            0x5992cda349f16ed6l },
          { 0x18b9a739790593eel,0x8b98e84dc4ba16d1l,0xac55701cb7b81615l,
            0xadb4533b15822291l },
          0 },
        /* 60 << 152 */
        { { 0x6210db7181236c97l,0x74f7685b3ee0781fl,0x4df7da7ba3e41372l,
            0x2aae38b1b1a1553el },
          { 0x1688e222f6dd9d1bl,0x576954485b8b6487l,0x478d21274b2edeaal,
            0xb2818fa51e85956al },
          0 },
        /* 61 << 152 */
        { { 0xc0677533f255ba8el,0x2bdae2a1efa2aabel,0xf7aebbd4b086c8a6l,
            0x148455d992cb1147l },
          { 0xa084e8d715402565l,0x33f111a8fa41bf23l,0x4bc990d627ac189bl,
            0x48dbe6569d505f76l },
          0 },
        /* 63 << 152 */
        { { 0x59df7fab596766f3l,0x4cadcbfe604f26e4l,0x0cf199338a6af592l,
            0x3af1ace287b826c1l },
          { 0xf09a5b38ee60684el,0xa04cbeda4ed7c711l,0xdb28c42eb1731040l,
            0x75fcc0ec2e6e6523l },
          0 },
        /* 64 << 152 */
        { { 0x1e6adddaf176f2c0l,0x01ca4604e2572658l,0x0a404ded85342ffbl,
            0x8cf60f96441838d6l },
          { 0x9bbc691cc9071c4al,0xfd58874434442803l,0x97101c85809c0d81l,
            0xa7fb754c8c456f7fl },
          0 },
        /* 65 << 152 */
        { { 0x4374020072196f30l,0x59ed0dc0dcd6c935l,0x17d4ed8e5034161bl,
            0x8abe3e13009e7170l },
          { 0xe51c41c96c791456l,0xc671807704d72bb6l,0xd4309cf56bba424al,
            0x6122b951d0ca4ceal },
          0 },
        /* 71 << 152 */
        { { 0xdfdb2e9c4278982bl,0xf3a282b32d6a2a61l,0x5611650cd2f2b03cl,
            0xa62c177f43f7f83al },
          { 0x372310ab4c593d32l,0x2bb6903a2b570f9cl,0x2930da3df43af904l,
            0x2bbd04aa2c8a5a7dl },
          0 },
        /* 77 << 152 */
        { { 0x10c324c007e536del,0xc456836d377be1b4l,0x9a627d75d785af3fl,
            0xde74559118b58b31l },
          { 0xeac83ea60c47239al,0x35da24abbc02f670l,0x2d4abde0c3af6e63l,
            0xac53acba5a7ebf1bl },
          0 },
        /* 83 << 152 */
        { { 0x2b03ec2efd9a9f3el,0xc967cd2b9d898a09l,0xb24bcba8039dc4f6l,
            0x0ea1d297061ada1el },
          { 0x3a7a25fbc134b8bcl,0x846282d6f61cd312l,0xfa1de0d2e0d778d9l,
            0xf75fad4ef09be264l },
          0 },
        /* 89 << 152 */
        { { 0x7d35695bcf74afb3l,0x34d43d9f15bb36fbl,0x15f0b43960b45fbel,
            0xb15db8d84f38ec06l },
          { 0x93ce7d50f7da1406l,0x2db97edd9f076aaal,0x27ebb9aa354429dcl,
            0xf97eb5c446ace469l },
          0 },
        /* 95 << 152 */
        { { 0x758fa2312dcf498fl,0xaa8c14d15cf3853al,0x416f5dab097d786al,
            0xceec00ef38f242a0l },
          { 0x2f8b10b9d8b75ef2l,0xee64912b2281be6al,0xa883481aa382a51el,
            0x9442300f61b16b8al },
          0 },
        /* 101 << 152 */
        { { 0x80e7fbc4f4b171e1l,0xdd2246f5661564a4l,0xcf08d73cd00d4e54l,
            0xf725f5389fca9a30l },
          { 0xd9607358af20debel,0xa97c81e16f7d1cf2l,0x72794ae70dedfb2al,
            0xc328cb93159ff29dl },
          0 },
        /* 107 << 152 */
        { { 0xaf9491d6252f6d59l,0x6744d7518feda60dl,0xa485f8aa34c5c048l,
            0x2ed794b4b50ea53bl },
          { 0x0da82650db26c289l,0xed3ab4c50904af55l,0x425eda1176544463l,
            0x917be5f48939b29bl },
          0 },
        /* 113 << 152 */
        { { 0xa2e72d0f8e208e5dl,0x5a5e4344234a5fedl,0x6dcc56535005bee8l,
            0x09d0c254854e2e04l },
          { 0xade4bcdba82f0789l,0x5a3e3cd4ec460a91l,0x6b1a867be76695b2l,
            0xd1eb9df0a28b9331l },
          0 },
        /* 116 << 152 */
        { { 0x3f5cf5f678e62ddcl,0x2267c45407fd752bl,0x5e361b6b5e437bbel,
            0x95c595018354e075l },
          { 0xec725f85f2b254d9l,0x844b617d2cb52b4el,0xed8554f5cf425fb5l,
            0xab67703e2af9f312l },
          0 },
        /* 119 << 152 */
        { { 0x8dcc920005fb96bbl,0x29d2442470f84705l,0x540bb6e63f09628fl,
            0x07f8b4de2a9c2359l },
          { 0xb8e002d1957e41dcl,0x9a0fe82b9e683a3fl,0x996b1a5250e633fdl,
            0x748a11e500c669cal },
          0 },
        /* 125 << 152 */
        { { 0x0593a788581dfd6el,0x99f1164f64e1b329l,0x1142c44b1defddbbl,
            0xbc95c9c7660b9036l },
          { 0xf24b5a47079179ccl,0x6175b52c21f7033bl,0x8b5d84183bc2eec0l,
            0xc1332c8272d12670l },
          0 },
    },
    {
        /* 0 << 160 */
        { { 0x00, 0x00, 0x00, 0x00 },
          { 0x00, 0x00, 0x00, 0x00 },
          1 },
        /* 1 << 160 */
        { { 0xd433e50f6d3549cfl,0x6f33696ffacd665el,0x695bfdacce11fcb4l,
            0x810ee252af7c9860l },
          { 0x65450fe17159bb2cl,0xf7dfbebe758b357bl,0x2b057e74d69fea72l,
            0xd485717a92731745l },
          0 },
        /* 3 << 160 */
        { { 0x6c8d0aa9b898fd52l,0x2fb38a57be9af1a7l,0xe1f2b9a93b4f03f8l,
            0x2b1aad44c3f0cc6fl },
          { 0x58b5332e7cf2c084l,0x1c57d96f0367d26dl,0x2297eabdfa6e4a8dl,
            0x65a947ee4a0e2b6al },
          0 },
        /* 4 << 160 */
        { { 0xaaafafb0285b9491l,0x01a0be881e4c705el,0xff1d4f5d2ad9caabl,
            0x6e349a4ac37a233fl },
          { 0xcf1c12464a1c6a16l,0xd99e6b6629383260l,0xea3d43665f6d5471l,
            0x36974d04ff8cc89bl },
          0 },
        /* 5 << 160 */
        { { 0xf535b616fdd5b854l,0x592549c85728719fl,0xe231468606921cadl,
            0x98c8ce34311b1ef8l },
          { 0x28b937e7e9090b36l,0x67fc3ab90bf7bbb7l,0x12337097a9d87974l,
            0x3e5adca1f970e3fel },
          0 },
        /* 7 << 160 */
        { { 0xcdcc68a7b3f85ff0l,0xacd21cdd1a888044l,0xb6719b2e05dbe894l,
            0xfae1d3d88b8260d4l },
          { 0xedfedece8a1c5d92l,0xbca01a94dc52077el,0xc085549c16dd13edl,
            0xdc5c3bae495ebaadl },
          0 },
        /* 9 << 160 */
        { { 0xcc17063fbe7b643al,0x7872e1c846085760l,0x86b0fffbb4214c9el,
            0xb18bbc0e72bf3638l },
          { 0x8b17de0c722591c9l,0x1edeab1948c29e0cl,0x9fbfd98ef4304f20l,
            0x2d1dbb6b9c77ffb6l },
          0 },
        /* 10 << 160 */
        { { 0xf53f2c658ead09f7l,0x1335e1d59780d14dl,0x69cc20e0cd1b66bcl,
            0x9b670a37bbe0bfc8l },
          { 0xce53dc8128efbeedl,0x0c74e77c8326a6e5l,0x3604e0d2b88e9a63l,
            0xbab38fca13dc2248l },
          0 },
        /* 11 << 160 */
        { { 0x255616d3c7141771l,0xa86691ab2f226b66l,0xda19fea4b3ca63a9l,
            0xfc05dc42ae672f2bl },
          { 0xa9c6e786718ba28fl,0x07b7995b9c66b984l,0x0f434f551b3702f2l,
            0xd6f6212fda84eeffl },
          0 },
        /* 13 << 160 */
        { { 0x4b0e7987b5b41d78l,0xea7df9074bf0c4f8l,0xb4d03560fab80ecdl,
            0x6cf306f6fb1db7e5l },
          { 0x0d59fb5689fd4773l,0xab254f4000f9be33l,0x18a09a9277352da4l,
            0xf81862f5641ea3efl },
          0 },
        /* 15 << 160 */
        { { 0xb59b01579f759d01l,0xa2923d2f7eae4fdel,0x18327757690ba8c0l,
            0x4bf7e38b44f51443l },
          { 0xb6812563b413fc26l,0xedb7d36379e53b36l,0x4fa585c4c389f66dl,
            0x8e1adc3154bd3416l },
          0 },
        /* 16 << 160 */
        { { 0xd3b3a13f1402b9d0l,0x573441c32c7bc863l,0x4b301ec4578c3e6el,
            0xc26fc9c40adaf57el },
          { 0x96e71bfd7493cea3l,0xd05d4b3f1af81456l,0xdaca2a8a6a8c608fl,
            0x53ef07f60725b276l },
          0 },
        /* 17 << 160 */
        { { 0x971e9eedd5098497l,0x97692be63077d8a7l,0xb57e02ad79625a8al,
            0x5e3d20f6a688ecd5l },
          { 0xa4431a28188f964dl,0xd4eb23bd5a11c1dbl,0xfcda853eadc7446fl,
            0x9e2e98b593c94046l },
          0 },
        /* 19 << 160 */
        { { 0x4a649b66eddaa4f1l,0x35a04f185e690c50l,0x1639bdcff908bc53l,
            0xce6d525c121726e8l },
          { 0x70f34948902b402cl,0x3a40c6950e290579l,0x7b0ed90f469a0085l,
            0xecb979c60189c501l },
          0 },
        /* 21 << 160 */
        { { 0x847e2bde5cee8d07l,0x1bed198cd3340037l,0x439ffb3ce41586e3l,
            0x594980f1856f15b0l },
          { 0x22c3b86c6e9307c6l,0xf8b3ee08876382dbl,0x850c628e628f3f30l,
            0x22ec0acb51ee3659l },
          0 },
        /* 23 << 160 */
        { { 0xa4052591efcef5a0l,0x82692a47106d55afl,0xdac3ea88e6ead453l,
            0xaa1368fcf3dfd875l },
          { 0x87bc688aa0c539eal,0x905e206040b1de3el,0x072240b8f1d52452l,
            0x3ebf0644d57b6580l },
          0 },
        /* 25 << 160 */
        { { 0x12109bcc07a0b2f8l,0x336f87d2ca23f14cl,0xb39ae282452a2ea2l,
            0x8e085f5bab59a500l },
          { 0xf7daeb69b63f015cl,0x44c555bcacb47b38l,0x96190454b623910al,
            0x4b666e2255b41b70l },
          0 },
        /* 27 << 160 */
        { { 0xf146914eb53419fdl,0xd2109b07493e88bfl,0x30bf9cbccc54bcd5l,
            0xcf9ea59750e34a1fl },
          { 0x70ade8a59588591dl,0xf668be676b41c269l,0x3497c58f78df2e6bl,
            0x0fad05cc71042b56l },
          0 },
        /* 28 << 160 */
        { { 0x27f536e049ce89e7l,0x18908539cc890cb5l,0x308909abd83c2aa1l,
            0xecd3142b1ab73bd3l },
          { 0x6a85bf59b3f5ab84l,0x3c320a68f2bea4c6l,0xad8dc5386da4541fl,
            0xeaf34eb0b7c41186l },
          0 },
        /* 29 << 160 */
        { { 0x709da836093aa5f6l,0x567a9becb4644edel,0xae02a46044466b0cl,
            0xc80b237a407f1b3bl },
          { 0x451df45ab4168a98l,0xdc9b40ef24a3f7c9l,0x23593ef32671341dl,
            0x40f4533190b90faal },
          0 },
        /* 31 << 160 */
        { { 0x7f97768e922f36e3l,0x936943f8491034a2l,0x72f6c17f21483753l,
            0x5489fa0cb2918619l },
          { 0x55b31aa59cc21a46l,0xde4cc71a8e54ab14l,0x942cb8be9eaff8b0l,
            0xe38f6116d1755231l },
          0 },
        /* 33 << 160 */
        { { 0xf0c0606a395b39abl,0x0efcbc699b5166a5l,0x85995e6895453d85l,
            0xadc9a2920806ee5cl },
          { 0xc3662e804928fe09l,0x2a2ddcc6969c87e7l,0xa02d7947111d319dl,
            0xde23bcf12d20f66dl },
          0 },
        /* 34 << 160 */
        { { 0xc47cb3395f6d4a09l,0x6b4f355cee52b826l,0x3d100f5df51b930al,
            0xf4512fac9f668f69l },
          { 0x546781d5206c4c74l,0xd021d4d4cb4d2e48l,0x494a54c2ca085c2dl,
            0xf1dbaca4520850a8l },
          0 },
        /* 35 << 160 */
        { { 0xb2d15b14a911cc2bl,0xab2dfaf7643e28eal,0xfccc9ed1f52c4c2dl,
            0xfb4b1d4a09d8faa3l },
          { 0x6fd72a9b7f5ce767l,0x0233c856a287e2b5l,0xd42135e05775ebb9l,
            0xb3c9dada7376568bl },
          0 },
        /* 36 << 160 */
        { { 0x63c79326490a1acal,0xcb64dd9c41526b02l,0xbb772591a2979258l,
            0x3f58297048d97846l },
          { 0xd66b70d17c213ba7l,0xc28febb5e8a0ced4l,0x6b911831c10338c1l,
            0x0d54e389bf0126f3l },
          0 },
        /* 37 << 160 */
        { { 0x5952996b5306af1bl,0x99f444f4354b67bel,0x6f670181633a2928l,
            0x289023f0e9bdc4a6l },
          { 0xcbed12148f7455a2l,0x501ace2f659a4858l,0x83ee678d5f8e1784l,
            0x95c984587335c5bdl },
          0 },
        /* 39 << 160 */
        { { 0x2e25a1f3e0233000l,0xed0028cd44fe8ba9l,0x447501a6021d43b3l,
            0x4ec203906b4dffccl },
          { 0x50642f9ad0169740l,0x9360003373cc58adl,0x825f1a82fe9cf9acl,
            0x456194c653242bd6l },
          0 },
        /* 40 << 160 */
        { { 0x40242efeb483689bl,0x2575d3f6513ac262l,0xf30037c80ca6db72l,
            0xc9fcce8298864be2l },
          { 0x84a112ff0149362dl,0x95e575821c4ae971l,0x1fa4b1a8945cf86cl,
            0x4525a7340b024a2fl },
          0 },
        /* 41 << 160 */
        { { 0x83205e8f5db5e2b1l,0x94e7a2621e311c12l,0xe1cac7333e37068fl,
            0xe3f43f6d39965acfl },
          { 0xd28db9e854d905bal,0x686f372a101f2162l,0x409cfe5d3d1b46d4l,
            0x17648f1cbd0bb63al },
          0 },
        /* 43 << 160 */
        { { 0xef83315b821f4ee4l,0xb90766998ba78b4dl,0xee6a15880fce5260l,
            0x828f4a72d754affbl },
          { 0x4650ec7daaae54d2l,0x3174301f1057efe9l,0x174e0683eb7704cel,
            0xb7e6aeb357eb0b14l },
          0 },
        /* 44 << 160 */
        { { 0xcaead1c2c905d85fl,0xe9d7f7900733ae57l,0x24c9a65cf07cdd94l,
            0x7389359ca4b55931l },
          { 0xf58709b7367e45f7l,0x1f203067cb7e7adcl,0x82444bffc7b72818l,
            0x07303b35baac8033l },
          0 },
        /* 45 << 160 */
        { { 0xd59528fb38a0dc96l,0x8179dc9088d0e857l,0x55e9ba039ed4b1afl,
            0x8a2c0dc787b74cacl },
          { 0xe8ca91aeef1c0006l,0x67f59ab2de0e15d4l,0xba0cddf86e6634d2l,
            0x352803657b7ba591l },
          0 },
        /* 46 << 160 */
        { { 0x1e1ee4e4d13b7ea1l,0xe6489b24e0e74180l,0xa5f2c6107e70ef70l,
            0xa1655412bdd10894l },
          { 0x555ebefb7af4194el,0x533c1c3c8e89bd9cl,0x735b9b5789895856l,
            0x15fb3cd2567f5c15l },
          0 },
        /* 47 << 160 */
        { { 0xef07bfedfb0986c7l,0xde138afe47c1659al,0x8b79c159a555e907l,
            0x21d572f1125518bbl },
          { 0x2005999ad320410cl,0x4167dc469484414bl,0x0cd965c34c6aaefdl,
            0x2a1abc9a0e1d5e9dl },
          0 },
        /* 48 << 160 */
        { { 0x057fed45526f09fdl,0xe8a4f10c8128240al,0x9332efc4ff2bfd8dl,
            0x214e77a0bd35aa31l },
          { 0x32896d7314faa40el,0x767867ec01e5f186l,0xc9adf8f117a1813el,
            0xcb6cda7854741795l },
          0 },
        /* 49 << 160 */
        { { 0xadfaf39b888dedf1l,0x4f8b178aab1750b9l,0x26418617ffe6b0eal,
            0x01d1be82af04a59fl },
          { 0x41584147e652db64l,0xf7775ac5727f9ea7l,0x58052a20e72ad8bbl,
            0x5badf0dc6021160el },
          0 },
        /* 51 << 160 */
        { { 0x8490ea99183de59dl,0xc95f72146f5c6f8cl,0x89b55d15df00c334l,
            0x84386ad8a0ec36f7l },
          { 0x24dadaefe4dc1ed1l,0xc606ba4c1e717227l,0x7e4756c0bbfa62eal,
            0x3916cf14afc29cf3l },
          0 },
        /* 52 << 160 */
        { { 0xb7b4d00101dae185l,0x45434e0b9b7a94bcl,0xf54339affbd8cb0bl,
            0xdcc4569ee98ef49el },
          { 0x7789318a09a51299l,0x81b4d206b2b025d8l,0xf64aa418fae85792l,
            0x3e50258facd7baf7l },
          0 },
        /* 53 << 160 */
        { { 0x4152c508492d91f3l,0x59d6cf9c678f9db4l,0xb0a8c966404608d1l,
            0xdced55d0e3fed558l },
          { 0x0914a3cb33a76188l,0x79df212423d35d46l,0x2322507fca13b364l,
            0x0aed41d60078ab93l },
          0 },
        /* 55 << 160 */
        { { 0x7acdaa7f6b2ebfc2l,0xb5ab1a9a80d9f67fl,0x53ba8173ff8aa8b0l,
            0x9cd85cf874ca56a6l },
          { 0xabac57f49c4fad81l,0x2325bb8521078995l,0xbac5e3a1b928a054l,
            0x7219047a2394cc2al },
          0 },
        /* 57 << 160 */
        { { 0xa33410d2aa75fd37l,0x821093affc0f1192l,0xe45e85ed155e39a9l,
            0xd0e87cd12de67188l },
          { 0xdeca97d965d43d87l,0x8c73826f9d2c99ecl,0x1bfe111e33237ddbl,
            0xda32e865587bfb28l },
          0 },
        /* 59 << 160 */
        { { 0xde456d92c89e9e4el,0xe45688a98e47f3cdl,0x3deacfca3bacbde0l,
            0xdf9b32efc9683a70l },
          { 0x749bc007e1691106l,0x788a05342a5154d7l,0x1a06baecf7c7b70dl,
            0xb5b608eeae6ffc4cl },
          0 },
        /* 60 << 160 */
        { { 0x4cd296df5579bea4l,0x10e35ac85ceedaf1l,0x04c4c5fde3bcc5b1l,
            0x95f9ee8a89412cf9l },
          { 0x2c9459ee82b6eb0fl,0x2e84576595c2aaddl,0x774a84aed327fcfel,
            0xd8c937220368d476l },
          0 },
        /* 61 << 160 */
        { { 0x39ebf947ccd25abbl,0x74e7a868cb49ebael,0x576ea108332e6147l,
            0xcf3ba166150c1e5dl },
          { 0xb5411fc3515c0e93l,0x51b15761f15c8a34l,0x362a4a3a0d213f38l,
            0xf6f63c2e24e93aeal },
          0 },
        /* 63 << 160 */
        { { 0x0cb3a2dcb78528d5l,0xa1888c18d585bb41l,0x210cca40de402a6el,
            0x10c6339d9ed7c381l },
          { 0xcd3558d561fe2a0cl,0xc97db05dad5140b1l,0x3366b028b21f8d11l,
            0x878b09033e38be13l },
          0 },
        /* 64 << 160 */
        { { 0x211cde10296c36efl,0x7ee8967282c4da77l,0xb617d270a57836dal,
            0xf0cd9c319cb7560bl },
          { 0x01fdcbf7e455fe90l,0x3fb53cbb7e7334f3l,0x781e2ea44e7de4ecl,
            0x8adab3ad0b384fd0l },
          0 },
        /* 65 << 160 */
        { { 0x081e505aa353ba05l,0x244ab34a288b86b1l,0x1155f06214e3a829l,
            0x383300daf2118a6bl },
          { 0xe8fc17cef27032b9l,0xed7f05c9c7bd2389l,0x78f70d14202f8a88l,
            0x8a8310c0647b3f20l },
          0 },
        /* 71 << 160 */
        { { 0xc80786e1a3633369l,0x496d55de9073f5b9l,0x10deeb6a89ae93cel,
            0x6a2dd5c8b12e00c6l },
          { 0xc25cd2f90c68e26dl,0x29d7ad8b53f0bb64l,0x2dd0d027d7fc9b00l,
            0xad21e1f7ca9c4d5dl },
          0 },
        /* 77 << 160 */
        { { 0xd45cb932d83465f3l,0x95830c0faf22fdbdl,0x41d830e007cd2a0al,
            0x4a08500e3616e716l },
          { 0x5931fc9f277755a5l,0x7d11680731006764l,0xa409a0ad1b3999aal,
            0xec70368c9939d566l },
          0 },
        /* 83 << 160 */
        { { 0x3905cb59f2030370l,0x7e9bdee56dcc8fd7l,0xb1b7b04e9806e06fl,
            0xfbdadce22c73eb57l },
          { 0xfb1ab2e98d5b2eb3l,0x58fbf2df7699338bl,0x81b1c54a63b5a032l,
            0xefd1a1896a5d7ff4l },
          0 },
        /* 89 << 160 */
        { { 0x0265189da1f769eal,0x22fa0bbbfdb5a502l,0xf69f0d1b21027534l,
            0x64302b81f6066b99l },
          { 0xdef85fc98a717e80l,0xe066166386879a3bl,0xe5489b347f95b22cl,
            0x106dca9aa054a563l },
          0 },
        /* 95 << 160 */
        { { 0xd624b4f4b4be9a77l,0x21a11ed77d50acb1l,0x707181f43d406e11l,
            0x3f324d203ef158bcl },
          { 0xb29a2a34aa8cc8del,0x482f4a15315db969l,0x42ce4fc7d9af272el,
            0x784665b1f8f4cdc4l },
          0 },
        /* 101 << 160 */
        { { 0x66ff7f73ab43a863l,0xa90be2cba77fd07el,0x84843997f76e5288l,
            0x288c197f3cee129bl },
          { 0x39acc080c0a060a6l,0x4c8e574bd24e27cal,0x1dd6170ffcd3d5e9l,
            0x9736bb51f75e5150l },
          0 },
        /* 107 << 160 */
        { { 0x2133810e6ba75716l,0x4debf728712886a8l,0x351e46a1f527d1f3l,
            0x29709ae8e9591564l },
          { 0x696163d3a3dc1780l,0xd5b7825ae02aadf3l,0x23579d7cd565ae68l,
            0x105380124fa42cecl },
          0 },
        /* 113 << 160 */
        { { 0x04eb554d13ffa704l,0x7441a62f2ed33d20l,0xaa926fa0b5b81324l,
            0xb981bcb829836f61l },
          { 0x313a78d4cc9a7a15l,0xff1242d11b3921d2l,0xc0053fd36a209d4dl,
            0x95ac85caf7e92ca9l },
          0 },
        /* 116 << 160 */
        { { 0x6d2a483d6f73c51el,0xa4cb2412ea0dc2ddl,0x50663c411eb917ffl,
            0x3d3a74cfeade299el },
          { 0x29b3990f4a7a9202l,0xa9bccf59a7b15c3dl,0x66a3ccdca5df9208l,
            0x48027c1443f2f929l },
          0 },
        /* 119 << 160 */
        { { 0xdf8a6f9673c3f6fbl,0xe4b1f0d98cc03220l,0x5ddacd618350480cl,
            0x485c4fababdfb016l },
          { 0xdc840628b4d424b7l,0x07d3a99c215b2359l,0xad3dc5af56dff52el,
            0x5a3a6754973b6825l },
          0 },
        /* 125 << 160 */
        { { 0xcfe231b83539a06dl,0xb36d1f72f46770ddl,0x126049747bb900d6l,
            0x8d0990973fc31661l },
          { 0x03b2749c920bc39el,0xf933d510b0486e23l,0x09cc958f0e9b0bb5l,
            0x0b254dd1aa1e23abl },
          0 },
    },
    {
        /* 0 << 168 */
        { { 0x00, 0x00, 0x00, 0x00 },
          { 0x00, 0x00, 0x00, 0x00 },
          1 },
        /* 1 << 168 */
        { { 0x263a2cfb9db3b381l,0x9c3a2deed4df0a4bl,0x728d06e97d04e61fl,
            0x8b1adfbc42449325l },
          { 0x6ec1d9397e053a1bl,0xee2be5c766daf707l,0x80ba1e14810ac7abl,
            0xdd2ae778f530f174l },
          0 },
        /* 3 << 168 */
        { { 0xadbaeb79b6828f36l,0x9d7a025801bd5b9el,0xeda01e0d1e844b0cl,
            0x4b625175887edfc9l },
          { 0x14109fdd9669b621l,0x88a2ca56f6f87b98l,0xfe2eb788170df6bcl,
            0x0cea06f4ffa473f9l },
          0 },
        /* 4 << 168 */
        { { 0x43ed81b5c4e83d33l,0xd9f358795efd488bl,0x164a620f9deb4d0fl,
            0xc6927bdbac6a7394l },
          { 0x45c28df79f9e0f03l,0x2868661efcd7e1a9l,0x7cf4e8d0ffa348f1l,
            0x6bd4c284398538e0l },
          0 },
        /* 5 << 168 */
        { { 0x2618a091289a8619l,0xef796e606671b173l,0x664e46e59090c632l,
            0xa38062d41e66f8fbl },
          { 0x6c744a200573274el,0xd07b67e4a9271394l,0x391223b26bdc0e20l,
            0xbe2d93f1eb0a05a7l },
          0 },
        /* 7 << 168 */
        { { 0x7efa14b84444896bl,0x64974d2ff94027fbl,0xefdcd0e8de84487dl,
            0x8c45b2602b48989bl },
          { 0xa8fcbbc2d8463487l,0xd1b2b3f73fbc476cl,0x21d005b7c8f443c0l,
            0x518f2e6740c0139cl },
          0 },
        /* 9 << 168 */
        { { 0xae51dca2a91f6791l,0x2abe41909baa9efcl,0xd9d2e2f4559c7ac1l,
            0xe82f4b51fc9f773al },
          { 0xa77130274073e81cl,0xc0276facfbb596fcl,0x1d819fc9a684f70cl,
            0x29b47fddc9f7b1e0l },
          0 },
        /* 10 << 168 */
        { { 0x358de103459b1940l,0xec881c595b013e93l,0x51574c9349532ad3l,
            0x2db1d445b37b46del },
          { 0xc6445b87df239fd8l,0xc718af75151d24eel,0xaea1c4a4f43c6259l,
            0x40c0e5d770be02f7l },
          0 },
        /* 11 << 168 */
        { { 0x6a4590f4721b33f2l,0x2124f1fbfedf04eal,0xf8e53cde9745efe7l,
            0xe7e1043265f046d9l },
          { 0xc3fca28ee4d0c7e6l,0x847e339a87253b1bl,0x9b5953483743e643l,
            0xcb6a0a0b4fd12fc5l },
          0 },
        /* 13 << 168 */
        { { 0xec1214eda714181dl,0x609ac13b6067b341l,0xff4b4c97a545df1fl,
            0xa124050134d2076bl },
          { 0x6efa0c231409ca97l,0x254cc1a820638c43l,0xd4e363afdcfb46cdl,
            0x62c2adc303942a27l },
          0 },
        /* 15 << 168 */
        { { 0x27b6a8ab3fd40e09l,0xe455842e77313ea9l,0x8b51d1e21f55988bl,
            0x5716dd73062bbbfcl },
          { 0x633c11e54e8bf3del,0x9a0e77b61b85be3bl,0x565107290911cca6l,
            0x27e76495efa6590fl },
          0 },
        /* 16 << 168 */
        { { 0xe4ac8b33070d3aabl,0x2643672b9a2cd5e5l,0x52eff79b1cfc9173l,
            0x665ca49b90a7c13fl },
          { 0x5a8dda59b3efb998l,0x8a5b922d052f1341l,0xae9ebbab3cf9a530l,
            0x35986e7bf56da4d7l },
          0 },
        /* 17 << 168 */
        { { 0x3a636b5cff3513ccl,0xbb0cf8ba3198f7ddl,0xb8d4052241f16f86l,
            0x760575d8de13a7bfl },
          { 0x36f74e169f7aa181l,0x163a3ecff509ed1cl,0x6aead61f3c40a491l,
            0x158c95fcdfe8fcaal },
          0 },
        /* 19 << 168 */
        { { 0x6b47accdd9eee96cl,0x0ca277fbe58cec37l,0x113fe413e702c42al,
            0xdd1764eec47cbe51l },
          { 0x041e7cde7b3ed739l,0x50cb74595ce9e1c0l,0x355685132925b212l,
            0x7cff95c4001b081cl },
          0 },
        /* 21 << 168 */
        { { 0x726f0973da50c991l,0x48afcd5b822d6ee2l,0xe5fc718b20fd7771l,
            0xb9e8e77dfd0807a1l },
          { 0x7f5e0f4499a7703dl,0x6972930e618e36f3l,0x2b7c77b823807bbel,
            0xe5b82405cb27ff50l },
          0 },
        /* 23 << 168 */
        { { 0x98cb1ae9255c0980l,0x4bd863812b4a739fl,0x5a5c31e11e4a45a1l,
            0x1e5d55fe9cb0db2fl },
          { 0x74661b068ff5cc29l,0x026b389f0eb8a4f4l,0x536b21a458848c24l,
            0x2e5bf8ec81dc72b0l },
          0 },
        /* 25 << 168 */
        { { 0x9f0af483d309cbe6l,0x5b020d8ae0bced4fl,0x606e986db38023e3l,
            0xad8f2c9d1abc6933l },
          { 0x19292e1de7400e93l,0xfe3e18a952be5e4dl,0xe8e9771d2e0680bfl,
            0x8c5bec98c54db063l },
          0 },
        /* 27 << 168 */
        { { 0x4c23f62a2c160dcdl,0x34e6c5e38f90eaefl,0x35865519a9a65d5al,
            0x07c48aae8fd38a3dl },
          { 0xb7e7aeda50068527l,0x2c09ef231c90936al,0x31ecfeb6e879324cl,
            0xa0871f6bfb0ec938l },
          0 },
        /* 28 << 168 */
        { { 0xb1f0fb68d84d835dl,0xc90caf39861dc1e6l,0x12e5b0467594f8d7l,
            0x26897ae265012b92l },
          { 0xbcf68a08a4d6755dl,0x403ee41c0991fbdal,0x733e343e3bbf17e8l,
            0xd2c7980d679b3d65l },
          0 },
        /* 29 << 168 */
        { { 0x33056232d2e11305l,0x966be492f3c07a6fl,0x6a8878ffbb15509dl,
            0xff2211010a9b59a4l },
          { 0x6c9f564aabe30129l,0xc6f2c940336e64cfl,0x0fe752628b0c8022l,
            0xbe0267e96ae8db87l },
          0 },
        /* 31 << 168 */
        { { 0x9d031369a5e829e5l,0xcbb4c6fc1607aa41l,0x75ac59a6241d84c1l,
            0xc043f2bf8829e0eel },
          { 0x82a38f758ea5e185l,0x8bda40b9d87cbd9fl,0x9e65e75e2d8fc601l,
            0x3d515f74a35690b3l },
          0 },
        /* 33 << 168 */
        { { 0xf6b5b2d0bc8fa5bcl,0x8a5ead67500c277bl,0x214625e6dfa08a5dl,
            0x51fdfedc959cf047l },
          { 0x6bc9430b289fca32l,0xe36ff0cf9d9bdc3fl,0x2fe187cb58ea0edel,
            0xed66af205a900b3fl },
          0 },
        /* 34 << 168 */
        { { 0x00e0968b5fa9f4d6l,0x2d4066ce37a362e7l,0xa99a9748bd07e772l,
            0x710989c006a4f1d0l },
          { 0xd5dedf35ce40cbd8l,0xab55c5f01743293dl,0x766f11448aa24e2cl,
            0x94d874f8605fbcb4l },
          0 },
        /* 35 << 168 */
        { { 0xa365f0e8a518001bl,0xee605eb69d04ef0fl,0x5a3915cdba8d4d25l,
            0x44c0e1b8b5113472l },
          { 0xcbb024e88b6740dcl,0x89087a53ee1d4f0cl,0xa88fa05c1fc4e372l,
            0x8bf395cbaf8b3af2l },
          0 },
        /* 36 << 168 */
        { { 0x1e71c9a1deb8568bl,0xa35daea080fb3d32l,0xe8b6f2662cf8fb81l,
            0x6d51afe89490696al },
          { 0x81beac6e51803a19l,0xe3d24b7f86219080l,0x727cfd9ddf6f463cl,
            0x8c6865ca72284ee8l },
          0 },
        /* 37 << 168 */
        { { 0x32c88b7db743f4efl,0x3793909be7d11dcel,0xd398f9222ff2ebe8l,
            0x2c70ca44e5e49796l },
          { 0xdf4d9929cb1131b1l,0x7826f29825888e79l,0x4d3a112cf1d8740al,
            0x00384cb6270afa8bl },
          0 },
        /* 39 << 168 */
        { { 0xbe7e990ff0d796a0l,0x5fc62478df0e8b02l,0x8aae8bf4030c00adl,
            0x3d2db93b9004ba0fl },
          { 0xe48c8a79d85d5ddcl,0xe907caa76bb07f34l,0x58db343aa39eaed5l,
            0x0ea6e007adaf5724l },
          0 },
        /* 40 << 168 */
        { { 0xe00df169d23233f3l,0x3e32279677cb637fl,0x1f897c0e1da0cf6cl,
            0xa651f5d831d6bbddl },
          { 0xdd61af191a230c76l,0xbd527272cdaa5e4al,0xca753636d0abcd7el,
            0x78bdd37c370bd8dcl },
          0 },
        /* 41 << 168 */
        { { 0xc23916c217cd93fel,0x65b97a4ddadce6e2l,0xe04ed4eb174e42f8l,
            0x1491ccaabb21480al },
          { 0x145a828023196332l,0x3c3862d7587b479al,0x9f4a88a301dcd0edl,
            0x4da2b7ef3ea12f1fl },
          0 },
        /* 43 << 168 */
        { { 0x71965cbfc3dd9b4dl,0xce23edbffc068a87l,0xb78d4725745b029bl,
            0x74610713cefdd9bdl },
          { 0x7116f75f1266bf52l,0x0204672218e49bb6l,0xdf43df9f3d6f19e3l,
            0xef1bc7d0e685cb2fl },
          0 },
        /* 44 << 168 */
        { { 0xcddb27c17078c432l,0xe1961b9cb77fedb7l,0x1edc2f5cc2290570l,
            0x2c3fefca19cbd886l },
          { 0xcf880a36c2af389al,0x96c610fdbda71ceal,0xf03977a932aa8463l,
            0x8eb7763f8586d90al },
          0 },
        /* 45 << 168 */
        { { 0x3f3424542a296e77l,0xc871868342837a35l,0x7dc710906a09c731l,
            0x54778ffb51b816dbl },
          { 0x6b33bfecaf06defdl,0xfe3c105f8592b70bl,0xf937fda461da6114l,
            0x3c13e6514c266ad7l },
          0 },
        /* 46 << 168 */
        { { 0xe363a829855938e8l,0x2eeb5d9e9de54b72l,0xbeb93b0e20ccfab9l,
            0x3dffbb5f25e61a25l },
          { 0x7f655e431acc093dl,0x0cb6cc3d3964ce61l,0x6ab283a1e5e9b460l,
            0x55d787c5a1c7e72dl },
          0 },
        /* 47 << 168 */
        { { 0x4d2efd47deadbf02l,0x11e80219ac459068l,0x810c762671f311f0l,
            0xfa17ef8d4ab6ef53l },
          { 0xaf47fd2593e43bffl,0x5cb5ff3f0be40632l,0x546871068ee61da3l,
            0x7764196eb08afd0fl },
          0 },
        /* 48 << 168 */
        { { 0x831ab3edf0290a8fl,0xcae81966cb47c387l,0xaad7dece184efb4fl,
            0xdcfc53b34749110el },
          { 0x6698f23c4cb632f9l,0xc42a1ad6b91f8067l,0xb116a81d6284180al,
            0xebedf5f8e901326fl },
          0 },
        /* 49 << 168 */
        { { 0xf2274c9f97e3e044l,0x4201852011d09fc9l,0x56a65f17d18e6e23l,
            0x2ea61e2a352b683cl },
          { 0x27d291bc575eaa94l,0x9e7bc721b8ff522dl,0x5f7268bfa7f04d6fl,
            0x5868c73faba41748l },
          0 },
        /* 51 << 168 */
        { { 0x1c52e63596e78cc4l,0x5385c8b20c06b4a8l,0xd84ddfdbb0e87d03l,
            0xc49dfb66934bafadl },
          { 0x7071e17059f70772l,0x3a073a843a1db56bl,0x034949033b8af190l,
            0x7d882de3d32920f0l },
          0 },
        /* 52 << 168 */
        { { 0x91633f0ab2cf8940l,0x72b0b1786f948f51l,0x2d28dc30782653c8l,
            0x88829849db903a05l },
          { 0xb8095d0c6a19d2bbl,0x4b9e7f0c86f782cbl,0x7af739882d907064l,
            0xd12be0fe8b32643cl },
          0 },
        /* 53 << 168 */
        { { 0x358ed23d0e165dc3l,0x3d47ce624e2378cel,0x7e2bb0b9feb8a087l,
            0x3246e8aee29e10b9l },
          { 0x459f4ec703ce2b4dl,0xe9b4ca1bbbc077cfl,0x2613b4f20e9940c1l,
            0xfc598bb9047d1eb1l },
          0 },
        /* 55 << 168 */
        { { 0x52fb0c9d7fc63668l,0x6886c9dd0c039cdel,0x602bd59955b22351l,
            0xb00cab02360c7c13l },
          { 0x8cb616bc81b69442l,0x41486700b55c3ceel,0x71093281f49ba278l,
            0xad956d9c64a50710l },
          0 },
        /* 57 << 168 */
        { { 0xbaca6591d4b66947l,0xb452ce9804460a8cl,0x6830d24643768f55l,
            0xf4197ed87dff12dfl },
          { 0x6521b472400dd0f7l,0x59f5ca8f4b1e7093l,0x6feff11b080338ael,
            0x0ada31f6a29ca3c6l },
          0 },
        /* 59 << 168 */
        { { 0x04e5dfe0d809c7bdl,0xd7b2580c8f1050abl,0x6d91ad78d8a4176fl,
            0x0af556ee4e2e897cl },
          { 0x162a8b73921de0acl,0x52ac9c227ea78400l,0xee2a4eeaefce2174l,
            0xbe61844e6d637f79l },
          0 },
        /* 60 << 168 */
        { { 0x0491f1bc789a283bl,0x72d3ac3d880836f4l,0xaa1c5ea388e5402dl,
            0x1b192421d5cc473dl },
          { 0x5c0b99989dc84cacl,0xb0a8482d9c6e75b8l,0x639961d03a191ce2l,
            0xda3bc8656d837930l },
          0 },
        /* 61 << 168 */
        { { 0xca990653056e6f8fl,0x84861c4164d133a7l,0x8b403276746abe40l,
            0xb7b4d51aebf8e303l },
          { 0x05b43211220a255dl,0xc997152c02419e6el,0x76ff47b6630c2feal,
            0x50518677281fdadel },
          0 },
        /* 63 << 168 */
        { { 0x6d2d99b7ea7b979bl,0xcd78cd74e6fb3bcdl,0x11e45a9e86cffbfel,
            0x78a61cf4637024f6l },
          { 0xd06bc8723d502295l,0xf1376854458cb288l,0xb9db26a1342f8586l,
            0xf33effcf4beee09el },
          0 },
        /* 64 << 168 */
        { { 0xd7e0c4cdb30cfb3al,0x6d09b8c16c9db4c8l,0x40ba1a4207c8d9dfl,
            0x6fd495f71c52c66dl },
          { 0xfb0e169f275264dal,0x80c2b746e57d8362l,0xedd987f749ad7222l,
            0xfdc229af4398ec7bl },
          0 },
        /* 65 << 168 */
        { { 0xfe81af4609418a51l,0xdbb60b836f18e3a5l,0x5e7a86ea4566ec9cl,
            0xb76ff40f25093925l },
          { 0x5fe6662c429c5554l,0xfc9ec35384e478cfl,0x73dbb5f3e8cfa761l,
            0x031e506592f82709l },
          0 },
        /* 71 << 168 */
        { { 0x108c736abd49f2e0l,0xe230f2417487dcc8l,0x073fc4f8f74d939cl,
            0x98532487e9745bbel },
          { 0x5208eb981714b10bl,0xec35d0510458725dl,0x35dbb60bf203f4b6l,
            0x064299b27781ab38l },
          0 },
        /* 77 << 168 */
        { { 0x43cc7bbc02d26929l,0xeb00a683162d9607l,0x2af152b8ed9fa224l,
            0xf24e8bee12257f0cl },
          { 0xdf065dd5d004b1cbl,0x6aa20bcf9f9908c6l,0x8e5e86b6941c593dl,
            0x0e0034b398969717l },
          0 },
        /* 83 << 168 */
        { { 0x5be62e155c43b8fcl,0xd9e0adfc3c445636l,0xc5141df0e0d78f48l,
            0xd134bbed2c277716l },
          { 0x79033a84598fe069l,0x6c704367b081614cl,0x55c45d66bf5bf772l,
            0xf08744c57a444730l },
          0 },
        /* 89 << 168 */
        { { 0x866752091422b528l,0xdb297411c3e028eel,0x1f5575b040e1c3ccl,
            0x85367b84d333b04fl },
          { 0x57864c86e9804aa9l,0xf13fa8e3439156dfl,0xa3b337e0464e0aecl,
            0x0018dfd7f2ae382bl },
          0 },
        /* 95 << 168 */
        { { 0xe93cece9cea132fcl,0x985542d8f74e867al,0x2a3d18a5cc8fcf87l,
            0xa0561055479d0039l },
          { 0x3513c7eaac4b3f9dl,0xc095967256477606l,0xa63960f330df8ad6l,
            0x59ca8d53cc9ddcb3l },
          0 },
        /* 101 << 168 */
        { { 0x6d8e942b2f208191l,0xd49a6d9453fe5457l,0x2b55e391003010bal,
            0x3dd1fd9fdf4605ebl },
          { 0xdc006a3358682886l,0x60a5e86c1bd9ac88l,0xc4bd320ed0cab8f2l,
            0x7281e7cb7751855bl },
          0 },
        /* 107 << 168 */
        { { 0x7d564222e1881e7al,0x59061a89db0673c2l,0x1f9d607213f27313l,
            0x5b3b29368ff3aeb7l },
          { 0x6cf2304ccf969f43l,0x8eff4a25e7f69ae5l,0xbaeb6411d17da4ffl,
            0x666af0af9eea17ecl },
          0 },
        /* 113 << 168 */
        { { 0x6c0b811697f4cd0bl,0xcd7825d40e4ea852l,0x80158fb0677fef3dl,
            0x5bb1a3aaa10ee693l },
          { 0xc5df66678066fc9bl,0x3200dc11f404d4a6l,0x58868950a8686d8el,
            0xbdaaffb53770fabal },
          0 },
        /* 116 << 168 */
        { { 0xba6a9f84660326f5l,0x61c1e44161bc3e88l,0xfbf992a0bde85cf8l,
            0xe704dd1e6f8c8f5fl },
          { 0x231caa0ab1d7d486l,0xd10616d8891cd571l,0x2ddada75c008833cl,
            0x44337d6dad514c94l },
          0 },
        /* 119 << 168 */
        { { 0xd48678b8f6933cf0l,0x7b4d623e0b739471l,0x4ad620287b216238l,
            0xb4d4918959c4fabel },
          { 0x8c2a1bdc296d42d5l,0x9235d0ec2fd3eb96l,0xfe271972f81c135bl,
            0x82b5181741471e16l },
          0 },
        /* 125 << 168 */
        { { 0xe9aa8ce4051f8e81l,0x14484af67cd1391fl,0x53a361dcafb1656el,
            0x6ad8ba02f4d9d0cbl },
          { 0xfb4385466c50a722l,0x2f1c5bbc7edb37f4l,0x8dc90ccb16e4b795l,
            0xbcb32e1508127094l },
          0 },
    },
    {
        /* 0 << 176 */
        { { 0x00, 0x00, 0x00, 0x00 },
          { 0x00, 0x00, 0x00, 0x00 },
          1 },
        /* 1 << 176 */
        { { 0xb81d783e979f3925l,0x1efd130aaf4c89a7l,0x525c2144fd1bf7fal,
            0x4b2969041b265a9el },
          { 0xed8e9634b9db65b6l,0x35c82e3203599d8al,0xdaa7a54f403563f3l,
            0x9df088ad022c38abl },
          0 },
        /* 3 << 176 */
        { { 0x9e93ba24f111661el,0xedced484b105eb04l,0x96dc9ba1f424b578l,
            0xbf8f66b7e83e9069l },
          { 0x872d4df4d7ed8216l,0xbf07f3778e2cbecfl,0x4281d89998e73754l,
            0xfec85fbb8aab8708l },
          0 },
        /* 4 << 176 */
        { { 0x13b5bf22765fa7d0l,0x59805bf01d6a5370l,0x67a5e29d4280db98l,
            0x4f53916f776b1ce3l },
          { 0x714ff61f33ddf626l,0x4206238ea085d103l,0x1c50d4b7e5809ee3l,
            0x999f450d85f8eb1dl },
          0 },
        /* 5 << 176 */
        { { 0x82eebe731a3a93bcl,0x42bbf465a21adc1al,0xc10b6fa4ef030efdl,
            0x247aa4c787b097bbl },
          { 0x8b8dc632f60c77dal,0x6ffbc26ac223523el,0xa4f6ff11344579cfl,
            0x5825653c980250f6l },
          0 },
        /* 7 << 176 */
        { { 0xeda6c595d314e7bcl,0x2ee7464b467899edl,0x1cef423c0a1ed5d3l,
            0x217e76ea69cc7613l },
          { 0x27ccce1fe7cda917l,0x12d8016b8a893f16l,0xbcd6de849fc74f6bl,
            0xfa5817e2f3144e61l },
          0 },
        /* 9 << 176 */
        { { 0xc0b48d4e49ccd6d7l,0xff8fb02c88bd5580l,0xc75235e907d473b2l,
            0x4fab1ac5a2188af3l },
          { 0x030fa3bc97576ec0l,0xe8c946e80b7e7d2fl,0x40a5c9cc70305600l,
            0x6d8260a9c8b013b4l },
          0 },
        /* 10 << 176 */
        { { 0xe6c51073615cd9e4l,0x498ec047f1243c06l,0x3e5a8809b17b3d8cl,
            0x5cd99e610cc565f1l },
          { 0x81e312df7851dafel,0xf156f5baa79061e2l,0x80d62b71880c590el,
            0xbec9746f0a39faa1l },
          0 },
        /* 11 << 176 */
        { { 0x2b09d2c3cfdcf7ddl,0x41a9fce3723fcab4l,0x73d905f707f57ca3l,
            0x080f9fb1ac8e1555l },
          { 0x7c088e849ba7a531l,0x07d35586ed9a147fl,0x602846abaf48c336l,
            0x7320fd320ccf0e79l },
          0 },
        /* 13 << 176 */
        { { 0x92eb40907f8f875dl,0x9c9d754e56c26bbfl,0x158cea618110bbe7l,
            0x62a6b802745f91eal },
          { 0xa79c41aac6e7394bl,0x445b6a83ad57ef10l,0x0c5277eb6ea6f40cl,
            0x319fe96b88633365l },
          0 },
        /* 15 << 176 */
        { { 0x77f84203d39b8c34l,0xed8b1be63125eddbl,0x5bbf2441f6e39dc5l,
            0xb00f6ee66a5d678al },
          { 0xba456ecf57d0ea99l,0xdcae0f5817e06c43l,0x01643de40f5b4baal,
            0x2c324341d161b9bel },
          0 },
        /* 16 << 176 */
        { { 0x949c9976e1337c26l,0x6faadebdd73d68e5l,0x9e158614f1b768d9l,
            0x22dfa5579cc4f069l },
          { 0xccd6da17be93c6d6l,0x24866c61a504f5b9l,0x2121353c8d694da1l,
            0x1c6ca5800140b8c6l },
          0 },
        /* 17 << 176 */
        { { 0x4e77c5575b45afb4l,0xe9ded649efb8912dl,0x7ec9bbf542f6e557l,
            0x2570dfff62671f00l },
          { 0x2b3bfb7888e084bdl,0xa024b238f37fe5b4l,0x44e7dc0495649aeel,
            0x498ca2555e7ec1d8l },
          0 },
        /* 19 << 176 */
        { { 0x2e44d22526a1fc90l,0x0d6d10d24d70705dl,0xd94b6b10d70c45f4l,
            0x0f201022b216c079l },
          { 0xcec966c5658fde41l,0xa8d2bc7d7e27601dl,0xbfcce3e1ff230be7l,
            0x3394ff6b0033ffb5l },
          0 },
        /* 21 << 176 */
        { { 0x05d99be8b9c20cdal,0x89f7aad5d5cd0c98l,0x7ef936fe5bb94183l,
            0x92ca0753b05cd7f2l },
          { 0x9d65db1174a1e035l,0x02628cc813eaea92l,0xf2d9e24249e4fbf2l,
            0x94fdfd9be384f8b7l },
          0 },
        /* 23 << 176 */
        { { 0x29882d7c98379d44l,0xd000bdfb509edc8al,0xc6f95979e66fe464l,
            0x504a6115fa61bde0l },
          { 0x56b3b871effea31al,0x2d3de26df0c21a54l,0x21dbff31834753bfl,
            0xe67ecf4969269d86l },
          0 },
        /* 25 << 176 */
        { { 0xed29a56da16d4b34l,0x7fba9d09dca21c4fl,0x66d7ac006d8de486l,
            0x6006198773a2a5e1l },
          { 0x8b400f869da28ff0l,0x3133f70843c4599cl,0x9911c9b8ee28cb0dl,
            0xcd7e28748e0af61dl },
          0 },
        /* 27 << 176 */
        { { 0x6a7bb6a93b5bdb83l,0x08da65c0a4a72318l,0xc58d22aa63eb065fl,
            0x1717596c1b15d685l },
          { 0x112df0d0b266d88bl,0xf688ae975941945al,0x487386e37c292cacl,
            0x42f3b50d57d6985cl },
          0 },
        /* 28 << 176 */
        { { 0x69e3be0427596893l,0xb6bb02a645bf452bl,0x0875c11af4c698c8l,
            0x6652b5c7bece3794l },
          { 0x7b3755fd4f5c0499l,0x6ea16558b5532b38l,0xd1c69889a2e96ef7l,
            0x9c773c3a61ed8f48l },
          0 },
        /* 29 << 176 */
        { { 0x5a304ada8545d185l,0x82ae44ea738bb8cbl,0x628a35e3df87e10el,
            0xd3624f3da15b9fe3l },
          { 0xcc44209b14be4254l,0x7d0efcbcbdbc2ea5l,0x1f60336204c37bbel,
            0x21f363f556a5852cl },
          0 },
        /* 31 << 176 */
        { { 0x81262e4225346689l,0x716da290b07c7004l,0x35f911eab7950ee3l,
            0x6fd72969261d21b5l },
          { 0x5238980308b640d3l,0x5b0026ee887f12a1l,0x20e21660742e9311l,
            0x0ef6d5415ff77ff7l },
          0 },
        /* 33 << 176 */
        { { 0x64aa0874925dd0b0l,0x5ffd503851c474c6l,0x4478c72c8ebd4157l,
            0xb98694cb8c8375e2l },
          { 0xeda4edeecd8e208cl,0xf98a053d2c0670a6l,0x564bd3057f346b9dl,
            0xafbbf3e94c318fddl },
          0 },
        /* 34 << 176 */
        { { 0x8a03410aa96c4685l,0xef1b6b16a978a31bl,0x44738a3b629df6cfl,
            0xa1dc65da807713e9l },
          { 0x569cc7884c373442l,0x1f30a2464965fb52l,0x56822f1677ff5e2el,
            0x63f18812e303748bl },
          0 },
        /* 35 << 176 */
        { { 0x2abdc403dd0983ecl,0xec0c08c7f365c6f5l,0xe555083fbdb66b8bl,
            0x593685bc4e8973ffl },
          { 0x737df3f920e9c705l,0x00c7bcc309c31a5al,0x5f1d23e2efdcb34dl,
            0x79d9b382470f7949l },
          0 },
        /* 36 << 176 */
        { { 0x44a315645fd2eb1dl,0x4e7397263fdd1356l,0x9b96735463200efel,
            0xcb70402e520bbb6al },
          { 0xcbc90d7e693d2642l,0x6fb00064bc9b4002l,0x95f2eab3d96f7150l,
            0xb1619e3fe035f47al },
          0 },
        /* 37 << 176 */
        { { 0xd22d6073d1561bb7l,0x40666e4ba9928683l,0x90654dab8ab3f9b1l,
            0x7625c507b8773421l },
          { 0x288f28220ca88cd2l,0xbb88114ed8d005c1l,0xbeec2b0af603a11bl,
            0x8fdda60325f7949el },
          0 },
        /* 39 << 176 */
        { { 0x6503632d6ee4f1d0l,0xd5449747ea394840l,0xd696167a8abe13a1l,
            0xc080f76e609ebaa9l },
          { 0x181acf0c10aa70d6l,0x70614461291e5e50l,0x7ade8e84b9f0c0a3l,
            0xef1de9f2cb11b41el },
          0 },
        /* 40 << 176 */
        { { 0x2d5c3c848e592413l,0x727022961832ba2cl,0x22979b51596c6321l,
            0x738f31cb5a04db64l },
          { 0x0bdaa6ca98f84ee5l,0x4e9e827c15e21eeel,0x4c59dbcc3ea632e0l,
            0xed3404db5bc6f027l },
          0 },
        /* 41 << 176 */
        { { 0x2841f05cfbaf8b26l,0xac9830db5b243770l,0xde3ab1707787f324l,
            0x1ee12efe079209bcl },
          { 0x2d3fd62d5bcf6e3cl,0x8a680655d60b0582l,0xdafc5061bc2b64a1l,
            0xe0d91e7526a88788l },
          0 },
        /* 43 << 176 */
        { { 0x2d49c685426b1b1el,0x6c2149caeabb02f7l,0xa4697d7fde11984fl,
            0xa0e32fb3ed3c8707l },
          { 0xb783e825f4ca12dal,0xb2666e2448770a50l,0x82d47f478660e923l,
            0x6e36cd71fb4a984fl },
          0 },
        /* 44 << 176 */
        { { 0x3295a8ea43c66b92l,0x99387af6ac5d19d4l,0x545f9b1b8e9d2090l,
            0x138b1c4c2660f530l },
          { 0xbfb05fd2ff872627l,0xb6614b0f4c3bc45cl,0x13defece62ca0fb0l,
            0x82ddae134fededd8l },
          0 },
        /* 45 << 176 */
        { { 0x5a34499b871c4cbbl,0x3ab0e69a2eb6084bl,0xa8d0160025ef7755l,
            0x5db8f611d9e70f5dl },
          { 0x63f9eb9a7afa95d7l,0x328b97f9706d7964l,0x8bcf9a0f4b71dfcal,
            0x53d4c3042a5c7934l },
          0 },
        /* 46 << 176 */
        { { 0x0c87dd3a8768d9aal,0x201ce5a082f6a55fl,0xa3de6f3049ca4602l,
            0x36f421422aeb5f17l },
          { 0x5c9962399817b77al,0x2584a10ae8d165acl,0x80f683d0c726f4aal,
            0x524307502dcdfa48l },
          0 },
        /* 47 << 176 */
        { { 0x0c04399f94683df2l,0x0978e9d4e954838dl,0x01faa5e8cf4a7a7bl,
            0x92f6e6a90dae61cfl },
          { 0x0c0f1293373dc957l,0x8320178fd8cc6b67l,0x4af977ed4b6444f2l,
            0xd8c9a401ad8e5f84l },
          0 },
        /* 48 << 176 */
        { { 0xbd5660ed9aed9f40l,0x70ca6ad1532a8c99l,0xc4978bfb95c371eal,
            0xe5464d0d7003109dl },
          { 0x1af32fdfd9e535efl,0xabf57ea798c9185bl,0xed7a741712b42488l,
            0x8e0296a7e97286fal },
          0 },
        /* 49 << 176 */
        { { 0x79ee35ac16fca804l,0x8f16e6165f59782el,0x8fbef1011737694el,
            0xb34b7625462be08bl },
          { 0x7e63e1b016e75c91l,0xb6a18edd2d23728dl,0xcf761a1e7f299ab6l,
            0x796dcdebf16c770el },
          0 },
        /* 51 << 176 */
        { { 0x47354f22308ee4afl,0x96959a538ecd6f4bl,0xf60b5f104055cbd2l,
            0x04b1c9599bd86095l },
          { 0x26accd8486008564l,0x46b2fe0478f31ea7l,0x5500dbf72dd76f23l,
            0x36bcdf584c496c6fl },
          0 },
        /* 52 << 176 */
        { { 0x8836cd431527d7cel,0x1f236623187a50eal,0x6470c0ae847221f0l,
            0xc61f86b47e449110l },
          { 0x7cc9cc20fa9fcec1l,0xa394903019134349l,0xafe5a08ff53ab467l,
            0x9caba02301ed2919l },
          0 },
        /* 53 << 176 */
        { { 0xffecbdce406abf1el,0x0ef4bcd73ae340d4l,0x7e37bae0e19d5613l,
            0xe191669be4c6e97al },
          { 0x9fafe59797292db7l,0xab7ef3713172d716l,0x9f0fff330ce3b533l,
            0xca94ff8f932dd8cfl },
          0 },
        /* 55 << 176 */
        { { 0x659c8b5d78aea69el,0xdde7ab46476a8fb9l,0x26bfe303bd01b5e6l,
            0xf3dfb08a726a937cl },
          { 0xe7a591fa0a263670l,0xe872c3f8f97434a0l,0x4881a82e2e0f2c21l,
            0x17624e48788ef958l },
          0 },
        /* 57 << 176 */
        { { 0xd526d66da7222e5bl,0xd33bb78efeb00e25l,0x9a7d670b932c8d08l,
            0xea31e5273cee093fl },
          { 0x55cc091bd04b7a43l,0x12b08d6dd01a123dl,0x1d98a6467fb0e7bal,
            0xdabb09483535fd0dl },
          0 },
        /* 59 << 176 */
        { { 0x2862314d08b69b19l,0x9cf302e191effcfal,0x43bdc8462ead917al,
            0x21b238bbf94b3d8fl },
          { 0xa3736160e2f465d3l,0x4d7fb6818541e255l,0x46fa089a23551edcl,
            0xf7c41d17c1fefa8cl },
          0 },
        /* 60 << 176 */
        { { 0x8ed0807fed113000l,0x8e1672d04c691484l,0x33a13ab31ee86ca0l,
            0x9df0d9573bcaee4fl },
          { 0x0cf0c638ef0dfb71l,0x1e0fe22ac2c9510al,0x43f506716fcc6a21l,
            0xccb58404cec03a94l },
          0 },
        /* 61 << 176 */
        { { 0x59547e37fd0936c1l,0x81e0517df45140b1l,0xcc6ccd89ed49e3fcl,
            0xc2fa23eff3b897del },
          { 0x149511ef2050c80al,0xf66bea6b3140b833l,0xbbe1401e2786d723l,
            0x0aeb549c887509bcl },
          0 },
        /* 63 << 176 */
        { { 0xf938e85060f5867al,0x806e1fff72429adcl,0x5ff7962a45f43b52l,
            0xd8375ab6b2bbb403l },
          { 0x00d5819b21b287fcl,0x15c7190ebae37d58l,0x075ce5ce05fcfb07l,
            0x76368d06dbc003cbl },
          0 },
        /* 64 << 176 */
        { { 0x01079383171b445fl,0x9bcf21e38131ad4cl,0x8cdfe205c93987e8l,
            0xe63f4152c92e8c8fl },
          { 0x729462a930add43dl,0x62ebb143c980f05al,0x4f3954e53b06e968l,
            0xfe1d75ad242cf6b1l },
          0 },
        /* 65 << 176 */
        { { 0x1cf508197630655el,0x9b4685c408d417f5l,0x6ea942619b049259l,
            0x31c29b54fe73b755l },
          { 0x3d2872a1f1f2af17l,0xbcd1139956bcbc4bl,0x4d14f59890d7a85cl,
            0xd2c46040dbcbe998l },
          0 },
        /* 71 << 176 */
        { { 0x3c8a06ca9792c42al,0x92535628602460ddl,0xa95e13f2ddd4c676l,
            0xe823841d3b20d463l },
          { 0x0248605bbfad6051l,0x82985dd61af51233l,0x3d243a5cdef7d742l,
            0x0a88ce55ff6aa911l },
          0 },
        /* 77 << 176 */
        { { 0xcf5b5962449aec98l,0x40322a6531a41389l,0xcd15606fd72c0527l,
            0xfe91eac7b90d65a0l },
          { 0xcd32415487636360l,0x82f2c7bdfc653a6fl,0xd04d138ae315ce7cl,
            0x40ebfd5e78118dbcl },
          0 },
        /* 83 << 176 */
        { { 0x0f9ea6ae4144660fl,0x02345c6513279b25l,0x139497b65c7671cbl,
            0x7259f14b2ebed1d5l },
          { 0xa1e5d98ce9b29988l,0xaed0efcd8df73ac8l,0x88339f073b81a77cl,
            0x28f2bbca7109c8a6l },
          0 },
        /* 89 << 176 */
        { { 0xa264f99d811472ddl,0x0e7eae0afc07a80cl,0x77f264d4a683cdc6l,
            0x0512df49d053c668l },
          { 0x2b4dfbade61dea15l,0x83de61acfd74890al,0xd2552bab32d41182l,
            0x1fb9411435924e6al },
          0 },
        /* 95 << 176 */
        { { 0x85efe53ade23c988l,0x89d41dbbf897f91bl,0x1357f91e7873fa8dl,
            0x7a6ec2e3718d911cl },
          { 0xf9e4f92e8f209a01l,0x4ffb96a70fdd67f3l,0x4c81a787f83dde1cl,
            0x0d68fce15e163b60l },
          0 },
        /* 101 << 176 */
        { { 0xbc79b4b26ab6da9dl,0xb4be5c278bb005f1l,0x63624530cd3b280bl,
            0x543142f04e880026l },
          { 0xbf7fb14cad90ddbfl,0xfe456e8a3966732dl,0x85499fb987ce35e9l,
            0x8af09e6b24f1305dl },
          0 },
        /* 107 << 176 */
        { { 0x5fc563ec16dc2b4bl,0xfe5631b25d0e535fl,0xbf4c489f9a93e36cl,
            0x56badff1da2a07c4l },
          { 0x72ac6b77fb7c5595l,0x4b25b9428e6645d9l,0xeeae127251f0657el,
            0x30779ca51abeb76bl },
          0 },
        /* 113 << 176 */
        { { 0x3d602ef5d909f43dl,0x2b2951a6bb347c79l,0x44903bfaa0d88896l,
            0xd4ab20e8684c104fl },
          { 0x55f70b4dd9b7e626l,0x084b3ee646a5f9ecl,0x1799cbe3da4ae81al,
            0xc7cfac937fd6b80fl },
          0 },
        /* 116 << 176 */
        { { 0x45647911ca20c525l,0x78f83186004706abl,0x5596377d97510538l,
            0x047863defe041f8cl },
          { 0xaea784896ec82367l,0x9d4eac2601eee8fcl,0xb32728f19b57d9dbl,
            0x60a158f5313c0f65l },
          0 },
        /* 119 << 176 */
        { { 0xf78caf129754377bl,0xa7fce16b6966f0c4l,0xfea937555a54a2b7l,
            0x52d7f79b7cdfe951l },
          { 0x3e14b92e94b1dac0l,0x363f2e5af168b73bl,0xcc0e9dcb6436a8c2l,
            0x2dbece4bb52cbd27l },
          0 },
        /* 125 << 176 */
        { { 0x7e7907ed8df38ffel,0xa68ec827e24e8a24l,0x5093a97e5f168732l,
            0xa9ffea2f39ebb6dbl },
          { 0x89e02c12284276d4l,0xc1179e3b3f9502d6l,0x01becb51d8f69eb6l,
            0x86eee2935eb1c73cl },
          0 },
    },
    {
        /* 0 << 184 */
        { { 0x00, 0x00, 0x00, 0x00 },
          { 0x00, 0x00, 0x00, 0x00 },
          1 },
        /* 1 << 184 */
        { { 0xf3b7963f4c830320l,0x842c7aa0903203e3l,0xaf22ca0ae7327afbl,
            0x38e13092967609b6l },
          { 0x73b8fb62757558f1l,0x3cc3e831f7eca8c1l,0xe4174474f6331627l,
            0xa77989cac3c40234l },
          0 },
        /* 3 << 184 */
        { { 0xb32cb8b0b796d219l,0xc3e95f4f34741dd9l,0x8721212568edf6f5l,
            0x7a03aee4a2b9cb8el },
          { 0x0cd3c376f53a89aal,0x0d8af9b1948a28dcl,0xcf86a3f4902ab04fl,
            0x8aacb62a7f42002dl },
          0 },
        /* 4 << 184 */
        { { 0xfd8e139f8f5fcda8l,0xf3e558c4bdee5bfdl,0xd76cbaf4e33f9f77l,
            0x3a4c97a471771969l },
          { 0xda27e84bf6dce6a7l,0xff373d9613e6c2d1l,0xf115193cd759a6e9l,
            0x3f9b702563d2262cl },
          0 },
        /* 5 << 184 */
        { { 0x9cb0ae6c252bd479l,0x05e0f88a12b5848fl,0x78f6d2b2a5c97663l,
            0x6f6e149bc162225cl },
          { 0xe602235cde601a89l,0xd17bbe98f373be1fl,0xcaf49a5ba8471827l,
            0x7e1a0a8518aaa116l },
          0 },
        /* 7 << 184 */
        { { 0x8b1e572235e6fc06l,0x3477728f0b3e13d5l,0x150c294daa8a7372l,
            0xc0291d433bfa528al },
          { 0xc6c8bc67cec5a196l,0xdeeb31e45c2e8a7cl,0xba93e244fb6e1c51l,
            0xb9f8b71b2e28e156l },
          0 },
        /* 9 << 184 */
        { { 0x343ac0a3ee9523f0l,0xbb75eab2975ea978l,0x1bccf332107387f4l,
            0x790f92599ab0062el },
          { 0xf1a363ad1e4f6a5fl,0x06e08b8462519a50l,0x609151877265f1eel,
            0x6a80ca3493ae985el },
          0 },
        /* 10 << 184 */
        { { 0xa3f4f521e447f2c4l,0x81b8da7a604291f0l,0xd680bc467d5926del,
            0x84f21fd534a1202fl },
          { 0x1d1e31814e9df3d8l,0x1ca4861a39ab8d34l,0x809ddeec5b19aa4al,
            0x59f72f7e4d329366l },
          0 },
        /* 11 << 184 */
        { { 0x2dfb9e08be0f4492l,0x3ff0da03e9d5e517l,0x03dbe9a1f79466a8l,
            0x0b87bcd015ea9932l },
          { 0xeb64fc83ab1f58abl,0x6d9598da817edc8al,0x699cff661d3b67e5l,
            0x645c0f2992635853l },
          0 },
        /* 13 << 184 */
        { { 0xd50e57c7d7fe71f3l,0x15342190bc97ce38l,0x51bda2de4df07b63l,
            0xba12aeae200eb87dl },
          { 0xabe135d2a9b4f8f6l,0x04619d65fad6d99cl,0x4a6683a77994937cl,
            0x7a778c8b6f94f09al },
          0 },
        /* 15 << 184 */
        { { 0x8dd1fb83425c6559l,0x7fc00ee60af06fdal,0xe98c922533d956dfl,
            0x0f1ef3354fbdc8a2l },
          { 0x2abb5145b79b8ea2l,0x40fd2945bdbff288l,0x6a814ac4d7185db7l,
            0xc4329d6fc084609al },
          0 },
        /* 16 << 184 */
        { { 0x511053e453544774l,0x834d0ecc3adba2bcl,0x4215d7f7bae371f5l,
            0xfcfd57bf6c8663bcl },
          { 0xded2383dd6901b1dl,0x3b49fbb4b5587dc3l,0xfd44a08d07625f62l,
            0x3ee4d65b9de9b762l },
          0 },
        /* 17 << 184 */
        { { 0x55ef9d3dcc26e8b0l,0xf869c827729b707al,0xdbbf450d8c47e00cl,
            0x73d546ea60972ed7l },
          { 0x9563e11f0dcd6821l,0xe48e1af57d80de7fl,0xbe7139b49057838dl,
            0xf3f0ad4d7e5ca535l },
          0 },
        /* 19 << 184 */
        { { 0xac66d1d49f8f8cc2l,0x43fe5c154ef18941l,0xbae77b6ddc30fcbfl,
            0xdb95ea7d945723b7l },
          { 0x43298e2bda8097e2l,0x8004167baf22ea9bl,0x9cf5974196a83d57l,
            0xb35c9aba3cf67d5el },
          0 },
        /* 21 << 184 */
        { { 0x0569a48df766f793l,0x6b4c7b16706b3442l,0xcc97754416ff41e0l,
            0x800c56e31fee2e86l },
          { 0xce0c3d0fcdf93450l,0x6ec3703582f35916l,0x902520d5bbc11e68l,
            0x7e2b988505078223l },
          0 },
        /* 23 << 184 */
        { { 0xb30d1769101da00bl,0xb26872d5113cfdb6l,0x7b0491da44e48db5l,
            0x810e73bb2013f8c9l },
          { 0xc86e579a570f0b59l,0xf34107e37a918f34l,0x49286d00277473f1l,
            0x74423f5abc85905dl },
          0 },
        /* 25 << 184 */
        { { 0x90d7417879de6b48l,0xe762caf0d14fa75bl,0xa309dcf3bd91ec5dl,
            0x7aafe1ddf526d04fl },
          { 0x76911342d39e36ffl,0xe28994d2fabb34b8l,0xac23a92c863110cbl,
            0x9f0f69673aabd166l },
          0 },
        /* 27 << 184 */
        { { 0x7436bdf47e333f98l,0x879cf31f2455af64l,0x07933a9cf6cfde92l,
            0xfcac38a5b6e3203fl },
          { 0xa39b6a8098e5a6e0l,0x1d600b5da4837528l,0x54718de7c32d412bl,
            0x02870f46317937ccl },
          0 },
        /* 28 << 184 */
        { { 0x1f13756db1761ec8l,0xe53c8b98a4b97e55l,0xb2aee3f84096cc28l,
            0x48c361a0920f1a8dl },
          { 0xa98b672d8c31190al,0x7bc1e7d1001855d4l,0x242cfb07bf3f4b2al,
            0x9bf44a3f32a28bc4l },
          0 },
        /* 29 << 184 */
        { { 0x96d4b271e36eeccdl,0x2d8c01b859237e23l,0x24f7a6eb8adf2653l,
            0xc08ac4ab41183d80l },
          { 0xc35e5bb7036367c3l,0xd8c97cbc0ba59f61l,0x296b1f4c5aafe986l,
            0xa519c7a17d179c37l },
          0 },
        /* 31 << 184 */
        { { 0x4043490790ae5f49l,0x8ac8f73649556b81l,0xb57a89b0f4e77a16l,
            0xe1a1565d071020eal },
          { 0x4a27f34d3dda8450l,0x65af18b9bc395814l,0xaf21939f9ff49991l,
            0x47e00639b4af7691l },
          0 },
        /* 33 << 184 */
        { { 0x4b3e263246b1f9b2l,0x6457d838efde99d3l,0x77d5142325e56171l,
            0xb45de3df7d54996cl },
          { 0x1ee2dd3194098d98l,0x986896141f3ebdc5l,0x2704a107997efb47l,
            0x96b502eecb11e520l },
          0 },
        /* 34 << 184 */
        { { 0x58c8039ec19f866el,0xc84c053e386c2644l,0xb3708ab049435704l,
            0x1b70c3c86fc47b24l },
          { 0x235582a27f095649l,0x0d344b66673c9a9el,0x777c9e71e2b00efdl,
            0x91691d6e5b877856l },
          0 },
        /* 35 << 184 */
        { { 0x11c663c49cd31e22l,0x46ae0bd95fb943d7l,0x6e36bca6a392fc01l,
            0x4f8cc3a77948716fl },
          { 0x10ae9d6b3aa4bbb0l,0xcc9b6cb5d8001a86l,0x012c8e3aa0a4ceedl,
            0xe462971e52274942l },
          0 },
        /* 36 << 184 */
        { { 0x9982e2ac42e176a5l,0x324eba46e2782b64l,0x3d8caaafe18350f5l,
            0xf3d82af2f5d674cal },
          { 0xc2090fed56600d1el,0x4548e0ef5950de07l,0xb2f0023f765a4febl,
            0xb303103339f16790l },
          0 },
        /* 37 << 184 */
        { { 0xb94095dc7bdacf7al,0x0e73db39509b310al,0x76e99a6b41b5f772l,
            0xef40e9c596f3dbd7l },
          { 0xd0d644f980f2179el,0xe0db831d5a89807el,0xa0188493c2a2d6c6l,
            0xf2d9a85e5ba9faa9l },
          0 },
        /* 39 << 184 */
        { { 0x598b7876cdd95b93l,0x5f7cc827336966e8l,0x01887109e797f102l,
            0x665671c446c7c296l },
          { 0xb314793c6e019c72l,0x5a6c81580e0329acl,0x4faf2f1b44281b98l,
            0x825884072e1fc97el },
          0 },
        /* 40 << 184 */
        { { 0xa692781d61a3c8b3l,0x08bc385432876d0el,0xbecf05fb28027b03l,
            0x636c687da4b1e12fl },
          { 0x00e3003d07217c58l,0x613ba9375e01b2a3l,0xa58c8405881de16el,
            0xc653c43014f8f48bl },
          0 },
        /* 41 << 184 */
        { { 0x68e53c7c89c0c7c2l,0xf2e680b23c423272l,0xacd47fae60f50133l,
            0x4c484c6534f05605l },
          { 0x663bdcf9ebffbb7dl,0xb49cff3be42421c6l,0x0549f7b13f53f261l,
            0xc516aeda7c374766l },
          0 },
        /* 43 << 184 */
        { { 0xa515fe0f76a0ec26l,0xf727c0797b0b8b21l,0xaeed4c671993651el,
            0x1465a7f828ac7c87l },
          { 0x776bd5131f0ef90bl,0x57515d2cd9773e61l,0x235455e95564c50bl,
            0xf44daef80bf06a24l },
          0 },
        /* 44 << 184 */
        { { 0xbc1c6897d6a0d0f9l,0xd8e0ea0e3b0d7f55l,0xb35baa92b85b7aadl,
            0x2becd1b7674e48f4l },
          { 0xe2d7f78d6d7a9ac2l,0xf5074262f99c95d0l,0x4852470a89f611e9l,
            0xf7aa911992869decl },
          0 },
        /* 45 << 184 */
        { { 0x0bd1755b0ac4840fl,0x0f4c6c2aa22eef10l,0x3f72fe2d78d16dd9l,
            0xb2d49200ff7096a4l },
          { 0xa5dead555ffca031l,0x1d013c320b65f4cfl,0x67e498582a23f441l,
            0x55bae166d02412c0l },
          0 },
        /* 46 << 184 */
        { { 0x546dd4545739a62al,0x353dc1422a30b836l,0x1462449d99cbd704l,
            0xda02d0772da69411l },
          { 0xcb115fe565b1a1adl,0x395235f501230a22l,0x8ae630eed164d970l,
            0x60b679f0074e3a7el },
          0 },
        /* 47 << 184 */
        { { 0x2e64695245d231e1l,0xc96663ac00d8a0fbl,0xc1fbaa0cd07e1f41l,
            0x4b31484488758781l },
          { 0xd6971a835183e72el,0xd1d01f174cbe99b7l,0xe90b438c5a2f7512l,
            0xf858fa452957c620l },
          0 },
        /* 48 << 184 */
        { { 0xed7f2e774e6daae2l,0x7b3ae0e39e0a19bcl,0xd3293f8a91ae677el,
            0xd363b0cb45c8611fl },
          { 0xbe1d1ccf309ae93bl,0xa3f80be73920cae1l,0xaaacba74498edf01l,
            0x1e6d2a4ab2f5ac90l },
          0 },
        /* 49 << 184 */
        { { 0xb5c5bb67b972a778l,0xc2423a4a190f9b5al,0x4e693cf365247948l,
            0xc37d129ea94a65a3l },
          { 0xbea4736b6e9cd47bl,0xf3d1bd212338f524l,0xa2a0278e067a45dal,
            0xc86d631b5b5dce9bl },
          0 },
        /* 51 << 184 */
        { { 0xc2d75f46116952cel,0xd2b66269b75e40dal,0x024f670f921c4111l,
            0x37ffd854c91fd490l },
          { 0x6be44d0385b2f613l,0x040cd7d9ba11c4f9l,0x04c1cb762c0efb1fl,
            0xd905ff4f505e4698l },
          0 },
        /* 52 << 184 */
        { { 0x60c5f03f233550f1l,0xd4d09411925afd2el,0xa95b65c3d258e5a6l,
            0x1a19cfb59f902c6al },
          { 0xb486013af5ad5c68l,0xa2506776979638f3l,0x1232b4d0a38e0b28l,
            0xa64784b8d36a7b4fl },
          0 },
        /* 53 << 184 */
        { { 0x22c75830a13dcb47l,0xd6e81258efd7a08fl,0x6db703b6e4fc49b8l,
            0x8a5ac636f01817e9l },
          { 0x8d27b6e1b3f24514l,0x40edc3bc708c51d7l,0x9a1eec7765bb086dl,
            0x812ccb42b10800f8l },
          0 },
        /* 55 << 184 */
        { { 0x1a39c6acd4338453l,0x3d93822954b1295dl,0x7bf0bf45e0d81165l,
            0x83d58ca5972804d2l },
          { 0x105d3ddb00524b94l,0x65d516e7920378ecl,0x1d28f5f1aea33926l,
            0xa0b354313901c906l },
          0 },
        /* 57 << 184 */
        { { 0x000442a1e4f354del,0x165b44d9d1d112f5l,0x67fd9ced0d05c0a9l,
            0xd6ce074360bd5d60l },
          { 0x9ac80c931522af2al,0x8232d522fa07d449l,0x287b5534c3fdb652l,
            0x9f0548b3abd2ab98l },
          0 },
        /* 59 << 184 */
        { { 0xde8d7086b9aea1d4l,0x692180d98a7dc3fcl,0xd64ffb53bad3e6f3l,
            0x84628acf36ce3f91l },
          { 0xf76e470b6d498ac5l,0xa16945547abad602l,0x5b8fd6a5a255c1f6l,
            0xffe24e4a8576ae2al },
          0 },
        /* 60 << 184 */
        { { 0x5655179de7d70e03l,0x3e780c5c72a84570l,0xc102b4cb1d50029cl,
            0x3e71bdd5f075e839l },
          { 0x6460f4f0b498b822l,0x2682e06c6d4b8da5l,0x4eae53c996a740d4l,
            0xc19d8bef6389702cl },
          0 },
        /* 61 << 184 */
        { { 0x711be2081025fe1dl,0x2e562c89f0bc6a99l,0xcfd2be3a28bf4150l,
            0x33037b4a38e5bc91l },
          { 0x10c6da9df52fea02l,0x511f62444f0ea410l,0x19d37ca81a294c3fl,
            0x7e40f444618e6fd3l },
          0 },
        /* 63 << 184 */
        { { 0x4095f5ddbedb8734l,0x9c16027c4432f51al,0xced8179d873d0f11l,
            0x70c2bc9f6ebe6e61l },
          { 0x5c31035d616cf2f4l,0xf92e0fbd00a4af3dl,0xe6048a03511893c4l,
            0x639a804b52e2f462l },
          0 },
        /* 64 << 184 */
        { { 0x8735728dc2c6ff70l,0x79d6122fc5dc2235l,0x23f5d00319e277f9l,
            0x7ee84e25dded8cc7l },
          { 0x91a8afb063cd880al,0x3f3ea7c63574af60l,0x0cfcdc8402de7f42l,
            0x62d0792fb31aa152l },
          0 },
        /* 65 << 184 */
        { { 0x0f4bcefd9da373e4l,0x7278f44d119271a3l,0xb2dff94449e111c0l,
            0xb0a3abf8e5d2b2d4l },
          { 0x01baabb48ea80631l,0x27517ed3da305f85l,0x0a1ca6fc3f56aa86l,
            0x183d9c7694c22839l },
          0 },
        /* 71 << 184 */
        { { 0xe9a0dfbf22e238d7l,0x8690dfd97e8d8d31l,0xb3cb2a0d4006c59cl,
            0xe4d297caa1850d74l },
          { 0x066f10517842d14cl,0x68dd32737d43602bl,0x1f9f5cf931345f39l,
            0x44f18c2b10593890l },
          0 },
        /* 77 << 184 */
        { { 0x8d8c0233a7c3f60bl,0xfb59fe2d2bcbbd4cl,0xfa311680dc3e5b44l,
            0xb3cba9f3fbea5eedl },
          { 0xcb353b2f61e0e690l,0x06edf0c1b6e0efe0l,0xa29578cb1d0c02a2l,
            0xaeb2d677937fec07l },
          0 },
        /* 83 << 184 */
        { { 0xa19a81c5cdd0cac9l,0x5c10b942ec9cf85bl,0x0843ef4639e8c298l,
            0xcfd45d0e6c043258l },
          { 0x1011bcb9fb7e4b58l,0xae6362a544402bbdl,0x9ecc8c68ec15d751l,
            0xbc05998869d1a00bl },
          0 },
        /* 89 << 184 */
        { { 0xe9a43619460147e3l,0x881a6af423067448l,0x94f93ae6cee17a6bl,
            0x469e692f10782558l },
          { 0x01e244a1289bdb32l,0x240645779dddf970l,0x664cbd92d8f521ecl,
            0xadaf8ffb600222d0l },
          0 },
        /* 95 << 184 */
        { { 0x68314c740dbec437l,0x2095e1295ec75e2cl,0x8e88a3ddf0e6c606l,
            0x40ac647d1230f6b2l },
          { 0x09d124aaa2e6b991l,0xa22f9e2bcc81037cl,0xc842b64d15c3a1c2l,
            0x4d822becce808c65l },
          0 },
        /* 101 << 184 */
        { { 0xb02204d06ffb396bl,0x82eb6ecc881bead6l,0xf58432cebd6896c8l,
            0xc243468da38f4b9dl },
          { 0x8486402df8e628bdl,0x5dd338a1a4df2401l,0x748a41ab0daac953l,
            0xaa121d13e51e6235l },
          0 },
        /* 107 << 184 */
        { { 0x6daa0a4e50abc6aal,0x99fcc5bdeafb7cf2l,0xc705f64c4b8dbd2al,
            0x7deff836e7b51e90l },
          { 0xd92f42b859a8180fl,0x3bb298f8618d24acl,0x2433aa7357a56438l,
            0xcf29895b48a6a238l },
          0 },
        /* 113 << 184 */
        { { 0x74079dc59ed25aafl,0x7988245c023d5143l,0x7edfc6a6feb79c24l,
            0x7ed03c50a6baa70fl },
          { 0x71d3413596a753b4l,0x59efbafcef976246l,0xed050260a4a6947fl,
            0xabbc1f8066254247l },
          0 },
        /* 116 << 184 */
        { { 0x1f804e00caa4646fl,0x8643dc8870944924l,0xa37f1ca273f86de9l,
            0xa3199f9228889898l },
          { 0xc273ba580c1e4adfl,0x0f0d38af65bc82f0l,0xd8b28ab5f8a6cd3bl,
            0xeea6e08575894d8el },
          0 },
        /* 119 << 184 */
        { { 0x398f39132c1620f7l,0x9046d2dea921f3a3l,0x40a25a2785b50bb0l,
            0xb9adeca0d32e95f3l },
          { 0xa4199b1bdede5cbfl,0x9068aee084f5410bl,0x6665e4f5730f0397l,
            0x2e9ba18c8ae20659l },
          0 },
        /* 125 << 184 */
        { { 0xd76e9b2351835897l,0x72a0e000012deda6l,0x5bf08922bfec23e4l,
            0x8c2fcf1385cf2b7bl },
          { 0x6c42f935c63332c6l,0x8736c58395eccce9l,0x2d2abbb10721afc8l,
            0x1f7a76cc42d4e029l },
          0 },
    },
    {
        /* 0 << 192 */
        { { 0x00, 0x00, 0x00, 0x00 },
          { 0x00, 0x00, 0x00, 0x00 },
          1 },
        /* 1 << 192 */
        { { 0x56f8410ef4f8b16al,0x97241afec47b266al,0x0a406b8e6d9c87c1l,
            0x803f3e02cd42ab1bl },
          { 0x7f0309a804dbec69l,0xa83b85f73bbad05fl,0xc6097273ad8e197fl,
            0xc097440e5067adc1l },
          0 },
        /* 3 << 192 */
        { { 0x266344a43794f8dcl,0xdcca923a483c5c36l,0x2d6b6bbf3f9d10a0l,
            0xb320c5ca81d9bdf3l },
          { 0x620e28ff47b50a95l,0x933e3b01cef03371l,0xf081bf8599100153l,
            0x183be9a0c3a8c8d6l },
          0 },
        /* 4 << 192 */
        { { 0xb6c185c341dca566l,0x7de7fedad8622aa3l,0x99e84d92901b6dfbl,
            0x30a02b0e7c4ad288l },
          { 0xc7c81daa2fd3cf36l,0xd1319547df89e59fl,0xb2be8184cd496733l,
            0xd5f449eb93d3412bl },
          0 },
        /* 5 << 192 */
        { { 0x25470fabe085116bl,0x04a4337587285310l,0x4e39187ee2bfd52fl,
            0x36166b447d9ebc74l },
          { 0x92ad433cfd4b322cl,0x726aa817ba79ab51l,0xf96eacd8c1db15ebl,
            0xfaf71e910476be63l },
          0 },
        /* 7 << 192 */
        { { 0x72cfd2e949dee168l,0x1ae052233e2af239l,0x009e75be1d94066al,
            0x6cca31c738abf413l },
          { 0xb50bd61d9bc49908l,0x4a9b4a8cf5e2bc1el,0xeb6cc5f7946f83acl,
            0x27da93fcebffab28l },
          0 },
        /* 9 << 192 */
        { { 0x3ce519ef76257c51l,0x6f5818d318d477e7l,0xab022e037963edc0l,
            0xf0403a898bd1f5f3l },
          { 0xe43b8da0496033cal,0x0994e10ea1cfdd72l,0xb1ec6d20ba73c0e2l,
            0x0329c9ecb6bcfad1l },
          0 },
        /* 10 << 192 */
        { { 0xf1ff42a12c84bd9dl,0x751f3ec4390c674al,0x27bb36f701e5e0cal,
            0x65dfff515caf6692l },
          { 0x5df579c4cd7bbd3fl,0xef8fb29785591205l,0x1ded7203e47ac732l,
            0xa93dc45ccd1c331al },
          0 },
        /* 11 << 192 */
        { { 0xbdec338e3318d2d4l,0x733dd7bbbe8de963l,0x61bcc3baa2c47ebdl,
            0xa821ad1935efcbdel },
          { 0x91ac668c024cdd5cl,0x7ba558e4c1cdfa49l,0x491d4ce0908fb4dal,
            0x7ba869f9f685bde8l },
          0 },
        /* 13 << 192 */
        { { 0xed1b5ec279f464bal,0x2d65e42c47d72e26l,0x8198e5749e67f926l,
            0x4106673834747e44l },
          { 0x4637acc1e37e5447l,0x02cbc9ecf3e15822l,0x58a8e98e805aa83cl,
            0x73facd6e5595e800l },
          0 },
        /* 15 << 192 */
        { { 0x468ff80338330507l,0x06f34ddf4037a53el,0x70cd1a408d6993a4l,
            0xf85a159743e5c022l },
          { 0x396fc9c2c125a67dl,0x03b7bebf1064bfcbl,0x7c444592a9806dcbl,
            0x1b02614b4487cd54l },
          0 },
        /* 16 << 192 */
        { { 0x8303604f692ac542l,0xf079ffe1227b91d3l,0x19f63e6315aaf9bdl,
            0xf99ee565f1f344fbl },
          { 0x8a1d661fd6219199l,0x8c883bc6d48ce41cl,0x1065118f3c74d904l,
            0x713889ee0faf8b1bl },
          0 },
        /* 17 << 192 */
        { { 0xb47b60f70de21bb6l,0x64acae4fdcd836cal,0x3375ea6dc744ce63l,
            0xb764265fb047955bl },
          { 0xc68a5d4c9841c2c3l,0x60e98fd7cf454f60l,0xc701fbe2756aea0cl,
            0x09c8885eaab21c79l },
          0 },
        /* 19 << 192 */
        { { 0x45bb810869d2d46cl,0xe47c8b3968c8365al,0xf3b87663267551bdl,
            0x1590768f5b67547al },
          { 0x371c1db2fb2ed3ffl,0xe316691917a59440l,0x03c0d178df242c14l,
            0x40c93fceed862ac1l },
          0 },
        /* 21 << 192 */
        { { 0x1286da692bc982d6l,0x5f6d80f27bdae7e3l,0x3d9c5647a6f064fbl,
            0xfdc8e6a1d74c1540l },
          { 0x97da48c6d68b135al,0xc2097979d66dbfffl,0x0296adb9ea20531dl,
            0xa333730d4ab2c8f0l },
          0 },
        /* 23 << 192 */
        { { 0x0eb3565429847fedl,0xfdc142860a673dd0l,0x721b36278b62dd0bl,
            0x105a293e711a5771l },
          { 0xdf001cce7f761927l,0xf7b681b011d04c7dl,0x16dff792a3ac1996l,
            0x580c120b0fc4ae30l },
          0 },
        /* 25 << 192 */
        { { 0x31ea3d4f7ee8d0bcl,0x3832f22a0f42c3dcl,0xc661061a1a87a2f4l,
            0x0978c9f64b45576bl },
          { 0xb7abac3c6dfb5fd2l,0x27f36a00b7e01b90l,0x68f733cde9429e36l,
            0x953a4681dcbfe8cbl },
          0 },
        /* 27 << 192 */
        { { 0xbfb7c41067fe1eafl,0xa2073c6a6929a785l,0x6f2536f4a75fdb79l,
            0x859ad26d809bca69l },
          { 0x06f2c0693b197e7bl,0x656ad9f48ec0a573l,0xe7c7901f9a4d0262l,
            0xbec29443b938602bl },
          0 },
        /* 28 << 192 */
        { { 0xd00397fc0f0073a4l,0x5b668fa46f8d675fl,0x14374ac91522108cl,
            0x92efa7d10283e42el },
          { 0x673e6df90b6d024al,0x05f914d457581f26l,0xf5c8516267df8c12l,
            0x1197f1b4e06c2462l },
          0 },
        /* 29 << 192 */
        { { 0x6e2d1cb3dd9c90c1l,0x28f82d5a7990579el,0x90e189cd06226195l,
            0xbd2939df19b0dc74l },
          { 0x18b18505c0917177l,0xeed5470d3117d9c4l,0x39ef92eb6c893ca0l,
            0x4533ef8244a41940l },
          0 },
        /* 31 << 192 */
        { { 0xcaee9dec34943ddal,0x8e50e98e8b4b6782l,0x24358ea591ea3a1fl,
            0x71c4c827a9e1c194l },
          { 0xa38baa5d09bb7a94l,0xfb4ab4c057b58f9cl,0x4a01065e24e0ee19l,
            0xb9cf805107b877bfl },
          0 },
        /* 33 << 192 */
        { { 0xd38c1ce0a2980d5el,0x8b84cca4541face7l,0x93298136dbd8d05dl,
            0x582708d03f85c85al },
          { 0x6545eec7282960e4l,0x92e184aebaadec07l,0x05452564fd27a20fl,
            0x79d4668abddce6ebl },
          0 },
        /* 34 << 192 */
        { { 0xf5cc5cccf5191707l,0xe800328bd5d01f67l,0x0572012ebd9b1599l,
            0xf5be11a6863d0125l },
          { 0x4da7ca876ea441e0l,0x47dbf83b321b134al,0x5cbadcdac1acfb4al,
            0x19ac798a734f8e25l },
          0 },
        /* 35 << 192 */
        { { 0xe312623a7002114fl,0xb888b637e047686bl,0x23b2c270cbac91bdl,
            0xb50b31884dbfe02dl },
          { 0x8335ce43de97eef6l,0x6a4e65502bac193al,0xf2b35aac3101f720l,
            0x5b2c88d5379a2015l },
          0 },
        /* 36 << 192 */
        { { 0xf445e77131547128l,0x22761665e27811cal,0x9b944e91a37c6681l,
            0xc0aa06a536899860l },
          { 0x8c2b5816cfcd557el,0xf2734a19945aa357l,0x536ca07ca55a0049l,
            0x8328fdccc636d967l },
          0 },
        /* 37 << 192 */
        { { 0x52b513616aca06bdl,0x8d19b893cdf16560l,0x06b28179c3b438cdl,
            0xde1ef747cd1819e4l },
          { 0xbc6cc43b5f557985l,0xa277e11f61e0142al,0x58890f1e429cc392l,
            0x28d17dbfe5fc8f5el },
          0 },
        /* 39 << 192 */
        { { 0x556df61a29a8f7cbl,0x5cf554dfd14ab27al,0x243f933ba755b886l,
            0xa4d0b06ff2d4ce87l },
          { 0xa745eb8d2c0f1d39l,0xc228747aea3047a5l,0xced774c41d2cecc0l,
            0x54a55c3a774fb01al },
          0 },
        /* 40 << 192 */
        { { 0xa691398a4a9eb3f0l,0x56c1dbff3b99a48fl,0x9a87e1b91b4b5b32l,
            0xad6396145378b5fel },
          { 0x437a243ec26b5302l,0x0275878c3ccb4c10l,0x0e81e4a21de07015l,
            0x0c6265c9850df3c0l },
          0 },
        /* 41 << 192 */
        { { 0x182c3f0e6be95db0l,0x8c5ab38cae065c62l,0xcce8294ebe23abacl,
            0xed5b65c47d0add6dl },
          { 0xbce57d78cc9494cal,0x76f75c717f435877l,0xb3084b2eb06560a9l,
            0x67216bc850b55981l },
          0 },
        /* 43 << 192 */
        { { 0x49c9fd92557de68bl,0x357aa44fc3151b7al,0xd36286d11e4aebd0l,
            0x84562cd736a51203l },
          { 0x42a57e7c3cacc002l,0x794a47751b1e25a3l,0x2c2ab68cac0d4356l,
            0xececb6addb31afdcl },
          0 },
        /* 44 << 192 */
        { { 0x47a5f010b4c21bfel,0x45c5610f0ac3dc20l,0x20e689fcea3bf4dcl,
            0xf244ea49fb5f46e4l },
          { 0xd918e59e8ca38e45l,0x7d6c601d96189a6fl,0x1a40f03854138471l,
            0xfe867d7308a9d034l },
          0 },
        /* 45 << 192 */
        { { 0x3b49e489100c0410l,0x8831d3992adc2b29l,0xb6726cd1247a8116l,
            0x83a71a59d1d56d8el },
          { 0x82ade2fe5cd333e9l,0x3b087ef83ea11f1al,0x17b96ca66ce879cel,
            0xc2f74a971871dc43l },
          0 },
        /* 46 << 192 */
        { { 0xa11a1e3680b576cel,0xf91278bbce2683e8l,0xc3bab95fbae8bc5bl,
            0x642ca26397351715l },
          { 0x5ffc14726fecbbc1l,0x2465e996a23f36d4l,0x06fc53bf5187d428l,
            0x54b4014351fbce91l },
          0 },
        /* 47 << 192 */
        { { 0x081ca6f0eafc7b2cl,0x1ba047a38c48703fl,0xe84865046663accfl,
            0xde1f97568d43689cl },
          { 0xf5373e1d5bc19f75l,0x4e48c493d64b0a54l,0x0c43f4e25807dbf6l,
            0x73bef15167778c36l },
          0 },
        /* 48 << 192 */
        { { 0xca6c0937b1b76ba6l,0x1a2eab854d2026dcl,0xb1715e1519d9ae0al,
            0xf1ad9199bac4a026l },
          { 0x35b3dfb807ea7b0el,0xedf5496f3ed9eb89l,0x8932e5ff2d6d08abl,
            0xf314874e25bd2731l },
          0 },
        /* 49 << 192 */
        { { 0x9d5322e89e9bba53l,0xdd7c9ceb989ff350l,0xd76147eadab0d7b3l,
            0x8e45b1c6d7a9a9a1l },
          { 0x8f896a91d4f10c10l,0x999a73c54068de06l,0x84a9d0839cf0a779l,
            0x4d7cc7689f608ab2l },
          0 },
        /* 51 << 192 */
        { { 0x1833ccddaee93c82l,0x6a05ef7b9f35f20fl,0xc538dac9ae413bc2l,
            0x1e74f4658b4784bdl },
          { 0xccb2bc4a49ffd544l,0x9b88183d2b17ae88l,0x96037a136e43824fl,
            0xbbb61441480bf3dfl },
          0 },
        /* 52 << 192 */
        { { 0x13319d20e090ad42l,0x4ff3186e12cbb719l,0xf38e504913fc0a46l,
            0x83185a1254e60378l },
          { 0x08c4057797ea8935l,0x7b2212a946b614f9l,0xedcdfa520634cfb3l,
            0xdbc60eed9e7d5726l },
          0 },
        /* 53 << 192 */
        { { 0x9b0785c6c7e1070fl,0xec112f53cbf561e5l,0xc93511e37fab3464l,
            0x9e6dc4da9de8e0c2l },
          { 0x7733c425e206b4eel,0xb8b254ef50cedf29l,0xfaee4bbbd50ad285l,
            0x216e76d58c4eb6cfl },
          0 },
        /* 55 << 192 */
        { { 0x9d6a28641d51f254l,0x26c5062a0c2822c3l,0xd74ebba8334bf4eel,
            0x6e5446eb0b8f7305l },
          { 0x5988ae8eb629beccl,0x71e576d0a1de7d1dl,0x15e39592a8873970l,
            0x2b1f9a9342ecc74el },
          0 },
        /* 57 << 192 */
        { { 0xcbdb70727c519bf9l,0x112986bbcaaf48e6l,0x64d4c6d1a13baf3cl,
            0x85ccf6f7a065e77el },
          { 0x183be337749beaedl,0xb3703096cba6c9b1l,0x1edf81f0e42b8afel,
            0xf04ed594ccb73ad7l },
          0 },
        /* 59 << 192 */
        { { 0xfa954ebc38491e9fl,0xf75a5808d32f0b03l,0x196d4a828083b9d3l,
            0x92d5a0be5e8dc9fel },
          { 0x4a507ae9aea628bal,0xeea5861e11a02fb5l,0xa033b84fd23ec8f7l,
            0x1a68c36ec60f11d5l },
          0 },
        /* 60 << 192 */
        { { 0x3dfb55bdab920ef2l,0xe0090971e6244484l,0xdc39fd08f7c6e1a3l,
            0x1ca765356ee79e72l },
          { 0x472c8985287d590cl,0x67635e35ad6daeb4l,0x06ec4e7980f9fee3l,
            0x0aceb39921dc5fdbl },
          0 },
        /* 61 << 192 */
        { { 0xdb2478fd9410a756l,0xd106aefe3a53a1e6l,0x1f4c940d14286333l,
            0x6a98659d04950958l },
          { 0x3232a1c6a6bbe060l,0x19ad132ca5e7ca9bl,0x3c9c13ef800fae29l,
            0x9b0d9068b8660f49l },
          0 },
        /* 63 << 192 */
        { { 0x1e7f043795c53027l,0x5221e5c0da9a3806l,0xf297d8e379d9385fl,
            0x4d69e95f78ba697el },
          { 0xdda936cee76d13c1l,0xd9a5790a485b12f5l,0xeab84add51efbfd0l,
            0xc9a3ee9ca9f44aa4l },
          0 },
        /* 64 << 192 */
        { { 0xefb26a753f73f449l,0x1d1c94f88d44fc79l,0x49f0fbc53bc0dc4dl,
            0xb747ea0b3698a0d0l },
          { 0x5218c3fe228d291el,0x35b804b543c129d6l,0xfac859b8d1acc516l,
            0x6c10697d95d6e668l },
          0 },
        /* 65 << 192 */
        { { 0x8c12e87a15454db4l,0xbc1fc546908e8fbcl,0xc35d83c7e4cf1636l,
            0xcb2f5ac820641524l },
          { 0x2400aae2e644ecd0l,0x9b01e2d14be37119l,0x6cffd52831b54857l,
            0xb3fd5d864b5cbf81l },
          0 },
        /* 71 << 192 */
        { { 0x2e999a4739709fb9l,0x4cb4bbdb62c2b30fl,0x4c7259ac09de0c92l,
            0x73c1e34f8c59a0ffl },
          { 0x0a9e5f2e48cb0a12l,0x5e07449fcf499bb0l,0x0527a8b4b02c4a54l,
            0x7381287159da01e4l },
          0 },
        /* 77 << 192 */
        { { 0xe0b876ca0548ff87l,0x74b5a9b25e03bae3l,0xd5564cc5dd0642d2l,
            0x29ed211b668c4977l },
          { 0xf29d3b7aa7422b11l,0x17f2d3586d29b8bal,0x2e35cdda2bb887del,
            0x650f148078e4444bl },
          0 },
        /* 83 << 192 */
        { { 0x8c75532fb47435ebl,0x2234e2c5a113f905l,0x27b75fea31508ae9l,
            0x09733e40d489ad0bl },
          { 0x73b38464a1b06da1l,0x0aed522dc5b7ccf2l,0xcc04783e78d7e5afl,
            0xa81c8a8ff23eaab7l },
          0 },
        /* 89 << 192 */
        { { 0x6bb5eca73c149ffal,0x4593d851c536487al,0x3675daaad85eb9edl,
            0xbf65d0f9b8a58ffbl },
          { 0x1dc6ddddc22e83eel,0xb673397ee10d3c17l,0x6bdc20600ca62c93l,
            0x260389c30b821f6dl },
          0 },
        /* 95 << 192 */
        { { 0x45f5cf07b417be10l,0x0acb1a44e5d561d8l,0x54b7baeafb1dfbe9l,
            0x0e6e66219044672el },
          { 0xa9b6db6d9a793601l,0xd70eadb8a4a0ba4al,0xaedace846098b89el,
            0x970f2c23ac39d40fl },
          0 },
        /* 101 << 192 */
        { { 0x9dff8d289c7eaaa8l,0x38bcd076db0cc361l,0x25760147cdea9db8l,
            0x44c89dd40163f343l },
          { 0x18815d7544db8365l,0xa186d57b37f3e4b3l,0xa71de7806e84a7fal,
            0xf1c08989e56646b3l },
          0 },
        /* 107 << 192 */
        { { 0xad73e1448fb56a43l,0x078c14fb715543c9l,0xa57770fd64b92d54l,
            0xf0420a9277e9b919l },
          { 0xc660d0cb588ccc1dl,0x069baa1471415c2el,0x747438dc32982740l,
            0x4782ce08767381eel },
          0 },
        /* 113 << 192 */
        { { 0xc2a1ee5fdb3b6b5dl,0x08ce544820e1339fl,0x3cb954b77073955fl,
            0xb9ed2ee7f32d0832l },
          { 0xc0a998b1b4aac98el,0x4912273dbca4bac7l,0xac0f5014c3f92c4al,
            0xbf3dc27f9e916e78l },
          0 },
        /* 116 << 192 */
        { { 0x222c7bae28833944l,0xbb78a867f5e3cf67l,0x590cbd96faf6cfd6l,
            0x1c50aecb3b0d842el },
          { 0x8f2c5df1dbade9a5l,0x60923fb7e3840cecl,0xe8f2db6b03a67512l,
            0x90af187be0d7c628l },
          0 },
        /* 119 << 192 */
        { { 0xb4162b615fee3ccbl,0xe9786e7d7327e651l,0x6c85bd938812d9c1l,
            0xfe4905083dc9e838l },
          { 0xe66f25178a6765dfl,0x72fd294edeee184cl,0x07608bd27b6ec227l,
            0x9df7b664dfdaa5e6l },
          0 },
        /* 125 << 192 */
        { { 0x4aea16602d53a155l,0x7285069a32ab07fdl,0xf6f3000d8b6fcd19l,
            0x010b1f246e98953fl },
          { 0xe180bc559f9aa221l,0x7717ee383cba4534l,0x5997f3aa36cbda06l,
            0x54c6090064a04b05l },
          0 },
    },
    {
        /* 0 << 200 */
        { { 0x00, 0x00, 0x00, 0x00 },
          { 0x00, 0x00, 0x00, 0x00 },
          1 },
        /* 1 << 200 */
        { { 0x25914f7881fdad90l,0xcf638f560d2cf6abl,0xb90bc03fcc054de5l,
            0x932811a718b06350l },
          { 0x2f00b3309bbd11ffl,0x76108a6fb4044974l,0x801bb9e0a851d266l,
            0x0dd099bebf8990c1l },
          0 },
        /* 3 << 200 */
        { { 0xebd6a6777b0ac93dl,0xa6e37b0d78f5e0d7l,0x2516c09676f5492bl,
            0x1e4bf8889ac05f3al },
          { 0xcdb42ce04df0ba2bl,0x935d5cfd5062341bl,0x8a30333382acac20l,
            0x429438c45198b00el },
          0 },
        /* 4 << 200 */
        { { 0xfb2838be67e573e0l,0x05891db94084c44bl,0x9131137396c1c2c5l,
            0x6aebfa3fd958444bl },
          { 0xac9cdce9e56e55c1l,0x7148ced32caa46d0l,0x2e10c7efb61fe8ebl,
            0x9fd835daff97cf4dl },
          0 },
        /* 5 << 200 */
        { { 0x6c626f56c1770616l,0x5351909e09da9a2dl,0xe58e6825a3730e45l,
            0x9d8c8bc003ef0a79l },
          { 0x543f78b6056becfdl,0x33f13253a090b36dl,0x82ad4997794432f9l,
            0x1386493c4721f502l },
          0 },
        /* 7 << 200 */
        { { 0xe566f400b008733al,0xcba0697d512e1f57l,0x9537c2b240509cd0l,
            0x5f989c6957353d8cl },
          { 0x7dbec9724c3c2b2fl,0x90e02fa8ff031fa8l,0xf4d15c53cfd5d11fl,
            0xb3404fae48314dfcl },
          0 },
        /* 9 << 200 */
        { { 0xf02cc3a9f327a07fl,0xefb27a9b4490937dl,0x81451e96b1b3afa5l,
            0x67e24de891883be4l },
          { 0x1ad65d4770869e54l,0xd36291a464a3856al,0x070a1abf7132e880l,
            0x9511d0a30e28dfdfl },
          0 },
        /* 10 << 200 */
        { { 0xfdeed650f8d1cac4l,0xeb99194b6d16bda5l,0xb53b19f71cabbe46l,
            0x5f45af5039b9276cl },
          { 0xd0784c6126ee9d77l,0xf7a1558b0c02ca5dl,0xb61d6c59f032e720l,
            0xae3ffb95470cf3f7l },
          0 },
        /* 11 << 200 */
        { { 0x9b185facc72a4be5l,0xf66de2364d848089l,0xba14d07c717afea9l,
            0x25bfbfc02d551c1cl },
          { 0x2cef0ecd4cdf3d88l,0x8cee2aa3647f73c4l,0xc10a7d3d722d67f7l,
            0x090037a294564a21l },
          0 },
        /* 13 << 200 */
        { { 0x6ac07bb84f3815c4l,0xddb9f6241aa9017el,0x31e30228ca85720al,
            0xe59d63f57cb75838l },
          { 0x69e18e777baad2d0l,0x2cfdb784d42f5d73l,0x025dd53df5774983l,
            0x2f80e7cee042cd52l },
          0 },
        /* 15 << 200 */
        { { 0x43f18d7f4d6ee4abl,0xd3ac8cde9570c3dcl,0x527e49070b8c9b2al,
            0x716709a7c5a4c0f1l },
          { 0x930852b0916a26b1l,0x3cc17fcf4e071177l,0x34f5e3d459694868l,
            0xee0341aba28f655dl },
          0 },
        /* 16 << 200 */
        { { 0xf431f462060b5f61l,0xa56f46b47bd057c2l,0x348dca6c47e1bf65l,
            0x9a38783e41bcf1ffl },
          { 0x7a5d33a9da710718l,0x5a7799872e0aeaf6l,0xca87314d2d29d187l,
            0xfa0edc3ec687d733l },
          0 },
        /* 17 << 200 */
        { { 0x4b764317aa365220l,0x7a24affe68cc0355l,0x76732ed0ceb3df5el,
            0x2ce1332aae096ed0l },
          { 0x89ce70a7b8adac9dl,0xfdddcf05b3fc85c8l,0xbd7b29c6f2ee8bfel,
            0xa1effcb9457d50f3l },
          0 },
        /* 19 << 200 */
        { { 0x6053972dac953207l,0xc2ca9a8408ad12f6l,0x9ed6cd386ba36190l,
            0xa5b50a48539d18a4l },
          { 0xd9491347dbf18c2al,0x2cdce4662e9697cfl,0x4e97db5ca9e31819l,
            0x0fb02e2d4c044b74l },
          0 },
        /* 21 << 200 */
        { { 0x66a4dd414aa5e9ddl,0x6ec7576e64f6aeb9l,0x3f08ce06c7e980b5l,
            0x52fe9fd6c1a2aa7el },
          { 0xfe46e6d95074326al,0xd570ed734c126c1dl,0x86c7ec257217d55al,
            0x3cb434057c3de2b2l },
          0 },
        /* 23 << 200 */
        { { 0x48e0295dcc9e79bfl,0x2419485693eb403dl,0x9386fb7709dd8194l,
            0xb6e89bb101a242f6l },
          { 0xc7994f3924d308d7l,0xf0fbc392de673d88l,0x43eed52ea11abb62l,
            0xc900f9d0c83e7fbel },
          0 },
        /* 25 << 200 */
        { { 0x214a10dca8152891l,0xe6787b4c64f1abb2l,0x276333d9fa1a10edl,
            0xc0e1c88e47dbccbcl },
          { 0x8a3c37c4849dd12el,0x2144a8c8d86e109fl,0xbb6891f7286c140cl,
            0xb0b8c5e29cce5e6fl },
          0 },
        /* 27 << 200 */
        { { 0x3f9e0e3499753288l,0x6b26f1ebe559d93al,0x647fe21d9841faf1l,
            0x48a4b6efa786ea02l },
          { 0x6e09cd22665a882dl,0x95390d81b63ccda6l,0x5b014db4b026a44al,
            0x5b96efb22ad30ff1l },
          0 },
        /* 28 << 200 */
        { { 0x64c50c8b4a3b99e9l,0x2489a675d0a26f4fl,0xe2aacaeed85bc6fdl,
            0x556882038a6019bal },
          { 0x7ceb9da645cfac07l,0xe1ad3d25652dbd09l,0x086adf348d3b5d2bl,
            0xf9256d8aec3654a0l },
          0 },
        /* 29 << 200 */
        { { 0x571c246bf009a690l,0x8fe54231ccd90d3al,0x8adde6adfe173b79l,
            0x75d9a392b05a5e3bl },
          { 0x607f47b0d1bb3a84l,0xe4e3b472058e691al,0xfc0f793bf3d956e3l,
            0x6a6730b605de54dal },
          0 },
        /* 31 << 200 */
        { { 0x4daf7f540d80aaa1l,0xc571d04c229c4574l,0x469e2da5fffca53dl,
            0x9fffe29513ff7f59l },
          { 0x2075da5a33a254f7l,0x769f33acd35e575dl,0x7b940d2c3d35001al,
            0x2d606b57e34c95b7l },
          0 },
        /* 33 << 200 */
        { { 0xc7e4f8b899365f86l,0x8f6f959faae69527l,0x749ffedffdfaeeeal,
            0x2b91f0221b54c2a0l },
          { 0xe75c2352addbdf83l,0xe7329922fff2694cl,0xbb65ae06badadeacl,
            0x16cbb9d1f56be3b5l },
          0 },
        /* 34 << 200 */
        { { 0xb100a4c67a07bd70l,0x222fee7634787efel,0xa4dafc14f1e79d1bl,
            0x0d3a82dad18b8be4l },
          { 0xe0181445fc06922fl,0x0873d99b714a90b6l,0xdf43082fa5087a0el,
            0x195e49367399e0dbl },
          0 },
        /* 35 << 200 */
        { { 0x7e83545aae6fcc9cl,0x1a24fce819e15ce2l,0x4a3465c536d8c6a8l,
            0xd1e5f24109436ae0l },
          { 0xed334bfc6be463d5l,0xc46a600b934fbdcfl,0xbd2fd65b920321ffl,
            0x953fa91767fa154el },
          0 },
        /* 36 << 200 */
        { { 0x5dca4995f93ddad1l,0x061efcabf72470c2l,0xad78d54d5e7e0741l,
            0xa91f4e839c4e0ab4l },
          { 0xdd4403af5c75aa0dl,0x4308c8ee13c69113l,0x3a3b66f51ebc36adl,
            0xc07cc3f0f4bf777al },
          0 },
        /* 37 << 200 */
        { { 0x3fd1963e37a86b32l,0x22e236d60bd0880el,0xb87467cf89f0fa5cl,
            0x85b9c6c0310e0265l },
          { 0x82979a96783459ael,0xd19b0919bd529ed3l,0xa21f771808434f94l,
            0x3dd130a9195369c6l },
          0 },
        /* 39 << 200 */
        { { 0xc61e62767915d157l,0xc48244279e07fb0el,0x8980c1cc8420ea49l,
            0x10d82e4a588d4e2bl },
          { 0xdddecd52b17eff2dl,0xe44c7b2ded8492a4l,0x96ca89ebb9bea6afl,
            0x724166fe1b03ed03l },
          0 },
        /* 40 << 200 */
        { { 0xfc87975f8fb54738l,0x3516078827c3ead3l,0x834116d2b74a085al,
            0x53c99a73a62fe996l },
          { 0x87585be05b81c51bl,0x925bafa8be0852b7l,0x76a4fafda84d19a7l,
            0x39a45982585206d4l },
          0 },
        /* 41 << 200 */
        { { 0x8bbc484ed551f3e1l,0x6e058a90b7eb06d2l,0xfaccd9a0e5cd281al,
            0xe7661b78d5b44900l },
          { 0x03afe115725fde22l,0xbe929230c7229fd1l,0x5cd0d16a0000035el,
            0x1f6a9df0c8f5a910l },
          0 },
        /* 43 << 200 */
        { { 0xe54bbcfd535dfc82l,0x89be0b89a9012196l,0xa67831ee71011beal,
            0x2ea7a8292db43878l },
          { 0xff7c144378ffe871l,0xa67dc3d4c63f65eal,0xbbfc7fc2a1527419l,
            0x6440380bf6c36b8fl },
          0 },
        /* 44 << 200 */
        { { 0x71ab9f69d812d7e6l,0x2847c5516e142126l,0x9e27755bb31e7753l,
            0xb89533e2943b8c7fl },
          { 0xbe7f0c6e14fa7dc6l,0x782a06388cee1f7al,0x7069292938e13a6bl,
            0x1e1221f0c63f4d28l },
          0 },
        /* 45 << 200 */
        { { 0x9030aa9a63a431f4l,0x0fa7b5d45039a318l,0x6a0cf40af083687dl,
            0x46689cec659fa752l },
          { 0x8259727a456fa97el,0x4f618a355b08d7fcl,0x2c44217b72028d15l,
            0x8083b09935111e32l },
          0 },
        /* 46 << 200 */
        { { 0xaa5976523b5b29f1l,0xb07f10ab37432a54l,0x16e3e2236e36556fl,
            0xf1c7c9bd47cd4586l },
          { 0xa4eef99d3f87216dl,0x4e54d3c52e1eaa79l,0x534c5901d2540d91l,
            0x718df7c9b6f0fcfcl },
          0 },
        /* 47 << 200 */
        { { 0x99497f8a2eb0ee3bl,0x87e550c1caeb3a20l,0xd23e053dfb91627cl,
            0xb971c043873124e6l },
          { 0x3581ab853b16e467l,0x24541c926145187bl,0x4423ec5c010c2527l,
            0x775f13029fa82a68l },
          0 },
        /* 48 << 200 */
        { { 0x499b6ab65eb03c0el,0xf19b795472bc3fdel,0xa86b5b9c6e3a80d2l,
            0xe43775086d42819fl },
          { 0xc1663650bb3ee8a3l,0x75eb14fcb132075fl,0xa8ccc9067ad834f6l,
            0xea6a2474e6e92ffdl },
          0 },
        /* 49 << 200 */
        { { 0xbaebdd8a0c40aec4l,0x5eccafb563e8cfd0l,0x1c204c0eb5159938l,
            0x607109d34b996aa9l },
          { 0x024c6c4b9cef59fel,0xbc846e216ed4b6f1l,0xf6a50ff3ff652c0al,
            0x368af2c72d95220cl },
          0 },
        /* 51 << 200 */
        { { 0xec9c2e35cbd3ccafl,0xb9eeff3ddcda8f30l,0x82012e191062d02el,
            0xed964cc94efc6b6el },
          { 0x8853ea0a6bf54c22l,0xea40fcc0f3cbe264l,0x21f9c01ddecf114el,
            0x05e754c63da71e59l },
          0 },
        /* 52 << 200 */
        { { 0xe6a26d38046dfc72l,0x70409579c2175175l,0x2a575ac5d44e0c1dl,
            0xb35395e01479ab5al },
          { 0x1550a5d4f7bfbd8el,0x01daeb680778807bl,0xe0aa940321294dbal,
            0x84bcdc8c5b5a93b7l },
          0 },
        /* 53 << 200 */
        { { 0x876cc4d2520f04abl,0x6e320f5da85ff6a8l,0x7c504720ce17bc80l,
            0xe7907079a62089f9l },
          { 0xa45c4ac7bca45feel,0xd8f3facd5bd54b0cl,0xc0b036277b3e4a24l,
            0xaabe96dfe4cd4b57l },
          0 },
        /* 55 << 200 */
        { { 0xdc85a54773862ce4l,0x169051a3cc6f5d85l,0x8e3d3be0355f4df7l,
            0xa139d6fac72bac76l },
          { 0xddc95d0dfeb0a6f0l,0xd53f70e545cd6955l,0x18eede5e47e54112l,
            0x4a135dc9cbc6a52el },
          0 },
        /* 57 << 200 */
        { { 0x705a08ba90a58fb4l,0x10eef880fb3f8a64l,0x4ced9ba2f8e585ffl,
            0xb4f0f955fc6ebef5l },
          { 0x152c1a338d8b739el,0xb2be701db495bee5l,0xd27141a8d3540a74l,
            0x20c8a00247f9e9d7l },
          0 },
        /* 59 << 200 */
        { { 0x6d5ae921f5adcb3fl,0xaed1047003a3b610l,0x7c75e36f22256df9l,
            0xe664b36fb97dae99l },
          { 0x138b5eca91e746ael,0xb3e01ef5648674a7l,0xa3f256da9e375c74l,
            0xa00e82bc6a82d6f3l },
          0 },
        /* 60 << 200 */
        { { 0xe7a01eae6e28b4a8l,0xb3bf8224782166c9l,0x0b7ba2a06a244510l,
            0x9751a69c2abbb4dbl },
          { 0xb611adc1b3f9fcbcl,0x1d08eb3b436c4675l,0x1c71e98a20f96a64l,
            0x33d9b58c7ffd3f08l },
          0 },
        /* 61 << 200 */
        { { 0x7c7b03c1affa2d6cl,0x5f189bb9aec6e624l,0xe77a1eedadeff5e7l,
            0xfc58b90f4280b467l },
          { 0x561e5d579b71cb4el,0x8ed767aa36d6a17el,0x38d8671e8aa9e188l,
            0x7bc68f07a95350c0l },
          0 },
        /* 63 << 200 */
        { { 0xe0cd38cf98c01384l,0xc6741123a4226d9fl,0xdd1d42dbf877a0b8l,
            0xc5986ef0110b3cbal },
          { 0xeba949f809c8cebel,0x96b47bc4bd39f1dcl,0xbad140b6e07a2a3cl,
            0x2a8d80999ac5ca8al },
          0 },
        /* 64 << 200 */
        { { 0x39d934abd3c095f1l,0x04b261bee4b76d71l,0x1d2e6970e73e6984l,
            0x879fb23b5e5fcb11l },
          { 0x11506c72dfd75490l,0x3a97d08561bcf1c1l,0x43201d82bf5e7007l,
            0x7f0ac52f798232a7l },
          0 },
        /* 65 << 200 */
        { { 0x8cf27618590ca850l,0x58134f6f44bb94f2l,0x0a147562b78b4eecl,
            0x2e5986e39f1ed647l },
          { 0x9becf893348393b0l,0xaea21b92c31c2a86l,0x3d69859e5ff1b9a6l,
            0x6fcd19f4cd805691l },
          0 },
        /* 71 << 200 */
        { { 0x81619bd4841f43c3l,0x3a3325538e5c61f0l,0x2b68921eda862151l,
            0x97f5c8a741a491f8l },
          { 0x8b452094d3b9afa0l,0x93b2b7b4f2124dbcl,0x53285e7d26e0e26dl,
            0x3f003fc5c8a24edel },
          0 },
        /* 77 << 200 */
        { { 0x4cdabb586c025824l,0x5935ad1586bfcd7dl,0x8ce2c3101b7c5533l,
            0x761c9fe96cae8808l },
          { 0x8a0723f5d9e66d70l,0xb640b323dcced11dl,0x5768528051ae548cl,
            0x83576f75d53f3f2cl },
          0 },
        /* 83 << 200 */
        { { 0xc715edc47b532ec3l,0x159765e6c4a6e14bl,0x4a74f15228cd2d45l,
            0xbfd309edae8c753bl },
          { 0xf56bb5315d6d5245l,0x2c89c21833b30a55l,0xe436141acd4ed5fal,
            0x7eb7a5c707868ee6l },
          0 },
        /* 89 << 200 */
        { { 0x9a3ad3ffb0c7c48cl,0x25e8d977738e3638l,0xbb6c6c9d1c024074l,
            0xeda1ac0f8cfdf416l },
          { 0x93059ba538de49e2l,0xdb199cfc1b9ce741l,0x49b05e9446f3b494l,
            0x717cafc606480902l },
          0 },
        /* 95 << 200 */
        { { 0x8d27421052885708l,0x9d2297fd74e5b9b5l,0xe7cb6a68dc4d7318l,
            0x0b60b0d276357b31l },
          { 0x57301994532c2095l,0xfbae2ba203373452l,0xe8020b20ba700583l,
            0x1ca7772c2988919cl },
          0 },
        /* 101 << 200 */
        { { 0x723296eb918f3eecl,0x358c9ff0b79901c6l,0x64a1934c8d5e814cl,
            0x7e5a9afced165177l },
          { 0xd783840168733e7al,0xfcf3c0b6f61ede6dl,0x94ec0bf08434e804l,
            0xa5a70153c192c1cdl },
          0 },
        /* 107 << 200 */
        { { 0x03cdf976c23e49d4l,0x51e5cfa5a2ae72d5l,0x7716faa3100f7a51l,
            0xc53153a2c14dc015l },
          { 0xe7c69b052b47ec18l,0xff4756907ea93b01l,0x55fde3c540a2f205l,
            0x0263d0b12f85aed6l },
          0 },
        /* 113 << 200 */
        { { 0x668c56619686fe30l,0x382a8ccd8f73a476l,0xda012cbfb40a85e7l,
            0x55ea1e72e9e88b91l },
          { 0x8312556088cc5afcl,0x44ae54cbc45b19c7l,0xc91fffa8f86a02cdl,
            0xc79f573752d7e89bl },
          0 },
        /* 116 << 200 */
        { { 0x652b50523e357579l,0x08ce7d3a2afe5746l,0x9dc1cca6f71a12efl,
            0x80a221c24f6c4196l },
          { 0xdde40eff0f49f508l,0x7995bb46913b0dc3l,0x4adbdeb385e44f6el,
            0x6816bb3ab222e4bbl },
          0 },
        /* 119 << 200 */
        { { 0xce1ee518579a1a4dl,0x5d86e8912bc3870al,0x230878d18da907c4l,
            0xc648392777ae7ea8l },
          { 0x64319653016c0ad7l,0x7cbfa0b0b71f20dal,0xbf087dc3395ed4d8l,
            0x59512add307d218dl },
          0 },
        /* 125 << 200 */
        { { 0x7378a969d8ae335el,0x11c69965506d3a42l,0x212539769949468al,
            0x570cf87e64995050l },
          { 0xf300ad2e30b94e22l,0xbc159cf8f36dad32l,0xdff3b3767ca8aa6al,
            0xa5de93b5627fb9e7l },
          0 },
    },
    {
        /* 0 << 208 */
        { { 0x00, 0x00, 0x00, 0x00 },
          { 0x00, 0x00, 0x00, 0x00 },
          1 },
        /* 1 << 208 */
        { { 0x75d9bc15adf7cccfl,0x81a3e5d6dfa1e1b0l,0x8c39e444249bc17el,
            0xf37dccb28ea7fd43l },
          { 0xda654873907fba12l,0x35daa6da4a372904l,0x0564cfc66283a6c5l,
            0xd09fa4f64a9395bfl },
          0 },
        /* 3 << 208 */
        { { 0xc51aa29e5cfe5c48l,0x82c020ae815ee096l,0x7848ad827549a68al,
            0x7933d48960471355l },
          { 0x04998d2e67c51e57l,0x0f64020ad9944afcl,0x7a299fe1a7fadac6l,
            0x40c73ff45aefe92cl },
          0 },
        /* 4 << 208 */
        { { 0xe5f649be9d8e68fdl,0xdb0f05331b044320l,0xf6fde9b3e0c33398l,
            0x92f4209b66c8cfael },
          { 0xe9d1afcc1a739d4bl,0x09aea75fa28ab8del,0x14375fb5eac6f1d0l,
            0x6420b560708f7aa5l },
          0 },
        /* 5 << 208 */
        { { 0xbf44ffc75488771al,0xcb76e3f17f2f2191l,0x4197bde394f86a42l,
            0x45c25bb970641d9al },
          { 0xd8a29e31f88ce6dcl,0xbe2becfd4bb7ac7dl,0x13094214b5670cc7l,
            0xe90a8fd560af8433l },
          0 },
        /* 7 << 208 */
        { { 0x0ecf9b8b4ebd3f02l,0xa47acd9d86b770eal,0x93b84a6a2da213cel,
            0xd760871b53e7c8cfl },
          { 0x7a5f58e536e530d7l,0x7abc52a51912ad51l,0x7ad43db02ea0252al,
            0x498b00ecc176b742l },
          0 },
        /* 9 << 208 */
        { { 0x9ff713ef888ae17fl,0x6007f68fb34b7bebl,0x5d2b18983b653d64l,
            0xcbf73e91d3ca4b1bl },
          { 0x4b050ad56cdfb3a1l,0x41bd3ec3d1f833a4l,0x78d7e2ee719d7bf5l,
            0xea4604672a27412el },
          0 },
        /* 10 << 208 */
        { { 0x7dad6d1b42cd7900l,0xb6e6b439e058f8a4l,0x8836f1e662aa3bbcl,
            0xd45bf2c811142b0al },
          { 0xae324bac3c045ed1l,0x372be24d270a8333l,0xeeda7a3a6b7c73b6l,
            0xf6675402db49562al },
          0 },
        /* 11 << 208 */
        { { 0xc312ba68441e760dl,0x84d0d061a50e512el,0xfe764f4e4bbdd849l,
            0xa924adcf9dadd5c0l },
          { 0x08685961debfe976l,0xd3d846c529fba601l,0x43bf8227dc3f4040l,
            0x05e767b8a49e9ff5l },
          0 },
        /* 13 << 208 */
        { { 0xc4689c309953e453l,0x5e355a2e1712dca5l,0x1ff83c81f1cd96f7l,
            0xb06b89fb44cf56dbl },
          { 0x1827705365f16e0dl,0x6403b91de5618672l,0xba3f9475be384bc6l,
            0x7f691cbe303ce5f3l },
          0 },
        /* 15 << 208 */
        { { 0x4589ba03210f4045l,0xd5e7366301e8012al,0x1c26052d74462ffal,
            0xe78f600c4f989519l },
          { 0xc63ca0c97cee0b2fl,0xbe588573af760b5fl,0x05906fc4593773cdl,
            0xd5970fb0e322d5afl },
          0 },
        /* 16 << 208 */
        { { 0x103c46e60ebcf726l,0x4482b8316231470el,0x6f6dfaca487c2109l,
            0x2e0ace9762e666efl },
          { 0x3246a9d31f8d1f42l,0x1b1e83f1574944d2l,0x13dfa63aa57f334bl,
            0x0cf8daed9f025d81l },
          0 },
        /* 17 << 208 */
        { { 0xf67c098aae0690aal,0x1a4656422b7bc62bl,0xaffc6b917220dea2l,
            0xd97ac543d2552deel },
          { 0x1f84514a7e816b8el,0xe9887e81a8f38552l,0x2e6358e6847ad46bl,
            0x1f67871e6bc9895el },
          0 },
        /* 19 << 208 */
        { { 0x2462b6e0d47f43fal,0x71db3610d8a245e5l,0x0c26b0e734208974l,
            0x0cd6d49d2029bd2el },
          { 0xf207c9f6091922b8l,0x0c476c5c7f0fbf66l,0x6de7efb2295d6da8l,
            0xea054ee10ced6cfel },
          0 },
        /* 21 << 208 */
        { { 0xd21496e3e9bd795cl,0xf293f617c6a557del,0x9d041b7239a45642l,
            0xe8353dab4ac87f80l },
          { 0x21e9f35620d8d019l,0x1f4adca9d2fb2668l,0xe5f68227dfecd64al,
            0x10d71b79d7f09ec0l },
          0 },
        /* 23 << 208 */
        { { 0xca3f068999f87118l,0x99a933911b2417f0l,0xa383481a3d1f70e5l,
            0x7a31a6c833b14414l },
          { 0x9d60f4368b2a9931l,0xd4c97ded80588534l,0x7cb29e82ab6a8bdal,
            0x3799bdad97b4c45al },
          0 },
        /* 25 << 208 */
        { { 0x51da0ff629011af3l,0xcbb03c809a4f0855l,0xea3536725555b10bl,
            0x4bf94e025c7da97el },
          { 0x384352f5ff713300l,0xb2c2b675192d41e6l,0x4ff66861625ca046l,
            0xf0f5e472013dddc4l },
          0 },
        /* 27 << 208 */
        { { 0x38c44cdc59987914l,0xad7f2829757fb853l,0x9aabf1c8688e3342l,
            0xbe0f1e4ef534c850l },
          { 0x732cac652ec24ecal,0x9328b657933bb5e4l,0xe2747ff60bb31033l,
            0xdbaab72cfcdc36acl },
          0 },
        /* 28 << 208 */
        { { 0x0e5e3049a639fc6bl,0xe75c35d986003625l,0x0cf35bd85dcc1646l,
            0x8bcaced26c26273al },
          { 0xe22ecf1db5536742l,0x013dd8971a9e068bl,0x17f411cb8a7909c5l,
            0x5757ac98861dd506l },
          0 },
        /* 29 << 208 */
        { { 0xaf410d5aac66a3e8l,0x39fcbffb2031f658l,0xd29e58c947ce11fbl,
            0x7f0b874965f73e49l },
          { 0xedc30f4b27fea6c6l,0xe03b9103d2baa340l,0xa7bb3f17ae680612l,
            0xe06656a8197af6f0l },
          0 },
        /* 31 << 208 */
        { { 0x84562095bff86165l,0x994194e916bc7589l,0xb1320c7ec14c6710l,
            0x508a8d7f766e978fl },
          { 0xd04adc9ec7e1f6fel,0x7bafaff68398cecfl,0x906df2fccef3b934l,
            0xc65afe18f3008c38l },
          0 },
        /* 33 << 208 */
        { { 0x477ffeeeab983130l,0x5426363a96e83d55l,0xcf0370a15204af42l,
            0x99834414b5a6ea8fl },
          { 0xf475ba711ab4ee8al,0x8486da5d0102d8f2l,0x55082e713839c821l,
            0xa57e58395b65defal },
          0 },
        /* 34 << 208 */
        { { 0x34b2185bbbb33a76l,0x189038b7d48158c2l,0xfa32eb90e9e90217l,
            0x79271771730e74dfl },
          { 0x315ed8c2a5d01ffdl,0x9799dae723e6a95el,0x40070aa016f5715al,
            0x40e6c0ca5ea51f8cl },
          0 },
        /* 35 << 208 */
        { { 0x099c0570d8132163l,0xcd5508a3023dbbf3l,0x18162ff526bfe6a6l,
            0xf39e071144bbb455l },
          { 0x49664996eaa3cf96l,0x1c6442d5e2649be9l,0x6199f740c01d269dl,
            0x4be605ee37542c11l },
          0 },
        /* 36 << 208 */
        { { 0xc7313e9cf36658f0l,0xc433ef1c71f8057el,0x853262461b6a835al,
            0xc8f053987c86394cl },
          { 0xff398cdfe983c4a1l,0xbf5e816203b7b931l,0x93193c46b7b9045bl,
            0x1e4ebf5da4a6e46bl },
          0 },
        /* 37 << 208 */
        { { 0xd032fbfd0dbf82b4l,0x707181f668e58969l,0xef434381e7be2d5el,
            0x290669176f2c64ddl },
          { 0xf66cffc3772769abl,0x68d8a76a17aad01cl,0xdd3991c590f6e078l,
            0xdb74db06ea4ac7dcl },
          0 },
        /* 39 << 208 */
        { { 0x9f34a7c11c78be71l,0x7bf2f2d149ca6987l,0xb528a514dcd34afcl,
            0x4dddb3f1183a68b1l },
          { 0x54d2626660b83883l,0x9073e4e0e0cd8dadl,0xbd2b837d9eb818b2l,
            0x5fa5f9086ae2e32dl },
          0 },
        /* 40 << 208 */
        { { 0xf9942a6043a24fe7l,0x29c1191effb3492bl,0x9f662449902fde05l,
            0xc792a7ac6713c32dl },
          { 0x2fd88ad8b737982cl,0x7e3a0319a21e60e3l,0x09b0de447383591al,
            0x6df141ee8310a456l },
          0 },
        /* 41 << 208 */
        { { 0xcd02ba1e0df98a64l,0x301b6bfa03f5676el,0x41e1a8d4a2fe4090l,
            0x489c1cbf47f0e1dcl },
          { 0x4171a98c20760847l,0xdcb21cee77af4796l,0x5fb0f0c9d0b7e981l,
            0x4c2791dff33b9f8dl },
          0 },
        /* 43 << 208 */
        { { 0x95d7ec0c50420a50l,0x5794665c2a6756d5l,0x73558c6e9101e7f5l,
            0xa3fa0f8c1642af0el },
          { 0xa11b309b4ee43551l,0x3939de30cb8fc712l,0x9710f2320fde8921l,
            0x2a4db2d5cae8b41cl },
          0 },
        /* 44 << 208 */
        { { 0xaec1a039e6d6f471l,0x14b2ba0f1198d12el,0xebc1a1603aeee5acl,
            0x401f4836e0b964cel },
          { 0x2ee437964fd03f66l,0x3fdb4e49dd8f3f12l,0x6ef267f629380f18l,
            0x3e8e96708da64d16l },
          0 },
        /* 45 << 208 */
        { { 0xdf6cdac0bc4c78adl,0xbe9e32182e97376el,0xa37f9d8b1a139274l,
            0x7640c3982807128el },
          { 0xe9735166c05b5f85l,0xbccd3675100e5716l,0x51376a293e5c9682l,
            0x95efe088848f6aeal },
          0 },
        /* 46 << 208 */
        { { 0xfac2d7dd23d14105l,0xdda17149a9136f52l,0xb9f3a9c672d1a99bl,
            0x2fcf532a142c3b20l },
          { 0xc2731f1e61190c1bl,0x26dbe810a76509e4l,0xc96cc431908bb92fl,
            0x5661a84d80e3e694l },
          0 },
        /* 47 << 208 */
        { { 0x5194d144150ba121l,0x8de57c48b6b11561l,0x803228da96c156d9l,
            0x2112e4250a8f6376l },
          { 0x15436294643449ffl,0xfc3880add4118cd0l,0x16ed90731e3f7413l,
            0xa400699901d38d6dl },
          0 },
        /* 48 << 208 */
        { { 0xbc19180c207674f1l,0x112e09a733ae8fdbl,0x996675546aaeb71el,
            0x79432af1e101b1c7l },
          { 0xd5eb558fde2ddec6l,0x81392d1f5357753fl,0xa7a76b973ae1158al,
            0x416fbbff4a899991l },
          0 },
        /* 49 << 208 */
        { { 0xf84c9147c52d7384l,0x86391accec01efa6l,0xffd68616f9c6f3f4l,
            0xc7536461b17c2de6l },
          { 0xa81f4ba10121abdfl,0xa068a2e26f6eae27l,0xe0ee90350eb159f0l,
            0x4c48f761fd8c4b9cl },
          0 },
        /* 51 << 208 */
        { { 0x4b6d71e87790000cl,0xced195744ce9293el,0xc25626a3747585e8l,
            0xb8307d22d7044270l },
          { 0xf08e7ef6117c24cbl,0xae6403162f660d04l,0xbc3ffdcff224a2fdl,
            0x1ebc0328d0586c7el },
          0 },
        /* 52 << 208 */
        { { 0x9e65fdfd0d4a9dcfl,0x7bc29e48944ddf12l,0xbc1a92d93c856866l,
            0x273c69056e98dfe2l },
          { 0x69fce418cdfaa6b8l,0x606bd8235061c69fl,0x42d495a06af75e27l,
            0x8ed3d5056d873a1fl },
          0 },
        /* 53 << 208 */
        { { 0x46b160e5a6022278l,0x86b1d50cc30a51fcl,0xe898ac0e684b81b7l,
            0x04d591e277b93597l },
          { 0xd20cac347626e18al,0xb49c941f0a968733l,0x054e6e7e21631627l,
            0xd6d33db9d4c716b1l },
          0 },
        /* 55 << 208 */
        { { 0xaa79ab4bf91e9b75l,0x7df3235bd34d961dl,0x9f3954e6534a40e1l,
            0x80f88d2c790b4456l },
          { 0x98f7711b21e9fb2al,0x0a04c318877d27e6l,0x499b7c2412338848l,
            0x0b1dbe9ccd5e7ec3l },
          0 },
        /* 57 << 208 */
        { { 0xb430ff44e04715ffl,0x671358d565d076d0l,0x3946d38f22c3aa06l,
            0x80919ea363b2d627l },
          { 0x14ffa219e8790922l,0xfe1d895ae8d89c48l,0x717e9e51748e806el,
            0xb91e1ddf550d711dl },
          0 },
        /* 59 << 208 */
        { { 0x8aac26225f540127l,0x57cd5d7cba25f742l,0x87006a6b1df7a0fcl,
            0x88e9ab863ecbf26cl },
          { 0xe1b8155f9143b314l,0xc00196130b679bddl,0x819e7b61a1871d07l,
            0xc36e7892cc2c9cc9l },
          0 },
        /* 60 << 208 */
        { { 0x4b03c55b8e33787fl,0xef42f975a6384673l,0xff7304f75051b9f0l,
            0x18aca1dc741c87c2l },
          { 0x56f120a72d4bfe80l,0xfd823b3d053e732cl,0x11bccfe47537ca16l,
            0xdf6c9c741b5a996bl },
          0 },
        /* 61 << 208 */
        { { 0x65729b05301ee370l,0x3ed09a2a24c2824cl,0x781ef66a33481977l,
            0xf2ccdeec193506d0l },
          { 0x92b4f70d703422d6l,0x7f004a43f80a1b99l,0x47db23607a856445l,
            0x783a8dd1ce5b0622l },
          0 },
        /* 63 << 208 */
        { { 0x7febefd34e9aac5al,0x601c89e2bdd6173el,0x79b08930c257431el,
            0x915d601d399ee099l },
          { 0xfa48347eca02acd2l,0xc33249baeeb7ccedl,0xd76e408755704722l,
            0xd3709c600dcf4878l },
          0 },
        /* 64 << 208 */
        { { 0xee7332c7904fc3fal,0x14a23f45c7e3636al,0xc38659c3f091d9aal,
            0x4a995e5db12d8540l },
          { 0x20a53becf3a5598al,0x56534b17b1eaa995l,0x9ed3dca4bf04e03cl,
            0x716c563ad8d56268l },
          0 },
        /* 65 << 208 */
        { { 0x963353201580f3adl,0x6c495304b0cd50d4l,0xd035cdc7555ff981l,
            0xe65cd063c6b6bdfbl },
          { 0x7deb3cbb437e749cl,0xa9de9f3db5dc24a1l,0xe2e76a2b35c29ffal,
            0x4d35e261323ba650l },
          0 },
        /* 71 << 208 */
        { { 0x52c46fc8c89e2766l,0x7330b02bb945e5f2l,0xc77ef75c2673ebbcl,
            0x1740e72657c33783l },
          { 0xf0312d29623565fbl,0xff9f707af0ca1ed9l,0xb98609ca5ea51a4al,
            0xde86b9a87b5cc91fl },
          0 },
        /* 77 << 208 */
        { { 0x0dece4badca158b7l,0x5e39baf6a3e9f837l,0xcf14e6dc4d57b640l,
            0x0548aaa4b67bcbe7l },
          { 0xb6cf5b393c90e434l,0xf8b3c5645006f3abl,0xa74e92859bf04bd9l,
            0xf59a3a6bf99c8977l },
          0 },
        /* 83 << 208 */
        { { 0x652ca66ac5b072d5l,0x2102b55993ad4928l,0x1b5f192d88210f9bl,
            0xb18710144c6ad7e5l },
          { 0x3979fde3bc0abf13l,0xb5cb4c7dac3fd631l,0x4aedffa6c200ec7bl,
            0x8aed81ceaddf3610l },
          0 },
        /* 89 << 208 */
        { { 0x72b48105abeefbael,0x0e9e6e41827bb22bl,0xf45ada151e52a848l,
            0xb8e94579534867a2l },
          { 0x3a08773b7adb0fdcl,0xe7133a28b83316dfl,0xc8b7b08c5bb41470l,
            0x28719eb4aaf140c7l },
          0 },
        /* 95 << 208 */
        { { 0x398996cd430007cel,0x20d8c0e07642d616l,0x81566639a7eb2397l,
            0x74aa0b692e133732l },
          { 0x326745907ba80aa7l,0x56a491c39bd69d64l,0xc8c8b040e54dcce0l,
            0x3f991872d571d037l },
          0 },
        /* 101 << 208 */
        { { 0x70e681fa4fb595c9l,0xf0635d6386b4d97bl,0xfc029284c1347081l,
            0x5a4e9cbe4fee0303l },
          { 0xd43da8609c31094fl,0x0412cfed6515b4aal,0x10fc06da8d53be86l,
            0x4b7b380b4bccc94dl },
          0 },
        /* 107 << 208 */
        { { 0x560d57408e7d6738l,0xa82268a8937f12a2l,0x87787b2d3d95b463l,
            0xb36539b2030e23bfl },
          { 0x60d16b8fd61e761dl,0x96ba2949fe8efccdl,0x8c170eda667fa7ebl,
            0xc880d74cf800d7c3l },
          0 },
        /* 113 << 208 */
        { { 0x7c05d6c1efcbfea0l,0xae7ba3291a2f6dd8l,0x521598ed5bd42ecfl,
            0x58e07842ef0ab40cl },
          { 0xae65105f66c752a5l,0x4910fba45f99d499l,0xbfdaf5fce9e44357l,
            0x6aaf4053796ee5b6l },
          0 },
        /* 116 << 208 */
        { { 0xf58fecb16f640f62l,0xe274b92b39f51946l,0x7f4dfc046288af44l,
            0x0a91f32aeac329e5l },
          { 0x43ad274bd6aaba31l,0x719a16400f6884f9l,0x685d29f6daf91e20l,
            0x5ec1cc3327e49d52l },
          0 },
        /* 119 << 208 */
        { { 0x615ac02527ba93edl,0x0d43915d3556ef47l,0x8c739fd1cb0cda89l,
            0xa2318169625f7a16l },
          { 0x17d486113e0479cel,0x814beb6038ee541el,0x09c9807fb98ef355l,
            0x4ad3668752d07af6l },
          0 },
        /* 125 << 208 */
        { { 0x5c1f42e444f3f568l,0xd743b7c078fb409bl,0xe09edccb6224362cl,
            0x7f13d140c5fe872cl },
          { 0x85e8cb88f403c0ebl,0x918a231b688d20a0l,0xc65b7ab9f246c73fl,
            0xda743fbf76dbd6adl },
          0 },
    },
    {
        /* 0 << 216 */
        { { 0x00, 0x00, 0x00, 0x00 },
          { 0x00, 0x00, 0x00, 0x00 },
          1 },
        /* 1 << 216 */
        { { 0xa0158eeae457a477l,0xd19857dbee6ddc05l,0xb326522418c41671l,
            0x3ffdfc7e3c2c0d58l },
          { 0x3a3a525426ee7cdal,0x341b0869df02c3a8l,0xa023bf42723bbfc8l,
            0x3d15002a14452691l },
          0 },
        /* 3 << 216 */
        { { 0xf3cae7e9262a3539l,0x78a49d1d6670d59el,0x37de0f63c1c5e1b9l,
            0x3072c30c69cb7c1cl },
          { 0x1d278a5277c850e6l,0x84f15f8f1f6a3de6l,0x46a8bb45592ca7adl,
            0x1912e3eee4d424b8l },
          0 },
        /* 4 << 216 */
        { { 0x6ba7a92079e5fb67l,0xe1331feb70aa725el,0x5080ccf57df5d837l,
            0xe4cae01d7ff72e21l },
          { 0xd9243ee60412a77dl,0x06ff7cacdf449025l,0xbe75f7cd23ef5a31l,
            0xbc9578220ddef7a8l },
          0 },
        /* 5 << 216 */
        { { 0xdc988086365e668bl,0xada8dcdaaabda5fbl,0xbc146b4c255f1fbel,
            0x9cfcde29cf34cfc3l },
          { 0xacbb453e7e85d1e4l,0x9ca09679f92358b5l,0x15fc2d96240823ffl,
            0x8d65adf70c11d11el },
          0 },
        /* 7 << 216 */
        { { 0x775557f10296f4fdl,0x1dca76a3ea51b436l,0xf3e98f60fb950805l,
            0x31ff32ea831cf7f1l },
          { 0x643e7bf18d2c714bl,0x64b5c3392e9d2acal,0xa9fd9ccc6adc2d23l,
            0xfc2397eccc721b9bl },
          0 },
        /* 9 << 216 */
        { { 0xf031182db48ec57dl,0x515d32f804b233b9l,0x06bbb1d4093aad26l,
            0x88a142fe0d83d1ecl },
          { 0x3b95c099245c73f8l,0xb126d4af52edcd32l,0xf8022c1e8fcb52e6l,
            0x5a51ac4c0106d339l },
          0 },
        /* 10 << 216 */
        { { 0xc589e1ce44ace150l,0xe0f8d3d94381e97cl,0x59e99b1162c5a4b8l,
            0x90d262f7fd0ec9f9l },
          { 0xfbc854c9283e13c9l,0x2d04fde7aedc7085l,0x057d776547dcbecbl,
            0x8dbdf5919a76fa5fl },
          0 },
        /* 11 << 216 */
        { { 0xb7f70a1a7c64a054l,0x0dc1c0df9db43e79l,0x6d0a4ae251fe63d6l,
            0xe0d5e3327f0c8abfl },
          { 0xff5500362b7ecee8l,0x3ea0e6f75d055008l,0x30deb62ff24ac84fl,
            0x936969fd5d7116b7l },
          0 },
        /* 13 << 216 */
        { { 0x02da76122617cf7fl,0xd6e25d4eeee35260l,0xb2fa5b0afd3533e9l,
            0xe76bb7b0b9126f88l },
          { 0x692e6a9988856866l,0x3fdf394f49db65cal,0x2529699122d8d606l,
            0xe815bfbf3dd7c4cfl },
          0 },
        /* 15 << 216 */
        { { 0x69c984ed4d844e7fl,0xd354b2174a2e8a82l,0x25bd4addfb2c4136l,
            0xf72df4de144b26e1l },
          { 0xd0aa9db0e6101afdl,0x4445efaae49bd1b8l,0x5dc54eee331593b2l,
            0xfa35e3b9094bf10bl },
          0 },
        /* 16 << 216 */
        { { 0xdb567d6ac42bd6d2l,0x6df86468bb1f96ael,0x0efe5b1a4843b28el,
            0x961bbb056379b240l },
          { 0xb6caf5f070a6a26bl,0x70686c0d328e6e39l,0x80da06cf895fc8d3l,
            0x804d8810b363fdc9l },
          0 },
        /* 17 << 216 */
        { { 0x660a0f893ea089c3l,0xa25823aac9009b09l,0xb2262d7ba681f5e5l,
            0x4fc30c8c3413863al },
          { 0x691544b7c32059f7l,0xf65cf276b21c6134l,0xe3a96b2a5104dabal,
            0xbb08d109a43ee42fl },
          0 },
        /* 19 << 216 */
        { { 0x85a52d69f9916861l,0x595469a4da4fa813l,0x1dd7786e3338502fl,
            0x34b8ef2853963ac5l },
          { 0xc0f019f81a891b25l,0xb619970c4f4bd775l,0x8c2a5af3be19f681l,
            0x9463db0498ec1728l },
          0 },
        /* 21 << 216 */
        { { 0xeb62c27801f39eabl,0x27de39340ab3a4aal,0xfbd17520a982ca8dl,
            0x58817ec2e4bdc6edl },
          { 0x312d78de31c6ac13l,0x9483bf7609202ea6l,0xf64ab8b622c6d8e1l,
            0xdddf589ce580de74l },
          0 },
        /* 23 << 216 */
        { { 0xe0fa3336ee98a92al,0x7d80eeef66a4d745l,0xb612531bba0119d3l,
            0x86e770c1b351fe15l },
          { 0xafbad6f882d5a397l,0x1e5f1cb80dbf0110l,0x25138ac09f79063dl,
            0x089ed22f2746a156l },
          0 },
        /* 25 << 216 */
        { { 0x198d1b5d7d8b8ddel,0xf32c11078dab37fbl,0xf15fcb6d42b93874l,
            0x91ddb74f41f94f84l },
          { 0x6a64540a271524b2l,0x950a0c12758b5a64l,0xf9f237933dce9580l,
            0xc8edd0ab2cf8ce32l },
          0 },
        /* 27 << 216 */
        { { 0xefc6357eae1046b7l,0xe6704929612932e4l,0xa20305d4b1355b17l,
            0x88a9136a58b4a156l },
          { 0xbc379985b4d275ecl,0x718b91316eaf338bl,0x61229a7ad152a509l,
            0x1109f7c445157ae9l },
          0 },
        /* 28 << 216 */
        { { 0xcf197ca7fb8088fal,0x014272474ddc96c5l,0xa2d2550a30777176l,
            0x534698984d0cf71dl },
          { 0x6ce937b83a2aaac6l,0xe9f91dc35af38d9bl,0x2598ad83c8bf2899l,
            0x8e706ac9b5536c16l },
          0 },
        /* 29 << 216 */
        { { 0x2bde42140df85c2cl,0x4fb839f4058a7a63l,0x7c10572a47f51231l,
            0x878826231989824el },
          { 0xa8293d2016e1564al,0xcb11c0f818c04576l,0x83b91e7d9740c631l,
            0xbdcb23d0cbffcea0l },
          0 },
        /* 31 << 216 */
        { { 0x64bdfd2a9094bfc8l,0x8558acc60fc54d1el,0x3992848faf27721el,
            0x7a8fcbdaa14cd009l },
          { 0x6de6120900a4b9c2l,0xbd192b1b20cf8f28l,0x2356b90168d9be83l,
            0xce1e7a944a49a48al },
          0 },
        /* 33 << 216 */
        { { 0x7630103b6ac189b9l,0x15d35edc6f1f5549l,0x9051799d31cb58edl,
            0xb4f32694a7a8579el },
          { 0x6f037435f2abe306l,0xf0595696410fb2f7l,0x2a0d347a5cc98f59l,
            0x9c19a9a87e3bbd69l },
          0 },
        /* 34 << 216 */
        { { 0x87f8df7c0e58d493l,0xb1ae5ed058b73f12l,0xc368f784dea0c34dl,
            0x9bd0a120859a91a0l },
          { 0xb00d88b7cc863c68l,0x3a1cc11e3d1f4d65l,0xea38e0e70aa85593l,
            0x37f13e987dc4aee8l },
          0 },
        /* 35 << 216 */
        { { 0x91dbe00e49430cd2l,0xcc67c0b17aa8ef6bl,0x769985b8a273f1a5l,
            0x358371dc360e5dafl },
          { 0xbf9b9127d6d8b5e8l,0x748ae12cb45588c1l,0x9c609eb556076c58l,
            0xf287489109733e89l },
          0 },
        /* 36 << 216 */
        { { 0x10d38667bc947badl,0x738e07ce2a36ee2el,0xc93470cdc577fcacl,
            0xdee1b6162782470dl },
          { 0x36a25e672e793d12l,0xd6aa6caee0f186dal,0x474d0fd980e07af7l,
            0xf7cdc47dba8a5cd4l },
          0 },
        /* 37 << 216 */
        { { 0xceb6aa80f8a08fddl,0xd98fc56f46fead7bl,0xe26bd3f8b07b3f1fl,
            0x3547e9b99d361c3el },
          { 0x1a89f802e94b8eccl,0x2210a590c0a40ef2l,0xe7e5b965afc01bf2l,
            0xca3d57fe234b936bl },
          0 },
        /* 39 << 216 */
        { { 0x9230a70db9f9e8cdl,0xa63cebfcb81ba2ecl,0x8482ca87a8f664d6l,
            0xa8ae78e00b137064l },
          { 0xb787bd558384c687l,0xfde1d1bdb29ae830l,0xc4a9b2e39f0b7535l,
            0x7e6c9a15efde2d01l },
          0 },
        /* 40 << 216 */
        { { 0x7d2e5c054f7269b1l,0xfcf30777e287c385l,0x10edc84ff2a46f21l,
            0x354417574f43fa36l },
          { 0xf1327899fd703431l,0xa438d7a616dd587al,0x65c34c57e9c8352dl,
            0xa728edab5cc5a24el },
          0 },
        /* 41 << 216 */
        { { 0xcd6e6db872896d4fl,0x324afa99896c4640l,0x37d18c3d33a292bdl,
            0x98dba3b44143421fl },
          { 0x2406f3c949c61b84l,0x402d974754899588l,0xc73b7fd634a485e5l,
            0x75c9bae08587f0c3l },
          0 },
        /* 43 << 216 */
        { { 0x6c32fa8cb0b4a04dl,0xeb58d0d875fda587l,0x61d8a157c4b86563l,
            0x92191bf01006b8afl },
          { 0xd04d3eff32d3478bl,0x3cc52eab2a684fc8l,0xb19a0f1625de54ccl,
            0x5c5295973620db2dl },
          0 },
        /* 44 << 216 */
        { { 0xa97b51265c3427b0l,0x6401405cd282c9bdl,0x3629f8d7222c5c45l,
            0xb1c02c16e8d50aedl },
          { 0xbea2ed75d9635bc9l,0x226790c76e24552fl,0x3c33f2a365f1d066l,
            0x2a43463e6dfccc2el },
          0 },
        /* 45 << 216 */
        { { 0x09b2e0d3b8da1e01l,0xa3a1a8fee9c0eb04l,0x59af5afe8bf653bal,
            0xba979f8bd0a54836l },
          { 0xa0d8194b51ee6ffbl,0x451c29e2f4b0586cl,0x7eb5fddb7471ee3dl,
            0x84b627d4bcb3afd8l },
          0 },
        /* 46 << 216 */
        { { 0x8cc3453adb483761l,0xe7cc608565d5672bl,0x277ed6cbde3efc87l,
            0x19f2f36869234eafl },
          { 0x9aaf43175c0b800bl,0x1f1e7c898b6da6e2l,0x6cfb4715b94ec75el,
            0xd590dd5f453118c2l },
          0 },
        /* 47 << 216 */
        { { 0xa70e9b0afb54e812l,0x092a0d7d8d86819bl,0x5421ff042e669090l,
            0x8af770c6b133c952l },
          { 0xc8e8dd596c8b1426l,0x1c92eb0e9523b483l,0x5a7c88f2cf3d40edl,
            0x4cc0c04bf5dd98f8l },
          0 },
        /* 48 << 216 */
        { { 0x14e49da11f17a34cl,0x5420ab39235a1456l,0xb76372412f50363bl,
            0x7b15d623c3fabb6el },
          { 0xa0ef40b1e274e49cl,0x5cf5074496b1860al,0xd6583fbf66afe5a4l,
            0x44240510f47e3e9al },
          0 },
        /* 49 << 216 */
        { { 0xb3939a8ffd617288l,0x3d37e5c2d68c2636l,0x4a595fac9d666c0el,
            0xfebcad9edb3a4978l },
          { 0x6d284a49c125016fl,0x05a7b9c80ee246a2l,0xe8b351739436c6e9l,
            0xffb89032d4be40b7l },
          0 },
        /* 51 << 216 */
        { { 0xba1387a5436ebf33l,0xc351a400e8d05267l,0x18645dde4259dbe8l,
            0x5fc32895c10fd676l },
          { 0x1ef7a944807f040el,0x9486b5c625738e5fl,0xc9e56cf4a7e3e96cl,
            0x34c7dc87a20be832l },
          0 },
        /* 52 << 216 */
        { { 0xe10d49996fe8393fl,0x0f809a3fe91f3a32l,0x61096d1c802f63c8l,
            0x289e146257750d3dl },
          { 0xed06167e9889feeal,0xd5c9c0e2e0993909l,0x46fca0d856508ac6l,
            0x918260474f1b8e83l },
          0 },
        /* 53 << 216 */
        { { 0x1d5f2ad7a9bf79cbl,0x228fb24fca9c2f98l,0x5f7c3883701c4b71l,
            0x18cf76c4ec42d686l },
          { 0x3680d2e94dcdec8dl,0x6d58e87ba0d60cb6l,0x72fbf086a0e513cfl,
            0xb922d3c5346ed99al },
          0 },
        /* 55 << 216 */
        { { 0x1678d658c2b9b874l,0x0e0b2c47f6360d4dl,0x01a45c02a0c9b9acl,
            0x05e82e9d0da69afbl },
          { 0x50be4001f28b8018l,0x503d967b667d8241l,0x6cd816534981da04l,
            0x9b18c3117f09c35fl },
          0 },
        /* 57 << 216 */
        { { 0xdfdfd5b409d22331l,0xf445126817f0c6a2l,0xe51d1aa8a5cde27bl,
            0xb61a12a37aaf9513l },
          { 0xe43a241d3b3ea114l,0x5c62b624366ae28dl,0x085a530db5f237eal,
            0x7c4ed375651205afl },
          0 },
        /* 59 << 216 */
        { { 0xf9de879dce842decl,0xe505320a94cedb89l,0xee55dae7f05ad888l,
            0x44ffbfa7f028b4efl },
          { 0xa3c1b32e63b2cd31l,0x201a058910c5ab29l,0x20f930afcd4085d6l,
            0xda79ed169f6ff24bl },
          0 },
        /* 60 << 216 */
        { { 0x7e8cfbcf704e23c6l,0xc71b7d2228aaa65bl,0xa041b2bd245e3c83l,
            0x69b98834d21854ffl },
          { 0x89d227a3963bfeecl,0x99947aaade7da7cbl,0x1d9ee9dbee68a9b1l,
            0x0a08f003698ec368l },
          0 },
        /* 61 << 216 */
        { { 0x04c64f33b0959be5l,0x182332ba396a7fe2l,0x4c5401e302e15b97l,
            0x92880f9877db104bl },
          { 0x0bf0b9cc21726a33l,0x780264741acc7b6dl,0x9721f621a26f08e3l,
            0xe3935b434197fed1l },
          0 },
        /* 63 << 216 */
        { { 0x0bffae503652be69l,0x395a9c6afb3fd5d8l,0x17f66adaa4fadfbfl,
            0x1ee92a35f9268f8cl },
          { 0x40ded34d6827781al,0xcd36224e34e63dccl,0xec90cf571cd1ef7al,
            0xf6067d578f72a3bfl },
          0 },
        /* 64 << 216 */
        { { 0x142b55021a93507al,0xb4cd11878d3c06cfl,0xdf70e76a91ec3f40l,
            0x484e81ad4e7553c2l },
          { 0x830f87b5272e9d6el,0xea1c93e5c6ff514al,0x67cc2adcc4192a8el,
            0xc77e27e242f4535al },
          0 },
        /* 65 << 216 */
        { { 0x537388d299e2f9d2l,0x15ead88612cd6d08l,0x33dfe3a769082d86l,
            0x0ef25f4266d79d40l },
          { 0x8035b4e546ba5cf1l,0x4e48f53711eec591l,0x40b56cda122a7aael,
            0x78e270211dbb79a7l },
          0 },
        /* 71 << 216 */
        { { 0x520b655355b4a5b1l,0xeee835cafb4f5fdel,0xb2ae86e59a823d7fl,
            0x24325f4fc084497fl },
          { 0x542bed4e6f0eefa4l,0x2909233b141792fdl,0x74bfc3bfc847a946l,
            0x8ec1d009e212cb44l },
          0 },
        /* 77 << 216 */
        { { 0xc2082b6d5cedd516l,0xaf148eadeafa3a10l,0x104cd5855ad63aa6l,
            0xe3fdbf8c78c11e1el },
          { 0x78651c493c25c24el,0x8064c4f37b7cce0el,0xa55441d4a6d8a928l,
            0x4525c40eb0db3adcl },
          0 },
        /* 83 << 216 */
        { { 0x5f69e49cfde6001el,0xc61e753aee59b47el,0xd0d4559971b0db5bl,
            0x7f76f7b45ad4acc3l },
          { 0xb0318a9c39830897l,0x2b15da22feef3822l,0x34049400acfb0753l,
            0x16f4fb51a5114ed4l },
          0 },
        /* 89 << 216 */
        { { 0x0b5c76928defbf10l,0xb9f1795cb79cdb6el,0xba17e7759a90317cl,
            0x3cb69cf950cf514bl },
          { 0x076cc4c1e5b892ffl,0x75724e8fb548b73cl,0x2ebcdb33248ff2e6l,
            0x1f12967be109b08fl },
          0 },
        /* 95 << 216 */
        { { 0x3f514c63461b7bb3l,0x3bdca5aa70afbad7l,0x368ce251eab3e38bl,
            0xdc0fb3300d101049l },
          { 0x7ce09abdff5013eel,0x926dd7dd7d10729dl,0xe6fe47ab6f486197l,
            0xd23964eaa6eb6903l },
          0 },
        /* 101 << 216 */
        { { 0x537ceb74eca30797l,0xf171bba557b0f338l,0x220a31fee831f1f8l,
            0xabbc2c7c5ae6bbbcl },
          { 0xaf7609f27eadfb60l,0x22cff1d58f28b51bl,0x63c3d76d6d1863bdl,
            0x3a6a2fb489e8a4c8l },
          0 },
        /* 107 << 216 */
        { { 0x9e74f8beb26e38f0l,0xc4c73fc4ea8bd55bl,0x086f688e1429e1fcl,
            0x91438ff40f78159fl },
          { 0x3571ae5f20810acbl,0x305edafe7451eb00l,0x8443c96d5704385cl,
            0xc03b234e542605b5l },
          0 },
        /* 113 << 216 */
        { { 0x2e5ff4fed85567c2l,0x136f49c7e4abd0c6l,0x5a68730cfb8a62d1l,
            0x101ebfd030bcb848l },
          { 0x634b0618fee950bbl,0xfa748d21c8aa65bal,0xc1d67c3e699f5560l,
            0x6fb0546cb22889d2l },
          0 },
        /* 116 << 216 */
        { { 0xa9784ebd9c95f0f9l,0x5ed9deb224640771l,0x31244af7035561c4l,
            0x87332f3a7ee857del },
          { 0x09e16e9e2b9e0d88l,0x52d910f456a06049l,0x507ed477a9592f48l,
            0x85cb917b2365d678l },
          0 },
        /* 119 << 216 */
        { { 0x6108f2b458a9d40dl,0xb036034838e15a52l,0xcc5610a3fd5625d6l,
            0x79825dd083b0418el },
          { 0xf83a95fc6324b6e5l,0x2463114deedfc4ebl,0x58b177e32250707fl,
            0x778dcd454af8d942l },
          0 },
        /* 125 << 216 */
        { { 0x1ecf2670eb816bf8l,0xa2d6e73aaa6d59c6l,0xf9a11434156852ebl,
            0x9bc9bb70f6f82c83l },
          { 0xd23a018d9c874836l,0xd26bf8bc6db5a8b5l,0x1d648846bec0c624l,
            0x39f15d97ef90302fl },
          0 },
    },
    {
        /* 0 << 224 */
        { { 0x00, 0x00, 0x00, 0x00 },
          { 0x00, 0x00, 0x00, 0x00 },
          1 },
        /* 1 << 224 */
        { { 0xe3417bc035d0b34al,0x440b386b8327c0a7l,0x8fb7262dac0362d1l,
            0x2c41114ce0cdf943l },
          { 0x2ba5cef1ad95a0b1l,0xc09b37a867d54362l,0x26d6cdd201e486c9l,
            0x20477abf42ff9297l },
          0 },
        /* 3 << 224 */
        { { 0x126f35b51e706ad9l,0xb99cebb4c3a9ebdfl,0xa75389afbf608d90l,
            0x76113c4fc6c89858l },
          { 0x80de8eb097e2b5aal,0x7e1022cc63b91304l,0x3bdab6056ccc066cl,
            0x33cbb144b2edf900l },
          0 },
        /* 4 << 224 */
        { { 0xc41764717af715d2l,0xe2f7f594d0134a96l,0x2c1873efa41ec956l,
            0xe4e7b4f677821304l },
          { 0xe5c8ff9788d5374al,0x2b915e6380823d5bl,0xea6bc755b2ee8fe2l,
            0x6657624ce7112651l },
          0 },
        /* 5 << 224 */
        { { 0x157af101dace5acal,0xc4fdbcf211a6a267l,0xdaddf340c49c8609l,
            0x97e49f52e9604a65l },
          { 0x9be8e790937e2ad5l,0x846e2508326e17f1l,0x3f38007a0bbbc0dcl,
            0xcf03603fb11e16d6l },
          0 },
        /* 7 << 224 */
        { { 0x5ed0c007f8ae7c38l,0x6db07a5c3d740192l,0xbe5e9c2a5fe36db3l,
            0xd5b9d57a76e95046l },
          { 0x54ac32e78eba20f2l,0xef11ca8f71b9a352l,0x305e373eff98a658l,
            0xffe5a100823eb667l },
          0 },
        /* 9 << 224 */
        { { 0x5c8ed8d5da64309dl,0x61a6de5691b30704l,0xd6b52f6a2f9b5808l,
            0x0eee419498c958a7l },
          { 0xcddd9aab771e4caal,0x83965dfd78bc21bel,0x02affce3b3b504f5l,
            0x30847a21561c8291l },
          0 },
        /* 10 << 224 */
        { { 0xd2eb2cf152bfda05l,0xe0e4c4e96197b98cl,0x1d35076cf8a1726fl,
            0x6c06085b2db11e3dl },
          { 0x15c0c4d74463ba14l,0x9d292f830030238cl,0x1311ee8b3727536dl,
            0xfeea86efbeaedc1el },
          0 },
        /* 11 << 224 */
        { { 0xb9d18cd366131e2el,0xf31d974f80fe2682l,0xb6e49e0fe4160289l,
            0x7c48ec0b08e92799l },
          { 0x818111d8d1989aa7l,0xb34fa0aaebf926f9l,0xdb5fe2f5a245474al,
            0xf80a6ebb3c7ca756l },
          0 },
        /* 13 << 224 */
        { { 0x8ea610593de9abe3l,0x404348819cdc03bel,0x9b261245cfedce8cl,
            0x78c318b4cf5234a1l },
          { 0x510bcf16fde24c99l,0x2a77cb75a2c2ff5dl,0x9c895c2b27960fb4l,
            0xd30ce975b0eda42bl },
          0 },
        /* 15 << 224 */
        { { 0x09521177ff57d051l,0x2ff38037fb6a1961l,0xfc0aba74a3d76ad4l,
            0x7c76480325a7ec17l },
          { 0x7532d75f48879bc8l,0xea7eacc058ce6bc1l,0xc82176b48e896c16l,
            0x9a30e0b22c750fedl },
          0 },
        /* 16 << 224 */
        { { 0xc37e2c2e421d3aa4l,0xf926407ce84fa840l,0x18abc03d1454e41cl,
            0x26605ecd3f7af644l },
          { 0x242341a6d6a5eabfl,0x1edb84f4216b668el,0xd836edb804010102l,
            0x5b337ce7945e1d8cl },
          0 },
        /* 17 << 224 */
        { { 0xd2075c77c055dc14l,0x2a0ffa2581d89cdfl,0x8ce815ea6ffdcbafl,
            0xa3428878fb648867l },
          { 0x277699cf884655fbl,0xfa5b5bd6364d3e41l,0x01f680c6441e1cb7l,
            0x3fd61e66b70a7d67l },
          0 },
        /* 19 << 224 */
        { { 0xfd5bb657b1fa70fbl,0xfa07f50fd8073a00l,0xf72e3aa7bca02500l,
            0xf68f895d9975740dl },
          { 0x301120605cae2a6al,0x01bd721802874842l,0x3d4238917ce47bd3l,
            0xa66663c1789544f6l },
          0 },
        /* 21 << 224 */
        { { 0xb4b9a39b36194d40l,0xe857a7c577612601l,0xf4209dd24ecf2f58l,
            0x82b9e66d5a033487l },
          { 0xc1e36934e4e8b9ddl,0xd2372c9da42377d7l,0x51dc94c70e3ae43bl,
            0x4c57761e04474f6fl },
          0 },
        /* 23 << 224 */
        { { 0xa39114e24415503bl,0xc08ff7c64cbb17e9l,0x1eff674dd7dec966l,
            0x6d4690af53376f63l },
          { 0xff6fe32eea74237bl,0xc436d17ecd57508el,0x15aa28e1edcc40fel,
            0x0d769c04581bbb44l },
          0 },
        /* 25 << 224 */
        { { 0xfe51d0296ae55043l,0x8931e98f44a87de1l,0xe57f1cc609e4fee2l,
            0x0d063b674e072d92l },
          { 0x70a998b9ed0e4316l,0xe74a736b306aca46l,0xecf0fbf24fda97c7l,
            0xa40f65cb3e178d93l },
          0 },
        /* 27 << 224 */
        { { 0x8667e981c27253c9l,0x05a6aefb92b36a45l,0xa62c4b369cb7bb46l,
            0x8394f37511f7027bl },
          { 0x747bc79c5f109d0fl,0xcad88a765b8cc60al,0x80c5a66b58f09e68l,
            0xe753d451f6127eacl },
          0 },
        /* 28 << 224 */
        { { 0xc44b74a15b0ec6f5l,0x47989fe45289b2b8l,0x745f848458d6fc73l,
            0xec362a6ff61c70abl },
          { 0x070c98a7b3a8ad41l,0x73a20fc07b63db51l,0xed2c2173f44c35f4l,
            0x8a56149d9acc9dcal },
          0 },
        /* 29 << 224 */
        { { 0x98f178819ac6e0f4l,0x360fdeafa413b5edl,0x0625b8f4a300b0fdl,
            0xf1f4d76a5b3222d3l },
          { 0x9d6f5109587f76b8l,0x8b4ee08d2317fdb5l,0x88089bb78c68b095l,
            0x95570e9a5808d9b9l },
          0 },
        /* 31 << 224 */
        { { 0x2e1284943fb42622l,0x3b2700ac500907d5l,0xf370fb091a95ec63l,
            0xf8f30be231b6dfbdl },
          { 0xf2b2f8d269e55f15l,0x1fead851cc1323e9l,0xfa366010d9e5eef6l,
            0x64d487b0e316107el },
          0 },
        /* 33 << 224 */
        { { 0xc9a9513929607745l,0x0ca07420a26f2b28l,0xcb2790e74bc6f9ddl,
            0x345bbb58adcaffc0l },
          { 0xc65ea38cbe0f27a2l,0x67c24d7c641fcb56l,0x2c25f0a7a9e2c757l,
            0x93f5cdb016f16c49l },
          0 },
        /* 34 << 224 */
        { { 0x2ca5a9d7c5ee30a1l,0xd1593635b909b729l,0x804ce9f3dadeff48l,
            0xec464751b07c30c3l },
          { 0x89d65ff39e49af6al,0xf2d6238a6f3d01bcl,0x1095561e0bced843l,
            0x51789e12c8a13fd8l },
          0 },
        /* 35 << 224 */
        { { 0xd633f929763231dfl,0x46df9f7de7cbddefl,0x01c889c0cb265da8l,
            0xfce1ad10af4336d2l },
          { 0x8d110df6fc6a0a7el,0xdd431b986da425dcl,0xcdc4aeab1834aabel,
            0x84deb1248439b7fcl },
          0 },
        /* 36 << 224 */
        { { 0x8796f1693c2a5998l,0x9b9247b47947190dl,0x55b9d9a511597014l,
            0x7e9dd70d7b1566eel },
          { 0x94ad78f7cbcd5e64l,0x0359ac179bd4c032l,0x3b11baaf7cc222ael,
            0xa6a6e284ba78e812l },
          0 },
        /* 37 << 224 */
        { { 0x8392053f24cea1a0l,0xc97bce4a33621491l,0x7eb1db3435399ee9l,
            0x473f78efece81ad1l },
          { 0x41d72fe0f63d3d0dl,0xe620b880afab62fcl,0x92096bc993158383l,
            0x41a213578f896f6cl },
          0 },
        /* 39 << 224 */
        { { 0x6fb4d4e42bad4d5fl,0xfa4c3590fef0059bl,0x6a10218af5122294l,
            0x9a78a81aa85751d1l },
          { 0x04f20579a98e84e7l,0xfe1242c04997e5b5l,0xe77a273bca21e1e4l,
            0xfcc8b1ef9411939dl },
          0 },
        /* 40 << 224 */
        { { 0xe20ea30292d0487al,0x1442dbec294b91fel,0x1f7a4afebb6b0e8fl,
            0x1700ef746889c318l },
          { 0xf5bbffc370f1fc62l,0x3b31d4b669c79ccal,0xe8bc2aaba7f6340dl,
            0xb0b08ab4a725e10al },
          0 },
        /* 41 << 224 */
        { { 0x44f05701ae340050l,0xba4b30161cf0c569l,0x5aa29f83fbe19a51l,
            0x1b9ed428b71d752el },
          { 0x1666e54eeb4819f5l,0x616cdfed9e18b75bl,0x112ed5be3ee27b0bl,
            0xfbf2831944c7de4dl },
          0 },
        /* 43 << 224 */
        { { 0x722eb104e2b4e075l,0x49987295437c4926l,0xb1e4c0e446a9b82dl,
            0xd0cb319757a006f5l },
          { 0xf3de0f7dd7808c56l,0xb5c54d8f51f89772l,0x500a114aadbd31aal,
            0x9afaaaa6295f6cabl },
          0 },
        /* 44 << 224 */
        { { 0x94705e2104cf667al,0xfc2a811b9d3935d7l,0x560b02806d09267cl,
            0xf19ed119f780e53bl },
          { 0xf0227c09067b6269l,0x967b85335caef599l,0x155b924368efeebcl,
            0xcd6d34f5c497bae6l },
          0 },
        /* 45 << 224 */
        { { 0x1dd8d5d36cceb370l,0x2aeac579a78d7bf9l,0x5d65017d70b67a62l,
            0x70c8e44f17c53f67l },
          { 0xd1fc095086a34d09l,0xe0fca256e7134907l,0xe24fa29c80fdd315l,
            0x2c4acd03d87499adl },
          0 },
        /* 46 << 224 */
        { { 0xbaaf75173b5a9ba6l,0xb9cbe1f612e51a51l,0xd88edae35e154897l,
            0xe4309c3c77b66ca0l },
          { 0xf5555805f67f3746l,0x85fc37baa36401ffl,0xdf86e2cad9499a53l,
            0x6270b2a3ecbc955bl },
          0 },
        /* 47 << 224 */
        { { 0xafae64f5974ad33bl,0x04d85977fe7b2df1l,0x2a3db3ff4ab03f73l,
            0x0b87878a8702740al },
          { 0x6d263f015a061732l,0xc25430cea32a1901l,0xf7ebab3ddb155018l,
            0x3a86f69363a9b78el },
          0 },
        /* 48 << 224 */
        { { 0x349ae368da9f3804l,0x470f07fea164349cl,0xd52f4cc98562baa5l,
            0xc74a9e862b290df3l },
          { 0xd3a1aa3543471a24l,0x239446beb8194511l,0xbec2dd0081dcd44dl,
            0xca3d7f0fc42ac82dl },
          0 },
        /* 49 << 224 */
        { { 0x1f3db085fdaf4520l,0xbb6d3e804549daf2l,0xf5969d8a19ad5c42l,
            0x7052b13ddbfd1511l },
          { 0x11890d1b682b9060l,0xa71d3883ac34452cl,0xa438055b783805b4l,
            0x432412774725b23el },
          0 },
        /* 51 << 224 */
        { { 0x40b08f7443b30ca8l,0xe10b5bbad9934583l,0xe8a546d6b51110adl,
            0x1dd50e6628e0b6c5l },
          { 0x292e9d54cff2b821l,0x3882555d47281760l,0x134838f83724d6e3l,
            0xf2c679e022ddcda1l },
          0 },
        /* 52 << 224 */
        { { 0x40ee88156d2a5768l,0x7f227bd21c1e7e2dl,0x487ba134d04ff443l,
            0x76e2ff3dc614e54bl },
          { 0x36b88d6fa3177ec7l,0xbf731d512328fff5l,0x758caea249ba158el,
            0x5ab8ff4c02938188l },
          0 },
        /* 53 << 224 */
        { { 0x33e1605635edc56dl,0x5a69d3497e940d79l,0x6c4fd00103866dcbl,
            0x20a38f574893cdefl },
          { 0xfbf3e790fac3a15bl,0x6ed7ea2e7a4f8e6bl,0xa663eb4fbc3aca86l,
            0x22061ea5080d53f7l },
          0 },
        /* 55 << 224 */
        { { 0x635a8e5ec3a0ee43l,0x70aaebca679898ffl,0x9ee9f5475dc63d56l,
            0xce987966ffb34d00l },
          { 0xf9f86b195e26310al,0x9e435484382a8ca8l,0x253bcb81c2352fe4l,
            0xa4eac8b04474b571l },
          0 },
        /* 57 << 224 */
        { { 0x2617f91c93aa96b8l,0x0fc8716b7fca2e13l,0xa7106f5e95328723l,
            0xd1c9c40b262e6522l },
          { 0xb9bafe8642b7c094l,0x1873439d1543c021l,0xe1baa5de5cbefd5dl,
            0xa363fc5e521e8affl },
          0 },
        /* 59 << 224 */
        { { 0xbc00fc2f2f8ba2c7l,0x0966eb2f7c67aa28l,0x13f7b5165a786972l,
            0x3bfb75578a2fbba0l },
          { 0x131c4f235a2b9620l,0xbff3ed276faf46bel,0x9b4473d17e172323l,
            0x421e8878339f6246l },
          0 },
        /* 60 << 224 */
        { { 0x0fa8587a25a41632l,0xc0814124a35b6c93l,0x2b18a9f559ebb8dbl,
            0x264e335776edb29cl },
          { 0xaf245ccdc87c51e2l,0x16b3015b501e6214l,0xbb31c5600a3882cel,
            0x6961bb94fec11e04l },
          0 },
        /* 61 << 224 */
        { { 0x3b825b8deff7a3a0l,0xbec33738b1df7326l,0x68ad747c99604a1fl,
            0xd154c9349a3bd499l },
          { 0xac33506f1cc7a906l,0x73bb53926c560e8fl,0x6428fcbe263e3944l,
            0xc11828d51c387434l },
          0 },
        /* 63 << 224 */
        { { 0x659b17c8d8ceb147l,0x9b649eeeb70a5554l,0x6b7fa0b5ac6bc634l,
            0xd99fe2c71d6e732fl },
          { 0x30e6e7628d3abba2l,0x18fee6e7a797b799l,0x5c9d360dc696464dl,
            0xe3baeb4827bfde12l },
          0 },
        /* 64 << 224 */
        { { 0x2bf5db47f23206d5l,0x2f6d34201d260152l,0x17b876533f8ff89al,
            0x5157c30c378fa458l },
          { 0x7517c5c52d4fb936l,0xef22f7ace6518cdcl,0xdeb483e6bf847a64l,
            0xf508455892e0fa89l },
          0 },
        /* 65 << 224 */
        { { 0xf77bb113a74ed3bel,0x89e4eb8f074f2637l,0x7fbfa84df7ce2aebl,
            0xe7c6ecd5baaefe4cl },
          { 0x176bba7df6319542l,0x70098120f6080799l,0x2e2118339054d9aal,
            0x1be4c6a78295a912l },
          0 },
        /* 71 << 224 */
        { { 0x6bb4d8c35df1455fl,0xb839f08f0384b033l,0x718868af11f95d50l,
            0xae256a92e07a8801l },
          { 0xa5bafaf24d71a273l,0x18ff04ea2a30e68fl,0x364c193287ba727el,
            0x4bb8cf99befcaf73l },
          0 },
        /* 77 << 224 */
        { { 0xc79f5b1f4e9fb3d7l,0x52854970a51cccddl,0xa4e27e97f00054a3l,
            0x26a79792240e1232l },
          { 0xb15579fecb5ff465l,0x6ef54c3bd1722a84l,0xee211bfa5239a4d8l,
            0x36c7db27270b7059l },
          0 },
        /* 83 << 224 */
        { { 0x5e7da0a9f9858cd3l,0x67459de5b633de49l,0x2db0d54b2e73892el,
            0x37f50877adae399al },
          { 0x83c28b83b65e6179l,0xae5a915ca39faf17l,0x6ab8f3fbe841b53cl,
            0x7c30997b0df7d004l },
          0 },
        /* 89 << 224 */
        { { 0x87904ca7b3b862bdl,0x7593db93cf9ea671l,0x8a2670f8739aa783l,
            0x3921d779f5154ca6l },
          { 0xe81ca56468f65ebbl,0x0c600603bc4e64d4l,0xdf170049cb83b2d1l,
            0x373893b863487064l },
          0 },
        /* 95 << 224 */
        { { 0x7c3c52b9c0c4e88el,0x0f0484d06f0c2446l,0xeb876827000fe87bl,
            0xa749b3136d20f94al },
          { 0x0876dae9d55abda6l,0xe6e4367620726911l,0xf85e8a8c4a2676b4l,
            0x4e8c97f1b4a890ebl },
          0 },
        /* 101 << 224 */
        { { 0xa992f482a3c0a4f4l,0xe1536f3f7a8d961al,0x26fc79ae000752b0l,
            0xdbfb706b76ad8508l },
          { 0x2642b2ed6f4cf9e4l,0xa013db54557fa7e2l,0x2ef711821d326116l,
            0x8dc3f5bcbafc83ecl },
          0 },
        /* 107 << 224 */
        { { 0x9671258578e5a201l,0xc71aca1de9125569l,0x360c45c0e2231379l,
            0x2d71783512e82369l },
          { 0x392432d3d84b2153l,0x502fd3f6d6939ffel,0x33c440ae6e766cacl,
            0x99f1fbee28062416l },
          0 },
        /* 113 << 224 */
        { { 0xe51ad841861604cbl,0x1ec9c54f630283a7l,0xcc42cad582a39473l,
            0xa2eb053709929c4al },
          { 0xe374459767f655a3l,0x9f54c2451d7f2674l,0xd85e9163fbc8aba5l,
            0x12fd0b55866bc892l },
          0 },
        /* 116 << 224 */
        { { 0x4f2c3063d7bd4661l,0xe533798d57a974ccl,0x44860d503ea02d85l,
            0xf2a7f4e5acaa0521l },
          { 0x05593061abb108f0l,0x56d1056044528309l,0x1f674df9c88b6d1el,
            0x19fdc4cbd8744c4dl },
          0 },
        /* 119 << 224 */
        { { 0xfd1488ec00f2f1d5l,0x24fcc67b44a825ddl,0xc7bfae2ea925a0f4l,
            0x5e03249cad59cf48l },
          { 0x1dc5a8e11af4844cl,0x89b2fbc58a598c20l,0xb0f56afff2078121l,
            0x8194012d4878bb0dl },
          0 },
        /* 125 << 224 */
        { { 0xc1cbe9d3a5ae1031l,0x38da74435706b987l,0x01844b55b353f188l,
            0x390c59ca87a807c5l },
          { 0x55ac7b1fb13b780cl,0x060970bff375c1cbl,0x8dd1f378c7ab4e5cl,
            0xcca782e5cf726645l },
          0 },
    },
    {
        /* 0 << 232 */
        { { 0x00, 0x00, 0x00, 0x00 },
          { 0x00, 0x00, 0x00, 0x00 },
          1 },
        /* 1 << 232 */
        { { 0x91213462f23f2d92l,0x6cab71bd60b94078l,0x6bdd0a63176cde20l,
            0x54c9b20cee4d54bcl },
          { 0x3cd2d8aa9f2ac02fl,0x03f8e617206eedb0l,0xc7f68e1693086434l,
            0x831469c592dd3db9l },
          0 },
        /* 3 << 232 */
        { { 0x4a9090cde36d0757l,0xf722d7b1d9a29382l,0xfb7fb04c04b48ddfl,
            0x628ad2a7ebe16f43l },
          { 0xcd3fbfb520226040l,0x6c34ecb15104b6c4l,0x30c0754ec903c188l,
            0xec336b082d23cab0l },
          0 },
        /* 4 << 232 */
        { { 0x9f51439e558df019l,0x230da4baac712b27l,0x518919e355185a24l,
            0x4dcefcdd84b78f50l },
          { 0xa7d90fb2a47d4c5al,0x55ac9abfb30e009el,0xfd2fc35974eed273l,
            0xb72d824cdbea8fafl },
          0 },
        /* 5 << 232 */
        { { 0xd213f923cbb13d1bl,0x98799f425bfb9bfel,0x1ae8ddc9701144a9l,
            0x0b8b3bb64c5595eel },
          { 0x0ea9ef2e3ecebb21l,0x17cb6c4b3671f9a7l,0x47ef464f726f1d1fl,
            0x171b94846943a276l },
          0 },
        /* 7 << 232 */
        { { 0xc9941109a607419dl,0xfaa71e62bb6bca80l,0x34158c1307c431f3l,
            0x594abebc992bc47al },
          { 0x6dfea691eb78399fl,0x48aafb353f42cba4l,0xedcd65af077c04f0l,
            0x1a29a366e884491al },
          0 },
        /* 9 << 232 */
        { { 0x7bf6a5c1f7ea25aal,0xd165e6bffbb07d5fl,0xe353936189e78671l,
            0xa3fcac892bac4219l },
          { 0xdfab6fd4f0baa8abl,0x5a4adac1e2c1c2e5l,0x6cd75e3140d85849l,
            0xce263fea19b39181l },
          0 },
        /* 10 << 232 */
        { { 0xb8d804a3315980cdl,0x693bc492fa3bebf7l,0x3578aeee2253c504l,
            0x158de498cd2474a2l },
          { 0x1331f5c7cfda8368l,0xd2d7bbb378d7177el,0xdf61133af3c1e46el,
            0x5836ce7dd30e7be8l },
          0 },
        /* 11 << 232 */
        { { 0xe042ece59a29a5c5l,0xb19b3c073b6c8402l,0xc97667c719d92684l,
            0xb5624622ebc66372l },
          { 0x0cb96e653c04fa02l,0x83a7176c8eaa39aal,0x2033561deaa1633fl,
            0x45a9d0864533df73l },
          0 },
        /* 13 << 232 */
        { { 0xa29ae9df5ece6e7cl,0x0603ac8f0facfb55l,0xcfe85b7adda233a5l,
            0xe618919fbd75f0b8l },
          { 0xf555a3d299bf1603l,0x1f43afc9f184255al,0xdcdaf341319a3e02l,
            0xd3b117ef03903a39l },
          0 },
        /* 15 << 232 */
        { { 0xb6b82fa74d82f4c2l,0x90725a606804efb3l,0xbc82ec46adc3425el,
            0xb7b805812787843el },
          { 0xdf46d91cdd1fc74cl,0xdc1c62cbe783a6c4l,0x59d1b9f31a04cbbal,
            0xd87f6f7295e40764l },
          0 },
        /* 16 << 232 */
        { { 0x196860411e84e0e5l,0xa5db84d3aea34c93l,0xf9d5bb197073a732l,
            0xb8d2fe566bcfd7c0l },
          { 0x45775f36f3eb82fal,0x8cb20cccfdff8b58l,0x1659b65f8374c110l,
            0xb8b4a422330c789al },
          0 },
        /* 17 << 232 */
        { { 0xa6312c9e8977d99bl,0xbe94433183f531e7l,0x8232c0c218d3b1d4l,
            0x617aae8be1247b73l },
          { 0x40153fc4282aec3bl,0xc6063d2ff7b8f823l,0x68f10e583304f94cl,
            0x31efae74ee676346l },
          0 },
        /* 19 << 232 */
        { { 0xd98bf2a43734e520l,0x5e3abbe3209bdcbal,0x77c76553bc945b35l,
            0x5331c093c6ef14aal },
          { 0x518ffe2976b60c80l,0x2285593b7ace16f8l,0xab1f64ccbe2b9784l,
            0xe8f2c0d9ab2421b6l },
          0 },
        /* 21 << 232 */
        { { 0x481dae5fd5ecfefcl,0x07084fd8c2bff8fcl,0x8040a01aea324596l,
            0x4c646980d4de4036l },
          { 0x9eb8ab4ed65abfc3l,0xe01cb91f13541ec7l,0x8f029adbfd695012l,
            0x9ae284833c7569ecl },
          0 },
        /* 23 << 232 */
        { { 0xc83605f6f10ff927l,0xd387145123739fc6l,0x6d163450cac1c2ccl,
            0x6b521296a2ec1ac5l },
          { 0x0606c4f96e3cb4a5l,0xe47d3f41778abff7l,0x425a8d5ebe8e3a45l,
            0x53ea9e97a6102160l },
          0 },
        /* 25 << 232 */
        { { 0x6b72fab526bc2797l,0x13670d1699f16771l,0x001700521e3e48d1l,
            0x978fe401b7adf678l },
          { 0x55ecfb92d41c5dd4l,0x5ff8e247c7b27da5l,0xe7518272013fb606l,
            0x5768d7e52f547a3cl },
          0 },
        /* 27 << 232 */
        { { 0x0e966e64c73b2383l,0x49eb3447d17d8762l,0xde1078218da05dabl,
            0x443d8baa016b7236l },
          { 0x163b63a5ea7610d6l,0xe47e4185ce1ca979l,0xae648b6580baa132l,
            0xebf53de20e0d5b64l },
          0 },
        /* 28 << 232 */
        { { 0x6ba535da9a85788bl,0xd21f03aebd0626d4l,0x099f8c47e873dc64l,
            0xcda8564d018ec97el },
          { 0x3e8d7a5cde92c68cl,0x78e035a173323cc4l,0x3ef26275f880ff7cl,
            0xa4ee3dff273eedaal },
          0 },
        /* 29 << 232 */
        { { 0x8bbaec49571d92acl,0x569e85fe4692517fl,0x8333b014a14ea4afl,
            0x32f2a62f12e5c5adl },
          { 0x98c2ce3a06d89b85l,0xb90741aa2ff77a08l,0x2530defc01f795a2l,
            0xd6e5ba0b84b3c199l },
          0 },
        /* 31 << 232 */
        { { 0x3d1b24cb28c682c6l,0x27f252288612575bl,0xb587c779e8e66e98l,
            0x7b0c03e9405eb1fel },
          { 0xfdf0d03015b548e7l,0xa8be76e038b36af7l,0x4cdab04a4f310c40l,
            0x6287223ef47ecaecl },
          0 },
        /* 33 << 232 */
        { { 0x0a4c6f3670ad54aal,0xc24cfd0d2a543909l,0xe1b0bc5b745c1a97l,
            0xb8431cfd68f0ddbfl },
          { 0x326357989ed8cb06l,0xa00a80ff759d2b7dl,0x81f335c190570e02l,
            0xbfccd89849c4e4d9l },
          0 },
        /* 34 << 232 */
        { { 0x4dcb646bfd16d8c4l,0x76a6b640e38ba57bl,0xd92de1f79d8ae7e2l,
            0x126f48f13f77f23bl },
          { 0xb7b53ca977e8abc2l,0x3faa17112c0787ffl,0xf8f9308c8e5762f8l,
            0x600a8a7f6b83aea8l },
          0 },
        /* 35 << 232 */
        { { 0xa2aed4a799aa03c0l,0x1f93b93da18b79c5l,0x7b4550b7314192c3l,
            0x9da00676272bb08el },
          { 0xe42f0d7e23e072edl,0x7ce76494888b5783l,0x4c7900203680b63bl,
            0x6040c83f662a8718l },
          0 },
        /* 36 << 232 */
        { { 0xba9e5c88a56d73edl,0x6c24f7712ca054d3l,0x4a37c235083beae1l,
            0x04a883b26483e9fdl },
          { 0x0c63f3aee27c2c5dl,0x0e1da88dae4671f1l,0xa577e8e25995e1dbl,
            0xbfc4b1b16ed6066al },
          0 },
        /* 37 << 232 */
        { { 0x8b398541f53d9e63l,0x4ab045bb019395cbl,0x69a1b90371dd70c7l,
            0xdedf284b38aaa431l },
          { 0xb45e245aaed3efe7l,0x49460905079f2facl,0xde4dee470845bd78l,
            0x0540524039d02ec3l },
          0 },
        /* 39 << 232 */
        { { 0x300cf051675cc986l,0x758afea99324219fl,0xf524c3fad5a93b5fl,
            0xb73385abc3864a8al },
          { 0xbde19289f6be9050l,0xbb9018558205a3f3l,0x99a9d14d229f6b89l,
            0x4c3a802f4336e68fl },
          0 },
        /* 40 << 232 */
        { { 0xdd4a12d8e12b31f8l,0x577e29bc177736e6l,0x2353722ba88935e8l,
            0xca1d3729015f286dl },
          { 0x86c7b6a239a3e035l,0x6e5250bfd3b03a9fl,0x79d98930fd0d536el,
            0x8c4cbbabfa0c3832l },
          0 },
        /* 41 << 232 */
        { { 0x92ecff374f8e6163l,0x171cc8830f35faeal,0xc5434242bcd36142l,
            0x707049adb28b63bbl },
          { 0xa1f4d1dbf6443da9l,0x002bb062dabc108bl,0x17287f171a272b08l,
            0x2a3aac8c884cf6bbl },
          0 },
        /* 43 << 232 */
        { { 0x55524645651c0a5al,0x14624a9703cf0d12l,0xca9315a8f884a9e2l,
            0x9840c6e2df7c9d59l },
          { 0xd96bd10a7438e8d5l,0x12be73d2b2f887del,0x5e47445dca2493efl,
            0x85aef555e9fff03el },
          0 },
        /* 44 << 232 */
        { { 0x169b38c9a43b2339l,0x884308d91732bfabl,0xe4b593a28ff202ddl,
            0xaf51d11f1e65376cl },
          { 0x6ec648de741525ffl,0xf93cbd369ff4c628l,0xc76df9efb1129c79l,
            0x31a5f2e2b7a67294l },
          0 },
        /* 45 << 232 */
        { { 0x0661bc02801d0e38l,0x4a37dc0e71fc46b7l,0x0b224cfc80c3e311l,
            0x2dd3d2779646a957l },
          { 0xfa45aa18ef524012l,0x5d2a2d0916185a09l,0x34d5c630b5313dcel,
            0xd9581ed151e4cf84l },
          0 },
        /* 46 << 232 */
        { { 0x5845aa4a8ebd2af8l,0x141404ecd3df43ccl,0xff3fc7681ffd48d9l,
            0x8a096e72e0cefb65l },
          { 0xc9c81cfdffc3a5cdl,0x7550aa3029b27cf9l,0x34dca72b65fa0380l,
            0xe8c5f6059ddd032bl },
          0 },
        /* 47 << 232 */
        { { 0xe53da8a46bfbadb3l,0x4a9dfa55afaeeb5el,0x076245ea6644b1d4l,
            0xc19be4012307bbcbl },
          { 0x097774c19d77318bl,0xacc8a1519cfd51c4l,0x736ef6b3ecaa7b08l,
            0x107479132d643a80l },
          0 },
        /* 48 << 232 */
        { { 0x2d500910cab91f1el,0xbedd9e444d1cd216l,0xd634b74fedd02252l,
            0xbd60f8e11258617al },
          { 0xd8c7537b9e05614al,0xfd26c766e7af5fc5l,0x0660b581582bd926l,
            0x87019244acf07fc8l },
          0 },
        /* 49 << 232 */
        { { 0xd4889fdf6220ae8el,0x745d67ec1abf1549l,0x957b2e3d2fb89c36l,
            0x9768c90edc62ada9l },
          { 0x90332fd748e6c46el,0x5aa5a4e54e90ef0dl,0x58838fd3ddcc8571l,
            0xd12f6c6f9a721126l },
          0 },
        /* 51 << 232 */
        { { 0x2f0fd0b2cec757bal,0x46a7a9c63032cd1dl,0x9af3a600547d7a77l,
            0x828e16eca43da1bal },
          { 0x0b303a66092a8d92l,0x78ba0389c23d08bal,0x52aed08d4616bd29l,
            0x4c0ff1210539c9fal },
          0 },
        /* 52 << 232 */
        { { 0x2c3b7322badcfe8el,0x6e0616fac5e25a04l,0x0a3c12753da6e4a2l,
            0xe46c957e077bca01l },
          { 0xb46ca4e3da4be64bl,0xa59bda668e75ee78l,0x41835184a4de98f2l,
            0x6efb1f924ed6a568l },
          0 },
        /* 53 << 232 */
        { { 0xbb8cdc094af1dd72l,0x93c0aa38a2460633l,0xf66f5d238a7ebc93l,
            0x43ecda843e8e37a6l },
          { 0x399da8265fd5139el,0x8b39930fd446f38el,0x114414135d2b68efl,
            0x8be163b8d1637c38l },
          0 },
        /* 55 << 232 */
        { { 0x488e2a35b70ddbd3l,0xb4aa5f718da50077l,0xb38b74b1d8752bbdl,
            0x7007f328416106a3l },
          { 0xe6a62e4fcec4ea68l,0x9fdfb79741ef920bl,0x1a19d7dfe3c337a6l,
            0x08f643558be0f586l },
          0 },
        /* 57 << 232 */
        { { 0x91a5d8ff60343a1fl,0x921e442173ef8cdfl,0x4358f27b975138cdl,
            0x36fd8577a4992b08l },
          { 0xc07c8ca1f8d044c6l,0xcf42903687747b6bl,0x0932ffb0867c8632l,
            0x7e565213250e5a89l },
          0 },
        /* 59 << 232 */
        { { 0xae7c3b9b06255feal,0x2eb1d9a78a6fe229l,0xf81548e77601e6f8l,
            0x777394eb7bd96d6cl },
          { 0x54734187000a3509l,0xaeec146492d43c04l,0xc9b7f0d7c428b4acl,
            0x9d4bcedccd7f7018l },
          0 },
        /* 60 << 232 */
        { { 0x4741b9b311370605l,0x47fa72f75d09b355l,0x391a71ac7a144c6al,
            0x0808c0f498b6e3cal },
          { 0x7eaed9ef7fe53900l,0xf157a2a5e5a830bal,0xd13ec09127974afcl,
            0x78d710a70b87997dl },
          0 },
        /* 61 << 232 */
        { { 0xcbb96ecb4e263f81l,0x093e0d1509084351l,0x7af3232629220a81l,
            0xd721b415c60f36dcl },
          { 0xe3340a87fe9387a1l,0x6088bf482ff2b126l,0xd31028f1d2bc982cl,
            0x9794e106630d52cbl },
          0 },
        /* 63 << 232 */
        { { 0x1dac76780b11e972l,0x46e814c62698dafel,0x553f7370c37640d6l,
            0xdcf588cc51cede93l },
          { 0x4d6b56d3c3f6215bl,0x07edc6621b8f8f03l,0xdfef9d60b9a5dfbcl,
            0x377edf4d10af7a5bl },
          0 },
        /* 64 << 232 */
        { { 0x8928e99aeeaf8c49l,0xee7aa73d6e24d728l,0x4c5007c2e72b156cl,
            0x5fcf57c5ed408a1dl },
          { 0x9f719e39b6057604l,0x7d343c01c2868bbfl,0x2cca254b7e103e2dl,
            0xe6eb38a9f131bea2l },
          0 },
        /* 65 << 232 */
        { { 0x26ae28bede7a4b7el,0xd2f07569d2664163l,0x798690d4ff69266al,
            0x77093d356ef3695dl },
          { 0xaca9903d567dd3dfl,0x259c59a3a274c67bl,0x9f34bc0bfc1198b0l,
            0x51a7726290b1521cl },
          0 },
        /* 71 << 232 */
        { { 0xa20644bc80ca5391l,0xf9cdb4f7e5b36ea3l,0xe7936c0641426e22l,
            0x39bc23033eef8a52l },
          { 0x31253f43e5d8f896l,0xb0e5a588dc3df499l,0x1d03519a2d7e66d5l,
            0x923de91f6d7da5e3l },
          0 },
        /* 77 << 232 */
        { { 0x17a833ffedf861e4l,0x0ee3d0af4ebec965l,0xd0fac1c1ea66870el,
            0x325756d0ae810cf4l },
          { 0x4ed78d2c78e9a415l,0x6cc65685192046e4l,0x03e4243d8498a91el,
            0x56a02dd25ab97794l },
          0 },
        /* 83 << 232 */
        { { 0xc2fd373748e2b156l,0x259e9a98139645bel,0xe90106fb9877b4f1l,
            0x49e5bac5889ce002l },
          { 0x936a7dd18cf14e0bl,0x70bf6d304e3a8a01l,0x99d3e8bfeb748b62l,
            0xa52a27c99b31c55cl },
          0 },
        /* 89 << 232 */
        { { 0x9db1d41d300637d5l,0xe38744397c2dd836l,0x36179baf0d04ceb3l,
            0xe9ccd17b251b3f2dl },
          { 0xd8228073442b6d1dl,0x59a038363eed2971l,0xb443732046979f5cl,
            0x54ad4113ae63937cl },
          0 },
        /* 95 << 232 */
        { { 0x092c34e6d9246e9fl,0xb4b3b63d3eeb18a7l,0x8b3778beed9d1383l,
            0xe4cb7be9d70d5d80l },
          { 0xcff12e9b3d059203l,0x277af117ba86699fl,0x9bd4e8e363603585l,
            0x0750b0f28e89c8d5l },
          0 },
        /* 101 << 232 */
        { { 0x38b77e5958f7187bl,0x31c7068de0cb618el,0xa0f8e0d6c11ebe62l,
            0x07adc8010473d7ebl },
          { 0x36161a2c5c3e9510l,0xb2ec90d64ad04815l,0x01e2dd1f917d8166l,
            0x549bcbdd6aa0f794l },
          0 },
        /* 107 << 232 */
        { { 0x4ab27c3a8e4e45e5l,0xf6bd9d82f2bb99e7l,0xcab48c735e9da59fl,
            0xdeb09eb2b9727353l },
          { 0xc4a7954bafb8fa3el,0x34af2a49abf6803dl,0xc1ee1416d63e13bbl,
            0xd49bf42d7a949193l },
          0 },
        /* 113 << 232 */
        { { 0x504823ea9c9c07c6l,0x9dbec902bee2288cl,0x018d7875f0ceb6bbl,
            0x678b997304f7022cl },
          { 0x74d658238c5fb369l,0x7d4e1f114ca89ee8l,0x148316399905abc0l,
            0xc107324e2c4deff4l },
          0 },
        /* 116 << 232 */
        { { 0x1bc4fa8bdadc4404l,0x0edb9534daa12ee3l,0x084481b6a5f7289cl,
            0x7f42461d9d8fb3d2l },
          { 0xf93f1d3212293c70l,0xc14706596bb73ea3l,0xf80834afde339cadl,
            0x99dcfc0081f22953l },
          0 },
        /* 119 << 232 */
        { { 0x497e544f9fca737el,0x7f6342210e91e1afl,0x638e500c78d7b20bl,
            0xb1ffed3f7ebaa947l },
          { 0x751aa54871086f83l,0x8100bb703cf97848l,0xc32f91ace19ad68fl,
            0x7dffb6851fb9157el },
          0 },
        /* 125 << 232 */
        { { 0x5108589778e25060l,0x33e3cb7316cfe6cbl,0x0884cb8d410c0822l,
            0xaa806ecc0be3fc94l },
          { 0x9f9121f5f692353el,0xb9ab0310f8ee3349l,0x390032ce2561973el,
            0xc07b6c6c8856b766l },
          0 },
    },
    {
        /* 0 << 240 */
        { { 0x00, 0x00, 0x00, 0x00 },
          { 0x00, 0x00, 0x00, 0x00 },
          1 },
        /* 1 << 240 */
        { { 0x1083e2ea1f095615l,0x0a28ad7714e68c33l,0x6bfc02523d8818bel,
            0xb585113af35850cdl },
          { 0x7d935f0b30df8aa1l,0xaddda07c4ab7e3acl,0x92c34299552f00cbl,
            0xc33ed1de2909df6cl },
          0 },
        /* 3 << 240 */
        { { 0xabe7905a83cdd60el,0x50602fb5a1170184l,0x689886cdb023642al,
            0xd568d090a6e1fb00l },
          { 0x5b1922c70259217fl,0x93831cd9c43141e4l,0xdfca35870c95f86el,
            0xdec2057a568ae828l },
          0 },
        /* 4 << 240 */
        { { 0x568f8925913cc16dl,0x18bc5b6de1a26f5al,0xdfa413bef5f499ael,
            0xf8835decc3f0ae84l },
          { 0xb6e60bd865a40ab0l,0x65596439194b377el,0xbcd8562592084a69l,
            0x5ce433b94f23ede0l },
          0 },
        /* 5 << 240 */
        { { 0x860d523d42e06189l,0xbf0779414e3aff13l,0x0b616dcac1b20650l,
            0xe66dd6d12131300dl },
          { 0xd4a0fd67ff99abdel,0xc9903550c7aac50dl,0x022ecf8b7c46b2d7l,
            0x3333b1e83abf92afl },
          0 },
        /* 7 << 240 */
        { { 0xefecdef7be42a582l,0xd3fc608065046be6l,0xc9af13c809e8dba9l,
            0x1e6c9847641491ffl },
          { 0x3b574925d30c31f7l,0xb7eb72baac2a2122l,0x776a0dacef0859e7l,
            0x06fec31421900942l },
          0 },
        /* 9 << 240 */
        { { 0x7ec62fbbf4737f21l,0xd8dba5ab6209f5acl,0x24b5d7a9a5f9adbel,
            0x707d28f7a61dc768l },
          { 0x7711460bcaa999eal,0xba7b174d1c92e4ccl,0x3c4bab6618d4bf2dl,
            0xb8f0c980eb8bd279l },
          0 },
        /* 10 << 240 */
        { { 0x9d658932790691bfl,0xed61058906b736ael,0x712c2f04c0d63b6el,
            0x5cf06fd5c63d488fl },
          { 0x97363facd9588e41l,0x1f9bf7622b93257el,0xa9d1ffc4667acacel,
            0x1cf4a1aa0a061ecfl },
          0 },
        /* 11 << 240 */
        { { 0x28d675b2c0519a23l,0x9ebf94fe4f6952e3l,0xf28bb767a2294a8al,
            0x85512b4dfe0af3f5l },
          { 0x18958ba899b16a0dl,0x95c2430cba7548a7l,0xb30d1b10a16be615l,
            0xe3ebbb9785bfb74cl },
          0 },
        /* 13 << 240 */
        { { 0x81eeb865d2fdca23l,0x5a15ee08cc8ef895l,0x768fa10a01905614l,
            0xeff5b8ef880ee19bl },
          { 0xf0c0cabbcb1c8a0el,0x2e1ee9cdb8c838f9l,0x0587d8b88a4a14c0l,
            0xf6f278962ff698e5l },
          0 },
        /* 15 << 240 */
        { { 0x9c4b646e9e2fce99l,0x68a210811e80857fl,0x06d54e443643b52al,
            0xde8d6d630d8eb843l },
          { 0x7032156342146a0al,0x8ba826f25eaa3622l,0x227a58bd86138787l,
            0x43b6c03c10281d37l },
          0 },
        /* 16 << 240 */
        { { 0x02b37a952f41deffl,0x0e44a59ae63b89b7l,0x673257dc143ff951l,
            0x19c02205d752baf4l },
          { 0x46c23069c4b7d692l,0x2e6392c3fd1502acl,0x6057b1a21b220846l,
            0xe51ff9460c1b5b63l },
          0 },
        /* 17 << 240 */
        { { 0x7aca2632f02fc0f0l,0xb92b337dc7f01c86l,0x624bc4bf5afbdc7dl,
            0x812b07bc4de21a5el },
          { 0x29d137240b2090ccl,0x0403c5095a1b2132l,0x1dca34d50e35e015l,
            0xf085ed7d3bbbb66fl },
          0 },
        /* 19 << 240 */
        { { 0xc27b98f9f781e865l,0x51e1f692994e1345l,0x0807d516e19361eel,
            0x13885ceffb998aefl },
          { 0xd223d5e92f0f8a17l,0x48672010e8d20280l,0x6f02fd60237eac98l,
            0xcc51bfad9ada7ee7l },
          0 },
        /* 21 << 240 */
        { { 0x2756bcdd1e09701dl,0x94e31db990d45c80l,0xb9e856a98566e584l,
            0x4f87d9deab10e3f3l },
          { 0x166ecb373ded9cb2l,0xfd14c7073f653d3el,0x105d049b92aec425l,
            0x7f657e4909a42e11l },
          0 },
        /* 23 << 240 */
        { { 0xea6490076a159594l,0x3e424d6b1f97ce52l,0xac6df30a185e8ccbl,
            0xad56ec80517747bfl },
          { 0xf0935ccf4391fe93l,0x866b260f03811d40l,0x792047b99f7b9abel,
            0xb1600bc88ee42d84l },
          0 },
        /* 25 << 240 */
        { { 0x2d97b3db7768a85fl,0x2b78f6334287e038l,0x86c947676f892bb1l,
            0x920bfb1ac0a9c200l },
          { 0x4292f6ec332041b2l,0xa30bb937c9989d54l,0x39f941ebc6d5879el,
            0x76a450fcdfdbb187l },
          0 },
        /* 27 << 240 */
        { { 0x31256089ee430db6l,0xaece9bd8f6836f56l,0x484cfc4bfb85a046l,
            0xee1e3e2c1599b2b9l },
          { 0x7e3c38903d122eafl,0xaa940ce0c770556cl,0x4802d6631b08fae8l,
            0xb08a85807f69f8bal },
          0 },
        /* 28 << 240 */
        { { 0x70ed0a0405411eael,0x60deb08f16494c66l,0x8cf20fc6133797bbl,
            0x3e30f4f50c6bc310l },
          { 0x1a677c29749c46c7l,0xfe1d93f4f11e981cl,0x937303d82e3e688bl,
            0x01aef5a7a6aa9e85l },
          0 },
        /* 29 << 240 */
        { { 0x4902f495b959b920l,0x13b0fdbdfca2d885l,0x41cbd9e7b6a2f0fal,
            0xf9bdf11056430b87l },
          { 0xd705a223954d19b9l,0x74d0fc5c972a4fdel,0xcbcbfed6912977eal,
            0x870611fdcc59a5afl },
          0 },
        /* 31 << 240 */
        { { 0xf4f19bd04089236al,0x3b206c12313d0e0bl,0x73e70df303feaeb2l,
            0x09dba0eb9bd1efe0l },
          { 0x4c7fd532fc4e5305l,0xd792ffede93d787al,0xc72dc4e2e4245010l,
            0xe7e0d47d0466bbbdl },
          0 },
        /* 33 << 240 */
        { { 0x549c861983e4f8bbl,0xf70133fbd8e06829l,0xc962b8e28c64e849l,
            0xad87f5b1901e4c25l },
          { 0xd005bde568a1cab5l,0x6a591acf0d2a95bal,0x728f14ce30ebcae4l,
            0x303cec99a3459b0fl },
          0 },
        /* 34 << 240 */
        { { 0x62e62f258350e6bcl,0x5a5ea94d96adba1fl,0x36c2a2844a23c7b3l,
            0x32f50a72992f5c8bl },
          { 0x55d685204136c6afl,0x1aafd32992794f20l,0x69f5d820b59aa9bfl,
            0x218966a8570e209al },
          0 },
        /* 35 << 240 */
        { { 0xf3204feb2f9a31fcl,0x77f33a360429f463l,0xfb9f3a5a59a1d6a7l,
            0x4445a2e93b1a78e0l },
          { 0xc77a9b6fd58e32d3l,0xa44e23c8302e6390l,0x7d8e00b4c0f7bcb0l,
            0xd2e2237b0ffa46f4l },
          0 },
        /* 36 << 240 */
        { { 0xb3046cb13c8ea6d3l,0xf0151b5efce2f445l,0xa968e60b55e5715el,
            0x39e52662587dce61l },
          { 0xfde176e0b7de2862l,0x298d83e68e8db497l,0x1042136773641bfbl,
            0xd72ac78d36e0bb0dl },
          0 },
        /* 37 << 240 */
        { { 0x2cabb94fff6b8340l,0xf425a35a21771acbl,0x564fec3d12c4a758l,
            0x57a61af39ba8f281l },
          { 0x5807e78c97e9a71dl,0x991d9be75b8314e6l,0x1cd90b16ec4133b9l,
            0xff043efa0f1ac621l },
          0 },
        /* 39 << 240 */
        { { 0xea6e5527d7e58321l,0xfb95c13c04056ff1l,0x9447361f2fc4e732l,
            0x63cbc655786d0154l },
          { 0x302c0d668610fb71l,0xbf692d6920d06613l,0x8465b74b4be8355al,
            0xcc883c95c31356b7l },
          0 },
        /* 40 << 240 */
        { { 0x4ab6e919b33eabcal,0xb58f0998a1acacbfl,0xa747e5782ddbc28fl,
            0xf9dd04ca59866cbcl },
          { 0x084c062ff7a0073fl,0x6d22acdfb577fc38l,0x0870ee08eacd907cl,
            0x710b4b266c9fcf95l },
          0 },
        /* 41 << 240 */
        { { 0xa99546faf1c835a7l,0x1514a5a30d59f933l,0x1f6ad0f81bedd730l,
            0x24de76287b528aaal },
          { 0x4d9e7845c02fff87l,0xba74f8a942c79e67l,0x5bf5015f476e285bl,
            0x0b1a5d8b1b93b364l },
          0 },
        /* 43 << 240 */
        { { 0x8c7c0d7ff839819fl,0xc82b819827a95965l,0xce7294d377270519l,
            0xfb508d6cad47aff7l },
          { 0xf6de15431035076al,0x697d60ac5dd465c6l,0x88d771b8a76dcd26l,
            0x8c7ce11ab10c9c44l },
          0 },
        /* 44 << 240 */
        { { 0x215ea44a08216060l,0xccfa18a187996cf6l,0xccfb2483f7eccdd2l,
            0x07aa601ad453c66al },
          { 0xd43cf263cffee9e2l,0x230bc099718f69bfl,0xc43de21300c193e8l,
            0x94cf251799c8746fl },
          0 },
        /* 45 << 240 */
        { { 0x4785d7f87d1320c5l,0x84bed8c3d0771dcbl,0xff28044d22254edbl,
            0x2e5992a445f71504l },
          { 0xcb92695b72bbf5cdl,0x9bcbde35c42422e5l,0x856594fd1d07ed86l,
            0x3aaf0b717716b4ffl },
          0 },
        /* 46 << 240 */
        { { 0x3edf24f9eebed405l,0x9e3141360eccb503l,0xf7704c25b85c2bc2l,
            0x4cb7c1de9a3247eel },
          { 0x798ac8f2f0b507c5l,0x6e6217206851bbf1l,0xc0b89398c0d9ed16l,
            0xf7d5d2a09f20728fl },
          0 },
        /* 47 << 240 */
        { { 0x7358a94a19f0ededl,0x5e08c4c3e32ccfbbl,0x84a8eeeb0089f071l,
            0xdaf0514c41fc436el },
          { 0x30fe216f310309afl,0xe72f77bd564e6fc9l,0xe7ef3bddfdc59fd5l,
            0xd199b1c9a8e1169cl },
          0 },
        /* 48 << 240 */
        { { 0xb9dc857c5b0f7bd4l,0x6990c2c9108ea1cdl,0x84730b83b984c7a9l,
            0x552723d2eab18a78l },
          { 0x9752c2e2919ba0f9l,0x075a3bd94bf40890l,0x71e52a04a6d98212l,
            0x3fb6607a9f18a4c8l },
          0 },
        /* 49 << 240 */
        { { 0xa0305d01e8c3214dl,0x025b3cae8d51cea3l,0xeeaf7ab239923274l,
            0x51179407c876b72cl },
          { 0xcf0241c7d4549a68l,0xffae7f4c793dab3dl,0xdfb5917b4bdf2280l,
            0xcf25c870a652e391l },
          0 },
        /* 51 << 240 */
        { { 0xb1345466b922e1c8l,0xae42f46ab5bf8a34l,0x1e1ab6053310e604l,
            0x64093cd9b4d7a658l },
          { 0x5d3b385ab3d9242cl,0x2225b99ae56f8ec7l,0x19a8cbfc9a916e11l,
            0x11c5df831f957c03l },
          0 },
        /* 52 << 240 */
        { { 0x09f1d04af381147bl,0x7be13628b26b345fl,0xd8371966d1c60b78l,
            0xf1743c2c5d91808fl },
          { 0x8a2966acafc71cc3l,0x0ba9702efdfc24c3l,0x60c80158e6fbb539l,
            0x58eaee49812c32f4l },
          0 },
        /* 53 << 240 */
        { { 0x31af7f5ee89d0b84l,0xa776dada6caa110bl,0xd67b7891df6d54ddl,
            0x831613cab82b8a5cl },
          { 0x7a4eb86ef020af6dl,0x2914fd11bd795a7bl,0xc038a273fcb54a17l,
            0x6b2dc8e18219cc75l },
          0 },
        /* 55 << 240 */
        { { 0x031fc875464ba9b5l,0xe268cf45bd812dd3l,0x443f57defbfb664al,
            0xfd1a38544e28c2fal },
          { 0xb8799782cb96515bl,0xa12d3e3f1138c95dl,0x0cc5ee117748ee57l,
            0x6ab167cf955a7dfcl },
          0 },
        /* 57 << 240 */
        { { 0x0d54aaca4dc1c74fl,0x74af1807bf2e0d61l,0x151254f87aebe0f1l,
            0x4072f38bf6376095l },
          { 0x31ebe17a26646abfl,0xdc8cb6b40ecc1282l,0x4f6326bbbc095a66l,
            0x37dad65a0363636dl },
          0 },
        /* 59 << 240 */
        { { 0xc851860a70f8c15al,0xb2d4555488368381l,0xbfd46e197019c7b6l,
            0xa1a9b12f6bb6f33bl },
          { 0xecfd5fe6f170c82bl,0x6d58bb52d601afc3l,0xb8b3de15fe6eb102l,
            0xad07336886a47964l },
          0 },
        /* 60 << 240 */
        { { 0x89f514c91911840fl,0xc9fa6b504cc106bcl,0x70a97f0dfe55b4f1l,
            0xada6306be5888609l },
          { 0xa9437881c6dc8d15l,0x0fc0f5368411f3dfl,0xd26162087a913dd2l,
            0x4fe1c7c4e92848cdl },
          0 },
        /* 61 << 240 */
        { { 0xaa18eb262e07383dl,0xb948c35c34e90f3dl,0x95e97f81d3653565l,
            0x4a821a2687b5b75dl },
          { 0x87b4d81c892db882l,0xa69e65d689f3bfadl,0xe475f532eb371cacl,
            0xd8cc23fa17194d5dl },
          0 },
        /* 63 << 240 */
        { { 0x3fc0052ad789d484l,0xe8c67aac29324323l,0x133fd07cf54c43d3l,
            0xd4a0848fb91d4faal },
          { 0xf683ce065ea5098fl,0xe84348f9887c8a76l,0x38f8c2cf79b224b6l,
            0x327e4c534a818cb1l },
          0 },
        /* 64 << 240 */
        { { 0xb6d92a7f3e5f9f11l,0x9afe153ad6cb3b8el,0x4d1a6dd7ddf800bdl,
            0xf6c13cc0caf17e19l },
          { 0x15f6c58e325fc3eel,0x71095400a31dc3b2l,0x168e7c07afa3d3e7l,
            0x3f8417a194c7ae2dl },
          0 },
        /* 65 << 240 */
        { { 0x0c9e9237d5f812bcl,0xdae5b7e9595f02e5l,0x5ec1dece42b1e9a8l,
            0x506a6ef8e527a685l },
          { 0xe3049290236af251l,0x6322dd1bf81970acl,0x1459d39c516d5e61l,
            0x672f502d9455b694l },
          0 },
        /* 71 << 240 */
        { { 0xf83788e06b228af2l,0xaafc823911f596fal,0x6d47fa592f0fcb13l,
            0x0b7af65f1c99c5d4l },
          { 0xbc4c185dca961e6fl,0xec02b09f158481a4l,0x4bbfd9f31423fdd4l,
            0x0ff44a53b619644bl },
          0 },
        /* 77 << 240 */
        { { 0x23e255a3ea3f59d8l,0x1f4a47a8261ac30bl,0x346bf409c8faf0b3l,
            0xd13e73fbc03a226bl },
          { 0x670ddc792fe8a79bl,0x335fa172f1aac412l,0xe2347de1a5ceff20l,
            0x66e02c73381130f2l },
          0 },
        /* 83 << 240 */
        { { 0xa6b874c51db717cdl,0x027d318ab00f160bl,0x578f89f49be791afl,
            0x659ef2f01f3b5e9bl },
          { 0xa0c593033835d84cl,0xb71e261fdb6f9a60l,0x65837c7f44b7813fl,
            0xea776163ea4bcc96l },
          0 },
        /* 89 << 240 */
        { { 0x208234118df3f15fl,0xe0514d4694f341acl,0xdc66282d6486d704l,
            0xd5fb354ad2548389l },
          { 0xf3e98d72df273295l,0x27ded7fa50cd09fcl,0x4f486af3c5c1c169l,
            0xe51044150aa41ba3l },
          0 },
        /* 95 << 240 */
        { { 0x66b14d296fce0aecl,0x35fe5e60c8915ceal,0x06a023b736c5da39l,
            0x0977c9f0404e932fl },
          { 0x1dd6f95db54866del,0xe5ec79359387430cl,0x98dee57b5ef42e67l,
            0x1707f01912ed3ad0l },
          0 },
        /* 101 << 240 */
        { { 0xeb3abdedeec82495l,0x587a696e764a41c7l,0x13fdcce2add1a6a3l,
            0x299a0d43286b2162l },
          { 0x2c4e71e18131f1b4l,0x48f0e806ada3d04fl,0x91d2de80c57491b2l,
            0x1b1266236cc355cbl },
          0 },
        /* 107 << 240 */
        { { 0xdc28afe5a6d44444l,0xb5ad8d3cfe0b947bl,0x50c6126c96ce9fb9l,
            0x5384a998d1fc7d39l },
          { 0xa43ff8898788f51cl,0x30359593a6bc7b87l,0x3e1691dccc0d019al,
            0xda0ef5ad7943abcdl },
          0 },
        /* 113 << 240 */
        { { 0x5bc58b6f020b5cd7l,0x9098e202e103ff4el,0xc1f1a3d9f6fce7c7l,
            0xf9dc32a856090ccel },
          { 0x4c7d2520a9cc3b09l,0x98d47b5dd8c4dfcel,0xdcee788297e689b4l,
            0xe5eec71815f982b9l },
          0 },
        /* 116 << 240 */
        { { 0xff154bb8a1e1538cl,0xb9883276f7dcfae9l,0x1ac0a4d2c1c8cba4l,
            0x511a54cc76e6b284l },
          { 0xe2da436f00011f6dl,0x4d357a190f43a8adl,0xf36899c95458655bl,
            0xe5f75c768d613ed9l },
          0 },
        /* 119 << 240 */
        { { 0x15b4af1d93f12ef8l,0x3f4c5868fd032f88l,0x39f67a08f27d86bdl,
            0x2f551820da32db6bl },
          { 0x72fe295ac2c16214l,0x39927c381a2cf9afl,0x8dda23d6b1dc1ae7l,
            0x1209ff3ed32071d4l },
          0 },
        /* 125 << 240 */
        { { 0x861fdceb9a3c6c6fl,0x76d7a01386778453l,0xbf8d147cd5e422cbl,
            0xd16f532e51772d19l },
          { 0x72025ee2570d02cdl,0xe8e7737be80c7664l,0x81b7d56c334a8d8fl,
            0x42477a0ff1b79308l },
          0 },
    },
    {
        /* 0 << 248 */
        { { 0x00, 0x00, 0x00, 0x00 },
          { 0x00, 0x00, 0x00, 0x00 },
          1 },
        /* 1 << 248 */
        { { 0xf306a3c8ee3c76cbl,0x3cf11623d32a1f6el,0xe6d5ab646863e956l,
            0x3b8a4cbe5c005c26l },
          { 0xdcd529a59ce6bb27l,0xc4afaa5204d4b16fl,0xb0624a267923798dl,
            0x85e56df66b307fabl },
          0 },
        /* 3 << 248 */
        { { 0x896895959884aaf7l,0xb1959be307b348a6l,0x96250e573c147c87l,
            0xae0efb3add0c61f8l },
          { 0xed00745eca8c325el,0x3c911696ecff3f70l,0x73acbc65319ad41dl,
            0x7b01a020f0b1c7efl },
          0 },
        /* 4 << 248 */
        { { 0x9910ba6b23a5d896l,0x1fe19e357fe4364el,0x6e1da8c39a33c677l,
            0x15b4488b29fd9fd0l },
          { 0x1f4392541a1f22bfl,0x920a8a70ab8163e8l,0x3fd1b24907e5658el,
            0xf2c4f79cb6ec839bl },
          0 },
        /* 5 << 248 */
        { { 0x262143b5224c08dcl,0x2bbb09b481b50c91l,0xc16ed709aca8c84fl,
            0xa6210d9db2850ca8l },
          { 0x6d8df67a09cb54d6l,0x91eef6e0500919a4l,0x90f613810f132857l,
            0x9acede47f8d5028bl },
          0 },
        /* 7 << 248 */
        { { 0x45e21446de673629l,0x57f7aa1e703c2d21l,0xa0e99b7f98c868c7l,
            0x4e42f66d8b641676l },
          { 0x602884dc91077896l,0xa0d690cfc2c9885bl,0xfeb4da333b9a5187l,
            0x5f789598153c87eel },
          0 },
        /* 9 << 248 */
        { { 0xb19b1c4fca66eca8l,0xf04a20b55663de54l,0x42a29a33c223b617l,
            0x86c68d0d44827e11l },
          { 0x71f90ddeadba1206l,0xeeffb4167a6ceeeal,0x9e302fbac543e8afl,
            0xcf07f7471aa77b96l },
          0 },
        /* 10 << 248 */
        { { 0xcf57fca29849e95bl,0x96e9793ed510053cl,0x89fa443d07d3e75el,
            0xfe2bc235e52800a0l },
          { 0x1c208b8c0ac7e740l,0xb5852a49e7222263l,0x217e4005e541e592l,
            0xee52747dc960b0e1l },
          0 },
        /* 11 << 248 */
        { { 0x5fd7cafb475952afl,0x23a6d71954a43337l,0xa83a7523b1617941l,
            0x0b7f35d412b37dd4l },
          { 0x81ec51292ae27eafl,0x7ca92fb3318169dfl,0xc01bfd6078d0875al,
            0xcc6074e3c99c436el },
          0 },
        /* 13 << 248 */
        { { 0x4ca6bdebf57912b8l,0x9a17577e98507b5al,0x8ed4ab7759e51dfcl,
            0x103b7b2a470f5a36l },
          { 0x0c8545ac12553321l,0xab5861a760482817l,0xf4b5f602b9b856cfl,
            0x609955787adf2e5fl },
          0 },
        /* 15 << 248 */
        { { 0x60ce25b1ee5cb44fl,0xddcc7d182c2d7598l,0x1765a1b301847b5cl,
            0xf5d9c3635d0d23b7l },
          { 0x42ff1ba7928b65d0l,0x587ac69d6148e043l,0x3099be0dd320390bl,
            0xa7b88dfc4278329fl },
          0 },
        /* 16 << 248 */
        { { 0x80802dc91ec34f9el,0xd8772d3533810603l,0x3f06d66c530cb4f3l,
            0x7be5ed0dc475c129l },
          { 0xcb9e3c1931e82b10l,0xc63d2857c9ff6b4cl,0xb92118c692a1b45el,
            0x0aec44147285bbcal },
          0 },
        /* 17 << 248 */
        { { 0x7685bb9e0ba4e0b7l,0x330a7ebc5e58c29bl,0xbc1d9173e8a3797al,
            0x7c506a16ea60f86cl },
          { 0x9defb9248c099445l,0xcf1ddcc0256df210l,0x4844ce293d07e990l,
            0x92318e37e2628503l },
          0 },
        /* 19 << 248 */
        { { 0x61acd597fdf968d7l,0x7321a8b26598c381l,0xcb86a2809f448a0cl,
            0x38534a01855df66al },
          { 0xc119ec141e29037fl,0xe23c20ad0b42ba67l,0xefb1c4e033fb4f22l,
            0xf088358f445a5032l },
          0 },
        /* 21 << 248 */
        { { 0x2d73f5d1b8475744l,0xcc297e0a9d399b06l,0xa8c61d4038d3df06l,
            0xacc6e8651a2d27a0l },
          { 0x63dd6f6230153bf2l,0x6b23ad7bd73b83b7l,0x25382bf767ff7dcdl,
            0x7e268c8fcf7ce2d1l },
          0 },
        /* 23 << 248 */
        { { 0x4b9161c3cb2ebef1l,0x6009716b669ed801l,0x97c65219aacefe44l,
            0xde13597d71aae4b5l },
          { 0x3a077a816141d651l,0xe1b4e80129f876eal,0x729aed6d5c00c96cl,
            0x0c6f404374cc645el },
          0 },
        /* 25 << 248 */
        { { 0x22c51812df5a66e1l,0x1c8069c9ae7dedeal,0xcff9d86f0eea5180l,
            0x676dbd6f44235ddal },
          { 0xa53f01383db1ad42l,0xd079e571bcf19029l,0x1e37b9ecfab0cf82l,
            0x93ae35ed4844e9c4l },
          0 },
        /* 27 << 248 */
        { { 0xdaee55a543756358l,0x0ace18d41b2d3f89l,0x3391fa36824dd7d4l,
            0x7b9963d1770e5f3fl },
          { 0xc1fb9a78c94f724dl,0x94ff86fe76c4da6bl,0xb5d928c64170609bl,
            0xc9372becfb015a9fl },
          0 },
        /* 28 << 248 */
        { { 0x9c34b650e16e05e9l,0x965a774094e74640l,0xa3fd22fbcea3f029l,
            0x1eb6a9688f95277cl },
          { 0x2520a63d7bad84f6l,0xad917201f58f2feel,0xea92c1669b840d48l,
            0x12109c4aacef5cbdl },
          0 },
        /* 29 << 248 */
        { { 0xd85850d0d407a252l,0x6fa3b14de63909d4l,0x2ff9f6593e0fba69l,
            0x7f9fd2a2d1b2cd0bl },
          { 0x611233d745ad896al,0xfe4211648df850f9l,0x7808832399e32983l,
            0x4b040859dee6741dl },
          0 },
        /* 31 << 248 */
        { { 0x7dd2afd456e1ed5cl,0xd48429ec41ba4992l,0x97a02188968bab27l,
            0x09ecf813e63c4168l },
          { 0xf4ac65e77288b10cl,0x10630ab2afac7410l,0x4e3e59c3bb049e56l,
            0x25972fff40fea0b1l },
          0 },
        /* 33 << 248 */
        { { 0xfd8363da98365c18l,0x8aa57b1a8d47bf91l,0x423dce57695f4dd6l,
            0xfccf54d4cc17f034l },
          { 0x8fdba27c3610ea51l,0xcc0a06d654306b06l,0xb97a121c389b9dfdl,
            0x7dbb90eb1ed0ca42l },
          0 },
        /* 34 << 248 */
        { { 0xd32d7cec0094e84cl,0x862ae25e2ece8f72l,0x8644ef1cdfceb8abl,
            0x68a9969c8e225628l },
          { 0xdf209e27b3117876l,0x308a6e1882ba242bl,0xcbd09a659bf0cdb6l,
            0x79f2826cc85b9705l },
          0 },
        /* 35 << 248 */
        { { 0x3b36b6bf8f011496l,0xea6acc1a9bcf6ef8l,0x6db132263b101f12l,
            0x4fc4e35e3b7585c3l },
          { 0x641de27556eb64c6l,0x9b2834d3f3b08519l,0xebb76a2ba1f44b40l,
            0x1b545ccd3cd31677l },
          0 },
        /* 36 << 248 */
        { { 0xab293027aad991c1l,0x598d0bf8849be4b7l,0x8c94a21ab972da90l,
            0xada4cfdd7ecfa840l },
          { 0x93d4b9c0fbcec63al,0x7ca617a203219a34l,0x900424eb6a652a55l,
            0xaf9346e9eb8562e0l },
          0 },
        /* 37 << 248 */
        { { 0x9681a73d2d8bc904l,0x8b5f9b317b1553bel,0xfb03b874f6bc852fl,
            0x8e658fb8cbbec8b0l },
          { 0x9b2ff17bb9e9f9d1l,0xf46e9bf3e8679854l,0x7fbb1323618ed3aal,
            0x064a1c5d714ebc3dl },
          0 },
        /* 39 << 248 */
        { { 0xac0bdfc39f0e69dcl,0x71957386ae12f132l,0xa263ef2e6aa90b5bl,
            0xa94b152390d42976l },
          { 0xfb2d17741bcdbf7bl,0xba77b77c3a04f72fl,0xa6818ed8ec3e25a1l,
            0x2e0e01743733e251l },
          0 },
        /* 40 << 248 */
        { { 0xc3e04d7902381461l,0xb1643ab5911bc478l,0xc92becfa390b3ef2l,
            0x54476778acd2f1b6l },
          { 0x8daa0c4d66bf3aafl,0x2bc1287b2c21c65al,0xee182910b5a13ac3l,
            0xbb04730090b0790al },
          0 },
        /* 41 << 248 */
        { { 0x8bdd6f35a8540489l,0x788c03e5ee390d4el,0x203323c18f653017l,
            0x39953308c4bc0094l },
          { 0x6ee0857118308d0bl,0x70e9f90b450b0002l,0x191662aa8139f145l,
            0xd7c5415b62d71124l },
          0 },
        /* 43 << 248 */
        { { 0x41b37d72b927231cl,0xca17b5429e4de13al,0x7bc03469cded2ce3l,
            0x961b0ecb4f4560f9l },
          { 0x7c5bd41b43d31fa1l,0x3ed047f643f44dc3l,0x5b02083efe1a4d14l,
            0xcc2c66ac18b330bcl },
          0 },
        /* 44 << 248 */
        { { 0x83766947d17d4e0bl,0xc5772beefdc3a47bl,0x765a50db1a6fd0ffl,
            0x17f904ba45b0995el },
          { 0xcee643832883487el,0xf56db7f3c270aaedl,0x6738d94f46cb1fd9l,
            0xc8fa426a142fd4d5l },
          0 },
        /* 45 << 248 */
        { { 0xc85bef5b5a78efcel,0xaf380c6b0580e41el,0x6c093256a43b8d9bl,
            0xed9d07bbea670933l },
          { 0xfdb9a295f1682c6el,0x4cc29a63532b6bb7l,0x21a918f9f8e42dd0l,
            0x9ac935ce0edacca0l },
          0 },
        /* 46 << 248 */
        { { 0xbfe48a8ff43daf9dl,0xd7799b31b313c052l,0x46d480d77119c60el,
            0x5090d91f0b80bcb9l },
          { 0xc94c4c1e873bd7bfl,0x16e69b4f9915aa0al,0x769be02bb1d5928cl,
            0x3fdaf62162e1d85al },
          0 },
        /* 47 << 248 */
        { { 0x03497a57371c1b5cl,0x11e4c0b3552ab6abl,0xf857061f0a169ee7l,
            0xc21c6c43e6d1bc66l },
          { 0x706283a82832be7al,0xd35b143299aba62cl,0x7f4da83de9aef62dl,
            0x2b7e5fc8723fa4e5l },
          0 },
        /* 48 << 248 */
        { { 0xae485bb72b724759l,0x945353e1b2d4c63al,0x82159d07de7d6f2cl,
            0x389caef34ec5b109l },
          { 0x4a8ebb53db65ef14l,0x2dc2cb7edd99de43l,0x816fa3ed83f2405fl,
            0x73429bb9c14208a3l },
          0 },
        /* 49 << 248 */
        { { 0xc086e737eb4cfa54l,0x9400e1ad3c44aad9l,0x210bba94336959b4l,
            0x08621a809106f0cal },
          { 0x2ae66096c510ee9cl,0x2ba21617fc76a895l,0xc0707f8b0c186f1el,
            0x1fe170a3ed0bfe25l },
          0 },
        /* 51 << 248 */
        { { 0x3780fe2084759c5cl,0x716ec626b7050aa7l,0x6a43fb8b84b63bd1l,
            0xb01098a039bc449fl },
          { 0x96b3ff8ebb7daa4dl,0x2d146882654a7f01l,0x2500f701dcae6143l,
            0xc13d51d01626fd3bl },
          0 },
        /* 52 << 248 */
        { { 0x08ed8febd56daf06l,0x8d98277b4a837f69l,0x9947c636a9b6e05al,
            0x58c8a77ac0d58abdl },
          { 0xf45496a45f121e4fl,0x16cd67c71076d3d3l,0xecbd1958e3fb0c5dl,
            0xfbe185ec38e1eb47l },
          0 },
        /* 53 << 248 */
        { { 0x65b067eb740216e3l,0x1e19a71479db8760l,0x8d30dca18878de5al,
            0x627d03e8aa47c005l },
          { 0x096d58c0d2536c96l,0x232e6a4d69b12c2al,0x850eb8c0e7044bcel,
            0xd9cf923bef2ee9a1l },
          0 },
        /* 55 << 248 */
        { { 0x8b301094c8eaee90l,0x9a96950b8330928fl,0x472ba105faccc3bal,
            0x00f8620e9153172al },
          { 0x019b8164303fcdf5l,0x614d5c3c41fb4c73l,0x632d98f2c5992f89l,
            0xfbeb29d790e2dea5l },
          0 },
        /* 57 << 248 */
        { { 0xefd48b577f91d6e0l,0x8575605595bcf5d4l,0x7677b4a7bb9d891bl,
            0xdc9931e9685912c9l },
          { 0x69bca306f31a07c8l,0x3dd729534962a7f0l,0xdcea49cc9d366c2al,
            0xce664ba7dc79a57dl },
          0 },
        /* 59 << 248 */
        { { 0x7842d547013ec3b5l,0xa2785ceb433cf990l,0x9d667e5f700ab14al,
            0x4b46f362a0f46d55l },
          { 0x152c0e80cc7a3487l,0x7f3a88cef86f5e68l,0x6f950a73f1b2a75fl,
            0x9be5b1aa51d24f3bl },
          0 },
        /* 60 << 248 */
        { { 0xaea68626dc4ad4f4l,0x5dc516824ddbc0b6l,0xa76697bd602e9065l,
            0xbeeb3ea58c37888el },
          { 0x1ec4a2f214569113l,0xe48b820ca35f4484l,0x9fb560949ae44df2l,
            0x6ca1346292cc09fdl },
          0 },
        /* 61 << 248 */
        { { 0x887e0b87bcdc3a36l,0x6b0d617d503dee65l,0x96bda1f6cebcb893l,
            0xdc0dd17341e20b3el },
          { 0x812fbacfa6657c11l,0x32492fcbc94a6f4bl,0x854a0bcb6a772123l,
            0x1ed573f65d463f31l },
          0 },
        /* 63 << 248 */
        { { 0x22c7ef7bd022cc4dl,0xeec383d61e63b4bcl,0x52e0aaa06502b46fl,
            0x9224187ded5e41bfl },
          { 0x3a01f53dd26faf1cl,0x9bc4ee2e4e591d10l,0x10b7a98eea7e4c88l,
            0xe521c150e2c1beccl },
          0 },
        /* 64 << 248 */
        { { 0xb618d590b01e6e27l,0x047e2ccde180b2dcl,0xd1b299b504aea4a9l,
            0x412c9e1e9fa403a4l },
          { 0x88d28a3679407552l,0x49c50136f332b8e3l,0x3a1b6fcce668de19l,
            0x178851bc75122b97l },
          0 },
        /* 65 << 248 */
        { { 0x26f9b9322ed53a71l,0x0bac7348c72ef2e0l,0x7e96001da5c6faf1l,
            0x5d43f76dea00eb2dl },
          { 0x1327370f44f1c478l,0x1c83a9ac6bb964c8l,0xa3a9769f76ffbd25l,
            0xdf045fb6b04f1bddl },
          0 },
        /* 71 << 248 */
        { { 0x4283898d556b975el,0x6e2301ffe3880361l,0xc6d3b2bbe9198077l,
            0xc4799578d21cac02l },
          { 0x11448ff8f784eb7cl,0xb775973fbb81898dl,0x4e51f061519c76b9l,
            0xaba1f3ef3cad0393l },
          0 },
        /* 77 << 248 */
        { { 0x59d60c1c9b339830l,0x5af60a44ac32746dl,0x5ac006bc9dea8d80l,
            0x4a2a56d97f2b1180l },
          { 0x2032845a46946fc4l,0xe25b911226a3b503l,0xfed89db9a28827d3l,
            0xdd2d7e90c6b74593l },
          0 },
        /* 83 << 248 */
        { { 0x9b047a26cda38ecfl,0x6889284f5f6cb442l,0x4d128bcb14753820l,
            0x8f9937c160eedd78l },
          { 0xe333bad751ab9127l,0xd31b01c67ace3b19l,0x0732de39d7c0b4bel,
            0xad04fa4c649e2b9bl },
          0 },
        /* 89 << 248 */
        { { 0x02e042689d1495bal,0x95dca5a85591b5f8l,0xb10488d856f46c71l,
            0x97829baf3590000al },
          { 0xaeda5cb378c9e78al,0x3615873a7ba1c71cl,0x7c9f9f4d4333aa12l,
            0x893fab42cea8e6d3l },
          0 },
        /* 95 << 248 */
        { { 0x9eb09fff69aaa09fl,0xf36678a926731322l,0x8be61ee1cafcabafl,
            0x77a172f558ddb763l },
          { 0x7e09dfc66471130el,0x7f8909791039771el,0x0e44071d37800b9bl,
            0x09123d27fe762d10l },
          0 },
        /* 101 << 248 */
        { { 0xffd455a7a1b7fdd6l,0xb6162cb4dabdffael,0xf859519ec89c0e56l,
            0x07406c1b421f2846l },
          { 0x42db24ed9e96ddbbl,0x03bcae092dc5da85l,0x75099cd217aa7493l,
            0x8cd1aa4266b8740dl },
          0 },
        /* 107 << 248 */
        { { 0xe94333d5dde7fec3l,0x894fd673745a9be3l,0xaf3d97c725683748l,
            0xeaa469a2c9ec165fl },
          { 0xc9a18decdc7abd3bl,0xf059008082717b02l,0x9816374a4fdf4300l,
            0x449d3eb74fb5a6cel },
          0 },
        /* 113 << 248 */
        { { 0x7fc983ebd28001a6l,0xeabf5276dae74b6bl,0x50adb67d742ed0a5l,
            0x1d2ad363650e1446l },
          { 0x5a564253d122f5d0l,0x7e5aefc7e30471del,0xdc64cbb3e5dc2f2cl,
            0xe645b9fa9437be4el },
          0 },
        /* 116 << 248 */
        { { 0x0f58cec54e27d357l,0x08dcf2b70004539el,0xb1ead64104f96709l,
            0x350fed185a914c72l },
          { 0x44f43523c5147854l,0x45f8b46f46d04ac7l,0x62c306869a449d51l,
            0xaacc0f0d9e66d9a3l },
          0 },
        /* 119 << 248 */
        { { 0x94cb62e5bdd61b63l,0xe6ce5b5104a0ec57l,0x0461cb95f0bda8a4l,
            0xca2d6220cbadfe8fl },
          { 0x6c19bdf03c1ad65el,0x774a49bae04239d5l,0xf78cb7404a2fd59dl,
            0xaebf90ed66a09130l },
          0 },
        /* 125 << 248 */
        { { 0x10e4074857cc8d54l,0x29985831918e3cf9l,0x3d87def9f2e344eel,
            0x8899992c68977860l },
          { 0xbdc8d73b210f3c50l,0x98aa042fa9857f46l,0x76a34daf6c71357fl,
            0x086289d3200bcb6dl },
          0 },
    },
    {
        /* 0 << 256 */
        { { 0x00, 0x00, 0x00, 0x00 },
          { 0x00, 0x00, 0x00, 0x00 },
          1 },
        /* 1 << 256 */
        { { 0xb4e370af3aeac968l,0xe4f7fee9c4b63266l,0xb4acd4c2e3ac5664l,
            0xf8910bd2ceb38cbfl },
          { 0x1c3ae50cc9c0726el,0x15309569d97b40bfl,0x70884b7ffd5a5a1bl,
            0x3890896aef8314cdl },
          0 },
        /* 3 << 256 */
        { { 0x996884f5903fa271l,0xe6da0fd2b9da921el,0xa6f2f2695db01e54l,
            0x1ee3e9bd6876214el },
          { 0xa26e181ce27a9497l,0x36d254e48e215e04l,0x42f32a6c252cabcal,
            0x9948148780b57614l },
          0 },
        /* 4 << 256 */
        { { 0xab41b43a43228d83l,0x24ae1c304ad63f99l,0x8e525f1a46a51229l,
            0x14af860fcd26d2b4l },
          { 0xd6baef613f714aa1l,0xf51865adeb78795el,0xd3e21fcee6a9d694l,
            0x82ceb1dd8a37b527l },
          0 },
        /* 5 << 256 */
        { { 0x4a665bfd2f9fd51al,0x7f2f1fe2481b97f7l,0xcad05d69ad36ce50l,
            0x314fc2a4844f4dedl },
          { 0xd5593d8cb55fc5c6l,0xe3510ce8bfb1e23dl,0xf9b7be6937453ccel,
            0xd3541b7969fae631l },
          0 },
        /* 7 << 256 */
        { { 0x711b8a4176a9f05dl,0x06ca4e4b9011d488l,0x543bc62ba248a65el,
            0x017535ffc9290894l },
          { 0x840b84ce406851d7l,0xafa3acdf90e960b4l,0xac3394af7128fd34l,
            0x54eb4d5b2ac0f92cl },
          0 },
        /* 9 << 256 */
        { { 0x3549a0f14df48fecl,0x6ae7b1eec239f83al,0x001dcf253eb90ff3l,
            0x02ff0f02581e90edl },
          { 0x72921d8ca103dcefl,0x2c513c3c5876293el,0xc07064ca6b68875el,
            0x7198d44653b9537cl },
          0 },
        /* 10 << 256 */
        { { 0x58349b77685e089bl,0x1c678441219b7b8cl,0xba8da91f61e2e20dl,
            0xf9c50b8c309fd4e6l },
          { 0x99b0164996d0ef64l,0xac334ded60cdb63al,0x6b9ada19fb0bce4fl,
            0x39dc9375c7896377l },
          0 },
        /* 11 << 256 */
        { { 0x068dda8b7e1bc126l,0x77c7c58176243a21l,0xcc8ba55c875f9dael,
            0xdde7afe2ce469f95l },
          { 0xde2a15f5e9523b85l,0x447512c6d85674ael,0x5691f89e12c6c20cl,
            0xd64ef40e0fae4513l },
          0 },
        /* 13 << 256 */
        { { 0x10db2041c4d9eb40l,0x420eccb724f03f8al,0x64470fd17d29080el,
            0xf66c5b4416e52414l },
          { 0xa32cc70e4ca94031l,0xa67931592c8401bal,0x34f2dc29abfcc58dl,
            0x6f340f9a07325d7dl },
          0 },
        /* 15 << 256 */
        { { 0xf55d446b060a52bbl,0x2f33cb9f02939f24l,0x0f27a01bc8953718l,
            0x362882917fcd3932l },
          { 0x7485613488ed4436l,0xcfe69e27195f089el,0xd6ab040a8ff10bd8l,
            0x9741c5472e4a1623l },
          0 },
        /* 16 << 256 */
        { { 0xc52d8d8b6d55d6a4l,0xc4130fb3be58e8f9l,0x5f55c345e1275627l,
            0xb881a03c117042d3l },
          { 0x00a970a53238d301l,0x40d7cf2412a2c4f1l,0xace4a2f5d770ea74l,
            0x36a2e587e96940b2l },
          0 },
        /* 17 << 256 */
        { { 0x84793d9fef12d4c8l,0x04b89b152d8a163cl,0x0fdb566fb4a87740l,
            0xf7e6e5cf9e595680l },
          { 0xbcb973e41c5cd74el,0xafcb439fe4ed49d8l,0xd5c0820aebbae8eel,
            0x23483d836f56e2a2l },
          0 },
        /* 19 << 256 */
        { { 0x91f9b8be5e8ad115l,0xf1fd6a2e225db496l,0xf362d2cf4a444085l,
            0x033d9201eea043ebl },
          { 0x1e50c0989951a150l,0x4814fca5cfcf1f94l,0xaf3e8ef41bf82de5l,
            0xba0e2991038cff53l },
          0 },
        /* 21 << 256 */
        { { 0x904a41ae5fc373fal,0x235556d61a6a3fc4l,0xe44eb3ea36eeb570l,
            0xa4e1b34a26ba5ca6l },
          { 0x210e7c9131180257l,0x2c28669622158b0cl,0xc78b69c783ddd341l,
            0xfc05941b294e1750l },
          0 },
        /* 23 << 256 */
        { { 0x70666f51fc167dedl,0x47e9e289fe75b8d1l,0x8a5f59739605a03el,
            0x19876a58dd579094l },
          { 0x69a5c8cca964e426l,0xed74a652ccf20306l,0x5c93ae3cf06d31d5l,
            0x51922fa2127a8a12l },
          0 },
        /* 25 << 256 */
        { { 0xa18e26f99e3d509el,0xbc296dd2c10814fal,0x5dadd6eeaa24e147l,
            0xdba2121a8340f12el },
          { 0xd348e7f3e245ca21l,0x1e45a42978e3eb5bl,0x252bf89c169677bbl,
            0xfb33a2564021ac55l },
          0 },
        /* 27 << 256 */
        { { 0x30dc46586e7d72b8l,0x38df46fb0d81c3d6l,0x901bab6e10e84162l,
            0x25d7303ff7932801l },
          { 0xe781d5f37500be42l,0x9a7104c3380ff208l,0xfa801181652121a1l,
            0xef89f4f18d3bed43l },
          0 },
        /* 28 << 256 */
        { { 0xbe4ae5683594917al,0xef7c1c47a04bf81el,0xa1dc3612046d91a0l,
            0x3eee37affb11b338l },
          { 0x7e90278fd03d8f51l,0x3045a6da4fa183c6l,0xb39e573391cd16a9l,
            0xc748a504e54e9411l },
          0 },
        /* 29 << 256 */
        { { 0x07804331a1c6ec56l,0x25358e795b347123l,0x1ab9b39acf9432a4l,
            0x9628501d0a7881cel },
          { 0x749d58988a46d98el,0x01ea43346a17c321l,0xe2b197f9b1f9160fl,
            0x2052c7c07815f2a2l },
          0 },
        /* 31 << 256 */
        { { 0xaa691bfbc57a1a6dl,0x06cae127d737d525l,0x5be04b2f963c7c98l,
            0x936b1f5bfc00bc4al },
          { 0x3fed4ac77eda6a34l,0xba6ca7aa2500a438l,0x1e979fa6786c2a75l,
            0xa3db26bec13f37d4l },
          0 },
        /* 33 << 256 */
        { { 0x20afae333d7006d1l,0xdcbca6fbbda467d1l,0x2714b3827df4006cl,
            0x9abc0510c8e94549l },
          { 0x5b30a6d464c14915l,0xba91d0c35752b44fl,0x7ad9b19bbb389f1fl,
            0xe4c7aa04ef7c6e13l },
          0 },
        /* 34 << 256 */
        { { 0x1e24a3f23d12e2b6l,0xf99df403febd6db3l,0x61e580a6b0c8e12fl,
            0x819341b7c2bfe085l },
          { 0xd53002d640828921l,0x31e1eb65cea010efl,0xc48d0cfe85b3279fl,
            0xb90de69089f35fa5l },
          0 },
        /* 35 << 256 */
        { { 0xa3f6fd3c88ed748fl,0x6d72613af48127b9l,0xe85ed703d1e6f7e5l,
            0xbb563db449636f40l },
          { 0x23bae3c9708497bal,0x89dbff163aa65cf4l,0x70861847e6c0850al,
            0x5ef19d5d48b2e90cl },
          0 },
        /* 36 << 256 */
        { { 0xab6a1e13107f7bacl,0x83a8bc57972091f5l,0x3c65b454f6dcba41l,
            0xd7606ff96abc431dl },
          { 0xa3af9c189bd09971l,0x6ddd3bbf276bad63l,0xd2aba9beab4f0816l,
            0x8f13063c151581edl },
          0 },
        /* 37 << 256 */
        { { 0xf9c02364f5761b15l,0x3cfa250afd478139l,0x67d51e7416e26191l,
            0x0281bbf65eda396cl },
          { 0xbd38d4d70d1f4510l,0x2032a930edff593el,0x0ab74a0cf2ea4ad7l,
            0xb95aa9c3302498d6l },
          0 },
        /* 39 << 256 */
        { { 0x2995495dd7da3c7cl,0x28d579d0a0bb703el,0xabec6afec8288837l,
            0x93c34dfd05ab989bl },
          { 0xcc94f05dde5ea3dfl,0xc3e3d4ef90f436e6l,0x32b3dee1cf59dc4el,
            0x5eab01635447d9d9l },
          0 },
        /* 40 << 256 */
        { { 0xd31c5e8e2c23464el,0x5bcc382f50cfbde7l,0x6cee3d8da93c3d9bl,
            0xbee2948909ee62acl },
          { 0x4848d59c10742b84l,0x2486796fe35e9c84l,0x1a1d9570cd8f391al,
            0x839aa0913eedb743l },
          0 },
        /* 41 << 256 */
        { { 0xae02a7ce0f83f369l,0x3b67c56097994835l,0x715def441ae4bbeal,
            0x11e764ee59f6b9eel },
          { 0x70c775051c962c3al,0x42811507d937a258l,0x06dbdceed03e6e86l,
            0x39a3a7ed48cae79el },
          0 },
        /* 43 << 256 */
        { { 0xa32e729fb220eef8l,0x12d876baf37ac5d7l,0x9376ab45105a7f34l,
            0xb422331a4deb7275l },
          { 0x6ea07fb7686dea5el,0xba67ed3e1d8e32c9l,0x5ae52632bbc6bb9cl,
            0xdca55b86d1397575l },
          0 },
        /* 44 << 256 */
        { { 0xd9183f74378200b1l,0xe5ea1645762f5605l,0x78b42e2f7bd6290fl,
            0xa0bdfccc07fa0899l },
          { 0x2f92ea52dacda629l,0x810b4e6c48de27e2l,0x013d8587d9d1250dl,
            0xc153d519dd5141d5l },
          0 },
        /* 45 << 256 */
        { { 0x8f1f6cb5b8f1d719l,0xa9abc27b04e15a4el,0xc0d944a92ad42296l,
            0x69ecc877f3d2b0e5l },
          { 0xec60dbea16a5581al,0x2a0ead5fb85130d6l,0x7b3d2ebb6fddac23l,
            0x06213269ac448663l },
          0 },
        /* 46 << 256 */
        { { 0xe1074008ac11e180l,0xdff3339c14b8f830l,0x136e22be636504f3l,
            0xb07ae98aa09c5c4cl },
          { 0x9b0a0517192168e9l,0x39e09fac86ad0865l,0x24f90705adb08d41l,
            0x9c699cc759d3be24l },
          0 },
        /* 47 << 256 */
        { { 0xd9e16551907e36b0l,0x57f24b6caf91cb5al,0xbdb7dfdb062edae4l,
            0x99e3bffe4b85f424l },
          { 0x250774f4b2961ba7l,0xe7c0f2386d993c51l,0xcd0aae29f559b4bdl,
            0x3b12893a09a6859bl },
          0 },
        /* 48 << 256 */
        { { 0xac177eb985ae12c3l,0x8e6cb5cc6cf76537l,0x134abb19f265f9e3l,
            0xc37309b71ba3f55dl },
          { 0x570833b4392d564bl,0xaa273a27d8c22f00l,0x9ba6b6276006773al,
            0x2156c94f0a16c092l },
          0 },
        /* 49 << 256 */
        { { 0x2be0436b408e1258l,0xb179a2e34f47f121l,0x140b948fa42d3cfcl,
            0x96649c6700d2b4e6l },
          { 0x2bf934c7d08a4b34l,0x371c770136b472ddl,0x36297876e06adc73l,
            0x59e0d8251c3e6558l },
          0 },
        /* 51 << 256 */
        { { 0x9368cfd304a8bc81l,0x145249d4c49e58c7l,0x8c7ac1891392be01l,
            0x58cbcb5fbc7b0903l },
          { 0x502218a1a0377b0al,0x5c17eb8afb625836l,0x845c09ef349f4d26l,
            0x15fdeb2554ddce85l },
          0 },
        /* 52 << 256 */
        { { 0xf773535a64e8344dl,0xb8486a33d0dbabe6l,0x43c2df99b578862dl,
            0xcead29a11a39820el },
          { 0x3e5466fe63134d63l,0xc37ea88fdf43a104l,0x3b34ac34bbaacb5al,
            0x8281c240bc20be5al },
          0 },
        /* 53 << 256 */
        { { 0x55113d5e0f8dec77l,0xdfe59f251d7e1543l,0x3b2837e0a63a849al,
            0xdfbdb8b67a5691afl },
          { 0x8dd6faf0bd4cf444l,0x28b2bdfaab128b6cl,0x44af3ee24b1098ebl,
            0xbbf328ebe50b2d02l },
          0 },
        /* 55 << 256 */
        { { 0xf231b1f4e4e6151al,0x6ac7130413258c6al,0x6f9cb1c1a09b9f86l,
            0xbfc9291ee52ed880l },
          { 0x2a7d8230bea258a2l,0xd52a0da6baf386acl,0x5166764b3af00b7el,
            0x84792b043c985be2l },
          0 },
        /* 57 << 256 */
        { { 0x914ca588a906d9e4l,0xb4e4e86abc27a876l,0x97e6ed27724324f2l,
            0xda7e9aa5c0b87d2cl },
          { 0xafccbe6b33a56f84l,0x69e8fd4ac892d90al,0xb47512910bb5457fl,
            0xad65e4d05cb136fal },
          0 },
        /* 59 << 256 */
        { { 0xb09974d2fd679a1bl,0x17abc2a54578faf0l,0xe7da92828c830388l,
            0x7e455d8b0edf6146l },
          { 0xdff3b2f0c324bdb6l,0xe7a1718769f4a4f9l,0xfb4e0b3129c500a4l,
            0x1ed50799a09c5a07l },
          0 },
        /* 60 << 256 */
        { { 0x6b669496c679d9f9l,0x3b741f36e78f0830l,0xf99d4857eb3f9e53l,
            0x41be594276f7d4ael },
          { 0x75f44d57c09a112bl,0xa5139fd68475eeb7l,0xa4560cd5c6bc9df6l,
            0x8ce2c4cf50845434l },
          0 },
        /* 61 << 256 */
        { { 0x96b515c32b3cb0a6l,0x65836de3930d5344l,0xfb032d5b00e6d403l,
            0x2648301843c93bd1l },
          { 0xfc4525dd4b572363l,0x12b7923e7b28ab5cl,0xf376b633e22ac5e6l,
            0xd6ff6582e30b4707l },
          0 },
        /* 63 << 256 */
        { { 0x8bdce75c83b09e07l,0x64228b19227717c4l,0xeae8f8a2dc6a1f02l,
            0x1081031be72f3b6dl },
          { 0xba0f876072c3f736l,0xde38a0c5246a28adl,0x0b116fe08596c412l,
            0xb9e37be3fa135d11l },
          0 },
        /* 64 << 256 */
        { { 0x09800dc1b48d4168l,0xa740b282bfee87a2l,0x80c6b75dc94a547al,
            0x8cb622f0099c1985l },
          { 0xe6c789631467e05dl,0x027b658822fd3064l,0xe14735e2c2fdb68cl,
            0xfd2869947d853158l },
          0 },
        /* 65 << 256 */
        { { 0x301916a5bbd7caf1l,0xef563fda4e2076c2l,0xccbc56088467f279l,
            0xd7de3088b8d0f1bfl },
          { 0x3d9adcce8586910dl,0x3fa3b8b9d775e0e9l,0x4b7a4a1d88136503l,
            0xc748656de4994fcel },
          0 },
        /* 71 << 256 */
        { { 0x18cc605c2d9f8646l,0x3764f1c29e441b64l,0xb0ea7f7fc4b64ee3l,
            0xb5c22d0c042f8678l },
          { 0x3761f7f89b3057fdl,0xc85b8de64a207ce4l,0x11da715bc5c04cf7l,
            0x0cb1fa77c8e99c1fl },
          0 },
        /* 77 << 256 */
        { { 0x35f9cfc8045dab4el,0x08a65c6771a7d720l,0xf076767b8eef1351l,
            0x5351dbff8638fbe5l },
          { 0x5aead6f7772ad54cl,0x5f6b441fafe93e69l,0xb7b83d1aeeb876b5l,
            0xbe1ba4a7cdc094d9l },
          0 },
        /* 83 << 256 */
        { { 0x005d8f04ec0377bal,0x036b8e1ace58f05dl,0xdd6ffc6f1b28cf58l,
            0xc3d95a58e206189fl },
          { 0xcb2873c1f52e8b8cl,0xcffdb18d80142af1l,0x7cf88eb64c77ed78l,
            0xb3a3141981ef2c12l },
          0 },
        /* 89 << 256 */
        { { 0xbb17e6f957c175b1l,0xf33abc63260a6f6dl,0x9435f2de620ddd6bl,
            0x90bdde59ff3e99eal },
          { 0x3d7875e0567b520fl,0xdd6954aa813b4978l,0x1af3dc24de7b631cl,
            0x82ddcd08934d3c97l },
          0 },
        /* 95 << 256 */
        { { 0x7a9d60affc5ce598l,0xc6f507597c37abfdl,0xaa1b32f3a79355d0l,
            0xac581b94d7e4fcf3l },
          { 0x2669cefd139f6466l,0x560a98bb26f97570l,0x32e1c1db2837b908l,
            0x7823d7922d252781l },
          0 },
        /* 101 << 256 */
        { { 0xea018b4cdedf9af0l,0x4b64c0a380c1d2f9l,0x527a0b1c36992c44l,
            0x72a2408142b7adffl },
          { 0x0023d10f97a502eel,0xc0f9ed067b401ac4l,0xabd1bd03d6d3b516l,
            0xc320e3e478c5d0bel },
          0 },
        /* 107 << 256 */
        { { 0x9f5d2a6a37dd009cl,0x88c0f42ac2c3cbacl,0x3155636977552a1el,
            0xe78ec89d02f8098fl },
          { 0x276c2ad71b6eeff9l,0xf4c49a28f7f91856l,0x698a2368dc795124l,
            0x5502810de92a6c0fl },
          0 },
        /* 113 << 256 */
        { { 0x82a5042e9f5e5192l,0x64da65fac0965a88l,0xf4c80dd56668399el,
            0x635323757e33c233l },
          { 0x5e5339b1a0048616l,0x4a17b1931c91741fl,0x65fdc7c213dcf3d0l,
            0x230181426d10c410l },
          0 },
        /* 116 << 256 */
        { { 0x090a04220f46c635l,0xc7eac842a04de3f5l,0x45b69d4c8990d4b2l,
            0x032aeb50b8e0cdc6l },
          { 0x02ce332a4ee3f307l,0x3c80c1545043980fl,0xc774838bcbd5287cl,
            0x052661074a37d0ael },
          0 },
        /* 119 << 256 */
        { { 0xc401b9c0f4d70fbfl,0xf82bbfde98ee47fel,0x94965118c84d91afl,
            0xdd9a67c4d3b6ad1dl },
          { 0x85c9cf1eb66a3ad4l,0x05580a0fbf5f514cl,0xf3ef0fd00218536el,
            0x1dc2cf2bd14a7ca9l },
          0 },
        /* 125 << 256 */
        { { 0x18c83e337c1e24d4l,0x30911165563657c6l,0xf9be1af679e53083l,
            0x9b058059637753cel },
          { 0x6a37fa24e54522b9l,0xc11d38b426dbf4c4l,0xbc6738655ebd4d9al,
            0x2b40e9427fd4e2ecl },
          0 },
    },
};

/* Structure used to describe recoding of scalar multiplication. */
typedef struct ecc_recode_sum {
    /* Index into pre-computation table. */
    uint8_t i;
    /* Multiplier to add point into. */
    uint8_t mul;
    /* Use the negative of the point. */
    uint8_t neg;
} ecc_recode_sum;

/* The index into pre-computation table to use. */
static uint8_t recode_index_4_8[258] = {
     0,  1,  1,  1,  3,  4,  2,  5,  3,  2,  4,  8,  3,  9,  5,  4,
    11, 12,  6, 13,  7,  5,  8, 15, 55, 16,  9,  6, 18, 19,  7, 20,
    11,  8, 12, 23, 24, 25, 13,  9, 27, 28, 14, 29, 30, 10, 15, 33,
    11, 35, 16, 12, 37, 38, 17, 39, 18, 13, 19, 41, 42, 43, 20, 14,
    45, 46, 21, 44, 22, 15, 23, 47, 24, 43, 25, 16, 42, 48, 26, 41,
    27, 17, 28, 49, 18, 40, 29, 19, 30, 50, 31, 39, 32, 20, 33, 51,
    34, 38, 35, 21, 37, 52, 22, 36, 37, 23, 38, 53, 24, 35, 39, 25,
    34, 54, 40, 33, 55, 26, 32, 56, 27, 31, 43, 28, 30, 57, 44, 29,
    45, 29, 44, 57, 30, 28, 43, 31, 27, 56, 32, 26, 55, 33, 40, 54,
    34, 25, 39, 35, 24, 53, 38, 23, 37, 36, 22, 52, 37, 21, 35, 38,
    34, 51, 33, 20, 32, 39, 31, 50, 30, 19, 29, 40, 18, 49, 28, 17,
    27, 41, 26, 48, 42, 16, 25, 43, 24, 47, 23, 15, 22, 44, 21, 46,
    45, 14, 20, 43, 42, 41, 19, 13, 18, 39, 17, 38, 37, 12, 16, 35,
    11, 33, 15, 10, 30, 29, 14, 28, 27,  9, 13, 25, 24, 23, 12,  8,
    11, 20,  7, 19, 18,  6,  9, 16, 55, 15,  8,  5,  7, 13,  6, 12,
    11,  4,  5,  9,  3,  8,  4,  2,  3,  5,  2,  4,  3,  1,  1,  1,
     0,  1,
};

/* Multiple to add point into. */
static uint8_t recode_mul_4_8[258] = {
     0,  1,  2,  3,  1,  1,  2,  1,  2,  3,  2,  1,  3,  1,  2,  3,
     1,  1,  2,  1,  2,  3,  2,  1,  2,  1,  2,  3,  1,  1,  3,  1,
     2,  3,  2,  1,  1,  1,  2,  3,  1,  1,  2,  1,  1,  3,  2,  1,
     3,  1,  2,  3,  1,  1,  2,  1,  2,  3,  2,  1,  1,  1,  2,  3,
     1,  1,  2,  3,  2,  3,  2,  1,  2,  3,  2,  3,  3,  1,  2,  3,
     2,  3,  2,  1,  3,  3,  2,  3,  2,  1,  2,  3,  2,  3,  2,  1,
     2,  3,  2,  3,  3,  1,  3,  3,  2,  3,  2,  1,  3,  3,  2,  3,
     3,  1,  2,  3,  1,  3,  3,  1,  3,  3,  2,  3,  3,  1,  2,  3,
     2,  3,  2,  1,  3,  3,  2,  3,  3,  1,  3,  3,  1,  3,  2,  1,
     3,  3,  2,  3,  3,  1,  2,  3,  2,  3,  3,  1,  3,  3,  2,  3,
     2,  1,  2,  3,  2,  3,  2,  1,  2,  3,  2,  3,  3,  1,  2,  3,
     2,  3,  2,  1,  3,  3,  2,  3,  2,  1,  2,  3,  2,  3,  2,  1,
     1,  3,  2,  1,  1,  1,  2,  3,  2,  1,  2,  1,  1,  3,  2,  1,
     3,  1,  2,  3,  1,  1,  2,  1,  1,  3,  2,  1,  1,  1,  2,  3,
     2,  1,  3,  1,  1,  3,  2,  1,  2,  1,  2,  3,  2,  1,  2,  1,
     1,  3,  2,  1,  3,  1,  2,  3,  2,  1,  2,  1,  1,  3,  2,  1,
     0,  1,
};

/* Whether to negate y-ordinate. */
static uint8_t recode_neg_4_8[258] = {
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  1,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  1,  0,  0,  0,  0,  0,  1,  0,  0,  1,  0,  0,  1,
     0,  0,  0,  0,  0,  1,  0,  0,  0,  0,  0,  1,  0,  0,  0,  0,
     0,  1,  0,  0,  1,  0,  0,  1,  0,  0,  0,  0,  0,  1,  0,  0,
     1,  0,  0,  1,  0,  0,  1,  0,  0,  1,  0,  0,  1,  0,  0,  1,
     0,  0,  1,  1,  0,  1,  1,  0,  1,  1,  0,  1,  1,  0,  1,  1,
     0,  1,  1,  0,  1,  1,  1,  1,  1,  0,  1,  1,  0,  1,  1,  0,
     1,  1,  1,  1,  1,  0,  1,  1,  1,  1,  1,  0,  1,  1,  1,  1,
     1,  0,  1,  1,  0,  1,  1,  0,  1,  1,  1,  1,  1,  0,  1,  1,
     1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
     1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
     1,  1,  1,  1,  1,  1,  1,  1,  0,  1,  1,  1,  1,  1,  1,  1,
     1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
     0,  0,
};

/* Recode the scalar for multiplication using pre-computed values, multipliers
 * and subtraction.
 *
 * k  Scalar to multiply by.
 * v  Vector of operations to peform.
 */
static void sp_256_ecc_recode_sum_8_4(sp_digit* k, ecc_recode_sum* v)
{
    int i, j;
    uint16_t y;
    int carry = 0;
    int o;
    sp_digit n;

    j = 0;
    n = k[j];
    o = 0;
    for (i=0; i<33; i++) {
        y = n;
        if (o + 8 < 64) {
            y &= 0xff;
            n >>= 8;
            o += 8;
        }
        else if (o + 8 == 64) {
            n >>= 8;
            if (++j < 4)
                n = k[j];
            o = 0;
        }
        else if (++j < 4) {
            n = k[j];
            y |= (n << (64 - o)) & 0xff;
            o -= 56;
            n >>= o;
        }

        y += carry;
        v[i].i = recode_index_4_8[y];
        v[i].mul = recode_mul_4_8[y];
        v[i].neg = recode_neg_4_8[y];
        carry = (y >> 8) + v[i].neg;
    }
}

/* Multiply the base point of P256 by the scalar and return the result.
 * If map is true then convert result to affine co-ordinates.
 *
 * r     Resulting point.
 * k     Scalar to multiply by.
 * map   Indicates whether to convert result to affine.
 * heap  Heap to use for allocation.
 * returns MEMORY_E when memory allocation fails and MP_OKAY on success.
 */
static int sp_256_ecc_mulmod_base_4(sp_point* r, sp_digit* k, int map,
        void* heap)
{
#if !defined(WOLFSSL_SP_SMALL) && !defined(WOLFSSL_SMALL_STACK)
    sp_point td[4];
    sp_point pd;
    sp_digit tmpd[2 * 4 * 5];
#endif
    sp_point* t;
    sp_point* p;
    sp_digit* tmp;
    sp_digit* negy;
    int i;
    ecc_recode_sum v[33];
    int err;

    (void)heap;

    err = sp_ecc_point_new(heap, pd, p);
#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    t = (sp_point*)XMALLOC(sizeof(sp_point) * 4, heap, DYNAMIC_TYPE_ECC);
    if (t == NULL)
        err = MEMORY_E;
    tmp = (sp_digit*)XMALLOC(sizeof(sp_digit) * 2 * 4 * 5, heap,
                             DYNAMIC_TYPE_ECC);
    if (tmp == NULL)
        err = MEMORY_E;
#else
    t = td;
    tmp = tmpd;
#endif
    negy = tmp;

    if (err == MP_OKAY) {
        sp_256_ecc_recode_sum_8_4(k, v);

        XMEMCPY(p->z, p256_norm_mod, sizeof(p256_norm_mod));
        XMEMSET(t, 0, sizeof(sp_point) * 4);
        for (i=0; i<4; i++) {
            XMEMCPY(t[i].z, p256_norm_mod, sizeof(p256_norm_mod));
            t[i].infinity = 1;
        }

        i = 32;
        XMEMCPY(t[v[i].mul].x, p256_table[i][v[i].i].x,
                sizeof(p256_table[i]->x));
        XMEMCPY(t[v[i].mul].y, p256_table[i][v[i].i].y,
                sizeof(p256_table[i]->y));
        t[v[i].mul].infinity = p256_table[i][v[i].i].infinity;
        for (--i; i>=0; i--) {
            XMEMCPY(p->x, p256_table[i][v[i].i].x, sizeof(p256_table[i]->x));
            XMEMCPY(p->y, p256_table[i][v[i].i].y, sizeof(p256_table[i]->y));
            p->infinity = p256_table[i][v[i].i].infinity;
            sp_256_sub_4(negy, p256_mod, p->y);
            sp_256_cond_copy_4(p->y, negy, (sp_digit)0 - v[i].neg);
            sp_256_proj_point_add_qz1_4(&t[v[i].mul], &t[v[i].mul], p,
                    tmp);
        }
        sp_256_proj_point_add_4(&t[2], &t[2], &t[3], tmp);
        sp_256_proj_point_add_4(&t[1], &t[1], &t[3], tmp);
        sp_256_proj_point_dbl_4(&t[2], &t[2], tmp);
        sp_256_proj_point_add_4(&t[1], &t[1], &t[2], tmp);

        if (map)
            sp_256_map_4(r, &t[1], tmp);
        else
            XMEMCPY(r, &t[1], sizeof(sp_point));
    }

#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (t != NULL) {
        XMEMSET(t, 0, sizeof(sp_point) * 4);
        XFREE(t, heap, DYNAMIC_TYPE_ECC);
    }
    if (tmp != NULL) {
        XMEMSET(tmp, 0, sizeof(sp_digit) * 2 * 4 * 5);
        XFREE(tmp, heap, DYNAMIC_TYPE_ECC);
    }
#else
    ForceZero(tmpd, sizeof(tmpd));
    ForceZero(td, sizeof(td));
#endif
    sp_ecc_point_free(p, 0, heap);

    return MP_OKAY;
}

#ifdef HAVE_INTEL_AVX2
/* Multiply the base point of P256 by the scalar and return the result.
 * If map is true then convert result to affine co-ordinates.
 *
 * r     Resulting point.
 * k     Scalar to multiply by.
 * map   Indicates whether to convert result to affine.
 * heap  Heap to use for allocation.
 * returns MEMORY_E when memory allocation fails and MP_OKAY on success.
 */
static int sp_256_ecc_mulmod_base_avx2_4(sp_point* r, sp_digit* k, int map,
        void* heap)
{
#if !defined(WOLFSSL_SP_SMALL) && !defined(WOLFSSL_SMALL_STACK)
    sp_point td[4];
    sp_point pd;
    sp_digit tmpd[2 * 4 * 5];
#endif
    sp_point* t;
    sp_point* p;
    sp_digit* tmp;
    sp_digit* negy;
    int i;
    ecc_recode_sum v[33];
    int err;

    (void)heap;

    err = sp_ecc_point_new(heap, pd, p);
#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    t = (sp_point*)XMALLOC(sizeof(sp_point) * 4, heap, DYNAMIC_TYPE_ECC);
    if (t == NULL)
        err = MEMORY_E;
    tmp = (sp_digit*)XMALLOC(sizeof(sp_digit) * 2 * 4 * 5, heap,
                             DYNAMIC_TYPE_ECC);
    if (tmp == NULL)
        err = MEMORY_E;
#else
    t = td;
    tmp = tmpd;
#endif
    negy = tmp;

    if (err == MP_OKAY) {
        sp_256_ecc_recode_sum_8_4(k, v);

        XMEMCPY(p->z, p256_norm_mod, sizeof(p256_norm_mod));
        XMEMSET(t, 0, sizeof(sp_point) * 4);
        for (i=0; i<4; i++) {
            XMEMCPY(t[i].z, p256_norm_mod, sizeof(p256_norm_mod));
            t[i].infinity = 1;
        }

        i = 32;
        XMEMCPY(t[v[i].mul].x, p256_table[i][v[i].i].x,
                sizeof(p256_table[i]->x));
        XMEMCPY(t[v[i].mul].y, p256_table[i][v[i].i].y,
                sizeof(p256_table[i]->y));
        t[v[i].mul].infinity = p256_table[i][v[i].i].infinity;
        for (--i; i>=0; i--) {
            XMEMCPY(p->x, p256_table[i][v[i].i].x, sizeof(p256_table[i]->x));
            XMEMCPY(p->y, p256_table[i][v[i].i].y, sizeof(p256_table[i]->y));
            p->infinity = p256_table[i][v[i].i].infinity;
            sp_256_sub_4(negy, p256_mod, p->y);
            sp_256_cond_copy_4(p->y, negy, (sp_digit)0 - v[i].neg);
            sp_256_proj_point_add_qz1_avx2_4(&t[v[i].mul], &t[v[i].mul], p,
                    tmp);
        }
        sp_256_proj_point_add_avx2_4(&t[2], &t[2], &t[3], tmp);
        sp_256_proj_point_add_avx2_4(&t[1], &t[1], &t[3], tmp);
        sp_256_proj_point_dbl_avx2_4(&t[2], &t[2], tmp);
        sp_256_proj_point_add_avx2_4(&t[1], &t[1], &t[2], tmp);

        if (map)
            sp_256_map_avx2_4(r, &t[1], tmp);
        else
            XMEMCPY(r, &t[1], sizeof(sp_point));
    }

#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (t != NULL) {
        XMEMSET(t, 0, sizeof(sp_point) * 4);
        XFREE(t, heap, DYNAMIC_TYPE_ECC);
    }
    if (tmp != NULL) {
        XMEMSET(tmp, 0, sizeof(sp_digit) * 2 * 4 * 5);
        XFREE(tmp, heap, DYNAMIC_TYPE_ECC);
    }
#else
    ForceZero(tmpd, sizeof(tmpd));
    ForceZero(td, sizeof(td));
#endif
    sp_ecc_point_free(p, 0, heap);

    return MP_OKAY;
}

#endif /* HAVE_INTEL_AVX2 */
#endif /* WOLFSSL_SP_SMALL */
/* Multiply the base point of P256 by the scalar and return the result.
 * If map is true then convert result to affine co-ordinates.
 *
 * km    Scalar to multiply by.
 * r     Resulting point.
 * map   Indicates whether to convert result to affine.
 * heap  Heap to use for allocation.
 * returns MEMORY_E when memory allocation fails and MP_OKAY on success.
 */
int sp_ecc_mulmod_base_256(mp_int* km, ecc_point* r, int map, void* heap)
{
#if !defined(WOLFSSL_SP_SMALL) && !defined(WOLFSSL_SMALL_STACK)
    sp_point p;
    sp_digit kd[4];
#endif
    sp_point* point;
    sp_digit* k = NULL;
    int err = MP_OKAY;
#ifdef HAVE_INTEL_AVX2
    word32 cpuid_flags = cpuid_get_flags();
#endif

    err = sp_ecc_point_new(heap, p, point);
#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (err == MP_OKAY) {
        k = (sp_digit*)XMALLOC(sizeof(sp_digit) * 4, heap,
                                                              DYNAMIC_TYPE_ECC);
        if (k == NULL)
            err = MEMORY_E;
    }
#else
    k = kd;
#endif
    if (err == MP_OKAY) {
        sp_256_from_mp(k, 4, km);

#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
            err = sp_256_ecc_mulmod_base_avx2_4(point, k, map, heap);
        else
#endif
            err = sp_256_ecc_mulmod_base_4(point, k, map, heap);
    }
    if (err == MP_OKAY)
        err = sp_256_point_to_ecc_point_4(point, r);

#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (k != NULL)
        XFREE(k, heap, DYNAMIC_TYPE_ECC);
#endif
    sp_ecc_point_free(point, 0, heap);

    return err;
}

#if defined(WOLFSSL_VALIDATE_ECC_KEYGEN) || defined(HAVE_ECC_SIGN) || \
                                                        defined(HAVE_ECC_VERIFY)
/* Returns 1 if the number of zero.
 * Implementation is constant time.
 *
 * a  Number to check.
 * returns 1 if the number is zero and 0 otherwise.
 */
static int sp_256_iszero_4(const sp_digit* a)
{
    return (a[0] | a[1] | a[2] | a[3]) == 0;
}

#endif /* WOLFSSL_VALIDATE_ECC_KEYGEN || HAVE_ECC_SIGN || HAVE_ECC_VERIFY */
extern void sp_256_add_one_4(sp_digit* a);
/* Read big endian unsigned byte aray into r.
 *
 * r  A single precision integer.
 * a  Byte array.
 * n  Number of bytes in array to read.
 */
static void sp_256_from_bin(sp_digit* r, int max, const byte* a, int n)
{
    int i, j = 0, s = 0;

    r[0] = 0;
    for (i = n-1; i >= 0; i--) {
        r[j] |= ((sp_digit)a[i]) << s;
        if (s >= 56) {
            r[j] &= 0xffffffffffffffffl;
            s = 64 - s;
            if (j + 1 >= max)
                break;
            r[++j] = a[i] >> s;
            s = 8 - s;
        }
        else
            s += 8;
    }

    for (j++; j < max; j++)
        r[j] = 0;
}

/* Generates a scalar that is in the range 1..order-1.
 *
 * rng  Random number generator.
 * k    Scalar value.
 * returns RNG failures, MEMORY_E when memory allocation fails and
 * MP_OKAY on success.
 */
static int sp_256_ecc_gen_k_4(WC_RNG* rng, sp_digit* k)
{
    int err;
    byte buf[32];

    do {
        err = wc_RNG_GenerateBlock(rng, buf, sizeof(buf));
        if (err == 0) {
            sp_256_from_bin(k, 4, buf, sizeof(buf));
            if (sp_256_cmp_4(k, p256_order2) < 0) {
                sp_256_add_one_4(k);
                break;
            }
        }
    }
    while (err == 0);

    return err;
}

/* Makes a random EC key pair.
 *
 * rng   Random number generator.
 * priv  Generated private value.
 * pub   Generated public point.
 * heap  Heap to use for allocation.
 * returns ECC_INF_E when the point does not have the correct order, RNG
 * failures, MEMORY_E when memory allocation fails and MP_OKAY on success.
 */
int sp_ecc_make_key_256(WC_RNG* rng, mp_int* priv, ecc_point* pub, void* heap)
{
#if !defined(WOLFSSL_SP_SMALL) && !defined(WOLFSSL_SMALL_STACK)
    sp_point p;
    sp_digit kd[4];
#ifdef WOLFSSL_VALIDATE_ECC_KEYGEN
    sp_point inf;
#endif
#endif
    sp_point* point;
    sp_digit* k = NULL;
#ifdef WOLFSSL_VALIDATE_ECC_KEYGEN
    sp_point* infinity;
#endif
    int err;
#ifdef HAVE_INTEL_AVX2
    word32 cpuid_flags = cpuid_get_flags();
#endif

    (void)heap;

    err = sp_ecc_point_new(heap, p, point);
#ifdef WOLFSSL_VALIDATE_ECC_KEYGEN
    if (err == MP_OKAY)
        err = sp_ecc_point_new(heap, inf, infinity);
#endif
#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (err == MP_OKAY) {
        k = (sp_digit*)XMALLOC(sizeof(sp_digit) * 4, heap,
                                                              DYNAMIC_TYPE_ECC);
        if (k == NULL)
            err = MEMORY_E;
    }
#else
    k = kd;
#endif

    if (err == MP_OKAY)
        err = sp_256_ecc_gen_k_4(rng, k);
    if (err == MP_OKAY) {
#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
            err = sp_256_ecc_mulmod_base_avx2_4(point, k, 1, NULL);
        else
#endif
            err = sp_256_ecc_mulmod_base_4(point, k, 1, NULL);
    }

#ifdef WOLFSSL_VALIDATE_ECC_KEYGEN
    if (err == MP_OKAY) {
#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags)) {
            err = sp_256_ecc_mulmod_avx2_4(infinity, point, p256_order, 1,
                                                                          NULL);
        }
        else
#endif
            err = sp_256_ecc_mulmod_4(infinity, point, p256_order, 1, NULL);
    }
    if (err == MP_OKAY) {
        if (!sp_256_iszero_4(point->x) || !sp_256_iszero_4(point->y))
            err = ECC_INF_E;
    }
#endif

    if (err == MP_OKAY)
        err = sp_256_to_mp(k, priv);
    if (err == MP_OKAY)
        err = sp_256_point_to_ecc_point_4(point, pub);

#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (k != NULL)
        XFREE(k, heap, DYNAMIC_TYPE_ECC);
#endif
#ifdef WOLFSSL_VALIDATE_ECC_KEYGEN
    sp_ecc_point_free(infinity, 1, heap);
#endif
    sp_ecc_point_free(point, 1, heap);

    return err;
}

#ifdef HAVE_ECC_DHE
/* Write r as big endian to byte aray.
 * Fixed length number of bytes written: 32
 *
 * r  A single precision integer.
 * a  Byte array.
 */
static void sp_256_to_bin(sp_digit* r, byte* a)
{
    int i, j, s = 0, b;

    j = 256 / 8 - 1;
    a[j] = 0;
    for (i=0; i<4 && j>=0; i++) {
        b = 0;
        a[j--] |= r[i] << s; b += 8 - s;
        if (j < 0)
            break;
        while (b < 64) {
            a[j--] = r[i] >> b; b += 8;
            if (j < 0)
                break;
        }
        s = 8 - (b - 64);
        if (j >= 0)
            a[j] = 0;
        if (s != 0)
            j++;
    }
}

/* Multiply the point by the scalar and serialize the X ordinate.
 * The number is 0 padded to maximum size on output.
 *
 * priv    Scalar to multiply the point by.
 * pub     Point to multiply.
 * out     Buffer to hold X ordinate.
 * outLen  On entry, size of the buffer in bytes.
 *         On exit, length of data in buffer in bytes.
 * heap    Heap to use for allocation.
 * returns BUFFER_E if the buffer is to small for output size,
 * MEMORY_E when memory allocation fails and MP_OKAY on success.
 */
int sp_ecc_secret_gen_256(mp_int* priv, ecc_point* pub, byte* out,
                          word32* outLen, void* heap)
{
#if !defined(WOLFSSL_SP_SMALL) && !defined(WOLFSSL_SMALL_STACK)
    sp_point p;
    sp_digit kd[4];
#endif
    sp_point* point = NULL;
    sp_digit* k = NULL;
    int err = MP_OKAY;
#ifdef HAVE_INTEL_AVX2
    word32 cpuid_flags = cpuid_get_flags();
#endif

    if (*outLen < 32)
        err = BUFFER_E;

    if (err == MP_OKAY)
        err = sp_ecc_point_new(heap, p, point);
#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (err == MP_OKAY) {
        k = (sp_digit*)XMALLOC(sizeof(sp_digit) * 4, heap,
                                                              DYNAMIC_TYPE_ECC);
        if (k == NULL)
            err = MEMORY_E;
    }
#else
    k = kd;
#endif

    if (err == MP_OKAY) {
        sp_256_from_mp(k, 4, priv);
        sp_256_point_from_ecc_point_4(point, pub);
#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
            err = sp_256_ecc_mulmod_avx2_4(point, point, k, 1, heap);
        else
#endif
            err = sp_256_ecc_mulmod_4(point, point, k, 1, heap);
    }
    if (err == MP_OKAY) {
        sp_256_to_bin(point->x, out);
        *outLen = 32;
    }

#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (k != NULL)
        XFREE(k, heap, DYNAMIC_TYPE_ECC);
#endif
    sp_ecc_point_free(point, 0, heap);

    return err;
}
#endif /* HAVE_ECC_DHE */

#if defined(HAVE_ECC_SIGN) || defined(HAVE_ECC_VERIFY)
extern sp_digit sp_256_add_4(sp_digit* r, const sp_digit* a, const sp_digit* b);
#endif
#if defined(HAVE_ECC_SIGN) || defined(HAVE_ECC_VERIFY)
extern void sp_256_mul_4(sp_digit* r, const sp_digit* a, const sp_digit* b);
#ifdef HAVE_INTEL_AVX2
extern void sp_256_mul_avx2_4(sp_digit* r, const sp_digit* a, const sp_digit* b);
#endif /* HAVE_INTEL_AVX2 */
#endif
#if defined(HAVE_ECC_SIGN) || defined(HAVE_ECC_VERIFY)
extern sp_digit sp_256_sub_in_place_4(sp_digit* a, const sp_digit* b);
extern void sp_256_mul_d_4(sp_digit* r, const sp_digit* a, const sp_digit b);
extern void sp_256_mul_d_avx2_4(sp_digit* r, const sp_digit* a, const sp_digit b);
/* Divide the double width number (d1|d0) by the dividend. (d1|d0 / div)
 *
 * d1   The high order half of the number to divide.
 * d0   The low order half of the number to divide.
 * div  The dividend.
 * returns the result of the division.
 */
static WC_INLINE sp_digit div_256_word_4(sp_digit d1, sp_digit d0,
        sp_digit div)
{
    register sp_digit r asm("rax");
    __asm__ __volatile__ (
        "divq %3"
        : "=a" (r)
        : "d" (d1), "a" (d0), "r" (div)
        :
    );
    return r;
}
/* AND m into each word of a and store in r.
 *
 * r  A single precision integer.
 * a  A single precision integer.
 * m  Mask to AND against each digit.
 */
static void sp_256_mask_4(sp_digit* r, sp_digit* a, sp_digit m)
{
#ifdef WOLFSSL_SP_SMALL
    int i;

    for (i=0; i<4; i++)
        r[i] = a[i] & m;
#else
    r[0] = a[0] & m;
    r[1] = a[1] & m;
    r[2] = a[2] & m;
    r[3] = a[3] & m;
#endif
}

/* Divide d in a and put remainder into r (m*d + r = a)
 * m is not calculated as it is not needed at this time.
 *
 * a  Nmber to be divided.
 * d  Number to divide with.
 * m  Multiplier result.
 * r  Remainder from the division.
 * returns MP_OKAY indicating success.
 */
static WC_INLINE int sp_256_div_4(sp_digit* a, sp_digit* d, sp_digit* m,
        sp_digit* r)
{
    sp_digit t1[8], t2[5];
    sp_digit div, r1;
    int i;
#ifdef HAVE_INTEL_AVX2
    word32 cpuid_flags = cpuid_get_flags();
#endif

    (void)m;

    div = d[3];
    XMEMCPY(t1, a, sizeof(*t1) * 2 * 4);
    for (i=3; i>=0; i--) {
        r1 = div_256_word_4(t1[4 + i], t1[4 + i - 1], div);

#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
            sp_256_mul_d_avx2_4(t2, d, r1);
        else
#endif
            sp_256_mul_d_4(t2, d, r1);
        t1[4 + i] += sp_256_sub_in_place_4(&t1[i], t2);
        t1[4 + i] -= t2[4];
        sp_256_mask_4(t2, d, t1[4 + i]);
        t1[4 + i] += sp_256_add_4(&t1[i], &t1[i], t2);
        sp_256_mask_4(t2, d, t1[4 + i]);
        t1[4 + i] += sp_256_add_4(&t1[i], &t1[i], t2);
    }

    r1 = sp_256_cmp_4(t1, d) >= 0;
    sp_256_cond_sub_4(r, t1, d, (sp_digit)0 - r1);

    return MP_OKAY;
}

/* Reduce a modulo m into r. (r = a mod m)
 *
 * r  A single precision number that is the reduced result.
 * a  A single precision number that is to be reduced.
 * m  A single precision number that is the modulus to reduce with.
 * returns MP_OKAY indicating success.
 */
static WC_INLINE int sp_256_mod_4(sp_digit* r, sp_digit* a, sp_digit* m)
{
    return sp_256_div_4(a, m, NULL, r);
}

#endif
#if defined(HAVE_ECC_SIGN) || defined(HAVE_ECC_VERIFY)
extern void sp_256_sqr_4(sp_digit* r, const sp_digit* a);
#ifdef WOLFSSL_SP_SMALL
/* Order-2 for the P256 curve. */
static const uint64_t p256_order_2[4] = {
    0xf3b9cac2fc63254f,0xbce6faada7179e84,0xffffffffffffffff,
    0xffffffff00000000
};
#else
/* The low half of the order-2 of the P256 curve. */
static const uint64_t p256_order_low[2] = {
    0xf3b9cac2fc63254f,0xbce6faada7179e84
};
#endif /* WOLFSSL_SP_SMALL */

/* Multiply two number mod the order of P256 curve. (r = a * b mod order)
 *
 * r  Result of the multiplication.
 * a  First operand of the multiplication.
 * b  Second operand of the multiplication.
 */
static void sp_256_mont_mul_order_4(sp_digit* r, sp_digit* a, sp_digit* b)
{
    sp_256_mul_4(r, a, b);
    sp_256_mont_reduce_order_4(r, p256_order, p256_mp_order);
}

/* Square number mod the order of P256 curve. (r = a * a mod order)
 *
 * r  Result of the squaring.
 * a  Number to square.
 */
static void sp_256_mont_sqr_order_4(sp_digit* r, sp_digit* a)
{
    sp_256_sqr_4(r, a);
    sp_256_mont_reduce_order_4(r, p256_order, p256_mp_order);
}

#ifndef WOLFSSL_SP_SMALL
/* Square number mod the order of P256 curve a number of times.
 * (r = a ^ n mod order)
 *
 * r  Result of the squaring.
 * a  Number to square.
 */
static void sp_256_mont_sqr_n_order_4(sp_digit* r, sp_digit* a, int n)
{
    int i;

    sp_256_mont_sqr_order_4(r, a);
    for (i=1; i<n; i++)
        sp_256_mont_sqr_order_4(r, r);
}
#endif /* !WOLFSSL_SP_SMALL */

/* Invert the number, in Montgomery form, modulo the order of the P256 curve.
 * (r = 1 / a mod order)
 *
 * r   Inverse result.
 * a   Number to invert.
 * td  Temporary data.
 */
static void sp_256_mont_inv_order_4(sp_digit* r, sp_digit* a,
        sp_digit* td)
{
#ifdef WOLFSSL_SP_SMALL
    sp_digit* t = td;
    int i;

    XMEMCPY(t, a, sizeof(sp_digit) * 4);
    for (i=254; i>=0; i--) {
        sp_256_mont_sqr_order_4(t, t);
        if (p256_order_2[i / 64] & ((sp_digit)1 << (i % 64)))
            sp_256_mont_mul_order_4(t, t, a);
    }
    XMEMCPY(r, t, sizeof(sp_digit) * 4);
#else
    sp_digit* t = td;
    sp_digit* t2 = td + 2 * 4;
    sp_digit* t3 = td + 4 * 4;
    int i;

    /* t = a^2 */
    sp_256_mont_sqr_order_4(t, a);
    /* t = a^3 = t * a */
    sp_256_mont_mul_order_4(t, t, a);
    /* t2= a^c = t ^ 2 ^ 2 */
    sp_256_mont_sqr_n_order_4(t2, t, 2);
    /* t3= a^f = t2 * t */
    sp_256_mont_mul_order_4(t3, t2, t);
    /* t2= a^f0 = t3 ^ 2 ^ 4 */
    sp_256_mont_sqr_n_order_4(t2, t3, 4);
    /* t = a^ff = t2 * t3 */
    sp_256_mont_mul_order_4(t, t2, t3);
    /* t3= a^ff00 = t ^ 2 ^ 8 */
    sp_256_mont_sqr_n_order_4(t2, t, 8);
    /* t = a^ffff = t2 * t */
    sp_256_mont_mul_order_4(t, t2, t);
    /* t2= a^ffff0000 = t ^ 2 ^ 16 */
    sp_256_mont_sqr_n_order_4(t2, t, 16);
    /* t = a^ffffffff = t2 * t */
    sp_256_mont_mul_order_4(t, t2, t);
    /* t2= a^ffffffff0000000000000000 = t ^ 2 ^ 64  */
    sp_256_mont_sqr_n_order_4(t2, t, 64);
    /* t2= a^ffffffff00000000ffffffff = t2 * t */
    sp_256_mont_mul_order_4(t2, t2, t);
    /* t2= a^ffffffff00000000ffffffff00000000 = t2 ^ 2 ^ 32  */
    sp_256_mont_sqr_n_order_4(t2, t2, 32);
    /* t2= a^ffffffff00000000ffffffffffffffff = t2 * t */
    sp_256_mont_mul_order_4(t2, t2, t);
    /* t2= a^ffffffff00000000ffffffffffffffffbce6 */
    for (i=127; i>=112; i--) {
        sp_256_mont_sqr_order_4(t2, t2);
        if (p256_order_low[i / 64] & ((sp_digit)1 << (i % 64)))
            sp_256_mont_mul_order_4(t2, t2, a);
    }
    /* t2= a^ffffffff00000000ffffffffffffffffbce6f */
    sp_256_mont_sqr_n_order_4(t2, t2, 4);
    sp_256_mont_mul_order_4(t2, t2, t3);
    /* t2= a^ffffffff00000000ffffffffffffffffbce6faada7179e84 */
    for (i=107; i>=64; i--) {
        sp_256_mont_sqr_order_4(t2, t2);
        if (p256_order_low[i / 64] & ((sp_digit)1 << (i % 64)))
            sp_256_mont_mul_order_4(t2, t2, a);
    }
    /* t2= a^ffffffff00000000ffffffffffffffffbce6faada7179e84f */
    sp_256_mont_sqr_n_order_4(t2, t2, 4);
    sp_256_mont_mul_order_4(t2, t2, t3);
    /* t2= a^ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2 */
    for (i=59; i>=32; i--) {
        sp_256_mont_sqr_order_4(t2, t2);
        if (p256_order_low[i / 64] & ((sp_digit)1 << (i % 64)))
            sp_256_mont_mul_order_4(t2, t2, a);
    }
    /* t2= a^ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2f */
    sp_256_mont_sqr_n_order_4(t2, t2, 4);
    sp_256_mont_mul_order_4(t2, t2, t3);
    /* t2= a^ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc63254 */
    for (i=27; i>=0; i--) {
        sp_256_mont_sqr_order_4(t2, t2);
        if (p256_order_low[i / 64] & ((sp_digit)1 << (i % 64)))
            sp_256_mont_mul_order_4(t2, t2, a);
    }
    /* t2= a^ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632540 */
    sp_256_mont_sqr_n_order_4(t2, t2, 4);
    /* r = a^ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc63254f */
    sp_256_mont_mul_order_4(r, t2, t3);
#endif /* WOLFSSL_SP_SMALL */
}

#ifdef HAVE_INTEL_AVX2
extern void sp_256_sqr_avx2_4(sp_digit* r, const sp_digit* a);
#define sp_256_mont_reduce_order_avx2_4    sp_256_mont_reduce_avx2_4

extern void sp_256_mont_reduce_avx2_4(sp_digit* a, sp_digit* m, sp_digit mp);
/* Multiply two number mod the order of P256 curve. (r = a * b mod order)
 *
 * r  Result of the multiplication.
 * a  First operand of the multiplication.
 * b  Second operand of the multiplication.
 */
static void sp_256_mont_mul_order_avx2_4(sp_digit* r, sp_digit* a, sp_digit* b)
{
    sp_256_mul_avx2_4(r, a, b);
    sp_256_mont_reduce_order_avx2_4(r, p256_order, p256_mp_order);
}

/* Square number mod the order of P256 curve. (r = a * a mod order)
 *
 * r  Result of the squaring.
 * a  Number to square.
 */
static void sp_256_mont_sqr_order_avx2_4(sp_digit* r, sp_digit* a)
{
    sp_256_sqr_avx2_4(r, a);
    sp_256_mont_reduce_order_avx2_4(r, p256_order, p256_mp_order);
}

#ifndef WOLFSSL_SP_SMALL
/* Square number mod the order of P256 curve a number of times.
 * (r = a ^ n mod order)
 *
 * r  Result of the squaring.
 * a  Number to square.
 */
static void sp_256_mont_sqr_n_order_avx2_4(sp_digit* r, sp_digit* a, int n)
{
    int i;

    sp_256_mont_sqr_order_avx2_4(r, a);
    for (i=1; i<n; i++)
        sp_256_mont_sqr_order_avx2_4(r, r);
}
#endif /* !WOLFSSL_SP_SMALL */

/* Invert the number, in Montgomery form, modulo the order of the P256 curve.
 * (r = 1 / a mod order)
 *
 * r   Inverse result.
 * a   Number to invert.
 * td  Temporary data.
 */
static void sp_256_mont_inv_order_avx2_4(sp_digit* r, sp_digit* a,
        sp_digit* td)
{
#ifdef WOLFSSL_SP_SMALL
    sp_digit* t = td;
    int i;

    XMEMCPY(t, a, sizeof(sp_digit) * 4);
    for (i=254; i>=0; i--) {
        sp_256_mont_sqr_order_avx2_4(t, t);
        if (p256_order_2[i / 64] & ((sp_digit)1 << (i % 64)))
            sp_256_mont_mul_order_avx2_4(t, t, a);
    }
    XMEMCPY(r, t, sizeof(sp_digit) * 4);
#else
    sp_digit* t = td;
    sp_digit* t2 = td + 2 * 4;
    sp_digit* t3 = td + 4 * 4;
    int i;

    /* t = a^2 */
    sp_256_mont_sqr_order_avx2_4(t, a);
    /* t = a^3 = t * a */
    sp_256_mont_mul_order_avx2_4(t, t, a);
    /* t2= a^c = t ^ 2 ^ 2 */
    sp_256_mont_sqr_n_order_avx2_4(t2, t, 2);
    /* t3= a^f = t2 * t */
    sp_256_mont_mul_order_avx2_4(t3, t2, t);
    /* t2= a^f0 = t3 ^ 2 ^ 4 */
    sp_256_mont_sqr_n_order_avx2_4(t2, t3, 4);
    /* t = a^ff = t2 * t3 */
    sp_256_mont_mul_order_avx2_4(t, t2, t3);
    /* t3= a^ff00 = t ^ 2 ^ 8 */
    sp_256_mont_sqr_n_order_avx2_4(t2, t, 8);
    /* t = a^ffff = t2 * t */
    sp_256_mont_mul_order_avx2_4(t, t2, t);
    /* t2= a^ffff0000 = t ^ 2 ^ 16 */
    sp_256_mont_sqr_n_order_avx2_4(t2, t, 16);
    /* t = a^ffffffff = t2 * t */
    sp_256_mont_mul_order_avx2_4(t, t2, t);
    /* t2= a^ffffffff0000000000000000 = t ^ 2 ^ 64  */
    sp_256_mont_sqr_n_order_avx2_4(t2, t, 64);
    /* t2= a^ffffffff00000000ffffffff = t2 * t */
    sp_256_mont_mul_order_avx2_4(t2, t2, t);
    /* t2= a^ffffffff00000000ffffffff00000000 = t2 ^ 2 ^ 32  */
    sp_256_mont_sqr_n_order_avx2_4(t2, t2, 32);
    /* t2= a^ffffffff00000000ffffffffffffffff = t2 * t */
    sp_256_mont_mul_order_avx2_4(t2, t2, t);
    /* t2= a^ffffffff00000000ffffffffffffffffbce6 */
    for (i=127; i>=112; i--) {
        sp_256_mont_sqr_order_avx2_4(t2, t2);
        if (p256_order_low[i / 64] & ((sp_digit)1 << (i % 64)))
            sp_256_mont_mul_order_avx2_4(t2, t2, a);
    }
    /* t2= a^ffffffff00000000ffffffffffffffffbce6f */
    sp_256_mont_sqr_n_order_avx2_4(t2, t2, 4);
    sp_256_mont_mul_order_avx2_4(t2, t2, t3);
    /* t2= a^ffffffff00000000ffffffffffffffffbce6faada7179e84 */
    for (i=107; i>=64; i--) {
        sp_256_mont_sqr_order_avx2_4(t2, t2);
        if (p256_order_low[i / 64] & ((sp_digit)1 << (i % 64)))
            sp_256_mont_mul_order_avx2_4(t2, t2, a);
    }
    /* t2= a^ffffffff00000000ffffffffffffffffbce6faada7179e84f */
    sp_256_mont_sqr_n_order_avx2_4(t2, t2, 4);
    sp_256_mont_mul_order_avx2_4(t2, t2, t3);
    /* t2= a^ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2 */
    for (i=59; i>=32; i--) {
        sp_256_mont_sqr_order_avx2_4(t2, t2);
        if (p256_order_low[i / 64] & ((sp_digit)1 << (i % 64)))
            sp_256_mont_mul_order_avx2_4(t2, t2, a);
    }
    /* t2= a^ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2f */
    sp_256_mont_sqr_n_order_avx2_4(t2, t2, 4);
    sp_256_mont_mul_order_avx2_4(t2, t2, t3);
    /* t2= a^ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc63254 */
    for (i=27; i>=0; i--) {
        sp_256_mont_sqr_order_avx2_4(t2, t2);
        if (p256_order_low[i / 64] & ((sp_digit)1 << (i % 64)))
            sp_256_mont_mul_order_avx2_4(t2, t2, a);
    }
    /* t2= a^ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632540 */
    sp_256_mont_sqr_n_order_avx2_4(t2, t2, 4);
    /* r = a^ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc63254f */
    sp_256_mont_mul_order_avx2_4(r, t2, t3);
#endif /* WOLFSSL_SP_SMALL */
}

#endif /* HAVE_INTEL_AVX2 */
#endif /* HAVE_ECC_SIGN || HAVE_ECC_VERIFY */
#ifdef HAVE_ECC_SIGN
#ifndef SP_ECC_MAX_SIG_GEN
#define SP_ECC_MAX_SIG_GEN  64
#endif

/* Sign the hash using the private key.
 *   e = [hash, 256 bits] from binary
 *   r = (k.G)->x mod order
 *   s = (r * x + e) / k mod order
 * The hash is truncated to the first 256 bits.
 *
 * hash     Hash to sign.
 * hashLen  Length of the hash data.
 * rng      Random number generator.
 * priv     Private part of key - scalar.
 * rm       First part of result as an mp_int.
 * sm       Sirst part of result as an mp_int.
 * heap     Heap to use for allocation.
 * returns RNG failures, MEMORY_E when memory allocation fails and
 * MP_OKAY on success.
 */
int sp_ecc_sign_256(const byte* hash, word32 hashLen, WC_RNG* rng, mp_int* priv,
                    mp_int* rm, mp_int* sm, void* heap)
{
#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    sp_digit* d = NULL;
#else
    sp_digit ed[2*4];
    sp_digit xd[2*4];
    sp_digit kd[2*4];
    sp_digit rd[2*4];
    sp_digit td[3 * 2*4];
    sp_point p;
#endif
    sp_digit* e = NULL;
    sp_digit* x = NULL;
    sp_digit* k = NULL;
    sp_digit* r = NULL;
    sp_digit* tmp = NULL;
    sp_point* point = NULL;
    sp_digit carry;
    sp_digit* s;
    sp_digit* kInv;
    int err = MP_OKAY;
    int64_t c;
    int i;
#ifdef HAVE_INTEL_AVX2
    word32 cpuid_flags = cpuid_get_flags();
#endif

    (void)heap;

    err = sp_ecc_point_new(heap, p, point);
#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (err == MP_OKAY) {
        d = (sp_digit*)XMALLOC(sizeof(sp_digit) * 7 * 2 * 4, heap,
                                                              DYNAMIC_TYPE_ECC);
        if (d != NULL) {
            e = d + 0 * 4;
            x = d + 2 * 4;
            k = d + 4 * 4;
            r = d + 6 * 4;
            tmp = d + 8 * 4;
        }
        else
            err = MEMORY_E;
    }
#else
    e = ed;
    x = xd;
    k = kd;
    r = rd;
    tmp = td;
#endif
    s = e;
    kInv = k;

    if (err == MP_OKAY) {
        if (hashLen > 32)
            hashLen = 32;

        sp_256_from_bin(e, 4, hash, hashLen);
    }

    for (i = SP_ECC_MAX_SIG_GEN; err == MP_OKAY && i > 0; i--) {
        sp_256_from_mp(x, 4, priv);

        /* New random point. */
        err = sp_256_ecc_gen_k_4(rng, k);
        if (err == MP_OKAY) {
#ifdef HAVE_INTEL_AVX2
            if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
                err = sp_256_ecc_mulmod_base_avx2_4(point, k, 1, heap);
            else
#endif
                err = sp_256_ecc_mulmod_base_4(point, k, 1, NULL);
        }

        if (err == MP_OKAY) {
            /* r = point->x mod order */
            XMEMCPY(r, point->x, sizeof(sp_digit) * 4);
            sp_256_norm_4(r);
            c = sp_256_cmp_4(r, p256_order);
            sp_256_cond_sub_4(r, r, p256_order, 0 - (c >= 0));
            sp_256_norm_4(r);

            /* Conv k to Montgomery form (mod order) */
#ifdef HAVE_INTEL_AVX2
            if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
                sp_256_mul_avx2_4(k, k, p256_norm_order);
            else
#endif
                sp_256_mul_4(k, k, p256_norm_order);
            err = sp_256_mod_4(k, k, p256_order);
        }
        if (err == MP_OKAY) {
            sp_256_norm_4(k);
            /* kInv = 1/k mod order */
#ifdef HAVE_INTEL_AVX2
            if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
                sp_256_mont_inv_order_avx2_4(kInv, k, tmp);
            else
#endif
                sp_256_mont_inv_order_4(kInv, k, tmp);
            sp_256_norm_4(kInv);

            /* s = r * x + e */
#ifdef HAVE_INTEL_AVX2
            if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
                sp_256_mul_avx2_4(x, x, r);
            else
#endif
                sp_256_mul_4(x, x, r);
            err = sp_256_mod_4(x, x, p256_order);
        }
        if (err == MP_OKAY) {
            sp_256_norm_4(x);
            carry = sp_256_add_4(s, e, x);
            sp_256_cond_sub_4(s, s, p256_order, 0 - carry);
            sp_256_norm_4(s);
            c = sp_256_cmp_4(s, p256_order);
            sp_256_cond_sub_4(s, s, p256_order, 0 - (c >= 0));
            sp_256_norm_4(s);

            /* s = s * k^-1 mod order */
#ifdef HAVE_INTEL_AVX2
            if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
                sp_256_mont_mul_order_avx2_4(s, s, kInv);
            else
#endif
                sp_256_mont_mul_order_4(s, s, kInv);
            sp_256_norm_4(s);

            /* Check that signature is usable. */
            if (!sp_256_iszero_4(s))
                break;
        }
    }

    if (i == 0)
        err = RNG_FAILURE_E;

    if (err == MP_OKAY)
        err = sp_256_to_mp(r, rm);
    if (err == MP_OKAY)
        err = sp_256_to_mp(s, sm);

#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (d != NULL) {
        XMEMSET(d, 0, sizeof(sp_digit) * 8 * 4);
        XFREE(d, heap, DYNAMIC_TYPE_ECC);
    }
#else
    XMEMSET(e, 0, sizeof(sp_digit) * 2 * 4);
    XMEMSET(x, 0, sizeof(sp_digit) * 2 * 4);
    XMEMSET(k, 0, sizeof(sp_digit) * 2 * 4);
    XMEMSET(r, 0, sizeof(sp_digit) * 2 * 4);
    XMEMSET(r, 0, sizeof(sp_digit) * 2 * 4);
    XMEMSET(tmp, 0, sizeof(sp_digit) * 3 * 2*4);
#endif
    sp_ecc_point_free(point, 1, heap);

    return err;
}
#endif /* HAVE_ECC_SIGN */

#ifdef HAVE_ECC_VERIFY
/* Verify the signature values with the hash and public key.
 *   e = Truncate(hash, 256)
 *   u1 = e/s mod order
 *   u2 = r/s mod order
 *   r == (u1.G + u2.Q)->x mod order
 * Optimization: Leave point in projective form.
 *   (x, y, 1) == (x' / z'*z', y' / z'*z'*z', z' / z')
 *   (r + n*order).z'.z' mod prime == (u1.G + u2.Q)->x'
 * The hash is truncated to the first 256 bits.
 *
 * hash     Hash to sign.
 * hashLen  Length of the hash data.
 * rng      Random number generator.
 * priv     Private part of key - scalar.
 * rm       First part of result as an mp_int.
 * sm       Sirst part of result as an mp_int.
 * heap     Heap to use for allocation.
 * returns RNG failures, MEMORY_E when memory allocation fails and
 * MP_OKAY on success.
 */
int sp_ecc_verify_256(const byte* hash, word32 hashLen, mp_int* pX,
    mp_int* pY, mp_int* pZ, mp_int* r, mp_int* sm, int* res, void* heap)
{
#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    sp_digit* d = NULL;
#else
    sp_digit u1d[2*4];
    sp_digit u2d[2*4];
    sp_digit sd[2*4];
    sp_digit tmpd[2*4 * 5];
    sp_point p1d;
    sp_point p2d;
#endif
    sp_digit* u1;
    sp_digit* u2;
    sp_digit* s;
    sp_digit* tmp;
    sp_point* p1;
    sp_point* p2 = NULL;
    sp_digit carry;
    int64_t c;
    int err;
#ifdef HAVE_INTEL_AVX2
    word32 cpuid_flags = cpuid_get_flags();
#endif

    err = sp_ecc_point_new(heap, p1d, p1);
    if (err == MP_OKAY)
        err = sp_ecc_point_new(heap, p2d, p2);
#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (err == MP_OKAY) {
        d = (sp_digit*)XMALLOC(sizeof(sp_digit) * 16 * 4, heap,
                                                              DYNAMIC_TYPE_ECC);
        if (d != NULL) {
            u1  = d + 0 * 4;
            u2  = d + 2 * 4;
            s   = d + 4 * 4;
            tmp = d + 6 * 4;
        }
        else
            err = MEMORY_E;
    }
#else
    u1 = u1d;
    u2 = u2d;
    s  = sd;
    tmp = tmpd;
#endif

    if (err == MP_OKAY) {
        if (hashLen > 32)
            hashLen = 32;

        sp_256_from_bin(u1, 4, hash, hashLen);
        sp_256_from_mp(u2, 4, r);
        sp_256_from_mp(s, 4, sm);
        sp_256_from_mp(p2->x, 4, pX);
        sp_256_from_mp(p2->y, 4, pY);
        sp_256_from_mp(p2->z, 4, pZ);

#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
            sp_256_mul_avx2_4(s, s, p256_norm_order);
        else
#endif
            sp_256_mul_4(s, s, p256_norm_order);
        err = sp_256_mod_4(s, s, p256_order);
    }
    if (err == MP_OKAY) {
        sp_256_norm_4(s);
#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags)) {
            sp_256_mont_inv_order_avx2_4(s, s, tmp);
            sp_256_mont_mul_order_avx2_4(u1, u1, s);
            sp_256_mont_mul_order_avx2_4(u2, u2, s);
        }
        else
#endif
        {
            sp_256_mont_inv_order_4(s, s, tmp);
            sp_256_mont_mul_order_4(u1, u1, s);
            sp_256_mont_mul_order_4(u2, u2, s);
        }

#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
            err = sp_256_ecc_mulmod_base_avx2_4(p1, u1, 0, heap);
        else
#endif
            err = sp_256_ecc_mulmod_base_4(p1, u1, 0, heap);
    }
    if (err == MP_OKAY) {
#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
            err = sp_256_ecc_mulmod_avx2_4(p2, p2, u2, 0, heap);
        else
#endif
            err = sp_256_ecc_mulmod_4(p2, p2, u2, 0, heap);
    }

    if (err == MP_OKAY) {
#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
            sp_256_proj_point_add_avx2_4(p1, p1, p2, tmp);
        else
#endif
            sp_256_proj_point_add_4(p1, p1, p2, tmp);

        /* (r + n*order).z'.z' mod prime == (u1.G + u2.Q)->x' */
        /* Reload r and convert to Montgomery form. */
        sp_256_from_mp(u2, 4, r);
        err = sp_256_mod_mul_norm_4(u2, u2, p256_mod);
    }

    if (err == MP_OKAY) {
        /* u1 = r.z'.z' mod prime */
        sp_256_mont_sqr_4(p1->z, p1->z, p256_mod, p256_mp_mod);
        sp_256_mont_mul_4(u1, u2, p1->z, p256_mod, p256_mp_mod);
        *res = sp_256_cmp_4(p1->x, u1) == 0;
        if (*res == 0) {
            /* Reload r and add order. */
            sp_256_from_mp(u2, 4, r);
            carry = sp_256_add_4(u2, u2, p256_order);
            /* Carry means result is greater than mod and is not valid. */
            if (!carry) {
                sp_256_norm_4(u2);

                /* Compare with mod and if greater or equal then not valid. */
                c = sp_256_cmp_4(u2, p256_mod);
                if (c < 0) {
                    /* Convert to Montogomery form */
                    err = sp_256_mod_mul_norm_4(u2, u2, p256_mod);
                    if (err == MP_OKAY) {
                        /* u1 = (r + 1*order).z'.z' mod prime */
                        sp_256_mont_mul_4(u1, u2, p1->z, p256_mod,
                                                                  p256_mp_mod);
                        *res = sp_256_cmp_4(p1->x, u2) == 0;
                    }
                }
            }
        }
    }

#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (d != NULL)
        XFREE(d, heap, DYNAMIC_TYPE_ECC);
#endif
    sp_ecc_point_free(p1, 0, heap);
    sp_ecc_point_free(p2, 0, heap);

    return err;
}
#endif /* HAVE_ECC_VERIFY */

#ifdef HAVE_ECC_CHECK_KEY
/* Check that the x and y oridinates are a valid point on the curve.
 *
 * point  EC point.
 * heap   Heap to use if dynamically allocating.
 * returns MEMORY_E if dynamic memory allocation fails, MP_VAL if the point is
 * not on the curve and MP_OKAY otherwise.
 */
static int sp_256_ecc_is_point_4(sp_point* point, void* heap)
{
#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    sp_digit* d = NULL;
#else
    sp_digit t1d[2*4];
    sp_digit t2d[2*4];
#endif
    sp_digit* t1;
    sp_digit* t2;
    int err = MP_OKAY;

#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    d = (sp_digit*)XMALLOC(sizeof(sp_digit) * 4 * 4, heap,
                                                              DYNAMIC_TYPE_ECC);
    if (d != NULL) {
        t1 = d + 0 * 4;
        t2 = d + 2 * 4;
    }
    else
        err = MEMORY_E;
#else
    (void)heap;

    t1 = t1d;
    t2 = t2d;
#endif

    if (err == MP_OKAY) {
        sp_256_sqr_4(t1, point->y);
        sp_256_mod_4(t1, t1, p256_mod);
        sp_256_sqr_4(t2, point->x);
        sp_256_mod_4(t2, t2, p256_mod);
        sp_256_mul_4(t2, t2, point->x);
        sp_256_mod_4(t2, t2, p256_mod);
	sp_256_sub_4(t2, p256_mod, t2);
        sp_256_mont_add_4(t1, t1, t2, p256_mod);

        sp_256_mont_add_4(t1, t1, point->x, p256_mod);
        sp_256_mont_add_4(t1, t1, point->x, p256_mod);
        sp_256_mont_add_4(t1, t1, point->x, p256_mod);

        if (sp_256_cmp_4(t1, p256_b) != 0)
            err = MP_VAL;
    }

#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (d != NULL)
        XFREE(d, heap, DYNAMIC_TYPE_ECC);
#endif

    return err;
}

/* Check that the x and y oridinates are a valid point on the curve.
 *
 * pX  X ordinate of EC point.
 * pY  Y ordinate of EC point.
 * returns MEMORY_E if dynamic memory allocation fails, MP_VAL if the point is
 * not on the curve and MP_OKAY otherwise.
 */
int sp_ecc_is_point_256(mp_int* pX, mp_int* pY)
{
#if !defined(WOLFSSL_SP_SMALL) && !defined(WOLFSSL_SMALL_STACK)
    sp_point pubd;
#endif
    sp_point* pub;
    byte one[1] = { 1 };
    int err;

    err = sp_ecc_point_new(NULL, pubd, pub);
    if (err == MP_OKAY) {
        sp_256_from_mp(pub->x, 4, pX);
        sp_256_from_mp(pub->y, 4, pY);
        sp_256_from_bin(pub->z, 4, one, sizeof(one));

        err = sp_256_ecc_is_point_4(pub, NULL);
    }

    sp_ecc_point_free(pub, 0, NULL);

    return err;
}

/* Check that the private scalar generates the EC point (px, py), the point is
 * on the curve and the point has the correct order.
 *
 * pX     X ordinate of EC point.
 * pY     Y ordinate of EC point.
 * privm  Private scalar that generates EC point.
 * returns MEMORY_E if dynamic memory allocation fails, MP_VAL if the point is
 * not on the curve, ECC_INF_E if the point does not have the correct order,
 * ECC_PRIV_KEY_E when the private scalar doesn't generate the EC point and
 * MP_OKAY otherwise.
 */
int sp_ecc_check_key_256(mp_int* pX, mp_int* pY, mp_int* privm, void* heap)
{
#if !defined(WOLFSSL_SP_SMALL) && !defined(WOLFSSL_SMALL_STACK)
    sp_digit privd[4];
    sp_point pubd;
    sp_point pd;
#endif
    sp_digit* priv = NULL;
    sp_point* pub;
    sp_point* p = NULL;
    byte one[1] = { 1 };
    int err;
#ifdef HAVE_INTEL_AVX2
    word32 cpuid_flags = cpuid_get_flags();
#endif

    err = sp_ecc_point_new(heap, pubd, pub);
    if (err == MP_OKAY)
        err = sp_ecc_point_new(heap, pd, p);
#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (err == MP_OKAY) {
        priv = (sp_digit*)XMALLOC(sizeof(sp_digit) * 4, heap,
                                                              DYNAMIC_TYPE_ECC);
        if (priv == NULL)
            err = MEMORY_E;
    }
#else
    priv = privd;
#endif

    if (err == MP_OKAY) {
        sp_256_from_mp(pub->x, 4, pX);
        sp_256_from_mp(pub->y, 4, pY);
        sp_256_from_bin(pub->z, 4, one, sizeof(one));
        sp_256_from_mp(priv, 4, privm);

        /* Check point at infinitiy. */
        if (sp_256_iszero_4(pub->x) &&
            sp_256_iszero_4(pub->y))
            err = ECC_INF_E;
    }

    if (err == MP_OKAY) {
        /* Check range of X and Y */
        if (sp_256_cmp_4(pub->x, p256_mod) >= 0 ||
            sp_256_cmp_4(pub->y, p256_mod) >= 0)
            err = ECC_OUT_OF_RANGE_E;
    }

    if (err == MP_OKAY) {
        /* Check point is on curve */
        err = sp_256_ecc_is_point_4(pub, heap);
    }

    if (err == MP_OKAY) {
        /* Point * order = infinity */
#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
            err = sp_256_ecc_mulmod_avx2_4(p, pub, p256_order, 1, heap);
        else
#endif
            err = sp_256_ecc_mulmod_4(p, pub, p256_order, 1, heap);
    }
    if (err == MP_OKAY) {
        /* Check result is infinity */
        if (!sp_256_iszero_4(p->x) ||
            !sp_256_iszero_4(p->y)) {
            err = ECC_INF_E;
        }
    }

    if (err == MP_OKAY) {
        /* Base * private = point */
#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
            err = sp_256_ecc_mulmod_base_avx2_4(p, priv, 1, heap);
        else
#endif
            err = sp_256_ecc_mulmod_base_4(p, priv, 1, heap);
    }
    if (err == MP_OKAY) {
        /* Check result is public key */
        if (sp_256_cmp_4(p->x, pub->x) != 0 ||
            sp_256_cmp_4(p->y, pub->y) != 0) {
            err = ECC_PRIV_KEY_E;
        }
    }

#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (priv != NULL)
        XFREE(priv, heap, DYNAMIC_TYPE_ECC);
#endif
    sp_ecc_point_free(p, 0, heap);
    sp_ecc_point_free(pub, 0, heap);

    return err;
}
#endif
#ifdef WOLFSSL_PUBLIC_ECC_ADD_DBL
/* Add two projective EC points together.
 * (pX, pY, pZ) + (qX, qY, qZ) = (rX, rY, rZ)
 *
 * pX   First EC point's X ordinate.
 * pY   First EC point's Y ordinate.
 * pZ   First EC point's Z ordinate.
 * qX   Second EC point's X ordinate.
 * qY   Second EC point's Y ordinate.
 * qZ   Second EC point's Z ordinate.
 * rX   Resultant EC point's X ordinate.
 * rY   Resultant EC point's Y ordinate.
 * rZ   Resultant EC point's Z ordinate.
 * returns MEMORY_E if dynamic memory allocation fails and MP_OKAY otherwise.
 */
int sp_ecc_proj_add_point_256(mp_int* pX, mp_int* pY, mp_int* pZ,
                              mp_int* qX, mp_int* qY, mp_int* qZ,
                              mp_int* rX, mp_int* rY, mp_int* rZ)
{
#if !defined(WOLFSSL_SP_SMALL) && !defined(WOLFSSL_SMALL_STACK)
    sp_digit tmpd[2 * 4 * 5];
    sp_point pd;
    sp_point qd;
#endif
    sp_digit* tmp;
    sp_point* p;
    sp_point* q = NULL;
    int err;
#ifdef HAVE_INTEL_AVX2
    word32 cpuid_flags = cpuid_get_flags();
#endif

    err = sp_ecc_point_new(NULL, pd, p);
    if (err == MP_OKAY)
        err = sp_ecc_point_new(NULL, qd, q);
#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (err == MP_OKAY) {
        tmp = (sp_digit*)XMALLOC(sizeof(sp_digit) * 2 * 4 * 5, NULL,
                                                              DYNAMIC_TYPE_ECC);
        if (tmp == NULL)
            err = MEMORY_E;
    }
#else
    tmp = tmpd;
#endif

    if (err == MP_OKAY) {
        sp_256_from_mp(p->x, 4, pX);
        sp_256_from_mp(p->y, 4, pY);
        sp_256_from_mp(p->z, 4, pZ);
        sp_256_from_mp(q->x, 4, qX);
        sp_256_from_mp(q->y, 4, qY);
        sp_256_from_mp(q->z, 4, qZ);

#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
            sp_256_proj_point_add_avx2_4(p, p, q, tmp);
        else
#endif
            sp_256_proj_point_add_4(p, p, q, tmp);
    }

    if (err == MP_OKAY)
        err = sp_256_to_mp(p->x, rX);
    if (err == MP_OKAY)
        err = sp_256_to_mp(p->y, rY);
    if (err == MP_OKAY)
        err = sp_256_to_mp(p->z, rZ);

#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (tmp != NULL)
        XFREE(tmp, NULL, DYNAMIC_TYPE_ECC);
#endif
    sp_ecc_point_free(q, 0, NULL);
    sp_ecc_point_free(p, 0, NULL);

    return err;
}

/* Double a projective EC point.
 * (pX, pY, pZ) + (pX, pY, pZ) = (rX, rY, rZ)
 *
 * pX   EC point's X ordinate.
 * pY   EC point's Y ordinate.
 * pZ   EC point's Z ordinate.
 * rX   Resultant EC point's X ordinate.
 * rY   Resultant EC point's Y ordinate.
 * rZ   Resultant EC point's Z ordinate.
 * returns MEMORY_E if dynamic memory allocation fails and MP_OKAY otherwise.
 */
int sp_ecc_proj_dbl_point_256(mp_int* pX, mp_int* pY, mp_int* pZ,
                              mp_int* rX, mp_int* rY, mp_int* rZ)
{
#if !defined(WOLFSSL_SP_SMALL) && !defined(WOLFSSL_SMALL_STACK)
    sp_digit tmpd[2 * 4 * 2];
    sp_point pd;
#endif
    sp_digit* tmp;
    sp_point* p;
    int err;
#ifdef HAVE_INTEL_AVX2
    word32 cpuid_flags = cpuid_get_flags();
#endif

    err = sp_ecc_point_new(NULL, pd, p);
#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (err == MP_OKAY) {
        tmp = (sp_digit*)XMALLOC(sizeof(sp_digit) * 2 * 4 * 2, NULL,
                                                              DYNAMIC_TYPE_ECC);
        if (tmp == NULL)
            err = MEMORY_E;
    }
#else
    tmp = tmpd;
#endif

    if (err == MP_OKAY) {
        sp_256_from_mp(p->x, 4, pX);
        sp_256_from_mp(p->y, 4, pY);
        sp_256_from_mp(p->z, 4, pZ);

#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags))
            sp_256_proj_point_dbl_avx2_4(p, p, tmp);
        else
#endif
            sp_256_proj_point_dbl_4(p, p, tmp);
    }

    if (err == MP_OKAY)
        err = sp_256_to_mp(p->x, rX);
    if (err == MP_OKAY)
        err = sp_256_to_mp(p->y, rY);
    if (err == MP_OKAY)
        err = sp_256_to_mp(p->z, rZ);

#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (tmp != NULL)
        XFREE(tmp, NULL, DYNAMIC_TYPE_ECC);
#endif
    sp_ecc_point_free(p, 0, NULL);

    return err;
}

/* Map a projective EC point to affine in place.
 * pZ will be one.
 *
 * pX   EC point's X ordinate.
 * pY   EC point's Y ordinate.
 * pZ   EC point's Z ordinate.
 * returns MEMORY_E if dynamic memory allocation fails and MP_OKAY otherwise.
 */
int sp_ecc_map_256(mp_int* pX, mp_int* pY, mp_int* pZ)
{
#if !defined(WOLFSSL_SP_SMALL) && !defined(WOLFSSL_SMALL_STACK)
    sp_digit tmpd[2 * 4 * 4];
    sp_point pd;
#endif
    sp_digit* tmp;
    sp_point* p;
    int err;

    err = sp_ecc_point_new(NULL, pd, p);
#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (err == MP_OKAY) {
        tmp = (sp_digit*)XMALLOC(sizeof(sp_digit) * 2 * 4 * 4, NULL,
                                                              DYNAMIC_TYPE_ECC);
        if (tmp == NULL)
            err = MEMORY_E;
    }
#else
    tmp = tmpd;
#endif
    if (err == MP_OKAY) {
        sp_256_from_mp(p->x, 4, pX);
        sp_256_from_mp(p->y, 4, pY);
        sp_256_from_mp(p->z, 4, pZ);

        sp_256_map_4(p, p, tmp);
    }

    if (err == MP_OKAY)
        err = sp_256_to_mp(p->x, pX);
    if (err == MP_OKAY)
        err = sp_256_to_mp(p->y, pY);
    if (err == MP_OKAY)
        err = sp_256_to_mp(p->z, pZ);

#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (tmp != NULL)
        XFREE(tmp, NULL, DYNAMIC_TYPE_ECC);
#endif
    sp_ecc_point_free(p, 0, NULL);

    return err;
}
#endif /* WOLFSSL_PUBLIC_ECC_ADD_DBL */
#ifdef HAVE_COMP_KEY
/* Find the square root of a number mod the prime of the curve.
 *
 * y  The number to operate on and the result.
 * returns MEMORY_E if dynamic memory allocation fails and MP_OKAY otherwise.
 */
static int sp_256_mont_sqrt_4(sp_digit* y)
{
#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    sp_digit* d;
#else
    sp_digit t1d[2 * 4];
    sp_digit t2d[2 * 4];
#endif
    sp_digit* t1;
    sp_digit* t2;
    int err = MP_OKAY;
#ifdef HAVE_INTEL_AVX2
    word32 cpuid_flags = cpuid_get_flags();
#endif

#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    d = (sp_digit*)XMALLOC(sizeof(sp_digit) * 4 * 4, NULL, DYNAMIC_TYPE_ECC);
    if (d != NULL) {
        t1 = d + 0 * 4;
        t2 = d + 2 * 4;
    }
    else
        err = MEMORY_E;
#else
    t1 = t1d;
    t2 = t2d;
#endif

    if (err == MP_OKAY) {
#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags)) {
            /* t2 = y ^ 0x2 */
            sp_256_mont_sqr_avx2_4(t2, y, p256_mod, p256_mp_mod);
            /* t1 = y ^ 0x3 */
            sp_256_mont_mul_avx2_4(t1, t2, y, p256_mod, p256_mp_mod);
            /* t2 = y ^ 0xc */
            sp_256_mont_sqr_n_avx2_4(t2, t1, 2, p256_mod, p256_mp_mod);
            /* t1 = y ^ 0xf */
            sp_256_mont_mul_avx2_4(t1, t1, t2, p256_mod, p256_mp_mod);
            /* t2 = y ^ 0xf0 */
            sp_256_mont_sqr_n_avx2_4(t2, t1, 4, p256_mod, p256_mp_mod);
            /* t1 = y ^ 0xff */
            sp_256_mont_mul_avx2_4(t1, t1, t2, p256_mod, p256_mp_mod);
            /* t2 = y ^ 0xff00 */
            sp_256_mont_sqr_n_avx2_4(t2, t1, 8, p256_mod, p256_mp_mod);
            /* t1 = y ^ 0xffff */
            sp_256_mont_mul_avx2_4(t1, t1, t2, p256_mod, p256_mp_mod);
            /* t2 = y ^ 0xffff0000 */
            sp_256_mont_sqr_n_avx2_4(t2, t1, 16, p256_mod, p256_mp_mod);
            /* t1 = y ^ 0xffffffff */
            sp_256_mont_mul_avx2_4(t1, t1, t2, p256_mod, p256_mp_mod);
            /* t1 = y ^ 0xffffffff00000000 */
            sp_256_mont_sqr_n_avx2_4(t1, t1, 32, p256_mod, p256_mp_mod);
            /* t1 = y ^ 0xffffffff00000001 */
            sp_256_mont_mul_avx2_4(t1, t1, y, p256_mod, p256_mp_mod);
            /* t1 = y ^ 0xffffffff00000001000000000000000000000000 */
            sp_256_mont_sqr_n_avx2_4(t1, t1, 96, p256_mod, p256_mp_mod);
            /* t1 = y ^ 0xffffffff00000001000000000000000000000001 */
            sp_256_mont_mul_avx2_4(t1, t1, y, p256_mod, p256_mp_mod);
            sp_256_mont_sqr_n_avx2_4(y, t1, 94, p256_mod, p256_mp_mod);
        }
        else
#endif
        {
            /* t2 = y ^ 0x2 */
            sp_256_mont_sqr_4(t2, y, p256_mod, p256_mp_mod);
            /* t1 = y ^ 0x3 */
            sp_256_mont_mul_4(t1, t2, y, p256_mod, p256_mp_mod);
            /* t2 = y ^ 0xc */
            sp_256_mont_sqr_n_4(t2, t1, 2, p256_mod, p256_mp_mod);
            /* t1 = y ^ 0xf */
            sp_256_mont_mul_4(t1, t1, t2, p256_mod, p256_mp_mod);
            /* t2 = y ^ 0xf0 */
            sp_256_mont_sqr_n_4(t2, t1, 4, p256_mod, p256_mp_mod);
            /* t1 = y ^ 0xff */
            sp_256_mont_mul_4(t1, t1, t2, p256_mod, p256_mp_mod);
            /* t2 = y ^ 0xff00 */
            sp_256_mont_sqr_n_4(t2, t1, 8, p256_mod, p256_mp_mod);
            /* t1 = y ^ 0xffff */
            sp_256_mont_mul_4(t1, t1, t2, p256_mod, p256_mp_mod);
            /* t2 = y ^ 0xffff0000 */
            sp_256_mont_sqr_n_4(t2, t1, 16, p256_mod, p256_mp_mod);
            /* t1 = y ^ 0xffffffff */
            sp_256_mont_mul_4(t1, t1, t2, p256_mod, p256_mp_mod);
            /* t1 = y ^ 0xffffffff00000000 */
            sp_256_mont_sqr_n_4(t1, t1, 32, p256_mod, p256_mp_mod);
            /* t1 = y ^ 0xffffffff00000001 */
            sp_256_mont_mul_4(t1, t1, y, p256_mod, p256_mp_mod);
            /* t1 = y ^ 0xffffffff00000001000000000000000000000000 */
            sp_256_mont_sqr_n_4(t1, t1, 96, p256_mod, p256_mp_mod);
            /* t1 = y ^ 0xffffffff00000001000000000000000000000001 */
            sp_256_mont_mul_4(t1, t1, y, p256_mod, p256_mp_mod);
            sp_256_mont_sqr_n_4(y, t1, 94, p256_mod, p256_mp_mod);
        }
    }

#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (d != NULL)
        XFREE(d, NULL, DYNAMIC_TYPE_ECC);
#endif

    return err;
}

/* Uncompress the point given the X ordinate.
 *
 * xm    X ordinate.
 * odd   Whether the Y ordinate is odd.
 * ym    Calculated Y ordinate.
 * returns MEMORY_E if dynamic memory allocation fails and MP_OKAY otherwise.
 */
int sp_ecc_uncompress_256(mp_int* xm, int odd, mp_int* ym)
{
#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    sp_digit* d;
#else
    sp_digit xd[2 * 4];
    sp_digit yd[2 * 4];
#endif
    sp_digit* x;
    sp_digit* y;
    int err = MP_OKAY;
#ifdef HAVE_INTEL_AVX2
    word32 cpuid_flags = cpuid_get_flags();
#endif

#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    d = (sp_digit*)XMALLOC(sizeof(sp_digit) * 4 * 4, NULL, DYNAMIC_TYPE_ECC);
    if (d != NULL) {
        x = d + 0 * 4;
        y = d + 2 * 4;
    }
    else
        err = MEMORY_E;
#else
    x = xd;
    y = yd;
#endif

    if (err == MP_OKAY) {
        sp_256_from_mp(x, 4, xm);

        err = sp_256_mod_mul_norm_4(x, x, p256_mod);
    }

    if (err == MP_OKAY) {
        /* y = x^3 */
#ifdef HAVE_INTEL_AVX2
        if (IS_INTEL_BMI2(cpuid_flags) && IS_INTEL_ADX(cpuid_flags)) {
            sp_256_mont_sqr_avx2_4(y, x, p256_mod, p256_mp_mod);
            sp_256_mont_mul_avx2_4(y, y, x, p256_mod, p256_mp_mod);
        }
        else
#endif
        {
            sp_256_mont_sqr_4(y, x, p256_mod, p256_mp_mod);
            sp_256_mont_mul_4(y, y, x, p256_mod, p256_mp_mod);
        }
        /* y = x^3 - 3x */
        sp_256_mont_sub_4(y, y, x, p256_mod);
        sp_256_mont_sub_4(y, y, x, p256_mod);
        sp_256_mont_sub_4(y, y, x, p256_mod);
        /* y = x^3 - 3x + b */
        err = sp_256_mod_mul_norm_4(x, p256_b, p256_mod);
    }
    if (err == MP_OKAY) {
        sp_256_mont_add_4(y, y, x, p256_mod);
        /* y = sqrt(x^3 - 3x + b) */
        err = sp_256_mont_sqrt_4(y);
    }
    if (err == MP_OKAY) {
        XMEMSET(y + 4, 0, 4 * sizeof(sp_digit));
        sp_256_mont_reduce_4(y, p256_mod, p256_mp_mod);
        if (((y[0] ^ odd) & 1) != 0)
            sp_256_mont_sub_4(y, p256_mod, y, p256_mod);

        err = sp_256_to_mp(y, ym);
    }

#if defined(WOLFSSL_SP_SMALL) || defined(WOLFSSL_SMALL_STACK)
    if (d != NULL)
        XFREE(d, NULL, DYNAMIC_TYPE_ECC);
#endif

    return err;
}
#endif
#endif /* !WOLFSSL_SP_NO_256 */
#endif /* WOLFSSL_HAVE_SP_ECC */
#endif /* WOLFSSL_SP_X86_64_ASM */
#endif /* WOLFSSL_HAVE_SP_RSA || WOLFSSL_HAVE_SP_DH || WOLFSSL_HAVE_SP_ECC */
