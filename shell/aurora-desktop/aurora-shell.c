/*
 * aurora-shell — DaybreakOS's own native desktop shell.
 *
 * A GTK3 + gtk-layer-shell program that draws the entire visible desktop as
 * Wayland layer-shell surfaces over a wlroots compositor (labwc):
 *   - wallpaper (background layer, aurora-horizon gradient)
 *   - top bar   (top layer: logo, clock, indicators, launcher + Aura buttons)
 *   - dock      (top layer, bottom-anchored: pinned + running apps)
 *   - launcher  (overlay popup: grid of installed .desktop apps)
 *   - Aura      (overlay slide-over: prompt -> aurorad /ask -> reply)
 *
 * No browser, no toolkit desktop — this is Aurora's UI, written for Aurora.
 * Build: see scripts/13-aurora-desktop.sh (gcc `pkg-config --cflags/--libs
 * gtk+-3.0 gtk-layer-shell-0`).
 */
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <gdk/gdkwayland.h>
#include <gtk-layer-shell.h>
#include <wayland-client.h>
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/vfs.h>

/* ----- app model ----- */
typedef struct { char *name, *exec, *icon; } App;
static GList *g_apps = NULL;             /* App*  */
static GtkWidget *g_launcher = NULL;     /* launcher window (toggle) */
static GtkWidget *g_launcher_search = NULL; /* launcher search entry */
static GtkWidget *g_launcher_flow = NULL;   /* launcher app grid */
static GtkWidget *g_aura = NULL;         /* aura window (toggle) */
static GtkWidget *g_aura_log = NULL;     /* aura message list box */
static GtkWidget *g_clock = NULL;

/* ----- Daybreak Store model ----- */
typedef struct { char *id, *name, *category, *icon, *desc; gboolean installed; } StoreApp;
static GList *g_catalog = NULL;          /* StoreApp* */
static GtkWidget *g_store = NULL;        /* store window (toggle) */
static GtkWidget *g_store_list = NULL;   /* store card list box */
static void on_store_btn(GtkButton *b, gpointer u); /* defined with the Store, used by the top bar */

/* ----- desktop chrome: control center, notifications, widgets, overlays ----- */
static GtkWidget *g_cc = NULL;           /* control center flyout */
static GtkWidget *g_noti = NULL;         /* notifications + calendar flyout */
static GtkWidget *g_noti_list = NULL;    /* notifications list box */
static GtkWidget *g_widgets = NULL;      /* widgets flyout */
static GtkWidget *g_nightlight = NULL;   /* warm overlay surface (night light) */
static GtkWidget *g_toast = NULL;        /* transient toast surface */
static GtkWidget *g_toast_lbl = NULL;
static GtkWidget *g_lock = NULL;         /* lock-screen overlay */
static GtkWidget *g_lock_time = NULL, *g_lock_date = NULL;
static GtkWidget *g_bat_lbl = NULL, *g_net_lbl = NULL; /* live top-bar indicators */
static GtkWidget *g_wstats = NULL;       /* widgets: system-stats label */
static GtkWidget *g_wday = NULL;         /* widgets: Aura "your day" label */
static gboolean g_night_on = FALSE;
static GtkWidget *g_wallpaper = NULL;    /* background surface (right-click menu) */
static int g_aurora_phase = 0;           /* animated aurora band clock */
static GtkWidget *g_auroramenu = NULL;   /* top-bar logo menu (About / power) */

/* ----- taskbar: running windows via wlr-foreign-toplevel-management ----- */
static struct zwlr_foreign_toplevel_manager_v1 *g_ftl_mgr = NULL;
static struct wl_seat *g_ftl_seat = NULL;
static GtkWidget *g_taskbar = NULL;      /* dock box: one button per running window */
static GtkWidget *g_docksep = NULL;      /* pin/taskbar divider — only when windows exist */
static void dock_sep_sync(void) {
    if (!g_docksep || !g_taskbar) return;
    gboolean any = FALSE;
    GList *kids = gtk_container_get_children(GTK_CONTAINER(g_taskbar));
    for (GList *l = kids; l; l = l->next)
        if (gtk_widget_get_visible(GTK_WIDGET(l->data))) { any = TRUE; break; }
    g_list_free(kids);
    gtk_widget_set_visible(g_docksep, any);
}
typedef struct {
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    char *title, *app_id;
    gboolean activated, ico_done;
    GtkWidget *btn, *lbl, *hbox;
} Toplevel;
static GList *g_toplevels = NULL;        /* Toplevel* */

/* ----- pinned dock apps (persisted to ~/.config/aurora/pins) ----- */
typedef struct { char *name, *exec, *icon; } Pin;
static GList *g_pins = NULL;             /* Pin* */
static GtkWidget *g_pinbox = NULL;       /* dock box holding pinned launchers */
static GtkWidget *g_splash = NULL;       /* boot splash overlay (auto-dismisses) */
static void on_cc_btn(GtkButton *b, gpointer u);   /* forward: top bar opens control center */
static void on_widgets_btn(GtkButton *b, gpointer u);
static void on_noti_btn(GtkButton *b, gpointer u);
static void on_logo_clicked(GtkButton *b, gpointer u); /* logo opens the Daybreak menu */
static void aurora_toast(const char *glyph, const char *text);

/* Fixed fallback height for full-cover surfaces (wallpaper, Aura slide-over).
 *
 * These surfaces anchor to opposite edges so the compositor stretches them to
 * the real output size — we never query the monitor ourselves. Querying the
 * monitor geometry right after gtk_init() races the Wayland output sync and can
 * return uninitialised garbage; feeding that into a size request yields a
 * multi-hundred-thousand-pixel window that trips GTK's 65535px limit and aborts
 * in cairo. A generous constant fallback (covers 4K) is only used for the brief
 * moment before the compositor's first configure arrives, then it's overridden
 * by the real output size via the opposite-edge anchors. */
#define AURORA_COVER_H 2160

/* ----- resolution scaling ------------------------------------------------
 * The shell was designed at 1280x800. Rather than hardcode a second set of
 * sizes for 1080p, compute a scale factor from the monitor geometry at
 * startup and apply it to (a) widget layout sizes via S(), (b) the px font
 * sizes in style.css via a generated override provider, and (c) the GTK
 * default (pt-based) font via gtk-xft-dpi. Any resolution renders
 * proportionally — 800p is 1.0, 1080p ≈ 1.35, 4K ≈ 2.7 (capped). */
static double g_k = 1.0;
static int S(int v) { return (int)(v * g_k + 0.5); }
static void scale_init(void) {
    GdkDisplay *d = gdk_display_get_default();
    GdkMonitor *m = gdk_display_get_primary_monitor(d);
    if (!m) m = gdk_display_get_monitor(d, 0);
    if (!m) return;
    GdkRectangle geo; gdk_monitor_get_geometry(m, &geo);
    double k = MIN(geo.width / 1280.0, geo.height / 800.0);
    g_k = CLAMP(k, 1.0, 2.5);
    if (g_k < 1.05) return;   /* design size — nothing to override */

    /* pt-based (unstyled) text follows xft-dpi */
    g_object_set(gtk_settings_get_default(),
                 "gtk-xft-dpi", (int)(96 * 1024 * g_k), NULL);

    /* px font sizes from style.css, re-emitted scaled. Same selectors; this
     * provider is added at a higher priority so it wins the tie. */
    GString *c = g_string_new("");
    struct { const char *sel; int px; } f[] = {
        {".wp-brand", 120}, {"#logo", 14}, {"#logo .mark", 17},
        {"menu menuitem", 13}, {"#clock", 14}, {".tbtn", 13}, {".tray", 13},
        {".taskbtn", 13}, {"#splash-name", 30}, {"#splash-sub", 14},
        {"#launcher .title", 18}, {"#launcher-search", 14},
        {".applaunch .aname", 12}, {".applaunch .aicon", 40},
        {"#aura .head", 15}, {".orb", 20},
        {"#store .title", 18}, {".sicon", 30}, {".sname", 15}, {".sdesc", 12},
        {".abt-name", 26}, {".abt-ver", 13}, {".abt-desc", 13}, {".abt-link", 12},
        {".eyebrow", 11}, {".pclose", 14},
        {".qt", 13}, {".qt .qic", 16}, {".sgi", 16}, {".who", 13},
        {"#noti .title", 17}, {".nclear", 12}, {".nsrc", 11},
        {".ntitle", 13}, {".nbody", 12}, {".nempty", 12},
        {".cal-m", 12}, {".cal-d", 34}, {".cal-w", 12}, {".wh", 11},
        {"#toast", 13},
        {".lock-time", 118}, {".lock-date", 21}, {".lock-hint", 13},
        {"#instwin radiobutton", 13}, {"#instcard radiobutton", 13},
        {"#instcard button", 13}, {"#instwin button", 13},
        {".inst-warn", 12}, {".inst-title", 30}, {".inst-sect", 12},
        {".inst-logo", 34},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(f); i++)
        g_string_append_printf(c, "%s { font-size: %dpx; }\n",
                               f[i].sel, S(f[i].px));
    /* icon buttons need their touch targets scaled with the glyphs */
    g_string_append_printf(c,
        ".dbtn { font-size: %dpx; min-width: %dpx; min-height: %dpx; }\n"
        ".taskbtn { min-height: %dpx; }\n",
        S(30), S(52), S(52), S(40));

    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_data(p, c->str, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    g_string_free(c, TRUE);
}

/* strip .desktop Exec field codes (%U %F %f %u %i %c %k ...) */
static char *clean_exec(const char *raw) {
    GString *s = g_string_new("");
    for (const char *p = raw; *p; p++) {
        if (*p == '%' && p[1]) { p++; continue; }
        g_string_append_c(s, *p);
    }
    return g_string_free(s, FALSE);
}

static void free_apps(void) {
    for (GList *l = g_apps; l; l = l->next) {
        App *a = l->data;
        g_free(a->name); g_free(a->exec); g_free(a->icon); g_free(a);
    }
    g_list_free(g_apps);
    g_apps = NULL;
}

static void scan_apps(void) {
    free_apps();   /* re-runnable: the launcher rescans on open to catch new installs */
    const char *dirs[] = {
        "/usr/share/applications",
        "/usr/local/share/applications",
        NULL
    };
    char *home = g_build_filename(g_get_home_dir(), ".local/share/applications", NULL);
    GPtrArray *all = g_ptr_array_new();
    for (int i = 0; dirs[i]; i++) g_ptr_array_add(all, g_strdup(dirs[i]));
    g_ptr_array_add(all, home);

    for (guint i = 0; i < all->len; i++) {
        const char *dir = all->pdata[i];
        GDir *d = g_dir_open(dir, 0, NULL);
        if (!d) continue;
        const char *fn;
        while ((fn = g_dir_read_name(d))) {
            if (!g_str_has_suffix(fn, ".desktop")) continue;
            char *path = g_build_filename(dir, fn, NULL);
            GKeyFile *kf = g_key_file_new();
            if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
                char *type   = g_key_file_get_string(kf, "Desktop Entry", "Type", NULL);
                char *nodisp = g_key_file_get_string(kf, "Desktop Entry", "NoDisplay", NULL);
                char *name   = g_key_file_get_string(kf, "Desktop Entry", "Name", NULL);
                char *exec   = g_key_file_get_string(kf, "Desktop Entry", "Exec", NULL);
                char *icon   = g_key_file_get_string(kf, "Desktop Entry", "Icon", NULL);
                gboolean ok = name && exec && (!type || !strcmp(type, "Application"))
                              && (!nodisp || strcmp(nodisp, "true"));
                if (ok) {
                    App *a = g_new0(App, 1);
                    a->name = g_strdup(name);
                    a->exec = clean_exec(exec);
                    a->icon = icon ? g_strdup(icon) : NULL;
                    g_apps = g_list_append(g_apps, a);
                }
                g_free(type); g_free(nodisp); g_free(name); g_free(exec); g_free(icon);
            }
            g_key_file_free(kf);
            g_free(path);
        }
        g_dir_close(d);
    }
    g_ptr_array_free(all, TRUE);
}

static void launch(const char *cmd) {
    if (!cmd || !*cmd) return;
    char **argv = NULL;
    if (g_shell_parse_argv(cmd, NULL, &argv, NULL)) {
        g_spawn_async(NULL, argv, NULL,
                      G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                      G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL);
        g_strfreev(argv);
    }
}

/* Resolve a .desktop Icon= into a widget: an absolute path loads the image file
 * (PNG via gdk-pixbuf core, SVG via librsvg's loader); a bare name goes through
 * the icon theme; anything missing falls back to a geometric glyph. This keeps
 * the shell icon-font-free while showing real app art whenever it exists. */
static GtkWidget *icon_widget(const char *icon, int size, const char *glyph) {
    if (icon && *icon) {
        if (g_path_is_absolute(icon)) {
            if (g_file_test(icon, G_FILE_TEST_EXISTS)) {
                GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_size(icon, size, size, NULL);
                if (pb) { GtkWidget *img = gtk_image_new_from_pixbuf(pb); g_object_unref(pb); return img; }
            }
        } else {
            GtkIconTheme *th = gtk_icon_theme_get_default();
            if (gtk_icon_theme_has_icon(th, icon)) {
                GtkWidget *img = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_DIALOG);
                gtk_image_set_pixel_size(GTK_IMAGE(img), size);
                return img;
            }
        }
    }
    /* themed generic before the ASCII glyph — Adwaita ships with the OS now */
    GtkIconTheme *fth = gtk_icon_theme_get_default();
    if (gtk_icon_theme_has_icon(fth, "application-x-executable")) {
        GtkWidget *img = gtk_image_new_from_icon_name("application-x-executable",
                                                      GTK_ICON_SIZE_DIALOG);
        gtk_image_set_pixel_size(GTK_IMAGE(img), size);
        return img;
    }
    GtkWidget *lab = gtk_label_new(glyph ? glyph : "◈");
    gtk_style_context_add_class(gtk_widget_get_style_context(lab), "aicon");
    return lab;
}

/* ----- layer-shell window helper -----
 * `exclusive` is ALWAYS applied, including -1. A value of -1 opts the surface
 * out of the compositor's exclusive-zone arrangement entirely; leaving it at
 * gtk-layer-shell's default (0) makes labwc try to reserve/arrange space, and
 * when a tall full-cover surface is also present that arrangement produces a
 * bad configure that aborts GTK. Every Aurora surface passes -1 for v1. */
