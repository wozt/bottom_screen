/* drc_lab - banc d'essai GTK pour le streaming vers un Wii U GamePad.
 *
 * Tout ce que ce decodeur accepte a du etre trouve a l'oeil, un reglage a la
 * fois : il echoue en silence, sans jamais signaler qu'il ne comprend pas ce
 * qu'on lui envoie. Cette fenetre sert a faire ces essais sans retaper une
 * ligne de commande a chaque fois.
 *
 * gcc -O2 -o drc_lab drc_lab.c $(pkg-config --cflags --libs gtk+-3.0)
 */
#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ config */
static const char *IFACE, *GP_MAC, *AP_MAC, *HOSTAP, *LIBDRC, *RUNDIR, *DNSCONF;

typedef struct {
    GtkWidget *win, *log, *status;
    GtkWidget *video;                       /* chooser */
    GtkWidget *btn_start, *btn_stop, *loop;
    GtkWidget *wd_on, *wd_thresh, *wd_patience;
    /* encodeur */
    GtkWidget *qp, *preset, *refresh, *pir, *trellis, *me, *subme, *ref;
    GtkWidget *spread, *i4x4mask, *plane_l, *plane_c, *cintra, *deblock;
    GtkWidget *aq, *no_i4x4, *resync_us, *resync_restart;
    /* reseau */
    GtkWidget *mtu;
    /* appairage */
    GtkWidget *sym[4], *pin_lbl;
    /* etat */
    GPid stream_pgid;
    int wd_bad;
} App;

static App A;

/* ------------------------------------------------------------------ helpers */
static void logf_(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    char *msg = g_strdup_vprintf(fmt, ap); va_end(ap);
    GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(A.log));
    GtkTextIter end; gtk_text_buffer_get_end_iter(b, &end);
    char *stamp = g_date_time_format_iso8601(g_date_time_new_now_local());
    char *line = g_strdup_printf("%.8s  %s\n", stamp + 11, msg);
    gtk_text_buffer_insert(b, &end, line, -1);
    gtk_text_buffer_get_end_iter(b, &end);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(A.log), &end, 0, FALSE, 0, 0);
    g_free(line); g_free(stamp); g_free(msg);
}

/* Lance une commande shell et rend sa sortie (a liberer). */
static char *sh_capture(const char *cmd)
{
    char *out = NULL;
    char *argv[] = { "/bin/sh", "-c", (char *)cmd, NULL };
    if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
                      &out, NULL, NULL, NULL))
        return g_strdup("");
    return out ? out : g_strdup("");
}

/* Lance une commande en tache de fond, journalisee. */
static void sh_async(const char *what, const char *cmd)
{
    logf_("%s", what);
    char *full = g_strdup_printf("(%s) >> %s/lab.log 2>&1 &", cmd, RUNDIR);
    char *argv[] = { "/bin/sh", "-c", full, NULL };
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    g_free(full);
}

static int spin(GtkWidget *w) { return gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w)); }
static gboolean on(GtkWidget *w) { return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w)); }
static char *combo(GtkWidget *w) { return gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(w)); }

/* ------------------------------------------------------------------- flux */
static void stream_stop(void)
{
    /* Par nom exact : le lecteur est lance depuis demos/ en "./drc_player", donc
     * un motif de chemin complet ne le retrouve pas. ffmpeg part de lui-meme en
     * SIGPIPE quand le lecteur ferme le tube, mais on ramasse quand meme ceux
     * qui tiennent encore notre FIFO. */
    char *c = g_strdup_printf(
        "pkill -x drc_player 2>/dev/null; sleep 1;"
        "ps -eo pid,args | grep -F '%s/audio.fifo' | grep -v grep"
        " | awk '{print $1}' | xargs -r kill 2>/dev/null", RUNDIR);
    char *o = sh_capture(c); g_free(o); g_free(c);
    A.stream_pgid = 0;
}

static char *encoder_env(void)
{
    char *pre = combo(A.preset), *m = combo(A.me);
    char *s = g_strdup_printf(
        "DRC_STATS=1 DRC_QP=%d DRC_PRESET=%s DRC_REFRESH=%d DRC_PIR_QP_OFFSET=%d "
        "DRC_TRELLIS=%d DRC_ME=%s DRC_SUBME=%d DRC_REF=%d DRC_TX_SPREAD_US=%d "
        "DRC_I4X4_MASK=0x%x DRC_PLANE_LUMA=%d DRC_PLANE_CHROMA=%d "
        "DRC_CONSTRAINED_INTRA=%d DRC_DEBLOCK=%d DRC_AQ=%d DRC_RESYNC_US=%d "
        "DRC_RESYNC_RESTART=%d %s",
        spin(A.qp), pre, spin(A.refresh), spin(A.pir), spin(A.trellis), m,
        spin(A.subme), spin(A.ref), spin(A.spread), spin(A.i4x4mask),
        on(A.plane_l), on(A.plane_c), on(A.cintra), on(A.deblock), on(A.aq),
        spin(A.resync_us), on(A.resync_restart),
        on(A.no_i4x4) ? "DRC_NO_I4X4=1" : "");
    g_free(pre); g_free(m);
    return s;
}

