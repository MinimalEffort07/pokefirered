/*
 * math_util.c - Fixed-Point Math Operations
 *
 * ============================================================================
 * FIXED-POINT ARITHMETIC ON THE GBA
 * ============================================================================
 *
 * The ARM7TDMI CPU has NO floating-point hardware. Operations like
 * multiplication and division of decimal numbers (1.5 * 2.3) must be
 * done using INTEGER math with a technique called "fixed-point arithmetic".
 *
 * FIXED-POINT BASICS:
 *   Treat an integer as having an implicit "decimal point" at a fixed position.
 *   For example, in Q8.8 format:
 *     - The top 8 bits are the INTEGER part
 *     - The bottom 8 bits are the FRACTIONAL part
 *     - To represent 1.5: integer=1, fraction=128/256=0.5 -> value = 384 (0x0180)
 *     - To represent 2.0: integer=2, fraction=0 -> value = 512 (0x0200)
 *
 * Q NOTATION:
 *   "Q8.8" means 8 integer bits and 8 fractional bits (16-bit total).
 *   "Q24.8" means 24 integer bits and 8 fractional bits (32-bit total).
 *   "QN.S" means N integer bits and S fractional bits (variable).
 *
 * WHY FIXED-POINT?
 *   - Integer add/subtract works the same as fixed-point (just add the numbers)
 *   - Integer comparison works the same
 *   - Multiplication: multiply then shift right by fractional bits
 *   - Division: shift left by fractional bits then divide
 *   - Much faster than software floating-point (ARM FP emulation ~100+ cycles)
 *
 * MULTIPLICATION:
 *   For Q8.8: result = (a * b) >> 8
 *   The multiplication gives a result with 16 fractional bits (8+8),
 *   so we shift right by 8 to get back to 8 fractional bits.
 *   We divide by 256 (= >> 8) instead of shifting because signed
 *   right-shift behavior is implementation-defined in C.
 *
 * DIVISION:
 *   For Q8.8: result = (a << 8) / b
 *   We shift the numerator left first to preserve fractional precision,
 *   then divide. Without the shift, we'd lose all fractional bits.
 *
 * INVERSE (RECIPROCAL):
 *   For Q8.8: result = (1.0 << 16) / y = 0x10000 / y
 *   This computes 1/y in Q8.8 format. 0x10000 = 1.0 in Q16.0 format,
 *   and dividing by y gives the result in Q8.8.
 *
 * ============================================================================
 */

#include "global.h"
#include "math_util.h"

/**
 * FUNCTION: Q_8_8_mul
 *
 * PURPOSE: Multiply two Q8.8 fixed-point numbers.
 *
 * HOW IT WORKS:
 * Multiplies x * y (both Q8.8), producing a 32-bit intermediate result
 * with 16 fractional bits, then divides by 256 to shift back to Q8.8.
 * The intermediate is stored as s32 to avoid overflow (s16 * s16 can exceed s16).
 *
 * @param x — First factor (Q8.8 fixed-point, range: -128.0 to +127.996)
 * @param y — Second factor (Q8.8 fixed-point)
 *
 * RETURNS: x * y in Q8.8 format
 */
s16 Q_8_8_mul(s16 x, s16 y)
{
    s32 result;

    result = x;
    result *= y;
    result /= 256;  /* Equivalent to >> 8, but handles signed values correctly */
    return result;
}

/**
 * FUNCTION: Q_N_S_mul
 *
 * PURPOSE: Multiply two fixed-point numbers with variable fractional bits.
 *
 * HOW IT WORKS:
 * Same as Q_8_8_mul but the number of fractional bits 's' is a parameter.
 * Divides by 2^s instead of the fixed 256 (2^8).
 *
 * @param s — Number of fractional bits
 * @param x — First factor (QN.S fixed-point)
 * @param y — Second factor (QN.S fixed-point)
 *
 * RETURNS: x * y in QN.S format
 */
s16 Q_N_S_mul(u8 s, s16 x, s16 y)
{
    s32 result;

    result = x;
    result *= y;
    result /= (1 << s);  /* Divide by 2^s to restore fractional bit count */
    return result;
}

