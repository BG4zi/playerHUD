// main.c
// Minimal i3 HUD: center overlay showing current MPRIS metadata (artist/title/cover)
// Build:
//   cc main.c -o playerhud $(pkg-config --cflags --libs gtk+-3.0 gio-2.0 gdk-pixbuf-2.0)

#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

typedef struct {
    GtkWidget *win;
    GtkWidget *img;
    GtkWidget *lbl_title;
    GtkWidget *lbl_artist;

    GDBusConnection *bus;
    char *player_name;          // org.mpris.MediaPlayer2.xxx
    char cover_path[256];       // cached cover file

    gboolean centered_once;
} App;

static gboolean str_has_prefix(const char *s, const char *p) {
    return s && p && g_str_has_prefix(s, p);
}

static char* pick_mpris_player(GDBusConnection *bus) {
    GError *err = NULL;
    GVariant *reply = g_dbus_connection_call_sync(
        bus,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "ListNames",
        NULL,
        G_VARIANT_TYPE("(as)"),
        G_DBUS_CALL_FLAGS_NONE,
        2000,
        NULL,
        &err
    );
    if (!reply) {
        g_printerr("ListNames failed: %s\n", err ? err->message : "unknown");
        g_clear_error(&err);
        return NULL;
    }

    GVariantIter *it = NULL;
    g_variant_get(reply, "(as)", &it);

    const char *name = NULL;
    char *chosen = NULL;
    while (g_variant_iter_next(it, "&s", &name)) {
        if (g_str_has_prefix(name, "org.mpris.MediaPlayer2.")) {
            chosen = g_strdup(name);
            break;
        }
    }

    g_variant_iter_free(it);
    g_variant_unref(reply);
    return chosen;
}

static void set_label(GtkWidget *lbl, const char *text, const char *fallback) {
    if (text && *text) gtk_label_set_text(GTK_LABEL(lbl), text);
    else gtk_label_set_text(GTK_LABEL(lbl), fallback);
}

static void maybe_load_cover(App *app, const char *art_url) {
    if (!art_url || !*art_url) return;

    snprintf(app->cover_path, sizeof(app->cover_path),
             "/tmp/playerhud_cover_%d.jpg", (int)getuid());

    if (str_has_prefix(art_url, "file://")) {
        const char *path = art_url + strlen("file://");
        if (g_file_test(path, G_FILE_TEST_EXISTS)) {
            gtk_image_set_from_file(GTK_IMAGE(app->img), path);
        }
        return;
    }

    if (str_has_prefix(art_url, "http://") || str_has_prefix(art_url, "https://")) {
        char *cmd = g_strdup_printf("curl -L -s --max-time 2 -o '%s' '%s'",
                                    app->cover_path, art_url);
        int rc = system(cmd);
        g_free(cmd);

        if (rc == 0 && g_file_test(app->cover_path, G_FILE_TEST_EXISTS)) {
            gtk_image_set_from_file(GTK_IMAGE(app->img), app->cover_path);
        }
        return;
    }
}

static void parse_metadata_and_update(App *app, GVariant *metadata_dict) {
    const char *title = NULL;
    const char *artUrl = NULL;
    char artist_buf[256] = {0};

    GVariantIter iter;
    const char *key;
    GVariant *val;

    g_variant_iter_init(&iter, metadata_dict);
    while (g_variant_iter_next(&iter, "{&sv}", &key, &val)) {
        if (strcmp(key, "xesam:title") == 0 &&
            g_variant_is_of_type(val, G_VARIANT_TYPE_STRING)) {
            title = g_variant_get_string(val, NULL);

        } else if (strcmp(key, "mpris:artUrl") == 0 &&
                   g_variant_is_of_type(val, G_VARIANT_TYPE_STRING)) {
            artUrl = g_variant_get_string(val, NULL);

        } else if (strcmp(key, "xesam:artist") == 0) {
            if (g_variant_is_of_type(val, G_VARIANT_TYPE("as"))) {
                GVariantIter ai;
                const char *a = NULL;
                g_variant_iter_init(&ai, val);
                if (g_variant_iter_next(&ai, "&s", &a) && a) {
                    g_strlcpy(artist_buf, a, sizeof(artist_buf));
                }
            }
        }
        g_variant_unref(val);
    }

    set_label(app->lbl_title,  title,  "—");
    set_label(app->lbl_artist, artist_buf[0] ? artist_buf : NULL, "—");
    if (artUrl) maybe_load_cover(app, artUrl);
}

static gboolean poll_update(gpointer user_data) {
    App *app = (App*)user_data;
    if (!app->bus) return TRUE;

    if (!app->player_name) {
        app->player_name = pick_mpris_player(app->bus);
        if (!app->player_name) {
            set_label(app->lbl_title,  "No player", NULL);
            set_label(app->lbl_artist, "MPRIS not found", NULL);
            return TRUE;
        }
    }

    GError *err = NULL;
    GVariant *reply = g_dbus_connection_call_sync(
        app->bus,
        app->player_name,
        "/org/mpris/MediaPlayer2",
        "org.freedesktop.DBus.Properties",
        "Get",
        g_variant_new("(ss)", "org.mpris.MediaPlayer2.Player", "Metadata"),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        1500,
        NULL,
        &err
    );

    if (!reply) {
        g_printerr("Metadata Get failed: %s\n", err ? err->message : "unknown");
        g_clear_error(&err);

        g_clear_pointer(&app->player_name, g_free);
        set_label(app->lbl_title,  "—", NULL);
        set_label(app->lbl_artist, "—", NULL);
        return TRUE;
    }

    GVariant *v = NULL;
    g_variant_get(reply, "(v)", &v);

    if (v && g_variant_is_of_type(v, G_VARIANT_TYPE("a{sv}"))) {
        parse_metadata_and_update(app, v);
    }

    if (v) g_variant_unref(v);
    g_variant_unref(reply);
    return TRUE;
}

