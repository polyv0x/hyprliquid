// One-shot native workspace capture. Unlike output screenshots, this exercises
// Hyprland's isolated workspace renderer and its no_screen_share filtering.
#include "ext-workspace-v1.h"
#include "ext-image-capture-source-v1.h"
#include "ext-image-copy-capture-v1.h"
#include "hyprland-workspace-image-capture-source-v1.h"
#include <wayland-client.h>
#include <sys/mman.h>
#include <unistd.h>
#include <signal.h>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

struct Capture {
    wl_display* display = nullptr;
    wl_shm* shm = nullptr;
    ext_workspace_manager_v1* workspaces = nullptr;
    ext_workspace_handle_v1* workspace = nullptr;
    hyprland_workspace_image_capture_source_manager_v1* sources = nullptr;
    ext_image_copy_capture_manager_v1* captures = nullptr;
    const char* name = nullptr;
    uint32_t width = 0, height = 0, format = UINT32_MAX, transform = UINT32_MAX;
    bool constraints = false, ready = false, failed = false;
    void* pixels = MAP_FAILED;
    size_t bytes = 0;
    int fd = -1;

    ~Capture() {
        if (display) wl_display_disconnect(display);
        if (pixels != MAP_FAILED) munmap(pixels, bytes);
        if (fd >= 0) close(fd);
    }
};

static Capture& state(void* data) { return *static_cast<Capture*>(data); }
static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

static const ext_workspace_handle_v1_listener workspaceListener = {
    .id = [](void*, auto*, const char*) {},
    .name = [](void* data, auto* workspace, const char* name) {
        if (!std::strcmp(state(data).name, name)) state(data).workspace = workspace;
    },
    .coordinates = [](void*, auto*, wl_array*) {},
    .state = [](void*, auto*, uint32_t) {},
    .capabilities = [](void*, auto*, uint32_t) {},
    .removed = [](void* data, auto* workspace) {
        if (state(data).workspace == workspace) state(data).failed = true;
        ext_workspace_handle_v1_destroy(workspace);
    },
};

static const ext_workspace_manager_v1_listener workspaceManagerListener = {
    // Group geometry is irrelevant when selecting a workspace by name.
    .workspace_group = [](void*, auto*, auto* group) { ext_workspace_group_handle_v1_destroy(group); },
    .workspace = [](void* data, auto*, auto* workspace) {
        ext_workspace_handle_v1_add_listener(workspace, &workspaceListener, data);
    },
    .done = [](void*, auto*) {},
    .finished = [](void* data, auto*) { state(data).failed = true; },
};

static const wl_shm_listener shmListener = {.format = [](void*, auto*, uint32_t) {}};
static const wl_registry_listener registryListener = {
    .global = [](void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t) {
        auto& client = state(data);
        if (!std::strcmp(interface, wl_shm_interface.name)) {
            client.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
            wl_shm_add_listener(client.shm, &shmListener, data);
        } else if (!std::strcmp(interface, ext_workspace_manager_v1_interface.name)) {
            client.workspaces = static_cast<ext_workspace_manager_v1*>(
                wl_registry_bind(registry, name, &ext_workspace_manager_v1_interface, 1));
            ext_workspace_manager_v1_add_listener(client.workspaces, &workspaceManagerListener, data);
        } else if (!std::strcmp(interface, hyprland_workspace_image_capture_source_manager_v1_interface.name)) {
            client.sources = static_cast<hyprland_workspace_image_capture_source_manager_v1*>(
                wl_registry_bind(registry, name, &hyprland_workspace_image_capture_source_manager_v1_interface, 1));
        } else if (!std::strcmp(interface, ext_image_copy_capture_manager_v1_interface.name)) {
            client.captures = static_cast<ext_image_copy_capture_manager_v1*>(
                wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, 1));
        }
    },
    .global_remove = [](void*, auto*, uint32_t) {},
};

static const ext_image_copy_capture_session_v1_listener sessionListener = {
    .buffer_size = [](void* data, auto*, uint32_t width, uint32_t height) {
        state(data).width = width;
        state(data).height = height;
    },
    .shm_format = [](void* data, auto*, uint32_t format) {
        if (format == WL_SHM_FORMAT_ARGB8888 || format == WL_SHM_FORMAT_XRGB8888)
            state(data).format = format;
    },
    .dmabuf_device = [](void*, auto*, wl_array*) {},
    .dmabuf_format = [](void*, auto*, uint32_t, wl_array*) {},
    .done = [](void* data, auto*) { state(data).constraints = true; },
    .stopped = [](void* data, auto*) { state(data).failed = true; },
};

