"""Exercise lookup texture reuse when the GPU heap cannot grow."""
from pathlib import Path
import subprocess
import sys
import tempfile

from test_vertex_cache import function

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'tools/win'))
import build3ds

source = (root / 'port/wiiu/gx2_backend.cpp').read_text()
harness = r'''
#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <vector>
using u32 = unsigned;
using u64 = unsigned long long;
static u64 use_counter = 1, st_luts;
static unsigned waits;
static bool exhausted;
static int image;
struct GX2Texture { struct { void *image; } surface; };
static GX2Texture make_float_lut(u32, u32) { return {{exhausted ? nullptr : &image}}; }
static void draw_done() { ++waits; }
#define FATAL(...) throw std::runtime_error("no LUT storage")
''' + source[source.index('struct LutTex {'):source.index('constexpr u32 LIGHT_POOL')] + function(source, 'bool find_lut(') + r'''
int main() {
    std::vector<LutTex> pool;
    LutTex *current = nullptr;
    assert(find_lut(pool, 4, 1, 256, 24, current));
    auto *first = current;
    ++use_counter;
    assert(find_lut(pool, 4, 2, 256, 24, current));
    exhausted = true;
    ++use_counter;
    assert(find_lut(pool, 4, 3, 256, 24, current));
    assert(current == first && current->key == 3 && current->t.surface.image && pool.size() == 2);
    assert(waits == 1);
    assert(!find_lut(pool, 4, 3, 256, 24, current));
    std::vector<LutTex> empty;
    current = nullptr;
    try {
        find_lut(empty, 4, 1, 256, 24, current);
        assert(false && "a missing first allocation must fail cleanly");
    } catch (const std::runtime_error &) {
        assert(empty.empty() && current == nullptr);
    }
}
'''

with tempfile.TemporaryDirectory(prefix='3dii-lut-') as directory:
    src, exe = Path(directory) / 'test.cpp', Path(directory) / 'test.exe'
    src.write_text(harness)
    subprocess.run([*build3ds.HOST_CXX, '-std=c++20', '-O1', '-static', str(src), '-lpthread', '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('GX2 lookup allocation failure reuses the oldest live texture and fails cleanly on an empty pool.')
