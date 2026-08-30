#define _POSIX_C_SOURCE 200809L

#include "netchan.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_LISTEN "0.0.0.0:26001"
#define DEFAULT_PORT "26000"
#define DEFAULT_MAX_SESSIONS 64
#define MAX_SESSIONS_LIMIT 256
#define SESSION_TIMEOUT 300.0
#define HANDSHAKE_TIMEOUT 8.0
#define QUERY_TIMEOUT 3.0

#define NET_PROTOCOL_VERSION 3
#define CCREQ_CONNECT 0x01
#define CCREP_ACCEPT 0x81
#define CCREP_REJECT 0x82
#define CCREP_SERVER_INFO 0x83
#define MOD_PROQUAKE 1
#define PQF_IGNOREPORT 0x80
#define CLC_DISCONNECT 2

enum session_phase {
    SESSION_FREE = 0,
    SESSION_PENDING,
    SESSION_ACTIVE,
    SESSION_QUERY
};

struct packet_target {
    int fd;
    bool connected;
    struct sockaddr_in address;
};

struct session {
    enum session_phase phase;
    int upstream_fd;
    struct sockaddr_in client_address;
    struct sockaddr_in upstream_address;
    struct packet_target client_target;
    struct packet_target upstream_target;
    struct nq_chan client_chan;
    struct nq_chan upstream_chan;
    struct nq_xlat_state xlat;
    uint8_t connect_request[1024];
    size_t connect_request_len;
    uint8_t accept_reply[1024];
    size_t accept_reply_len;
    double created_at;
    double last_activity;
    unsigned int translation_errors;
};

struct proxy {
    int listen_fd;
    struct sockaddr_in listen_address;
    struct sockaddr_in upstream_address;
    uint16_t listen_port;
    char advertise[256];
    struct session *sessions;
    size_t max_sessions;
    bool verbose;
};

static volatile sig_atomic_t stop_requested;

static void on_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static double monotonic_seconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_be32(uint8_t *p, uint32_t value)
{
    value = htonl(value);
    memcpy(p, &value, sizeof(value));
}

static void write_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static const char *address_string(const struct sockaddr_in *address,
                                  char *buffer, size_t buffer_size)
{
    char ip[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &address->sin_addr, ip, sizeof(ip)))
        (void)snprintf(ip, sizeof(ip), "?");
    (void)snprintf(buffer, buffer_size, "%s:%u", ip,
                   (unsigned int)ntohs(address->sin_port));
    return buffer;
}

static bool same_address(const struct sockaddr_in *a,
                         const struct sockaddr_in *b)
{
    return a->sin_family == b->sin_family &&
           a->sin_port == b->sin_port &&
           a->sin_addr.s_addr == b->sin_addr.s_addr;
}

static bool set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static bool split_endpoint(const char *text, char *host, size_t host_size,
                           char *port, size_t port_size,
                           const char *default_port)
{
    const char *colon = strrchr(text, ':');
    size_t host_len;

    if (colon && strchr(colon + 1, ':') == NULL) {
        host_len = (size_t)(colon - text);
        if (host_len >= host_size || strlen(colon + 1) >= port_size)
            return false;
        memcpy(host, text, host_len);
        host[host_len] = 0;
        (void)snprintf(port, port_size, "%s", colon + 1);
    } else {
        if (strlen(text) >= host_size || strlen(default_port) >= port_size)
            return false;
        (void)snprintf(host, host_size, "%s", text);
        (void)snprintf(port, port_size, "%s", default_port);
    }
    return port[0] != 0;
}

static bool resolve_endpoint(const char *text, bool passive,
                             const char *default_port,
                             struct sockaddr_in *address)
{
    char host[256];
    char port[16];
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    int status;

    if (!split_endpoint(text, host, sizeof(host), port, sizeof(port),
                        default_port))
        return false;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = passive ? AI_PASSIVE : 0;
    status = getaddrinfo(host[0] ? host : NULL, port, &hints, &result);
    if (status != 0) {
        fprintf(stderr, "cannot resolve %s: %s\n", text,
                gai_strerror(status));
        return false;
    }
    if (!result || result->ai_addrlen != sizeof(*address)) {
        freeaddrinfo(result);
        return false;
    }
    memcpy(address, result->ai_addr, sizeof(*address));
    freeaddrinfo(result);
    return true;
}