static void stream_start(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    char *file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(A.video));
    if (!file) { logf_("aucune video selectionnee"); return; }
    stream_stop();

    char *env = encoder_env();
    char *cmd = g_strdup_printf(
        "mkdir -p %s; rm -f %s/audio.fifo; mkfifo %s/audio.fifo;"
        "cd %s/demos && env LD_LIBRARY_PATH=%s:%s/../libdrc-prefix/lib %s"
        " DRC_AUDIO_FIFO=%s/audio.fifo"
        " sh -c 'ffmpeg -re %s -i \"%s\" -map 0:v:0"
        " -vf scale=864:480 -r 60 -pix_fmt rgba -f rawvideo pipe:1"
        " -map 0:a:0 -f s16le -ar 48000 -ac 2 -y %s/audio.fifo 2>%s/ffmpeg.log"
        " | ./drc_player' > %s/player.log 2>&1",
        RUNDIR, RUNDIR, RUNDIR, LIBDRC, LIBDRC, LIBDRC, env,
        RUNDIR, on(A.loop) ? "-stream_loop -1" : "", file,
        RUNDIR, RUNDIR, RUNDIR);
    sh_async("flux demarre", cmd);
    logf_("  %s", env);
    g_free(cmd); g_free(env); g_free(file);
}

static void stream_stop_cb(GtkWidget *w, gpointer d)
{ (void)w; (void)d; stream_stop(); logf_("flux arrete"); }

/* --------------------------------------------------------------- reseau/AP */
static void do_deauth(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    char *c = g_strdup_printf("sudo %s/hostapd/hostapd_cli -p /var/run/hostapd"
                              " -i %s deauthenticate %s", HOSTAP, IFACE, GP_MAC);
    char *o = sh_capture(c); g_free(o); g_free(c);
    logf_("deauth envoye a %s", GP_MAC);
}

static void do_mtu(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    char *c = g_strdup_printf("sudo ip link set mtu %d dev %s", spin(A.mtu), IFACE);
    char *o = sh_capture(c); g_free(o); g_free(c);
    logf_("MTU -> %d", spin(A.mtu));
}

static void do_ap_normal(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    char *c = g_strdup_printf(
        "sudo sh -c 'pkill hostapd; pkill dnsmasq; sleep 2;"
        "nmcli device set %s managed no 2>/dev/null;"
        "ip link set %s down; sleep 1; iw dev %s set type managed 2>/dev/null;"
        "ip link set %s address %s; ip link set %s up; sleep 2;"
        "%s/hostapd/hostapd -dd %s/conf/local_normal2.conf > %s/ap.log 2>&1 &"
        "sleep 5; ip addr flush dev %s; ip addr add 192.168.1.10/24 dev %s;"
        "ip link set mtu %d dev %s;"
        "[ -n \"%s\" ] && /usr/sbin/dnsmasq -d -C %s > %s/dns.log 2>&1 &'",
        IFACE, IFACE, IFACE, IFACE, AP_MAC, IFACE, HOSTAP, HOSTAP, RUNDIR,
        IFACE, IFACE, spin(A.mtu), IFACE, DNSCONF, DNSCONF, RUNDIR);
    sh_async("AP normal redemarre", c);
    g_free(c);
}

static void do_ap_pair(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    int pin = 0;
    for (int i = 0; i < 4; i++)
        pin = pin * 10 + gtk_combo_box_get_active(GTK_COMBO_BOX(A.sym[i]));
    char *c = g_strdup_printf(
        "sudo env DRC_IF=%s DRC_AP_MAC=%s DRC_HOSTAP=%s DRC_RUN=%s"
        " DRC_MTU=%d DRC_DNSMASQ_CONF=%s %s/../tools/ap-pair.sh %04d5678",
        IFACE, AP_MAC, HOSTAP, RUNDIR, spin(A.mtu), DNSCONF, LIBDRC, pin);
    char *what = g_strdup_printf("appairage lance, PIN %04d5678", pin);
    sh_async(what, c);
    g_free(what); g_free(c);
}

