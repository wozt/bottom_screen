/*
 * A launcher for the three emulators.
 *
 * Each of them can stream its bottom screen, and each of them wants the
 * same three things set before it starts: the stream on, a port, and an
 * internal resolution. Doing that by hand means three different
 * configuration formats, two of which have a trap in them -- so it is
 * done here instead.
 *
 * The launcher deliberately stops there. Loading a game, choosing a
 * renderer, mapping a pad: the emulator already does all of that, and
 * reimplementing it would only produce a worse version that drifts.
 */
#include <gtk/gtk.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "bs_protocol.h"

/* Where the internal resolution lives, which is different every time. */
typedef enum {
    SCALE_MELONDS,  /* [3D.GL] ScaleFactor in melonDS.toml */
    SCALE_AZAHAR,   /* [Renderer] resolution_factor in qt-config.ini */
    SCALE_CEMU      /* <pad_size> in settings.xml */
} ScaleKind;

typedef struct {
    const char *name;
    const char *key;             /* what it is called in the settings file */
    const char *relative_binary; /* where it sits in a checkout, as a fallback */
    const char *console;
    int         native_w, native_h;
    int         default_port;
    ScaleKind   scale_kind;
    const char *scale_note;

    char        binary[1024];
    GtkWidget  *enable, *port, *scale, *launch, *quit, *restart;
    GtkWidget  *status, *path_label;
    GPid        pid;
    int         announced_port;
    /*
     * Asked to come back after it has gone. An emulator is stopped by
     * asking it to, which it may take a moment to do; relaunching has
     * to wait for the port to be free, so it happens when the child is
     * reaped rather than here.
     */
    gboolean    restart_wanted;
    guint       kill_timer;     /* the grace period before SIGKILL */
    /*
     * Read from the file before the widgets exist, applied to them
     * afterwards. -1 means the file said nothing, so the widget keeps
     * the default it was built with.
     */
    int         saved_port, saved_scale, saved_enable;
} Emu;

static Emu emus[] = {
    {
        .name = "melonDS",
        .key = "melonds",
        .relative_binary = "emulators/melonDS/build/melonDS",
        .console = "Nintendo DS",
        .native_w = 256, .native_h = 192,
        .default_port = BS_DEFAULT_PORT,
        .scale_kind = SCALE_MELONDS,
        /*
         * Anything above 1x means the OpenGL renderer, which is where
         * melonDS scales -- so the launcher selects it. That renderer
         * keeps the screens in a GPU texture rather than RAM, which the
         * bridge now reads back.
         */
        .scale_note = "Switches melonDS to its OpenGL renderer, which is "
                      "where the scale lives.",
    },
    {
        .name = "Azahar",
        .key = "azahar",
        .relative_binary = "emulators/azahar/build/bin/Release/azahar",
        .console = "Nintendo 3DS",
        .native_w = 320, .native_h = 240,
        .default_port = BS_DEFAULT_PORT + 1,
        .scale_kind = SCALE_AZAHAR,
        .scale_note = "The bottom screen is streamed at this size; the "
                      "client follows without reconnecting.",
    },
    {
        .name = "Cemu",
        .key = "cemu",
        .relative_binary = "emulators/Cemu/bin/Cemu_release",
        .console = "Wii U",
        .native_w = 854, .native_h = 480,
        .default_port = BS_DEFAULT_PORT + 2,
        .scale_kind = SCALE_CEMU,
        .scale_note = "The GamePad view is rendered at its window size, "
                      "so this sets that window. OpenGL and Vulkan both "
                      "stream.",
    },
};

static const int EMU_COUNT = (int)(sizeof(emus) / sizeof(emus[0]));

static char g_project[1024];
static GtkWidget *g_host_label;

/*
 * Where each emulator lives.
 *
 * A checkout with the emulators beside it is the convenient case and
 * stays the fallback, but it must not be the only one: this launcher
 * runs somebody else's build of somebody else's emulator, installed
 * wherever their distribution put it. Nothing here distributes an
 * emulator, and nothing here should assume it did.
 */
static char *paths_file(void)
{
    return g_build_filename(g_get_user_config_dir(),
                            "bottom_screen", "emulators.conf", NULL);
}

/*
 * The GamePad panel's saved values, in the order they are written.
 * Same rule as an emulator's: -1 means the file said nothing.
 */
static int saved_pad[6] = { -1, -1, -1, -1, -1, -1 };
enum { PAD_SCREEN, PAD_RES_BOTTOM, PAD_RES_TOP, PAD_FILTER, PAD_SHARP, PAD_RATE };
static const char *const pad_keys[6] = {
    "pad.screen", "pad.resolution", "pad.top_resolution",
    "pad.filter", "pad.sharpness", "pad.bitrate"
};

static void load_paths(void)
{
    /*
     * Zero is a meaningful resolution index, so "nothing was saved" has
     * to be a different value. Set before reading rather than in the
     * table, where a designated initialiser would leave it at zero and
     * force every emulator to 1x on the first run.
     */
    for (int i = 0; i < EMU_COUNT; i++)
        emus[i].saved_port = emus[i].saved_scale = emus[i].saved_enable = -1;

    char *path = paths_file();
    char *text = NULL;
    if (!g_file_get_contents(path, &text, NULL, NULL)) {
        g_free(path);
        return;
    }

    char **lines = g_strsplit(text, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char *eq = strchr(lines[i], '=');
        if (!eq)
            continue;
        *eq = '\0';
        const char *key = g_strstrip(lines[i]);
        const char *value = g_strstrip(eq + 1);
        for (int e = 0; e < EMU_COUNT; e++) {
            if (g_strcmp0(key, emus[e].key) == 0 && *value) {
                g_strlcpy(emus[e].binary, value, sizeof(emus[e].binary));
                continue;
            }
            char port_key[64], scale_key[64], on_key[64];
            g_snprintf(port_key, sizeof(port_key), "%s.port", emus[e].key);
            g_snprintf(scale_key, sizeof(scale_key), "%s.resolution", emus[e].key);
            g_snprintf(on_key, sizeof(on_key), "%s.stream", emus[e].key);
            if (g_strcmp0(key, port_key) == 0)  emus[e].saved_port = atoi(value);
            if (g_strcmp0(key, scale_key) == 0) emus[e].saved_scale = atoi(value);
            if (g_strcmp0(key, on_key) == 0)    emus[e].saved_enable = atoi(value);
        }
        for (int p = 0; p < 6; p++)
            if (g_strcmp0(key, pad_keys[p]) == 0)
                saved_pad[p] = atoi(value);
    }
    g_strfreev(lines);
    g_free(text);
    g_free(path);
}

