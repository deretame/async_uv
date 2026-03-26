#include "async_uv/async_uv.h"

int main() {
    async_uv::Mailbox<int> mailbox;
    (void)mailbox;
    return 0;
}
