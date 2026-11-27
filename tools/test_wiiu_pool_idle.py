"""Check the production worker's Wii U idle and work publication paths on the host."""
from pathlib import Path
import subprocess
import sys
import tempfile

from test_vertex_cache import function

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'tools/win'))
import build3ds

source = (root / 'third_party/3dsrecomp/runtime/gpu/gpu.cpp').read_text()
start = source.index('namespace {\nstd::vector<std::thread> workers;')
end = source.index('#ifdef __WIIU__\n// One worker', start)
pool = source[start:end]
pool = pool.replace(function(pool, 'inline void cpu_relax()'), 'inline void cpu_relax() { ++relaxes; }')
parallel = function(source, 'void parallel_for(u32 n,')

harness = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
using u32 = unsigned;
using u64 = unsigned long long;
#define __WIIU__ 1
static std::atomic<u32> relaxes{0};
''' + pool + '\nu32 pool_threads() { return 2; }\n' + parallel + r'''
int main() {
    std::thread(worker_main, 1).detach();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!pool_sleepers.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    assert(pool_sleepers.load() == 1);
    assert(relaxes.load() == 0 && "idle Wii U worker should park without spinning");
    for (u32 publication = 0; publication < 70000; ++publication) {
        std::atomic<u32> seen[7]{};
        parallel_for(7, [&](u32 item, u32 tid) {
            assert(item < 7 && tid < 2);
            ++seen[item];
        });
        for (auto &count : seen) assert(count == 1);
    }
    std::_Exit(0);  // Detached workers intentionally live until process exit.
}
'''

with tempfile.TemporaryDirectory(prefix='3dii-pool-idle-') as directory:
    temp = Path(directory)
    src, exe = temp / 'test.cpp', temp / 'test.exe'
    src.write_text(harness)
    subprocess.run([*build3ds.HOST_CXX, '-std=c++20', '-O2', '-static', str(src), '-lpthread', '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=30)
print('Wii U vertex pool parked without spinning and executed 70,000 publications exactly once.')