static gboolean on_key(GtkWidget *w, GdkEventKey *e, gpointer ud) {
    (void)w; (void)ud;
    if (e->keyval == GDK_KEY_Escape) {
        gtk_main_quit();
        return TRUE;
    }
    return FALSE;
}

static void apply_css(void) {
    const char *css =
        "window { background: rgba(10, 15, 25, 0.0); }"
        ".card {"
        "  background: rgba(10, 15, 25, 0.86);"
        "  border-radius: 18px;"
        "  padding: 18px;"
        "}"
        ".title { color: #ffffff; font-weight: 700; font-size: 18px; }"
        ".artist { color: #9ca3af; font-size: 13px; }";

    GtkCssProvider *prov = gtk_css_provider_new();
    gtk_css_provider_load_from_data(prov, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(prov),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );
    g_object_unref(prov);
}

static void center_on_pointer_monitor(GtkWindow *win, int ww, int wh) {
    GdkDisplay *dpy = gdk_display_get_default();
    if (!dpy) return;

    GdkSeat *seat = gdk_display_get_default_seat(dpy);
    GdkDevice *ptr = gdk_seat_get_pointer(seat);

    int px = 0, py = 0;
    GdkScreen *scr = NULL;
    gdk_device_get_position(ptr, &scr, &px, &py);

    GdkMonitor *mon = gdk_display_get_monitor_at_point(dpy, px, py);
    if (!mon) mon = gdk_display_get_primary_monitor(dpy);
    if (!mon) return;

    GdkRectangle geo;
    gdk_monitor_get_geometry(mon, &geo);

    int x = geo.x + (geo.width  - ww) / 2;
    int y = geo.y + (geo.height - wh) / 2;

    gtk_window_move(win, x, y);
}

static void on_size_allocate(GtkWidget *w, GdkRectangle *alloc, gpointer ud) {
    App *app = (App*)ud;
    if (app->centered_once) return;

    int ww = alloc->width;
    int wh = alloc->height;
    if (ww <= 1 || wh <= 1) return;

    center_on_pointer_monitor(GTK_WINDOW(w), ww, wh);
    app->centered_once = TRUE;
}

// Optional: bypass WM (true HUD). Uncomment signal connect if you want.
// static void on_realize(GtkWidget *w, gpointer ud) {
//     (void)ud;
//     GdkWindow *gw = gtk_widget_get_window(w);
//     if (gw) gdk_window_set_override_redirect(gw, TRUE);
// }

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    apply_css();

    App app = {0};

    GError *err = NULL;
    app.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!app.bus) {
        g_printerr("Failed to get session bus: %s\n", err ? err->message : "unknown");
        return 1;
    }

    app.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);

    // Makes i3 rules easy:
    gtk_window_set_wmclass(GTK_WINDOW(app.win), "playerhud", "playerhud");

    gtk_window_set_decorated(GTK_WINDOW(app.win), FALSE);
    gtk_window_set_keep_above(GTK_WINDOW(app.win), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(app.win), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(app.win), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(app.win), TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(app.win), GDK_WINDOW_TYPE_HINT_NOTIFICATION);

    // If you want WM-bypass HUD: uncomment this:
    // g_signal_connect(app.win, "realize", G_CALLBACK(on_realize), NULL);

    // enable transparency
    gtk_widget_set_app_paintable(app.win, TRUE);
    GdkScreen *screen = gtk_widget_get_screen(app.win);
    GdkVisual *visual = gdk_screen_get_rgba_visual(screen);
    if (visual) gtk_widget_set_visual(app.win, visual);

    g_signal_connect(app.win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(app.win, "key-press-event", G_CALLBACK(on_key), NULL);

    // Center precisely after size is known
    g_signal_connect(app.win, "size-allocate", G_CALLBACK(on_size_allocate), &app);

    // layout
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "card");

    app.img = gtk_image_new();
    gtk_widget_set_size_request(app.img, 220, 220);

    app.lbl_title  = gtk_label_new("—");
    app.lbl_artist = gtk_label_new("—");
    gtk_label_set_xalign(GTK_LABEL(app.lbl_title), 0.0f);
    gtk_label_set_xalign(GTK_LABEL(app.lbl_artist), 0.0f);

    gtk_style_context_add_class(gtk_widget_get_style_context(app.lbl_title),  "title");
    gtk_style_context_add_class(gtk_widget_get_style_context(app.lbl_artist), "artist");

    gtk_box_pack_start(GTK_BOX(card), app.img, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), app.lbl_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), app.lbl_artist, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(app.win), card);
    gtk_widget_show_all(app.win);

    // start polling every 1s
    g_timeout_add(1000, poll_update, &app);
    poll_update(&app);

    gtk_main();

    g_clear_pointer(&app.player_name, g_free);
    if (app.bus) g_object_unref(app.bus);
    return 0;
}
