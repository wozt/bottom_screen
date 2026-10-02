/*
 * A launcher for the three emulators.
 *
 * Each of them can stream its bottom screen, and each of them wants the
 * same things set before it starts: the stream, a port, an internal
 * resolution and, optionally, the console firmware. Doing that by hand
 * means three different configuration formats, two of which have a trap
 * in them -- so it is done here instead.
 *
 * Loading a game, choosing a renderer and mapping a pad remain the
 * emulator's job. The launcher only adds the shared GamePad controls,
 * system-menu shortcuts and live streaming diagnostics.
 */
#include <gtk/gtk.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
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
    GtkWidget  *enable, *port, *scale, *launch, *launch_firmware, *quit, *restart;
    GtkWidget  *status, *path_label;
    GtkWidget  *firmware_field, *stats_label;
    char        firmware[1024];
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
    gboolean    last_launch_firmware;

    int         stats_source_w[2], stats_source_h[2];
    int         stats_output_w[2], stats_output_h[2];
    int         stats_nominal_fps, stats_clients[2], stats_max_clients;
    uint32_t    stats_frames[2];
    double      stats_mbps[2], stats_measured_fps[2];
    char        stats_encoder[64];
    char        stats_event[160];
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
            char port_key[64], scale_key[64], on_key[64], firmware_key[64];
            g_snprintf(port_key, sizeof(port_key), "%s.port", emus[e].key);
            g_snprintf(scale_key, sizeof(scale_key), "%s.resolution", emus[e].key);
            g_snprintf(on_key, sizeof(on_key), "%s.stream", emus[e].key);
            g_snprintf(firmware_key, sizeof(firmware_key), "%s.firmware", emus[e].key);
            if (g_strcmp0(key, port_key) == 0)  emus[e].saved_port = atoi(value);
            if (g_strcmp0(key, scale_key) == 0) emus[e].saved_scale = atoi(value);
            if (g_strcmp0(key, on_key) == 0)    emus[e].saved_enable = atoi(value);
            if (g_strcmp0(key, firmware_key) == 0)
                g_strlcpy(emus[e].firmware, value, sizeof(emus[e].firmware));
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
static const char *selected_firmware(Emu *e);

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
        const char *firmware = selected_firmware(e);
        if (firmware && *firmware) {
            g_strlcpy(e->firmware, firmware, sizeof(e->firmware));
            g_string_append_printf(out, "%s.firmware=%s\n", e->key, e->firmware);
        }
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

/* Small readers for the emulators' own configuration files. They only
 * inspect the keys needed to locate installed firmware and leave every
 * other setting to the emulator. */
static char *ini_value(const char *path, const char *section, const char *key)
{
    char *text = read_file(path, NULL);
    if (!text)
        return NULL;

    char **lines = g_strsplit(text, "\n", -1);
    gboolean in_section = section == NULL;
    char *value = NULL;
    for (int i = 0; lines[i] && !value; i++) {
        char *line = g_strstrip(lines[i]);
        if (*line == '[') {
            in_section = section && g_strcmp0(line, section) == 0;
            continue;
        }
        if (!in_section)
            continue;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        if (g_strcmp0(g_strstrip(line), key) != 0)
            continue;
        char *raw = g_strstrip(eq + 1);
        if (raw[0] == '"') {
            gsize n = strlen(raw);
            if (n > 1 && raw[n - 1] == '"') {
                raw[n - 1] = '\0';
                raw++;
            }
        }
        value = g_strcompress(raw);
    }
    g_strfreev(lines);
    g_free(text);
    return value;
}

static char *xml_value(const char *path, const char *tag)
{
    char *text = read_file(path, NULL);
    if (!text)
        return NULL;
    char *open = g_strdup_printf("<%s", tag);
    char *close = g_strdup_printf("</%s>", tag);
    char *start = strstr(text, open);
    char *value = NULL;
    if (start) {
        start = strchr(start, '>');
        if (start) {
            start++;
            char *end = strstr(start, close);
            if (end)
                value = g_strndup(start, (gsize)(end - start));
        }
    }
    g_free(open);
    g_free(close);
    g_free(text);
    return value;
}

static const char *selected_firmware(Emu *e)
{
    if (!e->firmware_field)
        return e->firmware;
    if (GTK_IS_ENTRY(e->firmware_field))
        return gtk_entry_get_text(GTK_ENTRY(e->firmware_field));
    if (GTK_IS_COMBO_BOX(e->firmware_field)) {
        const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(e->firmware_field));
        return id ? id : "";
    }
    return e->firmware;
}

static void firmware_button_state(Emu *e)
{
    if (!e->launch_firmware)
        return;
    const char *value = selected_firmware(e);
    gtk_widget_set_sensitive(e->launch_firmware,
        !e->pid && value && *value &&
        (e->scale_kind == SCALE_CEMU || g_file_test(value, G_FILE_TEST_IS_REGULAR)));
}

static void on_firmware_changed(GtkWidget *widget, gpointer user)
{
    (void)widget;
    firmware_button_state(user);
}

static void firmware_combo_add(Emu *e, const char *value, const char *label)
{
    GtkTreeIter iter;
    gboolean was_empty = !gtk_tree_model_get_iter_first(
        gtk_combo_box_get_model(GTK_COMBO_BOX(e->firmware_field)), &iter);
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(e->firmware_field), value, label);
    if (was_empty)
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(e->firmware_field), value);
    if (*e->firmware && g_strcmp0(e->firmware, value) == 0)
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(e->firmware_field), value);
}

/* A 3DS TMD names the executable content and its version. Reading those
 * two big-endian fields avoids offering manuals or data .app files as a
 * HOME Menu. The layout and signature sizes are part of Nintendo's TMD
 * format and are the same values used by Azahar's TitleMetadata reader. */
static gboolean azahar_tmd_info(const char *path, guint32 *content_id,
                                guint16 *version)
{
    gsize size = 0;
    guint8 *data = (guint8 *)read_file(path, &size);
    if (!data || size < 4) {
        g_free(data);
        return FALSE;
    }
    guint32 type = ((guint32)data[0] << 24) | ((guint32)data[1] << 16) |
                   ((guint32)data[2] << 8) | data[3];
    gsize signature_size = 0;
    switch (type) {
    case 0x00010000: case 0x00010003: signature_size = 0x200; break;
    case 0x00010001: case 0x00010004: signature_size = 0x100; break;
    case 0x00010002: case 0x00010005: signature_size = 0x3c; break;
    default: break;
    }
    gsize body = (signature_size + 4 + 0x3f) & ~(gsize)0x3f;
    gsize chunk = body + 0x9c4;
    if (!signature_size || size < chunk + 4 || size < body + 0x9e) {
        g_free(data);
        return FALSE;
    }
    *version = ((guint16)data[body + 0x9c] << 8) | data[body + 0x9d];
    *content_id = ((guint32)data[chunk] << 24) |
                  ((guint32)data[chunk + 1] << 16) |
                  ((guint32)data[chunk + 2] << 8) | data[chunk + 3];
    g_free(data);
    return TRUE;
}