static GtkWidget *layer_window(GtkLayerShellLayer layer, gboolean top,
                               gboolean bottom, gboolean left, gboolean right,
                               int exclusive) {
    GtkWidget *w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_layer_init_for_window(GTK_WINDOW(w));
    gtk_layer_set_layer(GTK_WINDOW(w), layer);
    gtk_layer_set_anchor(GTK_WINDOW(w), GTK_LAYER_SHELL_EDGE_TOP, top);
    gtk_layer_set_anchor(GTK_WINDOW(w), GTK_LAYER_SHELL_EDGE_BOTTOM, bottom);
    gtk_layer_set_anchor(GTK_WINDOW(w), GTK_LAYER_SHELL_EDGE_LEFT, left);
    gtk_layer_set_anchor(GTK_WINDOW(w), GTK_LAYER_SHELL_EDGE_RIGHT, right);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(w), exclusive);
    return w;
}

/* ----- aurorad HTTP bridge (127.0.0.1:7212) -----
 * One tiny blocking HTTP/1.0 client used by both Aura chat and the Store.
 * Returns the malloc'd response body (JSON, headers stripped), or NULL if the
 * connection failed (aurorad not up yet). Caller frees. */
static char *aurorad_send(const char *method, const char *path, const char *body) {
    int port = 7212;
    const char *env = g_getenv("AURORAD_PORT");
    if (env) port = atoi(env);
    /* privileged endpoints (disk install, persistence, model download) are
     * served by the root aurorad-system service on its own port */
    if (g_str_has_prefix(path, "/system/")) {
        port = 7213;
        env = g_getenv("AURORAD_SYSTEM_PORT");
        if (env) port = atoi(env);
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { close(fd); return NULL; }

    char *req = body
        ? g_strdup_printf("%s %s HTTP/1.0\r\nHost: 127.0.0.1\r\n"
              "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
              method, path, strlen(body), body)
        : g_strdup_printf("%s %s HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n", method, path);
    ssize_t off = 0, len = strlen(req);
    while (off < len) {
        ssize_t n = write(fd, req + off, len - off);
        if (n <= 0) break;
        off += n;
    }
    g_free(req);

    GString *resp = g_string_new("");
    char buf[4096]; ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) g_string_append_len(resp, buf, n);
    close(fd);

    char *sep = strstr(resp->str, "\r\n\r\n");
    char *out = g_strdup(sep ? sep + 4 : resp->str);
    g_string_free(resp, TRUE);
    return out;
}

/* ----- Aura: POST /ask to aurorad, return reply text ----- */
static char *aura_ask(const char *q) {
    char *jq = g_strescape(q, "");           /* escape \, ", control chars */
    char *body = g_strdup_printf("{\"q\":\"%s\"}", jq);
    g_free(jq);
    char *json = aurorad_send("POST", "/ask", body);
    g_free(body);
    if (!json) return g_strdup("(Aura is still waking up…)");

    /* pull "reply"/"answer" out of the JSON. aurorad's /ask returns
     * {"a": "<reply>", "actions": [...]}, so "a" is the primary key; the others
     * are accepted for forward-compat with other bridges. */
    char *reply = NULL;
    const char *keys[] = {"\"a\"", "\"reply\"", "\"answer\"", "\"text\"", NULL};
    for (int k = 0; keys[k] && !reply; k++) {
        char *p = strstr(json, keys[k]);
        if (!p) continue;
        p = strchr(p, ':'); if (!p) continue; p++;
        while (*p == ' ') p++;
        if (*p != '"') continue;
        p++;
        GString *out = g_string_new("");
        for (; *p && *p != '"'; p++) {
            if (*p == '\\' && p[1]) { p++;
                if (*p == 'n') g_string_append_c(out, '\n');
                else g_string_append_c(out, *p);
            } else g_string_append_c(out, *p);
        }
        reply = g_string_free(out, FALSE);
    }
    if (!reply) reply = g_strndup(json, 400);
    g_free(json);
    return reply;
}

static GtkWidget *aura_add_msg(const char *text, gboolean user) {
    GtkWidget *row = gtk_label_new(text);
    gtk_label_set_line_wrap(GTK_LABEL(row), TRUE);
    gtk_label_set_xalign(GTK_LABEL(row), user ? 1.0 : 0.0);
    gtk_widget_set_halign(row, user ? GTK_ALIGN_END : GTK_ALIGN_START);
    GtkStyleContext *sc = gtk_widget_get_style_context(row);
    gtk_style_context_add_class(sc, user ? "msg-u" : "msg-a");
    gtk_widget_set_margin_top(row, 4);
    gtk_widget_set_margin_bottom(row, 4);
    gtk_box_pack_start(GTK_BOX(g_aura_log), row, FALSE, FALSE, 0);
    gtk_widget_show_all(row);
    return row;
}

/* Aura runs the LLM request on a worker thread so a slow on-device model never
 * freezes the desktop. The worker builds a result and hands it back to the GTK
 * main thread via g_idle_add (all widget access stays on the main thread). */
typedef struct { char *q; GtkWidget *bubble; GtkWidget *entry; } AuraJob;
typedef struct { char *reply; GtkWidget *bubble; GtkWidget *entry; } AuraResult;

static gboolean aura_apply_result(gpointer data) {
    AuraResult *r = data;
    gtk_label_set_text(GTK_LABEL(r->bubble), r->reply ? r->reply : "(no reply)");
    gtk_widget_set_sensitive(r->entry, TRUE);
    gtk_widget_grab_focus(r->entry);
    g_free(r->reply);
    g_free(r);
    return G_SOURCE_REMOVE;
}

static gpointer aura_worker(gpointer data) {
    AuraJob *j = data;
    char *reply = aura_ask(j->q);          /* blocking socket I/O, off the UI thread */
    AuraResult *r = g_new0(AuraResult, 1);
    r->reply = reply;
    r->bubble = j->bubble;
    r->entry = j->entry;
    g_idle_add(aura_apply_result, r);
    g_free(j->q);
    g_free(j);
    return NULL;
}

static void aura_submit(GtkEntry *entry, gpointer u) {
    const char *q = gtk_entry_get_text(entry);
    if (!q || !*q) return;
    aura_add_msg(q, TRUE);
    GtkWidget *bubble = aura_add_msg("…", FALSE);   /* thinking placeholder */
    gtk_entry_set_text(entry, "");
    gtk_widget_set_sensitive(GTK_WIDGET(entry), FALSE);
    AuraJob *j = g_new0(AuraJob, 1);
    j->q = g_strdup(q);
    j->bubble = bubble;
    j->entry = GTK_WIDGET(entry);
    GThread *t = g_thread_new("aura-ask", aura_worker, j);
    if (t) g_thread_unref(t);
}

static void toggle(GtkWidget *w) {
    if (!w) return;
    if (gtk_widget_get_visible(w)) gtk_widget_hide(w);
    else gtk_widget_show_all(w);
}
static void on_launcher_btn(GtkButton *b, gpointer u) { toggle(g_launcher); }
static void on_aura_btn(GtkButton *b, gpointer u)     { toggle(g_aura); }
static void on_aura_close(GtkButton *b, gpointer u)   { if (g_aura) gtk_widget_hide(g_aura); }

static void on_app_clicked(GtkButton *b, gpointer u) {
    App *a = u;
    launch(a->exec);
    if (g_launcher) gtk_widget_hide(g_launcher);
}

/* ----- live top-bar indicators (battery + network) ----- */
static void refresh_indicators(void) {
    if (g_bat_lbl) {
        GDir *d = g_dir_open("/sys/class/power_supply", 0, NULL);
        char *cap = NULL; const char *n;
        if (d) {
            while ((n = g_dir_read_name(d))) {
                char *tp = g_build_filename("/sys/class/power_supply", n, "type", NULL);
                char *t = NULL;
                if (g_file_get_contents(tp, &t, NULL, NULL) && g_str_has_prefix(t, "Battery")) {
                    char *cp = g_build_filename("/sys/class/power_supply", n, "capacity", NULL);
                    g_file_get_contents(cp, &cap, NULL, NULL); g_free(cp);
                }
                g_free(t); g_free(tp);
                if (cap) break;
            }
            g_dir_close(d);
        }
        if (cap) { char *s = g_strdup_printf("▤ %d%%", atoi(cap)); gtk_label_set_text(GTK_LABEL(g_bat_lbl), s); g_free(s); g_free(cap); }
        else gtk_label_set_text(GTK_LABEL(g_bat_lbl), "▤ AC");
    }
    if (g_net_lbl) {
        gboolean up = FALSE;
        GDir *d = g_dir_open("/sys/class/net", 0, NULL);
        const char *n;
        if (d) {
            while ((n = g_dir_read_name(d))) {
                if (!strcmp(n, "lo")) continue;
                char *sp = g_build_filename("/sys/class/net", n, "operstate", NULL);
                char *st = NULL;
                if (g_file_get_contents(sp, &st, NULL, NULL) && g_str_has_prefix(st, "up")) up = TRUE;
                g_free(st); g_free(sp);
            }
            g_dir_close(d);
        }
        gtk_label_set_text(GTK_LABEL(g_net_lbl), up ? "↑" : "⚠");
    }
}

/* ----- clock (also drives the lock screen + indicators) ----- */
static gboolean tick(gpointer u) {
    GDateTime *now = g_date_time_new_now_local();
    char *s = g_date_time_format(now, "%a %d %b   %H:%M");
    gtk_label_set_text(GTK_LABEL(g_clock), s);
    if (g_lock_time) { char *lt = g_date_time_format(now, "%H:%M"); gtk_label_set_text(GTK_LABEL(g_lock_time), lt); g_free(lt); }
    if (g_lock_date) { char *ld = g_date_time_format(now, "%A, %e %B"); gtk_label_set_text(GTK_LABEL(g_lock_date), g_strstrip(ld)); g_free(ld); }
    g_free(s); g_date_time_unref(now);
    refresh_indicators();
    return G_SOURCE_CONTINUE;
}

/* ----- builders ----- */
static GtkWidget *make_tbtn(const char *label, GCallback cb) {
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_style_context_add_class(gtk_widget_get_style_context(b), "tbtn");
    gtk_widget_set_focus_on_click(b, FALSE);
    if (cb) g_signal_connect(b, "clicked", cb, NULL);
    return b;
}

static void build_topbar(void) {
    /* Reserve the bar's height (positive exclusive zone) so maximized/tiled
     * windows sit BELOW it and their titlebars stay reachable. Only the
     * full-cover wallpaper must stay at -1 (its arrangement is what aborts
     * GTK); a fixed-height edge strip reserving an explicit +40 is well-defined. */
    GtkWidget *bar = layer_window(GTK_LAYER_SHELL_LAYER_TOP, TRUE, FALSE, TRUE, TRUE, S(40));
    gtk_widget_set_name(bar, "topbar");
    gtk_widget_set_size_request(bar, -1, S(40));
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(hb, 12); gtk_widget_set_margin_end(hb, 12);

    /* logo is a button that opens the Daybreak menu (About / Lock / power) */
    GtkWidget *logo = gtk_button_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(logo), "logobtn");
    gtk_widget_set_focus_on_click(logo, FALSE);
    GtkWidget *logolbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(logolbl),
        "<span foreground='#34e0c8' size='x-large'>◗</span>  <b>Daybreak</b>");
    gtk_widget_set_name(logolbl, "logo");
    gtk_container_add(GTK_CONTAINER(logo), logolbl);
    g_signal_connect(logo, "clicked", G_CALLBACK(on_logo_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(hb), logo, FALSE, FALSE, 0);

    GtkWidget *apps = make_tbtn("▦ Apps", G_CALLBACK(on_launcher_btn));
    gtk_box_pack_start(GTK_BOX(hb), apps, FALSE, FALSE, 0);

    GtkWidget *store = make_tbtn("◇ Store", G_CALLBACK(on_store_btn));
    gtk_box_pack_start(GTK_BOX(hb), store, FALSE, FALSE, 0);

    GtkWidget *widg = make_tbtn("▤ Widgets", G_CALLBACK(on_widgets_btn));
    gtk_box_pack_start(GTK_BOX(hb), widg, FALSE, FALSE, 0);

    /* center clock is a button that opens notifications + calendar */
    GtkWidget *clockb = gtk_button_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(clockb), "tbtn");
    gtk_widget_set_focus_on_click(clockb, FALSE);
    g_signal_connect(clockb, "clicked", G_CALLBACK(on_noti_btn), NULL);
    g_clock = gtk_label_new("");
    gtk_widget_set_name(g_clock, "clock");
    gtk_container_add(GTK_CONTAINER(clockb), g_clock);
    gtk_box_set_center_widget(GTK_BOX(hb), clockb);

    GtkWidget *aura = make_tbtn("◆ Aura", G_CALLBACK(on_aura_btn));
    gtk_style_context_add_class(gtk_widget_get_style_context(aura), "accent");
    gtk_box_pack_end(GTK_BOX(hb), aura, FALSE, FALSE, 0);

    /* right-side indicators open the control center; battery/network go live */
    GtkWidget *ind = gtk_button_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(ind), "tbtn");
    gtk_widget_set_focus_on_click(ind, FALSE);
    g_signal_connect(ind, "clicked", G_CALLBACK(on_cc_btn), NULL);
    GtkWidget *indrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(indrow), "tray");
    g_net_lbl = gtk_label_new("↑");
    g_bat_lbl = gtk_label_new("▤");
    gtk_box_pack_start(GTK_BOX(indrow), g_net_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(indrow), gtk_label_new("◑"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(indrow), g_bat_lbl, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(ind), indrow);
    gtk_box_pack_end(GTK_BOX(hb), ind, FALSE, FALSE, 6);

    gtk_container_add(GTK_CONTAINER(bar), hb);
    gtk_widget_show_all(bar);
}

