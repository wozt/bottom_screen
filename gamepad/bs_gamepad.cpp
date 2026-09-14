/*
 * bottom_screen -> a real Wii U GamePad.
 *
 * A client like any other: it connects to an emulator's server, decodes
 * the stream, and sends touch and buttons back. What is different is
 * where the picture goes. Instead of a window, a phone or a Switch, it
 * goes over the air to the GamePad itself, through libdrc and a
 * Realtek adapter pretending to be a Wii U.
 *
 * Which closes a circle that is worth naming: a Wii U GamePad, driven by
 * an emulator running a Wii U game, on a PC, with no console anywhere.
 * And a 3DS or a DS on a GamePad, if that is what the emulator happens
 * to be.
 *
 * ---------------------------------------------------------------------
 * A SKELETON, and the parts that are not yet honest are marked TODO.
 *
 * Everything here compiles and the shape is right, but only the pieces
 * this machine can test have been tested -- which does not include a
 * GamePad. See the notes on each TODO for what is unknown rather than
 * merely unwritten.
 * ---------------------------------------------------------------------
 *
 * The three halves, each on its own thread, because they have three
 * different clocks:
 *
 *   video   the server's frames, decoded, scaled to the panel, pushed
 *   audio   Opus in, 48 kHz stereo out, in the 8 ms chunks libdrc wants
 *   input   the GamePad's own buttons, sticks and touch, sent back
 *
 * The GamePad's panel is 864x480. A Wii U GamePad screen out of Cemu is
 * 848x480 and a 3DS is 400x240, so something always has to scale; doing
 * it here rather than asking the server for 864 keeps the server's sizes
 * whole multiples of the console's own screen, which is a rule worth
 * more than one client's convenience.
 */

#include <drc/input.h>
#include <drc/pixel-format.h>
#include <drc/streamer.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "bs_decoder.h"
#include "bs_net.h"
#include "bs_protocol.h"
#include <libswscale/swscale.h>
#include <opus/opus.h>
}

namespace {

/* The panel, which is not any console's screen and never will be. */
constexpr int kPanelW = 864;
constexpr int kPanelH = 480;

/* What libdrc consumes: 384 stereo samples, which is 8 ms at 48 kHz.
 * Pushing more per call does not make it faster, it makes the queue
 * grow -- and libdrc drops everything in silence once its 16 seconds
 * are full, which reads as "the sound stopped". */
constexpr int kAudioChunk = 384;

std::atomic<bool> g_stop{false};

/* ------------------------------------------------------------ the link */

struct Link {
    BsConn *conn = nullptr;
    BsHelloAck ack{};
    /* The size on the wire right now, which is not the size in the ack
     * once the emulator's internal resolution moves or somebody changes
     * screen. Touch is aimed in this space. */
    std::atomic<int> width{0}, height{0};
};

bool Connect(Link *link, const char *host, uint16_t port, std::string *err)
{
    char e[256] = "";
    link->conn = bs_connect(host, port, e, sizeof(e));
    if (!link->conn) { *err = e; return false; }

    BsHello hello{};
    hello.magic = BS_MAGIC;
    hello.version = BS_VERSION;
    if (bs_write_all(link->conn, &hello, sizeof(hello)) != 0) {
        *err = "the server closed on the greeting";
        return false;
    }
    if (bs_read_exact(link->conn, &link->ack, sizeof(link->ack)) != 0 ||
        link->ack.magic != BS_MAGIC || !link->ack.accepted) {
        *err = "refused, or not a bottom_screen server";
        return false;
    }
    /* The parameter sets ride in front of every keyframe here, so the
     * extradata is read and thrown away rather than needed. */
    if (link->ack.extradata_size) {
        std::vector<uint8_t> skip(link->ack.extradata_size);
        if (bs_read_exact(link->conn, skip.data(), skip.size()) != 0) {
            *err = "truncated greeting";
            return false;
        }
    }
    link->width = link->ack.width;
    link->height = link->ack.height;
    return true;
}

/* -------------------------------------------------------------- video */

/*
 * One decoder, one scaler, rebuilt whenever the picture changes shape.
 *
 * It changes more often than it sounds: the emulator's internal
 * resolution is a slider, and this client may be watching either screen.
 * Every other client in this project learned that the hard way -- a
 * decoder that is told a new size and not rebuilt simply stops, and on
 * hardware decoders it stops silently.
 */
class Video {
public:
    ~Video() { Reset(); }

