#include "proc.h"

#include <coreinit/foreground.h>
#include <proc_ui/procui.h>
#include <whb/log.h>

static int g_running;
static int g_procui_up;
static int g_release_pending;
static int g_last_status = -1;

static uint32_t on_save(void *context)
{
    (void)context;
    OSSavesDone_ReadyToRelease();
    return 0;
}

static void log_status(ProcUIStatus status)
{
    if ((int)status != g_last_status) {
        WHBLogPrintf("proc: ProcUI status=%d", (int)status);
        g_last_status = (int)status;
    }
}

void proc_init(void)
{
    WHBLogPrintf("proc: normal HOME overlay mode");

    g_running = 1;
    g_procui_up = 1;
    g_release_pending = 0;
    g_last_status = -1;

    ProcUIInitEx(&on_save, NULL);
}

int proc_running(void)
{
    if (!g_running) {
        return 0;
    }

    const ProcUIStatus status = ProcUIProcessMessages(TRUE);
    log_status(status);

    if (status == PROCUI_STATUS_EXITING) {
        WHBLogPrintf("proc: EXITING");
        g_running = 0;
    }
    else if (status == PROCUI_STATUS_RELEASE_FOREGROUND) {
        /*
         * IMPORTANT:
         *
         * Do NOT call ProcUIDrawDoneRelease() here.
         *
         * H264DEC and GX2 are foreground resources. The Wii U requires
         * the application to release them first. Calling DrawDoneRelease
         * before H264DECEnd() is what made H264DECEnd() hang forever.
         */
        WHBLogPrintf(
            "proc: RELEASE_FOREGROUND; application must release H264/GX2 first");

        g_release_pending = 1;
    }

    return g_running;
}

int proc_release_pending(void)
{
    return g_release_pending;
}

int proc_release_and_wait(void)
{
    if (!g_release_pending) {
        return g_running;
    }

    g_release_pending = 0;

    /*
     * main.c has now destroyed H264DEC and SDL/GX2.
     * Only NOW may the system take the foreground.
     */
    WHBLogPrintf(
        "proc: foreground resources released; ProcUIDrawDoneRelease");

    ProcUIDrawDoneRelease();

    /*
     * Stay out of the application loop while we have no foreground.
     *
     * If HOME is closed without quitting, ProcUI gives us foreground
     * again and main.c rebuilds SDL/GX2 + H264DEC.
     *
     * If Quitter is chosen, we receive EXITING instead.
     */
    while (g_running) {
        const ProcUIStatus status = ProcUIProcessMessages(TRUE);
        log_status(status);

        if (status == PROCUI_STATUS_EXITING) {
            WHBLogPrintf("proc: EXITING while suspended");
            g_running = 0;
            return 0;
        }

        if (status == PROCUI_STATUS_IN_FOREGROUND) {
            WHBLogPrintf("proc: foreground reacquired");
            return 1;
        }
    }

    return 0;
}

void proc_stop(void)
{
    /*
     * There is no synthetic SYSLaunchMenu/SYSRelaunchTitle path.
     * HOME is handled by ProcUI.
     */
    WHBLogPrintf("proc: local stop requested");
    g_running = 0;
}

void proc_shutdown(void)
{
    g_running = 0;
    g_release_pending = 0;

    if (g_procui_up) {
        ProcUIShutdown();
        g_procui_up = 0;
    }
}