static void discover_azahar_firmware(Emu *e)
{
    char *config = home_config("azahar-emu/qt-config.ini");
    char *nand = ini_value(config, "[Data%20Storage]", "nand_directory");
    if (!nand || !*nand) {
        g_free(nand);
        nand = g_build_filename(g_get_user_data_dir(), "azahar-emu", "nand", NULL);
    }

    static const struct { const char *low, *region; } menus[] = {
        {"00008202", "Japon"}, {"00008f02", "Amérique"},
        {"00009802", "Europe / Australie"}, {"0000a102", "Chine"},
        {"0000a902", "Corée"}, {"0000b102", "Taïwan"},
    };
    GDir *systems = g_dir_open(nand, 0, NULL);
    const char *system_id;
    while (systems && (system_id = g_dir_read_name(systems))) {
        if (strlen(system_id) != 32)
            continue;
        for (guint m = 0; m < G_N_ELEMENTS(menus); m++) {
            char *content = g_build_filename(nand, system_id, "title", "00040030",
                                             menus[m].low, "content", NULL);
            GDir *dir = g_dir_open(content, 0, NULL);
            const char *name;
            gboolean found_tmd = FALSE;
            while (dir && (name = g_dir_read_name(dir))) {
                if (!g_str_has_suffix(name, ".tmd"))
                    continue;
                char *tmd = g_build_filename(content, name, NULL);
                guint32 content_id;
                guint16 version;
                if (!azahar_tmd_info(tmd, &content_id, &version)) {
                    g_free(tmd);
                    continue;
                }
                char app_name[16];
                g_snprintf(app_name, sizeof(app_name), "%08x.app", content_id);
                char *file = g_build_filename(content, app_name, NULL);
                if (!g_file_test(file, G_FILE_TEST_IS_REGULAR)) {
                    g_free(file);
                    g_free(tmd);
                    continue;
                }
                char *label = g_strdup_printf("%s — 00040030%s — v%u",
                                              menus[m].region, menus[m].low, version);
                firmware_combo_add(e, file, label);
                found_tmd = TRUE;
                g_free(label);
                g_free(file);
                g_free(tmd);
            }
            if (dir) g_dir_close(dir);

            /* Old/manual NAND layouts without usable metadata still get
             * a practical fallback; most real installs take the TMD path. */
            dir = found_tmd ? NULL : g_dir_open(content, 0, NULL);
            while (dir && (name = g_dir_read_name(dir))) {
                if (!g_str_has_suffix(name, ".app"))
                    continue;
                char *file = g_build_filename(content, name, NULL);
                char *label = g_strdup_printf("%s — 00040030%s — %s",
                                              menus[m].region, menus[m].low, name);
                firmware_combo_add(e, file, label);
                g_free(label);
                g_free(file);
            }
            if (dir) g_dir_close(dir);
            g_free(content);
        }
    }
    if (systems) g_dir_close(systems);
    g_free(nand);
    g_free(config);
}

static void discover_cemu_firmware(Emu *e)
{
    char *config = home_config("Cemu/settings.xml");
    char *mlc = xml_value(config, "mlc_path");
    if (!mlc || !*mlc) {
        g_free(mlc);
        mlc = g_build_filename(g_get_user_data_dir(), "Cemu", "mlc01", NULL);
    }
    static const struct { const char *low, *region; } menus[] = {
        {"10040000", "Japon"}, {"10040100", "Amérique"},
        {"10040200", "Europe"},
    };
    for (guint m = 0; m < G_N_ELEMENTS(menus); m++) {
        char *meta = g_build_filename(mlc, "sys", "title", "00050010",
                                      menus[m].low, "meta", "meta.xml", NULL);
        if (!g_file_test(meta, G_FILE_TEST_IS_REGULAR)) {
            g_free(meta);
            continue;
        }
        char id[17];
        g_snprintf(id, sizeof(id), "00050010%s", menus[m].low);
        char *version = xml_value(meta, "title_version");
        char *label = version && *version
            ? g_strdup_printf("%s — %s — v%s", menus[m].region, id, version)
            : g_strdup_printf("%s — %s", menus[m].region, id);
        firmware_combo_add(e, id, label);
        g_free(label);
        g_free(version);
        g_free(meta);
    }
    g_free(mlc);
    g_free(config);
}

static void populate_firmware_field(Emu *e)
{
    if (e->scale_kind == SCALE_MELONDS) {
        e->firmware_field = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(e->firmware_field), "firmware.bin");
        if (!*e->firmware) {
            char *config = home_config("melonDS/melonDS.toml");
            char *detected = ini_value(config, "[DS]", "FirmwarePath");
            if (detected) {
                g_strlcpy(e->firmware, detected, sizeof(e->firmware));
                g_free(detected);
            }
            g_free(config);
        }
        gtk_entry_set_text(GTK_ENTRY(e->firmware_field), e->firmware);
        g_signal_connect(e->firmware_field, "changed",
                         G_CALLBACK(on_firmware_changed), e);
        return;
    }

    e->firmware_field = gtk_combo_box_text_new();
    if (e->scale_kind == SCALE_AZAHAR)
        discover_azahar_firmware(e);
    else
        discover_cemu_firmware(e);
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(e->firmware_field)) < 0) {
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(e->firmware_field), "",
                                  "Aucun menu détecté");
        gtk_combo_box_set_active(GTK_COMBO_BOX(e->firmware_field), 0);
    }
    g_signal_connect(e->firmware_field, "changed",
                     G_CALLBACK(on_firmware_changed), e);
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

