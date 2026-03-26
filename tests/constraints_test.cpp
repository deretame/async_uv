#include <memory>
#include <string>
#include <type_traits>

#include "async_uv/async_uv.h"

namespace {

struct CopyableMessage {
    std::string payload;
};

struct MoveOnlyLocalMessage {
    std::string payload;
    MoveOnlyLocalMessage() = default;
    explicit MoveOnlyLocalMessage(std::string payload_in) : payload(std::move(payload_in)) {}
    MoveOnlyLocalMessage(const MoveOnlyLocalMessage &) = delete;
    MoveOnlyLocalMessage &operator=(const MoveOnlyLocalMessage &) = delete;
    MoveOnlyLocalMessage(MoveOnlyLocalMessage &&) noexcept = default;
    MoveOnlyLocalMessage &operator=(MoveOnlyLocalMessage &&) noexcept = default;
};

static_assert(async_uv::MovableToChannel<MoveOnlyLocalMessage>);
static_assert(async_uv::MoveOnlyMessage<MoveOnlyLocalMessage>);
static_assert(async_uv::MoveOnlyMessage<std::unique_ptr<int>>);

static_assert(async_uv::MovableToChannel<int>);
static_assert(!async_uv::MoveOnlyMessage<int>);
static_assert(!async_uv::MoveOnlyMessage<std::string>);
static_assert(!async_uv::MoveOnlyMessage<std::shared_ptr<int>>);
static_assert(!async_uv::MoveOnlyMessage<CopyableMessage>);

static_assert(std::is_default_constructible_v<async_uv::Mailbox<MoveOnlyLocalMessage>>);

} // namespace

int main() {
    return 0;
}
