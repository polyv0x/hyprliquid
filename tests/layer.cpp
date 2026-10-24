// Deterministic translucent layer-shell surface for the disposable compositor.
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <string_view>

int main(int argc, char** argv) {
    bool animate = false, bottom = false;
    for (int i = 1; i < argc; ++i) {
        animate |= std::string_view(argv[i]) == "--animate";
        bottom |= std::string_view(argv[i]) == "--bottom";
    }
    gtk_init();
    auto* window = GTK_WINDOW(gtk_window_new());
    gtk_layer_init_for_window(window);
    gtk_layer_set_namespace(window, "hyprliquid-vram-test");
    gtk_layer_set_layer(window, bottom ? GTK_LAYER_SHELL_LAYER_BOTTOM : GTK_LAYER_SHELL_LAYER_TOP);
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
    unsigned phase = 0;
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(area), 120);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), [](GtkDrawingArea*, cairo_t* cr, int width, int, gpointer state) {
        const auto phase = *static_cast<unsigned*>(state);
        for (int x = 16; x < width - 16; x += 96) {
            cairo_set_source_rgba(cr, .2, .4, phase % 2 ? .2 : .8, .3);
            cairo_rectangle(cr, x, 12, 72, 96);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, .9, .9, .9, 1);
            cairo_rectangle(cr, x + 12, 40, 48, 12);
            cairo_fill(cr);
        }
    }, &phase, nullptr);
    struct SAnimation { GtkWidget* area; unsigned* phase; } animation{area, &phase};
    if (animate) g_timeout_add(16, [](gpointer data) -> gboolean {
        auto& animation = *static_cast<SAnimation*>(data);
        ++*animation.phase;
        gtk_widget_queue_draw(animation.area);
        return G_SOURCE_CONTINUE;
    }, &animation);
    gtk_window_set_child(window, area);
    gtk_window_present(window);
    g_main_loop_run(g_main_loop_new(nullptr, false));
}
