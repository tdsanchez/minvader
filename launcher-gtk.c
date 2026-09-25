#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <libgen.h>

static GtkWidget *port_entry;
static GtkWidget *dir_label;
static GtkWidget *warm_check;
static GtkWidget *browse_btn;
static GtkWidget *start_btn;
static GtkWidget *stop_btn;
static GtkWidget *status_label;
static char chosen_dir[4096];
static GPid child_pid;
static int child_running;

static char *core_binary_path(const char *argv0) {
    static char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        char *dir = dirname(buf);
        snprintf(buf, sizeof(buf), "%s/minvader", dir);
    } else {
        snprintf(buf, sizeof(buf), "./minvader");
    }
    return buf;
}

static void on_child_exit(GPid pid, gint status, gpointer data) {
    g_spawn_close_pid(pid);
    child_running = 0;
    child_pid = 0;
    gtk_label_set_text(GTK_LABEL(status_label), "Server stopped");
    gtk_widget_set_sensitive(start_btn, TRUE);
    gtk_widget_set_sensitive(stop_btn, FALSE);
    gtk_widget_set_sensitive(port_entry, TRUE);
    gtk_widget_set_sensitive(warm_check, TRUE);
    if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(warm_check)))
        gtk_widget_set_sensitive(browse_btn, TRUE);
}

static void warm_toggled(GtkWidget *w, gpointer data) {
    gboolean warm = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(warm_check));
    gtk_widget_set_sensitive(browse_btn, !warm);
    if (warm)
        gtk_label_set_text(GTK_LABEL(dir_label), "(using cache DB)");
    else
        gtk_label_set_text(GTK_LABEL(dir_label),
            chosen_dir[0] ? chosen_dir : "(none selected)");
}

static void browse_clicked(GtkWidget *w, gpointer data) {
    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Choose a directory to serve",
        GTK_WINDOW(gtk_widget_get_toplevel(w)),
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open", GTK_RESPONSE_ACCEPT,
        NULL);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *folder = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (folder) {
            snprintf(chosen_dir, sizeof(chosen_dir), "%s", folder);
            gtk_label_set_text(GTK_LABEL(dir_label), chosen_dir);
            g_free(folder);
        }
    }
    gtk_widget_destroy(dialog);
}

static void start_clicked(GtkWidget *w, gpointer data) {
    const char *port = gtk_entry_get_text(GTK_ENTRY(port_entry));
    if (!port || !port[0]) {
        gtk_label_set_text(GTK_LABEL(status_label), "Error: port is required");
        return;
    }
    gboolean warm = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(warm_check));
    if (!warm && !chosen_dir[0]) {
        gtk_label_set_text(GTK_LABEL(status_label),
            "Error: choose a directory or enable warm start");
        return;
    }

    char *binary = core_binary_path(NULL);
    if (access(binary, X_OK) != 0) {
        char msg[4200];
        snprintf(msg, sizeof(msg), "Error: %s not found", binary);
        gtk_label_set_text(GTK_LABEL(status_label), msg);
        return;
    }

    GError *err = NULL;
    gboolean ok;

    if (warm) {
        char port_arg[64];
        snprintf(port_arg, sizeof(port_arg), "--port=%s", port);
        char *argv[] = { binary, "--warm", port_arg, NULL };
        ok = g_spawn_async(NULL, argv, NULL,
            G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &child_pid, &err);
    } else {
        char cmd[8192];
        snprintf(cmd, sizeof(cmd),
            "find '%s' -type f | '%s' --stdin --port=%s",
            chosen_dir, binary, port);
        char *argv[] = { "/bin/bash", "-c", cmd, NULL };
        ok = g_spawn_async(NULL, argv, NULL,
            G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &child_pid, &err);
    }

    if (!ok) {
        char msg[4200];
        snprintf(msg, sizeof(msg), "Launch failed: %s",
            err ? err->message : "unknown");
        gtk_label_set_text(GTK_LABEL(status_label), msg);
        if (err) g_error_free(err);
        return;
    }

    child_running = 1;
    g_child_watch_add(child_pid, on_child_exit, NULL);

    char msg[256];
    snprintf(msg, sizeof(msg), "Starting on port %s…", port);
    gtk_label_set_text(GTK_LABEL(status_label), msg);
    gtk_widget_set_sensitive(start_btn, FALSE);
    gtk_widget_set_sensitive(stop_btn, TRUE);
    gtk_widget_set_sensitive(port_entry, FALSE);
    gtk_widget_set_sensitive(warm_check, FALSE);
    gtk_widget_set_sensitive(browse_btn, FALSE);
}

