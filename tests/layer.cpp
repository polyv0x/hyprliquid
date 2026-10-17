// Deterministic translucent layer-shell surface for the disposable compositor.
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>

int main() {
    gtk_init();
    auto* window = GTK_WINDOW(gtk_window_new());
    gtk_layer_init_for_window(window);
    gtk_layer_set_namespace(window, "hyprliquid-vram-test");
    gtk_layer_set_layer(window, GTK_LAYER_SHELL_LAYER_TOP);
    gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_TOP, true);
    gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_LEFT, true);
    gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_RIGHT, true);
    gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_TOP, 80);
    gtk_layer_set_exclusive_zone(window, 0);
    gtk_window_set_default_size(window, 600, 120);
    auto* css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, "window { background: transparent; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    auto* area = gtk_drawing_area_new();
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(area), 120);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), [](GtkDrawingArea*, cairo_t* cr, int width, int, gpointer) {
        for (int x = 16; x < width - 16; x += 96) {
            cairo_set_source_rgba(cr, .2, .4, .8, .3);
            cairo_rectangle(cr, x, 12, 72, 96);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, .9, .9, .9, 1);
            cairo_rectangle(cr, x + 12, 40, 48, 12);
            cairo_fill(cr);
        }
    }, nullptr, nullptr);
    gtk_window_set_child(window, area);
    gtk_window_present(window);
    g_main_loop_run(g_main_loop_new(nullptr, false));
}
