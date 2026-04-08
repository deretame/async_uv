#include <cassert>
#include <mutex>
#include <vector>

#include "flux/flux.h"

namespace {

flux::Task<void> run_trace_checks() {
    std::mutex mutex;
    std::vector<flux::TraceEvent> events;

    flux::set_trace_hook([&](const flux::TraceEvent &event) {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back(event);
    });

    flux::emit_trace_event({"test", "event1", 1, 10});
    flux::emit_trace_event({"test", "event2", 2, 20});
    flux::reset_trace_hook();
    flux::emit_trace_event({"test", "event3", 3, 30});

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
    flux::Runtime runtime;
    runtime.block_on(run_trace_checks());
    return 0;
}

