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
    GtkWidget  *enable, *port, *scale, *launch, *status, *path_label;
    GPid        pid;
    int         announced_port;
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

static void load_paths(void)
{
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
        for (int e = 0; e < EMU_COUNT; e++)
            if (g_strcmp0(key, emus[e].key) == 0 && *value)
                g_strlcpy(emus[e].binary, value, sizeof(emus[e].binary));
    }
    g_strfreev(lines);
    g_free(text);
    g_free(path);
}

static void save_paths(void)
{
    char *dir = g_build_filename(g_get_user_config_dir(), "bottom_screen", NULL);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    GString *out = g_string_new("# Where each emulator is installed.\n");
    for (int i = 0; i < EMU_COUNT; i++)
        g_string_append_printf(out, "%s=%s\n", emus[i].key, emus[i].binary);

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
    if (cond & (G_IO_HUP | G_IO_ERR))
        return FALSE;

    char *line = NULL;
    gsize len = 0;
    if (g_io_channel_read_line(src, &line, &len, NULL, NULL) != G_IO_STATUS_NORMAL)
        return FALSE;

    const char *found = strstr(line ? line : "", "listening on port ");
    if (found) {
        e->announced_port = atoi(found + strlen("listening on port "));
        char *msg = g_markup_printf_escaped(
            "<b>streaming on port %d</b>", e->announced_port);
        set_status(e, msg);
        g_free(msg);
    } else if (line && strstr(line, "bottom_screen:")) {
        char *trimmed = g_strchomp(g_strdup(line));
        char *msg = g_markup_printf_escaped("<small>%s</small>", trimmed);
        set_status(e, msg);
        g_free(msg);
        g_free(trimmed);
    }
    g_free(line);
    return TRUE;
}

static void on_child_gone(GPid pid, gint status, gpointer user)
{
    Emu *e = user;
    (void)status;
    g_spawn_close_pid(pid);
    e->pid = 0;
    e->announced_port = 0;
    gtk_button_set_label(GTK_BUTTON(e->launch), "Launch");
    set_status(e, "<small>not running</small>");
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
    int factor = gtk_combo_box_get_active(GTK_COMBO_BOX(e->scale)) + 1;
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

static void on_launch(GtkButton *button, gpointer user)
{
    Emu *e = user;
    (void)button;

    if (e->pid) {
        kill(e->pid, SIGTERM);
        return;
    }
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

    gboolean ok = g_spawn_async_with_pipes(
        g_path_get_dirname(e->binary), argv, env,
        G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL,
        &e->pid, NULL, NULL, &err_fd, &error);
    g_strfreev(env);

    if (!ok) {
        char *msg = g_markup_printf_escaped(
            "<small>%s</small>", error ? error->message : "cannot start");
        set_status(e, msg);
        g_free(msg);
        if (error) g_error_free(error);
        return;
    }

    gtk_button_set_label(GTK_BUTTON(e->launch), "Stop");
    set_status(e, "<small>starting\xe2\x80\xa6</small>");

    GIOChannel *ch = g_io_channel_unix_new(err_fd);
    g_io_channel_set_flags(ch, G_IO_FLAG_NONBLOCK, NULL);
    g_io_channel_set_encoding(ch, NULL, NULL);
    g_io_add_watch(ch, G_IO_IN | G_IO_HUP | G_IO_ERR, on_output, e);
    g_io_channel_unref(ch);

    g_child_watch_add(e->pid, on_child_gone, e);
}

/*
 * Points a phone already plugged in over adb at whichever emulator is
 * actually streaming, so nobody types an address on a phone keyboard.
 */
static void on_send_to_phone(GtkButton *button, gpointer user)
{
    Emu *e = user;
    (void)button;

    if (!e->announced_port) {
        set_status(e, "<small>launch it first</small>");
        return;
    }

    char port[16];
    g_snprintf(port, sizeof(port), "%d", e->announced_port);
    char *argv[] = {
        "adb", "shell", "am", "start", "-n",
        "fr.wozt.bottomscreen/.MainActivity",
        "--es", "host", "10.0.2.2", "--ei", "port", port, NULL
    };
    /* 10.0.2.2 is the host as seen from the Android emulator; a real
     * phone on the network needs this machine's address instead. */
    const char *host = g_object_get_data(G_OBJECT(g_host_label), "host");
    if (host) argv[8] = (char *)host;

    GError *error = NULL;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                       NULL, NULL, NULL, &error)) {
        char *msg = g_markup_printf_escaped(
            "<small>%s</small>", error ? error->message : "adb failed");
        set_status(e, msg);
        g_free(msg);
        if (error) g_error_free(error);
    }
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

    GtkWidget *phone = gtk_button_new_with_label("Send to phone");
    g_signal_connect(phone, "clicked", G_CALLBACK(on_send_to_phone), e);
    gtk_box_pack_start(GTK_BOX(brow), phone, FALSE, FALSE, 0);

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
    refresh_path(e);

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
    g_object_set_data_full(G_OBJECT(g_host_label), "host", lan, g_free);
    gtk_box_pack_start(GTK_BOX(outer), g_host_label, FALSE, FALSE, 0);

    for (int i = 0; i < EMU_COUNT; i++)
        gtk_box_pack_start(GTK_BOX(outer), build_emu_panel(&emus[i]),
                           FALSE, FALSE, 0);

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
    g_object_unref(app);
    return rc;
}
