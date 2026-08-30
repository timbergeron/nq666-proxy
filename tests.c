#include "netchan.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

static void put_u16(uint8_t **p, uint16_t value)
{
    *(*p)++ = (uint8_t)value;
    *(*p)++ = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t **p, uint32_t value)
{
    *(*p)++ = (uint8_t)value;
    *(*p)++ = (uint8_t)(value >> 8);
    *(*p)++ = (uint8_t)(value >> 16);
    *(*p)++ = (uint8_t)(value >> 24);
}

static void put_string(uint8_t **p, const char *string)
{
    size_t len = strlen(string) + 1;
    memcpy(*p, string, len);
    *p += len;
}

static bool test_serverinfo(void)
{
    uint8_t input[256];
    uint8_t *p = input;
    struct nq_xlat_state state;
    struct nq_batch batch;
    char error[128];
    const uint8_t *out;

    *p++ = 11;
    put_u32(&p, 666);
    *p++ = 4;
    *p++ = 1;
    put_string(&p, "Test level");
    put_string(&p, "maps/test.bsp");
    put_string(&p, "progs/player.mdl");
    *p++ = 0;
    put_string(&p, "misc/talk.wav");
    *p++ = 0;
    *p++ = 25;
    *p++ = 1;

    nq_xlat_init(&state, false);
    nq_batch_init(&batch);
    CHECK(nq_translate_server_message(&state, input, (size_t)(p - input),
                                      true, &batch, error, sizeof(error)));
    CHECK(batch.count == 1);
    out = batch.items[0].data;
    CHECK(batch.items[0].len == (size_t)(p - input));
    CHECK(out[0] == 11);
    CHECK(out[1] == 15 && out[2] == 0 && out[3] == 0 && out[4] == 0);
    CHECK(out[5] == 4 && out[6] == 1);
    CHECK(state.upstream_protocol == 666);
    CHECK(state.models_exposed == 3);
    CHECK(state.sounds_exposed == 2);
    nq_batch_free(&batch);
    return true;
}

static bool test_extended_update(void)
{
    enum {
        U_MOREBITS = 1 << 0, U_ORIGIN1 = 1 << 1, U_FRAME = 1 << 6,
        U_MODEL = 1 << 10, U_EXTEND1 = 1 << 15,
        U_ALPHA = 1 << 16, U_MODEL2 = 1 << 18
    };
    uint32_t bits = U_MOREBITS | U_ORIGIN1 | U_FRAME | U_MODEL |
                    U_EXTEND1 | U_ALPHA | U_MODEL2;
    uint8_t input[32];
    uint8_t *p = input;
    struct nq_xlat_state state;
    struct nq_batch batch;
    char error[128];
    const uint8_t *out;

    *p++ = (uint8_t)(0x80 | (bits & 0x7f));
    *p++ = (uint8_t)(bits >> 8);
    *p++ = (uint8_t)(bits >> 16);
    *p++ = 5;       /* entity */
    *p++ = 2;       /* model low */
    *p++ = 3;       /* frame */
    put_u16(&p, 0x1234);
    *p++ = 128;     /* alpha, stripped */
    *p++ = 1;       /* model high: 258 becomes model 0 */

    nq_xlat_init(&state, false);
    nq_batch_init(&batch);
    CHECK(nq_translate_server_message(&state, input, (size_t)(p - input),
                                      false, &batch, error, sizeof(error)));
    CHECK(batch.count == 1);
    out = batch.items[0].data;
    CHECK(batch.items[0].len == 7);
    CHECK(out[0] == (uint8_t)(0x80 | U_MOREBITS | U_ORIGIN1 | U_FRAME));
    CHECK(out[1] == (uint8_t)(U_MODEL >> 8));
    CHECK(out[2] == 5);
    CHECK(out[3] == 0);
    CHECK(out[4] == 3);
    CHECK(out[5] == 0x34 && out[6] == 0x12);
    nq_batch_free(&batch);
    return true;
}

static bool test_move_angle_expansion(void)
{
    uint8_t input[32] = {3};
    struct nq_xlat_state state;
    struct nq_batch batch;
    char error[128];
    const uint8_t *out;

    input[5] = 1;
    input[6] = 2;
    input[7] = 255;
    nq_xlat_init(&state, false);
    nq_batch_init(&batch);
    CHECK(nq_translate_client_message(&state, input, 16, false,
                                      &batch, error, sizeof(error)));
    CHECK(batch.count == 1 && batch.items[0].len == 19);
    out = batch.items[0].data;
    CHECK(out[5] == 0 && out[6] == 1);
    CHECK(out[7] == 0 && out[8] == 2);
    CHECK(out[9] == 0 && out[10] == 255);
    nq_batch_free(&batch);
    return true;
}

