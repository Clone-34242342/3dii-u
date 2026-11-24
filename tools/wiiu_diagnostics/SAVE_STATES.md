# Experimental save states

Resumable builds return guest execution to the scheduler at calls, service calls,
and preemption boundaries, so checkpoints do not retain native C call stacks.
The left-stick menu exposes Save State and Load State for all resumable titles.
This is an experimental execution mode with a CPU cost; all-title correctness
and performance have not been established.

The checkpoint barrier stops mutation while CPU registers, guest memory, kernel
objects, timers, supported services, DSP state and renderer state are captured.
Saved surfaces stay pinned against idle eviction. Replacing a checkpoint releases
the previous payload before allocating its replacement, avoiding two large RAM
snapshots overlapping. Native microphone capture remains continuous where its
sampling state has not changed.

Persistent saves use `checkpoint-<program-id>.bin` in the title's common save
storage. A same-session load uses the RAM checkpoint when available. After a
relaunch, loading validates the file and reconstructs runtime objects and GPU
resources before committing guest state. Native pointers and arbitrary callbacks
are not written into the archive. Unsupported state fails capture rather than
serializing unknown native resources.

The file is bounded and checksummed, with title, code, build and schema identity
checks. It is currently specific to the executable build that created it;
rebuilding or updating the executable can make an old state incompatible. Save
files are written through a temporary file and retain a backup of the previous
checkpoint. Saving approximately 106 MB on the Wii U can take substantially
longer than loading a RAM checkpoint. These files are separate from ordinary
in-game progress saves.

## Observed validation

- Kirby Triple Deluxe: three repeated save/load cycles on Wii U after the
  replacement-memory fix; the user confirmed working controls and audio.
- Brain Age, Kid Icarus Uprising and Super Mario 3D Land: individual session
  save/load tests succeeded. This is not coverage of every scene or service.
- Mario 3D Land: a file state loaded after a new Cemu process; the user confirmed
  controls and audio. On Wii U, the recording also shows a file save, a new game
  startup and a persistent-checkpoint rebuild, with successful play reported.
- The latest successful Wii U Mario run had FTPiiU disabled. Earlier runs
  detected corrupt native heap links during allocations, including FTPiiU's
  allocator calls. This does not identify the original writer or establish that
  the heap issue is fixed. Long-session stability remains unresolved.
- Mario's patched 60 Hz presentation build showed roughly 41–48 FPS in some
  hardware scenes. No matched before/after measurement isolates the cost of
  resumable execution from the existing patch and scene workload.

## Targeted checks

`tools/test_recomp_resumable.py` executes nested ARM/Thumb calls, loops, SVCs and
jump-table continuations and rewinds CPU and RAM at dispatcher boundaries.
`tools/test_checkpoint_scheduler.py` covers post-service scheduling.
`tools/test_checkpoint_transaction.py` covers rollback, replacement allocations,
memory-budget rejection, file reload and checksum rejection using host fixtures.
`tools/test_checkpoint_surface_lifetime.py` exercises actual eviction/pinning
paths. `tools/test_state_archive.py` covers logical collection serialization,
size/truncation rejection and rejection of native pointers/callbacks. These host
checks complement the hardware tests; they do not emulate the Wii U's allocator
or prove every persistent service combination safe.
