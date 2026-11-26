# Graphics replay checks

`replay.cpp` replays a memory/register snapshot through the production software
renderer (or Metal where built). This is a diagnostic tool, not a game check.

Compare a replay with previously reviewed references:

```sh
python3 compare.py reference new-output dump_6401 --replay build/gpu_replay
```

`compare.sh reference new-output dump_6401` calls the same gate. Set `REPLAY` to
select another replay binary, or `PYTHON` to select the Python interpreter.
The output directory must not exist. Each dump runs once in a fresh process;
benchmark warmups are deliberately excluded. The gate compares decoded RGBA
pixels, requires the exact PNG frame set, and fails on missing/extra/invalid
frames, process errors, timeouts, or changed inputs. Logs, difference images,
input hashes and per-frame error bounds are saved beside `report.json`.
Exit codes are 0 for a match, 1 for a replay/comparison failure, 2 for invalid
inputs or orchestration errors. Default comparison is exact. `--tolerance N`
allows at most N error in each channel of every pixel; any exception needs
separate justification in the reviewed fixture.

References are never generated or updated by the comparison gate. The default
`--oracle regression` records stability against an older output, not rendering
correctness. For mathematically specified fixtures use `--oracle analytical
--provenance "description of the expected image"`. Use `independent` only with
an independently captured reference and record its origin in `--provenance`.
`pngdiff.py ref.png actual.png [diff.png [threshold]]` compares individual PNGs
and exits 0 for a match, 1 for a mismatch, 2 for invalid input. It accepts
noninterlaced RGB8/RGBA8, all five PNG row filters, and compares raw samples
without color management.

Replay now requires exact-size memory files and a valid `gpustate.bin`. Direct
`gpu_replay dump out.png --allow-missing-state` opts into reset state when the file is absent;
an existing invalid state file always fails. This option follows the two
positional paths like the other replay flags. State restores validate the whole
file before changing live GPU state. Unknown/malformed operations, invalid
top-level memory ranges, missing presents, and failed PNG writes return failure.
Embedded PICA command payloads remain renderer inputs; replay is not a sandbox.

Legacy game dumps are not complete cross-backend oracles: GPU state V1 contains
native ABI/endian data, and capture does not record all subsequent CPU memory
writes, DMA and cache operations. Captured memory also predates the state-save
flush. Replaying one successfully proves neither faithful original gameplay nor
Wii U rendering. Use reset-state synthetic command streams with explicit guest
byte order for cross-backend checks. A Cemu result still needs hardware sampling
for driver, timing and resource differences.

Fast checks, without games or Cemu:

```sh
python3 ../../../../tools/test_gpu_pngdiff.py
python3 ../../../../tools/test_gpu_replay_check.py
python3 ../../../../tools/test_gpu_render.py
python3 ../../../../tools/test_deferred_transfer.py
```

All are included in `tools/selftest.py`, together with the actual replay/state
I/O controls. `test_gpu_render.py` builds the production software renderer and
checks six exact analytical frames, including triangle coverage, depth, texture
sampling and blend edges. `test_gpu_render.cpp` is also the shared GX2 fixture;
`tools/gpu_replay_verify.py` checks its raw readbacks against analytical PNGs and
rejects native shader misses or CPU fallback in GX2 results.

Build the same fixture and production native shader pack into a diagnostic RPX:

```sh
python3 ../../../../tools/test_gpu_render_target.py --out ../../../../build_win/gpu_replay_probe
```

This developer command copies its source inputs, uses one compiler at a time,
checks the software output and RPX loader, then records hashes and run/verify
arguments in `build_proof.json`. It never starts Cemu. Use an isolated Cemu/MLC;
an empty synthetic common-save folder selects diagnostic 240, while an empty
`default480` file there selects production 480. Archive each attempt's output
and verify it with `tools/gpu_replay_verify.py`. These checks do not certify
gameplay, patterned texture coordinates, hardware behavior or performance.
