/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NQ666_SOCKET_COMPAT_H
#define NQ666_SOCKET_COMPAT_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef _WIN32

#ifndef FD_SETSIZE
#define FD_SETSIZE 512
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

typedef SOCKET nq_socket_t;
typedef int nq_socklen_t;

#define NQ_INVALID_SOCKET INVALID_SOCKET

static inline int nq_socket_startup(void)
{
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data);
}

static inline void nq_socket_cleanup(void)
{
    (void)WSACleanup();
}

static inline int nq_close_socket(nq_socket_t socket_fd)
{
    return closesocket(socket_fd);
}

static inline bool nq_set_nonblocking(nq_socket_t socket_fd)
{
    u_long enabled = 1;
    return ioctlsocket(socket_fd, (long)FIONBIO, &enabled) == 0;
}

static inline int nq_socket_last_error(void)
{
    return WSAGetLastError();
}

static inline bool nq_socket_error_would_block(int error)
{
    return error == WSAEWOULDBLOCK;
}

static inline bool nq_socket_error_interrupted(int error)
{
    return error == WSAEINTR;
}

static inline bool nq_socket_fits_select(nq_socket_t socket_fd)
{
    (void)socket_fd;
    return true;
}

static inline void nq_socket_set_capacity_error(void)
{
    WSASetLastError(WSAEMFILE);
}

static inline void nq_report_socket_error(const char *operation)
{
    fprintf(stderr, "%s: Windows socket error %d\n", operation,
            nq_socket_last_error());
}

static inline double nq_monotonic_seconds(void)
{
    return (double)GetTickCount64() / 1000.0;
}

static inline int nq_socket_send(nq_socket_t socket_fd, const void *buffer,
                                 size_t length, int flags)
{
    if (length > INT_MAX) {
        WSASetLastError(WSAEMSGSIZE);
        return -1;
    }
    return send(socket_fd, (const char *)buffer, (int)length, flags);
}

static inline int nq_socket_sendto(nq_socket_t socket_fd, const void *buffer,
                                   size_t length, int flags,
                                   const struct sockaddr *address,
                                   nq_socklen_t address_length)
{
    if (length > INT_MAX) {
        WSASetLastError(WSAEMSGSIZE);
        return -1;
    }
    return sendto(socket_fd, (const char *)buffer, (int)length, flags,
                  address, address_length);
}

static inline int nq_socket_recv(nq_socket_t socket_fd, void *buffer,
                                 size_t length, int flags)
{
    if (length > INT_MAX) {
        WSASetLastError(WSAEMSGSIZE);
        return -1;
    }
    return recv(socket_fd, (char *)buffer, (int)length, flags);
}

static inline int nq_socket_recvfrom(nq_socket_t socket_fd, void *buffer,
                                     size_t length, int flags,
                                     struct sockaddr *address,
                                     nq_socklen_t *address_length)
{
    if (length > INT_MAX) {
        WSASetLastError(WSAEMSGSIZE);
        return -1;
    }
    return recvfrom(socket_fd, (char *)buffer, (int)length, flags,
                    address, address_length);
}

static inline int nq_socket_connect(nq_socket_t socket_fd,
                                    const struct sockaddr *address,
                                    nq_socklen_t address_length)
{
    return connect(socket_fd, address, address_length);
}

static inline int nq_socket_bind(nq_socket_t socket_fd,
                                 const struct sockaddr *address,
                                 nq_socklen_t address_length)
{
    return bind(socket_fd, address, address_length);
}

static inline int nq_socket_getsockname(nq_socket_t socket_fd,
                                        struct sockaddr *address,
                                        nq_socklen_t *address_length)
{
    return getsockname(socket_fd, address, address_length);
}

static inline int nq_socket_set_receive_timeout(nq_socket_t socket_fd,
                                                unsigned int milliseconds)
{
    DWORD timeout = (DWORD)milliseconds;
    return setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO,
                      (const char *)&timeout, (int)sizeof(timeout));
}

