#include <string>

#include "async_uv/async_uv.h"

int main() {
    async_uv::Mailbox<std::string> mailbox;
    (void)mailbox;
    return 0;
}