/**
 * FUNCTION: Q_24_8_mul
 *
 * PURPOSE: Multiply two Q24.8 fixed-point numbers (32-bit precision).
 *
 * HOW IT WORKS:
 * Same principle but uses 64-bit intermediate to prevent overflow.
 * s32 * s32 can exceed 32 bits, so we use s64 for the multiplication.
 *
 * @param x — First factor (Q24.8 fixed-point, 32-bit)
 * @param y — Second factor (Q24.8 fixed-point, 32-bit)
 *
 * RETURNS: x * y in Q24.8 format
 */
s32 Q_24_8_mul(s32 x, s32 y)
{
    s64 result;

    result = x;
    result *= y;
    result /= 256;
    return result;
}

/**
 * FUNCTION: Q_8_8_div
 *
 * PURPOSE: Divide two Q8.8 fixed-point numbers.
 *
 * HOW IT WORKS:
 * Shifts x left by 8 bits to add 8 extra fractional bits of precision,
 * then divides by y. The shift compensates for the fractional bits that
 * would be lost during integer division.
 *
 * Returns 0 for division by zero (instead of crashing).
 *
 * @param x — Numerator (Q8.8 fixed-point)
 * @param y — Denominator (Q8.8 fixed-point, must not be 0)
 *
 * RETURNS: x / y in Q8.8 format, or 0 if y == 0
 */
s16 Q_8_8_div(s16 x, s16 y)
{
    if (y == 0)
    {
        return 0;
    }
    return (x << 8) / y;
}

/**
 * FUNCTION: Q_N_S_div
 *
 * PURPOSE: Divide two fixed-point numbers with variable fractional bits.
 *
 * @param s — Number of fractional bits
 * @param x — Numerator (QN.S fixed-point)
 * @param y — Denominator (QN.S fixed-point)
 *
 * RETURNS: x / y in QN.S format, or 0 if y == 0
 */
s16 Q_N_S_div(u8 s, s16 x, s16 y)
{
    if (y == 0)
    {
        return 0;
    }
    return (x << s) / y;
}

/**
 * FUNCTION: Q_24_8_div
 *
 * PURPOSE: Divide two Q24.8 fixed-point numbers (32-bit precision).
 *
 * HOW IT WORKS:
 * Uses s64 intermediate to prevent overflow when shifting the 32-bit
 * numerator left by 8 bits (which could exceed 32 bits).
 *
 * @param x — Numerator (Q24.8 fixed-point)
 * @param y — Denominator (Q24.8 fixed-point)
 *
 * RETURNS: x / y in Q24.8 format, or 0 if y == 0
 */
s32 Q_24_8_div(s32 x, s32 y)
{
    s64 _x;

    if (y == 0)
    {
        return 0;
    }
    _x = x;
    _x *= 256;  /* Same as << 8 but avoids signed shift issues */
    return _x / y;
}

/**
 * FUNCTION: Q_8_8_inv
 *
 * PURPOSE: Calculate the reciprocal (1/y) in Q8.8 fixed-point.
 *
 * HOW IT WORKS:
 * 1.0 in Q8.8 = 0x100 (256). But we need Q8.8 * Q8.8 = Q16.16,
 * then divide to get Q8.8. So we use 0x10000 (= 1.0 in Q16.0)
 * divided by y to get the result in Q8.8.
 *
 * @param y — Value to invert (Q8.8 fixed-point, must not be 0)
 *
 * RETURNS: 1/y in Q8.8 format
 */
s16 Q_8_8_inv(s16 y)
{
    s32 x;

    x = 0x10000;  /* 1.0 in Q16.0 = 65536 */
    return x / y;
}

/**
 * FUNCTION: Q_N_S_inv
 *
 * PURPOSE: Calculate the reciprocal (1/y) with variable fractional bits.
 *
 * @param s — Number of fractional bits
 * @param y — Value to invert
 *
 * RETURNS: 1/y in QN.S format
 */
s16 Q_N_S_inv(u8 s, s16 y)
{
    s32 x;

    x = 0x100 << s;  /* 1.0 in Q(8+S).0 format */
    return x / y;
}

/**
 * FUNCTION: Q_24_8_inv
 *
 * PURPOSE: Calculate the reciprocal (1/y) in Q24.8 fixed-point.
 *
 * @param y — Value to invert (Q24.8 fixed-point, must not be 0)
 *
 * RETURNS: 1/y in Q24.8 format
 */
s32 Q_24_8_inv(s32 y)
{
    s64 x;

    x = 0x10000;  /* 1.0 in Q16.0, using s64 to prevent overflow */
    return x / y;
}
