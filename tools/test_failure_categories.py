#!/usr/bin/env python3
"""Verify cold-build failure categorization in tools/check_ports.py: test_failure_categories.py"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_ports as cp  # noqa: E402


def test_categories():
    cases = [
        # 1. missing code
        ("restart: code the translation missed (1): [lazy] no code at 00100100 (lr=00100050)", "missing code"),
        ("blank_save: no recompiled code at 0x00100200; added to missing_functions.txt", "missing code"),
        ("restart: 2 fallback function(s) executed; needs recompile in functions.txt", "missing code"),

        # 2. unsupported instruction
        ("restart: unsupported instruction (1): unsupported opcode 0xe12fff32 at 00100140", "unsupported instruction"),
        ("blank_save: undefined instruction at 0x00100144", "unsupported instruction"),
        ("FATAL: fallback interpreter unsupported ARM opcode 0xe0000291 at 0x00100100", "unsupported instruction"),
        ("FATAL: fallback interpreter unsupported Thumb opcode 0xdead at 0x00100140", "unsupported instruction"),

        # 3. missing service command
        ("restart: unimplemented service command (1): [fs] unimplemented command 0x08010002 (failing)", "missing service command"),
        ("restart: [svc] unimplemented SVC 0x59 (lr=00123000)", "missing service command"),
        ("game: [srv] unhandled port access", "missing service command"),

        # 4. shader fallback
        ("restart: 2 vertex shaders the pack lacks and the console did not compile", "shader fallback"),
        ("restart: the console compiler failed 3 times", "shader fallback"),
        ("blank_save: the console compiler is not running", "shader fallback"),
        ("restart: shader fallback: 4 CPU draw batches, 0 geometry shader fallbacks", "shader fallback"),

        # 5. save failure
        ("restart: save flush failed", "save failure"),
        ("restart: formatted the save again, 2 times", "save failure"),
        ("restart: the game saved in common storage, not the player's", "save failure"),

        # 6. no useful frames
        ("restart: nothing drawn", "no useful frames"),
        ("blank_save: no pictures of the GamePad", "no useful frames"),
        ("blank_save: insufficient visual coverage: 0 TV/GamePad capture pairs; at least 2 needed to compare rendering", "no useful frames"),
        ("blank_save: unreadable TV picture", "no useful frames"),
        ("kept save: no new drawing while the guest stayed busy for at least 30 seconds", "no useful frames"),
        ("kept save: route ended before completing its required input", "no useful frames"),
        ("kept save: design strokes missing in the final GamePad capture", "no useful frames"),

        # Cemu validation (emulation artifacts, not console behavior)
        ("restart: Cemu can't decode vertex data this game drew with, so these aren't a console's pictures: Unsupported fmt ...", "cemu validation"),

        # Crash
        ("restart: Cemu stopped: process exited with code 1", "crash"),  # an exit alone does not establish an emulator artifact
        ("restart: crash (1): FATAL: unhandled exception", "crash"),
    ]

    for fault_msg, expected_cat in cases:
        actual_cat = cp.categorize_fault(fault_msg)
        assert actual_cat == expected_cat, f"Expected {expected_cat} for '{fault_msg}', got {actual_cat}"
    for opcode in ('ARM', 'Thumb'):
        message = f'FATAL: fallback interpreter unsupported {opcode} opcode 0xdead at 0x00100140'
        assert cp.re.search(cp.UNSUPPORTED, message, cp.re.IGNORECASE), 'The log scanner must use the same pattern as categorization'
    for message in (
        '     3 [fs] unimplemented command 0x08010002 (failing)',
        '     7 [nwm::UDS] unimplemented command 0x001D0000 (failing)',
        '     8 [svc] unimplemented SVC 0x59 (lr=00123000)',
    ):
        assert cp.re.search(cp.MISSING_SERVICE, message, cp.re.IGNORECASE), message
    assert not cp.re.search(cp.MISSING_SERVICE, '[boss:U] unimplemented command 0x00160082 (returning success)')

    shutdown = '    90 [quit] sound system shut down\n'
    assert cp.quit_fault('    90 [quit] audio thread stopped after 13 ms\n' + shutdown) is None
    assert cp.quit_fault('    90 [quit] audio thread still running after 2000 ms\n' + shutdown) == 'audio thread still running at quit'
    assert cp.quit_fault('    90 [quit] audio thread stopped after 13 ms\n') == 'the game did not close when it was quit'
    final_zero = '[gx2 final] native draws 6, CPU batches 0, fragment misses 0, immediate native draws 1, immediate CPU runs 0\n'
    periodic_zero = '[gx2] 60 fps, 0 on CPU, 0 fragment misses\n[gx2 immediate] native draws 1, CPU shader runs 0\n'
    assert not cp.shader_faults(periodic_zero + final_zero)
    assert cp.shader_faults(periodic_zero), 'Periodic zeros cannot certify the last interval'
    assert cp.shader_faults('[gx2] 60 fps, 0 on CPU, 0 fragment misses\n'), 'Missing immediate/final counters cannot pass'
    for field, value in [('immediate CPU runs', 2), ('CPU batches', 1), ('fragment misses', 1)]:
        text = periodic_zero + final_zero.replace(field + ' 0', field + ' ' + str(value))
        assert any('shader fallback:' in fault for fault in cp.shader_faults(text)), field
    assert cp.shader_faults(periodic_zero + final_zero.replace(', immediate CPU runs 0', '')), 'Incomplete final counters cannot pass'
    assert cp.shader_faults(periodic_zero.replace('CPU shader runs 0', 'CPU shader runs 2') + final_zero), 'Earlier CPU work cannot disappear'
    assert len(cp.shader_faults('[gx2] 30 fps, 3 on CPU, 5 fragment misses\n[gx2] 60 fps, 0 on CPU, 5 fragment misses\n' + final_zero)) == 2
    assert '5 fragment misses' in cp.shader_faults('0 on CPU, 5 fragment misses\n0 on CPU, 5 fragment misses\n' + final_zero)[0]
    assert cp.shader_faults(''), 'Missing metrics cannot imply zero fallbacks'
    assert cp.shader_faults("2 geometry shaders' draws the pack lacked a shader for, drawn by the CPU")
    # Compacted samples from a route that drew earlier and then stalled while loading a stage.
    def samples(lists=20172, transfers=10053, idle=0):
        return '\n'.join(f'[wiiu] frame {frame}: gx lists {lists}, transfers {transfers}, guest threads idle {idle}%'
                         for frame in range(5700, 7501, 300))

    assert cp.render_stall_seconds(samples()) == 30
    assert cp.render_stall_seconds(samples(lists=0, transfers=0)) == 0
    assert cp.render_stall_seconds(samples(idle=95)) == 0
    assert cp.render_stall_seconds(samples().replace('transfers 10053', 'transfers 10054', 1)) == 25
    assert cp.render_stall_seconds(samples() + '\n[wiiu] frame 7800: gx lists 20173, transfers 10053, guest threads idle 0%') == 0

    route = cp.ROUTES['acnl_design']
    stroke, seconds = cp.ROUTE_CHECKPOINTS['acnl_design']
    stroke_command = route.index(stroke) + 1
    early_log = f'[live] command {stroke_command} starts (40 frames)\n'
    complete_log = early_log + f'[live] command {stroke_command + 1} starts (2400 frames)\n'
    assert not cp.route_checkpoint_reached(route, route, stroke, early_log)
    assert not cp.route_checkpoint_reached(route, route[:stroke_command] + ['KEY quit'], stroke, complete_log)
    assert cp.route_checkpoint_reached(route, cp.until(route, seconds - 15) + ['KEY quit'], stroke, complete_log)

    print("test_failure_categories: ok")


if __name__ == '__main__':
    test_categories()
