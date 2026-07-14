/*
 * aurora-shell — AuroraOS's own native desktop shell.
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
#include <gtk-layer-shell.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ----- app model ----- */
typedef struct { char *name, *exec, *icon; } App;
static GList *g_apps = NULL;             /* App*  */
static GtkWidget *g_launcher = NULL;     /* launcher window (toggle) */
static GtkWidget *g_launcher_search = NULL; /* launcher search entry */
static GtkWidget *g_launcher_flow = NULL;   /* launcher app grid */
static GtkWidget *g_aura = NULL;         /* aura window (toggle) */
static GtkWidget *g_aura_log = NULL;     /* aura message list box */
static GtkWidget *g_clock = NULL;

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

/* strip .desktop Exec field codes (%U %F %f %u %i %c %k ...) */
static char *clean_exec(const char *raw) {
    GString *s = g_string_new("");
    for (const char *p = raw; *p; p++) {
        if (*p == '%' && p[1]) { p++; continue; }
        g_string_append_c(s, *p);
    }
    return g_string_free(s, FALSE);
}

static void scan_apps(void) {
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

/* ----- Aura: POST /ask to aurorad, return reply text ----- */
static char *aura_ask(const char *q) {
    int port = 7212;
    const char *env = g_getenv("AURORAD_PORT");
    if (env) port = atoi(env);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return g_strdup("(Aura offline)");
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return g_strdup("(Aura is still waking up…)");
    }
    char *jq = g_strescape(q, "");           /* escape \, ", control chars */
    char *body = g_strdup_printf("{\"q\":\"%s\"}", jq);
    g_free(jq);

    char *req = g_strdup_printf(
        "POST /ask HTTP/1.0\r\nHost: 127.0.0.1\r\n"
        "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
        strlen(body), body);
    ssize_t off = 0, len = strlen(req);
    while (off < len) {
        ssize_t n = write(fd, req + off, len - off);
        if (n <= 0) break;
        off += n;
    }
    g_free(req); g_free(body);

    GString *resp = g_string_new("");
    char buf[4096]; ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) g_string_append_len(resp, buf, n);
    close(fd);

    /* split headers/body, pull "reply" or "answer" out of the JSON */
    char *sep = strstr(resp->str, "\r\n\r\n");
    char *json = sep ? sep + 4 : resp->str;
    char *reply = NULL;
    /* aurorad's /ask returns {"a": "<reply>", "actions": [...]}, so "a" is the
     * primary key; the others are accepted for forward-compat with other bridges. */
    const char *keys[] = {"\"a\"", "\"reply\"", "\"answer\"", "\"text\"", NULL};
    for (int k = 0; keys[k] && !reply; k++) {
        char *p = strstr(json, keys[k]);
        if (!p) continue;
        p = strchr(p, ':'); if (!p) continue; p++;
        while (*p == ' ' || *p == '"') p++;
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
    g_string_free(resp, TRUE);
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

/* ----- clock ----- */
static gboolean tick(gpointer u) {
    GDateTime *now = g_date_time_new_now_local();
    char *s = g_date_time_format(now, "%a %d %b   %H:%M");
    gtk_label_set_text(GTK_LABEL(g_clock), s);
    g_free(s); g_date_time_unref(now);
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
    GtkWidget *bar = layer_window(GTK_LAYER_SHELL_LAYER_TOP, TRUE, FALSE, TRUE, TRUE, -1);
    gtk_widget_set_name(bar, "topbar");
    gtk_widget_set_size_request(bar, -1, 40);
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(hb, 12); gtk_widget_set_margin_end(hb, 12);

    GtkWidget *logo = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(logo),
        "<span foreground='#34e0c8' size='x-large'>◗</span>  <b>Aurora</b>");
    gtk_widget_set_name(logo, "logo");
    gtk_box_pack_start(GTK_BOX(hb), logo, FALSE, FALSE, 0);

    GtkWidget *apps = make_tbtn("▦ Apps", G_CALLBACK(on_launcher_btn));
    gtk_box_pack_start(GTK_BOX(hb), apps, FALSE, FALSE, 0);

    g_clock = gtk_label_new("");
    gtk_widget_set_name(g_clock, "clock");
    gtk_box_set_center_widget(GTK_BOX(hb), g_clock);

    GtkWidget *aura = make_tbtn("◆ Aura", G_CALLBACK(on_aura_btn));
    gtk_style_context_add_class(gtk_widget_get_style_context(aura), "accent");
    gtk_box_pack_end(GTK_BOX(hb), aura, FALSE, FALSE, 0);

    GtkWidget *tray = gtk_label_new("↑  ◑  ▤");
    gtk_style_context_add_class(gtk_widget_get_style_context(tray), "tray");
    gtk_box_pack_end(GTK_BOX(hb), tray, FALSE, FALSE, 6);

    gtk_container_add(GTK_CONTAINER(bar), hb);
    gtk_widget_show_all(bar);
}

