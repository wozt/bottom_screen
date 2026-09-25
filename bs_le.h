#ifndef BOTTOM_SCREEN_LE_H
#define BOTTOM_SCREEN_LE_H

#include <string.h>

#include "bs_protocol.h"

/*
 * Explicit little-endian serialisation for the wire protocol.
 *
 * bs_protocol.h says "everything is little-endian" and gets away with a
 * plain memcpy of each packed struct, because every client it had was
 * natively little-endian: x86-64, aarch64. The Wii U is not -- its
 * PowerPC is big-endian -- and a memcpy there writes every multi-byte
 * field backwards, which looks like a handshake that connects and then
 * makes no sense.
 *
 * So this header is the same structs, read and written one field at a
 * time with explicit shifts. Shifts rather than a byte-swap of the
 * struct: the shifts mean the same thing on any host, so this file is
 * compiled and tested on the little-endian build machine and then
 * crosses to the console unchanged. Nothing in here depends on the
 * host's endianness at all.
 *
 * Header-only, because the Wii U build is a handful of files compiled
 * together and a second translation unit buys nothing.
 *
 * Reserved fields are written as zero, never copied from the struct:
 * what a caller left in a padding byte is its own business, and the
 * wire format says zero.
 */

/* --- the primitives -------------------------------------------------- */

static inline uint16_t bs_rd16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t bs_rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void bs_wr16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static inline void bs_wr32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

/* --- the handshake --------------------------------------------------- */

static inline void bs_hello_to_le(const BsHello *in, uint8_t *out)
{
    bs_wr32le(out + 0, in->magic);
    out[4] = in->version;
    memset(out + 5, 0, 3);
}

static inline void bs_hello_from_le(BsHello *out, const uint8_t *in)
{
    out->magic = bs_rd32le(in + 0);
    out->version = in[4];
    memset(out->reserved, 0, sizeof(out->reserved));
}

static inline void bs_hello_ack_to_le(const BsHelloAck *in, uint8_t *out)
{
    bs_wr32le(out + 0, in->magic);
    out[4] = in->version;
    out[5] = in->accepted;
    out[6] = in->console;
    out[7] = in->codec;
    bs_wr16le(out + 8, in->width);
    bs_wr16le(out + 10, in->height);
    bs_wr16le(out + 12, in->fps);
    bs_wr16le(out + 14, in->extradata_size);
    out[16] = in->audio_codec;
    out[17] = in->audio_channels;
    bs_wr16le(out + 18, in->audio_rate);
}

static inline void bs_hello_ack_from_le(BsHelloAck *out, const uint8_t *in)
{
    out->magic = bs_rd32le(in + 0);
    out->version = in[4];
    out->accepted = in[5];
    out->console = in[6];
    out->codec = in[7];
    out->width = bs_rd16le(in + 8);
    out->height = bs_rd16le(in + 10);
    out->fps = bs_rd16le(in + 12);
    out->extradata_size = bs_rd16le(in + 14);
    out->audio_codec = in[16];
    out->audio_channels = in[17];
    out->audio_rate = bs_rd16le(in + 18);
}

/* --- framing ---------------------------------------------------------- */

static inline void bs_msg_header_to_le(const BsMsgHeader *in, uint8_t *out)
{
    out[0] = in->type;
    memset(out + 1, 0, 3);
    bs_wr32le(out + 4, in->payload_size);
}

static inline void bs_msg_header_from_le(BsMsgHeader *out, const uint8_t *in)
{
    out->type = in[0];
    memset(out->reserved, 0, sizeof(out->reserved));
    out->payload_size = bs_rd32le(in + 4);
}

/* --- video ------------------------------------------------------------ */

static inline void bs_video_header_to_le(const BsVideoHeader *in, uint8_t *out)
{
    bs_wr32le(out + 0, in->frame_id);
    bs_wr32le(out + 4, in->timestamp_us);
    bs_wr16le(out + 8, in->fragment_id);
    bs_wr16le(out + 10, in->fragment_count);
    out[12] = in->flags;
    memset(out + 13, 0, 3);
}

static inline void bs_video_header_from_le(BsVideoHeader *out, const uint8_t *in)
{
    out->frame_id = bs_rd32le(in + 0);
    out->timestamp_us = bs_rd32le(in + 4);
    out->fragment_id = bs_rd16le(in + 8);
    out->fragment_count = bs_rd16le(in + 10);
    out->flags = in[12];
    memset(out->reserved, 0, sizeof(out->reserved));
}

/* --- the stream changing shape ---------------------------------------- */

static inline void bs_stream_info_to_le(const BsStreamInfo *in, uint8_t *out)
{
    bs_wr32le(out + 0, in->from_frame_id);
    bs_wr16le(out + 4, in->width);
    bs_wr16le(out + 6, in->height);
    bs_wr16le(out + 8, in->fps);
}

static inline void bs_stream_info_from_le(BsStreamInfo *out, const uint8_t *in)
{
    out->from_frame_id = bs_rd32le(in + 0);
    out->width = bs_rd16le(in + 4);
    out->height = bs_rd16le(in + 6);
    out->fps = bs_rd16le(in + 8);
}

