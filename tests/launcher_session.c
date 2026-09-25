/* Exercise process ownership and source transitions without a physical pad. */
#define main launcher_main
#include "../launcher/bs_launcher.c"
#undef main
#include <assert.h>
#include <sys/stat.h>

static void spin_until(gboolean (*ready)(void))
{
    gint64 deadline = g_get_monotonic_time() + 5000000;
    while (!ready() && g_get_monotonic_time() < deadline) {
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(10000);
    }
    assert(ready());
}
static gboolean preview_ready(void) { return pad_pid && !pad_stopping && pad_port == 19700; }
static gboolean game_ready(void) { return pad_pid && !pad_stopping && pad_port == 5092; }
static gboolean pad_idle(void) { return !pad_pid; }

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    char *tmp = g_dir_make_tmp("bs-launcher-test-XXXXXX", NULL);
    assert(tmp);
    g_strlcpy(g_project, tmp, sizeof(g_project));
    char *folder = g_build_filename(tmp, "gamepad", NULL);
    assert(!g_mkdir_with_parents(folder, 0700));
    char *pad = g_build_filename(folder, "bs_gamepad", NULL);
    char *server = g_build_filename(tmp, "bottom_screen_server", NULL);
    const char *pad_script = "#!/bin/sh\n"
        "mkdir pad-lock || { echo OVERLAP >> events; exit 9; }\n"
        "echo start-$2 >> events\n"
        "trap 'echo stop >> events; rmdir pad-lock; exit 0' TERM INT\n"
        "while :; do sleep 0.01; done\n";
    const char *server_script = "#!/bin/sh\n"
        "echo 'bottom_screen: listening on port 19700' >&2\n"
        "trap 'exit 0' TERM INT\n"
        "while :; do sleep 0.01; done\n";
    assert(g_file_set_contents(pad, pad_script, -1, NULL));
    assert(g_file_set_contents(server, server_script, -1, NULL));
    chmod(pad, 0700); chmod(server, 0700);
    pad_status = gtk_label_new("");
    Emu *e = &emus[2];
    build_emu_panel(e);
    on_pattern(NULL, e);
    spin_until(preview_ready);
    GPid first = pad_pid;
    // Deliver the same readiness line the real emulator writes to stderr.
    int fds[2]; assert(!pipe(fds));
    const char *line = "bottom_screen: listening on port 5092\n";
    assert(write(fds[1], line, strlen(line)) == (ssize_t)strlen(line));
    close(fds[1]);
    GIOChannel *ch = g_io_channel_unix_new(fds[0]);
    on_output(ch, G_IO_IN, e);
    g_io_channel_unref(ch); close(fds[0]);
    spin_until(game_ready);
    assert(pad_pid != first);
    // Emulator exits: the pattern must replace it with a fresh process.
    e->announced_port = 0;
    select_pad_port(pattern_port);
    spin_until(preview_ready);
    // Rapid changes are coalesced while the old pad is still exiting.
    select_pad_port(5091);
    select_pad_port(5092);
    spin_until(game_ready);
    select_pad_port(0);
    spin_until(pad_idle);
    stop_preview();
    char *events_path = g_build_filename(tmp, "events", NULL), *events = NULL;
    assert(g_file_get_contents(events_path, &events, NULL, NULL));
    assert(!strstr(events, "OVERLAP"));
    assert(!strcmp(events, "start-19700\nstop\nstart-5092\nstop\n"
                           "start-19700\nstop\nstart-5092\nstop\n"));
    puts("PASS: preview -> emulator -> preview, rapid switches, no overlapping pad processes");
    unlink(events_path); unlink(pad); unlink(server); rmdir(folder); rmdir(tmp);
    g_free(events); g_free(events_path); g_free(pad); g_free(server); g_free(folder); g_free(tmp);
}
