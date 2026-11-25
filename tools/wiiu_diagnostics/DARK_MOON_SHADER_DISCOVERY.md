# Dark Moon shader discovery and runtime compilation

## Current result and limits (November 25)

Fresh-cache hardware replays improved from 154 pixel compilations (54,733 ms
reported compilation time) to 31 (9,037 ms), without adding the held-out replay
to build inputs. One brief self-correcting white flash and some cutscene/audio
desynchronization remained. Capture lengths differ, so these results do not
establish a whole-cutscene FPS gain.

A separate stationary-lab off/on/off lighting comparison measured
20.8/25.2/20.7 FPS. A lower-scale, 30 FPS-limited configuration measured
29.6–29.7 presentations/second in that lab. Scale and pacing change the workload;
this is not proof of full-resolution 30 FPS throughout the game. The latest
persistent Balanced/Sharp/Fast menu policy was uploaded but still needs its own
boot/menu/cutscene validation. Other titles keep their existing policy.

The sections below retain the investigation sequence. Statements that a later
comparison was pending describe that point in the sequence; the later result
sections and this summary define the current evidence. Fragment predictions are
bounded heuristics from observed states, not a complete material/pass decoder.
The screen-slice scaling experiment remains off by default and is unverified on
hardware. Classic versus resumable execution has build/manifest support; a
controlled hardware performance comparison is still pending.

## Measured runtime problem

In the 2026-11-24 fresh-cache opening test, the vertex shaders with keys
`a0fe8d2b588dce55` (entry 327) and `ee4b6f52ab594d90` (entry 344) took
9,214 ms and 3,875 ms to compile. The existing vertex worker idled by polling
a 64-bit atomic up to 20,000 times. On Wii U that atomic uses a shared spinlock;
the worker also shares core 0 with the lower-priority shader compiler.

Parking idle vertex workers on Wii U, with the same shader pack and another
fresh-cache replay, reduced those compiler reports to 919 ms and 1,167 ms. The
user reported an improvement. This is a measured comparison of those shaders,
not a matched full-game benchmark or proof that every cutscene hitch is gone.
Desktop workers retain their previous spinning behavior.

The next revision prioritizes demanded vertex jobs, then demanded pixel jobs,
then speculative cache warm-up. Promotion retains cache identity without
duplicating a job. Host tests cover 1,000 pixel arrivals and cache promotion;
this queue policy is built but has not yet had its own Wii U comparison.

## What the game's files reveal

Both slow shaders reconstruct from the same program in
`/materials/material_new.data`. DVLE symbols name them `MorphGhostDepth` and
`MorphGhost`; their shared 31-instruction routine is `ApplyMorphs`. The captured
boolean-specialized structural analysis reaches 89 and 108 instruction sites,
respectively. Static sites are not dynamic instruction counts or CPU timings.

The ordinary DVLB scan found one vertex entry (two output-map records). All 34 captured vertex records
translate successfully, producing 50 distinct GPU vertex sources and 76 lookup
keys in the tested generator run. Thus these observations demonstrate a
discovery/first-use compilation gap, not missing GPU CALL support. The separate
native geometry translator rejects the captured vertex programs' calls/jumps;
it is not currently a native vertex backend, and its rejection must not be
presented as a GPU feature failure.

The material asset has 2,905 typed descriptors, 31 material headers and 294
pass descriptors. The existing NW4C material scanner finds no materials in it.
The new tagged-material parser follows the descriptor references and the
primitive/blob arenas, reads the shared DVLB and its boolean/int constants,
and selects the entry addresses actually named by material/pass headers. It
finds 38 vertex entries without a gameplay trace. They cover all 23 distinct
entries in the captured opening data; all 34 captured records agree on code,
used swizzles, integer defaults, fixed b14 and output mapping. Runtime boolean
variants remain the shader generator's responsibility.

The 38 selections generate 80 distinct vertex sources and 120 lookup keys;
Windows CafeGLSL under Steam Proton compiled them in the tested run with at
most 28 vertex registers (48 available). Synthetic fixtures cover selection,
helper exclusion, arena alignment, constants, default-pass tokens and malformed
input rejection. The parser is in the shared PC shader-discovery path and uses
format tags, not a title ID or captured shader list. Its fresh-cache hardware
comparison is pending. It currently handles the recognized uncompressed tagged
assets in RomFS; other containers/layouts may need additional support.

