#pragma once
#include <drc/input.h>
#include "image_settings.h"
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

class PadMenu {
public:
    explicit PadMenu(std::string path = DefaultPath());
    static std::string DefaultPath();
    // Mutates a copy for forwarding; raw samples remain available in the menu.
    void Filter(drc::InputData &in);
    void Draw(std::vector<unsigned char> &rgba);
    ImageSettings Image();
    // Which screen the menu is showing as chosen, and whether that
    // choice has changed since the caller last asked. The menu does not
    // talk to the server itself; the loop that owns the connection does.
    void SetScreen(int screen);
    // The box this console's screen gets on the panel, so the
    // resolution row can name real pixels instead of a bare multiplier.
    void SetFit(int w, int h);
    /*
     * Starting values from the command line, which the launcher passes.
     * They override what was saved but are not saved themselves: an
     * override chosen for one run should not quietly become the file's
     * new contents. The menu still owns every later change.
     */
    void Override(int bottom_res, int top_res, int filter, int sharpness);
    bool TakeScreenChange(int *screen);
    static void Correct(float &x, float &y, float cx, float cy, float deadzone);
private:
    void Save();
    void Tap(float x, float y);
    std::mutex mutex_;
    std::string path_, message_ = "Relâchez le stick, puis calibrez son centre.";
    ImageSettings image_;
    bool image_page_ = false;
    int screen_ = 0;            // 0 bottom, 1 top, as the protocol numbers them
    int fit_w_ = 864, fit_h_ = 480;
    /*
     * The resolution is per screen, because the two are different
     * pictures with different encoders behind them: a 3DS top screen
     * carries the game and the touch screen carries a map, and there is
     * no reason for one to pay for the other's sharpness.
     */
    int detail_[2] = {2, 2};
    bool screen_changed_ = false;
    int corner_ = 1; // TL, TR, BL, BR; eight physical screen pixels
    // Both sticks, indexed kLeft and kRight. The right one starts at 12%
    // because this pad's right stick drifts; the left has never needed
    // it, so it starts at nothing and changes only when asked.
    static constexpr int kLeft = 0, kRight = 1;
    int stick_ = kRight; // the one the panel is showing
    float cx_[2] = {0,0}, cy_[2] = {0,0}, deadzone_[2] = {0.f, .12f};
    float raw_x_[2] = {0,0}, raw_y_[2] = {0,0};
    float out_x_[2] = {0,0}, out_y_[2] = {0,0};
    bool open_ = false, touched_ = false, consumed_ = false, calibrating_ = false;
    unsigned suppressed_ = 0;
    std::chrono::steady_clock::time_point calibration_start_;
    double sum_x_ = 0, sum_y_ = 0;
    float min_x_ = 1, min_y_ = 1, max_x_ = -1, max_y_ = -1;
    int samples_ = 0;
};
