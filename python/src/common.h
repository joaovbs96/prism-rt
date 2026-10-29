#pragma once

// Shared by the Python extension modules prismrt._prism and
// prismrt._prism_usd (docs/python.md, docs/sdk_design.md section 6.6): the
// holders of the bound renderer objects, GIL helpers and the error mapping.

#include <nanobind/nanobind.h>

#include <prism/renderer.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace prism::python {

namespace nb = nanobind;

// False once the interpreter is gone or finalizing: C++ threads must not
// take the GIL then (Python 3.11 would block them forever).
inline bool PythonAlive()
{
#if PY_VERSION_HEX >= 0x030D0000
    return Py_IsInitialized() && !Py_IsFinalizing();
#else
    return Py_IsInitialized() && !_Py_IsFinalizing();
#endif
}

// A Python object that C++ may copy and drop on any thread: the last
// reference takes the GIL to release it (and leaks it during finalization).
using SharedObject = std::shared_ptr<nb::object>;
inline SharedObject Share(nb::object object)
{
    return SharedObject(new nb::object(std::move(object)), [](nb::object* held) {
        if (!PythonAlive()) {
            held->release();
            delete held;
            return;
        }
        nb::gil_scoped_acquire gil;
        delete held;
    });
}

// Runs `reset` without the GIL when this thread holds it: destroying a
// session joins its thread, which may wait for the GIL in a callback.
template <class F>
void WithoutGil(F&& reset)
{
    if (PythonAlive() && PyGILState_Check()) {
        nb::gil_scoped_release release;
        reset();
    } else {
        reset();
    }
}

// The bound objects hold the C++ handles, so that the last Python reference
// destroys them without the GIL.
struct PyRenderer {
    std::optional<prism::Renderer> renderer;
    SharedObject log;  // the Python log callable, if any
    ~PyRenderer() { WithoutGil([&] { renderer.reset(); }); }
    prism::Renderer& get() { return *renderer; }
};

struct PyScene {
    std::optional<prism::Scene> scene;
    ~PyScene() { WithoutGil([&] { scene.reset(); }); }
    prism::Scene& get() { return *scene; }
};

// A Python exception raised in a callback on a C++ thread, kept for the call
// (or wait()) that reports it.
struct CallbackError {
    std::mutex mutex;
    std::exception_ptr error;
    void set(std::exception_ptr e)
    {
        std::lock_guard lock(mutex);
        if (!error) error = std::move(e);
    }
    std::exception_ptr take()
    {
        std::lock_guard lock(mutex);
        return std::exchange(error, nullptr);
    }
};

struct PySession {
    std::optional<prism::Session> session;
    std::shared_ptr<CallbackError> progressiveError = std::make_shared<CallbackError>();
    ~PySession()
    {
        WithoutGil([&] {
            if (session) {
                session->cancel();
                session.reset();
            }
        });
    }
    prism::Session& get() { return *session; }
};

// A Python progress callable as a prism::ProgressCallback: it takes the GIL,
// passes a ProgressInfo, and reads a false result as a cancel. An exception
// cancels too and is kept in `error`.
inline prism::ProgressCallback ProgressFromPython(nb::object callable, std::shared_ptr<CallbackError> error,
                                                  std::shared_ptr<std::atomic<bool>> stop = {})
{
    if (callable.is_none() && !stop) return {};
    SharedObject held = callable.is_none() ? SharedObject() : Share(std::move(callable));
    return [held, error, stop](prism::ProgressInfo const& info) -> bool {
        if (stop && stop->load()) return false;
        if (!held) return true;
        if (!PythonAlive()) return false;
        nb::gil_scoped_acquire gil;
        try {
            nb::object result = (*held)(nb::cast(info, nb::rv_policy::copy));
            return result.is_none() || nb::cast<bool>(result);
        } catch (nb::python_error& e) {
            error->set(std::make_exception_ptr(std::move(e)));
            return false;
        } catch (...) {
            error->set(std::current_exception());
            return false;
        }
    };
}

// Runs `work` on a C++ thread while this (Python) thread waits without the
// GIL in 100 ms slices and checks for signals between them: on
// KeyboardInterrupt (or another signal handler's exception) it calls
// `cancel`, joins the work and raises. Called with the GIL held.
template <class R>
R RunInterruptible(std::function<R()> work, std::function<void()> const& cancel)
{
    std::packaged_task<R()> task(std::move(work));
    std::future<R> future = task.get_future();
    std::optional<nb::python_error> interrupt;
    {
        nb::gil_scoped_release release;
        std::thread worker(std::move(task));
        while (future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
            nb::gil_scoped_acquire gil;
            if (PyErr_CheckSignals() != 0) {
                interrupt.emplace();
                break;
            }
        }
        if (interrupt && cancel) cancel();
        worker.join();
    }
    if (interrupt) {
        try {
            future.get();
        } catch (...) {
        }
        throw std::move(*interrupt);
    }
    return future.get();
}

} // namespace prism::python