static bool test_baseline2(void)
{
    uint8_t input[64];
    uint8_t *p = input;
    struct nq_xlat_state state;
    struct nq_batch batch;
    char error[128];
    const uint8_t *out;
    int i;

    *p++ = 42;
    put_u16(&p, 17);
    *p++ = 1 | 2 | 4; /* large model, large frame, alpha */
    put_u16(&p, 300);
    put_u16(&p, 0x1234);
    *p++ = 0;
    *p++ = 0;
    for (i = 0; i < 3; i++) {
        put_u16(&p, (uint16_t)i);
        *p++ = (uint8_t)i;
    }
    *p++ = 100;

    nq_xlat_init(&state, false);
    nq_batch_init(&batch);
    CHECK(nq_translate_server_message(&state, input, (size_t)(p - input),
                                      true, &batch, error, sizeof(error)));
    CHECK(batch.count == 1 && batch.items[0].len == 16);
    out = batch.items[0].data;
    CHECK(out[0] == 22);
    CHECK(out[1] == 17 && out[2] == 0);
    CHECK(out[3] == 0);       /* unavailable model */
    CHECK(out[4] == 0x34);    /* frame truncates safely */
    nq_batch_free(&batch);
    return true;
}

static bool test_unreliable_chunking(void)
{
    uint8_t input[1500];
    struct nq_xlat_state state;
    struct nq_batch batch;
    char error[128];
    memset(input, 1, sizeof(input));
    nq_xlat_init(&state, false);
    nq_batch_init(&batch);
    CHECK(nq_translate_server_message(&state, input, sizeof(input), false,
                                      &batch, error, sizeof(error)));
    CHECK(batch.count == 2);
    CHECK(batch.items[0].len == 1024);
    CHECK(batch.items[1].len == 476);
    nq_batch_free(&batch);
    return true;
}

struct capture {
    uint8_t packet[2048];
    size_t len;
    unsigned int sends;
};

static ssize_t capture_send(void *opaque, const void *packet, size_t len)
{
    struct capture *capture = opaque;
    if (len > sizeof(capture->packet))
        return -1;
    memcpy(capture->packet, packet, len);
    capture->len = len;
    capture->sends++;
    return (ssize_t)len;
}

static bool test_reliable_fragmentation(void)
{
    struct nq_chan sender;
    struct nq_chan receiver;
    struct capture sent = {{0}, 0, 0};
    struct capture ack = {{0}, 0, 0};
    struct nq_received_message message;
    uint8_t payload[1500];
    size_t i;

    for (i = 0; i < sizeof(payload); i++)
        payload[i] = (uint8_t)i;
    nq_chan_init(&sender, 1024, capture_send, &sent);
    nq_chan_init(&receiver, 1024, capture_send, &ack);
    CHECK(nq_chan_queue_reliable(&sender, payload, sizeof(payload), 1.0));
    CHECK(sent.sends == 1 && sent.len == 1032);
    CHECK(nq_chan_receive(&receiver, sent.packet, sent.len, 1.0, &message));
    CHECK(message.kind == NQ_MESSAGE_NONE && ack.sends == 1);
    CHECK(nq_chan_receive(&sender, ack.packet, ack.len, 1.1, &message));
    CHECK(sent.sends == 2 && sent.len == 484);
    CHECK(nq_chan_receive(&receiver, sent.packet, sent.len, 1.1, &message));
    CHECK(message.kind == NQ_MESSAGE_RELIABLE);
    CHECK(message.len == sizeof(payload));
    CHECK(memcmp(message.data, payload, sizeof(payload)) == 0);
    CHECK(nq_chan_receive(&sender, ack.packet, ack.len, 1.2, &message));
    CHECK(sender.queue_head == NULL && sender.queued_bytes == 0);
    nq_chan_destroy(&sender);
    nq_chan_destroy(&receiver);
    return true;
}

int main(void)
{
    bool (*tests[])(void) = {
        test_serverinfo,
        test_extended_update,
        test_move_angle_expansion,
        test_baseline2,
        test_unreliable_chunking,
        test_reliable_fragmentation
    };
    size_t i;
    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        if (!tests[i]())
            return 1;
    }
    printf("all %zu tests passed\n", i);
    return 0;
}