Fragment-state decoding from these material fields remains an investigation.
Debug labels alone still cannot safely become drawable states. Rejected or
unrecognized material layouts retain the existing discovery/runtime fallback
paths, with a diagnostic rather than invented shaders.

The optional asset analyzer associates captured programs with recipe locations
and DVLE names. Raw programs and private reports stay in the local build area.
The repository's VSR records describe reconstruction offsets/fingerprints;
fragment catalogs contain generated fragment states, not game vertex programs.

## Generic changes and coverage limits

The PC builder can import a generator-fingerprinted fragment catalog before
CafeGLSL compilation. It preserves existing mappings, bounds input size and
rejects stale catalogs. The Dark Moon catalog contains 167 observed fragment
states. A real Windows CafeGLSL run under Steam Proton compiled them with at
most 39 pixel registers; the original mappings/payloads were checked intact.
This is partial observed coverage, not enumeration of every game state and not
the runtime starvation fix above. New runtime misses can still occur.

Actual GPU translator failures now produce a structured feature report and a
reviewable GitHub issue template through the shared GUI/CLI build pipeline.
Failures are keyed by program identity as well as entry/boolean variant, so
different programs at the same entry do not hide each other's diagnostics.
Reports contain no raw instructions or GLSL and are never uploaded automatically.
A clean report only covers variants discovered by that build.

The runtime separately counts missing vertex shaders, forced CPU draws, missing
geometry pipelines and GPU resource limits. Opt-in timing reports a bounded
top-six list of fallback keys/entries. Summed task wall durations can include
preemption and parallel work; they are not exclusive CPU milliseconds.

## Remaining performance work

One later hardware interval was predominantly GPU-bound, with about 25–26 ms
GPU work per presentation and little CPU fallback. In a warmed same-scene
comparison, temporarily disabling detailed fragment fallback did not materially
change roughly 39–40 FPS. Normal shading was restored. This does not exonerate
every first-use fragment state, but it prevents attributing all remaining lag
to CPU fallback or claiming a universal shader fix.

Further material decoding, representative scene coverage and fresh-cache Wii U
verification remain necessary. Emulator correctness checks are not Wii U
performance measurements. No raw game binaries, captures, saves or screenshots
are included in this report.

## Configuration prediction investigation (2026-11-25)

A newer hardware miss file was retrieved without uploading it as training input.
After normalization, it adds 43 distinct states to the older capture's 148.
All newer combiner sequences already occur in the older capture. The principal
new components are perspective rather than orthographic shadow sampling, a
three-light prefix, and different attenuation/directional/shadow combinations.
Thus additional TEV recipes alone do not explain these particular misses.

Private bounded ARM-sandbox experiments reconstructed fragment command setup
from virtual methods in the game executable. Both complete and partial combiner
writes were recovered; partial writes must retain inherited stages and fields.
Those experiments are not shipped as a general decoder: they require runtime
inputs, and their extra combiner sequences predicted none of the 43 newer
states. Raw executable extracts and recovered command streams remain local.

The shared build now generates a bounded configuration neighborhood from a
game's observed fragment states: light-count prefixes, alternate observed light
types on shadow lights, and both shadow projection modes. It retains physical
light numbers until after prediction. Generated candidates are excluded from
subsequent observation input and cross-game observation copying. Discovery emits
an explicit non-exhaustive JSON report; zero translator failures still does not
mean complete gameplay coverage.

Using only the older capture, this predicts 27 of the 43 held-out states with
2,572 candidates. This is a coverage result, not a measured speedup. The current
candidate combines these with existing discovery/variants and generates 3,499
fragment sources and 80 vertex sources, with zero translator rejections.
Native compiler validation and real-console rendering/timing remain required.

Normalization now removes per-light spot attenuation when its LUT is disabled,
per-light RGB shadow gating when no enabled RGB shadow path reads it, and a
physical light number when no attenuation LUT reads it. Shadow alpha remains
independent of the per-light RGB flag. C++ and Python normalization are compared
byte-for-byte in the fixture. The generator/cache revision advances to 6.

## Fallback candidate

