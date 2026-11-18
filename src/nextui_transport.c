#define _POSIX_C_SOURCE 200809L

#include "nextui_transport.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define NX_CONNECT_RETRY_MS 50

unsigned long long nx_now_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (unsigned long long)now.tv_sec * 1000ULL + (unsigned long long)now.tv_nsec / 1000000ULL;
}

void nx_sleep_ms(int milliseconds)
{
    struct timespec request;

    if (milliseconds <= 0) return;
    request.tv_sec = milliseconds / 1000;
    request.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    while (nanosleep(&request, &request) != 0 && errno == EINTR) continue;
}

int nx_set_cloexec(int descriptor)
{
    int flags = fcntl(descriptor, F_GETFD, 0);
    if (flags < 0) return 0;
    return fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) == 0;
}

int nx_set_socket_timeout(int descriptor, int timeout_ms)
{
    struct timeval timeout;

    if (timeout_ms <= 0) timeout_ms = 2000;
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    if (setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) != 0) return 0;
    if (setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout) != 0) return 0;
    return 1;
}

int nx_set_nonblocking(int descriptor, int enabled)
{
    int flags = fcntl(descriptor, F_GETFL, 0);

    if (flags < 0) return 0;
    if (enabled)
        flags |= O_NONBLOCK;
    else
        flags &= ~O_NONBLOCK;
    return fcntl(descriptor, F_SETFL, flags) == 0;
}

int nx_connect_unix(const char* path, int timeout_ms)
{
    struct sockaddr_un address;
    unsigned long long deadline;
    unsigned long long now;

    if (path == NULL || path[0] == '\0') return -1;
    if (strlen(path) >= sizeof address.sun_path)
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (timeout_ms <= 0) timeout_ms = 1;
    deadline = nx_now_ms() + (unsigned long long)timeout_ms;

    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1);

    for (;;)
    {
        int descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
        int saved;

        if (descriptor < 0) return -1;
        (void)nx_set_cloexec(descriptor);

        if (connect(descriptor, (struct sockaddr*)&address, sizeof address) == 0) return descriptor;

        saved = errno;
        close(descriptor);
        if (saved != ENOENT && saved != ECONNREFUSED && saved != ENOTSOCK && saved != EINTR)
        {
            errno = saved;
            return -1;
        }

        now = nx_now_ms();
        if (now >= deadline)
        {
            errno = saved;
            return -1;
        }
        nx_sleep_ms(NX_CONNECT_RETRY_MS);
    }
}

int nx_write_all(int descriptor, const char* data, size_t length)
{
    size_t written = 0;

    while (written < length)
    {
#ifdef MSG_NOSIGNAL
        ssize_t count = send(descriptor, data + written, length - written, MSG_NOSIGNAL);
#else
        ssize_t count = send(descriptor, data + written, length - written, 0);
#endif
        if (count < 0)
        {
            if (errno == EINTR) continue;
            return 0;
        }
        if (count == 0) return 0;
        written += (size_t)count;
    }
    return 1;
}

int nx_read_line(int descriptor, char* out, size_t out_size, int timeout_ms)
{
    size_t total = 0;
    unsigned long long deadline = nx_now_ms() + (unsigned long long)(timeout_ms > 0 ? timeout_ms : 2000);

    if (out == NULL || out_size == 0) return -1;
    out[0] = '\0';

    for (;;)
    {
        char ch;
        ssize_t count;
        struct pollfd wait;
        int ready;

        if (nx_now_ms() > deadline) return -3;

        wait.fd = descriptor;
        wait.events = POLLIN;
        wait.revents = 0;
        ready = poll(&wait, 1, 100);
        if (ready < 0)
        {
            if (errno == EINTR) continue;
            return -1;
        }
        if (ready == 0) continue;

        count = recv(descriptor, &ch, 1, 0);
        if (count == 0) break;
        if (count < 0)
        {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return -1;
        }
        if (ch == '\n')
        {
            out[total] = '\0';
            return 1;
        }
        if (ch == '\r') continue;
        if (total + 1 >= out_size) return -2;
        out[total++] = ch;
    }

    out[total] = '\0';
    return total > 0 ? 1 : 0;
}
