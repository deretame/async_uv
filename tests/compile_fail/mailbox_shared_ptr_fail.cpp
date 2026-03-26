#include <memory>

#include "async_uv/async_uv.h"

int main() {
    async_uv::Mailbox<std::shared_ptr<int>> mailbox;
    (void)mailbox;
    return 0;
}
