/*
 * The launcher's settings file, read and written.
 *
 * It used to hold three lines -- where each emulator lives -- and
 * everything else was retyped at every start. Now it holds the ports,
 * the resolutions and the whole GamePad panel, which means a key that
 * does not match, or a default that is indistinguishable from a saved
 * value, quietly changes how a session starts.
 *
 * That second one is the reason this exists: zero is a real resolution
 * index, so "nothing was saved" cannot also be zero. Left as it was,
 * every emulator came up forced to 1x.
 */
#define main launcher_main
#include "../launcher/bs_launcher.c"
#undef main
#include <assert.h>
#include <sys/stat.h>

static void wait_for_emu(Emu *e)
{
    gint64 deadline = g_get_monotonic_time() + 5000000;
    while (e->pid && g_get_monotonic_time() < deadline) {
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(10000);
    }
    assert(!e->pid);
}

static char *read_required(const char *path)
{
    char *text = NULL;
    assert(g_file_get_contents(path, &text, NULL, NULL));
    return text;
}

int main(int argc, char **argv)
{
    gtk_init_check(&argc, &argv);

    char *tmp = g_dir_make_tmp("bs-settings-test-XXXXXX", NULL);
    assert(tmp);
    g_setenv("XDG_CONFIG_HOME", tmp, TRUE);

    /* Nothing saved yet: every emulator keeps what it was built with. */
    load_paths();
    for (int i = 0; i < EMU_COUNT; i++) {
        assert(emus[i].saved_port == -1);
        assert(emus[i].saved_scale == -1);
    }
    for (int p = 0; p < 6; p++)
        assert(saved_pad[p] == -1);

    /* A file with a resolution of zero, which is a value and not a gap. */
    char *dir = g_build_filename(tmp, "bottom_screen", NULL);
    assert(!g_mkdir_with_parents(dir, 0700));
    char *path = g_build_filename(dir, "emulators.conf", NULL);
    assert(g_file_set_contents(path,
        "melonds=/somewhere/melonDS\n"
        "melonds.firmware=/somewhere/firmware.bin\n"
        "melonds.port=5555\n"
        "melonds.resolution=0\n"
        "azahar.resolution=5\n"
        "pad.screen=1\n"
        "pad.resolution=4\n"
        "pad.top_resolution=0\n"
        "pad.filter=3\n"
        "pad.sharpness=2\n"
        "pad.bitrate=16\n", -1, NULL));

    load_paths();
    assert(g_strcmp0(emus[0].key, "melonds") == 0);
    assert(g_strcmp0(emus[0].binary, "/somewhere/melonDS") == 0);
    assert(g_strcmp0(emus[0].firmware, "/somewhere/firmware.bin") == 0);
    assert(emus[0].saved_port == 5555);
    assert(emus[0].saved_scale == 0);    /* a saved zero, not a missing one */
    assert(emus[1].saved_scale == 5);
    assert(emus[1].saved_port == -1);    /* this one really was not saved */
    assert(saved_pad[PAD_SCREEN] == 1);
    assert(saved_pad[PAD_RES_BOTTOM] == 4);
    assert(saved_pad[PAD_RES_TOP] == 0);
    assert(saved_pad[PAD_FILTER] == 3);
    assert(saved_pad[PAD_SHARP] == 2);
    assert(saved_pad[PAD_RATE] == 16);

    /*
     * And written back without a window. Choosing a path saves, and the
     * interface may not exist yet when it does; reading a widget that is
     * not there must not be how the file loses a value.
     */
    g_strlcpy(emus[2].binary, "/elsewhere/Cemu", sizeof(emus[2].binary));
    save_paths();
    load_paths();
    assert(g_strcmp0(emus[2].binary, "/elsewhere/Cemu") == 0);
    assert(g_strcmp0(emus[0].binary, "/somewhere/melonDS") == 0);

    /*
     * And the widgets, which is where a saved value has to end up.
     *
     * Parsing the file right is half of it: the resolution a person
     * chose -- 3x, say -- has to be showing in the combo when the
     * window opens, and has to be what gets written back. Checking only
     * the parser would pass with the panel ignoring every value it was
     * given.
     */
    assert(g_file_set_contents(path,
        "melonds.port=5601\n"
        "melonds.firmware=/somewhere/firmware.bin\n"
        "melonds.resolution=2\n"       /* 3x: the list starts at 1x */
        "melonds.stream=0\n"
        "azahar.resolution=5\n"        /* 6x */
        "pad.screen=1\n"
        "pad.resolution=4\n"
        "pad.top_resolution=1\n"
        "pad.filter=0\n"
        "pad.sharpness=3\n"
        "pad.bitrate=8\n", -1, NULL));
    load_paths();

    /* The firmware fields are populated from each emulator's own storage. */
    char *melon_dir = g_build_filename(tmp, "melonDS", NULL);
    assert(!g_mkdir_with_parents(melon_dir, 0700));
    char *melon_cfg = g_build_filename(melon_dir, "melonDS.toml", NULL);
    assert(g_file_set_contents(melon_cfg,
        "[DS]\nFirmwarePath = \"/detected/firmware.bin\"\n", -1, NULL));

    char *nand = g_build_filename(tmp, "nand", NULL);
    char *az_content = g_build_filename(nand,
        "00000000000000000000000000000000", "title", "00040030",
        "00009802", "content", NULL);
    assert(!g_mkdir_with_parents(az_content, 0700));
    char *az_app = g_build_filename(az_content, "00000042.app", NULL);
    assert(g_file_set_contents(az_app, "menu", -1, NULL));
    char *az_tmd = g_build_filename(az_content, "00000000.tmd", NULL);
    guint8 tmd_data[0xb34] = {0};
    tmd_data[1] = 1; tmd_data[3] = 4;       /* RSA-2048/SHA-256 */
    tmd_data[0x1dc] = 1; tmd_data[0x1dd] = 1; /* version 257 */
    tmd_data[0xb07] = 0x42;                 /* main content ID */
    assert(g_file_set_contents(az_tmd, (const char *)tmd_data,
                               sizeof(tmd_data), NULL));
    char *az_dir = g_build_filename(tmp, "azahar-emu", NULL);
    assert(!g_mkdir_with_parents(az_dir, 0700));
    char *az_cfg = g_build_filename(az_dir, "qt-config.ini", NULL);
    char *az_text = g_strdup_printf(
        "[Data%%20Storage]\nnand_directory=%s\nsdmc_directory=%s/sd\n", nand, tmp);
    assert(g_file_set_contents(az_cfg, az_text, -1, NULL));

    char *mlc = g_build_filename(tmp, "mlc", NULL);
    char *cemu_meta_dir = g_build_filename(mlc, "sys", "title", "00050010",
                                           "10040200", "meta", NULL);
    assert(!g_mkdir_with_parents(cemu_meta_dir, 0700));
    char *cemu_meta = g_build_filename(cemu_meta_dir, "meta.xml", NULL);
    assert(g_file_set_contents(cemu_meta,
        "<menu><title_version type=\"unsignedInt\">257</title_version></menu>",
        -1, NULL));
    char *cemu_dir = g_build_filename(tmp, "Cemu", NULL);
    assert(!g_mkdir_with_parents(cemu_dir, 0700));
    char *cemu_cfg = g_build_filename(cemu_dir, "settings.xml", NULL);
    char *cemu_text = g_strdup_printf("<content><mlc_path>%s</mlc_path></content>", mlc);
    assert(g_file_set_contents(cemu_cfg, cemu_text, -1, NULL));

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    for (int i = 0; i < EMU_COUNT; i++)
        gtk_box_pack_start(GTK_BOX(box), build_emu_panel(&emus[i]), FALSE, FALSE, 0);

    assert(gtk_spin_button_get_value(GTK_SPIN_BUTTON(emus[0].port)) == 5601);
    assert(gtk_combo_box_get_active(GTK_COMBO_BOX(emus[0].scale)) == 2);
    assert(!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(emus[0].enable)));
    assert(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(emus[1].enable)));
    assert(gtk_combo_box_get_active(GTK_COMBO_BOX(emus[1].scale)) == 5);
    /* Not mentioned in the file, so the default the panel was built with. */
    assert(gtk_combo_box_get_active(GTK_COMBO_BOX(emus[2].scale)) == 0);
    assert(g_strcmp0(selected_firmware(&emus[0]), "/somewhere/firmware.bin") == 0);
    assert(g_strcmp0(selected_firmware(&emus[1]), az_app) == 0);
    assert(g_strcmp0(selected_firmware(&emus[2]), "0005001010040200") == 0);

    /* Both screen encoders feed the Statistics tab independently. */
    emus[2].stats_label = gtk_label_new(NULL);
    emus[2].pid = 123;
    parse_stats(&emus[2],
        "bottom_screen: 854x480 @ 60 fps, h264_vaapi, listening on port 5092 (up to 4 clients)\n");
    parse_stats(&emus[2],
        "bottom_screen: stats screen=top frames=300 fps=59.94 mbps=4.25 clients=2 "
        "source=1280x720 output=854x480 encoder=h264_vaapi\n");
    assert(emus[2].announced_port == 5092);
    assert(emus[2].stats_nominal_fps == 60);
    assert(emus[2].stats_frames[1] == 300);
    assert(emus[2].stats_clients[1] == 2);
    assert(emus[2].stats_source_w[1] == 1280);
    assert(emus[2].stats_output_w[1] == 854);
    assert(emus[2].stats_measured_fps[1] > 59.9);
    emus[2].pid = 0;

    /* Firmware launch means three deliberately different command lines. */
    char *stub = g_build_filename(tmp, "fake-emulator", NULL);
    const char *stub_text =
        "#!/bin/sh\n"
        "printf '%s\\n' \"$@\" > \"$LAUNCH_RECORD_DIR/$BOTTOM_SCREEN_PORT.args\"\n"
        "printf '%s %s\\n' \"$BOTTOM_SCREEN\" \"$BOTTOM_SCREEN_PORT\" > "
        "\"$LAUNCH_RECORD_DIR/$BOTTOM_SCREEN_PORT.env\"\n";
    assert(g_file_set_contents(stub, stub_text, -1, NULL));
    assert(chmod(stub, 0700) == 0);
    g_setenv("LAUNCH_RECORD_DIR", tmp, TRUE);
    for (int i = 0; i < EMU_COUNT; i++)
        g_strlcpy(emus[i].binary, stub, sizeof(emus[i].binary));

    char *firmware = g_build_filename(tmp, "firmware.bin", NULL);
    assert(g_file_set_contents(firmware, "firmware", -1, NULL));
    gtk_entry_set_text(GTK_ENTRY(emus[0].firmware_field), firmware);
    pad_auto_waiting = TRUE;
    launch_emu(&emus[0], TRUE);
    wait_for_emu(&emus[0]);
    assert(pad_owner == &emus[0]);
    assert(!pad_auto_waiting);
    launch_emu(&emus[1], TRUE);
    wait_for_emu(&emus[1]);
    launch_emu(&emus[2], TRUE);
    wait_for_emu(&emus[2]);

    char *args0_path = g_build_filename(tmp, "5601.args", NULL);
    char *args1_path = g_build_filename(tmp, "5091.args", NULL);
    char *args2_path = g_build_filename(tmp, "5092.args", NULL);
    char *env0_path = g_build_filename(tmp, "5601.env", NULL);
    char *env1_path = g_build_filename(tmp, "5091.env", NULL);
    char *env2_path = g_build_filename(tmp, "5092.env", NULL);
    char *args0 = read_required(args0_path), *args1 = read_required(args1_path);
    char *args2 = read_required(args2_path), *env0 = read_required(env0_path);
    char *env1 = read_required(env1_path), *env2 = read_required(env2_path);
    assert(g_strcmp0(args0, "--boot\nalways\n") == 0);
    char *az_expected = g_strdup_printf("%s\n", az_app);
    assert(g_strcmp0(args1, az_expected) == 0);
    assert(g_strcmp0(args2, "--title-id\n0005001010040200\n") == 0);
    assert(g_strcmp0(env0, "1 5601\n") == 0);
    assert(g_strcmp0(env1, "1 5091\n") == 0);
    assert(g_strcmp0(env2, "1 5092\n") == 0);

    /* Changed the way a person changes it, then saved and read back. */
    gtk_combo_box_set_active(GTK_COMBO_BOX(emus[0].scale), 3);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(emus[0].port), 5610);
    save_paths();
    load_paths();
    assert(emus[0].saved_scale == 3);
    assert(emus[0].saved_port == 5610);
    assert(emus[1].saved_scale == 5);   /* untouched, and not lost */

    g_free(az_expected); g_free(args0); g_free(args1); g_free(args2);
    g_free(env0); g_free(env1); g_free(env2); g_free(args0_path);
    g_free(args1_path); g_free(args2_path); g_free(env0_path);
    g_free(env1_path); g_free(env2_path); g_free(firmware); g_free(stub);
    g_free(cemu_text); g_free(cemu_cfg); g_free(cemu_dir); g_free(cemu_meta);
    g_free(cemu_meta_dir); g_free(mlc); g_free(az_text); g_free(az_cfg);
    g_free(az_dir); g_free(az_tmd); g_free(az_app); g_free(az_content); g_free(nand);
    g_free(melon_cfg); g_free(melon_dir);
    g_free(path); g_free(dir); g_free(tmp);
    puts("PASS: settings reach the widgets, survive a save, and a saved zero "
         "is kept");
    return 0;
}
