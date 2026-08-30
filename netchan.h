#ifndef NQ666_NETCHAN_H
#define NQ666_NETCHAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define NQ_NETFLAG_LENGTH_MASK 0x0000ffffu
#define NQ_NETFLAG_DATA        0x00010000u
#define NQ_NETFLAG_ACK         0x00020000u
#define NQ_NETFLAG_NAK         0x00040000u
#define NQ_NETFLAG_EOM         0x00080000u
#define NQ_NETFLAG_UNRELIABLE  0x00100000u
#define NQ_NETFLAG_CTL         0x80000000u
#define NQ_NET_HEADERSIZE      8u
#define NQ_MAX_WIRE_PAYLOAD    (65535u - NQ_NET_HEADERSIZE)

typedef ssize_t (*nq_send_packet_fn)(void *opaque, const void *packet,
                                     size_t packet_len);

enum nq_message_kind {
    NQ_MESSAGE_NONE = 0,
    NQ_MESSAGE_RELIABLE,
    NQ_MESSAGE_UNRELIABLE
};

struct nq_received_message {
    enum nq_message_kind kind;
    const uint8_t *data;
    size_t len;
};

struct nq_queued_message;

struct nq_chan {
    uint32_t send_sequence;
    uint32_t unreliable_send_sequence;
    uint32_t receive_sequence;
    uint32_t unreliable_receive_sequence;

    uint8_t receive_message[NQ_MAX_WIRE_PAYLOAD];
    size_t receive_message_len;

    struct nq_queued_message *queue_head;
    struct nq_queued_message *queue_tail;
    size_t queued_bytes;
    size_t send_offset;
    size_t fragment_len;
    uint32_t fragment_sequence;
    double fragment_sent_at;
    bool fragment_waiting;

    size_t mss;
    size_t max_queue_bytes;
    nq_send_packet_fn send_packet;
    void *send_opaque;
};

void nq_chan_init(struct nq_chan *chan, size_t mss,
                  nq_send_packet_fn send_packet, void *send_opaque);
void nq_chan_destroy(struct nq_chan *chan);

bool nq_chan_queue_reliable(struct nq_chan *chan, const uint8_t *data,
                            size_t len, double now);
bool nq_chan_send_unreliable(struct nq_chan *chan, const uint8_t *data,
                             size_t len);

bool nq_chan_receive(struct nq_chan *chan, const uint8_t *packet,
                     size_t packet_len, double now,
                     struct nq_received_message *message);
void nq_chan_pump(struct nq_chan *chan, double now);

bool nq_packet_is_control(const uint8_t *packet, size_t packet_len);

#endif
