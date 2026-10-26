// Controllable real Wayland surface tree for cache invalidation/pixel tests.
#include <wayland-client.h>
#include "xdg-shell-client.h"
#include <json-c/json.h>
#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
wl_display* display;
wl_compositor* compositor;
wl_subcompositor* subcompositor;
wl_shm* shm;
xdg_wm_base* shell;
int width = 600, height = 400;
bool configured = false, running = true;

int integer(json_object* object, const char* key, int fallback) {
    json_object* value = nullptr;
    return json_object_object_get_ex(object, key, &value) ? json_object_get_int(value) : fallback;
}

struct Surface {
    wl_surface* surface = nullptr;
    wl_subsurface* child = nullptr;
    wl_surface* parent = nullptr;
    std::string previous;
    std::vector<int> bufferKey;
    int oldWidth = 0, oldHeight = 0, oldBelow = -1;
};
Surface root, child, grandchild;

struct Buffer {
    void* pixels;
    size_t bytes;
};
void released(void* data, wl_buffer* buffer) {
    auto* allocation = static_cast<Buffer*>(data);
    munmap(allocation->pixels, allocation->bytes);
    delete allocation;
    wl_buffer_destroy(buffer);
}
const wl_buffer_listener bufferListener = {released};

void paint(Surface& surface, int w, int h, json_object* state) {
    const int scale = std::clamp(integer(state, "scale", 1), 1, 3);
    const int transform = integer(state, "transform", 0);
    int bw = w * scale, bh = h * scale;
    if (transform % 2) std::swap(bw, bh);
    const size_t bytes = size_t(bw) * bh * 4;
    const int fd = memfd_create("hyprliquid-surface-test", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, bytes) < 0) std::abort();
    void* pixels = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) std::abort();
    const int color = integer(state, "color", 0x2050b0);
    const int alpha = std::clamp(integer(state, "alpha", 210), 0, 255);
    const int phase = integer(state, "phase", 0);
    const int patch = integer(state, "patch", -1);
    const int patchX = std::clamp(integer(state, "patch_x", 0), 0, bw - 1);
    const int patchY = std::clamp(integer(state, "patch_y", 0), 0, bh - 1);
    const int patchW = std::clamp(integer(state, "patch_w", bw / 4), 1, bw - patchX);
    const int patchH = std::clamp(integer(state, "patch_h", bh / 4), 1, bh - patchY);
    auto* output = static_cast<uint32_t*>(pixels);
    for (int y = 0; y < bh; ++y) for (int x = 0; x < bw; ++x) {
        const int rgb = patch >= 0 && x >= patchX && x < patchX + patchW && y >= patchY && y < patchY + patchH ? patch : color;
        const int a = ((y / (24 * scale) + phase) % 2) ? alpha : alpha / 4;
        output[y * bw + x] = (a << 24) | (((rgb >> 16) & 255) * a / 255 << 16) |
            (((rgb >> 8) & 255) * a / 255 << 8) | ((rgb & 255) * a / 255);
    }
    auto* pool = wl_shm_create_pool(shm, fd, bytes);
    auto* buffer = wl_shm_pool_create_buffer(pool, 0, bw, bh, bw * 4, WL_SHM_FORMAT_ARGB8888);
    wl_buffer_add_listener(buffer, &bufferListener, new Buffer{pixels, bytes});
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_surface_set_buffer_scale(surface.surface, scale);
    wl_surface_set_buffer_transform(surface.surface, transform);
    wl_surface_attach(surface.surface, buffer, 0, 0);
    const bool partial = integer(state, "partial", 0) && surface.oldWidth == w && surface.oldHeight == h;
    wl_surface_damage_buffer(surface.surface, partial ? patchX : 0, partial ? patchY : 0, partial ? patchW : bw, partial ? patchH : bh);
}

