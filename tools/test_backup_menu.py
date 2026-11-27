"""Compile the actual menu input handler with host controls; never launches a game."""
from pathlib import Path
import subprocess
import sys
import tempfile

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
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <type_traits>
#include "menu_shortcut.h"
#include "save_backup.h"
using u32 = unsigned;
enum { VPAD_BUTTON_STICK_L=1, VPAD_BUTTON_ZL=2, VPAD_BUTTON_ZR=4,
       VPAD_BUTTON_A=8, VPAD_BUTTON_B=16, VPAD_BUTTON_UP=32,
       VPAD_BUTTON_DOWN=64, VPAD_BUTTON_LEFT=128, VPAD_BUTTON_RIGHT=256 };
std::string settings_directory;
namespace kernel { void set_clock_speed(int) {} std::string save_root() { return settings_directory; } }
namespace audio { void set_volume(float) {} }
SETTINGS_DECLARATION
namespace wiiu {
save_backup::Requests save_backup::requests;
int cap_game, cap_fps;
static volatile bool menu_is_open, menu_quit;
static std::atomic<bool> menu_restore_confirm;
static volatile int menu_item, menu_speed=100, menu_volume=100;
static bool menu_allowed=true, settings_due;
void next_language(int) {} void next_tv_layout(int) {} void next_pad_layout(int) {}
'''
settings = (ROOT / 'third_party/3dsrecomp/runtime/settings.cpp').read_text()
header = (ROOT / 'third_party/3dsrecomp/runtime/settings.h').read_text()
declaration = '#define TARGET_OS_IOS 0\nusing s64 = long long;\n' + function(header, 'struct Settings {') + ';\nSettings g_settings;'
code = code.replace('SETTINGS_DECLARATION', declaration)
settings_code = 'int test_setenv(const char *, const char *, int) { return 0; }\n#define setenv test_setenv\n'
for signature in ['static std::string settings_path()', 'void settings_load()', 'void settings_save()']:
    settings_code += function(settings, signature) + '\n'
code = code.replace('namespace wiiu {', settings_code + '\nnamespace wiiu {', 1)
code += 'u32 top_rows = 720; int start_resolution = 720;\n'
startup = next(line for line in source.splitlines() if line.strip().startswith('top_rows = start_resolution = '))
code += 'void start_resolution_for_test() { ' + startup + '\n}\n'
code += function(source, 'void rt_scale(') + '\n' + function(source, 'static void next_resolution(') + '\n'
code += function(source, 'static int menu_items()') + '\n' + function(source, 'void menu_input(u32 pressed, bool toggle)') + '\n'
frontend = (ROOT / 'port' / 'wiiu' / 'frontend_wiiu.cpp').read_text()
code += 'u32 overlay_combo;\n' + function(frontend, 'static bool overlay_pressed(')
code += r'''
}
int main(int argc, char **argv) {
    assert(argc == 2); settings_directory = argv[1];
    int checks = 0, failures = 0;
#define CHECK(test) do { ++checks; if (!(test)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #test); } } while (0)
    using namespace wiiu;
    using save_backup::Action;

    constexpr menu_shortcut::Button keys[] = {{"A", VPAD_BUTTON_A}, {"B", VPAD_BUTTON_B}};
    constexpr u32 ab = VPAD_BUTTON_A | VPAD_BUTTON_B;
    constexpr u32 triggers = VPAD_BUTTON_ZL | VPAD_BUTTON_ZR;
    CHECK(overlay_pressed(VPAD_BUTTON_STICK_L, VPAD_BUTTON_STICK_L));
    CHECK(overlay_pressed(VPAD_BUTTON_ZR, triggers));
    CHECK(!overlay_pressed(VPAD_BUTTON_ZR, VPAD_BUTTON_ZR));
    CHECK(!overlay_pressed(0, triggers));
    for (auto invalid : {"", "A", "A+A", "A+B+C", "A+X", "A+"})
        CHECK(menu_shortcut::parse(invalid, keys) == 0);
    overlay_combo = menu_shortcut::parse("B+A", keys);
    CHECK(overlay_combo == ab && menu_shortcut::parse("A+B", keys) == ab);
    CHECK(!overlay_pressed(VPAD_BUTTON_STICK_L, VPAD_BUTTON_STICK_L));
    CHECK(!overlay_pressed(VPAD_BUTTON_ZR, triggers));
    CHECK(!overlay_pressed(VPAD_BUTTON_A, VPAD_BUTTON_A));
    CHECK(!overlay_pressed(VPAD_BUTTON_B, VPAD_BUTTON_B));
    CHECK(overlay_pressed(VPAD_BUTTON_A, ab | VPAD_BUTTON_STICK_L));
    CHECK(!overlay_pressed(0, ab));
    menu_input(VPAD_BUTTON_B, overlay_pressed(VPAD_BUTTON_B, ab));
    CHECK(menu_is_open);
    menu_restore_confirm = true;
    menu_input(VPAD_BUTTON_B, overlay_pressed(VPAD_BUTTON_B, ab));
    CHECK(!menu_restore_confirm && !menu_is_open);
    menu_allowed = false;
    menu_input(VPAD_BUTTON_A, overlay_pressed(VPAD_BUTTON_A, ab));
    CHECK(!menu_is_open);
    menu_allowed = true;
    overlay_combo = 0;

    CHECK(Settings{}.resolution == 720);
    for (int rows : {240, 440, 480, 720}) {
        g_settings = Settings{}; g_settings.resolution = rows; settings_save();
        g_settings = Settings{}; settings_load();
        CHECK(g_settings.resolution == rows);
        start_resolution_for_test();
        CHECK(top_rows == (u32)rows && start_resolution == rows);
        float sx, sy;
        rt_scale(240, 400, sx, sy);
        CHECK(std::fabs(sx - rows / 240.0f) < 0.000001f && std::fabs(sy - rows / 240.0f) < 0.000001f);
        CHECK((u32)(240 * sx) == (u32)rows);
        std::printf("resolution %dp: top target %ux%u, scale %.8f\n", rows, (u32)(400 * sy), (u32)(240 * sx), sx);
        rt_scale(480, 400, sx, sy);
        CHECK(std::fabs(sx - rows / 480.0f) < 0.000001f && std::fabs(sy - rows / 240.0f) < 0.000001f);
        rt_scale(256, 416, sx, sy);
        CHECK(std::fabs(sx - rows / 240.0f) < 0.000001f && std::fabs(sy - rows / 240.0f) < 0.000001f);
        rt_scale(240, 320, sx, sy); CHECK(sx == 2 && sy == 2);
        rt_scale(256, 512, sx, sy, 240, 400);
        CHECK(std::fabs(sx - rows / 240.0f) < 0.000001f && sx == sy);
        rt_scale(512, 800, sx, sy); CHECK(sx == 1.5f && sy == 1.5f);
        rt_scale(64, 64, sx, sy); CHECK(sx == 1 && sy == 1);
    }
    for (const char *invalid : {"0", "-1", "241", "439", "441", "479", "481", "721", "1080", "bad"}) {
        { std::ofstream file(settings_directory + "/settings.ini"); file << "resolution=" << invalid << '\n'; }
        g_settings = Settings{}; settings_load(); CHECK(g_settings.resolution == 720);
    }
    g_settings = Settings{}; top_rows = start_resolution = 720; menu_is_open = true; menu_item = 6;
    for (int rows : {240, 440, 480, 720, 240, 440, 480, 720}) {
        settings_due = false; menu_input(VPAD_BUTTON_RIGHT, 0);
        CHECK(g_settings.resolution == rows && settings_due);
        CHECK(top_rows == 720 && start_resolution == 720);
    }
    for (int rows : {480, 440, 240, 720, 480, 440, 240, 720}) {
        settings_due = false; menu_input(VPAD_BUTTON_LEFT, 0);
        CHECK(g_settings.resolution == rows && settings_due);
        CHECK(top_rows == 720 && start_resolution == 720);
    }
    menu_is_open = false;
    for (int cap : {0,30}) {
        cap_game=cap; cap_fps=cap; menu_is_open=false; menu_quit=false;
        menu_input(0, true);
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
    std::printf("resolution checks: %d, failures: %d\n", checks, failures);
    return failures ? 1 : 0;
}
'''
out = ROOT / 'build_win' / 'checkpoint_backup'
out.mkdir(parents=True, exist_ok=True)
cpp = out / 'test_menu.cpp'
exe = out / 'test_menu.exe'
cpp.write_text(code)
subprocess.run(build3ds.HOST_CXX + ['-std=c++20', '-O1', '-static', '-I' + str(ROOT / 'port' / 'wiiu'), str(cpp), '-lpthread', '-o', str(exe)], check=True)
with tempfile.TemporaryDirectory(prefix='3dii-resolution-') as folder:
    subprocess.run([str(exe), folder], check=True, timeout=10)
print('backup menu: actual input confirmation, cancellation, queue and capped/uncapped rows passed')