static ssize_t send_packet(void *opaque, const void *packet, size_t packet_len)
{
    struct packet_target *target = opaque;
    if (target->connected)
        return send(target->fd, packet, packet_len, 0);
    return sendto(target->fd, packet, packet_len, 0,
                  (const struct sockaddr *)&target->address,
                  sizeof(target->address));
}

static int open_upstream_socket(const struct sockaddr_in *upstream)
{
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0)
        return -1;
    if (!set_nonblocking(fd) ||
        connect(fd, (const struct sockaddr *)upstream, sizeof(*upstream)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void session_close(struct session *session)
{
    if (session->phase == SESSION_FREE)
        return;
    nq_chan_destroy(&session->client_chan);
    nq_chan_destroy(&session->upstream_chan);
    if (session->upstream_fd >= 0)
        close(session->upstream_fd);
    memset(session, 0, sizeof(*session));
    session->upstream_fd = -1;
}

static struct session *find_client_session(struct proxy *proxy,
                                           const struct sockaddr_in *client)
{
    size_t i;
    for (i = 0; i < proxy->max_sessions; i++) {
        struct session *session = &proxy->sessions[i];
        if ((session->phase == SESSION_PENDING ||
             session->phase == SESSION_ACTIVE) &&
            same_address(&session->client_address, client))
            return session;
    }
    return NULL;
}

static struct session *allocate_session(struct proxy *proxy)
{
    size_t i;
    for (i = 0; i < proxy->max_sessions; i++) {
        if (proxy->sessions[i].phase == SESSION_FREE) {
            proxy->sessions[i].upstream_fd = -1;
            return &proxy->sessions[i];
        }
    }
    return NULL;
}

static bool valid_connect_request(const uint8_t *packet, size_t packet_len)
{
    static const uint8_t quake_name[] = {'Q','U','A','K','E',0};
    return nq_packet_is_control(packet, packet_len) &&
           packet_len >= 12 && packet[4] == CCREQ_CONNECT &&
           memcmp(packet + 5, quake_name, sizeof(quake_name)) == 0 &&
           packet[11] == NET_PROTOCOL_VERSION;
}

static void send_control_reject(struct proxy *proxy,
                                const struct sockaddr_in *client,
                                const char *reason)
{
    uint8_t packet[512];
    size_t reason_len = strlen(reason);
    size_t len;

    if (reason_len > sizeof(packet) - 7)
        reason_len = sizeof(packet) - 7;
    len = 4 + 1 + reason_len + 1;
    write_be32(packet, NQ_NETFLAG_CTL | (uint32_t)len);
    packet[4] = CCREP_REJECT;
    memcpy(packet + 5, reason, reason_len);
    packet[5 + reason_len] = 0;
    (void)sendto(proxy->listen_fd, packet, len, 0,
                 (const struct sockaddr *)client, sizeof(*client));
}

static void start_connection(struct proxy *proxy,
                             const struct sockaddr_in *client,
                             const uint8_t *packet, size_t packet_len,
                             double now)
{
    struct session *session = find_client_session(proxy, client);
    char address[64];

    if (packet_len > sizeof(session->connect_request)) {
        send_control_reject(proxy, client, "Connection request is too large.\n");
        return;
    }

    if (session) {
        if (now - session->created_at < 2.0) {
            if (session->phase == SESSION_PENDING)
                (void)send(session->upstream_fd, session->connect_request,
                           session->connect_request_len, 0);
            else if (session->accept_reply_len)
                (void)sendto(proxy->listen_fd, session->accept_reply,
                             session->accept_reply_len, 0,
                             (const struct sockaddr *)client,
                             sizeof(*client));
            return;
        }
        if (session->phase == SESSION_ACTIVE) {
            uint8_t disconnect = CLC_DISCONNECT;
            (void)nq_chan_send_unreliable(&session->upstream_chan,
                                          &disconnect, 1);
        }
        session_close(session);
    }

    session = allocate_session(proxy);
    if (!session) {
        send_control_reject(proxy, client, "Proxy is full.\n");
        return;
    }
    session->upstream_fd = open_upstream_socket(&proxy->upstream_address);
    if (session->upstream_fd < 0) {
        send_control_reject(proxy, client, "Proxy upstream error.\n");
        session_close(session);
        return;
    }

    session->phase = SESSION_PENDING;
    session->client_address = *client;
    session->upstream_address = proxy->upstream_address;
    session->created_at = now;
    session->last_activity = now;
    session->connect_request_len = packet_len;
    memcpy(session->connect_request, packet, packet_len);

    session->client_target.fd = proxy->listen_fd;
    session->client_target.connected = false;
    session->client_target.address = *client;
    session->upstream_target.fd = session->upstream_fd;
    session->upstream_target.connected = true;
    session->upstream_target.address = proxy->upstream_address;
    nq_chan_init(&session->client_chan, NQ_LEGACY_DATAGRAM_MAX,
                 send_packet, &session->client_target);
    nq_chan_init(&session->upstream_chan, 1442, send_packet,
                 &session->upstream_target);
    nq_xlat_init(&session->xlat, false);

    (void)send(session->upstream_fd, packet, packet_len, 0);
    if (proxy->verbose)
        fprintf(stderr, "connect %s\n",
                address_string(client, address, sizeof(address)));
}

static void start_query(struct proxy *proxy,
                        const struct sockaddr_in *client,
                        const uint8_t *packet, size_t packet_len,
                        double now)
{
    struct session *session = allocate_session(proxy);
    if (!session)
        return;
    session->upstream_fd = open_upstream_socket(&proxy->upstream_address);
    if (session->upstream_fd < 0) {
        session_close(session);
        return;
    }
    session->phase = SESSION_QUERY;
    session->client_address = *client;
    session->created_at = now;
    session->last_activity = now;
    (void)send(session->upstream_fd, packet, packet_len, 0);
}

static void handle_frontend_control(struct proxy *proxy,
                                    const struct sockaddr_in *client,
                                    const uint8_t *packet, size_t packet_len,
                                    double now)
{
    if (!nq_packet_is_control(packet, packet_len))
        return;
    if (packet[4] == CCREQ_CONNECT) {
        if (!valid_connect_request(packet, packet_len)) {
            send_control_reject(proxy, client, "Bad connection request.\n");
            return;
        }
        start_connection(proxy, client, packet, packet_len, now);
    } else {
        start_query(proxy, client, packet, packet_len, now);
    }
}

static bool rewrite_server_info(const struct proxy *proxy,
                                const uint8_t *input, size_t input_len,
                                uint8_t *output, size_t *output_len)
{
    const uint8_t *nul;
    size_t old_string_len;
    size_t new_string_len;
    size_t suffix_len;
    size_t len;

    if (!proxy->advertise[0] || input_len < 6 ||
        input[4] != CCREP_SERVER_INFO) {
        memcpy(output, input, input_len);
        *output_len = input_len;
        return true;
    }
    nul = memchr(input + 5, 0, input_len - 5);
    if (!nul)
        return false;
    old_string_len = (size_t)(nul - (input + 5)) + 1;
    new_string_len = strlen(proxy->advertise) + 1;
    suffix_len = input_len - 5 - old_string_len;
    len = 5 + new_string_len + suffix_len;
    if (len > 65535u)
        return false;
    write_be32(output, NQ_NETFLAG_CTL | (uint32_t)len);
    output[4] = input[4];
    memcpy(output + 5, proxy->advertise, new_string_len);
    memcpy(output + 5 + new_string_len,
           input + 5 + old_string_len, suffix_len);
    *output_len = len;
    return true;
}

static void handle_pending_upstream(struct proxy *proxy,
                                    struct session *session,
                                    const uint8_t *packet, size_t packet_len,
                                    double now)
{
    uint8_t reply[65535];
    uint32_t accepted_port;
    bool ignore_port = false;
    bool client_angle16 = false;

    if (!nq_packet_is_control(packet, packet_len))
        return;
    if (packet[4] == CCREP_REJECT) {
        (void)sendto(proxy->listen_fd, packet, packet_len, 0,
                     (const struct sockaddr *)&session->client_address,
                     sizeof(session->client_address));
        session_close(session);
        return;
    }
    if (packet[4] != CCREP_ACCEPT || packet_len < 9)
        return;
    if (packet_len > sizeof(session->accept_reply)) {
        send_control_reject(proxy, &session->client_address,
                            "Upstream accept packet is too large.\n");
        session_close(session);
        return;
    }

    accepted_port = read_le32(packet + 5);
    if (packet_len >= 12 && packet[9] == MOD_PROQUAKE) {
        ignore_port = (packet[11] & PQF_IGNOREPORT) != 0;
        client_angle16 = true;
    }
    if (accepted_port && !ignore_port) {
        session->upstream_address.sin_port = htons((uint16_t)accepted_port);
        if (connect(session->upstream_fd,
                    (const struct sockaddr *)&session->upstream_address,
                    sizeof(session->upstream_address)) != 0) {
            send_control_reject(proxy, &session->client_address,
                                "Proxy could not switch upstream port.\n");
            session_close(session);
            return;
        }
    }

    memcpy(reply, packet, packet_len);
    write_le32(reply + 5, proxy->listen_port);
    memcpy(session->accept_reply, reply, packet_len);
    session->accept_reply_len = packet_len;
    (void)sendto(proxy->listen_fd, reply, packet_len, 0,
                 (const struct sockaddr *)&session->client_address,
                 sizeof(session->client_address));
    session->phase = SESSION_ACTIVE;
    session->last_activity = now;
    nq_xlat_init(&session->xlat, client_angle16);
}

static bool forward_batch(struct nq_chan *destination,
                          const struct nq_batch *batch, bool reliable,
                          double now)
{
    size_t i;
    for (i = 0; i < batch->count; i++) {
        bool ok = reliable ?
            nq_chan_queue_reliable(destination, batch->items[i].data,
                                   batch->items[i].len, now) :
            nq_chan_send_unreliable(destination, batch->items[i].data,
                                    batch->items[i].len);
        if (!ok)
            return false;
    }
    return true;
}

static void translate_client_packet(struct proxy *proxy,
                                    struct session *session,
                                    const struct nq_received_message *message,
                                    double now)
{
    struct nq_batch batch;
    char error[256];
    bool reliable = message->kind == NQ_MESSAGE_RELIABLE;

    nq_batch_init(&batch);
    if (!nq_translate_client_message(&session->xlat,
                                     message->data, message->len, reliable,
                                     &batch, error, sizeof(error))) {
        fprintf(stderr, "legacy client message dropped: %s\n", error);
        session->translation_errors++;
    } else if (!forward_batch(&session->upstream_chan, &batch, reliable, now)) {
        fprintf(stderr, "upstream queue overflow\n");
        session->translation_errors += 5;
    }
    nq_batch_free(&batch);
    if (session->translation_errors >= 5) {
        if (proxy->verbose)
            fprintf(stderr, "closing client after translation errors\n");
        session_close(session);
    }
}

static void translate_server_packet(struct proxy *proxy,
                                    struct session *session,
                                    const struct nq_received_message *message,
                                    double now)
{
    struct nq_batch batch;
    char error[256];
    bool reliable = message->kind == NQ_MESSAGE_RELIABLE;

    nq_batch_init(&batch);
    if (!nq_translate_server_message(&session->xlat,
                                     message->data, message->len, reliable,
                                     &batch, error, sizeof(error))) {
        fprintf(stderr, "protocol-666 server message dropped: %s\n", error);
        session->translation_errors++;
    } else if (!forward_batch(&session->client_chan, &batch, reliable, now)) {
        fprintf(stderr, "legacy client queue overflow\n");
        session->translation_errors += 5;
    }
    nq_batch_free(&batch);
    if (session->translation_errors >= 5) {
        if (proxy->verbose)
            fprintf(stderr, "closing client after translation errors\n");
        session_close(session);
    }
}

static void handle_frontend_game(struct proxy *proxy,
                                 struct session *session,
                                 const uint8_t *packet, size_t packet_len,
                                 double now)
{
    struct nq_received_message message;
    session->last_activity = now;
    if (!nq_chan_receive(&session->client_chan, packet, packet_len,
                         now, &message)) {
        if (proxy->verbose)
            fprintf(stderr, "malformed legacy datagram dropped\n");
        return;
    }
    if (message.kind != NQ_MESSAGE_NONE)
        translate_client_packet(proxy, session, &message, now);
}

static void handle_active_upstream(struct proxy *proxy,
                                   struct session *session,
                                   const uint8_t *packet, size_t packet_len,
                                   double now)
{
    struct nq_received_message message;
    session->last_activity = now;
    if (nq_packet_is_control(packet, packet_len))
        return;
    if (!nq_chan_receive(&session->upstream_chan, packet, packet_len,
                         now, &message)) {
        if (proxy->verbose)
            fprintf(stderr, "malformed upstream datagram dropped\n");
        return;
    }
    if (message.kind != NQ_MESSAGE_NONE)
        translate_server_packet(proxy, session, &message, now);
}

static void receive_frontend(struct proxy *proxy, double now)
{
    uint8_t packet[65535];

    for (;;) {
        struct sockaddr_in client;
        socklen_t client_len = sizeof(client);
        ssize_t received = recvfrom(proxy->listen_fd, packet, sizeof(packet), 0,
                                    (struct sockaddr *)&client, &client_len);
        struct session *session;

        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            perror("recvfrom");
            return;
        }
        if (client_len != sizeof(client) || received < 4)
            continue;
        if (nq_packet_is_control(packet, (size_t)received)) {
            handle_frontend_control(proxy, &client, packet,
                                    (size_t)received, now);
            continue;
        }
        session = find_client_session(proxy, &client);
        if (session && session->phase == SESSION_ACTIVE)
            handle_frontend_game(proxy, session, packet,
                                 (size_t)received, now);
    }
}

static void receive_upstream(struct proxy *proxy, struct session *session,
                             double now)
{
    uint8_t packet[65535];

    for (;;) {
        ssize_t received = recv(session->upstream_fd, packet, sizeof(packet), 0);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            session_close(session);
            return;
        }
        if (received == 0)
            return;

        if (session->phase == SESSION_PENDING) {
            handle_pending_upstream(proxy, session, packet,
                                    (size_t)received, now);
        } else if (session->phase == SESSION_QUERY) {
            uint8_t reply[65535];
            size_t reply_len;
            if (nq_packet_is_control(packet, (size_t)received) &&
                rewrite_server_info(proxy, packet, (size_t)received,
                                    reply, &reply_len))
                (void)sendto(proxy->listen_fd, reply, reply_len, 0,
                             (const struct sockaddr *)&session->client_address,
                             sizeof(session->client_address));
            session_close(session);
        } else if (session->phase == SESSION_ACTIVE) {
            handle_active_upstream(proxy, session, packet,
                                   (size_t)received, now);
        }
        if (session->phase == SESSION_FREE)
            return;
    }
}