bool update(Surface& surface, json_object* state, int defaultWidth, int defaultHeight, bool defaultMapped) {
    const std::string serialized = state ? json_object_to_json_string_ext(state, JSON_C_TO_STRING_PLAIN) : "null";
    const int w = std::max(1, integer(state, "width", defaultWidth));
    const int h = std::max(1, integer(state, "height", defaultHeight));
    if (serialized == surface.previous && w == surface.oldWidth && h == surface.oldHeight)
        return false;
    if (surface.child) {
        wl_subsurface_set_position(surface.child, integer(state, "x", 0), integer(state, "y", 0));
        const int below = integer(state, "below", 0);
        if (below != surface.oldBelow) {
            if (below) wl_subsurface_place_below(surface.child, surface.parent);
            else wl_subsurface_place_above(surface.child, surface.parent);
            surface.oldBelow = below;
        }
    }
    const std::vector<int> bufferKey = {w, h, integer(state, "mapped", defaultMapped), integer(state, "scale", 1),
        integer(state, "transform", 0), integer(state, "color", 0x2050b0), integer(state, "alpha", 210),
        integer(state, "phase", 0), integer(state, "patch", -1), integer(state, "patch_x", 0), integer(state, "patch_y", 0),
        integer(state, "patch_w", -1), integer(state, "patch_h", -1)};
    // Position/stacking-only commits deliberately contain no buffer damage.
    if (bufferKey != surface.bufferKey) {
        if (bufferKey[2]) paint(surface, w, h, state);
        else wl_surface_attach(surface.surface, nullptr, 0, 0);
        surface.bufferKey = bufferKey;
    }
    wl_surface_commit(surface.surface);
    surface.previous = serialized;
    surface.oldWidth = w;
    surface.oldHeight = h;
    return true;
}

void global(void*, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    const std::string type(interface);
    if (type == wl_compositor_interface.name)
        compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, std::min(version, 4u)));
    else if (type == wl_subcompositor_interface.name)
        subcompositor = static_cast<wl_subcompositor*>(wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
    else if (type == wl_shm_interface.name)
        shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    else if (type == xdg_wm_base_interface.name) {
        shell = static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
        static const xdg_wm_base_listener listener = {[](void*, xdg_wm_base* base, uint32_t serial) { xdg_wm_base_pong(base, serial); }};
        xdg_wm_base_add_listener(shell, &listener, nullptr);
    }
}
const wl_registry_listener registryListener = {global, [](void*, wl_registry*, uint32_t) {}};
const xdg_surface_listener surfaceListener = {[](void*, xdg_surface* surface, uint32_t serial) {
    xdg_surface_ack_configure(surface, serial);
    configured = true;
}};
const xdg_toplevel_listener toplevelListener = {
    [](void*, xdg_toplevel*, int32_t w, int32_t h, wl_array*) { if (w > 0 && h > 0) { width = w; height = h; } },
    [](void*, xdg_toplevel*) { running = false; }, nullptr, nullptr
};
}

int main(int argc, char** argv) {
    if (argc != 3) { std::cerr << "usage: subsurface-test APP_ID CONTROL_JSON\n"; return 2; }
    display = wl_display_connect(nullptr);
    if (!display) return 1;
    auto* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registryListener, nullptr);
    if (wl_display_roundtrip(display) < 0 || !compositor || !subcompositor || !shm || !shell) return 1;
    root.surface = wl_compositor_create_surface(compositor);
    auto* xdgSurface = xdg_wm_base_get_xdg_surface(shell, root.surface);
    xdg_surface_add_listener(xdgSurface, &surfaceListener, nullptr);
    auto* toplevel = xdg_surface_get_toplevel(xdgSurface);
    xdg_toplevel_add_listener(toplevel, &toplevelListener, nullptr);
    xdg_toplevel_set_app_id(toplevel, argv[1]);
    xdg_toplevel_set_title(toplevel, argv[1]);
    child.surface = wl_compositor_create_surface(compositor);
    child.parent = root.surface;
    child.child = wl_subcompositor_get_subsurface(subcompositor, child.surface, root.surface);
    grandchild.surface = wl_compositor_create_surface(compositor);
    grandchild.parent = child.surface;
    grandchild.child = wl_subcompositor_get_subsurface(subcompositor, grandchild.surface, child.surface);
    wl_surface_commit(root.surface);
    while (running && wl_display_roundtrip(display) >= 0) {
        std::ifstream file(argv[2]);
        const std::string text((std::istreambuf_iterator<char>(file)), {});
        auto* state = json_tokener_parse(text.c_str());
        if (configured && state) {
            json_object *rootState = nullptr, *childState = nullptr, *grandchildState = nullptr;
            json_object_object_get_ex(state, "root", &rootState);
            json_object_object_get_ex(state, "child", &childState);
            json_object_object_get_ex(state, "grandchild", &grandchildState);
            const bool grandchildChanged = update(grandchild, grandchildState, 80, 60, false);
            const bool childChanged = update(child, childState, width, height, true);
            if (grandchildChanged && !childChanged) wl_surface_commit(child.surface);
            const bool rootChanged = update(root, rootState, width, height, true);
            if ((childChanged || grandchildChanged) && !rootChanged) wl_surface_commit(root.surface);
            if (wl_display_roundtrip(display) < 0) return 1;
            std::ofstream(std::string(argv[2]) + ".ack") << text;
        }
        if (state) json_object_put(state);
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    wl_display_disconnect(display);
}