/* ============================ pinned dock apps ============================ */
static char *pins_path(void) {
    return g_build_filename(g_get_user_config_dir(), "aurora", "pins", NULL);
}
static void free_pins(void) {
    for (GList *l = g_pins; l; l = l->next) {
        Pin *p = l->data; g_free(p->name); g_free(p->exec); g_free(p->icon); g_free(p);
    }
    g_list_free(g_pins); g_pins = NULL;
}
static void load_pins(void) {
    free_pins();
    char *path = pins_path(), *body = NULL;
    if (g_file_get_contents(path, &body, NULL, NULL)) {
        char **lines = g_strsplit(body, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            if (!*lines[i]) continue;
            char **f = g_strsplit(lines[i], "\t", 3);
            if (f[0] && f[1]) {
                Pin *p = g_new0(Pin, 1);
                p->name = g_strdup(f[0]); p->exec = g_strdup(f[1]);
                p->icon = g_strdup(f[2] ? f[2] : "");
                g_pins = g_list_append(g_pins, p);
            }
            g_strfreev(f);
        }
        g_strfreev(lines); g_free(body);
    } else {                                   /* first run: a sensible default */
        Pin *p = g_new0(Pin, 1);
        p->name = g_strdup("Terminal"); p->exec = g_strdup("foot"); p->icon = g_strdup("");
        g_pins = g_list_append(g_pins, p);
    }
    g_free(path);
}
static void save_pins(void) {
    char *path = pins_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    GString *s = g_string_new(NULL);
    for (GList *l = g_pins; l; l = l->next) {
        Pin *p = l->data;
        g_string_append_printf(s, "%s\t%s\t%s\n", p->name, p->exec, p->icon ? p->icon : "");
    }
    g_file_set_contents(path, s->str, -1, NULL);
    g_string_free(s, TRUE); g_free(dir); g_free(path);
}
static gboolean is_pinned(const char *exec) {
    for (GList *l = g_pins; l; l = l->next)
        if (!g_strcmp0(((Pin *)l->data)->exec, exec)) return TRUE;
    return FALSE;
}
static void rebuild_pinbox(void);
static void unpin_exec(const char *exec) {
    for (GList *l = g_pins; l; l = l->next) {
        Pin *p = l->data;
        if (!g_strcmp0(p->exec, exec)) {
            g_pins = g_list_remove(g_pins, p);
            g_free(p->name); g_free(p->exec); g_free(p->icon); g_free(p);
            break;
        }
    }
    save_pins(); rebuild_pinbox();
}
static void pin_app(const char *name, const char *exec, const char *icon) {
    if (is_pinned(exec)) { aurora_toast("▣", "Already pinned"); return; }
    Pin *p = g_new0(Pin, 1);
    p->name = g_strdup(name); p->exec = g_strdup(exec); p->icon = g_strdup(icon ? icon : "");
    g_pins = g_list_append(g_pins, p);
    save_pins(); rebuild_pinbox();
    aurora_toast("▣", "Pinned to dock");
}
static gboolean pin_btn_press(GtkWidget *w, GdkEventButton *e, gpointer exec) {
    if (e->button == 3) { unpin_exec((const char *)exec); return TRUE; }  /* right-click unpins */
    return FALSE;
}
static void rebuild_pinbox(void) {
    if (!g_pinbox) return;
    GList *kids = gtk_container_get_children(GTK_CONTAINER(g_pinbox));
    for (GList *l = kids; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);
    for (GList *l = g_pins; l; l = l->next) {
        Pin *p = l->data;
        GtkWidget *b = gtk_button_new();
        gtk_style_context_add_class(gtk_widget_get_style_context(b), "dbtn");
        gtk_widget_set_focus_on_click(b, FALSE);
        gtk_widget_set_tooltip_text(b, p->name);
        /* empty pin icon: try the exec name as a themed icon (e.g. "foot"
         * resolves to the terminal's own hicolor icon) before the generic */
        gtk_container_add(GTK_CONTAINER(b),
            icon_widget((p->icon && *p->icon) ? p->icon : p->exec, S(26), "▣"));
        g_signal_connect_swapped(b, "clicked", G_CALLBACK(launch), p->exec);
        g_signal_connect(b, "button-press-event", G_CALLBACK(pin_btn_press), p->exec);
        gtk_box_pack_start(GTK_BOX(g_pinbox), b, FALSE, FALSE, 0);
    }
    gtk_widget_show_all(g_pinbox);
}

/* ================= taskbar: running windows (wlr-foreign-toplevel) ========= */
static void update_task_button(Toplevel *tl) {
    /* Unnamed toplevels (no title, no app id) render as blank dead space in
     * the dock — keep the button parked until the window identifies itself. */
    gboolean named = (tl->title && *tl->title) || (tl->app_id && *tl->app_id);
    if (!named) { if (tl->btn) gtk_widget_hide(tl->btn); dock_sep_sync(); return; }
    const char *t = (tl->title && *tl->title) ? tl->title : tl->app_id;
    gtk_widget_set_tooltip_text(tl->btn, t);
    if (!tl->ico_done && tl->app_id && *tl->app_id) {
        /* real app icon on the taskbar chip: theme icon named like the
         * app id, else the matching launcher entry's icon */
        GtkWidget *ico = NULL;
        if (gtk_icon_theme_has_icon(gtk_icon_theme_get_default(), tl->app_id)) {
            ico = gtk_image_new_from_icon_name(tl->app_id, GTK_ICON_SIZE_MENU);
            gtk_image_set_pixel_size(GTK_IMAGE(ico), S(26));
        } else {
            char *want = g_ascii_strdown(tl->app_id, -1);
            for (GList *l = g_apps; l && !ico; l = l->next) {
                App *a = l->data;
                char *nm = g_ascii_strdown(a->name, -1);
                char *ex = g_ascii_strdown(a->exec ? a->exec : "", -1);
                if (strstr(ex, want) || strstr(nm, want))
                    if (a->icon && *a->icon)
                        ico = icon_widget(a->icon, S(26), NULL);
                g_free(nm); g_free(ex);
            }
            g_free(want);
        }
        if (!ico) {
            /* generic fallback so a chip always has an icon (shell-owned
             * windows like "Set up Aura" have no matching app icon) */
            ico = gtk_image_new_from_icon_name("application-x-executable",
                                               GTK_ICON_SIZE_MENU);
            gtk_image_set_pixel_size(GTK_IMAGE(ico), S(26));
        }
        if (ico) {
            gtk_box_pack_start(GTK_BOX(tl->hbox), ico, FALSE, FALSE, 0);
            gtk_box_reorder_child(GTK_BOX(tl->hbox), ico, 0);
        }
        tl->ico_done = TRUE;
    }
    if (!gtk_widget_get_parent(tl->btn) && g_taskbar)
        gtk_box_pack_start(GTK_BOX(g_taskbar), tl->btn, FALSE, FALSE, 0);
    GtkStyleContext *sc = gtk_widget_get_style_context(tl->btn);
    if (tl->activated) gtk_style_context_add_class(sc, "active");
    else               gtk_style_context_remove_class(sc, "active");
    gtk_widget_show_all(tl->btn);
    dock_sep_sync();
}
static void ftl_title(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *t) {
    (void)h; Toplevel *tl = d; g_free(tl->title); tl->title = g_strdup(t);
    update_task_button(tl);          /* reflect live title changes immediately */
}
static void ftl_app_id(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *a) {
    (void)h; Toplevel *tl = d; g_free(tl->app_id); tl->app_id = g_strdup(a);
    update_task_button(tl);
}
static void ftl_out_enter(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o) { (void)d;(void)h;(void)o; }
static void ftl_out_leave(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o) { (void)d;(void)h;(void)o; }
static void ftl_state(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *arr) {
    (void)h; Toplevel *tl = d; tl->activated = FALSE;
    uint32_t *st;
    wl_array_for_each(st, arr)
        if (*st == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) tl->activated = TRUE;
}
static void ftl_done(void *d, struct zwlr_foreign_toplevel_handle_v1 *h) { (void)h; update_task_button((Toplevel *)d); }
static GtkWidget *g_dock;
static void ftl_closed(void *d, struct zwlr_foreign_toplevel_handle_v1 *h) {
    Toplevel *tl = d;
    if (tl->btn) { gtk_widget_destroy(tl->btn); g_object_unref(tl->btn); }
    dock_sep_sync();
    zwlr_foreign_toplevel_handle_v1_destroy(h);
    g_toplevels = g_list_remove(g_toplevels, tl);
    g_free(tl->title); g_free(tl->app_id); g_free(tl);
    /* the layer surface keeps its old width after a chip is removed — force a
     * re-shrink-wrap so the dock doesn't show a dead gap */
    if (g_dock) gtk_window_resize(GTK_WINDOW(g_dock), 1, 1);
}
static void ftl_parent(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct zwlr_foreign_toplevel_handle_v1 *p) { (void)d;(void)h;(void)p; }
static const struct zwlr_foreign_toplevel_handle_v1_listener ftl_handle_listener = {
    .title = ftl_title, .app_id = ftl_app_id,
    .output_enter = ftl_out_enter, .output_leave = ftl_out_leave,
    .state = ftl_state, .done = ftl_done, .closed = ftl_closed, .parent = ftl_parent,
};
static void on_task_clicked(GtkButton *b, gpointer d) {
    (void)b; Toplevel *tl = d;
    if (g_ftl_seat) zwlr_foreign_toplevel_handle_v1_activate(tl->handle, g_ftl_seat);
}
/* right-click a running-window chip -> context menu with Quit (macOS "hold the
 * dock icon" affordance). Quit asks the toplevel to close via the protocol. */
static void task_quit(GtkMenuItem *mi, gpointer d) {
    (void)mi; Toplevel *tl = d;
    zwlr_foreign_toplevel_handle_v1_close(tl->handle);
}
static gboolean task_btn_press(GtkWidget *w, GdkEventButton *e, gpointer d) {
    (void)w; Toplevel *tl = d;
    if (e->type == GDK_BUTTON_PRESS && e->button == 3) {
        GtkWidget *menu = gtk_menu_new();
        GtkWidget *mi = gtk_menu_item_new_with_label("Quit");
        g_signal_connect(mi, "activate", G_CALLBACK(task_quit), tl);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
        gtk_widget_show_all(menu);
        gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)e);
        return TRUE;
    }
    return FALSE;
}
static void ftl_new(void *d, struct zwlr_foreign_toplevel_manager_v1 *m,
                    struct zwlr_foreign_toplevel_handle_v1 *h) {
    (void)d; (void)m;
    Toplevel *tl = g_new0(Toplevel, 1);
    tl->handle = h;
    tl->btn = gtk_button_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(tl->btn), "taskbtn");
    gtk_widget_set_focus_on_click(tl->btn, FALSE);
    /* macOS-style chip: icon only, window title as tooltip (set once the
     * toplevel names itself). Keeps the dock visually uniform. */
    tl->lbl = NULL;
    tl->hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(tl->hbox, GTK_ALIGN_CENTER);
    gtk_container_add(GTK_CONTAINER(tl->btn), tl->hbox);
    g_signal_connect(tl->btn, "clicked", G_CALLBACK(on_task_clicked), tl);
    g_signal_connect(tl->btn, "button-press-event", G_CALLBACK(task_btn_press), tl);
    g_object_ref_sink(tl->btn);   /* parked until the toplevel gets a name */
    zwlr_foreign_toplevel_handle_v1_add_listener(h, &ftl_handle_listener, tl);
    g_toplevels = g_list_append(g_toplevels, tl);
}
static void ftl_finished(void *d, struct zwlr_foreign_toplevel_manager_v1 *m) { (void)d;(void)m; }
static const struct zwlr_foreign_toplevel_manager_v1_listener ftl_mgr_listener = {
    .toplevel = ftl_new, .finished = ftl_finished,
};
static void reg_global(void *d, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t version) {
    (void)d;
    if (!strcmp(iface, zwlr_foreign_toplevel_manager_v1_interface.name)) {
        uint32_t v = version < 3 ? version : 3;
        g_ftl_mgr = wl_registry_bind(r, name, &zwlr_foreign_toplevel_manager_v1_interface, v);
        zwlr_foreign_toplevel_manager_v1_add_listener(g_ftl_mgr, &ftl_mgr_listener, NULL);
    } else if (!strcmp(iface, wl_seat_interface.name) && !g_ftl_seat) {
        g_ftl_seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
    }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d;(void)r;(void)name; }
static const struct wl_registry_listener reg_listener = {
    .global = reg_global, .global_remove = reg_remove,
};
static void taskbar_init(void) {
    GdkDisplay *gd = gdk_display_get_default();
    if (!GDK_IS_WAYLAND_DISPLAY(gd)) return;
    struct wl_display *dpy = gdk_wayland_display_get_wl_display(gd);
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(dpy);   /* bind manager + seat (adds mgr listener) */
    wl_display_roundtrip(dpy);   /* deliver already-open toplevels */
}

/* ============================== boot splash =============================== */
static gboolean splash_fade(gpointer u) {
    if (!g_splash) return G_SOURCE_REMOVE;
    double o = gtk_widget_get_opacity(g_splash) - 0.08;
    if (o <= 0.0) { gtk_widget_destroy(g_splash); g_splash = NULL; return G_SOURCE_REMOVE; }
    gtk_widget_set_opacity(g_splash, o);
    return G_SOURCE_CONTINUE;
}
static gboolean splash_begin_fade(gpointer u) {
    (void)u;
    if (g_splash) g_timeout_add(40, splash_fade, NULL);
    return G_SOURCE_REMOVE;
}
static void build_splash(void) {
    g_splash = layer_window(GTK_LAYER_SHELL_LAYER_OVERLAY, TRUE, TRUE, TRUE, TRUE, -1);
    gtk_widget_set_name(g_splash, "splash");
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_halign(v, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(v, GTK_ALIGN_CENTER);
    GtkWidget *logo = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(logo),
        "<span foreground='#34e0c8' size='100000'>◗</span>");
    GtkWidget *name = gtk_label_new("DaybreakOS");
    gtk_widget_set_name(name, "splash-name");
    GtkWidget *sub = gtk_label_new("aurora");
    gtk_widget_set_name(sub, "splash-sub");
    gtk_box_pack_start(GTK_BOX(v), logo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), name, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), sub, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(g_splash), v);
    gtk_widget_show_all(g_splash);
    g_timeout_add(1800, splash_begin_fade, NULL);   /* hold, then fade out */
}

static void build_dock(void) {
    /* Reserve the dock band (icon row + bottom margin) so maximized windows
     * stop above it instead of hiding behind the floating pill. */
    GtkWidget *dock = layer_window(GTK_LAYER_SHELL_LAYER_TOP, FALSE, TRUE, FALSE, FALSE, S(64));
    g_dock = dock;
    gtk_widget_set_name(dock, "dock");
    gtk_layer_set_margin(GTK_WINDOW(dock), GTK_LAYER_SHELL_EDGE_BOTTOM, S(12));
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    /* pinned apps (persisted) */
    g_pinbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(hb), g_pinbox, FALSE, FALSE, 0);
    load_pins();
    rebuild_pinbox();

    /* all-apps launcher */
    GtkWidget *all = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(all), icon_widget("view-app-grid-symbolic", S(26), "▦"));
    gtk_style_context_add_class(gtk_widget_get_style_context(all), "dbtn");
    gtk_widget_set_focus_on_click(all, FALSE);
    g_signal_connect(all, "clicked", G_CALLBACK(on_launcher_btn), NULL);
    gtk_box_pack_start(GTK_BOX(hb), all, FALSE, FALSE, 0);

    /* running windows (foreign-toplevel taskbar) */
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_style_context_add_class(gtk_widget_get_style_context(sep), "docksep");
    gtk_box_pack_start(GTK_BOX(hb), sep, FALSE, FALSE, 4);
    g_docksep = sep;
    g_taskbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(hb), g_taskbar, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(dock), hb);
    gtk_widget_show_all(dock);
    dock_sep_sync();          /* divider hidden until a window opens */
}

