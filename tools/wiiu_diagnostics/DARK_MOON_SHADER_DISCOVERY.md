# Dark Moon shader discovery and runtime compilation

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