static void pump_and_expire(struct proxy *proxy, double now)
{
    size_t i;
    for (i = 0; i < proxy->max_sessions; i++) {
        struct session *session = &proxy->sessions[i];
        double timeout;
        if (session->phase == SESSION_FREE)
            continue;
        timeout = session->phase == SESSION_QUERY ? QUERY_TIMEOUT :
                  session->phase == SESSION_PENDING ? HANDSHAKE_TIMEOUT :
                  SESSION_TIMEOUT;
        if (now - session->last_activity > timeout) {
            session_close(session);
            continue;
        }
        if (session->phase == SESSION_ACTIVE) {
            nq_chan_pump(&session->client_chan, now);
            nq_chan_pump(&session->upstream_chan, now);
        }
    }
}

static int run_proxy(struct proxy *proxy)
{
    while (!stop_requested) {
        fd_set read_fds;
        struct timeval timeout = {0, 50000};
        int max_fd = proxy->listen_fd;
        int ready;
        size_t i;
        double now;

        FD_ZERO(&read_fds);
        FD_SET(proxy->listen_fd, &read_fds);
        for (i = 0; i < proxy->max_sessions; i++) {
            struct session *session = &proxy->sessions[i];
            if (session->phase != SESSION_FREE) {
                FD_SET(session->upstream_fd, &read_fds);
                if (session->upstream_fd > max_fd)
                    max_fd = session->upstream_fd;
            }
        }

        ready = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
        now = monotonic_seconds();
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            perror("select");
            return 1;
        }
        if (FD_ISSET(proxy->listen_fd, &read_fds))
            receive_frontend(proxy, now);
        for (i = 0; i < proxy->max_sessions; i++) {
            struct session *session = &proxy->sessions[i];
            if (session->phase != SESSION_FREE &&
                FD_ISSET(session->upstream_fd, &read_fds))
                receive_upstream(proxy, session, now);
        }
        pump_and_expire(proxy, now);
    }
    return 0;
}