/* Declared here because the settings writer above reads them. */
static GtkWidget *pad_screen, *pad_res_bottom, *pad_res_top;
static GtkWidget *pad_filter, *pad_sharpness, *pad_bitrate;
static int combo_or(GtkWidget *w, int fallback);

static void save_paths(void)
{
    char *dir = g_build_filename(g_get_user_config_dir(), "bottom_screen", NULL);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    GString *out = g_string_new(
        "# The launcher's settings. Written by its Save button, and by\n"
        "# choosing a path. Values the file does not mention keep the\n"
        "# defaults, so deleting a line is a way to forget it.\n");
    for (int i = 0; i < EMU_COUNT; i++) {
        Emu *e = &emus[i];
        g_string_append_printf(out, "%s=%s\n", e->key, e->binary);
        if (e->port && GTK_IS_SPIN_BUTTON(e->port))
            g_string_append_printf(out, "%s.port=%d\n", e->key,
                (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(e->port)));
        if (e->scale && GTK_IS_COMBO_BOX(e->scale))
            g_string_append_printf(out, "%s.resolution=%d\n", e->key,
                gtk_combo_box_get_active(GTK_COMBO_BOX(e->scale)));
        if (e->enable && GTK_IS_TOGGLE_BUTTON(e->enable))
            g_string_append_printf(out, "%s.stream=%d\n", e->key,
                gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(e->enable)) ? 1 : 0);
    }
    if (pad_screen) {
        const int now[6] = {
            combo_or(pad_screen, 0), combo_or(pad_res_bottom, 2),
            combo_or(pad_res_top, 2), combo_or(pad_filter, 2),
            combo_or(pad_sharpness, 0),
            (pad_bitrate && GTK_IS_SPIN_BUTTON(pad_bitrate))
                ? (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(pad_bitrate)) : 12
        };
        for (int p = 0; p < 6; p++)
            g_string_append_printf(out, "%s=%d\n", pad_keys[p], now[p]);
    }

    char *path = paths_file();
    g_file_set_contents(path, out->str, -1, NULL);
    g_free(path);
    g_string_free(out, TRUE);
}

/* ------------------------------------------------------------ helpers */

static char *read_file(const char *path, gsize *len)
{
    char *text = NULL;
    if (!g_file_get_contents(path, &text, len, NULL))
        return NULL;
    return text;
}

/*
 * Writes a configuration file back, keeping a copy of what was there.
 *
 * These files belong to the emulator, not to us: they hold game paths,
 * controller bindings and accounts that took someone an evening to set
 * up. A launcher that mangles one should leave a way back.
 */
static gboolean write_file_with_backup(const char *path, const char *text)
{
    char *backup = g_strconcat(path, ".bs-backup", NULL);
    char *old = NULL;
    gsize old_len = 0;
    if (g_file_get_contents(path, &old, &old_len, NULL)) {
        g_file_set_contents(backup, old, (gssize)old_len, NULL);
        g_free(old);
    }
    g_free(backup);
    return g_file_set_contents(path, text, -1, NULL);
}

static char *home_config(const char *rest)
{
    const char *cfg = g_get_user_config_dir();
    return g_build_filename(cfg, rest, NULL);
}

/* ------------------------------------------------------- azahar config */

/*
 * Azahar keeps a "\default" marker beside every value and takes the
 * default whenever it is true. Writing the number alone looks like it
 * worked and changes nothing -- which cost me a while the first time --
 * so both lines are always written together.
 */
static gboolean azahar_set_scale(int factor, char **why)
{
    char *path = home_config("azahar-emu/qt-config.ini");
    gsize len = 0;
    char *text = read_file(path, &len);
    if (!text) {
        *why = g_strdup_printf("cannot read %s", path);
        g_free(path);
        return FALSE;
    }

    char **lines = g_strsplit(text, "\n", -1);
    GString *out = g_string_new(NULL);
    gboolean in_renderer = FALSE, wrote_value = FALSE, wrote_default = FALSE;

    for (int i = 0; lines[i]; i++) {
        const char *l = lines[i];

        if (l[0] == '[') {
            /* Leaving the section without having found the keys: add
             * them before the next one starts. */
            if (in_renderer && (!wrote_value || !wrote_default)) {
                if (!wrote_value)
                    g_string_append_printf(out, "resolution_factor=%d\n", factor);
                if (!wrote_default)
                    g_string_append(out, "resolution_factor\\default=false\n");
            }
            in_renderer = (g_strcmp0(l, "[Renderer]") == 0);
        }

        if (in_renderer && g_str_has_prefix(l, "resolution_factor=")) {
            g_string_append_printf(out, "resolution_factor=%d\n", factor);
            wrote_value = TRUE;
            continue;
        }
        if (in_renderer && g_str_has_prefix(l, "resolution_factor\\default=")) {
            g_string_append(out, "resolution_factor\\default=false\n");
            wrote_default = TRUE;
            continue;
        }
        g_string_append(out, l);
        if (lines[i + 1])
            g_string_append_c(out, '\n');
    }
    if (in_renderer && !wrote_value)
        g_string_append_printf(out, "\nresolution_factor=%d\n", factor);
    if (in_renderer && !wrote_default)
        g_string_append(out, "resolution_factor\\default=false\n");

    gboolean ok = write_file_with_backup(path, out->str);
    if (!ok)
        *why = g_strdup_printf("cannot write %s", path);

    g_string_free(out, TRUE);
    g_strfreev(lines);
    g_free(text);
    g_free(path);
    return ok;
}

