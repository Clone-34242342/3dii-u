"""Exercise the actual pool claim/wait code with Wii U's no-spin policy."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'third_party/3dsrecomp/runtime/gpu/gpu.cpp').read_text()
a=s.index('namespace {\nstd::vector<std::thread> workers;');b=s.index('#ifdef __WIIU__\n// One worker',a)
shared=s[a:b]
a=s.index('void parallel_for(u32 n,');b=s.index('// ---------------------------------------------------------------- uniform / program upload',a)
loop=s[a:b]
code='''#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <vector>
#include <algorithm>
#include <cassert>
#include <cstdlib>
using u32=unsigned;using u64=unsigned long long;
#define __WIIU__ 1
std::atomic<bool> g_pool_idle_spin{false};
'''+shared+'''\nu32 pool_threads(){return 2;}
'''+loop+'''
int main(){
 std::thread(worker_main,1).detach();
 // More than 65535 publications exercises generation rollover and late wakeup.
 for(unsigned gen=0;gen<70000;++gen){
  std::atomic<unsigned> seen[7]{};
  parallel_for(7,[&](unsigned item,unsigned tid){assert(item<7&&tid<2);seen[item].fetch_add(1);});
  for(auto &n:seen)assert(n==1);
 }
 // No new publication: the worker must eventually park instead of spin.
 for(unsigned i=0;i<1000000&&!pool_sleepers.load();++i)std::this_thread::yield();
 assert(pool_sleepers.load()==1);
 std::_Exit(0); // pool is intentionally process-lifetime state
}
'''
with tempfile.TemporaryDirectory() as t:
 p=Path(t);(p/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++20','-O2','-pthread',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,timeout=60)
print('PASS: 70000 no-spin publications, exactly-once execution, generation rollover, idle worker parks')
