// A blocked compositor must not hold the simulation/audio owner thread.
#include "stv/host/compositor_wait.hpp"
#include "stv/host/frame_clock.hpp"
#include <cstdio>

namespace {
HANDLE entered{}, release_flush{};
std::atomic<unsigned> calls{};
std::atomic<bool> fail_flush{};
int checks{}, failures{};
void check(bool condition, const char* name) {
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL %s\n", name); }
}
HRESULT WINAPI fake_flush() {
    calls.fetch_add(1);
    SetEvent(entered);
    WaitForSingleObject(release_flush, INFINITE);
    return fail_flush.load() ? E_FAIL : S_OK;
}
bool signaled(HANDLE event) { return event && WaitForSingleObject(event, 5000) == WAIT_OBJECT_0; }
}
int main() {
    entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    release_flush = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    check(entered && release_flush, "fixture events created");
    if (!entered || !release_flush) return 1;
    DWORD initial_handles{};
    GetProcessHandleCount(GetCurrentProcess(), &initial_handles);
    for (int cycle = 0; cycle < 12; ++cycle) {
        ResetEvent(entered);
        ResetEvent(release_flush);
        calls.store(0);
        fail_flush.store(false);
        stv::host::CompositorWait wait;
        check(wait.start(fake_flush) && wait.ready(), "worker starts ready");
        check(wait.request(), "first request returns before compositor completes");
        check(signaled(entered), "worker entered fake compositor");
        check(!wait.ready() && wait.pending_event(), "pending compositor exposes wake event");
        check(!wait.request() && calls.load() == 1, "pending request is not duplicated");
        stv::host::FrameClock clock;
        check(clock.configure(10000000), "fixed clock configured");
        clock.reset(0);
        const auto deadline = clock.deadline();
        check(clock.due(deadline), "simulation remains callable during compositor stall");
        clock.consume();
        check(clock.completed() == 1 && wait.completed() == 0, "simulation advances independently of compositor");
        HANDLE ready = wait.pending_event();
        SetEvent(release_flush);
        check(signaled(ready) && wait.ready(), "completion signals owner without polling");
        check(wait.completed() == 1 && wait.errors() == 0, "successful completion counted exactly once");
        check(wait.maximum_ms() >= 0 && !wait.pending_event(), "completed wait no longer wakes idle owner");

        ResetEvent(entered);
        fail_flush.store(true);
        check(wait.request(), "second compositor request accepted");
        // Capture the event before allowing the worker to finish is not required:
        // ready() is authoritative if completion races this observation.
        check(signaled(entered), "failing compositor called");
        ready = wait.pending_event();
        if (ready) check(signaled(ready), "failing compositor releases pending owner");
        check(wait.ready() && wait.completed() == 2 && wait.errors() == 1, "HRESULT failure retained");
        check(!wait.request(), "failed compositor rejects more work");
        wait.shutdown();
        DWORD handles{};
        GetProcessHandleCount(GetCurrentProcess(), &handles);
        check(handles == initial_handles, "normal shutdown releases worker and event handles");
    }
    // Shutdown remains bounded if the compositor never returns. The detached
    // worker keeps the event state alive until this fixture finally releases it.
    {
        ResetEvent(entered);
        ResetEvent(release_flush);
        fail_flush.store(false);
        stv::host::CompositorWait wait;
        check(wait.start(fake_flush) && wait.request() && signaled(entered), "blocked shutdown fixture entered");
        wait.shutdown(0);
        SetEvent(release_flush);
        DWORD handles{};
        const ULONGLONG limit = GetTickCount64() + 5000;
        do {
            GetProcessHandleCount(GetCurrentProcess(), &handles);
            if (handles == initial_handles) break;
            Sleep(1);
        } while (GetTickCount64() < limit);
        check(handles == initial_handles, "late compositor return releases retained event state");
    }
    CloseHandle(release_flush);
    CloseHandle(entered);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