    void Reset()
    {
        if (dec_) { bs_decoder_destroy(dec_); dec_ = nullptr; }
        if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
        src_w_ = src_h_ = 0;
    }

    bool Ensure(std::string *err)
    {
        if (dec_) return true;
        char e[128] = "";
        dec_ = bs_decoder_create(e, sizeof(e));
        if (!dec_) { *err = e; return false; }
        return true;
    }

    /* Returns true when a frame came out and `rgba` holds the panel. */
    bool Decode(const uint8_t *data, size_t size, std::vector<drc::byte> *rgba)
    {
        BsDecodedFrame f{};
        if (bs_decoder_decode(dec_, data, size, &f) <= 0)
            return false;

        if (f.width != src_w_ || f.height != src_h_) {
            if (sws_) sws_freeContext(sws_);
            /* Bilinear, not point: this is nearly always a resize --
             * 848 or 400 across into 864 -- and dropping pixels from a
             * picture being resized loses thin lines and text, which on
             * a menu is most of what is there. */
            sws_ = sws_getContext(f.width, f.height, AV_PIX_FMT_YUV420P,
                                  kPanelW, kPanelH, AV_PIX_FMT_RGBA,
                                  SWS_BILINEAR, nullptr, nullptr, nullptr);
            src_w_ = f.width;
            src_h_ = f.height;
            fprintf(stderr, "bs_gamepad: picture is %dx%d, scaling to %dx%d\n",
                    f.width, f.height, kPanelW, kPanelH);
        }
        if (!sws_) return false;

        rgba->resize(static_cast<size_t>(kPanelW) * kPanelH * 4);
        const uint8_t *planes[4] = { f.y, f.u, f.v, nullptr };
        int strides[4] = { f.y_stride, f.u_stride, f.v_stride, 0 };
        uint8_t *dst[4] = { reinterpret_cast<uint8_t *>(rgba->data()),
                            nullptr, nullptr, nullptr };
        int dst_stride[4] = { kPanelW * 4, 0, 0, 0 };
        sws_scale(sws_, planes, strides, 0, f.height, dst, dst_stride);
        return true;
    }

private:
    BsDecoder *dec_ = nullptr;
    SwsContext *sws_ = nullptr;
    int src_w_ = 0, src_h_ = 0;
};

/* -------------------------------------------------------------- audio */

/*
 * Opus in, 8 ms chunks out.
 *
 * The server sends one Opus packet per 20 ms and libdrc wants 8 ms, so
 * what comes out of the decoder is held and handed over a chunk at a
 * time. Anything left over stays for the next packet rather than being
 * padded with silence, which would be an audible click every 20 ms.
 */
class Audio {
public:
    bool Start(int channels, std::string *err)
    {
        int oerr = 0;
        channels_ = channels > 0 ? channels : 2;
        dec_ = opus_decoder_create(48000, channels_, &oerr);
        if (!dec_) { *err = "cannot start the Opus decoder"; return false; }
        return true;
    }

    ~Audio() { if (dec_) opus_decoder_destroy(dec_); }

