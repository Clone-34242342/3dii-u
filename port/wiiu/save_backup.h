#pragma once
#include <mutex>
#include <string>

namespace wiiu::save_backup {
// Run export/staging on the storage thread with guest save writes paused. Restore runs
// before guest files are opened; only Unsafe requires stopping startup.
enum class Restore { None, Restored, Deferred, Unsafe };
bool export_to_card(std::string &message);
bool stage_restore(std::string &message);
Restore take_staged_restore(std::string &message);

enum class Action { None, Export, StageRestore };
// Input queues one action; the card thread owns it through completion. GPU reads a copy.
class Requests {
    std::mutex mutex;
    Action pending = Action::None;
    bool busy = false;
    std::string message;
public:
    struct View { bool busy; std::string message; };
    bool request(Action action) {
        std::lock_guard<std::mutex> lock(mutex);
        if (busy || action == Action::None) return false;
        pending = action;
        busy = true;
        message = action == Action::Export ? "Backing up to the SD card..." : "Staging the newest backup for next start...";
        return true;
    }
    Action take() {
        std::lock_guard<std::mutex> lock(mutex);
        const Action action = pending;
        pending = Action::None;
        return action;
    }
    void complete(const std::string &result) {
        std::lock_guard<std::mutex> lock(mutex);
        message = result;
        busy = false;
    }
    View view() {
        std::lock_guard<std::mutex> lock(mutex);
        return {busy, message};
    }
};
extern Requests requests;
}  // namespace wiiu::save_backup
