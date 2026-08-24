# rgbx-extension-template

Template for building **rgbx animation extensions** for the
[RGB Sunglasses](https://github.com/skalldri/rgb-sunglasses) — standalone, with
no firmware-repo checkout and no Zephyr toolchain. One C (or C++) file becomes
both a device-loadable `.llext` and a `.wasm` you can test instantly in the
hosted web simulator.

It also builds the newer **RGBX v2** packaging format: a second, much smaller
translation unit compiled to a memoryless WebAssembly guest, gated on the way
out by the SDK's own admission checks and packed into a digest-checked `.rgbx`
container. See "Device RGBX v2 packages" below.

## Documentation

- **[Getting started: your first extension](https://rgb-sunglasses.autom8ed.com/api/getting-started.html)**
  — a start-to-finish walkthrough that builds a working extension and explains
  each concept as it uses it: declaring parameters, reading the IMU/audio/button
  inputs, drawing pixels, and the good-moment signal. Ends with complete C and
  C++ listings. **Start here if this is your first extension.**
- **[API reference](https://rgb-sunglasses.autom8ed.com/api/)** — every type,
  macro and function in the rgbx ABI, generated from the headers this template
  builds against. Handy jumping-off points:
  [`rgbx_api.h`](https://rgb-sunglasses.autom8ed.com/api/rgbx__api_8h.html)
  (the flat C ABI) and
  [`rgbx::Animation`](https://rgb-sunglasses.autom8ed.com/api/classrgbx_1_1Animation.html)
  (the C++ wrapper).

This README covers the template itself — building, publishing, and the sandbox
constraints the build gates enforce.

## Quick start

1. **Use this template** (or fork) → clone your new repo.
2. Build all three artifacts (first run downloads the pinned SDK +
   toolchains, ~5 min; afterwards it's seconds):

   ```bash
   ./build.sh
   ```

   Prerequisites: bash, cmake ≥ 3.21, Node.js ≥ 20, curl, tar
   (Linux or macOS; on Windows use WSL).

   `build.sh` checks the Node version before it configures anything — the
   wasm gate (`check-wasm.mjs`) needs ≥ 20, and an older one fails the wasm
   link with a bare `SyntaxError` from inside the SDK. If you upgrade Node
   after a build, re-run `./build.sh -URGBX_NODE` (or delete `build/`):
   CMake cached the old interpreter's path at configure time and keeps
   using it otherwise.

3. **Test without hardware**: drag `build/wasm/my_extension.wasm` onto
   <https://rgb-sunglasses.autom8ed.com/sim/>. The simulator runs your real
   code against the firmware's tick semantics — parameters, IMU/audio/button
   inputs, brightness behavior and all.
4. Edit `src/main.c` (it's the kitchen-sink "hello" reference — every
   parameter type and every input source) and iterate. The RGBX v2 sample is
   a separate, much smaller file, `src/main_v2.c`. Prefer C++? See
   "Writing your extension in C++" below. If the ABI isn't obvious from the
   example, the
   [getting started guide](https://rgb-sunglasses.autom8ed.com/api/getting-started.html)
   walks through the same ground one concept at a time.
5. **Rename your extension**: change `project(my_extension ...)` in
   `CMakeLists.txt`. The name must match `^[a-z0-9_]{1,25}$` — it becomes the
   `.llext` filename on the device and must equal your future registry-entry
   name (below).

## Writing your extension in C++

Copy the C++ example over the C one — the build automatically prefers
`src/main.cpp` when it exists (you can delete `src/main.c`):

```bash
cp examples/cpp-waves/main.cpp src/main.cpp
./build.sh
```

The [`rgbx::Animation`](https://rgb-sunglasses.autom8ed.com/api/classrgbx_1_1Animation.html)
wrapper (in the SDK's `include/rgbx/rgbx_animation.h`)
replaces the raw ABI boilerplate: subclass it, override `tick(dt_ms)`, and
declare everything with one `RGBX_ANIMATION(Class, "Name", 40, 12, params...)`
macro — it emits and exports all the ABI symbols for you. You get typed
parameter accessors (`paramU32`/`paramColor`/`paramBool`/`paramString` — the
last one hides the string-parameter indexing trap), `setPixel()`, and an
optional `goodMoment()` override for shuffle-mode switch points.

C++-specific rules (the SDK toolchain enforces the first two): no exceptions,
no RTTI, and the class must be trivially destructible (a `static_assert` in
the macro checks this). Still one translation unit, still no heap.

## Device RGBX v2 packages

RGBX v2 is the firmware's newer extension format. Instead of loading native
code into a memory-protected sandbox, it loads a **memoryless WebAssembly
guest** and drives it through a tiny import surface. The build produces a
`.rgbx` container: a fixed header, a canonical CBOR manifest describing the
package, the gated module, and a SHA-256 trailer over all three.

```bash
./build.sh                                                  # all three targets
cmake --preset rgbx-v2 && cmake --build --preset rgbx-v2    # just this one
```

Outputs land in `build/rgbx-v2/`: `my_extension.rgbx` (the package a device
installs), `my_extension.wasm` (the prepared module inside it), and
`my_extension.raw.wasm` (the linker's output before the post-link pass, kept
because it is what you disassemble when the gate rejects something).

The v2 guest is a different ABI, not a recompile of `src/main.c`: there is no
framebuffer, no Zephyr, and no C library. So it lives in its own translation
unit, `src/main_v2.c`, described by its own manifest, `rgbx-v2.json`. The
`.llext` and `.wasm` targets are untouched by any of this and keep building
from `src/main.c` (or `src/main.cpp`).

### Living inside the memoryless profile

The sample is written to these constraints and says so in its comments. All of
them are checked after linking, so a violation is a build failure rather than
a device-side surprise:

- **No linear memory.** The module carries no memory, table, data or element
  section, and every load/store opcode is refused. No arrays, no pointers, no
  string literals, nothing that spills to a stack frame.
- **No floating point.** Integer opcodes only. Use fixed point; the audio and
  IMU inputs already arrive as integers.
- **State lives in WebAssembly globals.** A variable declared in address
  space 1 compiles to a real Wasm global instead of a memory slot, which is
  the only mutable state a memoryless module can hold:

  ```c
  static __attribute__((address_space(1))) uint32_t phase_ms = 0u;
  ```

  Read and write it like any other variable. The profile allows 8 globals,
  but `wasm-ld` spends one on `__stack_pointer` even in a module with no
  memory and the gate counts it, so seven are yours. Treat them as reset on
  every activation and persist nothing.
- **Pixels leave through span calls.** One tick must make exactly 60 ordered
  calls to one span import, at `first_pixel` 0, 8, ... 472, covering the frame
  once. Import `set_span8` for a color per pixel, or `set_luma_span8` for a
  foreground and a background color plus one luma per pixel that picks a
  point between them. A module may import one of the two, never both.
- **Bounded everything else.** At most 8 functions including imports, 32
  locals per function, 16 parameter reads and 64 input reads per tick, and
  2048 module bytes. A guest that imports `set_good_moment` must call it
  exactly once per tick.
- **No host calls during `rgbx_init`.** It may only touch guest state.
- **Exactly two exports, in order: `rgbx_init` then `rgbx_tick`.** The order
  is the order the linker emits, which is the order you define them in the
  file. Defining `rgbx_tick` first fails the post-link pass with `module must
  export exactly rgbx_init followed by rgbx_tick`, and the fix is to swap the
  two definitions, not to change any flag.

Those numbers are not written down in this repo. They come from the release's
`sdk-manifest.json`, which the firmware's own admission path is compiled
against, so the gate here and the device cannot disagree about what is
admissible.

### The manifest

`rgbx-v2.json` is what the device reads about the package before it decides
whether to install it. Every field is validated against the release's pinned
profile, and an unrecognized key is an error rather than a default, so a typo
in a security-relevant field cannot quietly widen what you shipped.

| Field | Meaning |
| --- | --- |
| `extensionId` | Package identity, matching `^[a-z0-9][a-z0-9._-]{0,30}$`. Keep it equal to your CMake project name. |
| `displayName` | Up to 31 printable ASCII bytes, shown in the companion app. |
| `version` | `[major, minor, patch]`, each 0..65535. |
| `rgbxAbi` | Guest ABI the module targets. Must equal the SDK's (2 on `fw-v3.5.0`). |
| `minimumFirmwareAbi` | Oldest firmware ABI allowed to load this package. Cannot exceed the SDK's. |
| `geometry` | `[40, 12]`, checked against the release's profile. |
| `capabilities` | Sensor permissions requested: any of `buttons`, `imu`, `audio`. The sample asks for `buttons`. |
| `memoryMaxBytes` | Must be `0`. The v2 profile grants no linear memory. |
| `budgetClass` | Must be `0`. This release defines exactly one budget class. |
| `sourceLanguage` | Recorded as provenance, up to 15 bytes. |
| `compilerId` / `compilerVersion` | Must equal the SDK's pinned compiler, `wasi-sdk` / `33.0` on `fw-v3.5.0`. |
| `sourceFile` | Path relative to the manifest. Must be the exact translation unit CMake compiled. |
| `parameters` | Up to 16 entries, at most 4 of them `string`. |

Each parameter is `{ "name", "type", "default" }`, with `name` up to 19 ASCII
bytes and `type` one of `uint32`, `color`, `bool`, `string`. JSON has no hex
literals, so a `color` default is a plain integer in `0..16777215`; the
sample's `16711935` is `0xff00ff`. `string` defaults are at most 31 bytes.

Renaming your extension means editing `extensionId` here as well as the
`project()` name in `CMakeLists.txt`. Nothing derives one from the other, so a
rename that touches only CMake ships a package still calling itself
`my_extension`.

If you do not want a v2 package at all, remove it in three places, or CI goes
red on the pieces you left behind:

1. drop `rgbx-v2` from the preset loop and the artifact `ls` in `build.sh`;
2. remove the two `rgbx-v2` entries from `CMakePresets.json`;
3. in `.github/workflows/ci.yml`, delete the whole "Check the RGBX package is
   reproducible" step and the `build/rgbx-v2/*.rgbx` line from the upload
   path list.

`src/main_v2.c`, `rgbx-v2.json` and the `rgbx-v2` branch of `CMakeLists.txt`
can go too. Deleting `src/main_v2.c` on its own just makes `./build.sh` fail.

There is no way to declare a source digest yourself. The packager hashes the
translation unit CMake actually compiled and records that, and it refuses to
run at all if `sourceFile` names a different file, so the provenance in the
container cannot drift from the code in the package.

### What the gate proves

`rgbx_add_extension(... MANIFEST rgbx-v2.json)` chains three SDK tools into
the build. Any of them failing fails the build:

1. **`prepare-rgbx-v2.mjs`** rewrites the linker's output into the memoryless
   shape: strip compiler custom sections, drop the unused table and memory
   declarations, reject a start function or any data or element segment, and
   reduce the export section to exactly `rgbx_init` then `rgbx_tick`. It then
   validates the result, so a module whose code really did reference the
   memory it just dropped fails here rather than at activation time. The
   rewrite is deterministic, which is one of the things the package's
   reproducibility rests on; the pinned compiler is the other.
2. **`check-rgbx-v2.mjs`** runs two independent passes. The structural pass
   re-derives the firmware's admission decision from the module bytes:
   sections, function and global counts, locals, import names and signatures,
   export signatures, and an opcode walk that rejects floating point, memory,
   indirect calls, table and reference instructions. The **tick oracle** then
   instantiates the module against a host that enforces the same per-tick
   budgets the firmware enforces, and replays `rgbx_init` plus several
   `rgbx_tick` probes across different `dt` values and parameter sets. Each
   probe must paint every pixel exactly once. That is the part a structural
   check cannot do: it catches a module that paints a complete frame only for
   one `dt`, or divides by a parameter that can be zero, or drifts a span
   offset. The oracle runs in a worker with a wall deadline and memory limits,
   so a guest that does not terminate is a rejection, not a hang.
3. **`package-rgbx.mjs`** validates every manifest field against the same
   pinned profile, encodes the manifest as canonical CBOR, and writes the
   container with its SHA-256 trailer.

One thing the oracle does not do: it varies `dt` and the parameters, but it
answers every sensor read with zero. A branch keyed on a button, the IMU or
the audio bands is therefore never taken during the gate, so keep the pixel
count and the luma range provably in bounds on those paths by construction
rather than trusting a green build to have visited them.

Nor does anything cross-check the manifest's `capabilities` list against the
input kinds your guest actually reads. A guest that reads the accelerometer
while declaring only `buttons` packages green here and fails on the device,
where the capability bits are what gate the read. Keep the two in sync by
hand: every `rgbx_v2_input_u32` selector your code can reach needs its
capability listed.

### Reproducibility

Rebuild an unchanged tree and you get the same package back. Two clean builds
match, and so does a build into a differently named directory, which is the
one CI performs: it builds the package twice and fails if `cmp` does not pass.
That is the guarantee this repo actually checks, and it is enough to make a
digest a useful thing to quote next to a commit.

It is not a claim of byte identity across operating systems or host
architectures. Nothing here has verified that, so treat a digest someone else
publishes as a statement about their machine until your own build, or a CI
run, reproduces it.

The sample as committed produces:

```
d051be767d4e6329dbe1c3aa543e090c681612592e5571e8fbe8e98d831e8f77
```

built on macOS arm64 against the pinned wasi-sdk 33.0. To reproduce it:

```bash
rm -rf build && ./build.sh
sha256sum build/rgbx-v2/*.rgbx   # shasum -a 256 on macOS
```

### Proving the gates actually bite

Worth doing once, so you trust the build when it is green. Each of these
should fail, and none of them should produce a package:

- **Wrong SDK digest.** Change one character of `RGBX_SDK_SHA256` in
  `cmake/fw-release.cmake`, delete `build/`, and configure. The download is
  hash-checked before anything is extracted, so configure stops with a hash
  mismatch and no SDK tree appears.
- **Incomplete frame.** Make the span loop in `src/main_v2.c` stop one span
  short. The oracle reports the exact pixel count it saw.
- **Over the profile.** Read a parameter 17 times in one tick, or introduce a
  `float`. The first trips the per-tick budget in the oracle, the second is
  refused by the structural opcode walk.

## Testing on real hardware (optional)

This is the `.llext` path. Installing an RGBX v2 `.rgbx` package is
firmware-side and not covered here; see the `fw-v3.5.0` release notes.

Copy `build/arm/<name>.llext` onto the glasses' USB mass-storage disk under
`ext/`, then sync, eject, and reboot the board. The firmware discovers
extensions at boot; select yours from the companion app or the `ext` shell
command.

## Publishing your extension

When it's ready, submit a PR to the
[rgb-sunglasses](https://github.com/skalldri/rgb-sunglasses) repo adding one
entry to `extensions/registry.json`:

```json
{
  "name": "my_extension",
  "repo": "https://github.com/you/your-extension-repo",
  "rev": "<full 40-hex commit SHA to publish>",
  "description": "One line about what it looks like",
  "author": "you",
  "license": "MIT"
}
```

`name` must equal your CMake project name; `rev` is the exact commit the
maintainers review and build. Once merged, every firmware release rebuilds
your extension from that pinned commit and ships it as a release asset — the
companion app then installs it onto devices automatically. Your repo must
carry an OSI-approved license.

## Rules of the sandbox

This section is about the `.llext` and simulator `.wasm` targets. The RGBX v2
profile is stricter and different; "Device RGBX v2 packages" above covers it.

Your extension runs in a memory-protected sandbox with a per-tick CPU budget.
The build gates enforce most of this, but know the constraints:

- **One translation unit** (a single `.c` or `.cpp`; the build prefers
  `src/main.cpp` over `src/main.c`).
- **40×12 framebuffer**, RGB8. Render near full-scale (255) channel values —
  the firmware applies a global brightness factor (default 0.02), so dim
  drawing is invisible on the panel.
- **≤ 16 parameters, ≤ 4 of them strings** (see the manifest in `src/main.c`,
  or [`rgbx_manifest`](https://rgb-sunglasses.autom8ed.com/api/structrgbx__manifest.html)
  in the API reference).
- **No heap, no exceptions, no RTTI.**
- **Single-precision math only.** Firmware v3.1.0+ exports a curated libm set
  (`sinf`, `cosf`, `tanf`, `atan2f`, `sqrtf`, `expf`, `logf`, `powf`, `fmodf`,
  `floorf`, `ceilf`, `roundf`), the 64-bit integer helpers, and `memmove` —
  so real trig works now. **Double precision does not**: `sin`, `pow`, or any
  expression that promotes to `double` fails the build gate (write float
  literals with the `f` suffix). The FPU handles float `+ - * /` inline.
- **≤ 24 KB** total loaded size (build gate checks this).
- **Globals reset on every activation** — the firmware reloads your extension
  each time it's selected; persist nothing.
- **Bound any phase accumulator you feed to `sinf`/`cosf`/`tanf`.** See below —
  this one is not caught by any gate and has bitten two shipped extensions.
- Callable firmware functions are exactly the SDK's `arm/allowed-symbols.txt`:
  the math set above plus `str*`/`mem*` and `printk` (output shows in the
  simulator's console and the device's debug shell).

## The trig cliff: bound your phase accumulators

The single easiest way to ship a slow extension, and the one gate that cannot
catch it. Both extensions currently in the community registry shipped with this
bug ([rgb-sunglasses#304](https://github.com/skalldri/rgb-sunglasses/issues/304)).

The device's `sinf`/`cosf`/`tanf` take a cheap argument reduction only while
`|x| <= 201.06`. Above that they fall into a multi-precision reduction that costs
several times more **and keeps getting more expensive as the argument grows**.

So this — the obvious way to write a moving animation — degrades over time:

```cpp
t_ms_ += dt_ms * paramU32(0) / 50u;              // free-running: BAD
const float t = static_cast<float>(t_ms_) * 0.001f;
... sinf(fx * kTau + t * 1.1f) ...               // |arg| crosses 201 after ~3 min
```

It is nasty because it is invisible to every quick check: the animation runs at
full speed for the first minute or two, and the accumulator resets every time the
extension is activated — so it looks perfect right after you select it, every time.

**Fix: wrap the accumulator at a period where every rate you use completes a whole
number of cycles.**

```cpp
/* Rates 1.1, 0.7, 1.7 rad/s are 11 : 7 : 17 on a 0.1 grid, so all three phases
 * return to their exact starting values after 20*pi s. The wrap is seamless, and
 * the argument is bounded for ANY speed value -- the bound is on the accumulator,
 * not the rate. */
constexpr uint32_t kPeriodMs = 62832u;           // 20*pi s
t_ms_ = (t_ms_ + dt_ms * paramU32(0) / 50u) % kPeriodMs;
```

Costs one multiply per tick, nothing per pixel. `examples/cpp-waves/main.cpp`
shows it in context.

If your rates share no common period, reduce each phase with `fmodf` instead — but
`fmodf` **keeps the sign of its dividend**, so a phase that can go negative lands in
`(-2*pi, 0]`. That is fine passed straight to `sinf`/`cosf`, which accept negative
arguments; it is *not* fine if you then index a lookup table with it. Add the period
back when the result is negative.

**How to check it on hardware:** leave the animation running for several minutes on
one uninterrupted activation, then read `ext stats` — switching animations in between
zeroes the counters. Watch the device console for `Render overran the tick interval`,
which shows up long before the per-tick CPU budget does (the budget sits well above
the render interval and will not catch this). Raising your speed parameter compresses
the timeline proportionally.

## Updating the SDK pin

`cmake/fw-release.cmake` pins the firmware release (and its `rgbx-sdk`
tarball sha256) this extension builds against. To move to a newer firmware
release, update both lines from the release page's `rgbx-sdk-*.tar.gz` asset
and rebuild. The digest is checked before the archive is extracted, so a
wrong or corrupted download fails configure instead of building against
whatever arrived.

Four fields in `rgbx-v2.json` move with the pin: `compilerVersion` and
`rgbxAbi` must equal the SDK's, `geometry` must equal the release's frame
size, and `minimumFirmwareAbi` may not exceed the SDK's ABI version. The
packager checks all four and fails when they drift, so an SDK bump that moves
any of them is an edit to that file too.

The ABI is append-only within a version, so newer SDKs build older extension
source unchanged; a genuine ABI version bump is announced in the firmware
release notes.