    void Decode(const uint8_t *data, size_t size, drc::Streamer *streamer)
    {
        if (!dec_) return;
        /* 5760 is 120 ms at 48 kHz, the largest an Opus packet can be. */
        std::vector<drc::s16> pcm(5760 * channels_);
        const int frames = opus_decode(dec_, data, static_cast<opus_int32>(size),
                                       pcm.data(), 5760, 0);
        if (frames <= 0) return;

        /*
         * TODO(mono): a source with one channel is upmixed by repeating
         * the sample. Every emulator here sends stereo, so this has
         * never run -- it is written to be obviously wrong rather than
         * quietly absent.
         */
        for (int i = 0; i < frames; i++) {
            if (channels_ == 1) {
                held_.push_back(pcm[i]);
                held_.push_back(pcm[i]);
            } else {
                held_.push_back(pcm[i * 2]);
                held_.push_back(pcm[i * 2 + 1]);
            }
        }

        while (held_.size() >= static_cast<size_t>(kAudioChunk) * 2) {
            std::vector<drc::s16> chunk(held_.begin(),
                                        held_.begin() + kAudioChunk * 2);
            streamer->PushAudSamples(chunk);
            held_.erase(held_.begin(), held_.begin() + kAudioChunk * 2);
        }
    }

private:
    OpusDecoder *dec_ = nullptr;
    int channels_ = 2;
    std::vector<drc::s16> held_;
};

/* -------------------------------------------------------------- input */

/*
 * The GamePad's own controls, sent back to the emulator.
 *
 * Its buttons are the Wii U's, so on a Wii U they map one to one. On a
 * DS or a 3DS the server's backend drops what that machine does not
 * have, which is the arrangement every client here relies on: a client
 * says A, and what A means is the backend's business.
 */
struct Mapping { uint32_t mask; int code; };

const Mapping kButtons[] = {
    { drc::InputData::kBtnA,     BS_BTN_A },
    { drc::InputData::kBtnB,     BS_BTN_B },
    { drc::InputData::kBtnX,     BS_BTN_X },
    { drc::InputData::kBtnY,     BS_BTN_Y },
    { drc::InputData::kBtnL,     BS_BTN_L },
    { drc::InputData::kBtnR,     BS_BTN_R },
    { drc::InputData::kBtnZL,    BS_BTN_ZL },
    { drc::InputData::kBtnZR,    BS_BTN_ZR },
    { drc::InputData::kBtnPlus,  BS_BTN_START },
    { drc::InputData::kBtnMinus, BS_BTN_SELECT },
    { drc::InputData::kBtnUp,    BS_BTN_UP },
    { drc::InputData::kBtnDown,  BS_BTN_DOWN },
    { drc::InputData::kBtnLeft,  BS_BTN_LEFT },
    { drc::InputData::kBtnRight, BS_BTN_RIGHT },
    { drc::InputData::kBtnHome,  BS_BTN_HOME },
};

void SendEvent(Link *link, uint8_t type, uint8_t code, int16_t x, int16_t y)
{
    static std::atomic<uint32_t> seq{0};
    BsInputEvent ev{};
    ev.sequence = seq++;
    ev.timestamp_us = bs_now_us();
    ev.type = type;
    ev.code = code;
    ev.x = x;
    ev.y = y;
    bs_send_msg(link->conn, BS_MSG_INPUT, &ev, sizeof(ev), nullptr, 0);
}

void InputLoop(drc::Streamer *streamer, Link *link)
{
    uint32_t held = 0;
    bool touching = false;

    while (!g_stop) {
        drc::InputData in;
        streamer->PollInput(&in);
        if (in.valid) {
            const uint32_t now = static_cast<uint32_t>(in.buttons);
            for (const auto &m : kButtons) {
                const bool was = (held & m.mask) != 0;
                const bool is = (now & m.mask) != 0;
                if (was != is)
                    SendEvent(link, is ? BS_INPUT_BUTTON_DOWN : BS_INPUT_BUTTON_UP,
                              static_cast<uint8_t>(m.code), 0, 0);
            }
            held = now;

            /* Sticks: libdrc gives -1..1 and the protocol wants a signed
             * 16-bit deflection. Y is negated because a stick reports up
             * as positive and so does this protocol -- but libdrc's y
             * grows downward.
             *
             * TODO(deadzone): sent on every poll, including the drift a
             * stick at rest produces. Every other client here sends on
             * change only. Whether this pad's rest position is clean
             * enough to need a dead zone is a question for a pad. */
            auto axis = [&](int code, float v) {
                int val = static_cast<int>(v * 32767.0f);
                if (val > 32767) val = 32767;
                if (val < -32767) val = -32767;
                SendEvent(link, BS_INPUT_AXIS, static_cast<uint8_t>(code),
                          static_cast<int16_t>(val), 0);
            };
            axis(BS_AXIS_LEFT_X, in.left_stick_x);
            axis(BS_AXIS_LEFT_Y, -in.left_stick_y);
            axis(BS_AXIS_RIGHT_X, in.right_stick_x);
            axis(BS_AXIS_RIGHT_Y, -in.right_stick_y);

            /*
             * Touch, in the space the server announced -- never the
             * console's native size. Dividing by the latter puts every
             * tap wrong by exactly the resolution scale, and it reads as
             * a calibration fault when it is arithmetic. This project
             * has made that mistake once already.
             */
            const int w = link->width.load();
            const int h = link->height.load();
            if (in.ts_pressed && w > 0 && h > 0) {
                const int16_t x = static_cast<int16_t>(in.ts_x * (w - 1));
                const int16_t y = static_cast<int16_t>(in.ts_y * (h - 1));
                SendEvent(link, touching ? BS_INPUT_TOUCH_MOVE : BS_INPUT_TOUCH_DOWN,
                          0, x, y);
                touching = true;
            } else if (touching) {
                SendEvent(link, BS_INPUT_TOUCH_UP, 0, 0, 0);
                touching = false;
            }
        }
        /* TODO(rate): 200 Hz, picked because it is comfortably above the
         * GamePad's own 60 and below anything that would matter. The
         * real answer is whatever PollInput actually updates at, which
         * needs a pad to find out. */
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

}  // namespace

/* --------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1";
    uint16_t port = BS_DEFAULT_PORT;
    int screen = BS_SCREEN_BOTTOM;
    /*
     * Everything except the GamePad.
     *
     * Half of this program can be tested on a desk with no Realtek
     * adapter and no pad: the connection, the decoding, the scaling to
     * the panel and the shape of the loop. The other half cannot be
     * tested at all without hardware. Being able to run the first half
     * on its own is what keeps a fault in it from being blamed on the
     * second.
     */
    bool no_pad = false;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = (i + 1 < argc) ? argv[i + 1] : nullptr;
        if (!strcmp(a, "--host") && next) { host = next; i++; }
        else if (!strcmp(a, "--port") && next) { port = (uint16_t)atoi(next); i++; }
        else if (!strcmp(a, "--no-pad")) { no_pad = true; }
        else if (!strcmp(a, "--screen") && next) {
            screen = strcmp(next, "top") ? BS_SCREEN_BOTTOM : BS_SCREEN_TOP;
            i++;
        } else {
            fprintf(stderr,
                "bs_gamepad -- an emulator's screen, on a real Wii U GamePad\n"
                "\n"
                "  --host ADDR     where the emulator is listening\n"
                "  --port N        default %d\n"
                "  --screen top|bottom\n"
                "  --no-pad        decode and scale, send nothing to a pad\n"
                "\n"
                "Needs a paired GamePad and the driver loaded with\n"
                "disable_ips=1; see WIIU_GAMEPAD.md in rtw88_TSF.\n",
                BS_DEFAULT_PORT);
            return 1;
        }
    }

