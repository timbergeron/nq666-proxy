/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "protocol.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SVC_NOP = 1,
    SVC_DISCONNECT = 2,
    SVC_UPDATESTAT = 3,
    SVC_VERSION = 4,
    SVC_SETVIEW = 5,
    SVC_SOUND = 6,
    SVC_TIME = 7,
    SVC_PRINT = 8,
    SVC_STUFFTEXT = 9,
    SVC_SETANGLE = 10,
    SVC_SERVERINFO = 11,
    SVC_LIGHTSTYLE = 12,
    SVC_UPDATENAME = 13,
    SVC_UPDATEFRAGS = 14,
    SVC_CLIENTDATA = 15,
    SVC_STOPSOUND = 16,
    SVC_UPDATECOLORS = 17,
    SVC_PARTICLE = 18,
    SVC_DAMAGE = 19,
    SVC_SPAWNSTATIC = 20,
    SVC_SPAWNBASELINE = 22,
    SVC_TEMP_ENTITY = 23,
    SVC_SETPAUSE = 24,
    SVC_SIGNONNUM = 25,
    SVC_CENTERPRINT = 26,
    SVC_KILLEDMONSTER = 27,
    SVC_FOUNDSECRET = 28,
    SVC_SPAWNSTATICSOUND = 29,
    SVC_INTERMISSION = 30,
    SVC_FINALE = 31,
    SVC_CDTRACK = 32,
    SVC_SELLSCREEN = 33,
    SVC_CUTSCENE = 34,
    SVC_DP_SHOWPIC = 35,
    SVC_DP_HIDEPIC = 36,
    SVC_SKYBOX = 37,
    SVC_BF = 40,
    SVC_FOG = 41,
    SVC_SPAWNBASELINE2 = 42,
    SVC_SPAWNSTATIC2 = 43,
    SVC_SPAWNSTATICSOUND2 = 44,
    SVC_ACHIEVEMENT = 52
};

enum {
    CLC_NOP = 1,
    CLC_DISCONNECT = 2,
    CLC_MOVE = 3,
    CLC_STRINGCMD = 4
};

enum {
    U_MOREBITS = 1u << 0,
    U_ORIGIN1 = 1u << 1,
    U_ORIGIN2 = 1u << 2,
    U_ORIGIN3 = 1u << 3,
    U_ANGLE2 = 1u << 4,
    U_FRAME = 1u << 6,
    U_SIGNAL = 1u << 7,
    U_ANGLE1 = 1u << 8,
    U_ANGLE3 = 1u << 9,
    U_MODEL = 1u << 10,
    U_COLORMAP = 1u << 11,
    U_SKIN = 1u << 12,
    U_EFFECTS = 1u << 13,
    U_LONGENTITY = 1u << 14,
    U_EXTEND1 = 1u << 15,
    U_ALPHA = 1u << 16,
    U_FRAME2 = 1u << 17,
    U_MODEL2 = 1u << 18,
    U_LERPFINISH = 1u << 19,
    U_SCALE = 1u << 20,
    U_EXTEND2 = 1u << 23
};

enum {
    SU_EXTEND1 = 1u << 15,
    SU_WEAPON2 = 1u << 16,
    SU_ARMOR2 = 1u << 17,
    SU_AMMO2 = 1u << 18,
    SU_SHELLS2 = 1u << 19,
    SU_NAILS2 = 1u << 20,
    SU_ROCKETS2 = 1u << 21,
    SU_CELLS2 = 1u << 22,
    SU_EXTEND2 = 1u << 23,
    SU_WEAPONFRAME2 = 1u << 24,
    SU_WEAPONALPHA = 1u << 25
};

enum {
    B_LARGEMODEL = 1u << 0,
    B_LARGEFRAME = 1u << 1,
    B_ALPHA = 1u << 2,
    B_SCALE = 1u << 3
};

enum {
    SND_VOLUME = 1u << 0,
    SND_ATTENUATION = 1u << 1,
    SND_FTE_MOREFLAGS = 1u << 2,
    SND_LARGEENTITY = 1u << 3,
    SND_LARGESOUND = 1u << 4
};

#define PROTOCOL_FTE_PEXT1 ((uint32_t)'F' | ((uint32_t)'T' << 8) | \
                            ((uint32_t)'E' << 16) | ((uint32_t)'X' << 24))
#define PROTOCOL_FTE_PEXT2 ((uint32_t)'F' | ((uint32_t)'T' << 8) | \
                            ((uint32_t)'E' << 16) | ((uint32_t)'2' << 24))

