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
        ("game: [srv] unhandled port access", "missing service command"),

        # 4. shader fallback
        ("restart: 2 vertex shaders the pack lacks and the console did not compile", "shader fallback"),
        ("restart: the console compiler failed 3 times", "shader fallback"),
        ("blank_save: the console compiler is not running", "shader fallback"),

        # 5. save failure
        ("restart: save flush failed", "save failure"),
        ("restart: formatted the save again, 2 times", "save failure"),
        ("restart: the game saved in common storage, not the player's", "save failure"),

        # 6. no useful frames
        ("restart: nothing drawn", "no useful frames"),
        ("blank_save: no pictures of the GamePad", "no useful frames"),
        ("blank_save: insufficient visual coverage: 0 TV/GamePad capture pairs; at least 2 needed to compare rendering", "no useful frames"),

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

    print("test_failure_categories: ok")


if __name__ == '__main__':
    test_categories()