static void build_dock(void) {
    GtkWidget *dock = layer_window(GTK_LAYER_SHELL_LAYER_TOP, FALSE, TRUE, FALSE, FALSE, -1);
    gtk_widget_set_name(dock, "dock");
    gtk_layer_set_margin(GTK_WINDOW(dock), GTK_LAYER_SHELL_EDGE_BOTTOM, 12);
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    /* pinned apps: glyph + command. Glyphs are restricted to the U+25xx
     * geometric block, which DejaVu Sans covers — emoji/symbol codepoints
     * render as tofu boxes without a dedicated symbol font. */
    struct { const char *g, *cmd; } pins[] = {
        {"▸", "foot"},                 /* terminal */
        {"▤", "foot"},                 /* files (placeholder -> terminal) */
        {NULL, NULL}
    };
    for (int i = 0; pins[i].g; i++) {
        GtkWidget *b = gtk_button_new_with_label(pins[i].g);
        gtk_style_context_add_class(gtk_widget_get_style_context(b), "dbtn");
        gtk_widget_set_focus_on_click(b, FALSE);
        g_signal_connect_swapped(b, "clicked", G_CALLBACK(launch), (gpointer)pins[i].cmd);
        gtk_box_pack_start(GTK_BOX(hb), b, FALSE, FALSE, 0);
    }
    GtkWidget *all = gtk_button_new_with_label("▦");
    gtk_style_context_add_class(gtk_widget_get_style_context(all), "dbtn");
    g_signal_connect(all, "clicked", G_CALLBACK(on_launcher_btn), NULL);
    gtk_box_pack_start(GTK_BOX(hb), all, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(dock), hb);
    gtk_widget_show_all(dock);
}

static void build_wallpaper(void) {
    /* Anchor all four edges so the compositor stretches the wallpaper to the
     * exact output size, plus a fixed fallback size request for the pre-configure
     * frame. This pattern is stable; sizing from a queried monitor height is not
     * (see AURORA_COVER_H). The patched gtk-layer-shell bounds the geometry hints
     * so opposite-edge anchoring no longer trips the 65535px limit. */
    GtkWidget *wp = layer_window(GTK_LAYER_SHELL_LAYER_BACKGROUND, TRUE, TRUE, TRUE, TRUE, -1);
    gtk_style_context_add_class(gtk_widget_get_style_context(wp), "wallpaper");
    gtk_widget_set_size_request(wp, -1, AURORA_COVER_H);
    GtkWidget *brand = gtk_label_new("AuroraOS");
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
static void launcher_shown(GtkWidget *w, gpointer u) {
    if (g_launcher_search) {
        gtk_entry_set_text(GTK_ENTRY(g_launcher_search), "");
        gtk_widget_grab_focus(g_launcher_search);
    }
}

static void build_launcher(void) {
    g_launcher = layer_window(GTK_LAYER_SHELL_LAYER_OVERLAY, FALSE, FALSE, FALSE, FALSE, -1);
    gtk_widget_set_name(g_launcher, "launcher");
    gtk_layer_set_keyboard_mode(GTK_WINDOW(g_launcher), GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
    gtk_window_set_default_size(GTK_WINDOW(g_launcher), 720, 480);
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

    for (GList *l = g_apps; l; l = l->next) {
        App *a = l->data;
        GtkWidget *b = gtk_button_new();
        gtk_style_context_add_class(gtk_widget_get_style_context(b), "applaunch");
        GtkWidget *bv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        GtkWidget *ic = gtk_label_new("◈");
        gtk_style_context_add_class(gtk_widget_get_style_context(ic), "aicon");
        GtkWidget *nm = gtk_label_new(a->name);
        gtk_label_set_ellipsize(GTK_LABEL(nm), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(nm), 12);
        gtk_style_context_add_class(gtk_widget_get_style_context(nm), "aname");
        gtk_box_pack_start(GTK_BOX(bv), ic, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(bv), nm, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(b), bv);
        g_signal_connect(b, "clicked", G_CALLBACK(on_app_clicked), a);
        gtk_container_add(GTK_CONTAINER(g_launcher_flow), b);
        /* tag the auto-created flow child with the app name for the search filter */
        GtkWidget *child = gtk_widget_get_parent(b);
        g_object_set_data_full(G_OBJECT(child), "app-name",
                               g_strdup(a->name), g_free);
    }
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
    gtk_widget_set_size_request(g_aura, 380, -1);

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

    scan_apps();
    build_wallpaper();
    build_topbar();
    build_dock();
    build_launcher();
    build_aura();

    /* Popovers start closed — the greeting message's show_all realizes the Aura
     * surface, so hide it explicitly after building. Opened via the top-bar
     * buttons; Aura also has its own × close. */
    gtk_widget_hide(g_launcher);
    gtk_widget_hide(g_aura);

    tick(NULL);
    g_timeout_add_seconds(10, tick, NULL);
    gtk_main();
    return 0;
}
