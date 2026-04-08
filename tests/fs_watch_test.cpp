#include <cassert>
#include <chrono>
#include <string>

#include "flux/flux.h"

namespace {

flux::Task<void> run_watch_checks() {
    using namespace std::chrono_literals;

    const auto dir = co_await flux::Fs::create_temporary_directory("flux_watch_XXXXXX");
    const auto file = flux::path::join(dir, "watch.txt");
    co_await flux::Fs::write_file(file, "v1");

    auto watcher = co_await flux::FsWatcher::watch(file);
    assert(watcher.valid());

    co_await flux::sleep_for(80ms);
    co_await flux::Fs::append_file(file, "-changed");
    auto event = co_await watcher.next_for(2s);

    assert(event.has_value());
    assert(event->ok());
    assert(event->path == file);
    assert(event->previous.has_value());
    assert(event->current.has_value());
    assert(event->previous->size < event->current->size);

    co_await watcher.stop();
    (void)co_await flux::Fs::remove_all(dir);
}

} // namespace

int main() {
    flux::Runtime runtime(flux::Runtime::build().io_threads(2).blocking_threads(2));
    runtime.block_on(run_watch_checks());
    return 0;
}