static inline int nq_socket_select(nq_socket_t max_socket, fd_set *read_fds,
                                   struct timeval *timeout)
{
    (void)max_socket;
    return select(0, read_fds, NULL, NULL, timeout);
}

#else

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

typedef int nq_socket_t;
typedef socklen_t nq_socklen_t;

#define NQ_INVALID_SOCKET (-1)

static inline int nq_socket_startup(void)
{
    return 0;
}

static inline void nq_socket_cleanup(void)
{
}

static inline int nq_close_socket(nq_socket_t socket_fd)
{
    return close(socket_fd);
}

static inline bool nq_set_nonblocking(nq_socket_t socket_fd)
{
    int flags = fcntl(socket_fd, F_GETFL, 0);
    return flags >= 0 &&
           fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static inline int nq_socket_last_error(void)
{
    return errno;
}

static inline bool nq_socket_error_would_block(int error)
{
    return error == EAGAIN || error == EWOULDBLOCK;
}

static inline bool nq_socket_error_interrupted(int error)
{
    return error == EINTR;
}

static inline bool nq_socket_fits_select(nq_socket_t socket_fd)
{
    return socket_fd >= 0 && socket_fd < FD_SETSIZE;
}

static inline void nq_socket_set_capacity_error(void)
{
    errno = EMFILE;
}

static inline void nq_report_socket_error(const char *operation)
{
    perror(operation);
}

static inline double nq_monotonic_seconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static inline int nq_socket_send(nq_socket_t socket_fd, const void *buffer,
                                 size_t length, int flags)
{
    if (length > INT_MAX) {
        errno = EMSGSIZE;
        return -1;
    }
    ssize_t result = send(socket_fd, buffer, length, flags);
    return (int)result;
}

static inline int nq_socket_sendto(nq_socket_t socket_fd, const void *buffer,
                                   size_t length, int flags,
                                   const struct sockaddr *address,
                                   nq_socklen_t address_length)
{
    if (length > INT_MAX) {
        errno = EMSGSIZE;
        return -1;
    }
    ssize_t result = sendto(socket_fd, buffer, length, flags, address,
                            address_length);
    return (int)result;
}

static inline int nq_socket_recv(nq_socket_t socket_fd, void *buffer,
                                 size_t length, int flags)
{
    if (length > INT_MAX) {
        errno = EMSGSIZE;
        return -1;
    }
    ssize_t result = recv(socket_fd, buffer, length, flags);
    return (int)result;
}

static inline int nq_socket_recvfrom(nq_socket_t socket_fd, void *buffer,
                                     size_t length, int flags,
                                     struct sockaddr *address,
                                     nq_socklen_t *address_length)
{
    if (length > INT_MAX) {
        errno = EMSGSIZE;
        return -1;
    }
    ssize_t result = recvfrom(socket_fd, buffer, length, flags, address,
                              address_length);
    return (int)result;
}

static inline int nq_socket_connect(nq_socket_t socket_fd,
                                    const struct sockaddr *address,
                                    nq_socklen_t address_length)
{
    return connect(socket_fd, address, address_length);
}

static inline int nq_socket_bind(nq_socket_t socket_fd,
                                 const struct sockaddr *address,
                                 nq_socklen_t address_length)
{
    return bind(socket_fd, address, address_length);
}

static inline int nq_socket_getsockname(nq_socket_t socket_fd,
                                        struct sockaddr *address,
                                        nq_socklen_t *address_length)
{
    return getsockname(socket_fd, address, address_length);
}

static inline int nq_socket_set_receive_timeout(nq_socket_t socket_fd,
                                                unsigned int milliseconds)
{
    struct timeval timeout = {
        (time_t)(milliseconds / 1000u),
        (suseconds_t)((milliseconds % 1000u) * 1000u)
    };
    return setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                      (socklen_t)sizeof(timeout));
}

static inline int nq_socket_select(nq_socket_t max_socket, fd_set *read_fds,
                                   struct timeval *timeout)
{
    return select(max_socket + 1, read_fds, NULL, NULL, timeout);
}

#endif

#endif
