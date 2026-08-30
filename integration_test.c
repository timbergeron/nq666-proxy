/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L

#include "netchan.h"

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "integration FAIL %s:%d: %s (errno %d)\n", \
                __FILE__, __LINE__, #condition, errno); \
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

static int bind_loopback(uint16_t *port)
{
    struct sockaddr_in address;
    struct timeval timeout = {2, 0};
    socklen_t address_len = sizeof(address);
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        getsockname(fd, (struct sockaddr *)&address, &address_len) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        close(fd);
        return -1;
    }
    *port = ntohs(address.sin_port);
    return fd;
}

static bool wait_readable(int fd, long microseconds)
{
    fd_set read_fds;
    struct timeval timeout = {
        microseconds / 1000000L,
        microseconds % 1000000L
    };

    FD_ZERO(&read_fds);
    FD_SET(fd, &read_fds);
    return select(fd + 1, &read_fds, NULL, NULL, &timeout) > 0;
}

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
    int server_fd = -1;
    int reservation_fd = -1;
    int client_fd = -1;
    pid_t proxy_pid = -1;
    int result = 1;
    struct sockaddr_in proxy_address;
    struct sockaddr_in upstream_peer;
    socklen_t peer_len;
    char server_arg[64];
    char listen_arg[64];
    char advertise_arg[64];
    char advertised_expected[64];
    const char *proxy_program = getenv("NQ666_PROXY");
    uint8_t packet[2048];
    uint8_t payload[1024];
    uint8_t *p;
    size_t packet_len;
    ssize_t received;
    bool got_server_ack = false;
    bool got_pext = false;
    unsigned int attempt;

    if (!proxy_program || !proxy_program[0])
        proxy_program = "./nq666-proxy";

    server_fd = bind_loopback(&server_port);
    CHECK(server_fd >= 0);
    reservation_fd = bind_loopback(&unused_port);
    CHECK(reservation_fd >= 0);
    proxy_port = unused_port;
    close(reservation_fd);
    reservation_fd = -1;

    (void)snprintf(server_arg, sizeof(server_arg), "127.0.0.1:%u", server_port);
    (void)snprintf(listen_arg, sizeof(listen_arg), "127.0.0.1:%u", proxy_port);
    (void)snprintf(advertise_arg, sizeof(advertise_arg), "198.51.100.20");
    (void)snprintf(advertised_expected, sizeof(advertised_expected),
                   "198.51.100.20:%u", proxy_port);
    proxy_pid = fork();
    CHECK(proxy_pid >= 0);
    if (proxy_pid == 0) {
        execl(proxy_program, "nq666-proxy", "--server", server_arg,
              "--listen", listen_arg, "--advertise", advertise_arg,
              (char *)NULL);
        _exit(127);
    }
    client_fd = bind_loopback(&unused_port);
    CHECK(client_fd >= 0);
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
        CHECK(sendto(client_fd, packet, packet_len, 0,
                     (struct sockaddr *)&proxy_address,
                     sizeof(proxy_address)) == (ssize_t)packet_len);
        if (wait_readable(server_fd, 50000L))
            break;
    }
    CHECK(attempt < 40);
    peer_len = sizeof(upstream_peer);
    received = recvfrom(server_fd, packet, sizeof(packet), 0,
                        (struct sockaddr *)&upstream_peer, &peer_len);
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
    CHECK(sendto(server_fd, packet, packet_len, 0,
                 (struct sockaddr *)&upstream_peer,
                 sizeof(upstream_peer)) == (ssize_t)packet_len);
    received = recv(client_fd, packet, sizeof(packet), 0);
    CHECK(received > 6 && packet[4] == 0x83);
    CHECK(!strcmp((char *)packet + 5, advertised_expected));

    /* Do not turn a private upstream RCON endpoint into a public one. */
    packet_len = 5;
    packet[4] = 5;
    put_be32(packet, NQ_NETFLAG_CTL | (uint32_t)packet_len);
    CHECK(sendto(client_fd, packet, packet_len, 0,
                 (struct sockaddr *)&proxy_address,
                 sizeof(proxy_address)) == (ssize_t)packet_len);
    CHECK(!wait_readable(server_fd, 100000L));

    /* An oversized handshake is rejected without consuming a player slot. */
    memset(packet, 0, 1025);
    p = packet + 4;
    *p++ = 1;
    put_string(&p, "QUAKE");
    *p++ = 3;
    put_be32(packet, NQ_NETFLAG_CTL | 1025u);
    CHECK(sendto(client_fd, packet, 1025, 0,
                 (struct sockaddr *)&proxy_address,
                 sizeof(proxy_address)) == 1025);
    received = recv(client_fd, packet, sizeof(packet), 0);
    CHECK(received > 5 && packet[4] == 0x82);

    /* Normal NetQuake connection handshake. */
    p = packet + 4;
    *p++ = 1;
    put_string(&p, "QUAKE");
    *p++ = 3;
    packet_len = (size_t)(p - packet);
    put_be32(packet, NQ_NETFLAG_CTL | (uint32_t)packet_len);
    CHECK(sendto(client_fd, packet, packet_len, 0,
                 (struct sockaddr *)&proxy_address,
                 sizeof(proxy_address)) == (ssize_t)packet_len);

    peer_len = sizeof(upstream_peer);
    received = recvfrom(server_fd, packet, sizeof(packet), 0,
                        (struct sockaddr *)&upstream_peer, &peer_len);
    CHECK(received >= 12 && packet[4] == 1);

    /* Reject an impossible game port, then allow a clean retry. */
    p = packet + 4;
    *p++ = 0x81;
    put_le32(&p, 70000u);
    packet_len = (size_t)(p - packet);
    put_be32(packet, NQ_NETFLAG_CTL | (uint32_t)packet_len);
    CHECK(sendto(server_fd, packet, packet_len, 0,
                 (struct sockaddr *)&upstream_peer,
                 sizeof(upstream_peer)) == (ssize_t)packet_len);
    received = recv(client_fd, packet, sizeof(packet), 0);
    CHECK(received > 5 && packet[4] == 0x82);

    p = packet + 4;
    *p++ = 1;
    put_string(&p, "QUAKE");
    *p++ = 3;
    packet_len = (size_t)(p - packet);
    put_be32(packet, NQ_NETFLAG_CTL | (uint32_t)packet_len);
    CHECK(sendto(client_fd, packet, packet_len, 0,
                 (struct sockaddr *)&proxy_address,
                 sizeof(proxy_address)) == (ssize_t)packet_len);
    peer_len = sizeof(upstream_peer);
    received = recvfrom(server_fd, packet, sizeof(packet), 0,
                        (struct sockaddr *)&upstream_peer, &peer_len);
    CHECK(received >= 12 && packet[4] == 1);

    p = packet + 4;
    *p++ = 0x81;
    put_le32(&p, server_port);
    packet_len = (size_t)(p - packet);
    put_be32(packet, NQ_NETFLAG_CTL | (uint32_t)packet_len);
    CHECK(sendto(server_fd, packet, packet_len, 0,
                 (struct sockaddr *)&upstream_peer,
                 sizeof(upstream_peer)) == (ssize_t)packet_len);

    received = recv(client_fd, packet, sizeof(packet), 0);
    CHECK(received == 9 && packet[4] == 0x81);
    CHECK((uint16_t)(packet[5] | ((uint16_t)packet[6] << 8)) == proxy_port);

    p = payload;
    *p++ = 9;
    put_string(&p, "cmd pext\n");
    make_reliable(packet, &packet_len, 0, payload, (size_t)(p - payload));
    CHECK(sendto(server_fd, packet, packet_len, 0,
                 (struct sockaddr *)&upstream_peer,
                 sizeof(upstream_peer)) == (ssize_t)packet_len);

    received = recv(client_fd, packet, sizeof(packet), 0);
    CHECK(received > 8);
    CHECK((get_be32(packet) & (NQ_NETFLAG_DATA | NQ_NETFLAG_EOM)) ==
          (NQ_NETFLAG_DATA | NQ_NETFLAG_EOM));
    CHECK(packet[8] == 9 && !strcmp((char *)packet + 9, "cmd pext\n"));
    make_ack(packet, 0);
    CHECK(sendto(client_fd, packet, 8, 0,
                 (struct sockaddr *)&proxy_address,
                 sizeof(proxy_address)) == 8);

    p = payload;
    *p++ = 4;
    put_string(&p, "pext 123 456");
    make_reliable(packet, &packet_len, 0, payload, (size_t)(p - payload));
    CHECK(sendto(client_fd, packet, packet_len, 0,
                 (struct sockaddr *)&proxy_address,
                 sizeof(proxy_address)) == (ssize_t)packet_len);

    while (!got_server_ack || !got_pext) {
        received = recvfrom(server_fd, packet, sizeof(packet), 0,
                            (struct sockaddr *)&upstream_peer, &peer_len);
        CHECK(received >= 8);
        if ((get_be32(packet) & ~NQ_NETFLAG_LENGTH_MASK) == NQ_NETFLAG_ACK) {
            got_server_ack = true;
        } else if ((get_be32(packet) & NQ_NETFLAG_DATA) != 0) {
            CHECK(packet[8] == 4 && !strcmp((char *)packet + 9, "pext"));
            got_pext = true;
            make_ack(packet, 0);
            CHECK(sendto(server_fd, packet, 8, 0,
                         (struct sockaddr *)&upstream_peer,
                         sizeof(upstream_peer)) == 8);
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
    CHECK(sendto(server_fd, packet, packet_len, 0,
                 (struct sockaddr *)&upstream_peer,
                 sizeof(upstream_peer)) == (ssize_t)packet_len);

    for (;;) {
        received = recv(client_fd, packet, sizeof(packet), 0);
        CHECK(received >= 8);
        if ((get_be32(packet) & NQ_NETFLAG_DATA) != 0)
            break;
    }
    CHECK(packet[8] == 11);
    CHECK(packet[9] == 15 && packet[10] == 0 &&
          packet[11] == 0 && packet[12] == 0);
    make_ack(packet, 1);
    (void)sendto(client_fd, packet, 8, 0,
                 (struct sockaddr *)&proxy_address, sizeof(proxy_address));

    result = 0;
    printf("process integration test passed\n");

cleanup:
    if (proxy_pid > 0) {
        (void)kill(proxy_pid, SIGTERM);
        (void)waitpid(proxy_pid, NULL, 0);
    }
    if (client_fd >= 0)
        close(client_fd);
    if (reservation_fd >= 0)
        close(reservation_fd);
    if (server_fd >= 0)
        close(server_fd);
    return result;
}
