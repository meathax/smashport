// Only the optional compositor wait runs on this worker. The owner keeps all
// simulation, input, sound, framebuffer, and GDI drawing on its original thread.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <new>

namespace stv::host {
class CompositorWait {
public:
    using Flush = HRESULT(WINAPI*)();
    ~CompositorWait() { shutdown(); }
    CompositorWait() = default;
    CompositorWait(const CompositorWait&) = delete;
    CompositorWait& operator=(const CompositorWait&) = delete;

    bool start(Flush flush) {
        if (state_) return thread_ != nullptr;
        auto s = std::make_shared<State>();
        state_ = s;
        s->flush = flush;
        LARGE_INTEGER hz{};
        QueryPerformanceFrequency(&hz);
        s->frequency = std::uint64_t(hz.QuadPart);
        s->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        s->request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        s->ready = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        if (!flush || !s->frequency || !s->stop || !s->request || !s->ready) {
            s->errors.store(1);
            return false;
        }
        auto* context = new (std::nothrow) std::shared_ptr<State>(s);
        if (!context) { s->errors.store(1); return false; }
        thread_ = CreateThread(nullptr, 0, run, context, 0, nullptr);
        if (!thread_) { delete context; s->errors.store(1); return false; }
        return true;
    }
    bool ready() const noexcept {
        return state_ && state_->ready && WaitForSingleObject(state_->ready, 0) == WAIT_OBJECT_0;
    }
    // At most one wait is outstanding. Called after a completed GDI submission.
    bool request() noexcept {
        if (!thread_ || errors() || !ready()) return false;
        ResetEvent(state_->ready);
        if (SetEvent(state_->request)) return true;
        state_->errors.fetch_add(1);
        SetEvent(state_->ready);
        return false;
    }
    HANDLE pending_event() const noexcept {
        return thread_ && !ready() ? state_->ready : nullptr;
    }
    std::uint64_t completed() const noexcept { return state_ ? state_->completed.load() : 0; }
    std::uint64_t errors() const noexcept { return state_ ? state_->errors.load() : 0; }
    double maximum_ms() const noexcept {
        return state_ && state_->frequency ?
            1000.0 * double(state_->maximum_ticks.load()) / double(state_->frequency) : 0.0;
    }
    void shutdown(DWORD timeout_ms = 1000) noexcept {
        if (state_ && state_->stop) SetEvent(state_->stop);
        if (thread_) {
            // A stalled OS compositor must not hold shutdown indefinitely.
            // The worker retains shared state and its event handles until it
            // returns; it never references the window or any game object.
            WaitForSingleObject(thread_, timeout_ms);
            CloseHandle(thread_);
            thread_ = nullptr;
        }
        state_.reset();
    }

private:
    struct State {
        HANDLE stop{}, request{}, ready{};
        Flush flush{};
        std::uint64_t frequency{};
        std::atomic<std::uint64_t> completed{}, errors{}, maximum_ticks{};
        ~State() {
            if (ready) CloseHandle(ready);
            if (request) CloseHandle(request);
            if (stop) CloseHandle(stop);
        }
    };
    static DWORD WINAPI run(void* argument) noexcept {
        std::unique_ptr<std::shared_ptr<State>> context(static_cast<std::shared_ptr<State>*>(argument));
        const auto s = std::move(*context);
        context.reset();
        const HANDLE handles[] = {s->stop, s->request};
        while (WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
            LARGE_INTEGER before{}, after{};
            QueryPerformanceCounter(&before);
            const HRESULT result = s->flush();
            QueryPerformanceCounter(&after);
            const auto ticks = std::uint64_t(after.QuadPart - before.QuadPart);
            // This worker is the only writer of maximum_ticks.
            if (ticks > s->maximum_ticks.load()) s->maximum_ticks.store(ticks);
            s->completed.fetch_add(1);
            if (FAILED(result)) s->errors.fetch_add(1);
            SetEvent(s->ready);
        }
        return 0;
    }
    std::shared_ptr<State> state_;
    HANDLE thread_{};
};
} // namespace stv::host