    Link link;
    std::string err;
    if (!Connect(&link, host, port, &err)) {
        fprintf(stderr, "bs_gamepad: %s\n", err.c_str());
        return 1;
    }
    fprintf(stderr, "bs_gamepad: console %u, %ux%u @%u fps\n",
            link.ack.console, link.ack.width, link.ack.height, link.ack.fps);

    /*
     * TODO(streamer): Start() brings up the whole libdrc stack -- video,
     * audio, cmd and input -- and assumes the AP is already running and
     * the pad already paired. It returns false rather than saying why.
     * Whether that is enough to diagnose from is a question for a
     * machine with a pad on it.
     */
    drc::Streamer streamer;
    if (!no_pad && !streamer.Start()) {
        fprintf(stderr, "bs_gamepad: libdrc would not start -- is the AP up "
                        "and the pad paired?\n");
        return 1;
    }
    if (no_pad)
        fprintf(stderr, "bs_gamepad: no pad, decoding only\n");

    if (screen != BS_SCREEN_BOTTOM) {
        BsScreenChoice ch{};
        ch.screen = static_cast<uint8_t>(screen);
        bs_send_msg(link.conn, BS_MSG_SET_SCREEN, &ch, sizeof(ch), nullptr, 0);
    }

    Video video;
    if (!video.Ensure(&err)) {
        fprintf(stderr, "bs_gamepad: %s\n", err.c_str());
        return 1;
    }
    Audio audio;
    if (link.ack.audio_rate > 0 && !audio.Start(link.ack.audio_channels, &err))
        fprintf(stderr, "bs_gamepad: no sound (%s)\n", err.c_str());