static gboolean melonds_set_firmware(const char *firmware, char **why)
{
    char *path = home_config("melonDS/melonDS.toml");
    char *text = read_file(path, NULL);
    if (!text) {
        *why = g_strdup_printf("cannot read %s", path);
        g_free(path);
        return FALSE;
    }

    char *escaped = g_strescape(firmware, NULL);
    char **lines = g_strsplit(text, "\n", -1);
    GString *out = g_string_new(NULL);
    gboolean in_ds = FALSE, wrote = FALSE;
    for (int i = 0; lines[i]; i++) {
        const char *line = lines[i];
        if (line[0] == '[') {
            if (in_ds && !wrote) {
                g_string_append_printf(out, "FirmwarePath = \"%s\"\n", escaped);
                wrote = TRUE;
            }
            in_ds = g_strcmp0(line, "[DS]") == 0;
        }
        if (in_ds && g_str_has_prefix(line, "FirmwarePath =")) {
            g_string_append_printf(out, "FirmwarePath = \"%s\"", escaped);
            wrote = TRUE;
        } else {
            g_string_append(out, line);
        }
        if (lines[i + 1])
            g_string_append_c(out, '\n');
    }
    if (in_ds && !wrote)
        g_string_append_printf(out, "\nFirmwarePath = \"%s\"\n", escaped);

    gboolean ok = write_file_with_backup(path, out->str);
    if (!ok)
        *why = g_strdup_printf("cannot write %s", path);
    g_string_free(out, TRUE);
    g_strfreev(lines);
    g_free(escaped);
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

static void refresh_stats(Emu *e)
{
    if (!e->stats_label)
        return;
    if (!e->pid) {
        gtk_label_set_markup(GTK_LABEL(e->stats_label),
                             "<span foreground=\"#888888\">Arrêté</span>");
        return;
    }
    if (!e->stats_encoder[0]) {
        gtk_label_set_markup(GTK_LABEL(e->stats_label),
                             "<b>En cours</b>  — en attente du flux vidéo…");
        return;
    }

    GString *text = g_string_new(NULL);
    g_string_append_printf(text,
        "<b>%s</b>  ·  %s  ·  port %d  ·  jusqu’à %d clients",
        e->name, e->stats_encoder, e->announced_port, e->stats_max_clients);
    static const char *names[2] = { "Bas / GamePad", "Haut / TV" };
    for (int screen = 0; screen < 2; screen++) {
        if (!e->stats_source_w[screen])
            continue;
        g_string_append_printf(text,
            "\n%s : %d×%d → %d×%d  ·  %.1f fps  ·  %.2f Mbit/s  ·  "
            "%u images  ·  %d spectateur%s",
            names[screen], e->stats_source_w[screen], e->stats_source_h[screen],
            e->stats_output_w[screen], e->stats_output_h[screen],
            e->stats_measured_fps[screen] > 0.0
                ? e->stats_measured_fps[screen] : e->stats_nominal_fps,
            e->stats_mbps[screen], e->stats_frames[screen],
            e->stats_clients[screen], e->stats_clients[screen] > 1 ? "s" : "");
    }
    if (e->stats_event[0]) {
        char *escaped = g_markup_escape_text(e->stats_event, -1);
        g_string_append_printf(text, "\n<small>%s</small>", escaped);
        g_free(escaped);
    }
    gtk_label_set_markup(GTK_LABEL(e->stats_label), text->str);
    g_string_free(text, TRUE);
}

static void reset_stats(Emu *e)
{
    memset(e->stats_source_w, 0, sizeof(e->stats_source_w));
    memset(e->stats_source_h, 0, sizeof(e->stats_source_h));
    memset(e->stats_output_w, 0, sizeof(e->stats_output_w));
    memset(e->stats_output_h, 0, sizeof(e->stats_output_h));
    memset(e->stats_clients, 0, sizeof(e->stats_clients));
    memset(e->stats_frames, 0, sizeof(e->stats_frames));
    memset(e->stats_mbps, 0, sizeof(e->stats_mbps));
    memset(e->stats_measured_fps, 0, sizeof(e->stats_measured_fps));
    e->stats_nominal_fps = e->stats_max_clients = 0;
    e->stats_encoder[0] = e->stats_event[0] = '\0';
    refresh_stats(e);
}

static void parse_stats(Emu *e, const char *line)
{
    int sw, sh, ow, oh, fps, clients, max_clients;
    unsigned frames, port;
    double measured_fps, mbps;
    char encoder[64], screen_name[16];

    if (sscanf(line,
        "bottom_screen: %dx%d @ %d fps, %63[^,], listening on port %u (up to %d clients)",
        &sw, &sh, &fps, encoder, &port, &max_clients) == 6) {
        e->stats_source_w[0] = e->stats_output_w[0] = sw;
        e->stats_source_h[0] = e->stats_output_h[0] = sh;
        e->stats_nominal_fps = fps;
        e->stats_max_clients = max_clients;
        e->announced_port = (int)port;
        g_strlcpy(e->stats_encoder, encoder, sizeof(e->stats_encoder));
        g_strlcpy(e->stats_event, "Encodeur prêt", sizeof(e->stats_event));
        refresh_stats(e);
        return;
    }
    if (sscanf(line,
        "bottom_screen: stats screen=%15s frames=%u fps=%lf mbps=%lf clients=%d "
        "source=%dx%d output=%dx%d encoder=%63s",
        screen_name, &frames, &measured_fps, &mbps, &clients,
        &sw, &sh, &ow, &oh, encoder) == 10) {
        int screen = g_strcmp0(screen_name, "top") == 0 ? 1 : 0;
        e->stats_frames[screen] = frames;
        e->stats_measured_fps[screen] = measured_fps;
        e->stats_mbps[screen] = mbps;
        e->stats_clients[screen] = clients;
        e->stats_source_w[screen] = sw;
        e->stats_source_h[screen] = sh;
        e->stats_output_w[screen] = ow;
        e->stats_output_h[screen] = oh;
        g_strlcpy(e->stats_encoder, encoder, sizeof(e->stats_encoder));
        refresh_stats(e);
        return;
    }
    if (sscanf(line, "bottom_screen: sending %dx%d from a %dx%d source",
               &ow, &oh, &sw, &sh) == 4) {
        e->stats_output_w[0] = ow;
        e->stats_output_h[0] = oh;
        e->stats_source_w[0] = sw;
        e->stats_source_h[0] = sh;
        refresh_stats(e);
        return;
    }
    if (strstr(line, "bottom_screen: client ") ||
        strstr(line, "bottom_screen: native client ") ||
        strstr(line, "bottom_screen: web client ") ||
        strstr(line, "bottom_screen: bitrate now ") ||
        strstr(line, "bottom_screen: encoding the ")) {
        char *copy = g_strdup(line + strlen("bottom_screen: "));
        g_strchomp(copy);
        g_strlcpy(e->stats_event, copy, sizeof(e->stats_event));
        g_free(copy);
        refresh_stats(e);
    }
}

/* A single physical pad: never overlap two libdrc processes. The pattern
 * has its own port and remains available while the emulator starts. */
static Emu *pad_owner;
static Emu *latest_emu;
static GPid pad_pid, pattern_pid;
static int pad_port, wanted_port, pattern_port;
static gboolean pad_stopping, pad_auto_waiting, closing;
static GtkWidget *pad_status, *ap_status, *pair_symbols;
static GtkWidget *pair_button, *reconnect_button, *stop_ap_button;
typedef enum { AP_JOB_NONE, AP_JOB_PAIR, AP_JOB_NORMAL, AP_JOB_STOP } ApJob;
static GPid ap_job_pid;
static ApJob ap_job, pending_ap_job;
static char pending_pair_pin[9];
static gboolean pair_on_normal_ap;
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

/* hostapd removes its control socket when the AP stops. This is the same
 * socket bs_gamepad uses to find the wireless interface and reset the
 * GamePad association, so it is also the useful state to expose here. */
static gboolean refresh_ap_status(gpointer unused)
{
    (void)unused;
    if (!ap_status)
        return G_SOURCE_CONTINUE;

    if (ap_job == AP_JOB_PAIR && !pair_on_normal_ap) {
        gtk_label_set_markup(GTK_LABEL(ap_status),
            "<span foreground=\"#e5a442\">●</span> <b>AP d’appairage</b> — "
            "entrez les symboles sur le GamePad");
        return G_SOURCE_CONTINUE;
    }
    if (ap_job == AP_JOB_NORMAL) {
        gtk_label_set_markup(GTK_LABEL(ap_status),
            "<span foreground=\"#e5a442\">●</span> <b>Démarrage de l’AP…</b>");
        return G_SOURCE_CONTINUE;
    }
    if (ap_job == AP_JOB_STOP) {
        gtk_label_set_markup(GTK_LABEL(ap_status),
            "<span foreground=\"#e5a442\">●</span> <b>Arrêt de l’AP…</b>");
        return G_SOURCE_CONTINUE;
    }
    if (ap_job == AP_JOB_PAIR && pair_on_normal_ap) {
        gtk_label_set_markup(GTK_LABEL(ap_status),
            "<span foreground=\"#45c46b\">●</span> <b>AP normal actif</b> — "
            "attente de la reconnexion du GamePad");
        return G_SOURCE_CONTINUE;
    }

    char *iface = NULL;
    const char *forced = g_getenv("BS_PAD_IFACE");
    if (forced && *forced) {
        char *socket = g_build_filename("/var/run/hostapd", forced, NULL);
        if (g_file_test(socket, G_FILE_TEST_EXISTS))
            iface = g_strdup(forced);
        g_free(socket);
    } else {
        GDir *dir = g_dir_open("/var/run/hostapd", 0, NULL);
        const char *name;
        while (dir && (name = g_dir_read_name(dir))) {
            if (name[0] != '.') {
                iface = g_strdup(name);
                break;
            }
        }
        if (dir)
            g_dir_close(dir);
    }

    if (iface) {
        char *escaped = g_markup_escape_text(iface, -1);
        char *markup = g_strdup_printf(
            "<span foreground=\"#45c46b\">●</span> <b>AP actif</b> — interface %s",
            escaped);
        gtk_label_set_markup(GTK_LABEL(ap_status), markup);
        g_free(markup);
        g_free(escaped);
    } else {
        gtk_label_set_markup(GTK_LABEL(ap_status),
            "<span foreground=\"#e05d5d\">●</span> <b>AP arrêté</b> — "
            "utilisez Sync GamePad ou Launch AP / Reconnect");
    }
    g_free(iface);
    return G_SOURCE_CONTINUE;
}

static void start_pad(void);
static void start_ap_job(ApJob kind, const char *pin);

static void pad_gone(GPid pid, gint status, gpointer unused)
{
    (void)unused;
    g_spawn_close_pid(pid);
    pad_pid = 0;
    if (closing) return;
    if (pending_ap_job != AP_JOB_NONE) {
        ApJob next = pending_ap_job;
        char pin[sizeof(pending_pair_pin)];
        g_strlcpy(pin, pending_pair_pin, sizeof(pin));
        pending_ap_job = AP_JOB_NONE;
        pending_pair_pin[0] = '\0';
        start_ap_job(next, next == AP_JOB_PAIR ? pin : NULL);
        return;
    }
    if (pad_stopping) {
        pad_stopping = FALSE;
        start_pad();
    } else {
        wanted_port = 0;
        pad_message(status ? "GamePad arrêté : vérifier l’AP et les logs du terminal."
                           : "GamePad déconnecté. Relancer avec Launch AP / Reconnect.");
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
    pad_auto_waiting = FALSE;
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
        if (!closing) pad_message("La mire s’est arrêtée. Relancer avec Launch AP / Reconnect.");
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
        pad_message(pad_auto_waiting
            ? "Démarrage de la mire — connexion automatique au prochain émulateur…"
            : "Démarrage de la mire GamePad…");
    } else {
        pad_message(error->message);
        g_clear_error(&error);
    }
    g_free(binary);
}

static void begin_gamepad_connection(void)
{
    Emu *source = NULL;
    if (pad_owner && pad_owner->pid)
        source = pad_owner;
    else if (latest_emu && latest_emu->pid)
        source = latest_emu;
    else
        for (int i = 0; i < EMU_COUNT && !source; i++)
            if (emus[i].pid)
                source = &emus[i];

    pad_auto_waiting = source == NULL;
    /* The Wii U pattern is only a waiting room. The first emulator
     * launched afterwards takes ownership before its server starts. */
    on_pattern(NULL, source ? source : &emus[2]);
}

static void ap_buttons_sensitive(gboolean sensitive)
{
    if (pair_button) gtk_widget_set_sensitive(pair_button, sensitive);
    if (reconnect_button) gtk_widget_set_sensitive(reconnect_button, sensitive);
    if (stop_ap_button) gtk_widget_set_sensitive(stop_ap_button, sensitive);
}

static gboolean ap_output(GIOChannel *channel, GIOCondition cond, gpointer unused)
{
    (void)unused;
    char *line = NULL;
    while (g_io_channel_read_line(channel, &line, NULL, NULL, NULL) == G_IO_STATUS_NORMAL) {
        fputs(line, stderr);
        g_strchomp(line);
        if (strstr(line, "PIN arme"))
            pad_message("AP prêt : entrez maintenant les quatre symboles sur le GamePad.");
        else if (strstr(line, "M8 detecte"))
            pad_message("GamePad appairé : passage à l’AP normal…");
        else if (strstr(line, "AP normal actif")) {
            pair_on_normal_ap = TRUE;
            refresh_ap_status(NULL);
            if (ap_job == AP_JOB_PAIR)
                pad_message("Appairage réussi : attente de la reconnexion du GamePad…");
        } else if (strstr(line, "GamePad connectee"))
            pad_message("GamePad connecté : démarrage du flux…");
        else if (strstr(line, "AP arrêté"))
            pad_message("AP arrêté. L’interface Wi-Fi a été rendue au système.");
        else if (strstr(line, "erreur:"))
            pad_message(line);
        g_free(line);
        line = NULL;
    }
    g_free(line);
    return !(cond & (G_IO_HUP | G_IO_ERR));
}

static void ap_job_gone(GPid pid, gint status, gpointer unused)
{
    (void)unused;
    g_spawn_close_pid(pid);
    ap_job_pid = 0;
    ApJob finished = ap_job;
    ap_job = AP_JOB_NONE;
    pair_on_normal_ap = FALSE;
    ap_buttons_sensitive(TRUE);
    refresh_ap_status(NULL);
    if (closing)
        return;

    GError *error = NULL;
    if (!g_spawn_check_wait_status(status, &error)) {
        char *message = g_strdup_printf("%s impossible : %s",
            finished == AP_JOB_PAIR ? "Appairage" :
            finished == AP_JOB_STOP ? "Arrêt de l’AP" : "Démarrage de l’AP",
            error ? error->message : "échec inconnu");
        pad_message(message);
        g_free(message);
        g_clear_error(&error);
        return;
    }
    if (finished == AP_JOB_STOP) {
        pad_message("AP arrêté. Le GamePad est déconnecté.");
        return;
    }
    pad_message(finished == AP_JOB_PAIR
        ? "Appairage terminé. Connexion au flux…"
        : "AP normal actif. Reconnexion au flux…");
    begin_gamepad_connection();
}

/* Run the radio setup with a graphical PolicyKit authentication prompt.
 * Passing settings through /usr/bin/env is intentional: pkexec sanitises the
 * environment, but advanced installations may override the adapter or the
 * drc-hostap checkout with the same DRC_* variables as the command-line tools. */
static void start_ap_job(ApJob kind, const char *pin)
{
    if (ap_job_pid || pending_ap_job != AP_JOB_NONE) {
        pad_message("Une opération GamePad est déjà en cours.");
        return;
    }

    wanted_port = 0;
    if (pad_pid) {
        pending_ap_job = kind;
        g_strlcpy(pending_pair_pin, pin ? pin : "", sizeof(pending_pair_pin));
        pad_stopping = FALSE;
        kill(pad_pid, SIGTERM);
        ap_buttons_sensitive(FALSE);
        pad_message("Arrêt du flux GamePad avant de reconfigurer l’AP…");
        return;
    }

    const char *tool = kind == AP_JOB_PAIR ? "ap-pair.sh" :
                       kind == AP_JOB_STOP ? "ap-stop.sh" : "ap-normal.sh";
    const char *installed_helper =
        "/usr/local/libexec/bottom-screen-gamepad/gamepad-ap-control";
    const gboolean installed =
        g_file_test(installed_helper, G_FILE_TEST_IS_EXECUTABLE);
    char *script = NULL;
    if (!installed) {
        script = g_build_filename(g_project, "gamepad", "tools", tool, NULL);
        if (!g_file_test(script, G_FILE_TEST_IS_REGULAR)) {
            char *message = g_strdup_printf("Outil GamePad introuvable : %s", script);
            pad_message(message);
            g_free(message);
            g_free(script);
            ap_buttons_sensitive(TRUE);
            return;
        }
    }

    GPtrArray *args = g_ptr_array_new_with_free_func(g_free);
    if (geteuid() != 0) {
        char *pkexec = g_find_program_in_path("pkexec");
        if (!pkexec) {
            pad_message("pkexec est requis pour configurer l’interface Wi-Fi.");
            g_ptr_array_free(args, TRUE);
            g_free(script);
            ap_buttons_sensitive(TRUE);
            return;
        }
        g_ptr_array_add(args, pkexec);
    }
    if (installed) {
        const char *action = kind == AP_JOB_PAIR ? "pair" :
                             kind == AP_JOB_STOP ? "stop" : "start";
        g_ptr_array_add(args, g_strdup(installed_helper));
        g_ptr_array_add(args, g_strdup(action));
        if (pin) g_ptr_array_add(args, g_strdup(pin));
    } else {
        /* Compatibility path before the one-time Polkit installation. It
         * deliberately still asks for authentication on every operation. */
        g_ptr_array_add(args, g_strdup("/usr/bin/env"));
        static const char *const names[] = {
            "DRC_IF", "DRC_AP_MAC", "DRC_HOSTAP", "DRC_MTU",
            "DRC_DNSMASQ_CONF", "DRC_UUID", "DRC_PAIR_CONF", "DRC_NORMAL_CONF"
        };
        gboolean has_iface = FALSE, has_hostap = FALSE;
        for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
            const char *value = g_getenv(names[i]);
            if (value && *value) {
                g_ptr_array_add(args, g_strdup_printf("%s=%s", names[i], value));
                if (g_strcmp0(names[i], "DRC_IF") == 0) has_iface = TRUE;
                if (g_strcmp0(names[i], "DRC_HOSTAP") == 0) has_hostap = TRUE;
            }
        }
        if (!has_iface) {
            const char *iface = g_getenv("BS_PAD_IFACE");
            if (iface && *iface)
                g_ptr_array_add(args, g_strdup_printf("DRC_IF=%s", iface));
        }
        if (!has_hostap) {
            char *checkout = g_build_filename(g_get_home_dir(), "rtw88_TSF",
                                              "drc-hostap", NULL);
            if (g_file_test(checkout, G_FILE_TEST_IS_DIR))
                g_ptr_array_add(args, g_strdup_printf("DRC_HOSTAP=%s", checkout));
            g_free(checkout);
        }
        const char *custom_run = g_getenv("DRC_RUN");
        g_ptr_array_add(args, g_strdup_printf("DRC_RUN=%s",
            custom_run && *custom_run ? custom_run : "/run/bottom-screen-gamepad"));
        g_ptr_array_add(args, g_strdup("/bin/bash"));
        g_ptr_array_add(args, script);
        if (pin) g_ptr_array_add(args, g_strdup(pin));
    }
    g_ptr_array_add(args, NULL);

    GError *error = NULL;
    int out_fd = -1, err_fd = -1;
    if (g_spawn_async_with_pipes(g_project, (char **)args->pdata, NULL,
                                 G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL,
                                 &ap_job_pid, NULL, &out_fd, &err_fd, &error)) {
        ap_job = kind;
        pair_on_normal_ap = FALSE;
        ap_buttons_sensitive(FALSE);
        g_child_watch_add(ap_job_pid, ap_job_gone, NULL);
        watch_pipe(out_fd, ap_output, NULL);
        watch_pipe(err_fd, ap_output, NULL);
        if (installed)
            pad_message(kind == AP_JOB_PAIR
                ? "AP d’appairage en cours : entrez les symboles sur le GamePad."
                : kind == AP_JOB_STOP ? "Arrêt de l’AP…" : "Démarrage de l’AP normal…");
        else
            pad_message(kind == AP_JOB_PAIR
                ? "Autorisez l’accès Wi-Fi, puis entrez les symboles sur le GamePad."
                : kind == AP_JOB_STOP
                    ? "Autorisez l’accès Wi-Fi pour arrêter l’AP…"
                    : "Autorisez l’accès Wi-Fi pour lancer l’AP normal…");
        refresh_ap_status(NULL);
    } else {
        pad_message(error->message);
        g_clear_error(&error);
        ap_buttons_sensitive(TRUE);
    }
    g_ptr_array_free(args, TRUE);
}