static void stop_clicked(GtkWidget *w, gpointer data) {
    if (child_running && child_pid > 0)
        kill(child_pid, SIGTERM);
}

static gboolean on_delete(GtkWidget *w, GdkEvent *e, gpointer data) {
    if (child_running && child_pid > 0) {
        kill(child_pid, SIGTERM);
        usleep(500000);
    }
    gtk_main_quit();
    return FALSE;
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "minvader");
    gtk_window_set_default_size(GTK_WINDOW(win), 480, 200);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    g_signal_connect(win, "delete-event", G_CALLBACK(on_delete), NULL);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 15);
    gtk_container_add(GTK_CONTAINER(win), grid);

    /* Row 0: Port + Warm */
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Port:"), 0, 0, 1, 1);
    port_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(port_entry), "9090");
    gtk_entry_set_width_chars(GTK_ENTRY(port_entry), 8);
    gtk_grid_attach(GTK_GRID(grid), port_entry, 1, 0, 1, 1);
    warm_check = gtk_check_button_new_with_label("Warm start (use existing cache)");
    g_signal_connect(warm_check, "toggled", G_CALLBACK(warm_toggled), NULL);
    gtk_grid_attach(GTK_GRID(grid), warm_check, 2, 0, 2, 1);

    /* Row 1: Directory */
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Directory:"), 0, 1, 1, 1);
    dir_label = gtk_label_new("(none selected)");
    gtk_label_set_ellipsize(GTK_LABEL(dir_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(dir_label), 35);
    gtk_label_set_xalign(GTK_LABEL(dir_label), 0);
    gtk_grid_attach(GTK_GRID(grid), dir_label, 1, 1, 2, 1);
    browse_btn = gtk_button_new_with_label("Browse…");
    g_signal_connect(browse_btn, "clicked", G_CALLBACK(browse_clicked), NULL);
    gtk_grid_attach(GTK_GRID(grid), browse_btn, 3, 1, 1, 1);

    /* Row 2: Start / Stop */
    GtkWidget *btn_box = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout(GTK_BUTTON_BOX(btn_box), GTK_BUTTONBOX_CENTER);
    gtk_box_set_spacing(GTK_BOX(btn_box), 12);
    start_btn = gtk_button_new_with_label("Start");
    g_signal_connect(start_btn, "clicked", G_CALLBACK(start_clicked), NULL);
    gtk_container_add(GTK_CONTAINER(btn_box), start_btn);
    stop_btn = gtk_button_new_with_label("Stop");
    g_signal_connect(stop_btn, "clicked", G_CALLBACK(stop_clicked), NULL);
    gtk_widget_set_sensitive(stop_btn, FALSE);
    gtk_container_add(GTK_CONTAINER(btn_box), stop_btn);
    gtk_grid_attach(GTK_GRID(grid), btn_box, 0, 2, 4, 1);

    /* Row 3: Status */
    status_label = gtk_label_new("Ready");
    gtk_label_set_xalign(GTK_LABEL(status_label), 0);
    gtk_grid_attach(GTK_GRID(grid), status_label, 0, 3, 4, 1);

    gtk_widget_show_all(win);
    gtk_main();
    return 0;
}
