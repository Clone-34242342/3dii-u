// Production FS IPC, scheduler and save cache against private host files; only storage/OS calls are injected.
#undef NDEBUG
#include <cassert>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <thread>
#include <share.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../third_party/3dsrecomp/runtime/kernel.cpp"
#include "../third_party/3dsrecomp/runtime/service.cpp"

struct FSClient {};
struct FSCmdBlock {};
using FSStatus = int;
using FSErrorFlag = int;
using FSDirectoryHandle = int;
struct FSDirectoryEntry { char name[256]; };
using OSTime = long long;
using OSDynLoad_Module = void *;
enum { OS_DYNLOAD_OK=0, OS_DYNLOAD_EXPORT_FUNC=0, FS_ERROR_FLAG_ALL=-1, FS_STATUS_OK=0, FS_STATUS_END=-2 };
static void FSInit() {}
static int FSAddClient(FSClient *,int) { return 0; }
static void FSInitCmdBlock(FSCmdBlock *) {}
static FSStatus FSOpenDir(FSClient *, FSCmdBlock *, const char *, FSDirectoryHandle *, FSErrorFlag) { assert(false); return -1; }
static FSStatus FSReadDir(FSClient *, FSCmdBlock *, FSDirectoryHandle, FSDirectoryEntry *, FSErrorFlag) { assert(false); return -1; }
static FSStatus FSCloseDir(FSClient *, FSCmdBlock *, FSDirectoryHandle, FSErrorFlag) { assert(false); return -1; }
static OSTime OSGetSystemTime() { return (OSTime)(kernel::real_ns()/1000000); }
static OSTime OSTicksToMilliseconds(OSTime value) { return value; }
static OSTime OSTicksToSeconds(OSTime value) { return value/1000; }
static int OSDynLoad_Acquire(const char *,OSDynLoad_Module *) { return 1; }
static int OSDynLoad_FindExport(OSDynLoad_Module,int,const char *,void **) { return 1; }
static ssize_t test_pwrite(int,const void *,size_t,long long);
static ssize_t test_pread(int,void *,size_t,long long);
static int test_fstat(int,struct stat *);
static int test_ftruncate(int,off_t);
static int test_close(int);
#define __WIIU__
#define memalign(align,n) malloc(n)
#define pwrite test_pwrite
#define pread test_pread
#define fstat test_fstat
#define ftruncate test_ftruncate
#define close test_close
#include "../third_party/3dsrecomp/runtime/services/fs.cpp"
#include "../port/wiiu/save_storage.cpp"
#undef close
#undef ftruncate
#undef fstat
#undef pwrite
#undef pread
#undef memalign
#undef __WIIU__

u8 *g_mem;
int g_verbose;
u64 g_frame,rt_lookups,rt_lookup_misses;
GameInfo g_game;
std::string g_save_base;
void rt_log(const char *,...) {}
[[noreturn]] void rt_fatal(const char *,...) { abort(); }
void run_guest(CPU *) { assert(false); }
std::string read_cstr(u32,size_t) { assert(false); return {}; }
void cloud_save_written() {}
namespace stacks { void save_written(const std::string &) {} }
static std::vector<std::pair<u32, u32>> gpu_writes;
namespace kernel {
void gsp_cache_op(u32,u32,bool) { assert(false); }
void gsp_cpu_wrote(u32 addr, u32 size) { gpu_writes.emplace_back(addr, size); }
void register_all_services() { assert(false); }
const std::vector<std::array<u8,0x60>> &console_miis(int *) { assert(false); static const std::vector<std::array<u8,0x60>> none;return none; }
u16 mii_crc16(const u8 *,size_t) { assert(false);return 0; }
}
static std::recursive_mutex storage_mutex;
static void opening_storage(u32);
void wiiu::note(const char *,...) {}
void wiiu::fs_begin(u32 op) { opening_storage(op);storage_mutex.lock(); }
void wiiu::fs_end() { storage_mutex.unlock(); }
void wiiu::romfs_begin() { storage_mutex.lock(); }
void wiiu::romfs_end() { storage_mutex.unlock(); }
void wiiu::direct_read_open(const char *,int,u64) { assert(false); }
bool wiiu::direct_read(void *,u32,u64) { assert(false);return false; }
bool wiiu::fsa_direct_flush_quota(const char *) { return false; }
void wiiu::fsa_direct_check_writes() {}

