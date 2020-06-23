#pragma once

#include <atomic>
#include <memory>
#include <vector>

namespace dxcw
{
/**
 * Watch files on disk for changes
 *
 * Usage:
 *
 * auto flag = FileWatch::watchFile(path);
 *
 * // time passes...
 *
 * if (flag->isChanged())
 * {
 *      // reload resource..
 *      flag->clear();
 * }
 */
class FileWatch
{
public:
    struct Flag;
    using SharedFlag = std::shared_ptr<Flag>;
    struct Flag
    {
    public:
        ~Flag();

        bool isChanged() const { return mChanged.load(std::memory_order_relaxed); }
        void clear() { mChanged.store(false, std::memory_order_relaxed); }

    private:
        std::atomic_bool mChanged = {false};
        friend class FileWatch;
    };

    /// Start watching a file
    /// forceUnique: Force creation of a new flag even if the file already has flags
    static SharedFlag watchFile(const char* filename, bool forceUnique = true);

private:
    struct Monitor;
    static std::vector<std::unique_ptr<Monitor>> sMonitors;
    static void onFlagDestruction(Flag* flag);
    FileWatch() = delete;
};
}