static void build_wallpaper(void) {
    /* Anchor all four edges so the compositor stretches the wallpaper to the
     * exact output size, plus a fixed fallback size request for the pre-configure
     * frame. This pattern is stable; sizing from a queried monitor height is not
     * (see AURORA_COVER_H). The patched gtk-layer-shell bounds the geometry hints
     * so opposite-edge anchoring no longer trips the 65535px limit. */
    GtkWidget *wp = layer_window(GTK_LAYER_SHELL_LAYER_BACKGROUND, TRUE, TRUE, TRUE, TRUE, -1);
    g_wallpaper = wp;
    gtk_style_context_add_class(gtk_widget_get_style_context(wp), "wallpaper");
    gtk_widget_set_size_request(wp, -1, AURORA_COVER_H);
    gtk_widget_add_events(wp, GDK_BUTTON_PRESS_MASK);
    GtkWidget *brand = gtk_label_new("DaybreakOS");
    gtk_style_context_add_class(gtk_widget_get_style_context(brand), "wp-brand");
    gtk_widget_set_halign(brand, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(brand, GTK_ALIGN_CENTER);
    gtk_container_add(GTK_CONTAINER(wp), brand);
    gtk_widget_show_all(wp);
}

/* ----- launcher search + keyboard (KDE Kickoff-style type-to-filter) ----- */
static gboolean launcher_filter(GtkFlowBoxChild *child, gpointer u) {
    if (!g_launcher_search) return TRUE;
    const char *q = gtk_entry_get_text(GTK_ENTRY(g_launcher_search));
    if (!q || !*q) return TRUE;
    const char *name = g_object_get_data(G_OBJECT(child), "app-name");
    if (!name) return TRUE;
    char *ql = g_ascii_strdown(q, -1);
    char *nl = g_ascii_strdown(name, -1);
    gboolean match = (strstr(nl, ql) != NULL);
    g_free(ql); g_free(nl);
    return match;
}
static void launcher_search_changed(GtkSearchEntry *e, gpointer u) {
    if (g_launcher_flow) gtk_flow_box_invalidate_filter(GTK_FLOW_BOX(g_launcher_flow));
}
static gboolean launcher_key(GtkWidget *w, GdkEventKey *ev, gpointer u) {
    if (ev->keyval == GDK_KEY_Escape) { gtk_widget_hide(g_launcher); return TRUE; }
    return FALSE;
}
/* (Re)build the app grid from the current g_apps list. Safe to call repeatedly:
 * clears any existing tiles first. */
static void launcher_pin_toggle(GtkMenuItem *mi, gpointer a_ptr) {
    (void)mi; App *a = a_ptr;
    if (is_pinned(a->exec)) unpin_exec(a->exec);
    else                    pin_app(a->name, a->exec, a->icon);
}
static gboolean launcher_item_press(GtkWidget *w, GdkEventButton *e, gpointer a_ptr) {
    (void)w;
    if (e->button != 3) return FALSE;          /* right-click -> pin/unpin menu */
    App *a = a_ptr;
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *mi = gtk_menu_item_new_with_label(
        is_pinned(a->exec) ? "Unpin from Dock" : "Pin to Dock");
    g_signal_connect(mi, "activate", G_CALLBACK(launcher_pin_toggle), a);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)e);
    return TRUE;
}
static void populate_launcher(void) {
    if (!g_launcher_flow) return;
    GList *kids = gtk_container_get_children(GTK_CONTAINER(g_launcher_flow));
    for (GList *l = kids; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);

    for (GList *l = g_apps; l; l = l->next) {
        App *a = l->data;
        GtkWidget *b = gtk_button_new();
        gtk_style_context_add_class(gtk_widget_get_style_context(b), "applaunch");
        GtkWidget *bv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        GtkWidget *ic = icon_widget(a->icon, S(44), "◈");
        GtkWidget *nm = gtk_label_new(a->name);
        gtk_label_set_ellipsize(GTK_LABEL(nm), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(nm), 12);
        gtk_style_context_add_class(gtk_widget_get_style_context(nm), "aname");
        gtk_box_pack_start(GTK_BOX(bv), ic, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(bv), nm, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(b), bv);
        g_signal_connect(b, "clicked", G_CALLBACK(on_app_clicked), a);
        gtk_widget_add_events(b, GDK_BUTTON_PRESS_MASK);
        g_signal_connect(b, "button-press-event", G_CALLBACK(launcher_item_press), a);
        gtk_container_add(GTK_CONTAINER(g_launcher_flow), b);
        /* tag the auto-created flow child with the app name for the search filter */
        GtkWidget *child = gtk_widget_get_parent(b);
        g_object_set_data_full(G_OBJECT(child), "app-name",
                               g_strdup(a->name), g_free);
    }
    gtk_widget_show_all(g_launcher_flow);
}

static void launcher_shown(GtkWidget *w, gpointer u) {
    /* Rescan on every open so newly installed apps (e.g. from the Store)
     * show up without a shell restart. */
    scan_apps();
    populate_launcher();
    if (g_launcher_search) {
        gtk_entry_set_text(GTK_ENTRY(g_launcher_search), "");
        gtk_widget_grab_focus(g_launcher_search);
    }
}

static void build_launcher(void) {
    g_launcher = layer_window(GTK_LAYER_SHELL_LAYER_OVERLAY, FALSE, FALSE, FALSE, FALSE, -1);
    gtk_widget_set_name(g_launcher, "launcher");
    gtk_layer_set_keyboard_mode(GTK_WINDOW(g_launcher), GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
    gtk_window_set_default_size(GTK_WINDOW(g_launcher), S(720), S(480));
    g_signal_connect(g_launcher, "key-press-event", G_CALLBACK(launcher_key), NULL);
    g_signal_connect(g_launcher, "show", G_CALLBACK(launcher_shown), NULL);

    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);

    GtkWidget *title = gtk_label_new("Applications");
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "title");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(v), title, FALSE, FALSE, 0);

    /* search box: type to filter the grid live */
    g_launcher_search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_launcher_search), "Search applications…");
    gtk_widget_set_name(g_launcher_search, "launcher-search");
    g_signal_connect(g_launcher_search, "search-changed",
                     G_CALLBACK(launcher_search_changed), NULL);
    gtk_box_pack_start(GTK_BOX(v), g_launcher_search, FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_vexpand(scroll, TRUE);
    g_launcher_flow = gtk_flow_box_new();
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(g_launcher_flow), 6);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(g_launcher_flow), 4);
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(g_launcher_flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(g_launcher_flow), TRUE);
    gtk_flow_box_set_filter_func(GTK_FLOW_BOX(g_launcher_flow),
                                 launcher_filter, NULL, NULL);

    populate_launcher();
    gtk_container_add(GTK_CONTAINER(scroll), g_launcher_flow);
    gtk_box_pack_start(GTK_BOX(v), scroll, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(g_launcher), v);
    /* built hidden; toggled by the Apps button */
}

static void build_aura(void) {
    /* Right-side full-height slide-over: anchor top+bottom+right so the
     * compositor stretches it to the full output height; 380px wide, fixed
     * fallback height (same rationale as the wallpaper). */
    g_aura = layer_window(GTK_LAYER_SHELL_LAYER_OVERLAY, TRUE, TRUE, FALSE, TRUE, -1);
    gtk_widget_set_name(g_aura, "aura");
    gtk_layer_set_keyboard_mode(GTK_WINDOW(g_aura), GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
    /* Width only. The top+bottom anchors let the compositor set the real height;
     * forcing a tall AURORA_COVER_H size request here makes GTK lay the window out
     * at 2160px so the bottom-docked input entry falls below the visible screen —
     * i.e. "no place to type". A -1 height uses the compositor's configured size. */
    gtk_widget_set_size_request(g_aura, S(380), -1);

    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(v, 16); gtk_widget_set_margin_end(v, 16);
    gtk_widget_set_margin_top(v, 14);   gtk_widget_set_margin_bottom(v, 14);

    /* header row: title on the left, a close (×) button on the right so the
     * slide-over can always be dismissed without hunting for the top-bar button
     * (which the overlay covers). */
    GtkWidget *headrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *head = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(head),
        "<span size='large'>◐</span>  <b>Aura</b>  · on-device");
    gtk_style_context_add_class(gtk_widget_get_style_context(head), "head");
    gtk_widget_set_halign(head, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(headrow), head, FALSE, FALSE, 0);

    GtkWidget *close = gtk_button_new_with_label("×");
    gtk_style_context_add_class(gtk_widget_get_style_context(close), "tbtn");
    gtk_widget_set_focus_on_click(close, FALSE);
    g_signal_connect(close, "clicked", G_CALLBACK(on_aura_close), NULL);
    gtk_box_pack_end(GTK_BOX(headrow), close, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), headrow, FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_vexpand(scroll, TRUE);
    g_aura_log = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_add(GTK_CONTAINER(scroll), g_aura_log);
    gtk_box_pack_start(GTK_BOX(v), scroll, TRUE, TRUE, 0);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Ask Aura…");
    g_signal_connect(entry, "activate", G_CALLBACK(aura_submit), NULL);
    gtk_box_pack_start(GTK_BOX(v), entry, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(g_aura), v);
    aura_add_msg("Hi, I'm Aura — running fully on this device. Ask me to open apps, "
                 "change settings, or anything else.", FALSE);
}

/* ================= Daybreak Store =================
 * A native software centre: reads the same pipe-delimited catalog aurorad uses,
 * shows a card per app with a Get/Open button, and installs via POST
 * /store/install (aurorad downloads the AppImage and unpacks it under
 * ~/Applications). Installs run on a worker thread so a slow download never
 * freezes the shell. */

static gboolean store_is_installed(const char *id) {
    char *p = g_build_filename(g_get_home_dir(), "Applications", id, "AppRun", NULL);
    gboolean ex = g_file_test(p, G_FILE_TEST_EXISTS);
    g_free(p);
    return ex;
}

static void read_catalog(void) {
    const char *paths[] = {
        "/usr/share/aurora/store/catalog",
        "/opt/aura/store/catalog",
        NULL
    };
    char *data = NULL;
    for (int i = 0; paths[i] && !data; i++)
        g_file_get_contents(paths[i], &data, NULL, NULL);
    if (!data) {
        /* dev fallback: repo-relative store/catalog when run from the source tree */
        char *local = g_build_filename(g_get_current_dir(), "..", "..", "store", "catalog", NULL);
        g_file_get_contents(local, &data, NULL, NULL);
        g_free(local);
    }
    if (!data) return;

    char **lines = g_strsplit(data, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char *line = g_strstrip(lines[i]);
        if (!*line || line[0] == '#') continue;
        char **f = g_strsplit(line, "|", 6);
        if (g_strv_length(f) >= 6) {
            StoreApp *s = g_new0(StoreApp, 1);
            s->id       = g_strdup(g_strstrip(f[0]));
            s->name     = g_strdup(g_strstrip(f[1]));
            s->category = g_strdup(g_strstrip(f[2]));
            s->icon     = g_strdup(g_strstrip(f[3]));
            s->desc     = g_strdup(g_strstrip(f[4]));
            s->installed = store_is_installed(s->id);
            g_catalog = g_list_append(g_catalog, s);
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
    g_free(data);
}

/* async install: POST /store/install off the UI thread, update the button back
 * on it (same pattern as Aura's worker). */
typedef struct { StoreApp *app; GtkWidget *btn; } StoreJob;
typedef struct { StoreApp *app; GtkWidget *btn; gboolean ok; char *err; } StoreResult;

static gboolean store_apply_result(gpointer data) {
    StoreResult *r = data;
    if (r->ok) {
        r->app->installed = TRUE;
        gtk_button_set_label(GTK_BUTTON(r->btn), "Open");
        gtk_widget_set_tooltip_text(r->btn, NULL);
    } else {
        gtk_button_set_label(GTK_BUTTON(r->btn), "Retry");
        gtk_widget_set_tooltip_text(r->btn, r->err ? r->err : "Install failed");
    }
    gtk_widget_set_sensitive(r->btn, TRUE);
    g_free(r->err);
    g_free(r);
    return G_SOURCE_REMOVE;
}

static gpointer store_worker(gpointer data) {
    StoreJob *j = data;
    char *jid = g_strescape(j->app->id, "");
    char *body = g_strdup_printf("{\"id\":\"%s\"}", jid);
    g_free(jid);
    char *resp = aurorad_send("POST", "/store/install", body);
    g_free(body);

    StoreResult *r = g_new0(StoreResult, 1);
    r->app = j->app;
    r->btn = j->btn;
    r->ok  = resp && (strstr(resp, "\"ok\": true") || strstr(resp, "\"ok\":true"));
    if (!r->ok) {
        /* surface aurorad's "error" string if present */
        char *e = resp ? strstr(resp, "\"error\"") : NULL;
        if (e && (e = strchr(e, ':'))) {
            e++; while (*e == ' ' || *e == '"') e++;
            GString *m = g_string_new("");
            for (; *e && *e != '"'; e++) g_string_append_c(m, *e);
            r->err = g_string_free(m, FALSE);
        } else {
            r->err = g_strdup(resp ? "install failed" : "Store bridge offline");
        }
    }
    g_free(resp);
    g_idle_add(store_apply_result, r);
    g_free(j);
    return NULL;
}

static void on_store_btn_clicked(GtkButton *b, gpointer u) {
    StoreApp *app = u;
    if (app->installed) {
        /* launch the unpacked AppImage directly */
        char *run = g_build_filename(g_get_home_dir(), "Applications", app->id, "AppRun", NULL);
        launch(run);
        g_free(run);
        if (g_store) gtk_widget_hide(g_store);
        return;
    }
    gtk_button_set_label(b, "Installing…");
    gtk_widget_set_sensitive(GTK_WIDGET(b), FALSE);
    StoreJob *j = g_new0(StoreJob, 1);
    j->app = app;
    j->btn = GTK_WIDGET(b);
    GThread *t = g_thread_new("store-install", store_worker, j);
    if (t) g_thread_unref(t);
}

static gboolean store_key(GtkWidget *w, GdkEventKey *ev, gpointer u) {
    if (ev->keyval == GDK_KEY_Escape) { gtk_widget_hide(g_store); return TRUE; }
    return FALSE;
}

static void build_store(void) {
    read_catalog();
    g_store = layer_window(GTK_LAYER_SHELL_LAYER_OVERLAY, FALSE, FALSE, FALSE, FALSE, -1);
    gtk_widget_set_name(g_store, "store");
    gtk_layer_set_keyboard_mode(GTK_WINDOW(g_store), GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
    gtk_window_set_default_size(GTK_WINDOW(g_store), S(560), S(520));
    g_signal_connect(g_store, "key-press-event", G_CALLBACK(store_key), NULL);

    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(v, 20); gtk_widget_set_margin_end(v, 20);
    gtk_widget_set_margin_top(v, 18);   gtk_widget_set_margin_bottom(v, 18);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title),
        "<b>Daybreak Store</b>  ·  <span size='small'>on-device app catalog</span>");
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "title");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(v), title, FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_vexpand(scroll, TRUE);
    g_store_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

    for (GList *l = g_catalog; l; l = l->next) {
        StoreApp *a = l->data;
        GtkWidget *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
        gtk_style_context_add_class(gtk_widget_get_style_context(card), "storecard");

        GtkWidget *ic = gtk_label_new(a->icon && *a->icon ? a->icon : "◈");
        gtk_style_context_add_class(gtk_widget_get_style_context(ic), "sicon");
        gtk_box_pack_start(GTK_BOX(card), ic, FALSE, FALSE, 0);

        GtkWidget *info = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *nm = gtk_label_new(a->name);
        gtk_widget_set_halign(nm, GTK_ALIGN_START);
        gtk_style_context_add_class(gtk_widget_get_style_context(nm), "sname");
        GtkWidget *ds = gtk_label_new(a->desc);
        gtk_widget_set_halign(ds, GTK_ALIGN_START);
        gtk_label_set_line_wrap(GTK_LABEL(ds), TRUE);
        gtk_label_set_xalign(GTK_LABEL(ds), 0.0);
        gtk_style_context_add_class(gtk_widget_get_style_context(ds), "sdesc");
        gtk_box_pack_start(GTK_BOX(info), nm, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(info), ds, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(card), info, TRUE, TRUE, 0);

        GtkWidget *btn = gtk_button_new_with_label(a->installed ? "Open" : "Get");
        gtk_style_context_add_class(gtk_widget_get_style_context(btn), "sget");
        gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
        gtk_widget_set_focus_on_click(btn, FALSE);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_store_btn_clicked), a);
        gtk_box_pack_end(GTK_BOX(card), btn, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(g_store_list), card, FALSE, FALSE, 0);
    }
    if (!g_catalog) {
        GtkWidget *empty = gtk_label_new("No catalog found.\nCheck /usr/share/aurora/store/catalog");
        gtk_style_context_add_class(gtk_widget_get_style_context(empty), "sdesc");
        gtk_box_pack_start(GTK_BOX(g_store_list), empty, FALSE, FALSE, 0);
    }

    gtk_container_add(GTK_CONTAINER(scroll), g_store_list);
    gtk_box_pack_start(GTK_BOX(v), scroll, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(g_store), v);
    /* built hidden; toggled by the Store button */
}

