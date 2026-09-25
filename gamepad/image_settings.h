#pragma once
#include <cstdint>
#include <deque>
#include <vector>

struct ImageSettings {
    int bitrate = 0; // index: automatic, 2, 4, 6, 8, 12, 16, 24 Mbit/s
    /*
     * How much the server is asked to render, as a fraction of the
     * largest box of the console's shape that fits the panel.
     *
     * Above 1 this is supersampling: more pixels arrive than the panel
     * has, and the scaler here brings them down, which is sharper than
     * asking the server's scaler to do the same job -- but only when
     * the emulator is actually rendering above the panel. At its own
     * native resolution there is nothing up there to fetch.
     */
    int detail = 2;  // index into kDetailNum/kDetailDen below
    int filter = 2;  // bilinear, bicubic, Lanczos, nearest -- Lanczos as it always was
    int sharpness = 0; // 0..4, quarter steps
    int delay = 0; // 0..5, 20 ms steps; video only
    bool operator==(const ImageSettings &s) const {
        return bitrate==s.bitrate && detail==s.detail && filter==s.filter &&
               sharpness==s.sharpness && delay==s.delay;
    }
    bool operator!=(const ImageSettings &s) const { return !(*this==s); }
    int Bitrate() const { const int rates[]={0,2,4,6,8,12,16,24}; return rates[bitrate]*1000000; }
    static constexpr int kDetailCount = 5;
    int Num() const { const int n[]={1,3,1,3,2}; return n[detail]; }
    int Den() const { const int d[]={2,4,1,2,1}; return d[detail]; }
};
int ImageScaleFlags(int filter);
void ProcessImage(std::vector<unsigned char> &rgba, const ImageSettings &settings);

// Bounded history, independent of the menu render clock. No added queue at zero.
class VideoHistory {
public:
    void Push(std::vector<unsigned char> pixels, int64_t time) {
        frames_.push_back({++serial_,time,std::move(pixels)});
        while (frames_.size()>16) frames_.pop_front();
    }
    uint64_t Read(int64_t now, int delay_ms, uint64_t previous,
                  std::vector<unsigned char> &pixels) const {
        if(frames_.empty()) return 0;
        const Frame *selected=&frames_.front();
        for(const auto &frame:frames_) {
            if (frame.time > now-delay_ms && delay_ms) break;
            selected=&frame;
        }
        if(selected->id!=previous) pixels=selected->pixels;
        return selected->id;
    }
private:
    struct Frame { uint64_t id; int64_t time; std::vector<unsigned char> pixels; };
    uint64_t serial_=0;
    std::deque<Frame> frames_;
};