/* ------------------------------------------------------ melonds config */

/*
 * melonDS scales in its OpenGL renderer and nowhere else, so setting a
 * factor means selecting that renderer too. Both are written here rather
 * than left for someone to discover: a scale that silently does nothing
 * because the software renderer is selected is worse than no control.
 */
static gboolean melonds_set_scale(int factor, char **why)
{
    char *path = home_config("melonDS/melonDS.toml");
    gsize len = 0;
    char *text = read_file(path, &len);
    if (!text) {
        *why = g_strdup_printf("cannot read %s", path);
        g_free(path);
        return FALSE;
    }

    char **lines = g_strsplit(text, "\n", -1);
    GString *out = g_string_new(NULL);
    char section[64] = "";

    for (int i = 0; lines[i]; i++) {
        const char *l = lines[i];

        if (l[0] == '[')
            g_strlcpy(section, l, sizeof(section));

        if (!g_strcmp0(section, "[3D]") && g_str_has_prefix(l, "Renderer =")) {
            g_string_append(out, "Renderer = 1");
        } else if (!g_strcmp0(section, "[3D.GL]") &&
                   g_str_has_prefix(l, "ScaleFactor =")) {
            g_string_append_printf(out, "ScaleFactor = %d", factor);
        } else if (!g_strcmp0(section, "[Screen]") &&
                   g_str_has_prefix(l, "UseGL =")) {
            g_string_append(out, "UseGL = true");
        } else {
            g_string_append(out, l);
        }
        if (lines[i + 1])
            g_string_append_c(out, '\n');
    }

    gboolean ok = write_file_with_backup(path, out->str);
    if (!ok)
        *why = g_strdup_printf("cannot write %s", path);

    g_string_free(out, TRUE);
    g_strfreev(lines);
    g_free(text);
    g_free(path);
    return ok;
}

/* --------------------------------------------------------- cemu config */

/*
 * Sets the GamePad window size, and the setting without which nothing is
 * captured at all: the pad view has to be open.
 *
 * The graphics API is deliberately left alone. It used to be forced to
 * OpenGL because the Vulkan path had no readback; it has one now, and a
 * launcher that quietly changed somebody's renderer would be taking a
 * decision that is no longer its business.
 *
 * Cemu rewrites this file when it exits, so this is only meaningful
 * before a launch -- which is exactly when a launcher runs.
 */
static gboolean cemu_set_pad(int w, int h, char **why)
{
    char *path = home_config("Cemu/settings.xml");
    gsize len = 0;
    char *text = read_file(path, &len);
    if (!text) {
        *why = g_strdup_printf("cannot read %s", path);
        g_free(path);
        return FALSE;
    }

    char **lines = g_strsplit(text, "\n", -1);
    GString *out = g_string_new(NULL);
    gboolean in_pad_size = FALSE;

    for (int i = 0; lines[i]; i++) {
        const char *l = lines[i];
        const char *t = l;
        while (*t == ' ' || *t == '\t') t++;

        if (g_str_has_prefix(t, "<pad_size>"))  in_pad_size = TRUE;

        if (g_str_has_prefix(t, "<open_pad>")) {
            g_string_append(out, "    <open_pad>true</open_pad>");
            if (lines[i + 1]) g_string_append_c(out, '\n');
            continue;
        }
        if (in_pad_size && g_str_has_prefix(t, "<x>")) {
            g_string_append_printf(out, "        <x>%d</x>", w);
            if (lines[i + 1]) g_string_append_c(out, '\n');
            continue;
        }
        if (in_pad_size && g_str_has_prefix(t, "<y>")) {
            g_string_append_printf(out, "        <y>%d</y>", h);
            if (lines[i + 1]) g_string_append_c(out, '\n');
            continue;
        }
        if (g_str_has_prefix(t, "</pad_size>")) in_pad_size = FALSE;

        g_string_append(out, l);
        if (lines[i + 1])
            g_string_append_c(out, '\n');
    }

    gboolean ok = write_file_with_backup(path, out->str);
    if (!ok)
        *why = g_strdup_printf("cannot write %s", path);

    g_string_free(out, TRUE);
    g_strfreev(lines);
    g_free(text);
    g_free(path);
    return ok;
}

/* -------------------------------------------------------------- launch */

static void set_status(Emu *e, const char *markup)
{
    gtk_label_set_markup(GTK_LABEL(e->status), markup);
}

/* A single physical pad: never overlap two libdrc processes. The pattern
 * has its own port and remains available while the emulator starts. */
static Emu *pad_owner;
static GPid pad_pid, pattern_pid;
static int pad_port, wanted_port, pattern_port;
static gboolean pad_stopping, closing;
static GtkWidget *pad_status;
/*
 * The GamePad's own settings, which belong here rather than on each
 * emulator's panel: there is one physical pad, and these follow it
 * whichever emulator it is showing.
 *
 * They are starting values. The menu on the pad itself owns every later
 * change and is the only thing that writes them to disk -- so a choice
 * made here for one session does not quietly become the saved default.
 */

static void pad_message(const char *text)
{
    if (!closing && pad_status) gtk_label_set_text(GTK_LABEL(pad_status), text);
}

static void start_pad(void);