static void on_store_btn(GtkButton *b, gpointer u) { toggle(g_store); }

/* ================= desktop chrome ================= */

/* Best-effort brightness: write the first /sys/class/backlight device. No-ops
 * on hardware without a backlight (e.g. a VM) — that's expected, not an error. */
static void set_brightness_pct(int pct) {
    GDir *d = g_dir_open("/sys/class/backlight", 0, NULL);
    if (!d) return;
    const char *n = g_dir_read_name(d);
    if (n) {
        char *mp = g_build_filename("/sys/class/backlight", n, "max_brightness", NULL);
        char *bp = g_build_filename("/sys/class/backlight", n, "brightness", NULL);
        char *mx = NULL;
        if (g_file_get_contents(mp, &mx, NULL, NULL)) {
            int v = atoi(mx) * pct / 100;
            char buf[32]; g_snprintf(buf, sizeof buf, "%d", v);
            g_file_set_contents(bp, buf, -1, NULL);
            g_free(mx);
        }
        g_free(mp); g_free(bp);
    }
    g_dir_close(d);
}
static void set_volume_pct(int pct) {
    char *cmd = g_strdup_printf("amixer -q sset Master %d%%", pct);
    launch(cmd); g_free(cmd);
}

/* ---------- night light: a warm, click-through full-screen overlay ---------- */
static void nightlight_apply(void) {
    if (!g_nightlight) return;
    if (g_night_on) {
        gtk_style_context_add_class(gtk_widget_get_style_context(g_nightlight), "on");
        gtk_widget_show_all(g_nightlight);
        GdkWindow *w = gtk_widget_get_window(g_nightlight);
        if (w) { cairo_region_t *e = cairo_region_create();
                 gdk_window_input_shape_combine_region(w, e, 0, 0); cairo_region_destroy(e); }
    } else {
        gtk_widget_hide(g_nightlight);
    }
}
static void build_nightlight(void) {
    g_nightlight = layer_window(GTK_LAYER_SHELL_LAYER_OVERLAY, TRUE, TRUE, TRUE, TRUE, -1);
    gtk_widget_set_name(g_nightlight, "nightlight");
    gtk_widget_set_size_request(g_nightlight, -1, AURORA_COVER_H);
    /* built hidden; shown only while night light is on */
}

/* ---------- toast ---------- */
static guint g_toast_timer = 0;
static gboolean toast_hide(gpointer u) { if (g_toast) gtk_widget_hide(g_toast); g_toast_timer = 0; return G_SOURCE_REMOVE; }
static void aurora_toast(const char *glyph, const char *text) {
    if (!g_toast) return;
    char *m = g_strdup_printf("%s   %s", glyph ? glyph : "◆", text);
    gtk_label_set_text(GTK_LABEL(g_toast_lbl), m);
    g_free(m);
    gtk_widget_show_all(g_toast);
    if (g_toast_timer) g_source_remove(g_toast_timer);
    g_toast_timer = g_timeout_add(2600, toast_hide, NULL);
}
static void build_toast(void) {
    g_toast = layer_window(GTK_LAYER_SHELL_LAYER_OVERLAY, FALSE, TRUE, FALSE, TRUE, -1);
    gtk_widget_set_name(g_toast, "toast");
    gtk_layer_set_margin(GTK_WINDOW(g_toast), GTK_LAYER_SHELL_EDGE_BOTTOM, S(86));
    gtk_layer_set_margin(GTK_WINDOW(g_toast), GTK_LAYER_SHELL_EDGE_RIGHT, S(16));
    g_toast_lbl = gtk_label_new("");
    gtk_widget_set_margin_start(g_toast_lbl, 16); gtk_widget_set_margin_end(g_toast_lbl, 16);
    gtk_widget_set_margin_top(g_toast_lbl, 12);   gtk_widget_set_margin_bottom(g_toast_lbl, 12);
    gtk_container_add(GTK_CONTAINER(g_toast), g_toast_lbl);
}

/* ---------- control center ---------- */
static void on_cc_toggle(GtkButton *b, gpointer u) {
    const char *name = u;
    gboolean on = !GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "on"));
    g_object_set_data(G_OBJECT(b), "on", GINT_TO_POINTER(on));
    GtkStyleContext *sc = gtk_widget_get_style_context(GTK_WIDGET(b));
    if (on) gtk_style_context_add_class(sc, "on"); else gtk_style_context_remove_class(sc, "on");
    if (!g_strcmp0(name, "Night Light")) { g_night_on = on; nightlight_apply(); }
    char *msg = g_strdup_printf("%s %s", name, on ? "on" : "off");
    aurora_toast("◑", msg); g_free(msg);
}
static void on_bright_changed(GtkRange *r, gpointer u) { set_brightness_pct((int)gtk_range_get_value(r)); }
static void on_vol_changed(GtkRange *r, gpointer u)    { set_volume_pct((int)gtk_range_get_value(r)); }
static void show_lock(void);
static void on_cc_power(GtkButton *b, gpointer u) { if (g_cc) gtk_widget_hide(g_cc); show_lock(); }

static void build_control_center(void) {
    g_cc = layer_window(GTK_LAYER_SHELL_LAYER_OVERLAY, TRUE, FALSE, FALSE, TRUE, -1);
    gtk_widget_set_name(g_cc, "cc");
    gtk_layer_set_keyboard_mode(GTK_WINDOW(g_cc), GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
    gtk_layer_set_margin(GTK_WINDOW(g_cc), GTK_LAYER_SHELL_EDGE_TOP, S(40) + 6);
    gtk_layer_set_margin(GTK_WINDOW(g_cc), GTK_LAYER_SHELL_EDGE_RIGHT, 8);
    gtk_widget_set_size_request(g_cc, S(360), -1);

    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(v, 16); gtk_widget_set_margin_end(v, 16);
    gtk_widget_set_margin_top(v, 16);   gtk_widget_set_margin_bottom(v, 16);

    GtkWidget *eb = gtk_label_new("CONTROL CENTER");
    gtk_style_context_add_class(gtk_widget_get_style_context(eb), "eyebrow");
    gtk_widget_set_halign(eb, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(v), eb, FALSE, FALSE, 0);

    /* spectrum toggles: name, glyph, hue-class */
    struct { const char *name, *glyph, *hue; gboolean on; } tg[] = {
        {"WiFi", "⇡", "h-teal", TRUE}, {"Bluetooth", "∗", "h-violet", FALSE},
        {"Night Light", "◐", "h-rose", FALSE}, {"Do Not Disturb", "◔", "h-pink", FALSE},
        {"Airplane", "▲", "h-green", FALSE}, {"Nearby", "◎", "h-teal", FALSE},
    };
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 9);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 9);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    for (int i = 0; i < 6; i++) {
        GtkWidget *b = gtk_button_new();
        GtkStyleContext *sc = gtk_widget_get_style_context(b);
        gtk_style_context_add_class(sc, "qt");
        gtk_style_context_add_class(sc, tg[i].hue);
        gtk_widget_set_focus_on_click(b, FALSE);
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *ic = gtk_label_new(tg[i].glyph);
        gtk_style_context_add_class(gtk_widget_get_style_context(ic), "qic");
        GtkWidget *nm = gtk_label_new(tg[i].name);
        gtk_label_set_xalign(GTK_LABEL(nm), 0.0);
        gtk_box_pack_start(GTK_BOX(row), ic, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), nm, TRUE, TRUE, 0);
        gtk_container_add(GTK_CONTAINER(b), row);
        g_object_set_data(G_OBJECT(b), "on", GINT_TO_POINTER(tg[i].on));
        if (tg[i].on) gtk_style_context_add_class(sc, "on");
        g_signal_connect(b, "clicked", G_CALLBACK(on_cc_toggle), (gpointer)tg[i].name);
        gtk_grid_attach(GTK_GRID(grid), b, i % 2, i / 2, 1, 1);
    }
    gtk_box_pack_start(GTK_BOX(v), grid, FALSE, FALSE, 0);

    /* sliders */
    GtkWidget *br = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 20, 100, 1);
    gtk_range_set_value(GTK_RANGE(br), 90); gtk_scale_set_draw_value(GTK_SCALE(br), FALSE);
    g_signal_connect(br, "value-changed", G_CALLBACK(on_bright_changed), NULL);
    GtkWidget *vol = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_range_set_value(GTK_RANGE(vol), 65); gtk_scale_set_draw_value(GTK_SCALE(vol), FALSE);
    g_signal_connect(vol, "value-changed", G_CALLBACK(on_vol_changed), NULL);
    for (int i = 0; i < 2; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        GtkWidget *gi = gtk_label_new(i == 0 ? "☀" : "◑");
        gtk_style_context_add_class(gtk_widget_get_style_context(gi), "sgi");
        gtk_box_pack_start(GTK_BOX(row), gi, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), i == 0 ? br : vol, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(v), row, FALSE, FALSE, 0);
    }

    /* footer: user + power */
    GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_top(foot, 6);
    GtkWidget *who = gtk_label_new("  Aneek");
    gtk_style_context_add_class(gtk_widget_get_style_context(who), "who");
    gtk_box_pack_start(GTK_BOX(foot), who, FALSE, FALSE, 0);
    GtkWidget *pwr = gtk_button_new_with_label("⏻");
    gtk_style_context_add_class(gtk_widget_get_style_context(pwr), "tbtn");
    gtk_widget_set_focus_on_click(pwr, FALSE);
    g_signal_connect(pwr, "clicked", G_CALLBACK(on_cc_power), NULL);
    gtk_box_pack_end(GTK_BOX(foot), pwr, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), foot, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(g_cc), v);
}
static void on_cc_btn(GtkButton *b, gpointer u) { toggle(g_cc); }

/* ---------- notifications + calendar ---------- */
static void on_noti_clear(GtkButton *b, gpointer u) {
    if (!g_noti_list) return;
    GList *ch = gtk_container_get_children(GTK_CONTAINER(g_noti_list));
    for (GList *l = ch; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(ch);
    GtkWidget *empty = gtk_label_new("You're all caught up.");
    gtk_style_context_add_class(gtk_widget_get_style_context(empty), "nempty");
    gtk_box_pack_start(GTK_BOX(g_noti_list), empty, FALSE, FALSE, 0);
    gtk_widget_show_all(g_noti_list);
}
static void noti_add(const char *src, const char *hue, const char *title, const char *body) {
    if (!g_noti_list) return;
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "ncard");
    char *sm = g_strdup_printf("<span foreground='%s'>●</span>  %s", hue, src);
    GtkWidget *s = gtk_label_new(NULL); gtk_label_set_markup(GTK_LABEL(s), sm); g_free(sm);
    gtk_label_set_xalign(GTK_LABEL(s), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(s), "nsrc");
    GtkWidget *t = gtk_label_new(title); gtk_label_set_xalign(GTK_LABEL(t), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(t), "ntitle");
    GtkWidget *p = gtk_label_new(body); gtk_label_set_xalign(GTK_LABEL(p), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(p), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(p), "nbody");
    gtk_box_pack_start(GTK_BOX(card), s, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), t, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), p, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(g_noti_list), card, FALSE, FALSE, 0);
}
static void build_notifications(void) {
    g_noti = layer_window(GTK_LAYER_SHELL_LAYER_OVERLAY, TRUE, FALSE, FALSE, FALSE, -1);
    gtk_widget_set_name(g_noti, "noti");
    gtk_layer_set_margin(GTK_WINDOW(g_noti), GTK_LAYER_SHELL_EDGE_TOP, S(40) + 6);
    gtk_widget_set_size_request(g_noti, S(360), -1);

    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(v, 16); gtk_widget_set_margin_end(v, 16);
    gtk_widget_set_margin_top(v, 14);   gtk_widget_set_margin_bottom(v, 16);

    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *ti = gtk_label_new("Notifications");
    gtk_style_context_add_class(gtk_widget_get_style_context(ti), "title");
    gtk_widget_set_halign(ti, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(h), ti, TRUE, TRUE, 0);
    GtkWidget *clr = gtk_button_new_with_label("Clear all");
    gtk_style_context_add_class(gtk_widget_get_style_context(clr), "nclear");
    gtk_widget_set_focus_on_click(clr, FALSE);
    g_signal_connect(clr, "clicked", G_CALLBACK(on_noti_clear), NULL);
    gtk_box_pack_end(GTK_BOX(h), clr, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), h, FALSE, FALSE, 0);

    g_noti_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    noti_add("Daybreak Store", "#34e0c8", "Calculator installed", "GNOME Calculator is ready in Apps and the dock.");
    noti_add("Aura", "#8b7cf6", "Good morning", "3 updates available and battery is healthy. Ask me for the rundown.");
    gtk_box_pack_start(GTK_BOX(v), g_noti_list, FALSE, FALSE, 0);

    /* calendar */
    GtkWidget *cal = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_style_context_add_class(gtk_widget_get_style_context(cal), "cal");
    GDateTime *now = g_date_time_new_now_local();
    char *m = g_date_time_format(now, "%B");
    char *d = g_date_time_format(now, "%e");
    char *w = g_date_time_format(now, "%A");
    GtkWidget *lm = gtk_label_new(m); gtk_style_context_add_class(gtk_widget_get_style_context(lm), "cal-m");
    GtkWidget *ld = gtk_label_new(g_strstrip(d)); gtk_style_context_add_class(gtk_widget_get_style_context(ld), "cal-d");
    GtkWidget *lw = gtk_label_new(w); gtk_style_context_add_class(gtk_widget_get_style_context(lw), "cal-w");
    gtk_box_pack_start(GTK_BOX(cal), lm, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(cal), ld, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(cal), lw, FALSE, FALSE, 0);
    g_free(m); g_free(d); g_free(w); g_date_time_unref(now);
    gtk_box_pack_start(GTK_BOX(v), cal, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(g_noti), v);
}
static void on_noti_btn(GtkButton *b, gpointer u) { toggle(g_noti); }

