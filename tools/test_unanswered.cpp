// A command no service answers comes back as success with every word after the result zero, not as what the game's own request left in
// the buffer (service.cpp's Service::unimplemented). Built and run by tools/selftest.py.
#include <cassert>
#include <cstdarg>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "../third_party/3dsrecomp/runtime/service.cpp"

static std::vector<u8> memory(0x10000, 0xcc);
u8 *g_mem = memory.data();
int g_verbose;
u64 g_frame;
GameInfo g_game;
static std::string captured_log;
void rt_log(const char *format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    captured_log += text;
}
[[noreturn]] void rt_fatal(const char *, ...) { abort(); }
namespace kernel {
KStats kstats;
Handle add_handle(ObjPtr) { abort(); }
void register_all_services() { abort(); }
}

struct ReplyService : kernel::Service {
    u32 result, request;
    ReplyService(u32 result_, u32 request_) : Service("reply-test"), result(result_), request(request_) {}
    void handle(kernel::IPC &ipc) override {
        assert(ipc[1] == request);
        ipc.result(result);
        assert(ipc[1] == request);
    }
};

static void reply_diagnostics() {
    kernel::Thread thread;
    thread.tls = 0x1000 - 0x80;
    std::mutex mutex;
    kernel::Lock lock(mutex);
    struct Case { u32 request, result; int verbose; const char *error; };
    for (const auto &test : {Case{0x80000002, 0, 1, ""}, Case{0, 0xd900182f, 1, "error d900182f"},
                             Case{0x80000002, 0xd900182f, 1, "error d900182f"}, Case{0, 0xd900182f, 0, ""}}) {
        auto session = std::make_shared<kernel::Session>();
        session->service = std::make_shared<ReplyService>(test.result, test.request);
        w32(0x1000, 0x00160040);
        w32(0x1004, test.request);
        g_verbose = test.verbose;
        captured_log.clear();
        assert(kernel::service_request(session, &thread, lock) == 0);
        assert(r32(0x1000) == 0x00160040 && r32(0x1004) == test.result);
        if (*test.error) assert(captured_log.find(test.error) != std::string::npos);
        else assert(captured_log.find("-> error") == std::string::npos);
    }
    g_verbose = 0;
    puts("service diagnostics: live replies, request snapshots and quiet mode preserved");
}

int main() {
    reply_diagnostics();
    kernel::StubService boss("boss:U");
    for (bool succeed : {true, false}) {
        u32 request[64];
        request[0] = 0x00160082;  // two normal and two translate words, as GetNsDataHeaderInfo asks
        for (int i = 1; i < 64; i++) request[i] = 0x5eed0000 + i;
        w32(0xffc, 0xcafebabe);
        w32(0x1100, 0xdeadbeef);
        for (int i = 0; i < 64; i++) w32(0x1000 + 4 * i, request[i]);
        kernel::IPC ipc(nullptr, 0x1000, nullptr);
        if (succeed) boss.handle(ipc);
        else boss.unimplemented(ipc, false);
        assert(r32(0x1000) == 0x00160040);  // either reply still carries the result word alone
        assert(r32(0x1004) == (succeed ? 0u : 0xd900182f));
        for (int i = 2; i < 64; i++) assert(r32(0x1000 + 4 * i) == (succeed ? 0u : request[i]));
        for (int i = 0; i < 64; i++) assert(ipc.word(i) == request[i]);
        assert(ipc.cmd() == 0x16);
        assert(r32(0xffc) == 0xcafebabe && r32(0x1100) == 0xdeadbeef);
    }
    puts("unanswered commands: success zeroes replies; failure, request snapshot and canaries preserved");
}