static void pad_gone(GPid pid, gint status, gpointer unused)
{
    (void)unused;
    g_spawn_close_pid(pid);
    pad_pid = 0;
    if (closing) return;
    if (pad_stopping) {
        pad_stopping = FALSE;
        start_pad();
    } else {
        wanted_port = 0;
        pad_message(status ? "GamePad arrêté : vérifier l’AP et les logs du terminal."
                           : "GamePad déconnecté. Relancer avec Sync GamePad.");
    }
}

static gboolean pad_output(GIOChannel *channel, GIOCondition cond, gpointer unused)
{
    (void)unused;
    char *line = NULL;
    while (g_io_channel_read_line(channel, &line, NULL, NULL, NULL) == G_IO_STATUS_NORMAL) {
        fputs(line, stderr);
        if (strstr(line, "streaming"))
            pad_message(pad_port == pattern_port ? "Mire interactive sur le GamePad"
                                                : "Émulateur → GamePad");
        g_free(line); line = NULL;
    }
    g_free(line);
    return !(cond & (G_IO_HUP | G_IO_ERR));
}

static void watch_pipe(int fd, GIOFunc callback, gpointer data)
{
    GIOChannel *ch = g_io_channel_unix_new(fd);
    g_io_channel_set_flags(ch, G_IO_FLAG_NONBLOCK, NULL);
    g_io_channel_set_encoding(ch, NULL, NULL);
    g_io_channel_set_close_on_unref(ch, TRUE);
    g_io_add_watch(ch, G_IO_IN | G_IO_HUP | G_IO_ERR, callback, data);
    g_io_channel_unref(ch);
}

/*
 * A combo's value, or a default when there is no window.
 *
 * The session test drives the pad without building the interface, and
 * reading a widget that is not there is a warning at best and a wrong
 * value at worst. The defaults here are the ones the combos are created
 * with, so both paths agree.
 */
static int combo_or(GtkWidget *w, int fallback)
{
    if (!w || !GTK_IS_COMBO_BOX(w))
        return fallback;
    const int n = gtk_combo_box_get_active(GTK_COMBO_BOX(w));
    return n < 0 ? fallback : n;
}

static void start_pad(void)
{
    if (!wanted_port || closing) return;
    char *binary = g_build_filename(g_project, "gamepad/bs_gamepad", NULL);
    char port[16], res_b[8], res_t[8], filter[8], sharp[8], rate[16];
    g_snprintf(port, sizeof(port), "%d", wanted_port);
    g_snprintf(res_b, sizeof(res_b), "%d", combo_or(pad_res_bottom, 2));
    g_snprintf(res_t, sizeof(res_t), "%d", combo_or(pad_res_top, 2));
    g_snprintf(filter, sizeof(filter), "%d", combo_or(pad_filter, 2));
    g_snprintf(sharp, sizeof(sharp), "%d", combo_or(pad_sharpness, 0));
    const int mbit = (pad_bitrate && GTK_IS_SPIN_BUTTON(pad_bitrate))
        ? (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(pad_bitrate)) : 12;
    g_snprintf(rate, sizeof(rate), "%d", mbit * 1000000);
    const char *screen = combo_or(pad_screen, 0) ? "top" : "bottom";
    char *argv[] = {binary, "--port", port,
                    "--screen", (char *)screen,
                    "--resolution", res_b, "--top-resolution", res_t,
                    "--filter", filter, "--sharpness", sharp,
                    "--bitrate", rate, NULL};
    GError *error = NULL;
    int fd;
    if (g_spawn_async_with_pipes(g_project, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD,
                                 NULL, NULL, &pad_pid, NULL, NULL, &fd, &error)) {
        pad_port = wanted_port;
        pad_message("GamePad : déauthentification puis reconnexion…");
        g_child_watch_add(pad_pid, pad_gone, NULL);
        watch_pipe(fd, pad_output, NULL);
    } else {
        pad_message(error->message);
        g_clear_error(&error);
    }
    g_free(binary);
}

static void select_pad_port(int port)
{
    wanted_port = port;
    if (pad_pid) {
        if (!pad_stopping && port != pad_port) {
            pad_stopping = TRUE;
            kill(pad_pid, SIGTERM);
        }
    } else start_pad();
}

/*
 * Let go of the pad without stopping anything else.
 *
 * Switching from one source to another goes through select_pad_port,
 * which replaces the client. This is the other case: putting the pad
 * down, so that something outside this launcher -- a bridge started by
 * hand, another machine -- can pick it up. There is one physical pad
 * and only one process may hold libdrc's ports.
 */
/*
 * Explicit, because the alternative is worse.
 *
 * Saving on every change would write a file each time a combo moves,
 * including the ones moved to try something -- and there is no way back
 * from that except editing the file by hand. A button says what was
 * kept and when.
 */
static void on_save(GtkButton *button, gpointer unused)
{
    (void)button; (void)unused;
    save_paths();
    char *where = paths_file();
    char *m = g_markup_printf_escaped("<small>Réglages enregistrés dans %s</small>",
                                      where);
    gtk_label_set_markup(GTK_LABEL(pad_status), m);
    g_free(m);
    g_free(where);
}

static void pad_release(GtkButton *button, gpointer unused)
{
    (void)button; (void)unused;
    pad_owner = NULL;
    wanted_port = 0;
    if (pad_pid) {
        pad_stopping = FALSE;
        kill(pad_pid, SIGTERM);
        pad_message("GamePad libéré.");
    } else {
        pad_message("GamePad déjà libre.");
    }
}

static void pattern_gone(GPid pid, gint status, gpointer unused)
{
    (void)status; (void)unused;
    g_spawn_close_pid(pid);
    if (pid == pattern_pid) {
        pattern_pid = 0;
        if (!closing) pad_message("La mire s’est arrêtée. Relancer avec Sync GamePad.");
    }
}