/* ---------- widgets ---------- */
static gboolean update_stats(gpointer u) {
    if (!g_wstats || !gtk_widget_get_visible(g_widgets)) return G_SOURCE_CONTINUE;
    char *mi = NULL; long total = 0, avail = 0;
    if (g_file_get_contents("/proc/meminfo", &mi, NULL, NULL)) {
        char *p;
        if ((p = strstr(mi, "MemTotal:")))     total = atol(p + 9);
        if ((p = strstr(mi, "MemAvailable:"))) avail = atol(p + 13);
        g_free(mi);
    }
    char *la = NULL; double load = 0;
    if (g_file_get_contents("/proc/loadavg", &la, NULL, NULL)) { load = atof(la); g_free(la); }
    double usedg = (total - avail) / 1048576.0, totg = total / 1048576.0;
    char *s = g_strdup_printf("%.1f GB of %.1f GB · load %.2f", usedg, totg, load);
    gtk_label_set_text(GTK_LABEL(g_wstats), s);
    g_free(s);
    return G_SOURCE_CONTINUE;
}
/* Aura "your day": one-shot LLM summary, off the UI thread */
static gboolean day_apply(gpointer data) { char *r = data; if (g_wday) gtk_label_set_text(GTK_LABEL(g_wday), r); g_free(r); return G_SOURCE_REMOVE; }
static gpointer day_worker(gpointer u) {
    char *r = NULL;
    /* the model may still be loading right after boot — retry until it answers */
    for (int i = 0; i < 5; i++) {
        g_free(r);
        r = aura_ask("In one short sentence, give me a friendly morning summary of my system.");
        if (r && !strstr(r, "warming up") && !strstr(r, "waking up") && !strstr(r, "offline")) break;
        g_usleep(6 * G_USEC_PER_SEC);
    }
    g_idle_add(day_apply, r); return NULL;
}
static void build_widgets(void) {
    g_widgets = layer_window(GTK_LAYER_SHELL_LAYER_OVERLAY, TRUE, FALSE, TRUE, FALSE, -1);
    gtk_widget_set_name(g_widgets, "widgets");
    gtk_layer_set_margin(GTK_WINDOW(g_widgets), GTK_LAYER_SHELL_EDGE_TOP, S(40) + 6);
    gtk_layer_set_margin(GTK_WINDOW(g_widgets), GTK_LAYER_SHELL_EDGE_LEFT, 8);
    gtk_widget_set_size_request(g_widgets, S(420), -1);

    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(v, 16); gtk_widget_set_margin_end(v, 16);
    gtk_widget_set_margin_top(v, 16);   gtk_widget_set_margin_bottom(v, 16);
    /* header: eyebrow on the left, a close (✕) button on the right so the
     * panel has an obvious dismiss control (it's an overlay with no click-away). */
    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *eb = gtk_label_new("YOUR GLANCE");
    gtk_style_context_add_class(gtk_widget_get_style_context(eb), "eyebrow");
    gtk_widget_set_halign(eb, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(hdr), eb, TRUE, TRUE, 0);
    GtkWidget *cls = gtk_button_new_with_label("✕");
    gtk_style_context_add_class(gtk_widget_get_style_context(cls), "pclose");
    gtk_widget_set_halign(cls, GTK_ALIGN_END);
    g_signal_connect_swapped(cls, "clicked", G_CALLBACK(gtk_widget_hide), g_widgets);
    gtk_box_pack_end(GTK_BOX(hdr), cls, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), hdr, FALSE, FALSE, 0);

    /* Aura day card */
    GtkWidget *ai = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(ai), "wcard-ai");
    GtkWidget *ah = gtk_label_new("◆  Aura · your day");
    gtk_widget_set_halign(ah, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(ah), "wh");
    g_wday = gtk_label_new("Warming up your summary…");
    gtk_label_set_line_wrap(GTK_LABEL(g_wday), TRUE); gtk_label_set_xalign(GTK_LABEL(g_wday), 0.0);
    gtk_box_pack_start(GTK_BOX(ai), ah, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ai), g_wday, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), ai, FALSE, FALSE, 0);

    /* system card */
    GtkWidget *sc = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(sc), "wcard");
    GtkWidget *sh = gtk_label_new("▤  SYSTEM");
    gtk_widget_set_halign(sh, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(sh), "wh");
    g_wstats = gtk_label_new("reading…");
    gtk_widget_set_halign(g_wstats, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(sc), sh, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sc), g_wstats, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), sc, FALSE, FALSE, 0);

    /* weather card (placeholder until a network source is wired) */
    GtkWidget *wc = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(wc), "wcard");
    GtkWidget *wh = gtk_label_new("◔  WEATHER");
    gtk_widget_set_halign(wh, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(wh), "wh");
    GtkWidget *wt = gtk_label_new("Connect Wi-Fi to see local conditions.");
    gtk_widget_set_halign(wt, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(wc), wh, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(wc), wt, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), wc, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(g_widgets), v);
}
static void on_widgets_btn(GtkButton *b, gpointer u) {
    toggle(g_widgets);
    if (gtk_widget_get_visible(g_widgets)) {
        update_stats(NULL);
        GThread *t = g_thread_new("aura-day", day_worker, NULL);
        if (t) g_thread_unref(t);
    }
}

/* ---------- lock screen ---------- */
static gboolean lock_key(GtkWidget *w, GdkEventKey *e, gpointer u) {
    if (e->keyval == GDK_KEY_Return || e->keyval == GDK_KEY_Escape) gtk_widget_hide(g_lock);
    return TRUE;
}
static gboolean lock_click(GtkWidget *w, GdkEventButton *e, gpointer u) { gtk_widget_hide(g_lock); return TRUE; }
static void show_lock(void) {
    if (!g_lock) return;
    gtk_widget_show_all(g_lock);
    gtk_widget_grab_focus(g_lock);
}
static void build_lock(void) {
    g_lock = layer_window(GTK_LAYER_SHELL_LAYER_OVERLAY, TRUE, TRUE, TRUE, TRUE, -1);
    gtk_widget_set_name(g_lock, "lock");
    gtk_layer_set_keyboard_mode(GTK_WINDOW(g_lock), GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
    gtk_widget_set_size_request(g_lock, -1, AURORA_COVER_H);
    gtk_widget_set_events(g_lock, GDK_BUTTON_PRESS_MASK | GDK_KEY_PRESS_MASK);
    g_signal_connect(g_lock, "key-press-event", G_CALLBACK(lock_key), NULL);
    g_signal_connect(g_lock, "button-press-event", G_CALLBACK(lock_click), NULL);
    gtk_widget_set_can_focus(g_lock, TRUE);

    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_halign(v, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(v, GTK_ALIGN_CENTER);
    g_lock_time = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(g_lock_time), "lock-time");
    g_lock_date = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(g_lock_date), "lock-date");
    GtkWidget *hint = gtk_label_new("Press Enter or click to unlock");
    gtk_style_context_add_class(gtk_widget_get_style_context(hint), "lock-hint");
    gtk_widget_set_margin_top(hint, S(40));
    gtk_box_pack_start(GTK_BOX(v), g_lock_time, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), g_lock_date, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), hint, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(g_lock), v);
}

/* ---------- animated aurora band (the signature) ---------- */
static gboolean draw_aurora(GtkWidget *w, cairo_t *cr, gpointer u) {
    GtkAllocation a; gtk_widget_get_allocation(w, &a);
    double W = a.width, H = a.height, p = g_aurora_phase / 100.0;
    struct { double r, g, b, base, spd; } rib[] = {
        {0.20, 0.88, 0.78, 0.28, 1.0}, {0.42, 0.90, 0.63, 0.52, 1.4},
        {0.55, 0.49, 0.96, 0.70, 0.8}, {0.94, 0.45, 0.71, 0.44, 1.7},
    };
    /* start fully transparent so only the glow shows over the wallpaper */
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SCREEN);
    for (int i = 0; i < 4; i++) {
        double cx = W * (rib[i].base + 0.16 * sin(p * rib[i].spd + i));
        double cy = H * 0.28 + 22 * sin(p * 0.7 + i * 1.3);
        cairo_pattern_t *pt = cairo_pattern_create_radial(cx, cy, 0, cx, cy, W * 0.5);
        double al = g_night_on ? 0.12 : 0.32;
        cairo_pattern_add_color_stop_rgba(pt, 0, rib[i].r, rib[i].g, rib[i].b, al);
        cairo_pattern_add_color_stop_rgba(pt, 1, rib[i].r, rib[i].g, rib[i].b, 0);
        cairo_set_source(cr, pt); cairo_paint(cr);
        cairo_pattern_destroy(pt);
    }
    /* The radial glows are wider than the band is tall, so without this they
     * hit the window edge at high alpha and render as a hard stripe. Fade the
     * whole band's alpha to 0 at the bottom so it melts into the wallpaper. */
    cairo_set_operator(cr, CAIRO_OPERATOR_DEST_IN);
    cairo_pattern_t *fade = cairo_pattern_create_linear(0, 0, 0, H);
    cairo_pattern_add_color_stop_rgba(fade, 0.00, 1, 1, 1, 1.00);
    cairo_pattern_add_color_stop_rgba(fade, 0.45, 1, 1, 1, 0.70);
    cairo_pattern_add_color_stop_rgba(fade, 1.00, 1, 1, 1, 0.00);
    cairo_set_source(cr, fade); cairo_paint(cr);
    cairo_pattern_destroy(fade);
    return FALSE;
}
static gboolean tick_aurora(gpointer canvas) { g_aurora_phase++; gtk_widget_queue_draw(GTK_WIDGET(canvas)); return G_SOURCE_CONTINUE; }
static void build_aurora_band(void) {
    GtkWidget *band = layer_window(GTK_LAYER_SHELL_LAYER_BACKGROUND, TRUE, FALSE, TRUE, TRUE, -1);
    gtk_widget_set_name(band, "aurora-band");
    gtk_widget_set_size_request(band, -1, S(170));
    /* an RGBA visual + app-paintable stops GTK filling the window opaque (white) */
    GdkScreen *scr = gdk_screen_get_default();
    GdkVisual *rgba = gdk_screen_get_rgba_visual(scr);
    if (rgba) gtk_widget_set_visual(band, rgba);
    gtk_widget_set_app_paintable(band, TRUE);
    GtkWidget *canvas = gtk_drawing_area_new();
    gtk_widget_set_app_paintable(canvas, TRUE);
    if (rgba) gtk_widget_set_visual(canvas, rgba);
    g_signal_connect(canvas, "draw", G_CALLBACK(draw_aurora), NULL);
    gtk_container_add(GTK_CONTAINER(band), canvas);
    gtk_widget_show_all(band);
    g_timeout_add(140, tick_aurora, canvas);
}

/* ---------- desktop context menu (right-click wallpaper) ---------- */
/* ---- Daybreak menu (top-bar logo): About + session power ---- */
static void am_about(GtkMenuItem *i, gpointer u) {
    /* Custom About window (GtkAboutDialog renders as an unthemed washed-out
     * light dialog on our dark desktop) — opaque dark card in Aurora identity. */
    GtkWidget *w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_name(w, "aboutwin");
    gtk_window_set_title(GTK_WINDOW(w), "About DaybreakOS");
    gtk_window_set_default_size(GTK_WINDOW(w), S(380), S(320));
    gtk_window_set_resizable(GTK_WINDOW(w), FALSE);

    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(v, 30); gtk_widget_set_margin_bottom(v, 24);
    gtk_widget_set_margin_start(v, 28); gtk_widget_set_margin_end(v, 28);

    GtkWidget *mark = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(mark), "<span foreground='#34e0c8' size='xx-large'>◗</span>");
    GtkWidget *name = gtk_label_new("DaybreakOS");
    gtk_style_context_add_class(gtk_widget_get_style_context(name), "abt-name");
    GtkWidget *ver = gtk_label_new("Version 1.0 — Daybreak");
    gtk_style_context_add_class(gtk_widget_get_style_context(ver), "abt-ver");
    GtkWidget *desc = gtk_label_new("A private, on-device AI desktop.\n"
        "Linux From Scratch · labwc/Wayland\nThe Aurora shell · on-device Aura");
    gtk_label_set_justify(GTK_LABEL(desc), GTK_JUSTIFY_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(desc), "abt-desc");
    GtkWidget *link = gtk_label_new("github.com/Aneek1/daybreakos");
    gtk_style_context_add_class(gtk_widget_get_style_context(link), "abt-link");
    GtkWidget *close = gtk_button_new_with_label("Close");
    gtk_style_context_add_class(gtk_widget_get_style_context(close), "abt-close");
    gtk_widget_set_halign(close, GTK_ALIGN_CENTER);
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_widget_destroy), w);

    gtk_box_pack_start(GTK_BOX(v), mark, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), name, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), ver, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), desc, FALSE, FALSE, 8);
    gtk_box_pack_start(GTK_BOX(v), link, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), close, FALSE, FALSE, 10);
    gtk_container_add(GTK_CONTAINER(w), v);
    gtk_widget_show_all(w);
}
/* pull a top-level JSON string value for `key` out of `json` (flat objects). */
static char *json_str(const char *json, const char *key) {
    if (!json) return NULL;
    char *pat = g_strdup_printf("\"%s\"", key);
    char *p = strstr(json, pat); g_free(pat);
    if (!p) return NULL;
    p = strchr(p, ':'); if (!p) return NULL; p++;
    /* skip spaces, then exactly ONE opening quote — a greedy skip here ate
     * both quotes of an empty string ("error": "") and returned '}' */
    while (*p == ' ') p++;
    if (*p != '"') return NULL;
    p++;
    GString *out = g_string_new("");
    for (; *p && *p != '"'; p++) {
        if (*p == '\\' && p[1]) { p++; g_string_append_c(out, *p == 'n' ? '\n' : *p); }
        else g_string_append_c(out, *p);
    }
    return g_string_free(out, FALSE);
}
static int json_int(const char *json, const char *key) {
    if (!json) return 0;
    char *pat = g_strdup_printf("\"%s\"", key);
    char *p = strstr(json, pat); g_free(pat);
    if (!p) return 0;
    p = strchr(p, ':'); if (!p) return 0;
    return atoi(p + 1);
}
static gboolean json_true(const char *json, const char *key) {
    if (!json) return FALSE;
    char *pat = g_strdup_printf("\"%s\"", key);
    char *p = strstr(json, pat); g_free(pat);
    if (!p) return FALSE;
    p = strchr(p, ':'); if (!p) return FALSE; p++;
    while (*p == ' ') p++;
    return strncmp(p, "true", 4) == 0;
}
static void am_persist(GtkMenuItem *i, gpointer u) {
    aurora_toast("▤", "Setting up persistent storage…");
    char *r = aurorad_send("POST", "/system/persist-setup", "{}");
    char *msg = json_str(r, "error");
    if (!msg) msg = json_str(r, "message");
    aurora_toast("▤", msg ? msg : "Storage: no response from aurorad.");
    g_free(msg); g_free(r);
}