static void usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "Usage: %s --server HOST[:PORT] [options]\n"
            "\n"
            "Options:\n"
            "  -s, --server ADDR       QSS-M protocol-666 server\n"
            "  -l, --listen ADDR       Legacy listener (default %s)\n"
            "  -a, --advertise ADDR    Address placed in server-browser replies\n"
            "  -m, --max-clients N     Proxy session/query slots (default %d)\n"
            "  -v, --verbose           Log connections and malformed packets\n"
            "  -h, --help              Show this help\n",
            program, DEFAULT_LISTEN, DEFAULT_MAX_SESSIONS);
}

int main(int argc, char **argv)
{
    const char *listen_text = DEFAULT_LISTEN;
    const char *server_text = NULL;
    const char *advertise_text = NULL;
    size_t max_sessions = DEFAULT_MAX_SESSIONS;
    struct proxy proxy;
    int reuse = 1;
    int i;
    char listen_display[64];
    char server_display[64];
    int result;

    memset(&proxy, 0, sizeof(proxy));
    proxy.listen_fd = -1;
    for (i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-s") || !strcmp(argv[i], "--server")) &&
            i + 1 < argc) {
            server_text = argv[++i];
        } else if ((!strcmp(argv[i], "-l") || !strcmp(argv[i], "--listen")) &&
                   i + 1 < argc) {
            listen_text = argv[++i];
        } else if ((!strcmp(argv[i], "-a") || !strcmp(argv[i], "--advertise")) &&
                   i + 1 < argc) {
            advertise_text = argv[++i];
        } else if ((!strcmp(argv[i], "-m") || !strcmp(argv[i], "--max-clients")) &&
                   i + 1 < argc) {
            char *end = NULL;
            unsigned long value = strtoul(argv[++i], &end, 10);
            if (!end || *end || value < 1 || value > MAX_SESSIONS_LIMIT) {
                fprintf(stderr, "invalid --max-clients value\n");
                return 2;
            }
            max_sessions = (size_t)value;
        } else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
            proxy.verbose = true;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(stdout, argv[0]);
            return 0;
        } else {
            usage(stderr, argv[0]);
            return 2;
        }
    }
    if (!server_text) {
        usage(stderr, argv[0]);
        return 2;
    }
    if (!resolve_endpoint(listen_text, true, "26001", &proxy.listen_address) ||
        !resolve_endpoint(server_text, false, DEFAULT_PORT,
                          &proxy.upstream_address))
        return 2;
    if (advertise_text) {
        if (strlen(advertise_text) >= sizeof(proxy.advertise)) {
            fprintf(stderr, "--advertise value is too long\n");
            return 2;
        }
        (void)snprintf(proxy.advertise, sizeof(proxy.advertise), "%s",
                       advertise_text);
    }

    proxy.listen_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (proxy.listen_fd < 0) {
        perror("socket");
        return 1;
    }
    (void)setsockopt(proxy.listen_fd, SOL_SOCKET, SO_REUSEADDR,
                     &reuse, sizeof(reuse));
    if (bind(proxy.listen_fd, (const struct sockaddr *)&proxy.listen_address,
             sizeof(proxy.listen_address)) != 0 ||
        !set_nonblocking(proxy.listen_fd)) {
        perror("bind/nonblocking");
        close(proxy.listen_fd);
        return 1;
    }
    {
        struct sockaddr_in actual;
        socklen_t actual_len = sizeof(actual);
        if (getsockname(proxy.listen_fd, (struct sockaddr *)&actual,
                        &actual_len) != 0) {
            perror("getsockname");
            close(proxy.listen_fd);
            return 1;
        }
        proxy.listen_port = ntohs(actual.sin_port);
        proxy.listen_address.sin_port = actual.sin_port;
    }

    proxy.sessions = calloc(max_sessions, sizeof(*proxy.sessions));
    if (!proxy.sessions) {
        perror("calloc");
        close(proxy.listen_fd);
        return 1;
    }
    proxy.max_sessions = max_sessions;
    for (i = 0; i < (int)max_sessions; i++)
        proxy.sessions[i].upstream_fd = -1;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    fprintf(stderr, "nq666-proxy listening on %s, upstream %s\n",
            address_string(&proxy.listen_address, listen_display,
                           sizeof(listen_display)),
            address_string(&proxy.upstream_address, server_display,
                           sizeof(server_display)));
    if (!proxy.advertise[0])
        fprintf(stderr,
                "note: direct connects work; use --advertise for server-browser connects\n");

    result = run_proxy(&proxy);
    for (i = 0; i < (int)max_sessions; i++)
        session_close(&proxy.sessions[i]);
    free(proxy.sessions);
    close(proxy.listen_fd);
    return result;
}
