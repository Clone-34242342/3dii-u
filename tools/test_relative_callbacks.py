"""Synthetic self-relative callback records: callable boundaries, Thumb leaves, and look-alike data."""
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'third_party/3dsrecomp'))
from tools.recomp import gen, image

BASE, RECORD = 0x100000, 0x101000
FIRST, SECOND = BASE + 0x41, BASE + 0x81


def program(targets=(FIRST, SECOND), *, writable=False, slots=None, text_size=0x200):
    text = bytearray(text_size)
    struct.pack_into('<I', text, 0, 0xE12FFF1E)  # ARM entry: bx lr
    struct.pack_into('<3H', text, 0x40, 0xB500, 0x2007, 0xBD00)  # Thumb push {lr}; movs r0,#7; pop {pc}
    struct.pack_into('<5H', text, 0x80, 0x297F, 0xD800, 0x7001, 0x2001, 0x4770)  # Thumb leaf without a prologue
    record = bytearray(0x40)
    for i, target in enumerate(targets):
        offset = slots[i] if slots else i * 4
        struct.pack_into('<I', record, offset, (target - (RECORD + offset)) & 0xffffffff)
    img = image.Image.__new__(image.Image)
    img.text = image.Segment('text', BASE, bytes(text), len(text), False)
    data = image.Segment('record', RECORD, bytes(record), len(record), writable)
    img.segments = [img.text, data]
    img.texts = [img.text]
    img.entry, img.extra_entries, img.volatile, img.cros = BASE, [], set(), None
    img.index()
    return img


def analyzer(img):
    return gen.Analyzer(img, log=lambda *args: None)


class RelativeCallbacks(unittest.TestCase):
    def test_pair_discovers_thumb_prologue_and_leaf_as_callable_entries(self):
        an = analyzer(program())
        self.assertEqual(set(an.discover()), {BASE, FIRST, SECOND})
        self.assertTrue({FIRST, SECOND} <= an.pointed)
        self.assertEqual(an.img.read32(RECORD), (FIRST - RECORD) & 0xffffffff)

    def test_mixed_arm_thumb_and_duplicate_callbacks(self):
        for targets in ((FIRST, BASE), (SECOND, SECOND)):
            an = analyzer(program(targets))
            self.assertTrue(set(targets) <= set(an.discover()))
            self.assertTrue(set(targets) <= an.pointed)

    def test_positive_offsets_to_another_text_segment(self):
        img = program((0x102001, 0x102009))
        code = struct.pack('<2H', 0x2001, 0x4770) + bytes(4) + struct.pack('<2H', 0x2002, 0x4770)
        segment = image.Segment('module.text', 0x102000, code, len(code), False)
        img.segments.append(segment)
        img.texts.append(segment)
        img.index()
        self.assertTrue({0x102001, 0x102009} <= set(analyzer(img).discover()))

    def test_nonadjacent_writable_text_and_volatile_records_are_not_seeds(self):
        examples = [program(slots=(0, 8)), program(writable=True)]
        volatile = program()
        volatile.volatile.add(RECORD + 4)
        examples.append(volatile)
        embedded = program()
        embedded.texts.append(embedded.segments[1])
        examples.append(embedded)
        for img in examples:
            an = analyzer(img)
            an.funcs[BASE] = True
            self.assertEqual(an.relative_callbacks(), set())

    def test_invalid_or_misaligned_partner_rejects_whole_pair(self):
        for partner in (0, BASE + 0x82, RECORD, BASE + 0x201):
            self.assertEqual(set(analyzer(program((FIRST, partner))).discover()), {BASE})
        img = program()
        text = bytearray(img.text.data)
        struct.pack_into('<H', text, 0x80, 0xDE00)  # undefined instruction, not a callback entry
        img.text.data = bytes(text)
        self.assertEqual(set(analyzer(img).discover()), {BASE})

    def test_common_base_offsets_cannot_split_an_existing_function(self):
        img = program((BASE + 0xc0, BASE + 0xd0))
        text = bytearray(img.text.data)
        struct.pack_into('<4I', text, 0xc0, 0xE92D4000, 0xE3A00001, 0xE2800001, 0xE8BD8000)
        struct.pack_into('<4I', text, 0xd0, 0xE92D4000, 0xE3A00002, 0xE2800001, 0xE8BD8000)
        img.text.data = bytes(text)
        # A common-base table's second word resolves four bytes inside its real target if misread as self-relative.
        record = bytearray(img.segments[1].data)
        struct.pack_into('<I', record, 4, (BASE + 0xd0 - RECORD) & 0xffffffff)
        img.segments[1].data = bytes(record)
        for known in (False, True):
            an = analyzer(img)
            if known:
                an.extra.update((BASE + 0xc0, BASE + 0xd0))
            self.assertNotIn(BASE + 0xd4, an.discover())
            self.assertEqual(an.relative_callbacks(), set())

    def test_unknown_partner_inside_new_callback_is_not_a_separate_entry(self):
        an = analyzer(program((FIRST, FIRST + 2)))
        self.assertEqual(set(an.discover()), {BASE})

    def test_different_records_cannot_seed_an_interior_of_another_new_callback(self):
        for stack_free in (False, True):
            img = program((FIRST, SECOND, FIRST + 2, SECOND), slots=(0, 4, 16, 20))
            if stack_free:
                text = bytearray(img.text.data)
                struct.pack_into('<3H', text, 0x40, 0x2007, 0x2102, 0x4770)  # two leaf entries could otherwise share a body
                img.text.data = bytes(text)
            an = analyzer(img)
            self.assertEqual(set(an.discover()), {BASE, FIRST, SECOND})
            self.assertNotIn(FIRST + 2, an.pointed)

    def test_incomplete_and_over_limit_bodies_are_rejected(self):
        img = program((FIRST, BASE + 0x1ff))
        text = bytearray(img.text.data)
        struct.pack_into('<H', text, 0x1fe, 0x2001)  # falls outside text without returning
        img.text.data = bytes(text)
        self.assertEqual(set(analyzer(img).discover()), {BASE})
        img = program((BASE + 0xc0, SECOND), text_size=0xc0 + 20002 * 4)
        text = bytearray(img.text.data)
        for offset in range(0xc0, len(text) - 4, 4):
            struct.pack_into('<I', text, offset, 0xE1A00000)  # valid ARM instructions, but more than the exploration bound
        struct.pack_into('<I', text, len(text) - 4, 0xE12FFF1E)
        img.text.data = bytes(text)
        img.segments[1].addr = BASE + len(text) + 0x1000
        record = bytearray(img.segments[1].data)
        for i, target in enumerate((BASE + 0xc0, SECOND)):
            struct.pack_into('<I', record, i * 4, (target - (img.segments[1].addr + i * 4)) & 0xffffffff)
        img.segments[1].data = bytes(record)
        img.index()
        an = analyzer(img)
        an.funcs[BASE] = True
        self.assertEqual(an.relative_callbacks(), set())


if __name__ == '__main__':
    unittest.main()