/* ---------- OS installer: copy the live system onto a real disk ---------- */
static GtkWidget *g_inst = NULL, *g_inst_disks = NULL, *g_inst_bar = NULL;
static GtkWidget *g_inst_status = NULL, *g_inst_go = NULL, *g_inst_gate = NULL;
static char g_inst_disk[128] = "";
static guint g_inst_timer = 0;
static gboolean g_inst_finished = FALSE;

static void inst_disk_toggled(GtkToggleButton *b, gpointer dev) {
    if (gtk_toggle_button_get_active(b))
        g_strlcpy(g_inst_disk, (char *)dev, sizeof g_inst_disk);
}
static void inst_gate_toggled(GtkToggleButton *b, gpointer u) {
    gtk_widget_set_sensitive(g_inst_go,
        gtk_toggle_button_get_active(b) && g_inst_disk[0]);
}
static guint g_inst_scan_timer = 0;
static gboolean inst_rescan(gpointer u);
static void inst_refresh_disks(void) {
    GList *kids = gtk_container_get_children(GTK_CONTAINER(g_inst_disks));
    for (GList *l = kids; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);
    g_inst_disk[0] = '\0';
    char *r = aurorad_send("GET", "/system/disks", NULL);
    if (!r) {
        /* aurorad is still starting (session race) — retry until it answers */
        GtkWidget *wait = gtk_label_new("Looking for disks…");
        gtk_widget_set_halign(wait, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(g_inst_disks), wait, FALSE, FALSE, 2);
        gtk_widget_show_all(g_inst_disks);
        if (!g_inst_scan_timer)
            g_inst_scan_timer = g_timeout_add(1200, inst_rescan, NULL);
        return;
    }
    if (g_inst_scan_timer) { g_source_remove(g_inst_scan_timer); g_inst_scan_timer = 0; }
    char *list = json_str(r, "list");
    GSList *group = NULL;
    int n = 0;
    if (list && *list) {
        char **lines = g_strsplit(list, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            if (!*lines[i]) continue;
            char **c = g_strsplit(lines[i], "|", 3);
            if (c[0] && c[1] && c[2]) {
                char *lbl = g_strdup_printf("%s   %s  ·  %s", c[0], c[1], c[2]);
                GtkWidget *rb = gtk_radio_button_new_with_label(group, lbl);
                group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(rb));
                g_signal_connect(rb, "toggled", G_CALLBACK(inst_disk_toggled),
                                 g_strdup(c[0]));   /* dev string, lives with widget */
                gtk_box_pack_start(GTK_BOX(g_inst_disks), rb, FALSE, FALSE, 2);
                g_free(lbl); n++;
                /* preselect the first (radio group starts active on the first) */
                if (n == 1) g_strlcpy(g_inst_disk, c[0], sizeof g_inst_disk);
            }
            g_strfreev(c);
        }
        g_strfreev(lines);
    }
    if (!n) {
        GtkWidget *none = gtk_label_new("No installable disk found.\n"
            "Add a hard disk to the machine, then press Refresh.");
        gtk_widget_set_halign(none, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(g_inst_disks), none, FALSE, FALSE, 2);
    }
    gtk_widget_show_all(g_inst_disks);
    if (g_inst_gate)
        inst_gate_toggled(GTK_TOGGLE_BUTTON(g_inst_gate), NULL);
    g_free(list); g_free(r);
}
static gboolean inst_rescan(gpointer u) {
    g_inst_scan_timer = 0;
    inst_refresh_disks();
    return G_SOURCE_REMOVE;   /* refresh re-arms the timer if still waiting */
}
static void inst_refresh_clicked(GtkButton *b, gpointer u) { inst_refresh_disks(); }
static gboolean inst_poll(gpointer u) {
    char *r = aurorad_send("GET", "/system/install-progress", NULL);
    if (!r) return G_SOURCE_CONTINUE;
    int pct = json_int(r, "pct");
    char *stage = json_str(r, "stage");
    char *err = json_str(r, "error");
    gboolean done = json_true(r, "done");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g_inst_bar), pct / 100.0);
    if (err && *err) {
        char *m = g_strdup_printf("Error: %s", err);
        gtk_label_set_text(GTK_LABEL(g_inst_status), m); g_free(m);
        gtk_widget_set_sensitive(g_inst_go, FALSE);
        g_free(stage); g_free(err); g_free(r); g_inst_timer = 0;
        return G_SOURCE_REMOVE;
    }
    if (done) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g_inst_bar), 1.0);
        gtk_label_set_text(GTK_LABEL(g_inst_status),
            "Installation complete. Remove the ISO, then restart.");
        gtk_button_set_label(GTK_BUTTON(g_inst_go), "Restart");
        gtk_widget_set_sensitive(g_inst_go, TRUE);
        g_inst_finished = TRUE;
        g_free(stage); g_free(err); g_free(r); g_inst_timer = 0;
        return G_SOURCE_REMOVE;
    }
    if (stage && *stage) gtk_label_set_text(GTK_LABEL(g_inst_status), stage);
    g_free(stage); g_free(err); g_free(r);
    return G_SOURCE_CONTINUE;
}
/* power actions go through the root system service — the session user can't
 * systemctl reboot without polkit (the button silently did nothing) */
static void power_action(const char *act) {
    char body[64];
    g_snprintf(body, sizeof body, "{\"action\":\"%s\"}", act);
    char *r = aurorad_send("POST", "/system/power", body);
    if (!r) launch(g_str_equal(act, "reboot") ? "systemctl reboot"
                                              : "systemctl poweroff");
    g_free(r);
}
static void inst_go(GtkButton *b, gpointer u) {
    if (g_inst_finished) { power_action("reboot"); return; }
    if (!g_inst_disk[0]) return;
    char body[196];
    g_snprintf(body, sizeof body, "{\"disk\":\"%s\"}", g_inst_disk);
    char *r = aurorad_send("POST", "/system/install", body);
    char *err = json_str(r, "error");
    if (err && *err) {
        char *m = g_strdup_printf("Error: %s", err);
        gtk_label_set_text(GTK_LABEL(g_inst_status), m); g_free(m);
        g_free(err); g_free(r); return;
    }
    g_free(err); g_free(r);
    gtk_widget_set_sensitive(g_inst_go, FALSE);
    gtk_widget_set_sensitive(g_inst_gate, FALSE);
    gtk_label_set_text(GTK_LABEL(g_inst_status), "Starting…");
    if (!g_inst_timer) g_inst_timer = g_timeout_add(700, inst_poll, NULL);
}
static void build_install_win(void) {
    g_inst = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_name(g_inst, "instwin");
    gtk_window_set_title(GTK_WINDOW(g_inst), "Install DaybreakOS");
    gtk_window_set_default_size(GTK_WINDOW(g_inst), S(470), S(420));
    gtk_window_set_resizable(GTK_WINDOW(g_inst), FALSE);
    g_signal_connect(g_inst, "delete-event",
                     G_CALLBACK(gtk_widget_hide_on_delete), NULL);

    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(v, 22); gtk_widget_set_margin_bottom(v, 20);
    gtk_widget_set_margin_start(v, 26); gtk_widget_set_margin_end(v, 26);

    GtkWidget *title = gtk_label_new("Install DaybreakOS");
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "abt-name");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    GtkWidget *sub = gtk_label_new("Copy DaybreakOS onto a disk so it boots without the ISO.");
    gtk_style_context_add_class(gtk_widget_get_style_context(sub), "abt-desc");
    gtk_widget_set_halign(sub, GTK_ALIGN_START);

    GtkWidget *pick = gtk_label_new("Install onto:");
    gtk_widget_set_halign(pick, GTK_ALIGN_START);
    gtk_widget_set_margin_top(pick, 6);
    g_inst_disks = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    g_inst_gate = gtk_check_button_new_with_label(
        "I understand this erases the entire selected disk.");
    gtk_style_context_add_class(gtk_widget_get_style_context(g_inst_gate), "inst-warn");
    g_signal_connect(g_inst_gate, "toggled", G_CALLBACK(inst_gate_toggled), NULL);
    gtk_widget_set_margin_top(g_inst_gate, 4);

    g_inst_bar = gtk_progress_bar_new();
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g_inst_bar), 0);
    g_inst_status = gtk_label_new("Select a disk to continue.");
    gtk_widget_set_halign(g_inst_status, GTK_ALIGN_START);
    gtk_label_set_line_wrap(GTK_LABEL(g_inst_status), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(g_inst_status), "abt-ver");

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(row, GTK_ALIGN_END);
    GtkWidget *quit = gtk_button_new_with_label("Close");
    g_signal_connect_swapped(quit, "clicked",
                             G_CALLBACK(gtk_widget_hide), g_inst);
    g_inst_go = gtk_button_new_with_label("Erase & Install");
    gtk_style_context_add_class(gtk_widget_get_style_context(g_inst_go), "inst-go");
    gtk_widget_set_sensitive(g_inst_go, FALSE);
    g_signal_connect(g_inst_go, "clicked", G_CALLBACK(inst_go), NULL);
    gtk_box_pack_start(GTK_BOX(row), quit, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), g_inst_go, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(v), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), sub, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), pick, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), g_inst_disks, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), g_inst_gate, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), g_inst_bar, FALSE, FALSE, 6);
    gtk_box_pack_start(GTK_BOX(v), g_inst_status, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(v), row, FALSE, FALSE, 4);
    gtk_container_add(GTK_CONTAINER(g_inst), v);
}
static void am_install(GtkMenuItem *i, gpointer u) {
    if (!g_inst) build_install_win();
    if (g_inst_timer) { g_source_remove(g_inst_timer); g_inst_timer = 0; }
    g_inst_finished = FALSE;
    inst_refresh_disks();
    gtk_button_set_label(GTK_BUTTON(g_inst_go), "Erase & Install");
    gtk_widget_set_sensitive(g_inst_go, FALSE);
    gtk_widget_set_sensitive(g_inst_gate, TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_inst_gate), FALSE);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g_inst_bar), 0);
    gtk_label_set_text(GTK_LABEL(g_inst_status), "Select a disk to continue.");
    gtk_widget_show_all(g_inst);
}

/* Installer-first screen: shown instead of the desktop when booted from the
 * live ISO. "Try DaybreakOS" launches the real desktop; "Erase & Install" runs
 * the same install flow as the menu item. On an installed disk the shell boots
 * straight to the desktop (the session picks the mode from the root fs type). */
