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
 * The receive loop decodes A/V while libdrc clocks radio transmission.
 * Input and reassociation run independently, so a WPA handshake cannot
 * accumulate stale frames or seconds of sound in the TCP receive queue.
 *
 * The GamePad's panel is 864x480. A Wii U GamePad screen out of Cemu is
 * 848x480 and a 3DS is 400x240, so something always has to scale; doing
 * it here rather than asking the server for 864 keeps the server's sizes
 * whole multiples of the console's own screen, which is a rule worth
 * more than one client's convenience.
 */

#include "pad_menu.h"
#include <drc/input.h>
#include <drc/pixel-format.h>
#include <drc/streamer.h>

#include <atomic>
#include <csignal>
#include <glob.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <cerrno>
#include <sstream>
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

/*
 * The native shape of one screen of one console.
 *
 * Not the size on the wire -- an emulator renders at a multiple of this
 * and the ceiling is 1440 tall -- but the shape, which is all that a
 * letterbox needs and the one thing the handshake cannot say about a
 * screen nobody has asked for yet.
 *
 * A DS is 4:3 twice. A Wii U is 16:9 twice. A 3DS is the odd one: 4:3
 * below and 5:3 above, which is why the request has to be recomputed
 * when the screen changes rather than kept from the handshake.
 */
void NativeShape(int console, int screen, int *w, int *h)
{
    const bool top = screen == BS_SCREEN_TOP;
    switch (console) {
    case BS_CONSOLE_DS:
        *w = top ? BS_DS_TOP_WIDTH : BS_DS_WIDTH;
        *h = top ? BS_DS_TOP_HEIGHT : BS_DS_HEIGHT;
        break;
    case BS_CONSOLE_3DS:
        *w = top ? BS_3DS_TOP_WIDTH : BS_3DS_WIDTH;
        *h = top ? BS_3DS_TOP_HEIGHT : BS_3DS_HEIGHT;
        break;
    default:
        *w = top ? BS_WIIU_TOP_WIDTH : BS_WIIU_WIDTH;
        *h = top ? BS_WIIU_TOP_HEIGHT : BS_WIIU_HEIGHT;
        break;
    }
}

/*
 * The largest box of that shape which fits the panel, in whole
 * macroblocks -- what the server is asked to encode.
 *
 * Asking for the panel itself whatever the console is makes the server
 * do the stretching, and a 4:3 screen then arrives a fifth too wide
 * with no way back: the circles are ovals before this program sees
 * them.
 */
void PanelFit(int sw, int sh, int *rw, int *rh)
{
    int w = kPanelW, h = kPanelH;
    if (sw > 0 && sh > 0) {
        if ((long long)sw * kPanelH > (long long)sh * kPanelW) {
            w = kPanelW;
            h = (int)((long long)kPanelW * sh / sw);
        } else {
            h = kPanelH;
            w = (int)((long long)kPanelH * sw / sh);
        }
        w = ((w + 8) / 16) * 16;
        h = ((h + 8) / 16) * 16;
        if (w > kPanelW) w = kPanelW;
        if (h > kPanelH) h = kPanelH;
    }
    *rw = w;
    *rh = h;
}

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
    /* Which screen is being watched. Touch belongs to the bottom one
     * only -- a top screen has no digitiser to pretend to be. */
    std::atomic<int> screen{BS_SCREEN_BOTTOM};
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

/* ----------------------------------------------- waking the decoder */

/*
 * Deauthenticate the pad, and wait for it to come back.
 *
 * The GamePad's decoder holds the last frame it understood. Point a new
 * stream at it without dropping the association first and it sits on
 * that frame for ever -- it is not waiting for a keyframe, it has
 * stopped listening. Dropping the association makes it start again from
 * nothing, which is the only thing that reliably moves it.
 *
 * Through hostapd's control socket, which needs no root: the socket
 * belongs to a group, and anybody who can run the AP is already in it.
 *
 * Failure is reported to the receive loop: continuing would leave a pad
 * holding the previous source and make the launcher claim it was working.
 */
class PadLink {
public:
    PadLink(const char *cli, const char *iface) : cli_(cli ? cli : ""), iface_(iface ? iface : "") {}

    std::string Station() const
    {
        std::istringstream lines(Run({"all_sta"}));
        std::string line;
        while (std::getline(lines, line)) {
            unsigned int m[6];
            int n = 0;
            if (sscanf(line.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x%n",
                       &m[0], &m[1], &m[2], &m[3], &m[4], &m[5], &n) == 6
                && n == 17 && line.size() == 17) return line;
        }
        return "";
    }

