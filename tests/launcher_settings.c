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

    /* Changed the way a person changes it, then saved and read back. */
    gtk_combo_box_set_active(GTK_COMBO_BOX(emus[0].scale), 3);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(emus[0].port), 5610);
    save_paths();
    load_paths();
    assert(emus[0].saved_scale == 3);
    assert(emus[0].saved_port == 5610);
    assert(emus[1].saved_scale == 5);   /* untouched, and not lost */

    g_free(path); g_free(dir); g_free(tmp);
    puts("PASS: settings reach the widgets, survive a save, and a saved zero "
         "is kept");
    return 0;
}