static gboolean pattern_output(GIOChannel *ch, GIOCondition cond, gpointer user)
{
    // Ignore output from a replaced pattern, even if its last lines were queued.
    GPid pid = GPOINTER_TO_INT(user);
    char *line = NULL;
    while (g_io_channel_read_line(ch, &line, NULL, NULL, NULL) == G_IO_STATUS_NORMAL) {
        fputs(line, stderr);
        const char *found = strstr(line, "listening on port ");
        if (found && pid == pattern_pid && pad_owner && !closing) {
            pattern_port = atoi(found + strlen("listening on port "));
            select_pad_port(pad_owner->announced_port ? pad_owner->announced_port : pattern_port);
        }
        g_free(line); line = NULL;
    }
    g_free(line);
    return !(cond & (G_IO_HUP | G_IO_ERR));
}

static void on_pattern(GtkButton *button, gpointer user)
{
    (void)button;
    Emu *e = user;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(e->enable), TRUE);
    pad_owner = e;
    select_pad_port(0);
    if (pattern_pid) kill(pattern_pid, SIGTERM);
    pattern_pid = 0; pattern_port = 0;
    char *binary = g_build_filename(g_project, "bottom_screen_server", NULL);
    char *console = e->scale_kind == SCALE_MELONDS ? "ds" :
                    e->scale_kind == SCALE_AZAHAR ? "3ds" : "wiiu";
    char *argv[] = {binary, "--console", console, "--port", "19700", NULL};
    GError *error = NULL;
    int fd;
    if (g_spawn_async_with_pipes(g_project, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD,
                                 NULL, NULL, &pattern_pid, NULL, NULL, &fd, &error)) {
        g_child_watch_add(pattern_pid, pattern_gone, NULL);
        watch_pipe(fd, pattern_output, GINT_TO_POINTER(pattern_pid));
        pad_message("Démarrage de la mire…");
    } else {
        pad_message(error->message);
        g_clear_error(&error);
    }
    g_free(binary);
}

static void stop_preview(void)
{
    closing = TRUE;
    if (pad_pid) { kill(pad_pid, SIGTERM); waitpid(pad_pid, NULL, 0); }
    if (pattern_pid) { kill(pattern_pid, SIGTERM); waitpid(pattern_pid, NULL, 0); }
}

/*
 * Watches the emulator's own output for the line it prints when the
 * server is up.
 *
 * The port asked for is not necessarily the port bound: a server whose
 * port is taken walks upwards, which is what lets three emulators run at
 * once. Showing the requested one would be showing a number that does
 * not work.
 */
static gboolean on_output(GIOChannel *src, GIOCondition cond, gpointer user)
{
    Emu *e = user;
    char *line = NULL;
    GIOStatus result;
    while ((result = g_io_channel_read_line(src, &line, NULL, NULL, NULL)) == G_IO_STATUS_NORMAL) {
        fputs(line, stderr);
        const char *found = strstr(line, "listening on port ");
        if (found) {
            e->announced_port = atoi(found + strlen("listening on port "));
            if (pad_owner == e) select_pad_port(e->announced_port);
            char *msg = g_markup_printf_escaped("<b>streaming on port %d</b>", e->announced_port);
            set_status(e, msg);
            g_free(msg);
        } else if (strstr(line, "bottom_screen:")) {
            char *msg = g_markup_printf_escaped("<small>%s</small>", g_strchomp(line));
            set_status(e, msg);
            g_free(msg);
        }
        g_free(line); line = NULL;
    }
    g_free(line);
    return result == G_IO_STATUS_AGAIN && !(cond & (G_IO_HUP | G_IO_ERR));
}

static void on_launch(GtkButton *button, gpointer user);
static void emu_buttons(Emu *e);

static void on_child_gone(GPid pid, gint status, gpointer user)
{
    Emu *e = user;
    (void)status;
    g_spawn_close_pid(pid);
    e->pid = 0;
    e->announced_port = 0;
    if (e->kill_timer) { g_source_remove(e->kill_timer); e->kill_timer = 0; }
    if (pad_owner == e) select_pad_port(pattern_port);
    gtk_button_set_label(GTK_BUTTON(e->launch), "Launch");
    emu_buttons(e);
    set_status(e, "<small>not running</small>");
    if (e->restart_wanted && !closing) {
        e->restart_wanted = FALSE;
        on_launch(NULL, e);
    }
}

static gboolean set_scale_for(Emu *e, int factor, char **why)
{
    switch (e->scale_kind) {
    case SCALE_MELONDS: return melonds_set_scale(factor, why);
    case SCALE_AZAHAR:  return azahar_set_scale(factor, why);
    default:            return cemu_set_pad(e->native_w * factor,
                                            e->native_h * factor, why);
    }
}

static void apply_scale(Emu *e)
{
    int factor = combo_or(e->scale, 0) + 1;
    char *why = NULL;
    gboolean ok = set_scale_for(e, factor, &why);

    if (!ok && why) {
        char *msg = g_markup_printf_escaped("<small>%s</small>", why);
        set_status(e, msg);
        g_free(msg);
        g_free(why);
    }
}

static void refresh_path(Emu *e)
{
    const gboolean ok = g_file_test(e->binary, G_FILE_TEST_IS_EXECUTABLE);
    char *m = g_markup_printf_escaped("<small>%s%s</small>",
                                      ok ? "" : "not found: ", e->binary);
    gtk_label_set_markup(GTK_LABEL(e->path_label), m);
    g_free(m);
    set_status(e, ok ? "<small>not running</small>"
                     : "<small>set the path to the program</small>");
}

