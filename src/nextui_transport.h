#ifndef NEXTUI_TRANSPORT_INTERNAL_H
#define NEXTUI_TRANSPORT_INTERNAL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Connect to a Unix stream socket, retrying transient failures for
 * `timeout_ms`. Returns a blocking fd or -1 (errno set). */
int nx_connect_unix(const char* path, int timeout_ms);

int nx_write_all(int descriptor, const char* data, size_t length);

/* Read one '\n'-terminated line into out (newline stripped). Returns:
 * 1 line, 0 clean EOF, -1 error, -2 line too long, -3 timeout. */
int nx_read_line(int descriptor, char* out, size_t out_size, int timeout_ms);

int nx_set_nonblocking(int descriptor, int enabled);
int nx_set_socket_timeout(int descriptor, int timeout_ms);
int nx_set_cloexec(int descriptor);

unsigned long long nx_now_ms(void);
void nx_sleep_ms(int milliseconds);

#ifdef __cplusplus
}
#endif

#endif
