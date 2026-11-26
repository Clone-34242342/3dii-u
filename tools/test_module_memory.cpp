// Actual loader, boot loan statement, core loan predicate and both ldr:ro overlap guards.
#undef NDEBUG
#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>
#include "loader.cpp"

using MEMHeapHandle = void *;
constexpr u32 MEM_HEAP_FLAG_USE_LOCK = 1;
static void *MEMCreateExpHeapEx(void *p,u32,u32) { return p; }
#include "loan_impl.inc"
#include MODULE_MEMORY_RO_SOURCE
#include "boot_loan.inc"

u8 *g_mem;
Settings g_settings;
static unsigned failures,cases,guards_checked;
static void expect(bool value,const char *message) {
    if (!value && failures++ < 12) std::fprintf(stderr,"FAILED: %s\n",message);
}
void rt_log(const char *,...) {}
[[noreturn]] void rt_fatal(const char *fmt,...) {
    char message[512];va_list args;va_start(args,fmt);vsnprintf(message,sizeof message,fmt,args);va_end(args);
    throw std::runtime_error(message);
}
std::string read_cstr(u32 at,size_t limit) {
    std::string out;while(limit-- && g_mem[at])out+=(char)g_mem[at++];return out;
}
namespace kernel {
u64 system_tick() { return 0; }
void register_service(std::shared_ptr<Service>) {}
void Service::unimplemented(IPC &,bool) {}
}

