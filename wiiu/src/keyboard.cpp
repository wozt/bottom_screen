#include "keyboard.h"

#include <cstdio>
#include <cstring>

#include <coreinit/filesystem.h>
#include <coreinit/memdefaultheap.h>
#include <nn/swkbd/swkbd_cpp.h>
#include <padscore/kpad.h>
#include <proc_ui/procui.h>
#include <vpad/input.h>
#include <whb/log.h>
#include "proc.h"
#include "ui.h"

namespace {

/* The keyboard speaks UTF-16 and this program speaks ASCII. Everything
 * it is used for here is an address, a port, or a password, so the
 * conversion is deliberately the narrow one -- anything outside ASCII
 * is dropped rather than mangled into something that looks right. */
void to_utf16(const char *in, char16_t *out, size_t out_chars)
{
    size_t n = 0;
    for (; in && in[n] && n + 1 < out_chars; n++) {
        out[n] = static_cast<char16_t>(static_cast<unsigned char>(in[n]));
    }
    out[n] = 0;
}

size_t from_utf16(const char16_t *in, char *out, size_t out_size)
{
    size_t n = 0;
    for (; in && in[n] && n + 1 < out_size; n++) {
        const char16_t c = in[n];
        out[n] = (c < 0x80) ? static_cast<char>(c) : '?';
    }
    out[n] = '\0';
    return n;
}

} // namespace

