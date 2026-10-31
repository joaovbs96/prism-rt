#pragma once

#include <atomic>
#include <mutex>

namespace prism::hydra {

// Serialises the delegate's background preparation worker (SceneBridge),
// which may restart the render thread, with host calls that stop the render
// thread and change what it reads (scene notices, settings, render buffer
// allocation and removal, Stop): the worker cannot restart the render thread
// in the middle of such a call. Recursive, as a host's notices can reach the
// delegate from inside its own calls. One per delegate, shared by its
// bridge and render buffers.
class RenderDrive {
public:
    // Held by a host call; the worker yields to waiting host calls.
    class HostLock {
    public:
        explicit HostLock(RenderDrive *drive) : _drive(drive)
        {
            if (!_drive) return;
            _drive->_hostWaiting.fetch_add(1);
            _drive->_mutex.lock();
            _drive->_hostWaiting.fetch_sub(1);
        }
        ~HostLock() { if (_drive) _drive->_mutex.unlock(); }
        HostLock(HostLock const &) = delete;
        HostLock &operator=(HostLock const &) = delete;
    private:
        RenderDrive *_drive;
    };

    std::unique_lock<std::recursive_mutex> WorkerLock() { return std::unique_lock(_mutex); }
    bool HostWaiting() const { return _hostWaiting.load() > 0; }

private:
    std::recursive_mutex _mutex;
    std::atomic<int> _hostWaiting{0};
};

} // namespace prism::hydra