static void on_browse(GtkButton *button, gpointer user)
{
    Emu *e = user;
    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Where is it installed?",
        GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(button))),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "Cancel", GTK_RESPONSE_CANCEL, "Select", GTK_RESPONSE_ACCEPT, NULL);

    if (e->binary[0])
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog), e->binary);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *chosen = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (chosen) {
            g_strlcpy(e->binary, chosen, sizeof(e->binary));
            g_free(chosen);
            save_paths();
            refresh_path(e);
        }
    }
    gtk_widget_destroy(dialog);
}

static gboolean emu_insist(gpointer user)
{
    Emu *e = user;
    e->kill_timer = 0;
    if (e->pid) {
        /*
         * Five seconds is enough for an emulator to save what it holds.
         * Past that it is hung, and a hung one keeps the port -- so the
         * next launch fails for a reason that looks like something else
         * entirely. That has cost an evening here already.
         */
        set_status(e, "<small>did not quit; closing it by force</small>");
        kill(e->pid, SIGKILL);
    }
    return G_SOURCE_REMOVE;
}

static void emu_stop(Emu *e, const char *why)
{
    if (!e->pid)
        return;
    set_status(e, why);
    kill(e->pid, SIGTERM);
    if (!e->kill_timer)
        e->kill_timer = g_timeout_add_seconds(5, emu_insist, e);
}

static void on_quit(GtkButton *button, gpointer user)
{
    (void)button;
    Emu *e = user;
    e->restart_wanted = FALSE;
    emu_stop(e, "<small>quitting\u2026</small>");
}

static void on_restart(GtkButton *button, gpointer user)
{
    Emu *e = user;
    if (!e->pid) { on_launch(button, e); return; }
    /*
     * Stopped now, started when it is actually gone: a new one started
     * here would find the port still held by the old one, and the
     * failure reads as a configuration fault rather than a race.
     */
    e->restart_wanted = TRUE;
    emu_stop(e, "<small>restarting\u2026</small>");
}

/* The three buttons say which of them is worth pressing. */
static void emu_buttons(Emu *e)
{
    const gboolean running = e->pid != 0;
    gtk_widget_set_sensitive(e->launch, !running);
    gtk_widget_set_sensitive(e->quit, running);
    gtk_widget_set_sensitive(e->restart, TRUE);
}

static void on_launch(GtkButton *button, gpointer user)
{
    Emu *e = user;
    (void)button;

    if (e->pid)
        return;     /* Quit and Restart are their own buttons now */
    if (!g_file_test(e->binary, G_FILE_TEST_IS_EXECUTABLE)) {
        set_status(e, "<small>set the path to the program</small>");
        return;
    }

    apply_scale(e);

    /*
     * The stream is configured through the environment rather than the
     * emulator's own settings, so a launch never rewrites a preference
     * somebody set by hand -- and Cemu would throw such an edit away on
     * exit in any case. All three read the same two names.
     */
    gboolean on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(e->enable));
    int port = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(e->port));

    char **env = g_get_environ();
    env = g_environ_setenv(env, "BOTTOM_SCREEN", on ? "1" : "0", TRUE);
    char portbuf[16];
    g_snprintf(portbuf, sizeof(portbuf), "%d", port);
    env = g_environ_setenv(env, "BOTTOM_SCREEN_PORT", portbuf, TRUE);

    char *argv[] = { e->binary, NULL };
    gint err_fd = -1;
    GError *error = NULL;

    char *cwd = g_path_get_dirname(e->binary);
    gboolean ok = g_spawn_async_with_pipes(
        cwd, argv, env,
        G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL,
        &e->pid, NULL, NULL, &err_fd, &error);
    g_free(cwd);
    g_strfreev(env);

    if (!ok) {
        char *msg = g_markup_printf_escaped(
            "<small>%s</small>", error ? error->message : "cannot start");
        set_status(e, msg);
        g_free(msg);
        if (error) g_error_free(error);
        return;
    }

    gtk_button_set_label(GTK_BUTTON(e->launch), "Launch");
    emu_buttons(e);
    set_status(e, "<small>starting\xe2\x80\xa6</small>");

    watch_pipe(err_fd, on_output, e);

    g_child_watch_add(e->pid, on_child_gone, e);
}

/* ------------------------------------------------------------------ ui */