struct reader {
    const uint8_t *data;
    size_t len;
    size_t pos;
    bool bad;
};

struct writer {
    uint8_t *data;
    size_t len;
    size_t capacity;
    size_t limit;
    bool bad;
};

static void set_error(char *error, size_t error_size, const char *fmt, ...)
{
    va_list args;
    if (!error || !error_size)
        return;
    va_start(args, fmt);
    (void)vsnprintf(error, error_size, fmt, args);
    va_end(args);
}

static const uint8_t *reader_take(struct reader *reader, size_t len)
{
    const uint8_t *result;
    if (len > reader->len - reader->pos) {
        reader->bad = true;
        return NULL;
    }
    result = reader->data + reader->pos;
    reader->pos += len;
    return result;
}

static uint8_t reader_u8(struct reader *reader)
{
    const uint8_t *p = reader_take(reader, 1);
    return p ? p[0] : 0;
}

static uint16_t reader_u16(struct reader *reader)
{
    const uint8_t *p = reader_take(reader, 2);
    if (!p)
        return 0;
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t reader_u32(struct reader *reader)
{
    const uint8_t *p = reader_take(reader, 4);
    return p ? (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
               ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24) : 0;
}

static const uint8_t *reader_string(struct reader *reader, size_t *wire_len)
{
    size_t start = reader->pos;
    while (reader->pos < reader->len && reader->data[reader->pos] != 0)
        reader->pos++;
    if (reader->pos == reader->len) {
        reader->bad = true;
        *wire_len = 0;
        return NULL;
    }
    reader->pos++;
    *wire_len = reader->pos - start;
    return reader->data + start;
}

static void writer_init(struct writer *writer, size_t limit)
{
    memset(writer, 0, sizeof(*writer));
    writer->limit = limit;
}

static void writer_free(struct writer *writer)
{
    free(writer->data);
    memset(writer, 0, sizeof(*writer));
}

static bool writer_reserve(struct writer *writer, size_t extra)
{
    size_t wanted;
    size_t capacity;
    uint8_t *data;

    if (extra > writer->limit - writer->len) {
        writer->bad = true;
        return false;
    }
    wanted = writer->len + extra;
    if (wanted <= writer->capacity)
        return true;
    capacity = writer->capacity ? writer->capacity : 128;
    while (capacity < wanted) {
        if (capacity > writer->limit / 2) {
            capacity = writer->limit;
            break;
        }
        capacity *= 2;
    }
    data = realloc(writer->data, capacity);
    if (!data) {
        writer->bad = true;
        return false;
    }
    writer->data = data;
    writer->capacity = capacity;
    return true;
}

static void writer_bytes(struct writer *writer, const void *data, size_t len)
{
    if (!len)
        return;
    if (!data) {
        writer->bad = true;
        return;
    }
    if (!writer_reserve(writer, len))
        return;
    memcpy(writer->data + writer->len, data, len);
    writer->len += len;
}

static void writer_u8(struct writer *writer, uint8_t value)
{
    writer_bytes(writer, &value, 1);
}

static void writer_u16(struct writer *writer, uint16_t value)
{
    uint8_t bytes[2] = {(uint8_t)value, (uint8_t)(value >> 8)};
    writer_bytes(writer, bytes, sizeof(bytes));
}

static void writer_u32(struct writer *writer, uint32_t value)
{
    uint8_t bytes[4] = {
        (uint8_t)value, (uint8_t)(value >> 8),
        (uint8_t)(value >> 16), (uint8_t)(value >> 24)
    };
    writer_bytes(writer, bytes, sizeof(bytes));
}

static void writer_input_string(struct writer *writer, struct reader *reader)
{
    size_t len;
    const uint8_t *string = reader_string(reader, &len);
    size_t available;

    if (!string)
        return;
    available = writer->limit - writer->len;
    if (len <= available) {
        writer_bytes(writer, string, len);
        return;
    }
    if (!available) {
        writer->bad = true;
        return;
    }
    if (available > 1)
        writer_bytes(writer, string, available - 1);
    writer_u8(writer, 0);
}

static bool command_is_pext(const uint8_t *string, size_t wire_len)
{
    static const char command[] = "pext";
    size_t i = 0;

    while (i + 1 < wire_len) {
        size_t j;
        while (i + 1 < wire_len && string[i] <= ' ')
            i++;
        for (j = 0; j < sizeof(command) - 1; j++) {
            uint8_t c;
            if (i + j + 1 >= wire_len)
                break;
            c = string[i + j];
            if (c >= 'A' && c <= 'Z')
                c = (uint8_t)(c - 'A' + 'a');
            if (c != (uint8_t)command[j])
                break;
        }
        if (j == sizeof(command) - 1) {
            size_t end = i + j;
            if (end < wire_len && (string[end] == 0 || string[end] <= ' '))
                return true;
        }
        while (i + 1 < wire_len && string[i] != ';' && string[i] != '\n')
            i++;
        if (i + 1 < wire_len)
            i++;
    }
    return false;
}

static bool copy_n(struct writer *writer, struct reader *reader, size_t len)
{
    const uint8_t *p = reader_take(reader, len);
    if (!p)
        return false;
    writer_bytes(writer, p, len);
    return !writer->bad;
}

void nq_batch_init(struct nq_batch *batch)
{
    memset(batch, 0, sizeof(*batch));
}

void nq_batch_free(struct nq_batch *batch)
{
    size_t i;
    for (i = 0; i < batch->count; i++)
        free(batch->items[i].data);
    free(batch->items);
    memset(batch, 0, sizeof(*batch));
}

static bool batch_append(struct nq_batch *batch, const uint8_t *data,
                         size_t len, size_t chunk_limit)
{
    struct nq_blob *blob;
    uint8_t *new_data;

    if (!len)
        return true;
    if (len > chunk_limit)
        return false;

    if (batch->count &&
        batch->items[batch->count - 1].len <= chunk_limit - len) {
        blob = &batch->items[batch->count - 1];
        new_data = realloc(blob->data, blob->len + len);
        if (!new_data)
            return false;
        memcpy(new_data + blob->len, data, len);
        blob->data = new_data;
        blob->len += len;
        return true;
    }

    if (batch->count == batch->capacity) {
        size_t capacity = batch->capacity ? batch->capacity * 2 : 8;
        struct nq_blob *items = realloc(batch->items,
                                        capacity * sizeof(*items));
        if (!items)
            return false;
        batch->items = items;
        batch->capacity = capacity;
    }
    blob = &batch->items[batch->count++];
    blob->data = malloc(len);
    if (!blob->data) {
        batch->count--;
        return false;
    }
    memcpy(blob->data, data, len);
    blob->len = len;
    return true;
}

void nq_xlat_init(struct nq_xlat_state *state, bool client_angle16)
{
    memset(state, 0, sizeof(*state));
    state->client_angle16 = client_angle16;
    state->max_entities = client_angle16 ? 2048u : 600u;
    state->max_scoreboard = 16;
    state->models_exposed = 256;
    state->sounds_exposed = 256;
}

static bool translate_serverinfo(struct nq_xlat_state *state,
                                 struct reader *reader, struct writer *writer,
                                 char *error, size_t error_size)
{
    uint32_t protocol = reader_u32(reader);
    uint32_t extension_bits;
    uint8_t maxclients;
    uint8_t gametype;
    size_t len;
    const uint8_t *string;
    unsigned int input_index;
    unsigned int output_next;
    bool exposing;
    size_t model_budget = writer->limit * 3 / 4;
    size_t sound_budget = writer->limit > 256 ? writer->limit - 256 : writer->limit;

    while (protocol == PROTOCOL_FTE_PEXT1 || protocol == PROTOCOL_FTE_PEXT2) {
        extension_bits = reader_u32(reader);
        if (extension_bits != 0) {
            set_error(error, error_size,
                      "upstream enabled FTE extensions (0x%08x)",
                      extension_bits);
            return false;
        }
        protocol = reader_u32(reader);
    }
    if (protocol != NQ_PROTOCOL_FITZQUAKE) {
        set_error(error, error_size,
                  "upstream gameplay protocol is %u, expected 666", protocol);
        return false;
    }
    state->upstream_protocol = protocol;
    state->static_count = 0;
    state->serverinfo_generation++;
    state->warned_limits = false;

    maxclients = reader_u8(reader);
    gametype = reader_u8(reader);
    state->max_scoreboard = maxclients > 16 ? 16 : maxclients;
    writer_u32(writer, NQ_PROTOCOL_NETQUAKE);
    writer_u8(writer, maxclients > 16 ? 16 : maxclients);
    writer_u8(writer, gametype);
    writer_input_string(writer, reader);

    input_index = 1;
    output_next = 1;
    exposing = true;
    for (;;) {
        string = reader_string(reader, &len);
        if (!string)
            return false;
        if (len == 1)
            break;
        if (exposing && input_index < 256 &&
            writer->len + len + 1 <= model_budget) {
            writer_bytes(writer, string, len);
            output_next = input_index + 1;
        } else {
            exposing = false;
            state->warned_limits = true;
        }
        input_index++;
    }
    writer_u8(writer, 0);
    state->models_exposed = output_next;

    input_index = 1;
    output_next = 1;
    exposing = true;
    for (;;) {
        string = reader_string(reader, &len);
        if (!string)
            return false;
        if (len == 1)
            break;
        if (exposing && input_index < 256 &&
            writer->len + len + 1 <= sound_budget) {
            writer_bytes(writer, string, len);
            output_next = input_index + 1;
        } else {
            exposing = false;
            state->warned_limits = true;
        }
        input_index++;
    }
    writer_u8(writer, 0);
    state->sounds_exposed = output_next;
    return !reader->bad && !writer->bad;
}

static bool translate_baseline(struct nq_xlat_state *state,
                               struct reader *reader, struct writer *writer,
                               bool version2, bool is_static, bool *keep)
{
    uint16_t entity = 0;
    uint8_t bits = 0;
    uint16_t model;
    uint16_t frame;
    const uint8_t *tail;

    if (!is_static)
        entity = reader_u16(reader);
    if (version2)
        bits = reader_u8(reader);
    model = (bits & B_LARGEMODEL) ? reader_u16(reader) : reader_u8(reader);
    frame = (bits & B_LARGEFRAME) ? reader_u16(reader) : reader_u8(reader);
    tail = reader_take(reader, 11); /* colormap, skin, 3*(coord + angle) */
    if (bits & B_ALPHA)
        (void)reader_u8(reader);
    if (bits & B_SCALE)
        (void)reader_u8(reader);
    if (bits & ~(B_LARGEMODEL | B_LARGEFRAME | B_ALPHA | B_SCALE))
        reader->bad = true;

    if (is_static) {
        if (state->static_count++ >= 128)
            *keep = false;
    } else if (entity >= state->max_entities) {
        *keep = false;
    }
    if (!*keep || reader->bad)
        return !reader->bad;

    writer_u8(writer, is_static ? SVC_SPAWNSTATIC : SVC_SPAWNBASELINE);
    if (!is_static)
        writer_u16(writer, entity);
    if (model >= state->models_exposed)
        model = 0;
    writer_u8(writer, (uint8_t)model);
    writer_u8(writer, (uint8_t)frame);
    writer_bytes(writer, tail, 11);
    return !writer->bad;
}

static bool translate_static_sound(struct nq_xlat_state *state,
                                   struct reader *reader,
                                   struct writer *writer, bool version2,
                                   bool *keep)
{
    const uint8_t *origin = reader_take(reader, 6);
    uint16_t sound = version2 ? reader_u16(reader) : reader_u8(reader);
    uint8_t volume = reader_u8(reader);
    uint8_t attenuation = reader_u8(reader);

    if (sound >= state->sounds_exposed)
        *keep = false;
    if (!*keep || reader->bad)
        return !reader->bad;
    writer_u8(writer, SVC_SPAWNSTATICSOUND);
    writer_bytes(writer, origin, 6);
    writer_u8(writer, (uint8_t)sound);
    writer_u8(writer, volume);
    writer_u8(writer, attenuation);
    return !writer->bad;
}

static bool translate_sound(struct nq_xlat_state *state,
                            struct reader *reader, struct writer *writer,
                            bool *keep, char *error, size_t error_size)
{
    uint8_t mask = reader_u8(reader);
    uint8_t volume = 255;
    uint8_t attenuation = 64;
    uint16_t entity;
    uint8_t channel;
    uint16_t sound;
    const uint8_t *origin;

    if (mask & SND_FTE_MOREFLAGS) {
        set_error(error, error_size, "FTE sound extensions were not negotiated");
        return false;
    }
    if (mask & ~(SND_VOLUME | SND_ATTENUATION |
                 SND_LARGEENTITY | SND_LARGESOUND)) {
        set_error(error, error_size, "unsupported sound flags 0x%02x", mask);
        return false;
    }
    if (mask & SND_VOLUME)
        volume = reader_u8(reader);
    if (mask & SND_ATTENUATION)
        attenuation = reader_u8(reader);
    if (mask & SND_LARGEENTITY) {
        entity = reader_u16(reader);
        channel = reader_u8(reader);
    } else {
        uint16_t packed = reader_u16(reader);
        entity = packed >> 3;
        channel = packed & 7;
    }
    sound = (mask & SND_LARGESOUND) ? reader_u16(reader) : reader_u8(reader);
    origin = reader_take(reader, 6);

    if (entity >= state->max_entities || sound >= state->sounds_exposed)
        *keep = false;
    if (!*keep || reader->bad)
        return !reader->bad;

    writer_u8(writer, SVC_SOUND);
    writer_u8(writer, mask & (SND_VOLUME | SND_ATTENUATION));
    if (mask & SND_VOLUME)
        writer_u8(writer, volume);
    if (mask & SND_ATTENUATION)
        writer_u8(writer, attenuation);
    writer_u16(writer, (uint16_t)((entity << 3) | (channel & 7)));
    writer_u8(writer, (uint8_t)sound);
    writer_bytes(writer, origin, 6);
    return !writer->bad;
}

static bool translate_clientdata(struct reader *reader, struct writer *writer)
{
    uint32_t bits = reader_u16(reader);
    struct writer base;
    unsigned int i;

    writer_init(&base, 128);
    if (bits & SU_EXTEND1)
        bits |= (uint32_t)reader_u8(reader) << 16;
    if (bits & SU_EXTEND2)
        bits |= (uint32_t)reader_u8(reader) << 24;

    if (bits & (1u << 0)) copy_n(&base, reader, 1);
    if (bits & (1u << 1)) copy_n(&base, reader, 1);
    for (i = 0; i < 3; i++) {
        if (bits & (1u << (2 + i))) copy_n(&base, reader, 1);
        if (bits & (1u << (5 + i))) copy_n(&base, reader, 1);
    }
    copy_n(&base, reader, 4); /* items is always present */
    if (bits & (1u << 12)) copy_n(&base, reader, 1);
    if (bits & (1u << 13)) copy_n(&base, reader, 1);
    if (bits & (1u << 14)) copy_n(&base, reader, 1);
    copy_n(&base, reader, 8); /* health, ammo[5], active weapon */

    if (bits & SU_WEAPON2) (void)reader_u8(reader);
    if (bits & SU_ARMOR2) (void)reader_u8(reader);
    if (bits & SU_AMMO2) (void)reader_u8(reader);
    if (bits & SU_SHELLS2) (void)reader_u8(reader);
    if (bits & SU_NAILS2) (void)reader_u8(reader);
    if (bits & SU_ROCKETS2) (void)reader_u8(reader);
    if (bits & SU_CELLS2) (void)reader_u8(reader);
    if (bits & SU_WEAPONFRAME2) (void)reader_u8(reader);
    if (bits & SU_WEAPONALPHA) (void)reader_u8(reader);

    writer_u8(writer, SVC_CLIENTDATA);
    writer_u16(writer, (uint16_t)(bits & 0x7fffu));
    writer_bytes(writer, base.data, base.len);
    if (reader->bad || base.bad || writer->bad) {
        writer_free(&base);
        return false;
    }
    writer_free(&base);
    return true;
}

struct update_values {
    uint8_t model;
    uint8_t frame;
    uint8_t colormap;
    uint8_t skin;
    uint8_t effects;
    uint16_t origins[3];
    uint8_t angles[3];
    uint8_t model2;
};

static bool translate_update(struct nq_xlat_state *state, uint8_t command,
                             struct reader *reader, struct writer *writer,
                             bool *keep)
{
    uint32_t bits = command & 127u;
    uint32_t out_bits;
    uint16_t entity;
    uint16_t model;
    struct update_values value;
    static const uint32_t origin_bits[3] = {U_ORIGIN1, U_ORIGIN2, U_ORIGIN3};
    static const uint32_t angle_bits[3] = {U_ANGLE1, U_ANGLE2, U_ANGLE3};
    unsigned int i;

    memset(&value, 0, sizeof(value));
    if (bits & U_MOREBITS)
        bits |= (uint32_t)reader_u8(reader) << 8;
    if (bits & U_EXTEND1)
        bits |= (uint32_t)reader_u8(reader) << 16;
    if (bits & U_EXTEND2)
        bits |= (uint32_t)reader_u8(reader) << 24;
    entity = (bits & U_LONGENTITY) ? reader_u16(reader) : reader_u8(reader);

    if (bits & U_MODEL) value.model = reader_u8(reader);
    if (bits & U_FRAME) value.frame = reader_u8(reader);
    if (bits & U_COLORMAP) value.colormap = reader_u8(reader);
    if (bits & U_SKIN) value.skin = reader_u8(reader);
    if (bits & U_EFFECTS) value.effects = reader_u8(reader);
    for (i = 0; i < 3; i++) {
        if (bits & origin_bits[i]) value.origins[i] = reader_u16(reader);
        if (bits & angle_bits[i]) value.angles[i] = reader_u8(reader);
    }
    if (bits & U_ALPHA) (void)reader_u8(reader);
    if (bits & U_SCALE) (void)reader_u8(reader);
    if (bits & U_FRAME2) (void)reader_u8(reader);
    if (bits & U_MODEL2) value.model2 = reader_u8(reader);
    if (bits & U_LERPFINISH) (void)reader_u8(reader);
    if (reader->bad)
        return false;

    if (entity >= state->max_entities)
        *keep = false;
    if (!*keep)
        return true;

    out_bits = bits & 0x7ffeu; /* strip extensions and rebuild MOREBITS */
    if (out_bits & 0x7f00u)
        out_bits |= U_MOREBITS;
    writer_u8(writer, (uint8_t)(U_SIGNAL | (out_bits & 0x7fu)));
    if (out_bits & U_MOREBITS)
        writer_u8(writer, (uint8_t)(out_bits >> 8));
    if (out_bits & U_LONGENTITY)
        writer_u16(writer, entity);
    else
        writer_u8(writer, (uint8_t)entity);

    if (out_bits & U_MODEL) {
        model = (uint16_t)(value.model | ((uint16_t)value.model2 << 8));
        if (model >= state->models_exposed)
            model = 0;
        writer_u8(writer, (uint8_t)model);
    }
    if (out_bits & U_FRAME) writer_u8(writer, value.frame);
    if (out_bits & U_COLORMAP) writer_u8(writer, value.colormap);
    if (out_bits & U_SKIN) writer_u8(writer, value.skin);
    if (out_bits & U_EFFECTS) writer_u8(writer, value.effects);
    for (i = 0; i < 3; i++) {
        if (out_bits & origin_bits[i]) writer_u16(writer, value.origins[i]);
        if (out_bits & angle_bits[i]) writer_u8(writer, value.angles[i]);
    }
    return !writer->bad;
}

static bool translate_temp_entity(struct reader *reader, struct writer *writer,
                                  bool *keep, char *error,
                                  size_t error_size)
{
    uint8_t type = reader_u8(reader);
    size_t payload = 0;
    bool legacy = false;

    switch (type) {
    case 0: case 1: case 2: case 3: case 4:
    case 7: case 8: case 10: case 11:
        payload = 6; legacy = true; break;
    case 5: case 6: case 9: case 13:
        payload = 14; legacy = true; break;
    case 12:
        payload = 8; legacy = true; break;
    case 16:
        payload = 12; break;
    case 17: {
        size_t string_len;
        (void)reader_string(reader, &string_len);
        payload = 14;
        break;
    }
    case 20: payload = 6; break;
    case 21: payload = 7; break;
    case 50: case 51: payload = 10; break;
    case 52: payload = 16; break;
    case 53: payload = 9; break;
    case 54: payload = 24; break;
    case 55: case 56: payload = 21; break;
    case 57: case 58: case 59: case 70: case 72: case 75:
        payload = 6; break;
    case 73: payload = 11; break;
    case 74: payload = 13; break;
    default:
        set_error(error, error_size, "unsupported temp entity type %u", type);
        return false;
    }

    if (legacy) {
        writer_u8(writer, SVC_TEMP_ENTITY);
        writer_u8(writer, type);
        return copy_n(writer, reader, payload);
    }
    *keep = false;
    return reader_take(reader, payload) != NULL;
}

static bool copy_filtered_slot(struct nq_xlat_state *state,
                               struct reader *reader, struct writer *writer,
                               size_t remainder, bool string_remainder,
                               bool *keep)
{
    uint8_t slot = reader_u8(reader);
    if (slot >= state->max_scoreboard)
        *keep = false;
    if (*keep)
        writer_u8(writer, slot);
    if (string_remainder) {
        size_t len;
        const uint8_t *string = reader_string(reader, &len);
        if (*keep && string)
            writer_bytes(writer, string, len);
    } else {
        const uint8_t *p = reader_take(reader, remainder);
        if (*keep && p)
            writer_bytes(writer, p, remainder);
    }
    return !reader->bad && !writer->bad;
}

bool nq_translate_server_message(struct nq_xlat_state *state,
                                 const uint8_t *input, size_t input_len,
                                 bool reliable, struct nq_batch *output,
                                 char *error, size_t error_size)
{
    struct reader reader = {input, input_len, 0, false};
    struct nq_xlat_state next_state = *state;
    size_t chunk_limit = reliable ? NQ_LEGACY_RELIABLE_MAX :
                                    NQ_LEGACY_DATAGRAM_MAX;

    if (error && error_size)
        error[0] = 0;

    while (reader.pos < reader.len) {
        uint8_t command = reader_u8(&reader);
        struct writer writer;
        bool keep = true;
        bool ok = true;

        writer_init(&writer, chunk_limit);
        if (command & U_SIGNAL) {
            ok = translate_update(&next_state, command, &reader, &writer,
                                  &keep);
        } else {
            switch (command) {
            case SVC_NOP:
            case SVC_DISCONNECT:
            case SVC_KILLEDMONSTER:
            case SVC_FOUNDSECRET:
            case SVC_INTERMISSION:
            case SVC_SELLSCREEN:
                writer_u8(&writer, command);
                break;
            case SVC_UPDATESTAT: {
                uint8_t stat = reader_u8(&reader);
                const uint8_t *value = reader_take(&reader, 4);
                if (stat >= 32)
                    keep = false;
                if (keep && value) {
                    writer_u8(&writer, command);
                    writer_u8(&writer, stat);
                    writer_bytes(&writer, value, 4);
                }
                break;
            }
            case SVC_VERSION: {
                uint32_t protocol = reader_u32(&reader);
                writer_u8(&writer, command);
                writer_u32(&writer, protocol == NQ_PROTOCOL_FITZQUAKE ?
                                       NQ_PROTOCOL_NETQUAKE : protocol);
                break;
            }
            case SVC_SETVIEW:
                writer_u8(&writer, command);
                ok = copy_n(&writer, &reader, 2);
                break;
            case SVC_STOPSOUND: {
                uint16_t packed = reader_u16(&reader);
                if ((packed >> 3) >= next_state.max_entities)
                    keep = false;
                if (keep) {
                    writer_u8(&writer, command);
                    writer_u16(&writer, packed);
                }
                break;
            }
            case SVC_TIME:
                writer_u8(&writer, command);
                ok = copy_n(&writer, &reader, 4);
                break;
            case SVC_PRINT:
            case SVC_STUFFTEXT:
            case SVC_CENTERPRINT:
            case SVC_FINALE:
            case SVC_CUTSCENE:
                writer_u8(&writer, command);
                writer_input_string(&writer, &reader);
                break;
            case SVC_SETANGLE:
                writer_u8(&writer, command);
                ok = copy_n(&writer, &reader, 3);
                break;
            case SVC_SERVERINFO:
                writer_u8(&writer, command);
                ok = translate_serverinfo(&next_state, &reader, &writer,
                                          error, error_size);
                break;
            case SVC_LIGHTSTYLE:
                writer_u8(&writer, command);
                ok = copy_n(&writer, &reader, 1);
                writer_input_string(&writer, &reader);
                break;
            case SVC_UPDATENAME:
                writer_u8(&writer, command);
                ok = copy_filtered_slot(&next_state, &reader, &writer, 0, true,
                                        &keep);
                break;
            case SVC_UPDATEFRAGS:
                writer_u8(&writer, command);
                ok = copy_filtered_slot(&next_state, &reader, &writer, 2,
                                        false,
                                        &keep);
                break;
            case SVC_UPDATECOLORS:
                writer_u8(&writer, command);
                ok = copy_filtered_slot(&next_state, &reader, &writer, 1,
                                        false,
                                        &keep);
                break;
            case SVC_CLIENTDATA:
                ok = translate_clientdata(&reader, &writer);
                break;
            case SVC_SOUND:
                ok = translate_sound(&next_state, &reader, &writer, &keep,
                                     error, error_size);
                break;
            case SVC_PARTICLE:
                writer_u8(&writer, command);
                ok = copy_n(&writer, &reader, 11);
                break;
            case SVC_DAMAGE:
                writer_u8(&writer, command);
                ok = copy_n(&writer, &reader, 8);
                break;
            case SVC_SPAWNSTATIC:
                ok = translate_baseline(&next_state, &reader, &writer, false,
                                        true, &keep);
                break;
            case SVC_SPAWNBASELINE:
                ok = translate_baseline(&next_state, &reader, &writer, false,
                                        false, &keep);
                break;
            case SVC_TEMP_ENTITY:
                ok = translate_temp_entity(&reader, &writer, &keep,
                                           error, error_size);
                break;
            case SVC_SETPAUSE:
            case SVC_SIGNONNUM:
                writer_u8(&writer, command);
                ok = copy_n(&writer, &reader, 1);
                break;
            case SVC_SPAWNSTATICSOUND:
                ok = translate_static_sound(&next_state, &reader, &writer,
                                            false,
                                            &keep);
                break;
            case SVC_CDTRACK:
                writer_u8(&writer, command);
                ok = copy_n(&writer, &reader, 2);
                break;
            case SVC_DP_SHOWPIC:
                keep = false;
                { size_t ignored; (void)reader_string(&reader, &ignored);
                  (void)reader_string(&reader, &ignored); }
                (void)reader_take(&reader, 2);
                break;
            case SVC_DP_HIDEPIC:
            case SVC_SKYBOX:
            case SVC_ACHIEVEMENT:
                keep = false;
                { size_t ignored; (void)reader_string(&reader, &ignored); }
                break;
            case SVC_BF:
                keep = false;
                break;
            case SVC_FOG:
                keep = false;
                (void)reader_take(&reader, 6);
                break;
            case SVC_SPAWNBASELINE2:
                ok = translate_baseline(&next_state, &reader, &writer, true,
                                        false, &keep);
                break;
            case SVC_SPAWNSTATIC2:
                ok = translate_baseline(&next_state, &reader, &writer, true,
                                        true, &keep);
                break;
            case SVC_SPAWNSTATICSOUND2:
                ok = translate_static_sound(&next_state, &reader, &writer,
                                            true,
                                            &keep);
                break;
            default:
                set_error(error, error_size,
                          "unsupported server command %u at byte %zu",
                          command, reader.pos - 1);
                ok = false;
                break;
            }
        }

        if (!ok || reader.bad || writer.bad) {
            if (error && error_size && !error[0])
                set_error(error, error_size,
                          "truncated or oversized server command %u", command);
            writer_free(&writer);
            return false;
        }
        if (keep && !batch_append(output, writer.data, writer.len, chunk_limit)) {
            set_error(error, error_size, "out of memory building server message");
            writer_free(&writer);
            return false;
        }
        writer_free(&writer);
    }
    *state = next_state;
    return true;
}

bool nq_translate_client_message(const struct nq_xlat_state *state,
                                 const uint8_t *input, size_t input_len,
                                 bool reliable, struct nq_batch *output,
                                 char *error, size_t error_size)
{
    struct reader reader = {input, input_len, 0, false};
    size_t chunk_limit = reliable ? 32000u : 1442u;

    if (error && error_size)
        error[0] = 0;

    while (reader.pos < reader.len) {
        uint8_t command = reader_u8(&reader);
        struct writer writer;

        writer_init(&writer, chunk_limit);
        writer_u8(&writer, command);
        switch (command) {
        case CLC_NOP:
        case CLC_DISCONNECT:
            break;
        case CLC_STRINGCMD: {
            static const uint8_t plain_pext[] = {'p', 'e', 'x', 't', 0};
            size_t wire_len;
            const uint8_t *string = reader_string(&reader, &wire_len);
            if (string && command_is_pext(string, wire_len))
                writer_bytes(&writer, plain_pext, sizeof(plain_pext));
            else if (string)
                writer_bytes(&writer, string, wire_len);
            break;
        }
        case CLC_MOVE: {
            unsigned int i;
            copy_n(&writer, &reader, 4); /* timestamp */
            for (i = 0; i < 3; i++) {
                if (state->client_angle16)
                    copy_n(&writer, &reader, 2);
                else
                    writer_u16(&writer,
                               (uint16_t)((uint16_t)reader_u8(&reader) << 8));
            }
            copy_n(&writer, &reader, 8); /* movement, buttons, impulse */
            break;
        }
        default:
            set_error(error, error_size,
                      "unsupported client command %u at byte %zu",
                      command, reader.pos - 1);
            writer_free(&writer);
            return false;
        }

        if (reader.bad || writer.bad) {
            set_error(error, error_size,
                      "truncated or oversized client command %u", command);
            writer_free(&writer);
            return false;
        }
        if (!batch_append(output, writer.data, writer.len, chunk_limit)) {
            set_error(error, error_size, "out of memory building client message");
            writer_free(&writer);
            return false;
        }
        writer_free(&writer);
    }
    return true;
}