namespace fs=std::filesystem;
enum class Delay { None,Write,Size,Resize };
static Delay delay=Delay::None;
static std::mutex progress_mutex;
static std::condition_variable progress_cv;
static bool progressed;
static bool test_open,open_waiting,checkpoint_waiting,holder_ready;
static bool fail_write,fail_stat,fail_truncate,fail_close;
static int partial_write;
static std::function<void()> while_writing;
static std::function<void()> while_reading;
static std::function<void()> after_reading;
static bool fail_read;
static void opening_storage(u32 op) {
    if(test_open && op==0x3802) { std::lock_guard lock(progress_mutex);open_waiting=true;progress_cv.notify_all(); }
}
static void delayed(Delay kind) {
    if (delay!=kind) return;
    std::unique_lock lock(progress_mutex);
    assert(progress_cv.wait_for(lock,std::chrono::seconds(2),[]{return progressed;}) && "another guest must run while storage is blocked");
}
static ssize_t test_pwrite(int fd,const void *data,size_t n,long long at) {
    delayed(Delay::Write);
    if (while_writing) { auto callback=std::move(while_writing);while_writing={};callback(); }
    if (partial_write>0) { partial_write=0;fail_write=true;return ::pwrite(fd,data,n/2,at); }
    if (fail_write) { errno=EIO;return -1; }
    return ::pwrite(fd,data,n,at);
}
static ssize_t test_pread(int fd,void *data,size_t n,long long at) {
    if(while_reading) {auto callback=std::move(while_reading);while_reading={};callback();}
    if(fail_read){errno=EIO;return -1;}
    const auto result=::pread(fd,data,n,at);
    if(after_reading) {auto callback=std::move(after_reading);after_reading={};callback();}
    return result;
}
static int test_fstat(int fd,struct stat *st) { delayed(Delay::Size);if(fail_stat){errno=EIO;return -1;}return ::fstat(fd,st); }
static int test_ftruncate(int fd,off_t n) { delayed(Delay::Resize);if(fail_truncate){errno=EIO;return -1;}return ::ftruncate(fd,n); }
static int test_close(int fd) { const int r=::close(fd);if(fail_close){errno=EIO;return -1;}return r; }
static void put(const fs::path &p,const std::string &bytes) { fs::create_directories(p.parent_path());std::ofstream(p,std::ios::binary).write(bytes.data(),bytes.size()); }
static std::string get(const fs::path &p) { std::ifstream file(p,std::ios::binary);return {std::istreambuf_iterator<char>(file),{}}; }
static std::shared_ptr<wiiu::SaveFile> cached(const fs::path &p,const std::string &bytes) {
    put(p,"previous bytes");const int fd=::open(p.generic_string().c_str(),O_RDWR|O_BINARY);assert(fd>=0);
    auto file=wiiu::save_file_open(p.generic_string(),fd);::close(fd);assert(file);
    wiiu::save_file_resize(*file,0);assert(wiiu::save_file_write(*file,bytes.data(),(u32)bytes.size(),0)==(long)bytes.size());
    wiiu::save_file_close(*file);return file;
}
static void ipc_words(u32 header,std::initializer_list<u32> words) {
    memset(gptr(0x1000),0,256);w32(0x1000,header);u32 at=0x1004;for(u32 word:words){w32(at,word);at+=4;}
}
static void scheduler_io(const fs::path &path,Delay operation) {
    using namespace kernel;
    put(path,"original");FileSession file;file.path=path.generic_string();file.fd=::open(file.path.c_str(),O_RDWR|O_BINARY);assert(file.fd>=0);
    auto a=std::make_shared<Thread>(),b=std::make_shared<Thread>();a->id=1;b->id=2;a->priority=b->priority=0x30;
    threads={a,b};a->status=TStatus::Running;b->status=TStatus::Ready;b->ready_seq=1;running=a.get();tls_thread=a.get();
    progressed=false;delay=operation;
    std::thread other([&]{
        tls_thread=b.get();Lock lock(kmutex);b->cv.wait(lock,[&]{return running==b.get();});assert(a->status==TStatus::WaitSleep);
        {std::lock_guard progress(progress_mutex);progressed=true;}progress_cv.notify_one();
        b->status=TStatus::WaitSleep;block_current(lock);
        b->status=TStatus::Dead;resume_thread(a.get());running=nullptr;schedule_async();
    });
    {
        Lock lock(kmutex);
        const std::string bytes("\0\x1a\xff saved",9);memcpy(gptr(0x4000),bytes.data(),bytes.size());
        if(operation==Delay::Write) ipc_words(0x08030102,{0,0,(u32)bytes.size(),0,((u32)bytes.size()<<4)|0xA,0x4000});
        else if(operation==Delay::Size) ipc_words(0x08040000,{});
        else ipc_words(0x08050080,{64,0});
        IPC ipc(a.get(),0x1000,&lock);file.handle(ipc);
        assert(lock.owns_lock() && running==a.get() && progressed && r32(0x1004)==0);
        delay=Delay::None;
        if(operation==Delay::Write) { assert(r32(0x1008)==bytes.size() && get(path)==bytes); }
        if(operation==Delay::Size) assert(r64(0x1008)==8);
        if(operation==Delay::Resize) { struct stat actual;assert(!::fstat(file.fd,&actual)&&actual.st_size==64); }
        resume_thread(b.get());a->status=TStatus::WaitSleep;block_current(lock);assert(running==a.get());
        a->status=TStatus::Dead;running=nullptr;
    }
    other.join();threads.clear();tls_thread=nullptr;
    ::close(file.fd);file.fd=-1;
    if(operation==Delay::Resize) assert(fs::file_size(path)==64);
}
static void scheduler_open(const fs::path &path) {
    using namespace kernel;
    put(path,"opened safely");FsUser user("fs:USER");archives[7]={4,path.parent_path().generic_string()};
    auto a=std::make_shared<Thread>(),b=std::make_shared<Thread>();a->id=1;b->id=2;a->priority=b->priority=0x30;
    threads={a,b};a->status=TStatus::Running;b->status=TStatus::Ready;b->ready_seq=1;running=a.get();tls_thread=a.get();
    test_open=true;open_waiting=checkpoint_waiting=holder_ready=false;std::atomic<bool> checkpoint_done=false;
    bool guest_parked=false;
    std::thread holder([&]{
        std::lock_guard storage(storage_mutex);std::unique_lock lock(progress_mutex);holder_ready=true;progress_cv.notify_all();
        assert(progress_cv.wait_for(lock,std::chrono::seconds(2),[]{return open_waiting&&checkpoint_waiting;}));
    });
    {std::unique_lock lock(progress_mutex);assert(progress_cv.wait_for(lock,std::chrono::seconds(2),[]{return holder_ready;}));}
    std::thread other([&]{
        tls_thread=b.get();Lock lock(kmutex);b->cv.wait(lock,[&]{return running==b.get();});
        memset(gptr(0x1200),0,256);w32(0x1200,0x080E0080);w32(0x1204,7);IPC close(b.get(),0x1200,&lock);user.handle(close);
        assert(r32(0x1204)==0&&!archives.count(7));b->status=TStatus::WaitSleep;
        {std::lock_guard progress(progress_mutex);guest_parked=true;progress_cv.notify_all();}
        block_current(lock);
        b->status=TStatus::Dead;resume_thread(a.get());running=nullptr;schedule_async();
    });
    std::thread checkpoint([&]{
        // The checkpoint runs once both guest threads have yielded the kernel lock.
        {std::unique_lock progress(progress_mutex);assert(progress_cv.wait_for(progress,std::chrono::seconds(2),[&]{return guest_parked;}));}
        Lock lock(kmutex);assert(!running);save_lock();
        {std::lock_guard progress(progress_mutex);checkpoint_waiting=true;progress_cv.notify_all();}
        wiiu::fs_begin(0xC00B);wiiu::fs_end();save_unlock();checkpoint_done=true;
    });
    std::shared_ptr<Session> opened;
    {
        Lock lock(kmutex);memcpy(gptr(0x5200),"file",5);ipc_words(0x080201C2,{0,7,0,3,5,1,0,0,0x5200});
        IPC ipc(a.get(),0x1000,&lock);user.handle(ipc);assert(r32(0x1004)==0&&!archives.count(7)&&checkpoint_done);
        opened=get<Session>(r32(0x100c));assert(opened);auto file=std::dynamic_pointer_cast<FileSession>(opened->service);
        assert(file&&file->path==path.generic_string()&&file->held&&wiiu::save_file_size(*file->held)==13);
        close_handle(r32(0x100c));resume_thread(b.get());a->status=TStatus::WaitSleep;block_current(lock);a->status=TStatus::Dead;running=nullptr;
    }
    other.join();checkpoint.join();holder.join();test_open=false;threads.clear();tls_thread=nullptr;opened.reset();
    assert(wiiu::save_settle(path.generic_string(),true)&&cache.empty()&&cached_bytes==0);
}
static u32 call(kernel::FsUser &service,u32 header,std::initializer_list<u32> words) {
    ipc_words(header,words);kernel::Lock lock(kernel::kmutex);kernel::IPC ipc(nullptr,0x1000,&lock);service.handle(ipc);return r32(0x1004);
}
static void fault_call(kernel::FsUser &service,const fs::path &target,const std::string &prefix,u32 header,std::initializer_list<u32> words,bool rename_retry=false,const fs::path &destination={}) {
    const std::string latest("\0\xff latest save\r\n",16);auto file=cached(target,latest);std::weak_ptr<wiiu::SaveFile> weak=file;
    fail_write=true;assert(call(service,header,words)==kernel::ERR_IO);fail_write=false;
    assert(file->dirty && file->path==target.generic_string() && std::string((char*)file->data.data(),file->data.size())==latest);
    assert(cache.count(target.generic_string())==1 && cached_bytes==file->data.capacity() && get(target)=="previous bytes");
    if(rename_retry) { assert(call(service,header,words)==0);assert(!fs::exists(target) && get(destination)==latest); }
    else { assert(wiiu::save_settle(prefix,true));assert(get(target)==latest); }
    assert(cache.empty() && cached_bytes==0);file.reset();assert(weak.expired());
}
static u64 cache_capacity() { u64 total=0;for(const auto &entry:cache)total+=entry.second->data.capacity();assert(total==cached_bytes);return total; }
static void pending_writer_rename(const fs::path &folder) {
    using namespace kernel;
    const auto path=folder/"source",destination=folder/"dest";put(path,"old");FsUser user("fs:USER");archives[17]={4,folder.generic_string()};
    auto file=std::make_shared<FileSession>();file->path=path.generic_string();int fd=::open(file->path.c_str(),O_RDWR|O_BINARY);
    file->held=wiiu::save_file_open(file->path,fd);::close(fd);assert(file->held&&file->held->users==1);auto held=file->held;
    auto a=std::make_shared<Thread>(),b=std::make_shared<Thread>();a->id=1;b->id=2;a->priority=b->priority=0x30;
    threads={a,b};a->status=TStatus::Running;b->status=TStatus::Ready;b->ready_seq=1;running=a.get();tls_thread=a.get();
    progressed=holder_ready=false;
    std::thread holder([&]{save_lock();std::unique_lock lock(progress_mutex);holder_ready=true;progress_cv.notify_all();assert(progress_cv.wait_for(lock,std::chrono::seconds(2),[]{return progressed;}));lock.unlock();save_unlock();});
    {std::unique_lock lock(progress_mutex);assert(progress_cv.wait_for(lock,std::chrono::seconds(2),[]{return holder_ready;}));}
    std::thread other([&]{
        tls_thread=b.get();Lock lock(kmutex);b->cv.wait(lock,[&]{return running==b.get();});
        memset(gptr(0x1200),0,256);w32(0x1200,0x08050244);
        const u32 words[]={0,17,0,3,7,17,0,3,5,0,0x5500,0,0x5600};for(u32 i=0;i<13;++i)w32(0x1204+i*4,words[i]);
        IPC rename(b.get(),0x1200,&lock);user.handle(rename);assert(r32(0x1204)==ERR_IO&&cache.count(path.generic_string())==1&&get(path)=="old"&&!fs::exists(destination));
        {std::lock_guard progress(progress_mutex);progressed=true;}progress_cv.notify_all();
        b->status=TStatus::WaitSleep;block_current(lock);b->status=TStatus::Dead;resume_thread(a.get());running=nullptr;schedule_async();
    });
    const std::string latest="writer completed after failed rename";
    {
        Lock lock(kmutex);memcpy(gptr(0x5500),"source",7);memcpy(gptr(0x5600),"dest",5);memcpy(gptr(0x4000),latest.data(),latest.size());
        ipc_words(0x08030102,{0,0,(u32)latest.size(),0,((u32)latest.size()<<4)|0xA,0x4000});IPC write(a.get(),0x1000,&lock);file->handle(write);
        assert(r32(0x1004)==0&&r32(0x1008)==latest.size()&&held->dirty&&held->users==1&&std::string((char*)held->data.data(),held->data.size())==latest);
        resume_thread(b.get());a->status=TStatus::WaitSleep;block_current(lock);a->status=TStatus::Dead;running=nullptr;
    }
    other.join();holder.join();threads.clear();tls_thread=nullptr;file.reset();assert(held->users==0);
    assert(call(user,0x08050244,{0,17,0,3,7,17,0,3,5,0,0x5500,0,0x5600})==0);
    assert(!fs::exists(path)&&get(destination)==latest&&cache.empty()&&cached_bytes==0);
}
static void cache_budget(const fs::path &folder) {
    std::vector<std::shared_ptr<wiiu::SaveFile>> files;
    for(int i=0;i<3;++i) {
        const auto path=folder/std::to_string(i);put(path,std::string(CACHE_FILE_MAX,'x'));
        int fd=::open(path.generic_string().c_str(),O_RDWR|O_BINARY);auto file=wiiu::save_file_open(path.generic_string(),fd);::close(fd);assert(file);
        const char byte='y';assert(wiiu::save_file_write(*file,&byte,1,0)==1);files.push_back(file);
    }
    assert(cache_capacity()==CACHE_MAX);
    const auto extra=folder/"fourth";put(extra,std::string(CACHE_FILE_MAX,'z'));
    kernel::FsUser user("fs:USER");kernel::Archive archive{4,folder.generic_string()};kernel::LowPath path;path.type=3;path.data={'f','o','u','r','t','h',0};kernel::ObjPtr opened;
    assert(user.open_file(&archive,path,2,&opened)==0);auto session=std::dynamic_pointer_cast<kernel::Session>(opened);auto direct=std::dynamic_pointer_cast<kernel::FileSession>(session->service);
    assert(direct&&!direct->held&&direct->fd>=0&&cache_capacity()==CACHE_MAX);
    const char byte='Q';assert(wiiu::save_write(direct->fd,&byte,1,0)==1);assert(get(extra)[0]=='Q');direct.reset();session.reset();opened.reset();
    // Growth of an already-open cached file remains allowed; its real capacity stays accounted.
    wiiu::save_file_resize(*files[0],CACHE_FILE_MAX+1);assert(cache_capacity()>CACHE_MAX);
    for(auto &file:files)wiiu::save_file_close(*file);
    assert(wiiu::save_settle(folder.generic_string(),true));files.clear();assert(cache.empty()&&cached_bytes==0);
    std::vector<std::weak_ptr<wiiu::SaveFile>> released;
    for(int i=0;i<8;++i) {
        const auto name=folder/("shrink"+std::to_string(i));put(name,"tiny");int fd=::open(name.generic_string().c_str(),O_RDWR|O_BINARY);
        auto file=wiiu::save_file_open(name.generic_string(),fd);::close(fd);assert(file);
        wiiu::save_file_resize(*file,CACHE_FILE_MAX);const size_t capacity=file->data.capacity();wiiu::save_file_resize(*file,0);
        assert(file->data.empty()&&file->data.capacity()==capacity);wiiu::save_file_close(*file);
        assert(wiiu::save_settle(name.generic_string(),false));released.push_back(file);file.reset();
        assert(cache_capacity()<=CACHE_MAX&&cached_bytes>=capacity);
    }
    assert(released.front().expired());assert(wiiu::save_settle(folder.generic_string(),true));assert(cache.empty()&&cached_bytes==0);
    for(auto &file:released)assert(file.expired());
}
static std::shared_ptr<kernel::FileSession> open_session(const fs::path &folder,const std::string &name) {
    kernel::FsUser user("fs:USER");kernel::Archive archive{4,folder.generic_string()};kernel::LowPath path;path.type=3;path.data.assign(name.begin(),name.end());path.data.push_back(0);
    kernel::ObjPtr object;assert(user.open_file(&archive,path,2,&object)==0);
    auto session=std::dynamic_pointer_cast<kernel::Session>(object);assert(session);
    auto file=std::dynamic_pointer_cast<kernel::FileSession>(session->service);assert(file);return file;
}
static void single_file_ipc(kernel::FileSession &file,u32 header,std::initializer_list<u32> words) {
    using namespace kernel;
    auto thread=std::make_shared<Thread>();thread->id=1;thread->status=TStatus::Running;threads={thread};tls_thread=thread.get();running=thread.get();
    {Lock lock(kmutex);ipc_words(header,words);IPC ipc(thread.get(),0x1000,&lock);file.handle(ipc);assert(r32(0x1004)==0&&running==thread.get());running=nullptr;thread->status=TStatus::Dead;}
    threads.clear();tls_thread=nullptr;
}
static void session_write(kernel::FileSession &file,const std::string &bytes) {
    memcpy(gptr(0x4000),bytes.data(),bytes.size());single_file_ipc(file,0x08030102,{0,0,(u32)bytes.size(),0,((u32)bytes.size()<<4)|0xA,0x4000});assert(r32(0x1008)==bytes.size());
}
static std::string session_read(kernel::FileSession &file,u32 n) {
    single_file_ipc(file,0x080200C2,{0,0,n,(n<<4)|0xC,0x6000});return std::string((char*)gptr(0x6000),r32(0x1008));
}
static void read_notifies_gpu(const fs::path &path) {
    put(path, "texture");
    kernel::FileSession file;
    file.fd = ::open(path.generic_string().c_str(), O_RDONLY | O_BINARY);
    assert(file.fd >= 0);

    gpu_writes.clear();
    assert(session_read(file, 4) == "text");
    assert(session_read(file, 32) == "texture");
    const std::vector<std::pair<u32, u32>> expected{{0x6000, 4}, {0x6000, 7}};
    assert(gpu_writes == expected);

    single_file_ipc(file, 0x080200C2, {7, 0, 4, (4 << 4) | 0xC, 0x6000});
    assert(r32(0x1008) == 0);
    fail_read = true;
    assert(session_read(file, 4).empty());
    fail_read = false;
    assert(gpu_writes == expected);
}
static void direct_ownership(const fs::path &folder) {
    std::vector<std::shared_ptr<kernel::FileSession>> budget;
    for(int i=0;i<3;++i) {const std::string name="budget/"+std::to_string(i);put(folder/name,std::string(CACHE_FILE_MAX,'b'));budget.push_back(open_session(folder,name));assert(budget.back()->held);}
    assert(cache_capacity()==CACHE_MAX);const auto shared=folder/"shared";put(shared,"start");auto first=open_session(folder,"shared");assert(!first->held&&first->direct_save&&direct_users.at(shared.generic_string()).users==1);
    budget.clear();assert(wiiu::save_settle((folder/"budget").generic_string(),true)&&cached_bytes==0);
    auto second=open_session(folder,"shared");assert(!second->held&&direct_users.at(shared.generic_string()).users==2);
    session_write(*first,"one");assert(session_read(*second,3)=="one");session_write(*second,"two");assert(session_read(*first,3)=="two");assert(write_out_all()&&session_read(*first,3)=="two");
    assert(!wiiu::save_settle(shared.generic_string(),true));first.reset();assert(direct_users.at(shared.generic_string()).users==1);
    auto third=open_session(folder,"shared");assert(!third->held&&direct_users.at(shared.generic_string()).users==2);second.reset();third.reset();assert(direct_users.empty());
    auto cached_file=open_session(folder,"shared");assert(cached_file->held&&session_read(*cached_file,3)=="two");session_write(*cached_file,"new");cached_file.reset();assert(wiiu::save_settle(shared.generic_string(),true)&&get(shared).substr(0,3)=="new");
    for(int fault=0;fault<3;++fault) {
        const auto path=folder/("fallback"+std::to_string(fault));put(path,std::string(fault==0 ? CACHE_FILE_MAX+1 : 8,'f'));
        if(fault==1)fail_read=true;if(fault==2)fail_stat=true;auto a=open_session(folder,path.filename().generic_string());fail_read=fail_stat=false;
        assert(!a->held&&a->direct_save);if(fault==0)single_file_ipc(*a,0x08050080,{8,0});
        auto b=open_session(folder,path.filename().generic_string());assert(!b->held&&direct_users.at(path.generic_string()).users==2);
        session_write(*a,"abc");assert(session_read(*b,3)=="abc");session_write(*b,"xyz");assert(write_out_all()&&session_read(*a,3)=="xyz");
        a.reset();assert(direct_users.at(path.generic_string()).users==1);b.reset();assert(direct_users.empty());
        auto c=open_session(folder,path.filename().generic_string());assert(c->held&&session_read(*c,3)=="xyz");c.reset();assert(wiiu::save_settle(path.generic_string(),true));
    }
    const auto loading=folder/"loading";put(loading,"pending load");std::shared_ptr<kernel::FileSession> concurrent;
    while_reading=[&]{concurrent=open_session(folder,"loading");};auto original=open_session(folder,"loading");
    assert(!original->held&&!concurrent->held&&direct_users.at(loading.generic_string()).users==2);
    session_write(*original,"ABC");assert(session_read(*concurrent,3)=="ABC");concurrent.reset();original.reset();assert(direct_users.empty()&&cache.empty()&&cached_bytes==0);
    const auto transient=folder/"transient";put(transient,"old snapshot");
    after_reading=[&]{auto writer=open_session(folder,"transient");assert(!writer->held);const char newest[]="NEW";assert(wiiu::save_write(writer->fd,newest,3,0)==3);};
    auto pending=open_session(folder,"transient");assert(!pending->held&&session_read(*pending,3)=="NEW");pending.reset();assert(direct_users.empty());
    auto recovered=open_session(folder,"transient");assert(recovered->held&&session_read(*recovered,3)=="NEW");recovered.reset();assert(wiiu::save_settle(transient.generic_string(),true));
}
int main(int argc,char **argv) {
    assert(argc==2);_fmode=O_BINARY;const fs::path root=fs::absolute(argv[1]);assert(fs::is_directory(root)&&fs::is_empty(root));
    g_save_base=root.generic_string();g_game.program_id=0x0004000000001234;g_game.dir=root.generic_string();g_mem=(u8*)calloc(1,0x200000);assert(g_mem);
    scheduler_io(root/"write",Delay::Write);scheduler_io(root/"size",Delay::Size);scheduler_io(root/"resize",Delay::Resize);
    scheduler_open(root/"opening/file");
    pending_writer_rename(root/"pending");
    kernel::FsUser user("fs:USER");const fs::path archive=root/"archive";fs::create_directory(archive);kernel::archives[1]={4,archive.generic_string()};
    memcpy(gptr(0x5000),"victim",7);memcpy(gptr(0x5100),"dest",5);
    const auto victim=archive/"victim",dest=archive/"dest";
    auto delete_victim = [&] {
        kernel::Lock lock(kernel::kmutex);
        ipc_words(0x08040142,{0,1,0,3,7,0,0x5000});
        kernel::IPC ipc(nullptr,0x1000,&lock);
        user.handle(ipc);
        return r32(0x1004);
    };
    assert(delete_victim() == kernel::ERR_FILE_NOT_FOUND);
    put(victim/"keep","directory data");
    assert(delete_victim() == kernel::fs_err(250, 9));
    assert(get(victim/"keep") == "directory data");
    fs::remove(victim/"keep");
    assert(delete_victim() == kernel::fs_err(250, 9) && fs::is_directory(victim));
    fs::remove(victim);
    put(victim,"file data");
    int lock_fd = -1;
    assert(_sopen_s(&lock_fd, victim.string().c_str(), O_RDONLY | O_BINARY, _SH_DENYRW, 0) == 0);
    assert(delete_victim() == kernel::ERR_IO);
    ::close(lock_fd);
    assert(get(victim) == "file data");
    assert(delete_victim() == 0 && !fs::exists(victim));
    fault_call(user,victim,victim.generic_string(),0x08040142,{0,1,0,3,7,0,0x5000});
    fs::remove(victim);
    fault_call(user,victim,victim.generic_string(),0x08050244,{0,1,0,3,7,1,0,3,5,0,0x5000,0,0x5100},true,dest);
    put(victim,"source unchanged");
    fault_call(user,dest,dest.generic_string(),0x08050244,{0,1,0,3,7,1,0,3,5,0,0x5000,0,0x5100});assert(get(victim)=="source unchanged");
    fs::remove(victim);fs::remove(dest);fs::create_directory(victim);
    for(u32 command:{0x0806u,0x0807u}) fault_call(user,victim/"file",victim.generic_string(),(command<<16)|0x142,{0,1,0,3,7,0,0x5000});
    fault_call(user,victim/"file",victim.generic_string(),0x080A0244,{0,1,0,3,7,1,0,3,5,0,0x5000,0,0x5100});assert(fs::exists(victim)&&!fs::exists(dest));
    fs::create_directory(dest);
    fault_call(user,dest/"file",dest.generic_string(),0x080A0244,{0,1,0,3,7,1,0,3,5,0,0x5000,0,0x5100});assert(fs::exists(victim)&&fs::exists(dest));
    fault_call(user,victim/"file",victim.generic_string(),0x080B0102,{1,0,3,7,0,0x5000});
    const fs::path save=kernel::save_root()+"/savedata";
    for(u32 command:{0x080Fu,0x084Cu}) fault_call(user,save/"file",save.generic_string(),command<<16,{0,0,0,0,0,0,0,0,0,0});
    for(u32 command:{0x0835u,0x0852u}) {
        const fs::path ext=kernel::save_root()+std::string(command==0x0835?"/extdata/0000000000000042":"/extdata/1234567800000042");
        put(ext.generic_string()+".format","format kept");fault_call(user,ext/"file",ext.generic_string(),command<<16,{1,0x42,0x12345678,0});assert(get(ext.generic_string()+".format")=="format kept");
    }
    for(int kind=0;kind<5;++kind) {
        const auto path=root/("retry"+std::to_string(kind));auto file=cached(path,"new bytes");
        if(kind==0)fail_write=true;if(kind==1)partial_write=1;if(kind==2)fail_stat=true;if(kind==3)fail_truncate=true;if(kind==4)fail_close=true;
        assert(!wiiu::save_settle(path.generic_string(),true));assert(file->dirty && cache.count(path.generic_string())==1);
        fail_write=fail_stat=fail_truncate=fail_close=false;assert(wiiu::save_settle(path.generic_string(),true));assert(get(path)=="new bytes" && cache.empty() && cached_bytes==0);
    }
    {
        const auto path=root/"concurrent";auto file=cached(path,"first");
        while_writing=[&]{wiiu::save_file_resize(*file,0);const char newest[]="changed while writing";wiiu::save_file_write(*file,newest,sizeof(newest)-1,0);};
        assert(!wiiu::save_settle(path.generic_string(),false));assert(file->dirty && cache.count(path.generic_string())==1);
        assert(wiiu::save_settle(path.generic_string(),true));assert(get(path)=="changed while writing" && cache.empty() && cached_bytes==0);
    }
    for(int i=0;i<128;++i) {
        const auto path=root/"cycles";const std::string bytes(1024+(i%17)*4096,(char)i);auto file=cached(path,bytes);std::weak_ptr<wiiu::SaveFile> weak=file;
        assert(file->users==0 && cache.size()==1 && cached_bytes==file->data.capacity());
        if(i%3==0) {fail_write=true;assert(!wiiu::save_settle(path.generic_string(),true));fail_write=false;assert(file->dirty && cached_bytes==file->data.capacity());}
        const int fd=::open(path.generic_string().c_str(),O_RDWR|O_BINARY);auto again=wiiu::save_file_open(path.generic_string(),fd);::close(fd);
        assert(again==file && file->users==1);wiiu::save_file_close(*again);again.reset();
        assert(wiiu::save_settle(path.generic_string(),true));assert(get(path)==bytes && cache.empty() && cached_bytes==0);file.reset();assert(weak.expired());
    }
    cache_budget(root/"budget");
    read_notifies_gpu(root/"gpu_read");
    direct_ownership(root/"direct");
    free(g_mem);puts("FS save: real IPC/scheduler Open/Write/GetSize/SetSize, checkpoint/archive-close contention, all settle callers, pending-writer rename protection, failure/retry/concurrent updates,128 ownership cycles, capacity/admission/shrink budgets and direct/cache lifetime isolation passed");
}