    bool Cycle(int wait_seconds)
    {
        const std::string mac = Station();
        if (mac.empty()) {
            fprintf(stderr, "bs_gamepad: no associated pad or inaccessible hostapd control socket\n");
            return false;
        }
        fprintf(stderr, "bs_gamepad: deauthenticating %s\n", mac.c_str());
        if (Run({"deauthenticate", mac}).find("OK") == std::string::npos) {
            fprintf(stderr, "bs_gamepad: deauthentication failed\n");
            return false;
        }
        // hostapd removes the old association before acknowledging deauth.
        // Wait for WPA authorization, not just a station entry or a strictly
        // smaller connected_time (which fails when it was already zero).
        for (int i = 0; i < wait_seconds * 5 && !g_stop; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            std::string state = Run({"sta", mac});
            if (state.find("[AUTHORIZED]") != std::string::npos) {
                fprintf(stderr, "bs_gamepad: WPA reauthorized after %.1fs\n", (i + 1) * .2);
                std::this_thread::sleep_for(std::chrono::seconds(1));
                return !g_stop;
            }
        }
        fprintf(stderr, "bs_gamepad: reauthorization timed out\n");
        return false;
    }

private:
    std::string Run(std::initializer_list<std::string> args) const
    {
        if (cli_.empty() || iface_.empty()) return "";
        std::vector<std::string> words{cli_, "-p", "/var/run/hostapd", "-i", iface_};
        words.insert(words.end(), args.begin(), args.end());
        std::vector<char *> argv;
        for (auto &word : words) argv.push_back(&word[0]);
        argv.push_back(nullptr);
        int fd[2];
        if (pipe(fd)) return "";
        const pid_t parent = getpid();
        pid_t child = fork();
        if (child == 0) {
            prctl(PR_SET_PDEATHSIG, SIGKILL);
            if (getppid() != parent) _exit(127);
            dup2(fd[1], STDOUT_FILENO);
            close(fd[0]); close(fd[1]);
            execvp(argv[0], argv.data());
            _exit(127);
        }
        close(fd[1]);
        std::string out;
        if (child > 0) {
            char buf[1024];
            ssize_t n;
            while ((n = read(fd[0], buf, sizeof(buf))) > 0) out.append(buf, n);
            int status;
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        }
        close(fd[0]);
        return out;
    }

