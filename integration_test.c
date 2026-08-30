/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L

#include "netchan.h"
#include "socket_compat.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <process.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, \
                "integration FAIL %s:%d: %s (socket error %d, errno %d)\n", \
                __FILE__, __LINE__, #condition, nq_socket_last_error(), \
                errno); \
        goto cleanup; \
    } \
} while (0)

static void put_be32(uint8_t *p, uint32_t value)
{
    value = htonl(value);
    memcpy(p, &value, 4);
}

static void put_le32(uint8_t **p, uint32_t value)
{
    *(*p)++ = (uint8_t)value;
    *(*p)++ = (uint8_t)(value >> 8);
    *(*p)++ = (uint8_t)(value >> 16);
    *(*p)++ = (uint8_t)(value >> 24);
}

static uint32_t get_be32(const uint8_t *p)
{
    uint32_t value;
    memcpy(&value, p, 4);
    return ntohl(value);
}

static void put_string(uint8_t **p, const char *string)
{
    size_t len = strlen(string) + 1;
    memcpy(*p, string, len);
    *p += len;
}

static nq_socket_t bind_loopback(uint16_t *port)
{
    struct sockaddr_in address;
    nq_socklen_t address_len = (nq_socklen_t)sizeof(address);
    nq_socket_t fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == NQ_INVALID_SOCKET)
        return NQ_INVALID_SOCKET;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (nq_socket_bind(fd, (struct sockaddr *)&address,
                       (nq_socklen_t)sizeof(address)) != 0 ||
        nq_socket_getsockname(fd, (struct sockaddr *)&address,
                              &address_len) != 0 ||
        nq_socket_set_receive_timeout(fd, 2000u) != 0) {
        (void)nq_close_socket(fd);
        return NQ_INVALID_SOCKET;
    }
    *port = ntohs(address.sin_port);
    return fd;
}

static bool wait_readable(nq_socket_t fd, long microseconds)
{
    fd_set read_fds;
    struct timeval timeout = {
        microseconds / 1000000L,
        microseconds % 1000000L
    };

    FD_ZERO(&read_fds);
    FD_SET(fd, &read_fds);
    return nq_socket_select(fd, &read_fds, &timeout) > 0;
}

#ifdef _WIN32

typedef intptr_t nq_process_t;

static nq_process_t start_proxy(const char *program, const char *server,
                                const char *listen, const char *advertise)
{
    return _spawnl(_P_NOWAIT, program, "nq666-proxy", "--server", server,
                   "--listen", listen, "--advertise", advertise,
                   (char *)NULL);
}

static void stop_proxy(nq_process_t process)
{
    HANDLE handle = (HANDLE)process;
    (void)TerminateProcess(handle, 0);
    (void)WaitForSingleObject(handle, 5000);
    (void)CloseHandle(handle);
}

#else

typedef pid_t nq_process_t;

static nq_process_t start_proxy(const char *program, const char *server,
                                const char *listen, const char *advertise)
{
    pid_t process = fork();
    if (process == 0) {
        execl(program, "nq666-proxy", "--server", server,
              "--listen", listen, "--advertise", advertise,
              (char *)NULL);
        _exit(127);
    }
    return process;
}

static void stop_proxy(nq_process_t process)
{
    (void)kill(process, SIGTERM);
    (void)waitpid(process, NULL, 0);
}

#endif

static void make_reliable(uint8_t *packet, size_t *packet_len,
                          uint32_t sequence, const uint8_t *payload,
                          size_t payload_len)
{
    *packet_len = 8 + payload_len;
    put_be32(packet, NQ_NETFLAG_DATA | NQ_NETFLAG_EOM |
                     (uint32_t)*packet_len);
    put_be32(packet + 4, sequence);
    memcpy(packet + 8, payload, payload_len);
}

static void make_ack(uint8_t packet[8], uint32_t sequence)
{
    put_be32(packet, NQ_NETFLAG_ACK | 8);
    put_be32(packet + 4, sequence);
}

