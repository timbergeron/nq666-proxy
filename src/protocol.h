/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NQ666_PROTOCOL_H
#define NQ666_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NQ_PROTOCOL_NETQUAKE 15
#define NQ_PROTOCOL_FITZQUAKE 666

#define NQ_LEGACY_RELIABLE_MAX 8192
#define NQ_LEGACY_DATAGRAM_MAX 1024
#define NQ_UPSTREAM_RELIABLE_MAX 32000

struct nq_blob {
    uint8_t *data;
    size_t len;
};

struct nq_batch {
    struct nq_blob *items;
    size_t count;
    size_t capacity;
};

struct nq_xlat_state {
    unsigned int max_entities;
    unsigned int max_scoreboard;
    unsigned int static_count;
    unsigned int models_exposed;
    unsigned int sounds_exposed;
    unsigned int upstream_protocol;
    unsigned int serverinfo_generation;
    bool client_angle16;
    bool warned_limits;
};

void nq_batch_init(struct nq_batch *batch);
void nq_batch_free(struct nq_batch *batch);

void nq_xlat_init(struct nq_xlat_state *state, bool client_angle16);

bool nq_translate_server_message(struct nq_xlat_state *state,
                                 const uint8_t *input, size_t input_len,
                                 bool reliable, struct nq_batch *output,
                                 char *error, size_t error_size);

bool nq_translate_client_message(const struct nq_xlat_state *state,
                                 const uint8_t *input, size_t input_len,
                                 bool reliable, struct nq_batch *output,
                                 char *error, size_t error_size);

#endif