static void on_gamepad_pair(GtkButton *button, gpointer unused)
{
    (void)button; (void)unused;
    static const char *const symbols[] = { "♠", "♥", "♦", "♣" };
    int value[4];
    char pin[9];
    GString *shown = g_string_new("<span size=\"xx-large\">");
    for (int i = 0; i < 4; i++) {
        value[i] = g_random_int_range(0, 4);
        g_string_append_printf(shown, "%s%s", i ? "   " : "", symbols[value[i]]);
    }
    g_string_append(shown, "</span>  <small>à saisir sur le GamePad</small>");
    gtk_label_set_markup(GTK_LABEL(pair_symbols), shown->str);
    g_string_free(shown, TRUE);
    g_snprintf(pin, sizeof(pin), "%d%d%d%d5678",
               value[0], value[1], value[2], value[3]);
    start_ap_job(AP_JOB_PAIR, pin);
}

static void on_gamepad_reconnect(GtkButton *button, gpointer unused)
{
    (void)button; (void)unused;
    start_ap_job(AP_JOB_NORMAL, NULL);
}

static void on_gamepad_stop_ap(GtkButton *button, gpointer unused)
{
    (void)button; (void)unused;
    pad_owner = NULL;
    pad_auto_waiting = FALSE;
    start_ap_job(AP_JOB_STOP, NULL);
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
        parse_stats(e, line);
        const char *found = strstr(line, "listening on port ");
        if (found) {
            e->announced_port = atoi(found + strlen("listening on port "));
            if (pad_owner == e) select_pad_port(e->announced_port);
            char *msg = g_markup_printf_escaped("<b>streaming on port %d</b>", e->announced_port);
            set_status(e, msg);
            g_free(msg);
        } else if (strstr(line, "bottom_screen:") &&
                   !strstr(line, "bottom_screen: stats ")) {
            char *msg = g_markup_printf_escaped("<small>%s</small>", g_strchomp(line));
            set_status(e, msg);
            g_free(msg);
        }
        g_free(line); line = NULL;
    }
    g_free(line);
    return result == G_IO_STATUS_AGAIN && !(cond & (G_IO_HUP | G_IO_ERR));
}