static GtkWidget *build_emu_panel(Emu *e)
{
    GtkWidget *frame = gtk_frame_new(NULL);
    GtkWidget *title = gtk_label_new(NULL);
    char *tm = g_markup_printf_escaped("<b>%s</b>  <small>%s</small>",
                                       e->name, e->console);
    gtk_label_set_markup(GTK_LABEL(title), tm);
    g_free(tm);
    gtk_frame_set_label_widget(GTK_FRAME(frame), title);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    gtk_container_add(GTK_CONTAINER(frame), box);

    e->enable = gtk_check_button_new_with_label("Stream the bottom screen");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(e->enable), TRUE);
    gtk_box_pack_start(GTK_BOX(box), e->enable, FALSE, FALSE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new("Port"), FALSE, FALSE, 0);
    e->port = gtk_spin_button_new_with_range(1024, 65535, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(e->port), e->default_port);
    gtk_box_pack_start(GTK_BOX(row), e->port, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row),
        gtk_label_new("(moves up if taken)"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

    GtkWidget *srow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(srow), gtk_label_new("Resolution"), FALSE, FALSE, 0);
    e->scale = gtk_combo_box_text_new();
    for (int n = 1; n <= 6; n++) {
        char item[64];
        g_snprintf(item, sizeof(item), "%d\xc3\x97  (%d\xc3\x97%d)",
                   n, e->native_w * n, e->native_h * n);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(e->scale), item);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(e->scale), 0);
    gtk_box_pack_start(GTK_BOX(srow), e->scale, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), srow, FALSE, FALSE, 0);

    GtkWidget *note = gtk_label_new(NULL);
    char *nm = g_markup_printf_escaped("<small>%s</small>", e->scale_note);
    gtk_label_set_markup(GTK_LABEL(note), nm);
    g_free(nm);
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_label_set_xalign(GTK_LABEL(note), 0.0f);
    gtk_box_pack_start(GTK_BOX(box), note, FALSE, FALSE, 0);

    GtkWidget *brow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    e->launch = gtk_button_new_with_label("Launch");
    g_signal_connect(e->launch, "clicked", G_CALLBACK(on_launch), e);
    gtk_box_pack_start(GTK_BOX(brow), e->launch, FALSE, FALSE, 0);

    /* What it does, not what it shows: it brings the pad up and
     * gives it something to decode. The picture until an emulator
     * starts happens to be the test pattern, which is a detail of
     * the moment rather than the point of the button. */
    GtkWidget *preview = gtk_button_new_with_label("Sync GamePad");
    g_signal_connect(preview, "clicked", G_CALLBACK(on_pattern), e);
    gtk_box_pack_start(GTK_BOX(brow), preview, FALSE, FALSE, 0);

    e->quit = gtk_button_new_with_label("Quit");
    g_signal_connect(e->quit, "clicked", G_CALLBACK(on_quit), e);
    gtk_box_pack_start(GTK_BOX(brow), e->quit, FALSE, FALSE, 0);

    e->restart = gtk_button_new_with_label("Restart");
    g_signal_connect(e->restart, "clicked", G_CALLBACK(on_restart), e);
    gtk_box_pack_start(GTK_BOX(brow), e->restart, FALSE, FALSE, 0);

    GtkWidget *browse = gtk_button_new_with_label("Path\u2026");
    g_signal_connect(browse, "clicked", G_CALLBACK(on_browse), e);
    gtk_box_pack_start(GTK_BOX(brow), browse, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), brow, FALSE, FALSE, 0);

    e->path_label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(e->path_label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(e->path_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(box), e->path_label, FALSE, FALSE, 0);

    e->status = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(e->status), 0.0f);
    gtk_box_pack_start(GTK_BOX(box), e->status, FALSE, FALSE, 0);
    if (e->saved_enable >= 0)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(e->enable),
                                     e->saved_enable != 0);
    if (e->saved_port > 0)
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(e->port), e->saved_port);
    if (e->saved_scale >= 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(e->scale), e->saved_scale);

    refresh_path(e);
    emu_buttons(e);

    return frame;
}

/* This machine's address on the network, which is what a real phone has
 * to reach -- not localhost, and not the emulator's 10.0.2.2. */
static char *guess_lan_address(void)
{
    char *out = NULL;
    if (g_spawn_command_line_sync(
            "sh -c \"ip -4 -o addr show scope global | "
            "awk '{print $4}' | cut -d/ -f1 | head -1\"",
            &out, NULL, NULL, NULL) && out) {
        g_strchomp(out);
        if (*out)
            return out;
    }
    g_free(out);
    return g_strdup("127.0.0.1");
}