int main(void)
{
    uint16_t server_port = 0;
    uint16_t proxy_port = 0;
    uint16_t unused_port = 0;
    nq_socket_t server_fd = NQ_INVALID_SOCKET;
    nq_socket_t reservation_fd = NQ_INVALID_SOCKET;
    nq_socket_t client_fd = NQ_INVALID_SOCKET;
    nq_process_t proxy_process = (nq_process_t)-1;
    int result = 1;
    struct sockaddr_in proxy_address;
    struct sockaddr_in upstream_peer;
    nq_socklen_t peer_len;
    char server_arg[64];
    char listen_arg[64];
    char advertise_arg[64];
    char advertised_expected[64];
    const char *proxy_program = getenv("NQ666_PROXY");
    uint8_t packet[2048];
    uint8_t payload[1024];
    uint8_t *p;
    size_t packet_len;
    int received;
    bool got_server_ack = false;
    bool got_pext = false;
    unsigned int attempt;

    if (!proxy_program || !proxy_program[0])
#ifdef _WIN32
        proxy_program = "./nq666-proxy.exe";
#else
        proxy_program = "./nq666-proxy";
#endif

    CHECK(nq_socket_startup() == 0);
    CHECK(atexit(nq_socket_cleanup) == 0);

    server_fd = bind_loopback(&server_port);
    CHECK(server_fd != NQ_INVALID_SOCKET);
    reservation_fd = bind_loopback(&unused_port);
    CHECK(reservation_fd != NQ_INVALID_SOCKET);
    proxy_port = unused_port;
    (void)nq_close_socket(reservation_fd);
    reservation_fd = NQ_INVALID_SOCKET;

    (void)snprintf(server_arg, sizeof(server_arg), "127.0.0.1:%u", server_port);
    (void)snprintf(listen_arg, sizeof(listen_arg), "127.0.0.1:%u", proxy_port);
    (void)snprintf(advertise_arg, sizeof(advertise_arg), "198.51.100.20");
    (void)snprintf(advertised_expected, sizeof(advertised_expected),
                   "198.51.100.20:%u", proxy_port);
    proxy_process = start_proxy(proxy_program, server_arg, listen_arg,
                                advertise_arg);
    CHECK(proxy_process != (nq_process_t)-1);
    client_fd = bind_loopback(&unused_port);
    CHECK(client_fd != NQ_INVALID_SOCKET);
    memset(&proxy_address, 0, sizeof(proxy_address));
    proxy_address.sin_family = AF_INET;
    proxy_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    proxy_address.sin_port = htons(proxy_port);

    /* Server-browser requests must return the public proxy address. */
    p = packet + 4;
    *p++ = 2;
    put_string(&p, "QUAKE");
    *p++ = 3;
    packet_len = (size_t)(p - packet);
    put_be32(packet, NQ_NETFLAG_CTL | (uint32_t)packet_len);
    for (attempt = 0; attempt < 40; attempt++) {
        CHECK(nq_socket_sendto(client_fd, packet, packet_len, 0,
                               (struct sockaddr *)&proxy_address,
                               (nq_socklen_t)sizeof(proxy_address)) ==
              (int)packet_len);
        if (wait_readable(server_fd, 50000L))
            break;
    }
    CHECK(attempt < 40);
    peer_len = sizeof(upstream_peer);
    received = nq_socket_recvfrom(server_fd, packet, sizeof(packet), 0,
                                  (struct sockaddr *)&upstream_peer,
                                  &peer_len);
    CHECK(received >= 12 && packet[4] == 2);
    p = packet + 4;
    *p++ = 0x83;
    put_string(&p, "127.0.0.1:1");
    put_string(&p, "Integration server");
    put_string(&p, "start");
    *p++ = 0;
    *p++ = 4;
    *p++ = 3;
    packet_len = (size_t)(p - packet);
    put_be32(packet, NQ_NETFLAG_CTL | (uint32_t)packet_len);
    CHECK(nq_socket_sendto(server_fd, packet, packet_len, 0,
                           (struct sockaddr *)&upstream_peer,
                           (nq_socklen_t)sizeof(upstream_peer)) ==
          (int)packet_len);
    received = nq_socket_recv(client_fd, packet, sizeof(packet), 0);
    CHECK(received > 6 && packet[4] == 0x83);
    CHECK(!strcmp((char *)packet + 5, advertised_expected));

    /* Do not turn a private upstream RCON endpoint into a public one. */
    packet_len = 5;
    packet[4] = 5;
    put_be32(packet, NQ_NETFLAG_CTL | (uint32_t)packet_len);
    CHECK(nq_socket_sendto(client_fd, packet, packet_len, 0,
                           (struct sockaddr *)&proxy_address,
                           (nq_socklen_t)sizeof(proxy_address)) ==
          (int)packet_len);
    CHECK(!wait_readable(server_fd, 100000L));

    /* An oversized handshake is rejected without consuming a player slot. */
    memset(packet, 0, 1025);
    p = packet + 4;
    *p++ = 1;
    put_string(&p, "QUAKE");
    *p++ = 3;
    put_be32(packet, NQ_NETFLAG_CTL | 1025u);
    CHECK(nq_socket_sendto(client_fd, packet, 1025, 0,
                           (struct sockaddr *)&proxy_address,
                           (nq_socklen_t)sizeof(proxy_address)) == 1025);
    received = nq_socket_recv(client_fd, packet, sizeof(packet), 0);
    CHECK(received > 5 && packet[4] == 0x82);

    /* Normal NetQuake connection handshake. */
    p = packet + 4;
    *p++ = 1;
    put_string(&p, "QUAKE");
    *p++ = 3;
    packet_len = (size_t)(p - packet);
    put_be32(packet, NQ_NETFLAG_CTL | (uint32_t)packet_len);
    CHECK(nq_socket_sendto(client_fd, packet, packet_len, 0,
                           (struct sockaddr *)&proxy_address,
                           (nq_socklen_t)sizeof(proxy_address)) ==
          (int)packet_len);

    peer_len = sizeof(upstream_peer);
    received = nq_socket_recvfrom(server_fd, packet, sizeof(packet), 0,
                                  (struct sockaddr *)&upstream_peer,
                                  &peer_len);
    CHECK(received >= 12 && packet[4] == 1);

    /* Reject an impossible game port, then allow a clean retry. */
    p = packet + 4;
    *p++ = 0x81;
    put_le32(&p, 70000u);
    packet_len = (size_t)(p - packet);
    put_be32(packet, NQ_NETFLAG_CTL | (uint32_t)packet_len);
    CHECK(nq_socket_sendto(server_fd, packet, packet_len, 0,
                           (struct sockaddr *)&upstream_peer,
                           (nq_socklen_t)sizeof(upstream_peer)) ==
          (int)packet_len);
    received = nq_socket_recv(client_fd, packet, sizeof(packet), 0);
    CHECK(received > 5 && packet[4] == 0x82);

    p = packet + 4;
    *p++ = 1;
    put_string(&p, "QUAKE");
    *p++ = 3;
    packet_len = (size_t)(p - packet);
    put_be32(packet, NQ_NETFLAG_CTL | (uint32_t)packet_len);
    CHECK(nq_socket_sendto(client_fd, packet, packet_len, 0,
                           (struct sockaddr *)&proxy_address,
                           (nq_socklen_t)sizeof(proxy_address)) ==
          (int)packet_len);
    peer_len = sizeof(upstream_peer);
    received = nq_socket_recvfrom(server_fd, packet, sizeof(packet), 0,
                                  (struct sockaddr *)&upstream_peer,
                                  &peer_len);
    CHECK(received >= 12 && packet[4] == 1);

    p = packet + 4;
    *p++ = 0x81;
    put_le32(&p, server_port);
    packet_len = (size_t)(p - packet);
    put_be32(packet, NQ_NETFLAG_CTL | (uint32_t)packet_len);
    CHECK(nq_socket_sendto(server_fd, packet, packet_len, 0,
                           (struct sockaddr *)&upstream_peer,
                           (nq_socklen_t)sizeof(upstream_peer)) ==
          (int)packet_len);

    received = nq_socket_recv(client_fd, packet, sizeof(packet), 0);
    CHECK(received == 9 && packet[4] == 0x81);
    CHECK((uint16_t)(packet[5] | ((uint16_t)packet[6] << 8)) == proxy_port);

    p = payload;
    *p++ = 9;
    put_string(&p, "cmd pext\n");
    make_reliable(packet, &packet_len, 0, payload, (size_t)(p - payload));
    CHECK(nq_socket_sendto(server_fd, packet, packet_len, 0,
                           (struct sockaddr *)&upstream_peer,
                           (nq_socklen_t)sizeof(upstream_peer)) ==
          (int)packet_len);

    received = nq_socket_recv(client_fd, packet, sizeof(packet), 0);
    CHECK(received > 8);
    CHECK((get_be32(packet) & (NQ_NETFLAG_DATA | NQ_NETFLAG_EOM)) ==
          (NQ_NETFLAG_DATA | NQ_NETFLAG_EOM));
    CHECK(packet[8] == 9 && !strcmp((char *)packet + 9, "cmd pext\n"));
    make_ack(packet, 0);
    CHECK(nq_socket_sendto(client_fd, packet, 8, 0,
                           (struct sockaddr *)&proxy_address,
                           (nq_socklen_t)sizeof(proxy_address)) == 8);

    p = payload;
    *p++ = 4;
    put_string(&p, "pext 123 456");
    make_reliable(packet, &packet_len, 0, payload, (size_t)(p - payload));
    CHECK(nq_socket_sendto(client_fd, packet, packet_len, 0,
                           (struct sockaddr *)&proxy_address,
                           (nq_socklen_t)sizeof(proxy_address)) ==
          (int)packet_len);

    while (!got_server_ack || !got_pext) {
        received = nq_socket_recvfrom(server_fd, packet, sizeof(packet), 0,
                                      (struct sockaddr *)&upstream_peer,
                                      &peer_len);
        CHECK(received >= 8);
        if ((get_be32(packet) & ~NQ_NETFLAG_LENGTH_MASK) == NQ_NETFLAG_ACK) {
            got_server_ack = true;
        } else if ((get_be32(packet) & NQ_NETFLAG_DATA) != 0) {
            CHECK(packet[8] == 4 && !strcmp((char *)packet + 9, "pext"));
            got_pext = true;
            make_ack(packet, 0);
            CHECK(nq_socket_sendto(
                      server_fd, packet, 8, 0,
                      (struct sockaddr *)&upstream_peer,
                      (nq_socklen_t)sizeof(upstream_peer)) == 8);
        }
    }

    p = payload;
    *p++ = 11;
    put_le32(&p, 666);
    *p++ = 4;
    *p++ = 1;
    put_string(&p, "Integration");
    put_string(&p, "maps/start.bsp");
    *p++ = 0;
    put_string(&p, "misc/talk.wav");
    *p++ = 0;
    *p++ = 25;
    *p++ = 1;
    make_reliable(packet, &packet_len, 1, payload, (size_t)(p - payload));
    CHECK(nq_socket_sendto(server_fd, packet, packet_len, 0,
                           (struct sockaddr *)&upstream_peer,
                           (nq_socklen_t)sizeof(upstream_peer)) ==
          (int)packet_len);

    for (;;) {
        received = nq_socket_recv(client_fd, packet, sizeof(packet), 0);
        CHECK(received >= 8);
        if ((get_be32(packet) & NQ_NETFLAG_DATA) != 0)
            break;
    }
    CHECK(packet[8] == 11);
    CHECK(packet[9] == 15 && packet[10] == 0 &&
          packet[11] == 0 && packet[12] == 0);
    make_ack(packet, 1);
    (void)nq_socket_sendto(client_fd, packet, 8, 0,
                           (struct sockaddr *)&proxy_address,
                           (nq_socklen_t)sizeof(proxy_address));

    result = 0;
    printf("process integration test passed\n");

cleanup:
    if (proxy_process != (nq_process_t)-1)
        stop_proxy(proxy_process);
    if (client_fd != NQ_INVALID_SOCKET)
        (void)nq_close_socket(client_fd);
    if (reservation_fd != NQ_INVALID_SOCKET)
        (void)nq_close_socket(reservation_fd);
    if (server_fd != NQ_INVALID_SOCKET)
        (void)nq_close_socket(server_fd);
    return result;
}