/* --- audio ------------------------------------------------------------- */

static inline void bs_audio_header_to_le(const BsAudioHeader *in, uint8_t *out)
{
    bs_wr32le(out + 0, in->timestamp_us);
    bs_wr32le(out + 4, in->sequence);
}

static inline void bs_audio_header_from_le(BsAudioHeader *out, const uint8_t *in)
{
    out->timestamp_us = bs_rd32le(in + 0);
    out->sequence = bs_rd32le(in + 4);
}

/* --- quality and size --------------------------------------------------- */

static inline void bs_quality_to_le(const BsQuality *in, uint8_t *out)
{
    bs_wr32le(out + 0, in->bitrate);
    bs_wr16le(out + 4, in->fps);
    bs_wr16le(out + 6, 0);
}

static inline void bs_quality_from_le(BsQuality *out, const uint8_t *in)
{
    out->bitrate = bs_rd32le(in + 0);
    out->fps = bs_rd16le(in + 4);
    out->reserved = 0;
}

static inline void bs_size_to_le(const BsSize *in, uint8_t *out)
{
    bs_wr16le(out + 0, in->width);
    bs_wr16le(out + 2, in->height);
}

static inline void bs_size_from_le(BsSize *out, const uint8_t *in)
{
    out->width = bs_rd16le(in + 0);
    out->height = bs_rd16le(in + 2);
}

/* --- input -------------------------------------------------------------- */

/*
 * x is int16_t -- the only signed field on the wire. It travels as its
 * own two bytes, so it is cast to the unsigned of the same width for the
 * shifts and back on the way in; two's complement makes that a copy.
 */
static inline void bs_input_event_to_le(const BsInputEvent *in, uint8_t *out)
{
    bs_wr32le(out + 0, in->sequence);
    bs_wr32le(out + 4, in->timestamp_us);
    out[8] = in->type;
    out[9] = in->code;
    bs_wr16le(out + 10, (uint16_t)in->x);
    bs_wr16le(out + 12, (uint16_t)in->y);
    bs_wr16le(out + 14, 0);
}

static inline void bs_input_event_from_le(BsInputEvent *out, const uint8_t *in)
{
    out->sequence = bs_rd32le(in + 0);
    out->timestamp_us = bs_rd32le(in + 4);
    out->type = in[8];
    out->code = in[9];
    out->x = (int16_t)bs_rd16le(in + 10);
    out->y = (int16_t)bs_rd16le(in + 12);
    out->reserved = 0;
}

/* --- prompts ------------------------------------------------------------ */

static inline void bs_prompt_to_le(const BsPrompt *in, uint8_t *out)
{
    bs_wr16le(out + 0, in->id);
    out[2] = in->kind;
    out[3] = in->choices;
    bs_wr16le(out + 4, in->max_len);
    out[6] = in->multiline;
    out[7] = 0;
}

static inline void bs_prompt_from_le(BsPrompt *out, const uint8_t *in)
{
    out->id = bs_rd16le(in + 0);
    out->kind = in[2];
    out->choices = in[3];
    out->max_len = bs_rd16le(in + 4);
    out->multiline = in[6];
    out->reserved = 0;
}

static inline void bs_prompt_reply_to_le(const BsPromptReply *in, uint8_t *out)
{
    bs_wr16le(out + 0, in->id);
    out[2] = in->cancelled;
    out[3] = in->choice;
}

static inline void bs_prompt_reply_from_le(BsPromptReply *out, const uint8_t *in)
{
    out->id = bs_rd16le(in + 0);
    out->cancelled = in[2];
    out->choice = in[3];
}

/* --- screens ------------------------------------------------------------ */

static inline void bs_screen_choice_to_le(const BsScreenChoice *in, uint8_t *out)
{
    out[0] = in->screen;
    memset(out + 1, 0, 3);
}

static inline void bs_screen_choice_from_le(BsScreenChoice *out, const uint8_t *in)
{
    out->screen = in[0];
    memset(out->reserved, 0, sizeof(out->reserved));
}

static inline void bs_screens_to_le(const BsScreens *in, uint8_t *out)
{
    out[0] = in->available;
    out[1] = in->watching_bottom;
    out[2] = in->watching_top;
    out[3] = 0;
}

static inline void bs_screens_from_le(BsScreens *out, const uint8_t *in)
{
    out->available = in[0];
    out->watching_bottom = in[1];
    out->watching_top = in[2];
    out->reserved = 0;
}

static inline void bs_audio_choice_to_le(const BsAudioChoice *in, uint8_t *out)
{
    out[0] = in->source;
    memset(out + 1, 0, 3);
}

static inline void bs_audio_choice_from_le(BsAudioChoice *out, const uint8_t *in)
{
    out->source = in[0];
    memset(out->reserved, 0, sizeof(out->reserved));
}

#endif /* BOTTOM_SCREEN_LE_H */