static constexpr u32 reported=0x00d9f000,reported_end=0x00f0e000,clear_begin=0x00d00000;
static std::vector<u8> header(size_t size=0x400) {
    std::vector<u8> ex(size);
    auto put=[&](size_t at,u32 value){if(at+4<=size)memcpy(ex.data()+at,&value,4);};
    if(size>=4)memcpy(ex.data(),"TEST",4);
    put(0x10,0x00100000);put(0x18,4);put(0x20,0x00101000);put(0x30,0x00101000);put(0x3c,0x00a00000);
    return ex;
}
static void service(std::vector<u8> &ex,u32 slot,const std::string &name) {
    assert(slot<34 && name.size()<=8 && ex.size()>=0x250+8*(slot+1));
    memset(ex.data()+0x250+8*slot,0,8);memcpy(ex.data()+0x250+8*slot,name.data(),name.size());
}
static void write(const std::filesystem::path &p,const std::vector<u8> &data) {
    std::ofstream out(p,std::ios::binary);out.write((const char *)data.data(),data.size());assert(out.good());
}
static void loan() {
    // Reset only this test allocator; production does not unlend a live heap.
    mem::lent_heap=nullptr;mem::lent_from=mem::lent_to=0;
    kernel::production_boot_loan();
}
static void run(const std::filesystem::path &dir,const std::vector<u8> &ex,bool reserve,bool generated=false) {
    write(dir/"exheader.bin",ex);load_game(dir.string());set_game_cros(nullptr,generated?1:0);loan();
    expect(mem::lent_from==(reserve||generated?0x04000000u:clear_begin),"actual boot policy retains only required module region");
    expect(mem::lent(reported,reported_end-reported)==!(reserve||generated),"reported module range avoids host loan on cold loader/unknown ACL");
    ++cases;
}
static void overlap_guards() {
    kernel::Ro ro("ldr:ro");
    const u32 host=mem::lent_from+0x1000,source=0x02000000,ipcbuf=0x1000;
    for(u32 cmd:{1u,4u}) {
        memset(gptr(host),0xa5,0x1000);memset(gptr(source),0x5a,0x1000);memset(gptr(ipcbuf),0,0x100);
        w32(ipcbuf,cmd<<16);w32(ipcbuf+4,source);w32(ipcbuf+8,cmd==1?0x1000:host);w32(ipcbuf+12,cmd==1?host:0x1000);
        kernel::IPC ipc(nullptr,ipcbuf,nullptr);bool stopped=false;
        try {ro.handle(ipc);} catch(const std::runtime_error &e) {stopped=std::string(e.what()).find("lent to the host")!=std::string::npos;}
        expect(stopped,"actual Initialize and LoadCRO reject overlaps before memmove");
        expect(std::all_of(gptr(host),gptr(host)+0x1000,[](u8 b){return b==0xa5;}),"actual guard preserves host canary bytes");
        ++guards_checked;
    }
}
template<class T>constexpr bool has_policy=requires(T value){value.may_load_modules;};
int main(int argc,char **argv) {
    if(argc!=2)return 2;std::filesystem::path dir=argv[1];std::filesystem::create_directories(dir);
    g_mem=(u8*)calloc(0x20010000,1);assert(g_mem);
    std::vector<u8> code(0x1000);memcpy(code.data()+0x20,"ldr:ro",6);write(dir/"code.bin",code);
    g_game=GameInfo{};set_game_cros(nullptr,0);loan();expect(!mem::lent(reported,reported_end-reported),"default GameInfo is unknown and reserves modules");
    auto known=header();service(known,0,"fs:USER");service(known,33,"ABCDEFGH");run(dir,known,false);run(dir,known,false,true);
    auto gap=header();service(gap,33,"ABCDEFGH");run(dir,gap,false);
    for(u32 slot:{0u,31u,32u,33u}) {
        auto ex=known;service(ex,slot,"ldr:ro");run(dir,ex,true);
    }
    for(const char *name:{"ldr:rox","ldr:roxx","xldr:ro","LDR:RO"}) {
        auto ex=header();service(ex,32,name);run(dir,ex,false);
    }
    run(dir,header(),true);run(dir,header(0x210),true);run(dir,header(0x3ff),true);
    for(const std::string &name:{std::string("fs\0USER",7),std::string("ldr:ro\0X",8),std::string("fs:\x1f",4),std::string("fs:\x7f",4),std::string("fs:\x80",4),std::string("*"),std::string("ldr:*")}) {
        auto ex=known;service(ex,33,name);run(dir,ex,true);
    }
    for(const char *name:{"fs: USR","fs:USER "}) {
        auto ex=known;service(ex,33,name);run(dir,ex,true);
    }
    // A loader-capable load, complete non-loader load, unknown load and failed load must never inherit the previous decision.
    auto loader=known;service(loader,31,"ldr:ro");run(dir,loader,true);run(dir,known,false);run(dir,header(0x210),true);run(dir,known,false);
    std::filesystem::remove(dir/"exheader.bin");bool missing=false;
    try {load_game(dir.string());}catch(const std::runtime_error &){missing=true;}
    expect(missing,"missing exheader fails loading");loan();expect(!mem::lent(reported,reported_end-reported),"failed repeated load resets policy to unknown");
    if constexpr(has_policy<GameInfo>) {
        for(size_t size:{size_t(0),size_t(0x200),size_t(0x20f)}) {
            run(dir,known,false);write(dir/"exheader.bin",header(size));bool short_header=false;
            try {load_game(dir.string());}catch(const std::runtime_error &){short_header=true;}
            expect(short_header,"short exheader fails before descriptor reads");loan();expect(!mem::lent(reported,reported_end-reported),"short repeated load resets policy to unknown");
        }
    }
    run(dir,loader,true);overlap_guards();
    expect(!mem::lent(0x03ffffff,1) && mem::lent(0x03ffffff,2),"actual loan predicate respects64MiB exactly");
    run(dir,known,false);overlap_guards();
    for(bool reserve:{false,true}) {
        run(dir,reserve?loader:known,reserve);
        for(u32 high:{0x04000000u,0x05000000u,0x07f00000u}) {
            for(bool generated:{false,true}) {
                g_game.data_addr=high-0x100000;g_game.data_size=g_game.bss_size=0;set_game_cros(nullptr,generated?1:0);loan();
                if(high<=mem::HEAP_BASE-(8<<20))expect(mem::lent_from==high,"high clear boundary is never lowered to64MiB");
                else expect(!mem::lent_heap,"less than8MiB headroom is not lent");
            }
        }
    }
    free(g_mem);std::printf("module memory: %u real loads, %u actual RO guards, %u failures\n",cases,guards_checked,failures);return failures?1:0;
}
