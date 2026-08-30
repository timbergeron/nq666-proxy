/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "netchan.h"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif
#include <stdlib.h>
#include <string.h>

struct nq_queued_message {
    struct nq_queued_message *next;
    size_t len;
    uint8_t data[];
};

static uint32_t read_be32(const uint8_t *p)
{
    uint32_t value;
    memcpy(&value, p, sizeof(value));
    return ntohl(value);
}

static void write_be32(uint8_t *p, uint32_t value)
{
    value = htonl(value);
    memcpy(p, &value, sizeof(value));
}

void nq_chan_init(struct nq_chan *chan, size_t mss,
                  nq_send_packet_fn send_packet, void *send_opaque)
{
    memset(chan, 0, sizeof(*chan));
    chan->mss = mss > NQ_MAX_WIRE_PAYLOAD ? NQ_MAX_WIRE_PAYLOAD : mss;
    if (!chan->mss)
        chan->mss = 1;
    chan->max_queue_bytes = 512u * 1024u;
    chan->send_packet = send_packet;
    chan->send_opaque = send_opaque;
}

void nq_chan_destroy(struct nq_chan *chan)
{
    struct nq_queued_message *message = chan->queue_head;
    while (message) {
        struct nq_queued_message *next = message->next;
        free(message);
        message = next;
    }
    chan->queue_head = NULL;
    chan->queue_tail = NULL;
    chan->queued_bytes = 0;
}

static void nq_chan_send_fragment(struct nq_chan *chan, double now)
{
    uint8_t packet[NQ_NET_HEADERSIZE + NQ_MAX_WIRE_PAYLOAD];
    struct nq_queued_message *message = chan->queue_head;
    size_t remaining;
    uint32_t flags;

    if (!message || chan->fragment_waiting || !chan->send_packet)
        return;

    remaining = message->len - chan->send_offset;
    chan->fragment_len = remaining < chan->mss ? remaining : chan->mss;
    flags = NQ_NETFLAG_DATA;
    if (chan->fragment_len == remaining)
        flags |= NQ_NETFLAG_EOM;

    chan->fragment_sequence = chan->send_sequence++;
    write_be32(packet, flags + (uint32_t)(NQ_NET_HEADERSIZE + chan->fragment_len));
    write_be32(packet + 4, chan->fragment_sequence);
    memcpy(packet + NQ_NET_HEADERSIZE, message->data + chan->send_offset,
           chan->fragment_len);
    (void)chan->send_packet(chan->send_opaque, packet,
                            NQ_NET_HEADERSIZE + chan->fragment_len);
    chan->fragment_sent_at = now;
    chan->fragment_waiting = true;
}

bool nq_chan_queue_reliable(struct nq_chan *chan, const uint8_t *data,
                            size_t len, double now)
{
    struct nq_queued_message *message;

    if (!len || len > NQ_MAX_RELIABLE_MESSAGE ||
        len > chan->max_queue_bytes - chan->queued_bytes)
        return false;

    message = malloc(sizeof(*message) + len);
    if (!message)
        return false;
    message->next = NULL;
    message->len = len;
    memcpy(message->data, data, len);

    if (chan->queue_tail)
        chan->queue_tail->next = message;
    else
        chan->queue_head = message;
    chan->queue_tail = message;
    chan->queued_bytes += len;
    nq_chan_send_fragment(chan, now);
    return true;
}

bool nq_chan_send_unreliable(struct nq_chan *chan, const uint8_t *data,
                             size_t len)
{
    uint8_t packet[NQ_NET_HEADERSIZE + NQ_MAX_WIRE_PAYLOAD];

    if (!len || len > chan->mss || len > NQ_MAX_WIRE_PAYLOAD ||
        !chan->send_packet)
        return false;

    write_be32(packet, NQ_NETFLAG_UNRELIABLE +
                        (uint32_t)(NQ_NET_HEADERSIZE + len));
    write_be32(packet + 4, chan->unreliable_send_sequence++);
    memcpy(packet + NQ_NET_HEADERSIZE, data, len);
    return chan->send_packet(chan->send_opaque, packet,
                             NQ_NET_HEADERSIZE + len) >= 0;
}

static void nq_chan_ack(struct nq_chan *chan, uint32_t sequence)
{
    uint8_t packet[NQ_NET_HEADERSIZE];
    write_be32(packet, NQ_NETFLAG_ACK + NQ_NET_HEADERSIZE);
    write_be32(packet + 4, sequence);
    (void)chan->send_packet(chan->send_opaque, packet, sizeof(packet));
}