int keyboard_prompt(const char *hint, const char *initial, int numeric,
                    char *out, size_t out_size, char *why, size_t why_size)
{
    if (!out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';

    /*
     * GX2 is already up, and that is the whole point.
     *
     * The first version called WHBGfxInit() here, because the menu was
     * OSScreen and something had to bring GX2 up for the keyboard. The
     * menu is SDL2 now, SDL2 owns GX2, and a second init simply fails
     * -- which it did, and put "cannot start the graphics layer" on
     * screen. So this draws inside the frame the program is already
     * running, with a flush between SDL's batched commands and the
     * keyboard's raw ones.
     */
    WHBLogPrintf("swkbd: FS client");
    FSClient *fsClient = static_cast<FSClient *>(MEMAllocFromDefaultHeap(sizeof(FSClient)));
    if (!fsClient) {
        snprintf(why, why_size, "out of memory");
        return -1;
    }
    /* FSInit before a client is added. wut does not do it for you, and
     * swkbd loads its fonts and layouts through this client -- a
     * keyboard with no resources is a keyboard that draws nothing,
     * which is exactly what a black screen looks like. */
    FSInit();
    FSAddClient(fsClient, FS_ERROR_FLAG_NONE);

    nn::swkbd::CreateArg createArg;
    createArg.regionType = nn::swkbd::RegionType::Europe;
    createArg.workMemory = MEMAllocFromDefaultHeap(nn::swkbd::GetWorkMemorySize(0));
    createArg.fsClient = fsClient;
    if (!createArg.workMemory) {
        FSDelClient(fsClient, FS_ERROR_FLAG_NONE);
        MEMFreeToDefaultHeap(fsClient);
        snprintf(why, why_size, "out of memory for the keyboard");
        return -1;
    }

    WHBLogPrintf("swkbd: Create, work memory %u bytes",
                 (unsigned)nn::swkbd::GetWorkMemorySize(0));
    if (!nn::swkbd::Create(createArg)) {
        MEMFreeToDefaultHeap(createArg.workMemory);
        FSDelClient(fsClient, FS_ERROR_FLAG_NONE);
        MEMFreeToDefaultHeap(fsClient);
        snprintf(why, why_size, "the keyboard would not open");
        return -1;
    }

    char16_t initial16[128];
    char16_t hint16[64];
    to_utf16(initial, initial16, sizeof(initial16) / sizeof(*initial16));
    to_utf16(hint, hint16, sizeof(hint16) / sizeof(*hint16));

    nn::swkbd::AppearArg appearArg;
    appearArg.keyboardArg.configArg.languageType = nn::swkbd::LanguageType::English;
    appearArg.keyboardArg.configArg.controllerType = nn::swkbd::ControllerType::DrcGamepad;
    /* Digits and dots for an address and a port: the full keyboard is
     * four taps of wandering to reach a number. */
    appearArg.keyboardArg.configArg.keyboardMode =
        numeric ? nn::swkbd::KeyboardMode::Numpad : nn::swkbd::KeyboardMode::Full;
    appearArg.keyboardArg.configArg.numpadCharRight = u'.';
    appearArg.inputFormArg.type = nn::swkbd::InputFormType::Default;
    appearArg.inputFormArg.initialText = initial16;
    appearArg.inputFormArg.hintText = hint16;
    appearArg.inputFormArg.maxTextLength = static_cast<int32_t>(out_size) - 1;
    appearArg.inputFormArg.higlightInitialText = true;

    WHBLogPrintf("swkbd: AppearInputForm");
    if (!nn::swkbd::AppearInputForm(appearArg)) {
        nn::swkbd::Destroy();
        MEMFreeToDefaultHeap(createArg.workMemory);
        FSDelClient(fsClient, FS_ERROR_FLAG_NONE);
        MEMFreeToDefaultHeap(fsClient);
        snprintf(why, why_size, "the input form would not appear");
        return -1;
    }

    WHBLogPrintf("swkbd: entering its loop");
    int result = 0;   /* cancelled, unless told otherwise */
    VPADStatus last{};
    bool have_sample = false;
    while (proc_running()) {
        VPADStatus vpad;
        VPADReadError verr;
        VPADRead(VPAD_CHAN_0, &vpad, 1, &verr);

        /*
         * A way out that does not depend on the keyboard being visible.
         *
         * This loop used to end only on swkbd's own OK and Cancel
         * buttons -- which are touched, on a keyboard that draws
         * itself. When it drew nothing, there was no way out at all:
         * two black screens, no input path, and a console that had to
         * be held down to power off. Wiiload could not even replace the
         * program, because the program was still the one running.
         *
         * B and MINUS are read here, before anything else, and they
         * always end it. A screen that may not appear must never be the
         * only way to leave.
         */
        if (verr == VPAD_READ_SUCCESS &&
            (vpad.trigger & (VPAD_BUTTON_B | VPAD_BUTTON_MINUS))) {
            WHBLogPrintf("swkbd: escape button, cancelling");
            result = 0;
            break;
        }

        /*
         * The last good sample is kept and reused.
         *
         * VPADRead produces one sixty times a second and returns
         * VPAD_READ_NO_SAMPLES in between. Handing the keyboard a null
         * on those frames means it sees input only on the frames where
         * a sample happens to land -- so a finger held on a key is a
         * key pressed intermittently, or not at all.
         */
        if (verr == VPAD_READ_SUCCESS) {
            /*
             * swkbd expects calibrated GamePad touch coordinates.
             * Physical buttons work without this, which is why the bug
             * looked like "keyboard works except for touch".
             */
            VPADGetTPCalibratedPoint(
                VPAD_CHAN_0,
                &vpad.tpNormal,
                &vpad.tpNormal);

            last = vpad;
            have_sample = true;
        }

        nn::swkbd::ControllerInfo controllerInfo;
        controllerInfo.vpad = have_sample ? &last : nullptr;
        nn::swkbd::Calc(controllerInfo);

        /* The keyboard loads fonts and runs word prediction on other
         * threads and asks to be driven; skipping these is a keyboard
         * that never finishes drawing itself. */
        if (nn::swkbd::IsNeedCalcSubThreadFont()) {
            nn::swkbd::CalcSubThreadFont();
        }
        if (nn::swkbd::IsNeedCalcSubThreadPredict()) {
            nn::swkbd::CalcSubThreadPredict();
        }

        bool selected = false;
        if (nn::swkbd::IsDecideOkButton(&selected)) {
            from_utf16(nn::swkbd::GetInputFormString(), out, out_size);
            result = 1;
            break;
        }
        if (nn::swkbd::IsDecideCancelButton(&selected)) {
            result = 0;
            break;
        }

        /* SDL clears the frame, then its queue is pushed out, then the
         * keyboard draws its own GX2 on top of it. Without the flush
         * SDL's commands would arrive afterwards and cover it. */
        ui_begin();
        ui_flush();
        nn::swkbd::DrawTV();
        nn::swkbd::DrawDRC();
        ui_present();
    }

    WHBLogPrintf("swkbd: leaving, result %d", result);
    nn::swkbd::DisappearInputForm();
    nn::swkbd::Destroy();
    MEMFreeToDefaultHeap(createArg.workMemory);
    FSDelClient(fsClient, FS_ERROR_FLAG_NONE);
    FSShutdown();
    MEMFreeToDefaultHeap(fsClient);

    return result;
}