static void pin_changed(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    static const char *sy[4] = { "♠", "♥", "♦", "♣" };
    int pin = 0; GString *s = g_string_new("");
    for (int i = 0; i < 4; i++) {
        int v = gtk_combo_box_get_active(GTK_COMBO_BOX(A.sym[i]));
        if (v < 0) v = 0;
        pin = pin * 10 + v;
        g_string_append(s, sy[v]);
    }
    char *t = g_strdup_printf("<big>%s</big>   PIN : <b>%04d5678</b>", s->str, pin);
    gtk_label_set_markup(GTK_LABEL(A.pin_lbl), t);
    g_free(t); g_string_free(s, TRUE);
}

/* ------------------------------------------------------------- telemetrie */
static gboolean tick(gpointer d)
{
    (void)d;
    static long pb = 0, pp = 0;

    char *c = g_strdup_printf(
        "grep -a '^\\[drc\\]' %s/player.log 2>/dev/null | tail -1", RUNDIR);
    char *last = sh_capture(c); g_free(c);

    int fps = 0, resync = -1;
    char *p = strstr(last, "frames/s");
    if (p) { while (p > last && *(p-1) != ' ') p--; fps = atoi(p); }
    p = strstr(last, "resync");
    if (p) { char *q = p; while (q > last && *(q-1) != ' ') q--; resync = atoi(q); }

    char *sb = g_strdup_printf("cat /sys/class/net/%s/statistics/tx_bytes", IFACE);
    char *sp = g_strdup_printf("cat /sys/class/net/%s/statistics/tx_packets", IFACE);
    char *a = sh_capture(sb), *b = sh_capture(sp);
    long nb = atol(a), np = atol(b);
    double mbps = pb ? (nb - pb) * 8.0 / 1e6 : 0;
    long pps = pp ? np - pp : 0;
    pb = nb; pp = np;
    g_free(a); g_free(b); g_free(sb); g_free(sp);

    c = g_strdup_printf("sudo %s/hostapd/hostapd_cli -p /var/run/hostapd -i %s"
                        " all_sta 2>/dev/null | grep -c AUTHORIZED", HOSTAP, IFACE);
    char *auth = sh_capture(c); g_free(c);
    c = g_strdup_printf("cat /sys/class/net/%s/mtu", IFACE);
    char *mtu = sh_capture(c); g_free(c);
    c = g_strdup_printf("sudo iw dev %s station dump 2>/dev/null"
                        " | grep -oP 'signal:\\s*\\K-[0-9]+' | head -1", IFACE);
    char *sig = sh_capture(c); g_free(c);

    char rs[16];
    if (resync < 0) g_strlcpy(rs, "-", sizeof rs);
    else g_snprintf(rs, sizeof rs, "%d", resync);
    char *t = g_strdup_printf(
        "GamePad %s   signal %s dBm   MTU %s   %d fps   %ld pkt/s   %.2f Mbit/s   resync %s",
        atoi(auth) ? "connectee" : "ABSENTE",
        g_strstrip(sig), g_strstrip(mtu), fps, pps, mbps, rs);
    gtk_label_set_text(GTK_LABEL(A.status), t);
    g_free(t); g_free(auth); g_free(mtu); g_free(sig);

    /* Chien de garde : la GamePad peut perdre la session, et aucune image cle
     * ne l'en sort - seule une reassociation le fait. */
    if (on(A.wd_on) && resync >= spin(A.wd_thresh)) {
        if (++A.wd_bad >= spin(A.wd_patience)) {
            logf_("resync bloque a %d/s -> reassociation", resync);
            do_deauth(NULL, NULL);
            A.wd_bad = 0;
        }
    } else A.wd_bad = 0;

    g_free(last);
    return TRUE;
}

/* ------------------------------------------------------------------- UI */
static GtkWidget *row(GtkWidget *grid, int r, const char *label, GtkWidget *w)
{
    GtkWidget *l = gtk_label_new(label);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), l, 0, r, 1, 1);
    gtk_widget_set_hexpand(w, TRUE);
    gtk_grid_attach(GTK_GRID(grid), w, 1, r, 1, 1);
    return w;
}

static GtkWidget *sp(int lo, int hi, int val)
{
    GtkWidget *w = gtk_spin_button_new_with_range(lo, hi, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w), val);
    return w;
}

static GtkWidget *chk(const char *l, gboolean v)
{
    GtkWidget *w = gtk_check_button_new_with_label(l);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), v);
    return w;
}

static GtkWidget *new_grid(void)
{
    GtkWidget *g = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(g), 6);
    gtk_grid_set_column_spacing(GTK_GRID(g), 12);
    gtk_container_set_border_width(GTK_CONTAINER(g), 12);
    return g;
}

