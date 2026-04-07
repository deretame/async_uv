#include <cassert>
#include <chrono>
#include <string>

#include "async_uv/async_uv.h"

namespace {

async_uv::Task<void> run_watch_checks() {
    using namespace std::chrono_literals;

    const auto dir = co_await async_uv::Fs::create_temporary_directory("async_uv_watch_XXXXXX");
    const auto file = async_uv::path::join(dir, "watch.txt");
    co_await async_uv::Fs::write_file(file, "v1");

    auto watcher = co_await async_uv::FsWatcher::watch(file);
    assert(watcher.valid());

    co_await async_uv::sleep_for(80ms);
    co_await async_uv::Fs::append_file(file, "-changed");
    auto event = co_await watcher.next_for(2s);

    assert(event.has_value());
    assert(event->ok());
    assert(event->path == file);
    assert(event->previous.has_value());
    assert(event->current.has_value());
    assert(event->previous->size < event->current->size);

    co_await watcher.stop();
    (void)co_await async_uv::Fs::remove_all(dir);
}

} // namespace

int main() {
    async_uv::Runtime runtime(async_uv::Runtime::build().io_threads(2).blocking_threads(2));
    runtime.block_on(run_watch_checks());
    return 0;
}