static void launch_emu(Emu *e, gboolean firmware);
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
    emu_buttons(e);
    set_status(e, "<b>Prêt</b>");
    refresh_stats(e);
    if (e->restart_wanted && !closing) {
        e->restart_wanted = FALSE;
        launch_emu(e, e->last_launch_firmware);
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
    gtk_widget_set_tooltip_text(e->path_label, e->binary);
    if (ok)
        gtk_widget_hide(e->path_label);
    else
        gtk_widget_show(e->path_label);
    set_status(e, ok ? "<b>Prêt</b>"
                     : "<b>Exécutable introuvable</b>");
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

static void on_browse_firmware(GtkButton *button, gpointer user)
{
    Emu *e = user;
    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Choisir le firmware DS",
        GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(button))),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "Annuler", GTK_RESPONSE_CANCEL, "Choisir", GTK_RESPONSE_ACCEPT, NULL);
    const char *current = selected_firmware(e);
    if (current && *current)
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog), current);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *chosen = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (chosen) {
            gtk_entry_set_text(GTK_ENTRY(e->firmware_field), chosen);
            g_strlcpy(e->firmware, chosen, sizeof(e->firmware));
            g_free(chosen);
            save_paths();
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
    (void)button;
    if (!e->pid) { launch_emu(e, e->last_launch_firmware); return; }
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
    firmware_button_state(e);
    gtk_widget_set_sensitive(e->quit, running);
    gtk_widget_set_sensitive(e->restart, TRUE);
}

