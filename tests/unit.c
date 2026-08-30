/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "netchan.h"
#include "protocol.h"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif
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

static void put_be32(uint8_t *p, uint32_t value)
{
    value = htonl(value);
    memcpy(p, &value, 4);
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
    CHECK(state.serverinfo_generation == 1);
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

static bool test_proquake_angles_preserved(void)
{
    uint8_t input[32] = {3};
    struct nq_xlat_state state;
    struct nq_batch batch;
    char error[128];

    input[5] = 0x34; input[6] = 0x12;
    input[7] = 0x78; input[8] = 0x56;
    input[9] = 0xbc; input[10] = 0x9a;
    nq_xlat_init(&state, true);
    nq_batch_init(&batch);
    CHECK(nq_translate_client_message(&state, input, 19, false,
                                      &batch, error, sizeof(error)));
    CHECK(batch.count == 1 && batch.items[0].len == sizeof(input) - 13);
    CHECK(batch.items[0].data[5] == 0x34 && batch.items[0].data[6] == 0x12);
    CHECK(batch.items[0].data[7] == 0x78 && batch.items[0].data[8] == 0x56);
    CHECK(batch.items[0].data[9] == 0xbc && batch.items[0].data[10] == 0x9a);
    nq_batch_free(&batch);
    return true;
}

static bool test_pext_is_sanitized(void)
{
    static const uint8_t input[] = {
        4, 'P','E','X','T',' ', '1','2','3',' ', '4','5','6', 0
    };
    static const uint8_t expected[] = {4, 'p','e','x','t', 0};
    struct nq_xlat_state state;
    struct nq_batch batch;
    char error[128];

    nq_xlat_init(&state, false);
    nq_batch_init(&batch);
    CHECK(nq_translate_client_message(&state, input, sizeof(input), true,
                                      &batch, error, sizeof(error)));
    CHECK(batch.count == 1 && batch.items[0].len == sizeof(expected));
    CHECK(memcmp(batch.items[0].data, expected, sizeof(expected)) == 0);
    nq_batch_free(&batch);

    {
        static const uint8_t chained[] = {
            4, 'n','a','m','e',' ','x',';', ' ', 'p','e','x','t',' ', '9', 0
        };
        nq_batch_init(&batch);
        CHECK(nq_translate_client_message(&state, chained, sizeof(chained),
                                          true, &batch, error, sizeof(error)));
        CHECK(batch.count == 1 && batch.items[0].len == sizeof(expected));
        CHECK(memcmp(batch.items[0].data, expected, sizeof(expected)) == 0);
        nq_batch_free(&batch);
    }
    return true;
}

static bool test_download_extension_stufftext_is_filtered(void)
{
    static const uint8_t input[] = {
        9, 'c','l','_','s','e','r','v','e','r','e','x','t','e','n','s','i','o','n','_',
        'd','o','w','n','l','o','a','d',' ','1','\n', 0,
        1
    };
    struct nq_xlat_state state;
    struct nq_batch batch;
    char error[128];

    nq_xlat_init(&state, false);
    nq_batch_init(&batch);
    CHECK(nq_translate_server_message(&state, input, sizeof(input), true,
                                      &batch, error, sizeof(error)));
    CHECK(batch.count == 1 && batch.items[0].len == 1);
    CHECK(batch.items[0].data[0] == 1);
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

static bool test_clientdata_hides_unavailable_weapon_model(void)
{
    enum {
        SU_WEAPON = 1 << 14,
        SU_EXTEND1 = 1 << 15,
        SU_WEAPON2 = 1 << 16
    };
    uint8_t input[32];
    uint8_t *p = input;
    struct nq_xlat_state state;
    struct nq_batch batch;
    char error[128];
    const uint8_t *out;

    *p++ = 15;
    put_u16(&p, SU_WEAPON | SU_EXTEND1);
    *p++ = (uint8_t)(SU_WEAPON2 >> 16);
    put_u32(&p, 0); /* items */
    *p++ = 2;       /* weapon model low: must not become model 2 */
    put_u16(&p, 100);
    *p++ = 10;
    *p++ = 20;
    *p++ = 30;
    *p++ = 40;
    *p++ = 50;
    *p++ = 1;
    *p++ = 1;       /* weapon model high: full index 258 */

    nq_xlat_init(&state, false);
    state.models_exposed = 3;
    nq_batch_init(&batch);
    CHECK(nq_translate_server_message(&state, input, (size_t)(p - input),
                                      false, &batch, error, sizeof(error)));
    CHECK(batch.count == 1 && batch.items[0].len == 16);
    out = batch.items[0].data;
    CHECK(out[0] == 15);
    CHECK(out[1] == 0 && out[2] == 0x40);
    CHECK(out[7] == 0);
    nq_batch_free(&batch);
    return true;
}

static bool test_setview_entity_limit(void)
{
    uint8_t input[8];
    uint8_t *p = input;
    struct nq_xlat_state state;
    struct nq_batch batch;
    char error[128];
    const uint8_t expected[] = {5, 0x57, 0x02, 1};

    *p++ = 5;
    put_u16(&p, 599);
    *p++ = 5;
    put_u16(&p, 600);
    *p++ = 1;

    nq_xlat_init(&state, false);
    nq_batch_init(&batch);
    CHECK(nq_translate_server_message(&state, input, (size_t)(p - input),
                                      true, &batch, error, sizeof(error)));
    CHECK(batch.count == 1 && batch.items[0].len == sizeof(expected));
    CHECK(memcmp(batch.items[0].data, expected, sizeof(expected)) == 0);
    nq_batch_free(&batch);
    return true;
}

static bool test_colormap_scoreboard_limit(void)
{
    enum { U_MOREBITS = 1 << 0, U_COLORMAP = 1 << 11 };
    uint8_t input[32];
    uint8_t *p = input;
    struct nq_xlat_state state;
    struct nq_batch batch;
    char error[128];
    const uint8_t *out;
    int i;

    *p++ = 22;
    put_u16(&p, 17);
    *p++ = 1;
    *p++ = 0;
    *p++ = 5; /* baseline colormap exceeds four-player scoreboard */
    *p++ = 0;
    for (i = 0; i < 3; i++) {
        put_u16(&p, 0);
        *p++ = 0;
    }
    *p++ = (uint8_t)(0x80 | U_MOREBITS);
    *p++ = (uint8_t)(U_COLORMAP >> 8);
    *p++ = 17;
    *p++ = 5; /* delta colormap exceeds four-player scoreboard */

    nq_xlat_init(&state, false);
    state.max_scoreboard = 4;
    nq_batch_init(&batch);
    CHECK(nq_translate_server_message(&state, input, (size_t)(p - input),
                                      true, &batch, error, sizeof(error)));
    CHECK(batch.count == 1 && batch.items[0].len == 20);
    out = batch.items[0].data;
    CHECK(out[5] == 0);
    CHECK(out[19] == 0);
    nq_batch_free(&batch);
    return true;
}

static bool test_lightstyle_limit(void)
{
    static const uint8_t input[] = {
        12, 63, 'a', 0,
        12, 64, 'b', 0,
        1
    };
    static const uint8_t expected[] = {12, 63, 'a', 0, 1};
    struct nq_xlat_state state;
    struct nq_batch batch;
    char error[128];

    nq_xlat_init(&state, false);
    nq_batch_init(&batch);
    CHECK(nq_translate_server_message(&state, input, sizeof(input), true,
                                      &batch, error, sizeof(error)));
    CHECK(batch.count == 1 && batch.items[0].len == sizeof(expected));
    CHECK(memcmp(batch.items[0].data, expected, sizeof(expected)) == 0);
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

static int capture_send(void *opaque, const void *packet, size_t len)
{
    struct capture *capture = opaque;
    if (len > sizeof(capture->packet))
        return -1;
    memcpy(capture->packet, packet, len);
    capture->len = len;
    capture->sends++;
    return (int)len;
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
    nq_chan_init(&sender, 1024, NQ_MAX_RELIABLE_MESSAGE,
                 capture_send, &sent);
    nq_chan_init(&receiver, 1024, NQ_MAX_RELIABLE_MESSAGE,
                 capture_send, &ack);
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

static bool test_oversized_reliable_is_fully_discarded(void)
{
    static uint8_t first[40008];
    static uint8_t overflow[30008];
    uint8_t end[9];
    uint8_t valid[9];
    struct nq_chan receiver;
    struct capture ack = {{0}, 0, 0};
    struct nq_received_message message;

    memset(first + 8, 0x11, sizeof(first) - 8);
    put_be32(first, NQ_NETFLAG_DATA | (uint32_t)sizeof(first));
    put_be32(first + 4, 0);
    memset(overflow + 8, 0x22, sizeof(overflow) - 8);
    put_be32(overflow, NQ_NETFLAG_DATA | (uint32_t)sizeof(overflow));
    put_be32(overflow + 4, 1);
    put_be32(end, NQ_NETFLAG_DATA | NQ_NETFLAG_EOM | (uint32_t)sizeof(end));
    put_be32(end + 4, 2);
    end[8] = 0x33;
    put_be32(valid, NQ_NETFLAG_DATA | NQ_NETFLAG_EOM |
                    (uint32_t)sizeof(valid));
    put_be32(valid + 4, 3);
    valid[8] = 0x44;

    nq_chan_init(&receiver, 1024, NQ_MAX_RELIABLE_MESSAGE,
                 capture_send, &ack);
    CHECK(nq_chan_receive(&receiver, first, sizeof(first), 1.0, &message));
    CHECK(message.kind == NQ_MESSAGE_NONE);
    CHECK(!nq_chan_receive(&receiver, overflow, sizeof(overflow),
                           1.1, &message));
    CHECK(receiver.receive_discarding);
    CHECK(nq_chan_receive(&receiver, end, sizeof(end), 1.2, &message));
    CHECK(message.kind == NQ_MESSAGE_NONE);
    CHECK(!receiver.receive_discarding);
    CHECK(nq_chan_receive(&receiver, valid, sizeof(valid), 1.3, &message));
    CHECK(message.kind == NQ_MESSAGE_RELIABLE && message.len == 1);
    CHECK(message.data[0] == 0x44);
    nq_chan_destroy(&receiver);
    return true;
}

static bool test_maximum_reliable_message(void)
{
    static uint8_t first[40008];
    static uint8_t last[25543];
    struct nq_chan receiver;
    struct capture ack = {{0}, 0, 0};
    struct nq_received_message message;

    memset(first + 8, 0x11, sizeof(first) - 8);
    put_be32(first, NQ_NETFLAG_DATA | (uint32_t)sizeof(first));
    put_be32(first + 4, 0);
    memset(last + 8, 0x22, sizeof(last) - 8);
    put_be32(last, NQ_NETFLAG_DATA | NQ_NETFLAG_EOM |
                   (uint32_t)sizeof(last));
    put_be32(last + 4, 1);

    nq_chan_init(&receiver, 1024, NQ_MAX_RELIABLE_MESSAGE,
                 capture_send, &ack);
    CHECK(nq_chan_receive(&receiver, first, sizeof(first), 1.0, &message));
    CHECK(message.kind == NQ_MESSAGE_NONE);
    CHECK(nq_chan_receive(&receiver, last, sizeof(last), 1.1, &message));
    CHECK(message.kind == NQ_MESSAGE_RELIABLE);
    CHECK(message.len == NQ_MAX_RELIABLE_MESSAGE);
    CHECK(message.data[0] == 0x11);
    CHECK(message.data[39999] == 0x11);
    CHECK(message.data[40000] == 0x22);
    CHECK(message.data[NQ_MAX_RELIABLE_MESSAGE - 1] == 0x22);
    nq_chan_destroy(&receiver);
    return true;
}

static bool test_legacy_reliable_message_limit(void)
{
    static uint8_t exact[NQ_LEGACY_RELIABLE_MAX];
    static uint8_t oversized[NQ_LEGACY_RELIABLE_MAX + 1];
    static uint8_t exact_packet[NQ_NET_HEADERSIZE +
                                NQ_LEGACY_RELIABLE_MAX];
    static uint8_t oversized_packet[NQ_NET_HEADERSIZE +
                                    NQ_LEGACY_RELIABLE_MAX + 1];
    uint8_t valid_packet[NQ_NET_HEADERSIZE + 1];
    struct nq_chan sender;
    struct nq_chan receiver;
    struct capture sent = {{0}, 0, 0};
    struct capture ack = {{0}, 0, 0};
    struct nq_received_message message;

    nq_chan_init(&sender, 1024, NQ_LEGACY_RELIABLE_MAX,
                 capture_send, &sent);
    CHECK(nq_chan_queue_reliable(&sender, exact, sizeof(exact), 1.0));
    CHECK(!nq_chan_queue_reliable(&sender, oversized, sizeof(oversized), 1.0));
    nq_chan_destroy(&sender);

    put_be32(exact_packet, NQ_NETFLAG_DATA | NQ_NETFLAG_EOM |
                           (uint32_t)sizeof(exact_packet));
    put_be32(exact_packet + 4, 0);
    put_be32(oversized_packet, NQ_NETFLAG_DATA | NQ_NETFLAG_EOM |
                               (uint32_t)sizeof(oversized_packet));
    put_be32(oversized_packet + 4, 1);
    put_be32(valid_packet, NQ_NETFLAG_DATA | NQ_NETFLAG_EOM |
                           (uint32_t)sizeof(valid_packet));
    put_be32(valid_packet + 4, 2);
    valid_packet[8] = 0x44;

    nq_chan_init(&receiver, 1024, NQ_LEGACY_RELIABLE_MAX,
                 capture_send, &ack);
    CHECK(nq_chan_receive(&receiver, exact_packet, sizeof(exact_packet),
                          1.0, &message));
    CHECK(message.kind == NQ_MESSAGE_RELIABLE &&
          message.len == NQ_LEGACY_RELIABLE_MAX);
    CHECK(!nq_chan_receive(&receiver, oversized_packet,
                           sizeof(oversized_packet), 1.1, &message));
    CHECK(message.kind == NQ_MESSAGE_NONE && !receiver.receive_discarding);
    CHECK(nq_chan_receive(&receiver, valid_packet, sizeof(valid_packet),
                          1.2, &message));
    CHECK(message.kind == NQ_MESSAGE_RELIABLE && message.len == 1);
    CHECK(message.data[0] == 0x44);
    nq_chan_destroy(&receiver);
    return true;
}

static bool test_stop_sound_entity_limit(void)
{
    uint8_t input[3] = {16, 0, 0};
    struct nq_xlat_state state;
    struct nq_batch batch;
    char error[128];
    uint16_t packed = (uint16_t)(600u << 3);

    input[1] = (uint8_t)packed;
    input[2] = (uint8_t)(packed >> 8);
    nq_xlat_init(&state, false);
    nq_batch_init(&batch);
    CHECK(nq_translate_server_message(&state, input, sizeof(input), false,
                                      &batch, error, sizeof(error)));
    CHECK(batch.count == 0);
    nq_batch_free(&batch);
    return true;
}

static bool test_failed_message_preserves_state(void)
{
    static const uint8_t truncated_serverinfo[] = {
        11, 0x9a, 0x02, 0, 0, 4, 1, 'T', 'e', 's', 't', 0,
        'm', 'a', 'p', 's', '/', 'b', 'a', 'd'
    };
    struct nq_xlat_state state;
    struct nq_xlat_state original;
    struct nq_batch batch;
    char error[128];

    nq_xlat_init(&state, false);
    state.static_count = 7;
    original = state;
    nq_batch_init(&batch);
    CHECK(!nq_translate_server_message(&state, truncated_serverinfo,
                                       sizeof(truncated_serverinfo), true,
                                       &batch, error, sizeof(error)));
    CHECK(memcmp(&state, &original, sizeof(state)) == 0);
    CHECK(batch.count == 0);
    nq_batch_free(&batch);
    return true;
}

static uint32_t fuzz_random(uint32_t *state)
{
    uint32_t value = *state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

static bool test_malformed_packet_fuzz(void)
{
    uint8_t input[256];
    uint32_t random_state = 0x51a7e5u;
    unsigned int iteration;

    for (iteration = 0; iteration < 10000; iteration++) {
        struct nq_xlat_state state;
        struct nq_batch batch;
        char error[128];
        size_t len = fuzz_random(&random_state) % sizeof(input);
        size_t i;
        bool reliable = (fuzz_random(&random_state) & 1u) != 0;

        for (i = 0; i < len; i++)
            input[i] = (uint8_t)fuzz_random(&random_state);
        nq_xlat_init(&state, (iteration & 1u) != 0);
        nq_batch_init(&batch);
        (void)nq_translate_server_message(&state, input, len, reliable,
                                          &batch, error, sizeof(error));
        for (i = 0; i < batch.count; i++)
            CHECK(batch.items[i].len <= (reliable ?
                  NQ_LEGACY_RELIABLE_MAX : NQ_LEGACY_DATAGRAM_MAX));
        nq_batch_free(&batch);

        nq_batch_init(&batch);
        (void)nq_translate_client_message(&state, input, len, reliable,
                                          &batch, error, sizeof(error));
        for (i = 0; i < batch.count; i++)
            CHECK(batch.items[i].len <= (reliable ? 32000u : 1442u));
        nq_batch_free(&batch);
    }
    return true;
}

int main(void)
{
    bool (*tests[])(void) = {
        test_serverinfo,
        test_extended_update,
        test_move_angle_expansion,
        test_proquake_angles_preserved,
        test_pext_is_sanitized,
        test_download_extension_stufftext_is_filtered,
        test_baseline2,
        test_clientdata_hides_unavailable_weapon_model,
        test_setview_entity_limit,
        test_colormap_scoreboard_limit,
        test_lightstyle_limit,
        test_unreliable_chunking,
        test_reliable_fragmentation,
        test_oversized_reliable_is_fully_discarded,
        test_maximum_reliable_message,
        test_legacy_reliable_message_limit,
        test_stop_sound_entity_limit,
        test_failed_message_preserves_state,
        test_malformed_packet_fuzz
    };
    size_t i;
    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        if (!tests[i]())
            return 1;
    }
    printf("all %zu tests passed\n", i);
    return 0;
}
