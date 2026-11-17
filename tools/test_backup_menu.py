"""Compile the actual menu input handler with host controls; never launches a game."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools' / 'win'))
import build3ds


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


source = (ROOT / 'port' / 'wiiu' / 'gx2_backend.cpp').read_text()
code = r'''
#undef NDEBUG
#include <cassert>
#include <algorithm>
#include <atomic>
#include "save_backup.h"
using u32 = unsigned;
enum { VPAD_BUTTON_STICK_L=1, VPAD_BUTTON_ZL=2, VPAD_BUTTON_ZR=4,
       VPAD_BUTTON_A=8, VPAD_BUTTON_B=16, VPAD_BUTTON_UP=32,
       VPAD_BUTTON_DOWN=64, VPAD_BUTTON_LEFT=128, VPAD_BUTTON_RIGHT=256 };
namespace kernel { void set_clock_speed(int) {} }
namespace audio { void set_volume(float) {} }
struct { bool show_fps; } g_settings;
namespace wiiu {
save_backup::Requests save_backup::requests;
int cap_game, cap_fps;
static volatile bool menu_is_open, menu_quit;
static std::atomic<bool> menu_restore_confirm;
static volatile int menu_item, menu_speed=100, menu_volume=100;
static bool menu_allowed=true, settings_due;
void next_language(int) {} void next_tv_layout(int) {} void next_pad_layout(int) {} void next_resolution() {}
'''
code += function(source, 'static int menu_items()') + '\n' + function(source, 'void menu_input(u32 pressed, u32 held)')
code += r'''
}
int main() {
    using namespace wiiu;
    using save_backup::Action;
    for (int cap : {0,30}) {
        cap_game=cap; cap_fps=cap; menu_is_open=false; menu_quit=false;
        menu_input(VPAD_BUTTON_STICK_L,0);
        assert(menu_is_open && !menu_restore_confirm);
        assert(menu_items() == (cap ? 12 : 11));
        menu_item=menu_items()-2;
        menu_input(VPAD_BUTTON_A,0);
        assert(menu_restore_confirm && !save_backup::requests.view().busy);
        menu_input(VPAD_BUTTON_B,0);
        assert(!menu_restore_confirm && menu_is_open);
        menu_input(VPAD_BUTTON_A,0);
        menu_input(VPAD_BUTTON_UP,0);
        assert(!menu_restore_confirm);
        menu_item=menu_items()-2;
        menu_input(VPAD_BUTTON_A,0); menu_input(VPAD_BUTTON_A,0);
        assert(save_backup::requests.take() == Action::StageRestore);
        assert(!menu_restore_confirm && save_backup::requests.view().busy);
        menu_input(VPAD_BUTTON_B,0);
        assert(menu_is_open); // cannot unpause/quit during the snapshot
        assert(!save_backup::requests.request(Action::Export));
        save_backup::requests.complete("Restore pending");
        menu_item=menu_items()-3;
        menu_input(VPAD_BUTTON_A,0);
        assert(save_backup::requests.take() == Action::Export);
        save_backup::requests.complete("Backup complete");
        if (cap) {
            menu_item=8; menu_input(VPAD_BUTTON_A,0);
            assert(!cap_fps && !save_backup::requests.view().busy);
        }
        menu_item=menu_items()-1; menu_input(VPAD_BUTTON_A,0);
        assert(menu_quit);
    }
}
'''
out = ROOT / 'build_win' / 'checkpoint_backup'
out.mkdir(parents=True, exist_ok=True)
cpp = out / 'test_menu.cpp'
exe = out / 'test_menu.exe'
cpp.write_text(code)
subprocess.run(build3ds.HOST_CXX + ['-std=c++20', '-O1', '-static', '-I' + str(ROOT / 'port' / 'wiiu'), str(cpp), '-lpthread', '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True, timeout=10)
print('backup menu: actual input confirmation, cancellation, queue and capped/uncapped rows passed')