    std::thread input;
    if (!no_pad)
        input = std::thread(InputLoop, &streamer, &link);

    std::vector<uint8_t> buf(BS_MAX_PAYLOAD);
    std::vector<drc::byte> rgba;
    long frames = 0;
    auto last = std::chrono::steady_clock::now();

    while (!g_stop) {
        uint8_t type = 0;
        size_t n = 0;
        if (bs_recv_msg(link.conn, &type, buf.data(), buf.size(), &n) != 0) {
            fprintf(stderr, "bs_gamepad: the stream ended\n");
            break;
        }

        if (type == BS_MSG_VIDEO && n > sizeof(BsVideoHeader)) {
            if (video.Decode(buf.data() + sizeof(BsVideoHeader),
                             n - sizeof(BsVideoHeader), &rgba)) {
                if (!no_pad)
                    streamer.PushVidFrame(&rgba, kPanelW, kPanelH,
                                          drc::PixelFormat::kRGBA);
                frames++;
            }
        } else if (type == BS_MSG_AUDIO && n > sizeof(BsAudioHeader)) {
            if (!no_pad)
                audio.Decode(buf.data() + sizeof(BsAudioHeader),
                             n - sizeof(BsAudioHeader), &streamer);
        } else if (type == BS_MSG_STREAM_INFO && n >= sizeof(BsStreamInfo)) {
            BsStreamInfo si{};
            memcpy(&si, buf.data(), sizeof(si));
            if (si.width > 0 && si.height > 0) {
                link.width = si.width;
                link.height = si.height;
                /* A new size means a new decoder. Writing the number
                 * down and carrying on is what made the Switch client
                 * freeze on a screen change. */
                video.Reset();
                if (!video.Ensure(&err)) {
                    fprintf(stderr, "bs_gamepad: %s\n", err.c_str());
                    break;
                }
            }
        } else if (type == BS_MSG_PING) {
            bs_send_msg(link.conn, BS_MSG_PONG, nullptr, 0, nullptr, 0);
        }
        /*
         * TODO(prompt): BS_MSG_PROMPT is ignored. A 3DS asking for a
         * name has nowhere to go on a GamePad -- unless its own
         * on-screen keyboard is drawn into the picture, which libdrc
         * does not offer and this would have to draw itself.
         */

        const auto now = std::chrono::steady_clock::now();
        if (now - last >= std::chrono::seconds(1)) {
            fprintf(stderr, "[bs_gamepad] %ld frames/s\n", frames);
            frames = 0;
            last = now;
        }
    }

    g_stop = true;
    if (input.joinable()) input.join();
    if (!no_pad) streamer.Stop();
    bs_conn_close(link.conn);
    return 0;
}