static void inst_try(GtkButton *b, gpointer u) {
    launch("/usr/bin/aurora-shell");   /* spawn the full desktop, then step aside */
    gtk_main_quit();
}
static void build_installer_first(void) {
    GtkWidget *w = layer_window(GTK_LAYER_SHELL_LAYER_OVERLAY, TRUE, TRUE, TRUE, TRUE, -1);
    gtk_widget_set_name(w, "instfull");
    gtk_layer_set_keyboard_mode(GTK_WINDOW(w), GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);

    GtkWidget *center = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign(center, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(center, GTK_ALIGN_CENTER);

    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_name(card, "instcard");
    gtk_widget_set_size_request(card, S(540), -1);
    gtk_widget_set_margin_top(card, 34);  gtk_widget_set_margin_bottom(card, 30);
    gtk_widget_set_margin_start(card, 42); gtk_widget_set_margin_end(card, 42);

    GtkWidget *logo = gtk_label_new("◗");
    gtk_style_context_add_class(gtk_widget_get_style_context(logo), "inst-logo");
    gtk_widget_set_halign(logo, GTK_ALIGN_START);
    GtkWidget *title = gtk_label_new("Welcome to DaybreakOS");
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "inst-title");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    GtkWidget *sub = gtk_label_new("Install DaybreakOS onto this computer, or try it "
        "first without changing anything on your disk.");
    gtk_label_set_line_wrap(GTK_LABEL(sub), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(sub), "abt-desc");
    gtk_widget_set_halign(sub, GTK_ALIGN_START);

    GtkWidget *pickrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(pickrow, 10);
    GtkWidget *pick = gtk_label_new("INSTALL ONTO");
    gtk_style_context_add_class(gtk_widget_get_style_context(pick), "inst-sect");
    gtk_widget_set_valign(pick, GTK_ALIGN_CENTER);
    GtkWidget *refresh = gtk_button_new_with_label("⟳ Refresh");
    g_signal_connect(refresh, "clicked", G_CALLBACK(inst_refresh_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(pickrow), pick, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(pickrow), refresh, FALSE, FALSE, 0);
    g_inst_disks = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    g_inst_gate = gtk_check_button_new_with_label(
        "I understand this erases the entire selected disk.");
    gtk_style_context_add_class(gtk_widget_get_style_context(g_inst_gate), "inst-warn");
    g_signal_connect(g_inst_gate, "toggled", G_CALLBACK(inst_gate_toggled), NULL);
    g_inst_bar = gtk_progress_bar_new();
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g_inst_bar), 0);
    g_inst_status = gtk_label_new("Select a disk to install, or choose Try DaybreakOS.");
    gtk_widget_set_halign(g_inst_status, GTK_ALIGN_START);
    gtk_label_set_line_wrap(GTK_LABEL(g_inst_status), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(g_inst_status), "abt-ver");

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_halign(row, GTK_ALIGN_END); gtk_widget_set_margin_top(row, 6);
    GtkWidget *tryb = gtk_button_new_with_label("Try DaybreakOS");
    gtk_widget_set_size_request(tryb, S(150), S(40));
    g_signal_connect(tryb, "clicked", G_CALLBACK(inst_try), NULL);
    g_inst_go = gtk_button_new_with_label("Erase & Install");
    gtk_widget_set_size_request(g_inst_go, S(160), S(40));
    gtk_style_context_add_class(gtk_widget_get_style_context(g_inst_go), "inst-go");
    gtk_widget_set_sensitive(g_inst_go, FALSE);
    g_signal_connect(g_inst_go, "clicked", G_CALLBACK(inst_go), NULL);
    gtk_box_pack_start(GTK_BOX(row), tryb, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), g_inst_go, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(card), logo,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), sub,   FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), pickrow, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), g_inst_disks,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), g_inst_gate,   FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), g_inst_bar,    FALSE, FALSE, 6);
    gtk_box_pack_start(GTK_BOX(card), g_inst_status, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), row,   FALSE, FALSE, 4);
    gtk_container_add(GTK_CONTAINER(center), card);
    gtk_container_add(GTK_CONTAINER(w), center);
    inst_refresh_disks();
    gtk_widget_show_all(w);
}
/* ---------- Set up Aura (one-time model download after install) ---------- */
static void aura_menu_refresh(void);
static GtkWidget *g_aura_win = NULL, *g_aura_bar = NULL, *g_aura_st = NULL, *g_aura_go = NULL;
static guint g_aura_timer = 0;
static gboolean aura_poll(gpointer u) {
    char *r = aurorad_send("GET", "/system/aura-status", NULL);
    if (!r) return G_SOURCE_CONTINUE;
    int pct = json_int(r, "pct");
    char *err = json_str(r, "error");
    gboolean done = json_true(r, "done");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g_aura_bar), pct / 100.0);
    if (err && *err) {
        char *m = g_strdup_printf("Download failed: %s", err);
        gtk_label_set_text(GTK_LABEL(g_aura_st), m); g_free(m);
        gtk_widget_set_sensitive(g_aura_go, TRUE);
        g_free(err); g_free(r); g_aura_timer = 0; return G_SOURCE_REMOVE;
    }
    if (done) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g_aura_bar), 1.0);
        gtk_label_set_text(GTK_LABEL(g_aura_st), "Aura is ready. Open Aura and ask away.");
        aura_menu_refresh();   /* flip the Aurora-menu item to "ready" */
        g_free(err); g_free(r); g_aura_timer = 0; return G_SOURCE_REMOVE;
    }
    char *m = g_strdup_printf("Downloading Aura's model…  %d%%", pct);
    gtk_label_set_text(GTK_LABEL(g_aura_st), m); g_free(m);
    g_free(err); g_free(r);
    return G_SOURCE_CONTINUE;
}
static void aura_go(GtkButton *b, gpointer u) {
    char *r = aurorad_send("POST", "/system/aura-setup", "{}");
    char *err = json_str(r, "error");
    char *msg = json_str(r, "message");
    if (err && *err) {
        gtk_label_set_text(GTK_LABEL(g_aura_st), err);
        g_free(err); g_free(msg); g_free(r); return;
    }
    if (msg && *msg) {   /* already set up */
        gtk_label_set_text(GTK_LABEL(g_aura_st), msg);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g_aura_bar), 1.0);
        g_free(err); g_free(msg); g_free(r); return;
    }
    g_free(err); g_free(msg); g_free(r);
    gtk_widget_set_sensitive(g_aura_go, FALSE);
    gtk_label_set_text(GTK_LABEL(g_aura_st), "Starting download…");
    if (!g_aura_timer) g_aura_timer = g_timeout_add(1000, aura_poll, NULL);
}
static void am_aura_setup(GtkMenuItem *i, gpointer u) {
    if (!g_aura_win) {
        g_aura_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_widget_set_name(g_aura_win, "instwin");
        gtk_window_set_title(GTK_WINDOW(g_aura_win), "Set up Aura");
        gtk_window_set_default_size(GTK_WINDOW(g_aura_win), S(430), S(220));
        gtk_window_set_resizable(GTK_WINDOW(g_aura_win), FALSE);
        g_signal_connect(g_aura_win, "delete-event",
                         G_CALLBACK(gtk_widget_hide_on_delete), NULL);
        GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        gtk_widget_set_margin_top(v, 22); gtk_widget_set_margin_bottom(v, 20);
        gtk_widget_set_margin_start(v, 26); gtk_widget_set_margin_end(v, 26);
        GtkWidget *t = gtk_label_new("Set up Aura");
        gtk_style_context_add_class(gtk_widget_get_style_context(t), "abt-name");
        gtk_widget_set_halign(t, GTK_ALIGN_START);
        GtkWidget *s = gtk_label_new("Download Aura's on-device AI model (~0.8 GB, "
            "one time). Everything runs locally after this — no cloud.");
        gtk_label_set_line_wrap(GTK_LABEL(s), TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(s), "abt-desc");
        gtk_widget_set_halign(s, GTK_ALIGN_START);
        g_aura_bar = gtk_progress_bar_new();
        g_aura_st = gtk_label_new("Ready to download.");
        gtk_widget_set_halign(g_aura_st, GTK_ALIGN_START);
        gtk_label_set_line_wrap(GTK_LABEL(g_aura_st), TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_aura_st), "abt-ver");
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_halign(row, GTK_ALIGN_END);
        GtkWidget *close = gtk_button_new_with_label("Close");
        g_signal_connect_swapped(close, "clicked",
                                 G_CALLBACK(gtk_widget_hide), g_aura_win);
        g_aura_go = gtk_button_new_with_label("Download");
        gtk_style_context_add_class(gtk_widget_get_style_context(g_aura_go), "inst-go");
        g_signal_connect(g_aura_go, "clicked", G_CALLBACK(aura_go), NULL);
        gtk_box_pack_start(GTK_BOX(row), close, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), g_aura_go, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(v), t, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(v), s, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(v), g_aura_bar, FALSE, FALSE, 6);
        gtk_box_pack_start(GTK_BOX(v), g_aura_st, FALSE, FALSE, 0);
        gtk_box_pack_end(GTK_BOX(v), row, FALSE, FALSE, 4);
        gtk_container_add(GTK_CONTAINER(g_aura_win), v);
    }
    /* reflect current state */
    char *r = aurorad_send("GET", "/system/aura-status", NULL);
    if (json_true(r, "installed")) {
        gtk_label_set_text(GTK_LABEL(g_aura_st), "Aura is already set up.");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g_aura_bar), 1.0);
        gtk_widget_set_sensitive(g_aura_go, FALSE);
    }
    g_free(r);
    gtk_widget_show_all(g_aura_win);
}
/* The menu reflects reality: the Aura item shows ready-vs-setup, and the
 * live-media items (Install / Persistent Storage) only exist on live boots. */
static GtkWidget *g_mi_aura = NULL;
static gboolean g_live_media = FALSE;
static void aura_menu_refresh(void) {
    if (!g_mi_aura) return;
    char *r = aurorad_send("GET", "/system/aura-status", NULL);
    gboolean ready = r && json_true(r, "installed");
    gtk_menu_item_set_label(GTK_MENU_ITEM(g_mi_aura),
        ready ? "◈    Aura (AI) — ready" : "◈    Set up Aura (AI)…");
    g_free(r);
}
static void am_lock(GtkMenuItem *i, gpointer u)     { show_lock(); }
static void am_restart(GtkMenuItem *i, gpointer u)  { aurora_toast("↻", "Restarting…"); power_action("reboot"); }
static void am_shutdown(GtkMenuItem *i, gpointer u) { aurora_toast("⏻", "Shutting down…"); power_action("poweroff"); }
static void on_logo_clicked(GtkButton *b, gpointer u) {
    aura_menu_refresh();   /* label reflects whether the model is installed */
    if (g_auroramenu)
        gtk_menu_popup_at_widget(GTK_MENU(g_auroramenu), GTK_WIDGET(b),
            GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, NULL);
}
static void build_aurora_menu(void) {
    g_auroramenu = gtk_menu_new();
    struct { const char *label; GCallback cb; gboolean live_only; gboolean aura; } it[] = {
        {"◗    About DaybreakOS",         G_CALLBACK(am_about),      FALSE, FALSE},
        {"⬇    Install DaybreakOS…",       G_CALLBACK(am_install),    TRUE,  FALSE},
        {"◈    Set up Aura (AI)…",      G_CALLBACK(am_aura_setup), FALSE, TRUE},
        {"▤    Enable Persistent Storage", G_CALLBACK(am_persist), TRUE,  FALSE},
        {"◔    Lock",                   G_CALLBACK(am_lock),       FALSE, FALSE},
        {"↻    Restart",                G_CALLBACK(am_restart),    FALSE, FALSE},
        {"⏻    Shut Down",              G_CALLBACK(am_shutdown),   FALSE, FALSE},
    };
    for (int i = 0; i < (int)(sizeof(it)/sizeof(it[0])); i++) {
        if (it[i].live_only && !g_live_media) continue;   /* installed disk: hide */
        GtkWidget *mi = gtk_menu_item_new_with_label(it[i].label);
        g_signal_connect(mi, "activate", it[i].cb, NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(g_auroramenu), mi);
        if (it[i].aura) g_mi_aura = mi;
    }
    gtk_widget_show_all(g_auroramenu);
    aura_menu_refresh();
}

static GtkWidget *g_ctxmenu = NULL;
static void ctx_wall(GtkMenuItem *i, gpointer u)    { aurora_toast("▦", "Only daybreak is installed — for now."); }
static void ctx_widgets(GtkMenuItem *i, gpointer u) { if (!gtk_widget_get_visible(g_widgets)) on_widgets_btn(NULL, NULL); }
static void ctx_night(GtkMenuItem *i, gpointer u)   { g_night_on = !g_night_on; nightlight_apply(); aurora_toast("◑", g_night_on ? "Night light on" : "Night light off"); }
static void ctx_ask(GtkMenuItem *i, gpointer u)     { if (g_aura) gtk_widget_show_all(g_aura); }
static gboolean wallpaper_press(GtkWidget *w, GdkEventButton *e, gpointer u) {
    if (e->type == GDK_BUTTON_PRESS && e->button == 3 && g_ctxmenu) {
        gtk_menu_popup_at_pointer(GTK_MENU(g_ctxmenu), (GdkEvent *)e);
        return TRUE;
    }
    return FALSE;
}
static void build_context_menu(void) {
    g_ctxmenu = gtk_menu_new();
    struct { const char *label; GCallback cb; } it[] = {
        {"▦    Change wallpaper", G_CALLBACK(ctx_wall)},
        {"▤    Show widgets",     G_CALLBACK(ctx_widgets)},
        {"◑    Toggle night light", G_CALLBACK(ctx_night)},
        {"◆    Ask Aura",         G_CALLBACK(ctx_ask)},
    };
    for (int i = 0; i < 4; i++) {
        GtkWidget *mi = gtk_menu_item_new_with_label(it[i].label);
        g_signal_connect(mi, "activate", it[i].cb, NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(g_ctxmenu), mi);
    }
    gtk_widget_show_all(g_ctxmenu);
    if (g_wallpaper) g_signal_connect(g_wallpaper, "button-press-event", G_CALLBACK(wallpaper_press), NULL);
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    if (!gtk_layer_is_supported()) {
        g_printerr("aurora-shell: compositor lacks wlr-layer-shell; is labwc running?\n");
        return 1;
    }
    GtkCssProvider *css = gtk_css_provider_new();
    const char *csspath = "/usr/share/aurora/desktop/style.css";
    if (!gtk_css_provider_load_from_path(css, csspath, NULL)) {
        char *local = g_build_filename(g_get_current_dir(), "style.css", NULL);
        gtk_css_provider_load_from_path(css, local, NULL);
        g_free(local);
    }
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    scale_init();   /* size everything from the real monitor geometry */

    /* Installer-first ONLY on the live image. Decide from ground truth — the
     * root filesystem: the live ISO runs on an overlay/tmpfs root, an
     * installed disk on a real ext4. This replaces the fragile
     * autostart/marker heuristics that let the installer cover the desktop. */
    gboolean installer_mode = FALSE;
    struct statfs rst;
    if (statfs("/", &rst) == 0) {
        const unsigned long OVERLAY = 0x794c7630, TMPFS = 0x01021994,
                            RAMFS = 0x858458f6;
        if (rst.f_type == OVERLAY || rst.f_type == TMPFS || rst.f_type == RAMFS)
            installer_mode = TRUE;
    }
    g_live_media = installer_mode;   /* actual media type, before UI-mode overrides */
    if (g_getenv("AURORA_INSTALLER")) installer_mode = TRUE;   /* force on */
    if (g_getenv("AURORA_DESKTOP"))   installer_mode = FALSE;  /* force off */
    for (int i = 1; i < argc; i++) {
        if (!g_strcmp0(argv[i], "--installer")) installer_mode = TRUE;
        if (!g_strcmp0(argv[i], "--desktop"))   installer_mode = FALSE;
    }
    if (installer_mode) {
        /* Live ISO: show only the installer-first screen (no desktop). */
        build_wallpaper();          /* aurora gradient behind the card */
        build_installer_first();
        gtk_main();
        return 0;
    }

    scan_apps();
    build_wallpaper();
    build_aurora_band();
    build_topbar();
    build_dock();
    build_launcher();
    build_aura();
    build_store();
    build_control_center();
    build_notifications();
    build_widgets();
    build_nightlight();
    build_toast();
    build_lock();
    build_context_menu();
    build_aurora_menu();
    build_splash();          /* fullscreen boot splash, fades out after ~2s */
    taskbar_init();          /* start listening for running windows */

    /* Popovers start closed — the greeting message's show_all realizes the Aura
     * surface, so hide it explicitly after building. Opened via the top-bar
     * buttons; Aura also has its own × close. */
    gtk_widget_hide(g_launcher);
    gtk_widget_hide(g_aura);
    gtk_widget_hide(g_store);

    tick(NULL);
    g_timeout_add_seconds(10, tick, NULL);
    g_timeout_add_seconds(2, update_stats, NULL);
    gtk_main();
    return 0;
}