Sixteen lit/unlit texture-mask families retain the combiner, fog and lighting
paths but omit TEV texture fetches for unused/null units. Null textures retain
the uniform-provided value. Bump/shadow sampling retains its own lighting path.
The lit fallback previously clamped to four lights; the candidate extends its
uniform flags and light invocations to all eight. Its uniform layout and the
shader pack must match the executable. These changes do not establish that all
previous fallback limitations (such as procedural textures) are solved.

Source tests cover all texture masks and unchanged lighting code across masks.
CafeGLSL register/program checks and console image/performance comparisons gate
promotion. The previous correctly shaded fallback must not be replaced by a
solid-colour performance shortcut.

## Execution and release comparisons

The native build driver now accepts explicit classic/resumable execution and
an explicit game optimization setting, writing an execution manifest. It also
sets RECOMP_RESUMABLE whenever the actual selected mode is resumable, including
the automatic default. A classic -O1 build and resumable -O1 build provide a
controlled execution comparison; save states remain enabled in the primary
candidate. No CPU cost has yet been established by these builds alone.

`analyze_frame_timing.py` reports weighted presentation cadence, disjoint gap
buckets, long hitches, noncached shader compilation and CPU fallback reasons.
It rejects malformed pace rows and excludes intervals crossing a requested
scene start. It does not infer frame percentiles from buckets or equate
presentation FPS with simulation speed. The renderer's first statistics report
now uses elapsed time since rendering began instead of dividing accumulated
startup frames by a nominal second.

Hardware regression matrix before release:

1. Same intro and renderer resolution; fresh per-pack cache, timers off.
2. Replay with a warm cache; compare hitch counts, compilation and picture.
3. A still lab scene, brief CPU/GPU timing samples, then timers off again.
4. Matched classic -O1 versus resumable -O1, same shaders/settings/scene.
5. Homebrew and installed-title save/load, including relaunch, repeated
   replacement, and indicator show/clear on both screens.

Keep FTP/plugin configuration consistent within comparisons. Take screenshots
outside the main timing intervals. Preserve ordinary game saves. Record build,
shader-pack identity, scene and cache condition with every result. GPU pass
changes require a measured expensive target and a rendering-equivalence check;
no new pass elimination or global resolution reduction is justified yet.


## Native validation of the fragment-family candidate (2026-11-25)

The final compiler batch exposed two separate failures. Proton's Steam launch
path failed during DLL copying; retrying through Proton `runinprefix` exposed
CafeGLSL's actual ALU scheduling error in fallback texture-mask variant 6.
The eight-light fallback generated a DOT4 channel group the scheduler could
not place. Explicit, ordered `precise` scalar dot products avoid that lowering
in fallback sources. Specialized fragment programs remain unchanged by this
workaround. Floating-point rounding may differ from a contracted dot product;
console image comparisons are still required.

The rebuilt complete pack contains 80 vertex and 3,499 fragment programs and
passes `check_registers` and `gsh_rules`: maximum 28/48 vertex registers and
39/200 pixel registers. Shader binary size is 24,948,140 bytes. The matching
resumable executable and shader index bring the transfer to 51,047,849 bytes.
The primary candidate retains session/persistent states. The screen-slice
resolution experiment is off by default. Native compiler success establishes
program validity, not rendering correctness or a measured console speedup.


## Fresh-cache failure and spotlight-enable sharing (2026-11-25)

The fragment-family candidate's fresh run was recorded in
`2026-11-25_16-37-34-503073`. Its screenshot at 16:40:10 shows missing white
background regions and a 4 FPS overlay. Nearby pace reports contain 10
presentations in 2,181 ms and repeated long waits inside presentation. The full
recording contains 154 noncached pixel compiles (maximum 539 ms), no vertex
compiles, and no fallback-route report rows. These are not evidence that CPU
vertex fallback never occurred. The timing intervals also do not independently
measure audio/video clock drift.

The previous warmed-cache observations cannot establish fresh-start correctness.
A shader pack passing native instruction/register validation is insufficient to
establish fallback rendering equivalence. A later forced-fallback comparison in
the lab rendered substantially the same room as specialized shaders; it does
not reproduce or exonerate the eight-light intro materials from the marked shot.

A read-only live miss capture recovered all 154 fragment requests (129 distinct
states under revision 6 normalization). Spotlight enables on individual lights
account for a major variant explosion: the old predictor varied shadow lights,
but ordinary lights also changed spotlight enables independently.