static void activate(GtkApplication *app, gpointer user)
{
    (void)user;
    GtkWidget *win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "Bottom Screen");
    gtk_window_set_default_size(GTK_WINDOW(win), 520, 720);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);
    gtk_container_add(GTK_CONTAINER(win), outer);

    char *lan = guess_lan_address();
    g_host_label = gtk_label_new(NULL);
    char *hm = g_markup_printf_escaped(
        "Clients connect to <b>%s</b>", lan);
    gtk_label_set_markup(GTK_LABEL(g_host_label), hm);
    g_free(hm);
    g_free(lan);
    gtk_box_pack_start(GTK_BOX(outer), g_host_label, FALSE, FALSE, 0);

    for (int i = 0; i < EMU_COUNT; i++)
        gtk_box_pack_start(GTK_BOX(outer), build_emu_panel(&emus[i]),
                           FALSE, FALSE, 0);

    GtkWidget *pad_frame = gtk_frame_new("GamePad Wii U");
    GtkWidget *pad_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(pad_box), 8);
    gtk_container_add(GTK_CONTAINER(pad_frame), pad_box);

    GtkWidget *prow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(prow), gtk_label_new("Écran"), FALSE, FALSE, 0);
    pad_screen = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pad_screen), "Bas");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pad_screen), "Haut");
    gtk_combo_box_set_active(GTK_COMBO_BOX(pad_screen), 0);
    gtk_box_pack_start(GTK_BOX(prow), pad_screen, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(prow), gtk_label_new("Débit (Mb/s)"), FALSE, FALSE, 0);
    pad_bitrate = gtk_spin_button_new_with_range(1, 24, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(pad_bitrate), 12);
    gtk_box_pack_start(GTK_BOX(prow), pad_bitrate, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pad_box), prow, FALSE, FALSE, 0);

    /* One multiplier list, used twice: the two screens are different
     * pictures with different encoders behind them. */
    const char *factors[] = {"×1/2", "×3/4", "×1", "×3/2", "×2"};
    GtkWidget *rrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(rrow), gtk_label_new("Résolution — bas"), FALSE, FALSE, 0);
    pad_res_bottom = gtk_combo_box_text_new();
    pad_res_top = gtk_combo_box_text_new();
    for (int n = 0; n < 5; n++) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pad_res_bottom), factors[n]);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pad_res_top), factors[n]);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(pad_res_bottom), 2);
    gtk_combo_box_set_active(GTK_COMBO_BOX(pad_res_top), 2);
    gtk_box_pack_start(GTK_BOX(rrow), pad_res_bottom, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rrow), gtk_label_new("haut"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rrow), pad_res_top, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pad_box), rrow, FALSE, FALSE, 0);

    GtkWidget *irow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(irow), gtk_label_new("Redimensionnement"), FALSE, FALSE, 0);
    pad_filter = gtk_combo_box_text_new();
    const char *filters[] = {"Bilinéaire", "Bicubique", "Lanczos", "Pixels"};
    for (int n = 0; n < 4; n++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pad_filter), filters[n]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(pad_filter), 2);
    gtk_box_pack_start(GTK_BOX(irow), pad_filter, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(irow), gtk_label_new("Netteté"), FALSE, FALSE, 0);
    pad_sharpness = gtk_combo_box_text_new();
    for (int n = 0; n < 5; n++) {
        char item[16];
        g_snprintf(item, sizeof(item), "%d %%", n * 25);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pad_sharpness), item);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(pad_sharpness), 0);
    gtk_box_pack_start(GTK_BOX(irow), pad_sharpness, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pad_box), irow, FALSE, FALSE, 0);

    if (saved_pad[PAD_SCREEN] >= 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(pad_screen), saved_pad[PAD_SCREEN]);
    if (saved_pad[PAD_RES_BOTTOM] >= 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(pad_res_bottom), saved_pad[PAD_RES_BOTTOM]);
    if (saved_pad[PAD_RES_TOP] >= 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(pad_res_top), saved_pad[PAD_RES_TOP]);
    if (saved_pad[PAD_FILTER] >= 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(pad_filter), saved_pad[PAD_FILTER]);
    if (saved_pad[PAD_SHARP] >= 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(pad_sharpness), saved_pad[PAD_SHARP]);
    if (saved_pad[PAD_RATE] > 0)
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(pad_bitrate), saved_pad[PAD_RATE]);

    GtkWidget *hrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *release = gtk_button_new_with_label("Libérer le GamePad");
    g_signal_connect(release, "clicked", G_CALLBACK(pad_release), NULL);
    gtk_box_pack_start(GTK_BOX(hrow), release, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hrow),
        gtk_label_new("— rend la dalle à un autre client"), FALSE, FALSE, 0);
    GtkWidget *save = gtk_button_new_with_label("Enregistrer les réglages");
    g_signal_connect(save, "clicked", G_CALLBACK(on_save), NULL);
    gtk_box_pack_end(GTK_BOX(hrow), save, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pad_box), hrow, FALSE, FALSE, 0);

    GtkWidget *pad_note = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(pad_note),
        "<small>Valeurs de départ : le menu sur le GamePad garde la main "
        "ensuite, écran par écran. Au-dessus de ×1 : utile seulement si "
        "l’émulateur rend plus grand que la dalle.</small>");
    gtk_label_set_line_wrap(GTK_LABEL(pad_note), TRUE);
    gtk_label_set_xalign(GTK_LABEL(pad_note), 0.0f);
    gtk_box_pack_start(GTK_BOX(pad_box), pad_note, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), pad_frame, FALSE, FALSE, 0);

    GtkWidget *help = gtk_label_new("Sync GamePad, avant un émulateur : sticks = couleurs · boutons = notes\n"
                                    "Tactile = son continu (X : hauteur, Y : timbre) · ↑/↓ = octave");
    gtk_label_set_line_wrap(GTK_LABEL(help), TRUE);
    gtk_box_pack_start(GTK_BOX(outer), help, FALSE, FALSE, 0);
    pad_status = gtk_label_new("GamePad prêt à connecter");
    gtk_label_set_line_wrap(GTK_LABEL(pad_status), TRUE);
    gtk_box_pack_start(GTK_BOX(outer), pad_status, FALSE, FALSE, 0);
    gtk_widget_show_all(win);
}

/*
 * Applying a resolution without opening a window.
 *
 *   bs_launcher --set-resolution azahar 4
 *
 * Worth having on its own -- a script can prepare a session -- and it is
 * how the config writers are tested, which matters because they edit
 * files holding game paths and accounts rather than anything of ours.
 */
static int apply_from_command_line(const char *which, const char *factor_text)
{
    int factor = atoi(factor_text);
    if (factor < 1 || factor > 16) {
        fprintf(stderr, "resolution must be between 1 and 16\n");
        return 2;
    }

    for (int i = 0; i < EMU_COUNT; i++) {
        Emu *e = &emus[i];
        if (g_ascii_strcasecmp(which, e->name) != 0)
            continue;
        char *why = NULL;
        gboolean ok = set_scale_for(e, factor, &why);
        if (!ok) {
            fprintf(stderr, "%s: %s\n", e->name, why ? why : "failed");
            g_free(why);
            return 1;
        }
        printf("%s: %dx (%dx%d)\n", e->name, factor,
               e->native_w * factor, e->native_h * factor);
        return 0;
    }
    fprintf(stderr, "unknown emulator: %s\n", which);
    return 2;
}

int main(int argc, char **argv)
{
    /*
     * The emulators are found relative to the project, so the launcher
     * works from a checkout without anything being installed.
     */
    char *exe = g_file_read_link("/proc/self/exe", NULL);
    char *dir = exe ? g_path_get_dirname(exe) : g_strdup(".");
    char *parent = g_path_get_dirname(dir);
    g_strlcpy(g_project, parent, sizeof(g_project));
    g_free(exe); g_free(dir); g_free(parent);

    for (int i = 0; i < EMU_COUNT; i++) {
        char *p = g_build_filename(g_project, emus[i].relative_binary, NULL);
        g_strlcpy(emus[i].binary, p, sizeof(emus[i].binary));
        g_free(p);
    }
    /* A saved path wins over the checkout layout: somebody who pointed
     * this at their own installation meant it. */
    load_paths();

    if (argc == 4 && !strcmp(argv[1], "--set-resolution"))
        return apply_from_command_line(argv[2], argv[3]);

    GtkApplication *app = gtk_application_new("fr.wozt.bottomscreen.launcher",
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int rc = g_application_run(G_APPLICATION(app), argc, argv);
    stop_preview();
    g_object_unref(app);
    return rc;
}