    std::string cli_, iface_;
};

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
    /*
     * The shape to letterbox to, which is the console's and not the
     * arriving frame's.
     *
     * They differ because a request is rounded to whole macroblocks:
     * three quarters of a 4:3 box is 480x360, and 360 is not a multiple
     * of sixteen, so 480x368 arrives -- 1.30 where the screen is 1.33.
     * Letterboxing to what arrives carries that 2% into the picture;
     * letterboxing to the console's own shape does not, and costs a
     * stretch of eight pixels nobody can see.
     */
    void SetShape(int w, int h)
    {
        if (w == shape_w_ && h == shape_h_) return;
        shape_w_ = w; shape_h_ = h;
        if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
        src_w_ = src_h_ = 0;
    }

    /* Rebuilds the scaler if the menu's choice changed under it. */
    void SetFilter(int filter)
    {
        if (filter == filter_) return;
        filter_ = filter;
        if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
        src_w_ = src_h_ = 0;
    }

    bool Decode(const uint8_t *data, size_t size, std::vector<drc::byte> *rgba)
    {
        BsDecodedFrame f{};
        if (bs_decoder_decode(dec_, data, size, &f) <= 0)
            return false;

        if (f.width != src_w_ || f.height != src_h_) {
            if (sws_) sws_freeContext(sws_);

            /*
             * The picture keeps its shape; the panel gets black beside
             * it.
             *
             * The panel is 864x480, which is 16:9. A Wii U GamePad
             * screen is the same shape and fills it. A DS is 256x192 and
             * a 3DS touch screen is 320x240 -- both 4:3 -- and stretching
             * either across a 16:9 panel makes everything a fifth too
             * wide. Circles become ovals and it is obvious the moment
             * you look at it.
             *
             * So the largest rectangle of the right shape that fits,
             * centred, and the rest left black. 4:3 into this panel is
             * 640x480 with 112 pixels of black down each side.
             *
             * Rounded to even numbers because the conversion is to
             * 4:2:0 and an odd width has half a chroma sample at the
             * edge, which swscale is entitled to dislike.
             */
            const int aw = shape_w_ > 0 ? shape_w_ : f.width;
            const int ah = shape_h_ > 0 ? shape_h_ : f.height;
            int w = kPanelW, h = kPanelH;
            if ((long long)aw * kPanelH != (long long)ah * kPanelW) {
                if ((long long)aw * kPanelH > (long long)ah * kPanelW) {
                    w = kPanelW;
                    h = (int)((long long)kPanelW * ah / aw);
                } else {
                    h = kPanelH;
                    w = (int)((long long)kPanelH * aw / ah);
                }
                w &= ~1;
                h &= ~1;
            }
            fit_w_ = w;
            fit_h_ = h;
            fit_x_ = (kPanelW - w) / 2 & ~1;
            fit_y_ = (kPanelH - h) / 2 & ~1;

            sws_ = sws_getContext(f.width, f.height, AV_PIX_FMT_YUV420P,
                                  w, h, AV_PIX_FMT_RGBA,
                                  (f.width == w && f.height == h)
                                      ? SWS_POINT      /* colour only */
                                      : ImageScaleFlags(filter_),
                                  nullptr, nullptr, nullptr);
            src_w_ = f.width;
            src_h_ = f.height;

            if (said_ != std::pair<int,int>(w, h)) {
                said_ = {w, h};
            if (w == kPanelW && h == kPanelH)
                fprintf(stderr, "bs_gamepad: %dx%d fills the panel%s\n",
                        f.width, f.height,
                        (f.width == kPanelW) ? " -- converting colour only" : "");
            else
                fprintf(stderr, "bs_gamepad: %dx%d is %d:%d, drawn as %dx%d at "
                                "%d,%d with black beside it\n",
                        f.width, f.height,
                        aw / Gcd(aw, ah), ah / Gcd(aw, ah),
                        w, h, fit_x(), fit_y());
            }
        }
        if (!sws_) return false;

        rgba->assign(static_cast<size_t>(kPanelW) * kPanelH * 4, 0);

        const uint8_t *planes[4] = { f.y, f.u, f.v, nullptr };
        int strides[4] = { f.y_stride, f.u_stride, f.v_stride, 0 };
        uint8_t *base = reinterpret_cast<uint8_t *>(rgba->data()) +
                        (static_cast<size_t>(fit_y()) * kPanelW + fit_x()) * 4;
        uint8_t *dst[4] = { base, nullptr, nullptr, nullptr };
        int dst_stride[4] = { kPanelW * 4, 0, 0, 0 };
        sws_scale(sws_, planes, strides, 0, f.height, dst, dst_stride);
        return true;
    }

    /* Only to say the shape out loud in the line above. */
    static int Gcd(int a, int b) { return b ? Gcd(b, a % b) : (a ? a : 1); }

    /* Where the picture sits on the panel, for anything that has to aim
     * at it -- a touch, or a menu drawn beside it. Atomic because the
     * input thread reads them while this one is rebuilding a scaler. */
    int fit_x() const { return fit_x_.load(std::memory_order_relaxed); }
    int fit_y() const { return fit_y_.load(std::memory_order_relaxed); }
    int fit_w() const { return fit_w_.load(std::memory_order_relaxed); }
    int fit_h() const { return fit_h_.load(std::memory_order_relaxed); }

private:
    BsDecoder *dec_ = nullptr;
    SwsContext *sws_ = nullptr;
    int src_w_ = 0, src_h_ = 0;
    int filter_ = 2;            /* Lanczos, which is what it always was */
    int shape_w_ = 0, shape_h_ = 0;
    std::pair<int,int> said_{0, 0};   /* the last rectangle announced */
    std::atomic<int> fit_x_{0}, fit_y_{0};
    std::atomic<int> fit_w_{kPanelW}, fit_h_{kPanelH};
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
        if (frames <= 0) {
            fprintf(stderr, "bs_gamepad: Opus decode failed: %s\n", opus_strerror(frames));
            return;
        }
        packets_++;
        samples_ += frames;
        for (int i = 0; i < frames * channels_; ++i)
            peak_ = std::max(peak_, abs(static_cast<int>(pcm[i])));

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

    void Report()
    {
        fprintf(stderr, "[bs_audio] %ld packets, %ld samples, peak %d\n", packets_, samples_, peak_);
        packets_ = samples_ = 0; peak_ = 0;
    }

private:
    long packets_ = 0, samples_ = 0;
    int peak_ = 0;
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