The new specialized shader reads spotlight enable from `Light.att.z`, a formerly
reserved float component, while specializing the LUT identity and other lighting
features. Both CPU/PC normalization paths omit this enable bit, retaining the
physical light number whenever the shared spotlight LUT is enabled, even if
this particular draw has it switched off. The real draw still supplies its
spotlight enable; decoded build states must not determine that uniform. The
cache revision is 7. The existing fully dynamic fallback still reads its own
spotlight flag. A synthetic check covers all 256 eight-light masks, source/key
sharing, disabled-LUT elimination, and physical-LUT identity retention.

Without adding this fresh capture to build inputs, the actual new index covers
108 of the 154 previously missed requests. Under the new normalization they
collapse to 88 distinct states, of which 57 find shipped programs. The remaining
31 distinct states include new combiners, fog variants and a lighting setup.
This is a coverage result, not a measured console speedup or a complete fix.
The pack shrinks from 3,499 to 1,214 fragment programs and passes native checks
at maximum 28/48 vertex and 36/200 pixel registers. Cold console replay, picture
checks and timing comparison remain required. Clear generated caches before
every cold-start comparison; label warm-cache tests separately.


The missing-program path now also requests/stores console compilations under
`fs_key(fs_normal(ds))`, matching the shipped-program lookup, and generates the
source from that normalized state. The miss log retains the first raw state.
Previously the lookup tried a canonical key but scheduled each absent raw key
separately, so fixed-function differences and uniform-only flags could compile
the same source repeatedly. Installing the completed canonical shader clears
the existing draw caches as before. With the spotlight change, the recorded
154 requests imply 31 distinct remaining compile keys, rather than 154 queued
raw-key compiles. This prediction still needs cold-console confirmation.

The new pack is 8,909,680 bytes, below the existing 16 MiB resident-pack limit;
it therefore also avoids the previous candidate's on-demand shader file seeks.
That is a second performance variable to record when attributing any speedup.
The matched resumable RPX, pack and index transfer is 34,977,990 bytes.


## Spotlight-sharing cold-console result

The fresh run `2026-11-25_16-53-34-800684` contains exactly 31 pixel compilations,
matching the prediction from the earlier capture; total reported compiler wall
time is 9,037 ms (previous recording: 154 compilations, 54,733 ms). No cached
compilations or vertex compilations appear. The user reports one brief white
background which corrected itself, with the remainder visually substantially
improved, but continued lag and slight cutscene audio desynchronization. There
is no marked screenshot in this recording, so the white flash cannot be tied
to a particular draw or shader from this log alone.

Long gaps above 100 ms fell from 189 to 46 across these recordings, but the
captures differ in length (267.610 versus 228.567 seconds of pace windows) and
scene residence; they are not a controlled average-FPS benchmark. Moderate
50–100 ms gaps remain numerous, including periods after compilation completed.
A 688.501 ms maximum in the new log is at frame 107 during startup, before the
main cutscene; it must not be presented as a remaining movie shader stall.
Both runs allocate the same main and transfer target sizes, including a
768x1536 main target and 369x1476 half-screen target. These support a further
GPU-cost investigation; they do not establish that reducing effect resolution
is visually acceptable. A short separate CPU/GPU sample follows the replay.


The separate live sample `2026-11-25_17-00-11-402747` reports 23.4
presentations/second across the measured interval, 479 draws/presentation,
zero CPU-rendered draws, no new texture uploads, and the same 31 fragment
misses. GPU timestamps cover 232 flips: 42.7 ms busy per 42.6 ms interval,
including 31.39 ms drawing the main target (3.0 passes), 3.13 ms presentation,
1.74 ms LCD work, 1.53 ms main clears, and 1.32 ms half-screen transfers.
This demonstrates GPU saturation in that later sampled scene, not the exact
unmarked white flash. The first report averages earlier execution and its
initial 0% busy/zero CPU lap timings are not a valid timed baseline. Both
CPU and GPU timers were acknowledged off afterward. Main-target shading and
pass cost are the next measured targets; CPU fallback is not the bottleneck
in this particular sample.


## Controlled rendering-work probe and next experimental build

