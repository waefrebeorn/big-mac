/* wb_ftz.c — Flush-To-Zero / Denormals-Are-Zero control.
 *
 * R077: Critical performance optimization for real-time audio.
 * Denormal floats (values < 1.18e-38) trigger microcode assists
 * that are 10-100x slower than normal FP operations.
 *
 * Setting FTZ + DAZ eliminates this penalty entirely.
 *
 * Call wb_ftz_enable() once at engine startup.
 * Pure C11, platform-specific intrinsics.
 */

#include <stdint.h>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#include <emmintrin.h>
#endif

/* Enable FTZ (Flush-To-Zero) and DAZ (Denormals-Are-Zero).
 * Call once at engine startup before any audio processing.
 * Returns 0 on success, -1 on unsupported platform. */
int wb_ftz_enable(void) {
#if defined(__x86_64__) || defined(__i386__)
    /* Get current MXCSR */
    unsigned int mxcsr = _mm_getcsr();

    /* Set FTZ (bit 15) and DAZ (bit 6) */
    mxcsr |= (1 << 15);  /* FTZ */
    mxcsr |= (1 << 6);   /* DAZ */

    /* Also set round-to-nearest (default) and mask all FP exceptions */
    mxcsr &= ~(0x1F << 7);  /* Clear exception mask bits */
    mxcsr |= (0x1F << 7);   /* Mask all FP exceptions (default) */

    _mm_setcsr(mxcsr);
    return 0;
#elif defined(__ARM_NEON) || defined(__aarch64__)
    /* ARM64: Set FZ bit in FPCR */
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ULL << 24);  /* FZ (Flush-to-Zero) */
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
    return 0;
#else
    return -1;  /* Unsupported platform */
#endif
}

/* Disable FTZ/DAZ (restore default behavior). */
int wb_ftz_disable(void) {
#if defined(__x86_64__) || defined(__i386__)
    unsigned int mxcsr = _mm_getcsr();
    mxcsr &= ~(1 << 15);  /* Clear FTZ */
    mxcsr &= ~(1 << 6);   /* Clear DAZ */
    _mm_setcsr(mxcsr);
    return 0;
#elif defined(__ARM_NEON) || defined(__aarch64__)
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr &= ~(1ULL << 24);  /* Clear FZ */
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
    return 0;
#else
    return -1;
#endif
}

/* Check if FTZ is currently enabled. */
int wb_ftz_is_enabled(void) {
#if defined(__x86_64__) || defined(__i386__)
    unsigned int mxcsr = _mm_getcsr();
    return (mxcsr & (1 << 15)) ? 1 : 0;
#else
    return -1;
#endif
}
