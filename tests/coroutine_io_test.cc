#include "ioscheduler.h"

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

int main()
{
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
    {
        return 1;
    }

    bool received = false;
    {
        sylar::IOManager iom(1, true, "io_test");

        iom.scheduleLock([&]() {
            char value = 0;
            received = recv(sockets[0], &value, 1, 0) == 1 && value == 'K';
        });

        iom.scheduleLock([&]() {
            const char value = 'K';
            send(sockets[1], &value, 1, 0);
        });
    }

    close(sockets[0]);
    close(sockets[1]);
    if (!received)
    {
        return 1;
    }

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
    {
        return 1;
    }
    bool timed_out = false;
    {
        sylar::IOManager iom(1, true, "timeout_test");
        iom.scheduleLock([&]() {
            timeval timeout{0, 50 * 1000};
            if (setsockopt(sockets[0], SOL_SOCKET, SO_RCVTIMEO,
                           &timeout, sizeof(timeout)) != 0)
            {
                return;
            }
            char value = 0;
            timed_out = recv(sockets[0], &value, 1, 0) == -1 && errno == ETIMEDOUT;
        });
    }
    close(sockets[0]);
    close(sockets[1]);
    return timed_out ? 0 : 1;
}