static const ext_image_copy_capture_frame_v1_listener frameListener = {
    .transform = [](void* data, auto*, uint32_t transform) { state(data).transform = transform; },
    .damage = [](void*, auto*, int32_t, int32_t, int32_t, int32_t) {},
    .presentation_time = [](void*, auto*, uint32_t, uint32_t, uint32_t) {},
    .ready = [](void* data, auto*) { state(data).ready = true; },
    .failed = [](void* data, auto*, uint32_t reason) {
        std::cerr << "Workspace capture failed: " << reason << '\n';
        state(data).failed = true;
    },
};

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: workspace-capture-test WORKSPACE OUTPUT.ppm\n";
        return 2;
    }
    signal(SIGALRM, [](int) {
        constexpr char message[] = "Workspace capture timed out\n";
        (void)write(STDERR_FILENO, message, sizeof(message) - 1);
        _exit(124);
    });
    alarm(12);
    try {
        Capture client;
        client.name = argv[1];
        client.display = wl_display_connect(nullptr);
        require(client.display, "Cannot connect to disposable compositor");
        auto* registry = wl_display_get_registry(client.display);
        wl_registry_add_listener(registry, &registryListener, &client);
        require(wl_display_roundtrip(client.display) >= 0, "Registry roundtrip failed");
        if (!client.sources) {
            std::cerr << "Workspace capture protocol unavailable\n";
            return 77;
        }
        require(client.shm && client.workspaces && client.captures, "Required capture protocol unavailable");
        require(wl_display_roundtrip(client.display) >= 0, "Workspace roundtrip failed");
        require(client.workspace && !client.failed, "Workspace not found");
        auto* source = hyprland_workspace_image_capture_source_manager_v1_create_source(
            client.sources, client.workspace,
            HYPRLAND_WORKSPACE_IMAGE_CAPTURE_SOURCE_MANAGER_V1_CAPTURE_MODE_EVERYTHING);
        auto* session = ext_image_copy_capture_manager_v1_create_session(client.captures, source, 0);
        ext_image_copy_capture_session_v1_add_listener(session, &sessionListener, &client);
        while (!client.constraints && !client.failed)
            require(wl_display_dispatch(client.display) >= 0, "Capture constraints disconnected");
        require(!client.failed, "Capture session stopped");
        require(client.width && client.height && client.width <= 8192 && client.height <= 8192,
                "Unsupported capture dimensions");
        require(client.format != UINT32_MAX, "No supported 8-bit shared-memory format");
        client.bytes = size_t(client.width) * client.height * 4;
        client.fd = memfd_create("hyprliquid-workspace-capture", MFD_CLOEXEC);
        require(client.fd >= 0 && ftruncate(client.fd, client.bytes) == 0, "Capture buffer allocation failed");
        client.pixels = mmap(nullptr, client.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, client.fd, 0);
        require(client.pixels != MAP_FAILED, "Capture buffer mapping failed");
        auto* pool = wl_shm_create_pool(client.shm, client.fd, client.bytes);
        auto* buffer = wl_shm_pool_create_buffer(pool, 0, client.width, client.height, client.width * 4, client.format);
        wl_shm_pool_destroy(pool);
        auto* frame = ext_image_copy_capture_session_v1_create_frame(session);
        ext_image_copy_capture_frame_v1_add_listener(frame, &frameListener, &client);
        ext_image_copy_capture_frame_v1_attach_buffer(frame, buffer);
        ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0, client.width, client.height);
        ext_image_copy_capture_frame_v1_capture(frame);
        while (!client.ready && !client.failed)
            require(wl_display_dispatch(client.display) >= 0, "Capture disconnected");
        require(client.ready && !client.failed, "Capture did not produce a frame");
        // This focused fixture uses the normal-transform disposable output.
        require(client.transform == WL_OUTPUT_TRANSFORM_NORMAL, "Unexpected capture transform");
        std::ofstream output(argv[2], std::ios::binary);
        output << "P6\n" << client.width << ' ' << client.height << "\n255\n";
        std::vector<char> row(client.width * 3);
        const auto* pixels = static_cast<const uint32_t*>(client.pixels);
        for (uint32_t y = 0; y < client.height; ++y) {
            for (uint32_t x = 0; x < client.width; ++x) {
                const auto pixel = pixels[size_t(y) * client.width + x];
                row[x * 3] = pixel >> 16;
                row[x * 3 + 1] = pixel >> 8;
                row[x * 3 + 2] = pixel;
            }
            output.write(row.data(), row.size());
        }
        output.close();
        require(bool(output), "Cannot write capture");
        std::cout << "{\"width\":" << client.width << ",\"height\":" << client.height
                  << ",\"format\":" << client.format << ",\"transform\":" << client.transform << "}\n";
        ext_image_copy_capture_frame_v1_destroy(frame);
        wl_buffer_destroy(buffer);
        ext_image_copy_capture_session_v1_destroy(session);
        ext_image_capture_source_v1_destroy(source);
        wl_display_flush(client.display);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