void InputLoop(drc::Streamer *streamer, Link *link, PadMenu *menu, Video *video)
{
    uint32_t held = 0;
    bool touching = false;
    int last_axis[4] = {0,0,0,0};

    while (!g_stop) {
        drc::InputData in;
        streamer->PollInput(&in);
        menu->Filter(in);
        if (!in.valid) { in = drc::InputData(); in.valid = true; }
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

            // Forward corrected axes only when their quantized value changes.
            auto axis = [&](int code, float v) {
                int val = static_cast<int>(v * 32767.0f);
                if (val > 32767) val = 32767;
                if (val < -32767) val = -32767;
                if (val == last_axis[code-BS_AXIS_LEFT_X]) return;
                last_axis[code-BS_AXIS_LEFT_X] = val;
                SendEvent(link, BS_INPUT_AXIS, static_cast<uint8_t>(code),
                          static_cast<int16_t>(val), 0);
            };
            /*
             * Not negated. libdrc already reports a stick the way this
             * protocol wants it -- up is positive on both sides -- and
             * flipping it here made both sticks answer up with down.
             * The screen's y grows downward and a stick's does not, and
             * that is a rule about screens, not about sticks.
             */
            axis(BS_AXIS_LEFT_X, in.left_stick_x);
            axis(BS_AXIS_LEFT_Y, in.left_stick_y);
            axis(BS_AXIS_RIGHT_X, in.right_stick_x);
            axis(BS_AXIS_RIGHT_Y, in.right_stick_y);

            /*
             * Touch, in the space the server announced -- never the
             * console's native size. Dividing by the latter puts every
             * tap wrong by exactly the resolution scale, and it reads as
             * a calibration fault when it is arithmetic. This project
             * has made that mistake once already.
             */
            const int w = link->width.load();
            const int h = link->height.load();
            /*
             * And in the part of the panel the picture actually
             * occupies. A 4:3 screen is drawn 640 wide with 112 pixels
             * of black down each side, so a touch read across the whole
             * 864 lands a fifth of the way off and gets worse towards
             * the edges -- the same arithmetic mistake as the one
             * above, one letterbox further along. A touch in the black
             * is not in the picture at all, so it is dropped.
             */
            const int fx = video->fit_x(), fw = video->fit_w();
            const int fy = video->fit_y(), fh = video->fit_h();
            float px = fw > 0 ? (in.ts_x * kPanelW - fx) / fw : -1.f;
            float py = fh > 0 ? (in.ts_y * kPanelH - fy) / fh : -1.f;
            const bool inside = px >= 0 && px <= 1 && py >= 0 && py <= 1;
            /* The top screen has no digitiser to pretend to be. */
            const bool touchable = link->screen.load() == BS_SCREEN_BOTTOM;
            if (in.ts_pressed && inside && touchable && w > 0 && h > 0) {
                const int16_t x = static_cast<int16_t>(px * (w - 1));
                const int16_t y = static_cast<int16_t>(py * (h - 1));
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
    bool deauth = true;
    const char *iface = getenv("BS_PAD_IFACE");
    const char *cli = getenv("BS_HOSTAPD_CLI");
    /* What the server should encode at before libdrc re-encodes it.
     * Twelve megabits at 864x480 is more than the picture needs, which
     * is the point: whatever is lost here is lost twice. */
    int bitrate = 12000000;
    /* -1 means: leave whatever the settings file holds. */
    int res_bottom = -1, res_top = -1, filter = -1, sharpness = -1;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = (i + 1 < argc) ? argv[i + 1] : nullptr;
        if (!strcmp(a, "--host") && next) { host = next; i++; }
        else if (!strcmp(a, "--port") && next) { port = (uint16_t)atoi(next); i++; }
        else if (!strcmp(a, "--no-pad")) { no_pad = true; }
        else if (!strcmp(a, "--no-deauth")) { deauth = false; }
        else if (!strcmp(a, "--bitrate") && next) { bitrate = atoi(next); i++; }
        else if (!strcmp(a, "--iface") && next) { iface = next; i++; }
        else if (!strcmp(a, "--hostapd-cli") && next) { cli = next; i++; }
        /*
         * The picture, as the GamePad sees it.
         *
         * libdrc reads these from the environment, so this sets them
         * rather than passing them -- which also means anything already
         * in the environment is left alone, and a value given here wins
         * over the default but not over one exported by hand.
         *
         * They are worth having on the command line because the one
         * that matters is not obvious: the preset decides whether the
         * pad can decode a game at all, and the difference between
         * medium and fast is a frozen picture and a clean one.
         */
        else if (!strcmp(a, "--preset") && next)  { setenv("DRC_PRESET", next, 1); i++; }
        else if (!strcmp(a, "--qp") && next)      { setenv("DRC_QP", next, 1); i++; }
        else if (!strcmp(a, "--keyint") && next)  { setenv("DRC_KEYINT", next, 1); i++; }
        else if (!strcmp(a, "--refresh") && next) { setenv("DRC_REFRESH", next, 1); i++; }
        else if (!strcmp(a, "--deblock") && next) { setenv("DRC_DEBLOCK", next, 1); i++; }
        else if (!strcmp(a, "--aq") && next)      { setenv("DRC_AQ", next, 1); i++; }
        else if (!strcmp(a, "--stats"))           { setenv("DRC_STATS", "1", 1); }
        else if (!strcmp(a, "--resolution") && next)     { res_bottom = atoi(next); i++; }
        else if (!strcmp(a, "--top-resolution") && next) { res_top = atoi(next); i++; }
        else if (!strcmp(a, "--filter") && next)         { filter = atoi(next); i++; }
        else if (!strcmp(a, "--sharpness") && next)      { sharpness = atoi(next); i++; }
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
                "  --no-deauth     do not drop the pad's association first\n"
                "  --bitrate N     what the server should encode at, before\n"
                "                  libdrc encodes it again (default 12M)\n"
                "  --resolution N      0..4: x1/2, x3/4, x1, x3/2, x2 of the box\n"
                "  --top-resolution N  the same, for the top screen\n"
                "  --filter N          0..3: bilinear, bicubic, Lanczos, nearest\n"
                "  --sharpness N       0..4, quarter steps\n"
                "  --iface NAME    the AP's interface (or BS_PAD_IFACE)\n"
                "  --hostapd-cli P where hostapd_cli is (or BS_HOSTAPD_CLI)\n"
                "\n"
                "The picture, as the pad sees it:\n"
                "  --preset NAME   x264 preset; fast by default because\n"
                "                  medium asks for a keyframe every frame\n"
                "                  on a game, and the picture then freezes\n"
                "  --qp N          quantiser; the protocol assumes 32 and\n"
                "                  anything else decodes as noise\n"
                "  --keyint N      frames between keyframes\n"
                "  --refresh N     intra refresh period, 0 to turn it off\n"
                "  --deblock 0|1   the deblocking filter\n"
                "  --aq N          adaptive quantisation mode\n"
                "  --stats         one line a second; resync is the number\n"
                "                  that says whether the pad can decode\n"
                "\n"
                "Needs a paired GamePad and the driver loaded with\n"
                "disable_ips=1; see WIIU_GAMEPAD.md in rtw88_TSF.\n",
                BS_DEFAULT_PORT);
            return 1;
        }
    }

    /*
     * TODO(streamer): Start() brings up the whole libdrc stack -- video,
     * audio, cmd and input -- and assumes the AP is already running and
     * the pad already paired. It returns false rather than saying why.
     * Whether that is enough to diagnose from is a question for a
     * machine with a pad on it.
     */
    /*
     * The encoder preset, chosen because the GamePad cannot decode what
     * a heavier one produces from real video.
     *
     * Measured against a running game, twenty seconds each, counting
     * the keyframe requests the pad sends when it cannot decode:
     *
     *     medium     60 resync/s   -- a keyframe asked for every frame
     *     fast        0
     *     veryfast    0            -- but 8 packets an image, not 5
     *     ultrafast   0
     *
     * libdrc's own note says medium or below, which was measured on a
     * flat test pattern; this project's picture is a game, and a game is
     * heavier. The same bridge fed the built-in test pattern decodes
     * perfectly at medium, which is what makes this a property of the
     * content rather than of this code.
     *
     * setenv without overwriting, so DRC_PRESET set by hand still wins.
     */
    setenv("DRC_PRESET", "fast", 0);

    // A source switch requires a new association while the new streamer
    // is already alive, as in rtw88_TSF's drc_lab. In particular, audio
    // stays silent if its transport only starts after reauthorization.
    std::signal(SIGTERM, [](int) { _exit(0); });
    std::signal(SIGINT, [](int) { _exit(0); });
    std::string detected_iface;
    if (!iface) {
        glob_t sockets{};
        if (glob("/var/run/hostapd/*", 0, nullptr, &sockets) == 0 && sockets.gl_pathc == 1) {
            const char *name = strrchr(sockets.gl_pathv[0], '/');
            detected_iface = name + 1;
            iface = detected_iface.c_str();
        }
        globfree(&sockets);
    }
    std::string detected_cli;
    if (!cli) {
        const char *home = getenv("HOME");
        detected_cli = std::string(home ? home : "") + "/rtw88_TSF/drc-hostap/hostapd/hostapd_cli";
        cli = access(detected_cli.c_str(), X_OK) == 0 ? detected_cli.c_str() : "hostapd_cli";
    }
    if (!no_pad && deauth && !iface) {
        fprintf(stderr, "bs_gamepad: cannot reset pad; check --iface and --hostapd-cli\n");
        return 1;
    }

    Link link;
    std::string err;
    if (!Connect(&link, host, port, &err)) {
        fprintf(stderr, "bs_gamepad: %s\n", err.c_str());
        return 1;
    }
    fprintf(stderr, "bs_gamepad: console %u, %ux%u @%u fps\n",
            link.ack.console, link.ack.width, link.ack.height, link.ack.fps);

    drc::Streamer streamer;
    /*
     * Installed again here, and unblocked, because libdrc's threads are
     * started by the call below and the mask they are created with is
     * this thread's.
     *
     * A bridge that had been streaming for a while stopped answering
     * SIGTERM entirely: it kept libdrc's UDP ports, pushed nothing, and
     * needed SIGKILL -- which on a pad looks like the picture simply
     * stopping and never coming back, with no replacement able to
     * start. Whoever ends up owning the signal, the main thread has to
     * be able to take it.
     */
    if (!no_pad && !streamer.Start()) {
        fprintf(stderr, "bs_gamepad: libdrc would not start -- is the AP up "
                        "and the pad paired?\n");
        return 1;
    }
    if (no_pad)
        fprintf(stderr, "bs_gamepad: no pad, decoding only\n");

    /*
     * Ask for the size this panel will actually draw, and for a generous
     * bitrate.
     *
     * Two lossy passes sit between the game and the pad: the server's
     * encoder, and libdrc's at a quantiser the protocol pins to 32. The
     * second cannot be moved, so everything handed to it should be as
     * clean as the first can make it.
     *
     * The size matters more than it sounds. Left alone, a Wii U GamePad
     * screen arrives at 848x480 and is resampled twice -- once by the
     * server into 848, once here into the panel -- and the second is an
     * upscale of an already compressed picture, which is where softness
     * and blocking come from. Asking for the size that will be drawn
     * leaves the bridge only a colour conversion to do.
     *
     * The bitrate is asked for rather than assumed because the server
     * derives one from the size and caps it at six megabits. On a link
     * that is usually a loopback, there is no reason to be shy.
     */
    /*
     * Both are sent per screen rather than once, because each screen has
     * its own encoder on the server, built on the server's defaults: one
     * that has just been joined has never heard of this client's size or
     * bitrate. And on a 3DS the size is not even the same number -- 4:3
     * below, 5:3 above -- so a request kept from the handshake would
     * stretch the top screen into the bottom one's box.
     */
    /*
     * Built before the first request, not after it: the menu is where
     * the saved resolution and bitrate live, and a request made before
     * it exists is a request at the defaults. The setting then only
     * took effect the first time somebody changed it by hand.
     */
    PadMenu menu;
    menu.Override(res_bottom, res_top, filter, sharpness);
    menu.SetScreen(screen == BS_SCREEN_TOP ? 1 : 0);
    ImageSettings image = menu.Image();
    int shape_w = 0, shape_h = 0;   /* filled in by request(), read by the decoder */
    auto request = [&](int want, const ImageSettings &img) {
        int sw = 0, sh = 0, fw = 0, fh = 0;
        NativeShape(link.ack.console, want, &sw, &sh);
        PanelFit(sw, sh, &fw, &fh);

        /*
         * The menu's multiplier, applied to the box this screen gets on
         * the panel. Above 1 it is supersampling: the server renders
         * and encodes more than the panel can show and the scaler here
         * brings it down, which beats asking the server's scaler for
         * the same picture -- but only while the emulator is rendering
         * above the panel, and the server's own 1440 ceiling still
         * applies whatever is asked here.
         */
        int rw = fw * img.Num() / img.Den();
        int rh = fh * img.Num() / img.Den();
        rw = ((rw + 8) / 16) * 16;
        rh = ((rh + 8) / 16) * 16;
        if (rh > BS_MAX_STREAM_HEIGHT) {
            rw = (int)((long long)rw * BS_MAX_STREAM_HEIGHT / rh);
            rh = BS_MAX_STREAM_HEIGHT;
        }
        if (rw < 16) rw = 16;
        if (rh < 16) rh = 16;

        fprintf(stderr, "bs_gamepad: asking for %dx%d, which is %dx%d's shape "
                        "at %sthe panel's size\n", rw, rh, sw, sh,
                img.Num() == img.Den() ? "" :
                img.Num() > img.Den() ? "more than " : "less than ");

        BsSize sz;
        sz.width = static_cast<uint16_t>(rw);
        sz.height = static_cast<uint16_t>(rh);
        bs_send_msg(link.conn, BS_MSG_SET_SIZE, &sz, sizeof(sz), nullptr, 0);

        /* Auto means the one the command line gave. */
        BsQuality q{};
        q.bitrate = static_cast<uint32_t>(img.Bitrate() ? img.Bitrate() : bitrate);
        bs_send_msg(link.conn, BS_MSG_SET_QUALITY, &q, sizeof(q), nullptr, 0);
        shape_w = sw;
        shape_h = sh;
        return std::pair<int,int>(fw, fh);
    };

    auto go_to = [&](int want) {
        screen = want;
        link.screen = want;
        BsScreenChoice ch{};
        ch.screen = static_cast<uint8_t>(want);
        bs_send_msg(link.conn, BS_MSG_SET_SCREEN, &ch, sizeof(ch), nullptr, 0);
        return request(want, image);
    };

    /*
     * Through the same path as a later change, and in that order: the
     * server files a size under whichever screen the client is on when
     * it arrives, so a size sent before the screen sizes the screen
     * being left. Starting on the top screen sized the bottom one and
     * left the top at whatever it already was.
     */
    auto fit = go_to(screen);

    {
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, SIGTERM);
        sigaddset(&unblock, SIGINT);
        pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr);
        std::signal(SIGTERM, [](int) { _exit(0); });
        std::signal(SIGINT, [](int) { _exit(0); });
    }

    /*
     * A receive that gives up, so that silence is something the loop
     * can see.
     *
     * Blocking for ever was right while the only way to stop receiving
     * was the server going away, which closes the socket. It is not
     * right for a stream that simply stops: a pump left asleep, an
     * emulator paused, a screen whose source was detached. The socket
     * stays open and empty and the picture on the pad stays frozen with
     * nothing anywhere saying why.
     *
     * One second, and the message boundary is safe because the timeout
     * is cleared for the rest of a message once its first byte has
     * arrived -- a partial read that gave up here would leave the
     * stream one header out of step, which is worse than the freeze.
     */
    bs_conn_set_idle_timeout(link.conn, 1000);

    Video video;
    video.SetShape(shape_w, shape_h);
    if (!video.Ensure(&err)) {
        fprintf(stderr, "bs_gamepad: %s\n", err.c_str());
        return 1;
    }
    Audio audio;
    if (link.ack.audio_rate > 0 && !audio.Start(link.ack.audio_channels, &err))
        fprintf(stderr, "bs_gamepad: no sound (%s)\n", err.c_str());

    std::thread reset;
    std::atomic<bool> reset_failed{false};
    bool reset_started = false;
    menu.SetFit(fit.first, fit.second);
    std::mutex frame_mutex;
    std::vector<drc::byte> latest_frame;
    std::thread output;
    std::thread input;
    if (!no_pad) {
        input = std::thread(InputLoop, &streamer, &link, &menu, &video);
        // The local menu must remain usable when the emulator stops producing
        // frames. It is composited into the same running libdrc stream.
        output = std::thread([&] {
            auto next = std::chrono::steady_clock::now();
            while (!g_stop) {
                std::vector<drc::byte> frame;
                { std::lock_guard<std::mutex> lock(frame_mutex); frame = latest_frame; }
                /* Before the menu is drawn, so the menu's own text is
                 * never sharpened along with the game. */
                ProcessImage(frame, menu.Image());
                menu.Draw(frame);
                streamer.PushVidFrame(&frame,kPanelW,kPanelH,drc::PixelFormat::kRGBA);
                next += std::chrono::microseconds(16683);
                auto now = std::chrono::steady_clock::now();
                if (next < now) next = now;
                std::this_thread::sleep_until(next);
            }
        });
    }

    std::vector<uint8_t> buf(BS_MAX_PAYLOAD);
    std::vector<drc::byte> rgba;
    /* Seconds of silence before each step of getting the picture back. */
    constexpr int kQuietAsk = 2, kQuietRetry = 4, kQuietGiveUp = 30;
    int quiet = 0;
    long frames = 0;
    auto last = std::chrono::steady_clock::now();

    while (!g_stop && !reset_failed) {
        uint8_t type = 0;
        size_t n = 0;
        if (bs_recv_msg(link.conn, &type, buf.data(), buf.size(), &n) != 0) {
            if (bs_conn_timed_out(link.conn)) {
                /*
                 * Nothing for a second. Ask once, gently: a keyframe
                 * costs one packet and covers the common case of a
                 * decoder that lost its reference.
                 *
                 * If that changes nothing, say the screen and the size
                 * again. That is what wakes a server whose pump went to
                 * sleep on a screen change, which is the fault this was
                 * written for -- and it is harmless when the silence had
                 * another cause, because it is what a client says on
                 * arrival anyway.
                 */
                if (++quiet == kQuietAsk) {
                    fprintf(stderr, "bs_gamepad: nothing for %d s, "
                                    "asking for a keyframe\n", kQuietAsk);
                    bs_send_msg(link.conn, BS_MSG_REQUEST_KEYFRAME,
                                nullptr, 0, nullptr, 0);
                } else if (quiet == kQuietRetry) {
                    fprintf(stderr, "bs_gamepad: still nothing, asking for "
                                    "the %s screen again\n",
                            screen == BS_SCREEN_TOP ? "top" : "bottom");
                    go_to(screen);
                } else if (quiet >= kQuietGiveUp) {
                    fprintf(stderr, "bs_gamepad: %d s of silence, giving up "
                                    "on this connection\n", kQuietGiveUp);
                    break;
                }
                continue;
            }
            fprintf(stderr, "bs_gamepad: the stream ended\n");
            break;
        }
        if (quiet) {
            if (quiet >= kQuietAsk)
                fprintf(stderr, "bs_gamepad: the picture is back\n");
            quiet = 0;
        }

        if (type == BS_MSG_VIDEO && n > sizeof(BsVideoHeader)) {
            if (video.Decode(buf.data() + sizeof(BsVideoHeader),
                             n - sizeof(BsVideoHeader), &rgba)) {
                if (!no_pad) {
                    std::lock_guard<std::mutex> lock(frame_mutex);
                    latest_frame.swap(rgba);
                }
                if (frames++ == 0)
                    fprintf(stderr, "bs_gamepad: streaming %s screen\n",
                            screen == BS_SCREEN_TOP ? "top" : "bottom");
                if (!no_pad && deauth && !reset_started && frames >= 3) {
                    reset_started = true;
                    // Keep receiving and feeding both clocks during the
                    // handshake; sleeping here would queue stale A/V in TCP.
                    reset = std::thread([&] {
                        reset_failed = !PadLink(cli, iface).Cycle(20);
                    });
                }
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

        /* The menu only records the choice; the connection is owned
         * here, so the messages go out here. */
        int want = 0;
        if (menu.TakeScreenChange(&want)) {
            const int target = want ? BS_SCREEN_TOP : BS_SCREEN_BOTTOM;
            if (target != screen) {
                fit = go_to(target);
                menu.SetFit(fit.first, fit.second);
            }
        }
        /* A resolution or a bitrate chosen in the menu is a new request
         * on the same screen. Only when it actually changed: each one
         * rebuilds an encoder that every viewer of this screen shares. */
        const ImageSettings now_image = menu.Image();
        if (now_image.detail != image.detail ||
            now_image.bitrate != image.bitrate) {
            image = now_image;
            fit = request(screen, image);
            menu.SetFit(fit.first, fit.second);
        } else {
            image = now_image;
        }
        video.SetFilter(image.filter);
        video.SetShape(shape_w, shape_h);

        /*
         * A line a second for as long as it runs is not information, it
         * is a wall -- and it buried the two lines that matter, the
         * freeze and the recovery. The rate is still there behind
         * --stats, where somebody is looking for it.
         */
        const auto now = std::chrono::steady_clock::now();
        if (now - last >= std::chrono::seconds(1)) {
            if (getenv("DRC_STATS")) {
                fprintf(stderr, "[bs_gamepad] %ld frames/s\n", frames);
                audio.Report();
            }
            frames = 0;
            last = now;
        }
    }

    g_stop = true;
    if (reset.joinable()) reset.join();
    if (input.joinable()) input.join();
    if (output.joinable()) output.join();
    if (!no_pad) streamer.Stop();
    bs_conn_close(link.conn);
    return reset_failed ? 1 : 0;
}
