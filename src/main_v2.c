/*
 * sweep - the RGBX v2 sample extension.
 *
 * This is the source the `rgbx-v2` target compiles into a device-loadable
 * .rgbx package. It is deliberately small: v2 runs guests in a memoryless
 * WebAssembly sandbox, so the interesting part of the file is which C
 * constructs survive that profile, not the animation.
 *
 * The effect: a single bright column sweeps left to right with a fading
 * tail, tinted by the Color parameter and paced by Speed. Pressing any
 * button widens the tail while it is held.
 *
 * WHAT THE V2 PROFILE TAKES AWAY (all of it enforced by the SDK's post-link
 * gate, so a violation is a build failure, not a device-side surprise):
 *
 *  - No linear memory at all. The module carries no memory, table, data or
 *    element section, and the gate rejects every load/store opcode. That
 *    rules out arrays, pointers, string literals, struct returns by
 *    reference, and anything the compiler would spill to a stack frame.
 *  - No floating point. Integer opcodes only; write fixed-point instead.
 *    Audio energies and IMU readings already arrive as integers.
 *  - No framebuffer. Pixels leave the guest through span calls: 60 ordered
 *    calls covering pixels 0, 8, ... 472 exactly once per tick. The oracle
 *    replays a tick and counts them, so a partially painted frame fails the
 *    build.
 *  - At most 8 functions, 8 globals and 32 locals per function, and 2048
 *    module bytes. Every one of those numbers comes from the release's
 *    sdk-manifest.json, not from anything written down here. One of the
 *    globals is not yours: wasm-ld emits __stack_pointer even for a module
 *    with no memory, and the gate counts it, so seven are left.
 *
 * STATE WITHOUT MEMORY: a variable in address space 1 becomes a real
 * WebAssembly global instead of a memory slot, which is the only mutable
 * state a memoryless module can hold. Read and write it like any other
 * variable; clang lowers the access to global.get/global.set. Anything that
 * needs more than a handful of scalars does not fit this profile.
 *
 * Treat globals as reset on every activation, the same rule the .llext
 * sandbox has: persist nothing across selections.
 */

#include <rgbx/rgbx_v2.h>

/* Parameter slots, matching the "parameters" array in rgbx-v2.json. The
 * manifest names them for the companion app; the guest only sees indices. */
#define P_SPEED 0u
#define P_COLOR 1u

/* Milliseconds the sweep spends on one column at Speed 50 (== 1x), so a
 * full pass takes MS_PER_COLUMN * 40 = 1000 ms. */
#define MS_PER_COLUMN 25u
#define PERIOD_MS (RGBX_V2_WIDTH * MS_PER_COLUMN)

/* Tail length in columns, and its widened form while a button is held. */
#define TAIL_COLUMNS 8u
#define TAIL_COLUMNS_HELD 16u

/* Luma drop from the head to the far end of the tail. Keeping it below 255
 * leaves the last column faintly lit rather than black. */
#define TAIL_FALLOFF 240u

/* The sweep position, in milliseconds into the current pass. Address space 1
 * is what makes this a WebAssembly global rather than a memory slot. */
static __attribute__((address_space(1))) uint32_t phase_ms = 0u;

/* Brightness of column `x` given the head column and the tail length. The
 * result is always 0..255, which the span import requires. */
static inline uint32_t luma_at(uint32_t x, uint32_t head, uint32_t tail)
{
    /* How far `x` sits BEHIND the head, wrapped, so the tail trails the
     * moving column and crosses column 0 without a discontinuity. Subtracting
     * the other way round would light the columns the sweep is about to reach
     * instead of the ones it just left. */
    const uint32_t distance = (head + RGBX_V2_WIDTH - x) % RGBX_V2_WIDTH;

    return distance < tail ? 255u - (TAIL_FALLOFF * distance) / tail : 0u;
}

RGBX_V2_EXPORT("rgbx_init") void rgbx_init(void)
{
    /* Host calls are rejected during init; this may only touch guest state. */
    phase_ms = 0u;
}

RGBX_V2_EXPORT("rgbx_tick") void rgbx_tick(uint32_t dt_ms)
{
    const uint32_t speed = rgbx_v2_param_u32(P_SPEED);
    const uint32_t color = rgbx_v2_param_u32(P_COLOR) & 0x00ffffffu;
    /* The gate's oracle answers every sensor read with zero, so this branch is
     * never taken while the build is being checked. Both tail lengths keep the
     * luma below 256 and the span count unchanged by construction, which is
     * the standard a sensor-driven branch has to meet here. */
    const uint32_t buttons = rgbx_v2_input_u32(RGBX_V2_INPUT_BUTTONS_PRESSED, 0u);
    const uint32_t tail = buttons != 0u ? TAIL_COLUMNS_HELD : TAIL_COLUMNS;

    /* Wrapping the accumulator at exactly one pass keeps the argument bounded
     * for any dt and any Speed, including the extremes the gate's oracle
     * replays. Unsigned overflow wraps; it never traps. Do not divide by a
     * parameter or an input, though - an integer division by zero is a trap,
     * and the oracle will find it. */
    const uint32_t previous_ms = phase_ms;

    phase_ms = (previous_ms + dt_ms * speed / 50u) % PERIOD_MS;
    const uint32_t head = phase_ms / MS_PER_COLUMN;

    /* One complete frame: 60 spans of 8 pixels, in ascending order, no gaps
     * and no repeats. A span never straddles two rows because the width (40)
     * is a whole number of spans, so every span sits in one row and its
     * columns are x .. x + 7. */
    for (uint32_t first = 0u; first < RGBX_V2_PIXEL_COUNT; first += RGBX_V2_PIXELS_PER_SPAN) {
        const uint32_t x = first % RGBX_V2_WIDTH;

        /* set_luma_span8 takes a foreground (the color at luma 255), a
         * background (the color at luma 0) and one luma per pixel, and
         * interpolates. That is how an effect fits in a module with no memory
         * to hold a palette: send two colors once and eight brightnesses
         * after them. Use set_span8 instead when each pixel needs its own
         * color; a module may import one span encoding or the other, never
         * both. */
        rgbx_v2_set_luma_span8(first, color, 0u,
                               luma_at(x + 0u, head, tail), luma_at(x + 1u, head, tail),
                               luma_at(x + 2u, head, tail), luma_at(x + 3u, head, tail),
                               luma_at(x + 4u, head, tail), luma_at(x + 5u, head, tail),
                               luma_at(x + 6u, head, tail), luma_at(x + 7u, head, tail));
    }

    /* Shuffle's switch-point signal: the sweep wrapping back past column 0 is
     * this animation's natural cycle boundary. Test for the wrap rather than
     * for head == 0, because one tick at a high Speed can step straight over
     * column 0 and the boundary would then never be reported. A module that
     * imports this must call it exactly once per tick, so the call is
     * unconditional and the value carries the answer. */
    rgbx_v2_set_good_moment(phase_ms < previous_ms ? 1u : 0u);
}
