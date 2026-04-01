/*
 * random.c - Pseudo-Random Number Generator (PRNG)
 *
 * ============================================================================
 * HOW RANDOMNESS WORKS ON THE GBA
 * ============================================================================
 *
 * The GBA has NO hardware random number generator. All "randomness" in the
 * game comes from this simple mathematical formula called a Linear
 * Congruential Generator (LCG).
 *
 * The formula:  newValue = oldValue * 1103515245 + 24691
 *
 * This produces a DETERMINISTIC sequence of numbers. Given the same seed,
 * you ALWAYS get the same sequence. This is actually useful for debugging
 * and competitive play (TAS - Tool-Assisted Speedruns).
 *
 * The "randomness" comes from the SEED, which is set from a hardware timer
 * (Timer 1) during startup. Timer 1 counts at the CPU clock rate (16.78 MHz),
 * so its value depends on the exact microsecond the player presses a button.
 * This makes the starting seed effectively unpredictable.
 *
 * Additionally, Random() is called every VBlank (from VBlankIntr in main.c),
 * even when no random number is needed. This means the PRNG state advances
 * ~60 times per second continuously, making the sequence harder to predict
 * or manipulate.
 *
 * IMPORTANT: Only the UPPER 16 bits of the 32-bit state are returned.
 * The lower bits of an LCG have shorter periods and are less "random".
 * Returning the upper 16 bits gives better distribution.
 *
 * ============================================================================
 */

#include "global.h"
#include "random.h"

/*
 * The 32-bit PRNG state variable.
 * Stored in IWRAM (COMMON_DATA) because it's accessed every frame.
 */
COMMON_DATA u32 gRngValue = 0;

/**
 * FUNCTION: Random
 *
 * PURPOSE: Generate a 16-bit pseudo-random number (0-65535).
 *
 * HOW IT WORKS:
 * Applies the LCG formula to advance the state, then returns the
 * upper 16 bits (bits 16-31) of the result.
 *
 * The constants 1103515245 and 24691 come from the ISO C standard's
 * example implementation of rand(). They produce a full-period sequence
 * (the state cycles through all 2^32 possible values before repeating).
 *
 * RETURNS: A pseudo-random u16 value (0x0000-0xFFFF)
 */
u16 Random(void)
{
    gRngValue = ISO_RANDOMIZE1(gRngValue);
    return gRngValue >> 16;  /* Return upper 16 bits for better randomness */
}

/**
 * FUNCTION: SeedRng
 *
 * PURPOSE: Set the initial state of the PRNG.
 *
 * HOW IT WORKS:
 * Simply sets gRngValue to the given seed. All subsequent Random() calls
 * will produce a deterministic sequence based on this seed.
 * Called during game startup with a value from hardware Timer 1.
 *
 * @param seed — Initial seed value (typically from Timer 1)
 */
void SeedRng(u16 seed)
{
    gRngValue = seed;
}