static void launch_emu(Emu *e, gboolean firmware)
{
    if (e->pid)
        return;     /* Quit and Restart are their own buttons now */
    if (!g_file_test(e->binary, G_FILE_TEST_IS_EXECUTABLE)) {
        set_status(e, "<small>set the path to the program</small>");
        return;
    }

    apply_scale(e);

    const char *firmware_value = selected_firmware(e);
    char *why = NULL;
    if (firmware) {
        if (!firmware_value || !*firmware_value) {
            set_status(e, "<small>aucun firmware/menu détecté</small>");
            return;
        }
        if (e->scale_kind != SCALE_CEMU &&
            !g_file_test(firmware_value, G_FILE_TEST_IS_REGULAR)) {
            set_status(e, "<small>firmware/menu introuvable</small>");
            return;
        }
        if (e->scale_kind == SCALE_MELONDS &&
            !melonds_set_firmware(firmware_value, &why)) {
            char *msg = g_markup_printf_escaped("<small>%s</small>", why);
            set_status(e, msg);
            g_free(msg);
            g_free(why);
            return;
        }
        g_strlcpy(e->firmware, firmware_value, sizeof(e->firmware));
        save_paths();
    }

    /*
     * The stream is configured through the environment rather than the
     * emulator's own settings, so a launch never rewrites a preference
     * somebody set by hand -- and Cemu would throw such an edit away on
     * exit in any case. All three read the same two names.
     */
    const gboolean claim_waiting_pad = pad_auto_waiting;
    if (claim_waiting_pad)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(e->enable), TRUE);
    gboolean on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(e->enable));
    int port = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(e->port));

    char **env = g_get_environ();
    env = g_environ_setenv(env, "BOTTOM_SCREEN", on ? "1" : "0", TRUE);
    char portbuf[16];
    g_snprintf(portbuf, sizeof(portbuf), "%d", port);
    env = g_environ_setenv(env, "BOTTOM_SCREEN_PORT", portbuf, TRUE);

    char *argv[4] = { e->binary, NULL, NULL, NULL };
    if (firmware && e->scale_kind == SCALE_MELONDS) {
        argv[1] = "--boot";
        argv[2] = "always";
    } else if (firmware && e->scale_kind == SCALE_AZAHAR) {
        argv[1] = (char *)firmware_value;
    } else if (firmware && e->scale_kind == SCALE_CEMU) {
        argv[1] = "--title-id";
        argv[2] = (char *)firmware_value;
    }
    gint out_fd = -1, err_fd = -1;
    GError *error = NULL;

    char *cwd = g_path_get_dirname(e->binary);
    gboolean ok = g_spawn_async_with_pipes(
        cwd, argv, env,
        G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL,
        &e->pid, NULL, &out_fd, &err_fd, &error);
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

    latest_emu = e;
    if (claim_waiting_pad) {
        pad_owner = e;
        pad_auto_waiting = FALSE;
    }
    e->last_launch_firmware = firmware;
    reset_stats(e);
    emu_buttons(e);
    set_status(e, firmware ? "<b>démarrage du firmware…</b>"
                           : "<b>démarrage…</b>");

    watch_pipe(out_fd, on_output, e);
    watch_pipe(err_fd, on_output, e);

    g_child_watch_add(e->pid, on_child_gone, e);
}