static void build(void)
{
    A.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(A.win), "Banc d'essai Wii U GamePad");
    gtk_window_set_default_size(GTK_WINDOW(A.win), 760, 620);
    g_signal_connect(A.win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(A.win), box);
    GtkWidget *nb = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(box), nb, TRUE, TRUE, 0);

    /* --- Flux --- */
    GtkWidget *g = new_grid(); int r = 0;
    A.video = gtk_file_chooser_button_new("Video", GTK_FILE_CHOOSER_ACTION_OPEN);
    row(g, r++, "Video", A.video);
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    A.btn_start = gtk_button_new_with_label("Demarrer");
    A.btn_stop  = gtk_button_new_with_label("Arreter");
    GtkWidget *bre = gtk_button_new_with_label("Relancer + deauth");
    gtk_box_pack_start(GTK_BOX(hb), A.btn_start, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hb), A.btn_stop, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hb), bre, TRUE, TRUE, 0);
    row(g, r++, "", hb);
    A.loop = chk("Rejouer la video en boucle", TRUE);
    row(g, r++, "Lecture", A.loop);
    A.wd_on = chk("Reassocier automatiquement si la GamePad ne decode plus", TRUE);
    row(g, r++, "Chien de garde", A.wd_on);
    A.wd_thresh = sp(1, 60, 30);   row(g, r++, "  seuil resync/s", A.wd_thresh);
    A.wd_patience = sp(1, 30, 4);  row(g, r++, "  patience (s)", A.wd_patience);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), g, gtk_label_new("Flux"));

    /* --- Encodeur --- */
    g = new_grid(); r = 0;
    A.qp = sp(1, 51, 32);          row(g, r++, "QP (32 = defaut DRH)", A.qp);
    A.preset = gtk_combo_box_text_new();
    const char *pr[] = { "medium", "fast", "faster", "veryfast", "slow", NULL };
    for (int i = 0; pr[i]; i++) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(A.preset), pr[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(A.preset), 0);
    row(g, r++, "Preset (au-dela de medium : casse)", A.preset);
    A.refresh = sp(10, 600, 60);   row(g, r++, "Periode de balayage (images)", A.refresh);
    A.pir = sp(0, 12, 8);          row(g, r++, "QP en moins sur la vague", A.pir);
    A.subme = sp(0, 11, 7);        row(g, r++, "subme (>=8 : casse)", A.subme);
    A.trellis = sp(0, 2, 1);       row(g, r++, "trellis", A.trellis);
    A.me = gtk_combo_box_text_new();
    const char *mm[] = { "hex", "umh", "esa", NULL };
    for (int i = 0; mm[i]; i++) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(A.me), mm[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(A.me), 0);
    row(g, r++, "Recherche de mouvement", A.me);
    A.ref = sp(1, 5, 1);           row(g, r++, "Images de reference (>1 : risque)", A.ref);
    A.spread = sp(0, 16000, 11000); row(g, r++, "Etalement des paquets (us)", A.spread);
    A.i4x4mask = sp(0, 511, 511);  row(g, r++, "Masque des modes intra 4x4", A.i4x4mask);
    A.no_i4x4 = chk("Desactiver completement l'intra 4x4", FALSE);
    row(g, r++, "", A.no_i4x4);
    A.plane_l = chk("Prediction planaire luma (casse : artefacts)", FALSE);
    row(g, r++, "", A.plane_l);
    A.plane_c = chk("Prediction planaire chroma (casse : vert)", FALSE);
    row(g, r++, "", A.plane_c);
    A.cintra = chk("constrained_intra_pred (doit rester actif)", TRUE);
    row(g, r++, "", A.cintra);
    A.deblock = chk("Filtre de deblocage", TRUE);
    row(g, r++, "", A.deblock);
    A.aq = chk("Quantification adaptative", FALSE);
    row(g, r++, "", A.aq);
    A.resync_restart = chk("Redemarrer l'encodeur sur demande de resync", TRUE);
    row(g, r++, "", A.resync_restart);
    A.resync_us = sp(0, 5000000, 500000); row(g, r++, "  au plus une fois par (us)", A.resync_us);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), g, gtk_label_new("Encodeur"));

    /* --- Reseau --- */
    g = new_grid(); r = 0;
    A.mtu = sp(576, 2304, 1800);
    GtkWidget *mb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(mb), A.mtu, TRUE, TRUE, 0);
    GtkWidget *bm = gtk_button_new_with_label("Appliquer");
    gtk_box_pack_start(GTK_BOX(mb), bm, FALSE, FALSE, 0);
    row(g, r++, "MTU", mb);
    GtkWidget *bd = gtk_button_new_with_label("Deauthentifier la GamePad");
    row(g, r++, "", bd);
    GtkWidget *bn = gtk_button_new_with_label("Redemarrer l'AP (mode normal)");
    row(g, r++, "", bn);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), g, gtk_label_new("Reseau"));

    /* --- Appairage --- */
    g = new_grid(); r = 0;
    GtkWidget *sb2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    const char *sy[] = { "♠ pique (0)", "♥ coeur (1)",
                         "♦ carreau (2)", "♣ trefle (3)", NULL };
    for (int i = 0; i < 4; i++) {
        A.sym[i] = gtk_combo_box_text_new();
        for (int j = 0; sy[j]; j++)
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(A.sym[i]), sy[j]);
        gtk_combo_box_set_active(GTK_COMBO_BOX(A.sym[i]), 0);
        g_signal_connect(A.sym[i], "changed", G_CALLBACK(pin_changed), NULL);
        gtk_box_pack_start(GTK_BOX(sb2), A.sym[i], TRUE, TRUE, 0);
    }
    row(g, r++, "Symboles affiches", sb2);
    A.pin_lbl = gtk_label_new("");
    gtk_widget_set_halign(A.pin_lbl, GTK_ALIGN_START);
    row(g, r++, "", A.pin_lbl);
    GtkWidget *bp = gtk_button_new_with_label("Lancer un appairage neuf");
    row(g, r++, "", bp);
    GtkWidget *note = gtk_label_new(
        "Les quatre symboles sont ceux que la console afficherait ; la seconde\n"
        "moitie du code est toujours 5678. L'AP bascule en mode normal des que\n"
        "le message M8 part : la GamePad n'attend que ~4 s.");
    gtk_widget_set_halign(note, GTK_ALIGN_START);
    row(g, r++, "", note);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), g, gtk_label_new("Appairage"));

    /* --- Journal --- */
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    A.log = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(A.log), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(A.log), TRUE);
    gtk_container_add(GTK_CONTAINER(sw), A.log);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), sw, gtk_label_new("Journal"));

    /* --- barre d'etat --- */
    A.status = gtk_label_new("...");
    gtk_widget_set_halign(A.status, GTK_ALIGN_START);
    gtk_widget_set_margin_start(A.status, 10);
    gtk_widget_set_margin_top(A.status, 4);
    gtk_widget_set_margin_bottom(A.status, 6);
    gtk_box_pack_start(GTK_BOX(box), A.status, FALSE, FALSE, 0);

    g_signal_connect(A.btn_start, "clicked", G_CALLBACK(stream_start), NULL);
    g_signal_connect(A.btn_stop, "clicked", G_CALLBACK(stream_stop_cb), NULL);
    g_signal_connect(bre, "clicked", G_CALLBACK(stream_start), NULL);
    g_signal_connect(bre, "clicked", G_CALLBACK(do_deauth), NULL);
    g_signal_connect(bm, "clicked", G_CALLBACK(do_mtu), NULL);
    g_signal_connect(bd, "clicked", G_CALLBACK(do_deauth), NULL);
    g_signal_connect(bn, "clicked", G_CALLBACK(do_ap_normal), NULL);
    g_signal_connect(bp, "clicked", G_CALLBACK(do_ap_pair), NULL);

    pin_changed(NULL, NULL);
    gtk_widget_show_all(A.win);
}

int main(int argc, char **argv)
{
    IFACE   = g_getenv("DRC_IF")      ?: "wlxe0ad474070d8";
    GP_MAC  = g_getenv("DRC_GP_MAC")  ?: "34:af:2c:a3:44:ce";
    AP_MAC  = g_getenv("DRC_AP_MAC")  ?: "34:af:2c:be:ef:01";
    HOSTAP  = g_getenv("DRC_HOSTAP")  ?: "/home/wozt/rtw88_TSF/drc-hostap";
    LIBDRC  = g_getenv("DRC_LIBDRC")  ?: "/home/wozt/rtw88_TSF/libdrc";
    DNSCONF = g_getenv("DRC_DNSMASQ_CONF") ?: "";
    RUNDIR  = g_build_filename(g_get_user_cache_dir(), "drc-lab", NULL);
    g_mkdir_with_parents(RUNDIR, 0755);

    gtk_init(&argc, &argv);
    build();
    logf_("interface %s, GamePad %s", IFACE, GP_MAC);
    logf_("journaux dans %s", RUNDIR);
    g_timeout_add_seconds(1, tick, NULL);
    gtk_main();
    return 0;
}