static void nq_chan_accept_ack(struct nq_chan *chan, uint32_t sequence,
                               double now)
{
    struct nq_queued_message *message;

    if (!chan->fragment_waiting || sequence != chan->fragment_sequence)
        return;

    chan->fragment_waiting = false;
    chan->send_offset += chan->fragment_len;
    message = chan->queue_head;
    if (message && chan->send_offset == message->len) {
        chan->queue_head = message->next;
        if (!chan->queue_head)
            chan->queue_tail = NULL;
        chan->queued_bytes -= message->len;
        free(message);
        chan->send_offset = 0;
    }
    nq_chan_send_fragment(chan, now);
}

bool nq_chan_receive(struct nq_chan *chan, const uint8_t *packet,
                     size_t packet_len, double now,
                     struct nq_received_message *message)
{
    uint32_t header;
    uint32_t flags;
    uint32_t declared_len;
    uint32_t sequence;
    const uint8_t *payload;
    size_t payload_len;

    message->kind = NQ_MESSAGE_NONE;
    message->data = NULL;
    message->len = 0;

    if (packet_len < NQ_NET_HEADERSIZE || packet_len > 65535u)
        return false;
    header = read_be32(packet);
    declared_len = header & NQ_NETFLAG_LENGTH_MASK;
    flags = header & ~NQ_NETFLAG_LENGTH_MASK;
    if (declared_len != packet_len || (flags & NQ_NETFLAG_CTL))
        return false;

    sequence = read_be32(packet + 4);
    payload = packet + NQ_NET_HEADERSIZE;
    payload_len = packet_len - NQ_NET_HEADERSIZE;

    if (flags & NQ_NETFLAG_ACK) {
        if (flags != NQ_NETFLAG_ACK || payload_len != 0)
            return false;
        nq_chan_accept_ack(chan, sequence, now);
        return true;
    }

    if (flags & NQ_NETFLAG_UNRELIABLE) {
        if (flags != NQ_NETFLAG_UNRELIABLE)
            return false;
        if (sequence < chan->unreliable_receive_sequence)
            return true;
        chan->unreliable_receive_sequence = sequence + 1;
        message->kind = NQ_MESSAGE_UNRELIABLE;
        message->data = payload;
        message->len = payload_len;
        return true;
    }

    if (!(flags & NQ_NETFLAG_DATA) ||
        (flags & ~(NQ_NETFLAG_DATA | NQ_NETFLAG_EOM)) != 0)
        return false;

    nq_chan_ack(chan, sequence);
    if (sequence != chan->receive_sequence)
        return true;
    chan->receive_sequence++;

    if (chan->receive_discarding) {
        if (flags & NQ_NETFLAG_EOM)
            chan->receive_discarding = false;
        return true;
    }

    if (payload_len > sizeof(chan->receive_message) - chan->receive_message_len) {
        chan->receive_message_len = 0;
        chan->receive_discarding = (flags & NQ_NETFLAG_EOM) == 0;
        return false;
    }
    memcpy(chan->receive_message + chan->receive_message_len, payload,
           payload_len);
    chan->receive_message_len += payload_len;

    if (flags & NQ_NETFLAG_EOM) {
        message->kind = NQ_MESSAGE_RELIABLE;
        message->data = chan->receive_message;
        message->len = chan->receive_message_len;
        chan->receive_message_len = 0;
    }
    return true;
}

void nq_chan_pump(struct nq_chan *chan, double now)
{
    uint8_t packet[NQ_NET_HEADERSIZE + NQ_MAX_WIRE_PAYLOAD];
    struct nq_queued_message *message;
    size_t packet_len;
    uint32_t flags;

    if (!chan->fragment_waiting) {
        nq_chan_send_fragment(chan, now);
        return;
    }
    if (now - chan->fragment_sent_at < 1.0)
        return;

    message = chan->queue_head;
    if (!message)
        return;
    flags = NQ_NETFLAG_DATA;
    if (chan->send_offset + chan->fragment_len == message->len)
        flags |= NQ_NETFLAG_EOM;
    packet_len = NQ_NET_HEADERSIZE + chan->fragment_len;
    write_be32(packet, flags + (uint32_t)packet_len);
    write_be32(packet + 4, chan->fragment_sequence);
    memcpy(packet + NQ_NET_HEADERSIZE, message->data + chan->send_offset,
           chan->fragment_len);
    (void)chan->send_packet(chan->send_opaque, packet, packet_len);
    chan->fragment_sent_at = now;
}

bool nq_packet_is_control(const uint8_t *packet, size_t packet_len)
{
    uint32_t header;
    if (packet_len < 5 || packet_len > 65535u)
        return false;
    header = read_be32(packet);
    return (header & ~NQ_NETFLAG_LENGTH_MASK) == NQ_NETFLAG_CTL &&
           (header & NQ_NETFLAG_LENGTH_MASK) == packet_len;
}
