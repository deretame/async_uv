#include <cassert>
#include <mutex>
#include <vector>

#include "async_uv/async_uv.h"

namespace {

async_uv::Task<void> run_trace_checks() {
    std::mutex mutex;
    std::vector<async_uv::TraceEvent> events;

    async_uv::set_trace_hook([&](const async_uv::TraceEvent &event) {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back(event);
    });

    async_uv::emit_trace_event({"test", "event1", 1, 10});
    async_uv::emit_trace_event({"test", "event2", 2, 20});
    async_uv::reset_trace_hook();
    async_uv::emit_trace_event({"test", "event3", 3, 30});

    {
        std::lock_guard<std::mutex> lock(mutex);
        assert(events.size() == 2);
        assert(events[0].name == std::string_view("event1"));
        assert(events[1].name == std::string_view("event2"));
    }

    co_return;
}

} // namespace

int main() {
    async_uv::Runtime runtime;
    runtime.block_on(run_trace_checks());
    return 0;
}