The stable lab dialogue was held during `2026-11-25_17-06-24-743595`.
Clean full/lower/full intervals measured 23.4/41.5/23.3 presentations per second,
with GPU busy time 42.8/24.1/42.8 ms and main-target drawing 31.45/16.14/31.36 ms.
Each reports zero CPU draws and no new texture uploads. Initial reset intervals
include screenshots, transitions and stale GPU timestamp spans and are excluded.
The two-thirds scale probe changes pixel workload, not shader coverage. Its
screenshot has visible horizontal seams absent from both full-size images, so
the speedup is not yet a shippable image-equivalent improvement. The live setting
was restored to full, then automatic; timers were turned off.

A new candidate retains spotlight sharing/canonical compilation and adds two
independent switches, both off by default. `lightguard` skips a distance-attenuated
light's remaining RGB calculations when its distance contribution is exactly
zero. Negative attenuation still takes the original path. The last light is
not skipped when its Fresnel LUT can write primary or secondary alpha, which is
independent of attenuation. The switch is passed in previously unused
`uLscale1.w`; generated-cache revision is 8. Source tests protect that alpha
exception and absence of guards on lights without distance attenuation.
All 1,214 fragment and 80 vertex programs pass native validation, at maxima
27/200 pixel and 28/48 vertex registers. These are validity checks, not hardware
speed or image-equivalence results.

`copyrect` changes linear GPU row copies to round rectangle endpoints rather
than separately rounding their extents. Source UVs use actual rounded surface
padding and dimensions, rather than logical texture-size ratios. Fractional
coverage tests verify adjacent rows/columns share boundaries and cover their
complete extent, including the observed scales. Hardware seam removal remains
unverified. Turning off automatic scaling at the selected resolution now returns
a previously forced lower level to full size instead of leaving it stuck lower.
A matched executable and shader pack is required for `lightguard`; the geometry
experiment does not alter game saves. The prior working build is retained locally
and in the immutable console release directory.


## Lighting/copy hardware tests and persistent Dark Moon modes

`live-lighting-171418` holds the "Where to, Luigi?" dialogue rather than the
previous "Greetings, Luigi!" dialogue. In an off/on/off test the new lighting
guard measured 20.8/25.2/20.7 FPS and 48.1/39.7/48.2 ms per frame; main-target
work fell 36.98 to 28.50 ms. Screenshots show no obvious missing room lighting.
This proves a gain from the switch within the candidate, not a controlled
comparison against the previous executable in this later dialogue.

`live-copy-171529` captures the older and corrected row-copy paths at low scale.
Both native TV screenshots appear coherent. Consequently the earlier GamePad
capture's seams are not conclusively attributed to the copy math; capture timing
may also be involved. The corrected interval has 42.7 presentations but only
30.1 top-screen swap requests per second and 47.3 VBlanks per second, so it is
not accepted as evidence of a 42.7 FPS game-rate improvement. Its before/after
neighbor intervals are consistent. Endpoint/padding correctness is established
by the coverage tests; broader hardware image testing remains required.

`live-balanced-171849` combines both switches and the existing 30 FPS limiter.
At 90%, 85%, then 90% scale it measured 29.6/29.7/29.7 presentations/second.
The 85% setting leaves more margin in main-target work (22.29 versus 23.87 ms),
with no obvious picture defect in the native TV captures. Timing reports under
a cap can include paced idle time and should not be interpreted as uncapped
GPU saturation. The code previously declared cap_game/cap_fps but never
initialized a title policy; this explains why the documented limiter was idle.

A persistent policy is now limited to the tested program ID 0004000000055f00:
Balanced is 85% of the selected 720p scale with a 30 FPS limit; Sharp is full
selected resolution with a 30 FPS limit; Fast uses the previous two-thirds
scale, uncapped. At selected 480p there is no further scale reduction. The
existing options row displays MODE and cycles these choices, stored in the
per-game wiiu_render_mode setting; Save/Load/Quit indices account for that row.
Other titles do not acquire this policy. Custom 1080p retains automatic scaling
except in Sharp. The lighting/copy switches start on for this tested title,
while remaining opt-in for other titles. Persistent-mode boot, menu selection,
save/load navigation and cutscene synchronization remain hardware test items;
a near-30 FPS lab test does not establish perfect timing throughout the game.