static void on_launch(GtkButton *button, gpointer user)
{
    (void)button;
    launch_emu(user, FALSE);
}

static void on_launch_firmware(GtkButton *button, gpointer user)
{
    (void)button;
    launch_emu(user, TRUE);
}

/* ------------------------------------------------------------------ ui */

static GtkWidget *build_emu_panel(Emu *e)
{
    GtkWidget *frame = gtk_frame_new(NULL);
    GtkWidget *title = gtk_label_new(NULL);
    char *tm = g_markup_printf_escaped("<b>%s</b>  <span foreground=\"#888888\">%s</span>",
                                       e->name, e->console);
    gtk_label_set_markup(GTK_LABEL(title), tm);
    g_free(tm);
    gtk_frame_set_label_widget(GTK_FRAME(frame), title);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_container_set_border_width(GTK_CONTAINER(box), 4);
    gtk_container_add(GTK_CONTAINER(frame), box);

    GtkWidget *settings = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    e->enable = gtk_check_button_new_with_label("Stream réseau");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(e->enable), TRUE);
    gtk_widget_set_tooltip_text(e->enable,
        "Active le serveur pour tous les clients réseau. La connexion au "
        "GamePad physique est gérée dans l’onglet WiiU GamePad.");
    gtk_box_pack_start(GTK_BOX(settings), e->enable, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(settings), gtk_label_new("Port"), FALSE, FALSE, 0);
    e->port = gtk_spin_button_new_with_range(1024, 65535, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(e->port), e->default_port);
    gtk_widget_set_size_request(e->port, 82, -1);
    gtk_box_pack_start(GTK_BOX(settings), e->port, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(settings), gtk_label_new("Résolution"), FALSE, FALSE, 0);
    e->scale = gtk_combo_box_text_new();
    for (int n = 1; n <= 6; n++) {
        char item[64];
        g_snprintf(item, sizeof(item), "%d\xc3\x97  (%d\xc3\x97%d)",
                   n, e->native_w * n, e->native_h * n);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(e->scale), item);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(e->scale), 0);
    gtk_widget_set_tooltip_text(e->scale, e->scale_note);
    gtk_box_pack_start(GTK_BOX(settings), e->scale, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), settings, FALSE, FALSE, 0);

    GtkWidget *firmware_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(firmware_row),
        gtk_label_new(e->scale_kind == SCALE_MELONDS ? "Firmware DS" : "Menu système"),
        FALSE, FALSE, 0);
    populate_firmware_field(e);
    gtk_widget_set_hexpand(e->firmware_field, TRUE);
    gtk_box_pack_start(GTK_BOX(firmware_row), e->firmware_field, TRUE, TRUE, 0);
    if (e->scale_kind == SCALE_MELONDS) {
        GtkWidget *firmware_browse = gtk_button_new_with_label("Parcourir…");
        g_signal_connect(firmware_browse, "clicked",
                         G_CALLBACK(on_browse_firmware), e);
        gtk_box_pack_start(GTK_BOX(firmware_row), firmware_browse, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(box), firmware_row, FALSE, FALSE, 0);

    GtkWidget *brow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    e->launch = gtk_button_new_with_label("Launch");
    g_signal_connect(e->launch, "clicked", G_CALLBACK(on_launch), e);
    gtk_box_pack_start(GTK_BOX(brow), e->launch, FALSE, FALSE, 0);

    e->launch_firmware = gtk_button_new_with_label("Launch firmware");
    g_signal_connect(e->launch_firmware, "clicked",
                     G_CALLBACK(on_launch_firmware), e);
    gtk_box_pack_start(GTK_BOX(brow), e->launch_firmware, FALSE, FALSE, 0);

    e->quit = gtk_button_new_with_label("Quit");
    g_signal_connect(e->quit, "clicked", G_CALLBACK(on_quit), e);
    gtk_box_pack_start(GTK_BOX(brow), e->quit, FALSE, FALSE, 0);

    e->restart = gtk_button_new_with_label("Restart");
    g_signal_connect(e->restart, "clicked", G_CALLBACK(on_restart), e);
    gtk_box_pack_start(GTK_BOX(brow), e->restart, FALSE, FALSE, 0);

    GtkWidget *browse = gtk_button_new_with_label("Exécutable…");
    g_signal_connect(browse, "clicked", G_CALLBACK(on_browse), e);
    gtk_box_pack_end(GTK_BOX(brow), browse, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), brow, FALSE, FALSE, 0);

    e->path_label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(e->path_label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(e->path_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_no_show_all(e->path_label, TRUE);
    gtk_widget_set_tooltip_text(e->path_label, e->binary);
    gtk_box_pack_start(GTK_BOX(box), e->path_label, FALSE, FALSE, 0);

    e->status = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(e->status), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(e->status), PANGO_ELLIPSIZE_END);
    gtk_widget_set_tooltip_text(e->status,
        "Les mesures détaillées se trouvent dans l’onglet Statistiques");
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
    gtk_window_set_default_size(GTK_WINDOW(win), 720, 565);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 9);
    gtk_container_add(GTK_CONTAINER(win), outer);

    char *lan = guess_lan_address();
    g_host_label = gtk_label_new(NULL);
    char *hm = g_markup_printf_escaped("Clients : <b>%s</b>", lan);
    gtk_label_set_markup(GTK_LABEL(g_host_label), hm);
    g_free(hm);
    g_free(lan);
    gtk_box_pack_start(GTK_BOX(outer), g_host_label, FALSE, FALSE, 0);

    GtkWidget *notebook = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(outer), notebook, TRUE, TRUE, 0);

    GtkWidget *emu_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(emu_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *emu_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(emu_box), 5);
    gtk_container_add(GTK_CONTAINER(emu_scroll), emu_box);
    for (int i = 0; i < EMU_COUNT; i++)
        gtk_box_pack_start(GTK_BOX(emu_box), build_emu_panel(&emus[i]),
                           FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), emu_scroll,
                             gtk_label_new("Émulateurs"));

    GtkWidget *pad_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(pad_box), 10);

    GtkWidget *sync_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *sync_title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(sync_title), "<b>Connexion du GamePad</b>");
    gtk_box_pack_start(GTK_BOX(sync_row), sync_title, FALSE, FALSE, 0);
    pair_button = gtk_button_new_with_label("Sync GamePad");
    gtk_widget_set_tooltip_text(pair_button,
        "Appairer un nouveau GamePad avec les quatre symboles Wii U");
    g_signal_connect(pair_button, "clicked", G_CALLBACK(on_gamepad_pair), NULL);
    gtk_box_pack_start(GTK_BOX(sync_row), pair_button, FALSE, FALSE, 0);
    reconnect_button = gtk_button_new_with_label("Launch AP / Reconnect");
    gtk_widget_set_tooltip_text(reconnect_button,
        "Lancer l’AP normal et reconnecter un GamePad déjà appairé");
    g_signal_connect(reconnect_button, "clicked",
                     G_CALLBACK(on_gamepad_reconnect), NULL);
    gtk_box_pack_start(GTK_BOX(sync_row), reconnect_button, FALSE, FALSE, 0);
    stop_ap_button = gtk_button_new_with_label("Stop AP");
    gtk_widget_set_tooltip_text(stop_ap_button,
        "Arrêter le point d’accès et rendre l’interface Wi-Fi au système");
    g_signal_connect(stop_ap_button, "clicked",
                     G_CALLBACK(on_gamepad_stop_ap), NULL);
    gtk_box_pack_start(GTK_BOX(sync_row), stop_ap_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pad_box), sync_row, FALSE, FALSE, 0);

    pair_symbols = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(pair_symbols),
        "<small>Sync GamePad affichera ici les quatre symboles à saisir.</small>");
    gtk_label_set_xalign(GTK_LABEL(pair_symbols), 0.0f);
    gtk_box_pack_start(GTK_BOX(pad_box), pair_symbols, FALSE, FALSE, 0);

    GtkWidget *ap_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(ap_row), gtk_label_new("Point d’accès"), FALSE, FALSE, 0);
    ap_status = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(ap_status), 0.0f);
    gtk_box_pack_start(GTK_BOX(ap_row), ap_status, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pad_box), ap_row, FALSE, FALSE, 0);
    refresh_ap_status(NULL);
    g_timeout_add_seconds(1, refresh_ap_status, NULL);

    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(pad_box), separator, FALSE, FALSE, 2);

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

    GtkWidget *help = gtk_label_new(
        "Sync GamePad = nouvel appairage · Launch AP / Reconnect = GamePad déjà connu.\n"
        "Stop AP coupe la liaison Wi-Fi GamePad. Sans émulateur, la mire attend puis bascule automatiquement."
        "  Mire : sticks = couleurs · boutons = notes · tactile = son continu");
    gtk_label_set_line_wrap(GTK_LABEL(help), TRUE);
    gtk_box_pack_start(GTK_BOX(pad_box), help, FALSE, FALSE, 0);
    pad_status = gtk_label_new("GamePad prêt à connecter");
    gtk_label_set_line_wrap(GTK_LABEL(pad_status), TRUE);
    gtk_label_set_xalign(GTK_LABEL(pad_status), 0.0f);
    gtk_box_pack_start(GTK_BOX(pad_box), pad_status, FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), pad_box,
                             gtk_label_new("WiiU GamePad"));

    GtkWidget *stats_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7);
    gtk_container_set_border_width(GTK_CONTAINER(stats_box), 10);
    GtkWidget *stats_intro = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(stats_intro),
        "<b>Encodage et envoi en direct</b>\n"
        "<small>Les mesures suivent automatiquement les émulateurs lancés ici. "
        "Chaque écran possède son propre encodeur lorsqu’il est regardé.</small>");
    gtk_label_set_xalign(GTK_LABEL(stats_intro), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(stats_intro), TRUE);
    gtk_box_pack_start(GTK_BOX(stats_box), stats_intro, FALSE, FALSE, 0);
    for (int i = 0; i < EMU_COUNT; i++) {
        GtkWidget *frame = gtk_frame_new(emus[i].console);
        emus[i].stats_label = gtk_label_new(NULL);
        gtk_label_set_xalign(GTK_LABEL(emus[i].stats_label), 0.0f);
        gtk_label_set_line_wrap(GTK_LABEL(emus[i].stats_label), TRUE);
        gtk_widget_set_margin_start(emus[i].stats_label, 8);
        gtk_widget_set_margin_end(emus[i].stats_label, 8);
        gtk_widget_set_margin_top(emus[i].stats_label, 8);
        gtk_widget_set_margin_bottom(emus[i].stats_label, 8);
        gtk_container_add(GTK_CONTAINER(frame), emus[i].stats_label);
        gtk_box_pack_start(GTK_BOX(stats_box), frame, FALSE, FALSE, 0);
        refresh_stats(&emus[i]);
    }
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), stats_box,
                             gtk_label_new("Statistiques"));
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
    /* Debian's GTK accessibility module prints a dbind warning when no
     * AT-SPI session bus exists (common over SSH and in minimal desktops).
     * Keep accessibility enabled whenever that service is actually present;
     * otherwise tell GTK not to initialise a bridge that cannot connect. */
    if (!g_getenv("NO_AT_BRIDGE") && !g_getenv("AT_SPI_BUS_ADDRESS")) {
        char *atspi = g_build_filename(g_get_user_runtime_dir(),
                                       "at-spi", "bus_0", NULL);
        if (!g_file_test(atspi, G_FILE_TEST_EXISTS))
            g_setenv("NO_AT_BRIDGE", "1", FALSE);
        g_free(atspi);
    }

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
