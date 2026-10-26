// Test-only raw capture. Load only in the disposable compositor, never the
// desktop. Captures the internal target before output conversion; native
// readback preserves both 10-bit SDR and RGBA16F values without quantization.
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <json-c/json.h>
#include <chrono>
#include <algorithm>
#include <deque>
#include <fstream>
#include <filesystem>
#include <dlfcn.h>
#include "../src/PixelTransfer.hpp"
#include "../src/RenderSafety.hpp"
#include <hyprutils/utils/ScopeGuard.hpp>

namespace {
HANDLE handle;
Render::IHyprRenderer* renderer = nullptr;
SP<SHyprCtlCommand> captureCommand, timingCommand, snapshotCommand;
Hyprutils::Signal::CHyprSignalListener listener;
std::string pending;
std::string pendingMonitor;
std::deque<double> times;
std::chrono::steady_clock::time_point begun;
uint64_t frames = 0;

class CCapture : public IPassElement {
  public:
    explicit CCapture(std::string path, SP<Render::IFramebuffer> snapshot = nullptr) : path(std::move(path)), snapshot(std::move(snapshot)) {}
    std::vector<UP<IPassElement>> draw() override {
        const auto fb = snapshot ? snapshot : renderer->m_renderData.currentFB;
        if (!fb || !RenderSafety::dimension(fb->m_size.x) || !RenderSafety::dimension(fb->m_size.y)) return {};
        auto* glFramebuffer = dynamic_cast<Render::GL::CGLFramebuffer*>(fb.get());
        if (!glFramebuffer) return {};
        const int width = fb->m_size.x, height = fb->m_size.y;
        GLint read = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
        Hyprutils::Utils::CScopeGuard restoreRead([&] { glBindFramebuffer(GL_READ_FRAMEBUFFER, read); });
        CPixelTransferGuard<false> pack;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, glFramebuffer->getFBID());
        // Record the implementation's native read format/type alongside the
        // bytes so no channel precision is discarded, including for FP16.
        GLint type = 0, format = 0;
        glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &type);
        glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &format);
        const int pixelBytes = type == GL_FLOAT || type == GL_UNSIGNED_INT || type == GL_INT ? 16 :
                               type == GL_HALF_FLOAT || type == GL_UNSIGNED_SHORT || type == GL_SHORT ? 8 : 4;
        if (size_t(width) > std::vector<unsigned char>().max_size() / height / pixelBytes)
            return {};
        std::vector<unsigned char> pixels(size_t(width) * height * pixelBytes);
        while (glGetError() != GL_NO_ERROR) {}
        glReadPixels(0, 0, width, height, format, type, pixels.data());
        const auto error = glGetError();
        auto* metadata = json_object_new_object();
        auto add = [&](const char* key, int64_t n) { json_object_object_add(metadata, key, json_object_new_int64(n)); };
        add("width", width); add("height", height); add("drm_format", fb->m_drmFormat);
        add("gl_type", type); add("gl_format", format); add("pixel_bytes", pixelBytes); add("gl_error", error);
        auto* damage = json_object_new_array();
        renderer->m_renderData.damage.forEachRect([&](const auto& r) {
            auto* rect = json_object_new_array();
            for (const auto v : {r.x1, r.y1, r.x2, r.y2}) json_object_array_add(rect, json_object_new_int(v));
            json_object_array_add(damage, rect);
        });
        json_object_object_add(metadata, "damage", damage);
        auto* nativeDamage = json_object_new_array();
        const auto monitor = renderer->m_renderData.pMonitor.lock();
        if (monitor) {
            const auto transform = Math::wlTransformToHyprutils(Math::invertTransform(monitor->m_transform));
            renderer->m_renderData.damage.copy().transform(transform, monitor->m_transformedSize.x, monitor->m_transformedSize.y)
                .intersect(CBox(0, 0, width, height)).forEachRect([&](const auto& r) {
                    auto* rect = json_object_new_array();
                    for (const auto v : {r.x1, r.y1, r.x2, r.y2}) json_object_array_add(rect, json_object_new_int(v));
                    json_object_array_add(nativeDamage, rect);
                });
        }
        json_object_object_add(metadata, "native_damage", nativeDamage);
        const auto color = fb->imageDescription();
        json_object_object_add(metadata, "color_description", json_object_new_string(color ? std::format("{}", color->value()).c_str() : "none"));
        if (!error) {
            std::ofstream data(path + ".raw", std::ios::binary);
            data.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
        }
        std::ofstream(path + ".json") << json_object_to_json_string_ext(metadata, JSON_C_TO_STRING_PRETTY) << '\n';
        json_object_put(metadata);
        return {};
    }
    bool needsLiveBlur() override { return false; }
    bool needsPrecomputeBlur() override { return false; }
    // Observe the normal pass; disabling culling here hides cache/damage bugs.
    bool disableSimplification() override { return false; }
    const char* passName() override { return "CHyprliquidTestCapture"; }
    ePassElementType type() override { return EK_CUSTOM; }
  private:
    std::string path;
    SP<Render::IFramebuffer> snapshot;
};
}

APICALL EXPORT std::string PLUGIN_API_VERSION() { return HYPRLAND_API_VERSION; }
APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE h) {
    handle = h;
    if (std::string(__hyprland_api_get_hash()) != __hyprland_api_get_client_hash()) throw std::runtime_error("Capture ABI mismatch");
    // The fixture's version script hides its helpers to prevent interposition
    // on comparison plugins. It also hides Hyprland's inline globals, so our
    // local g_pHyprRenderer is empty. Borrow the host's owner explicitly; never
    // export numerical helpers just to make that inline global resolve.
    const auto* owner = static_cast<const decltype(g_pHyprRenderer)*>(dlsym(RTLD_DEFAULT, "g_pHyprRenderer"));
    if (!owner || !*owner) throw std::runtime_error("Capture host renderer unavailable");
    renderer = owner->get();
    snapshotCommand = HyprlandAPI::registerHyprCtlCommand(handle, {"hyprliquid-test:snapshot", false,
        [](eHyprCtlOutputFormat, std::string request) {
            const auto first = request.find(' '), second = request.find(' ', first + 1);
            if (first == std::string::npos || second == std::string::npos) return std::string("missing class and path");
            const auto app = request.substr(first + 1, second - first - 1);
            const std::filesystem::path path = request.substr(second + 1);
            std::error_code error;
            if (!path.is_absolute() || !std::filesystem::is_directory(path.parent_path(), error)) return std::string("invalid artifact path");
            for (const auto& window : Desktop::windowState()->windows()) if (window->m_class == app && window->m_isMapped) {
                auto snapshot = renderer->makeSnapshotFB(window);
                if (!snapshot) return std::string("snapshot unavailable");
                CCapture(path, snapshot).draw();
                return std::string("ok");
            }
            return std::string("missing window");
        }});
    captureCommand = HyprlandAPI::registerHyprCtlCommand(handle, {"hyprliquid-test:capture", false,
        [](eHyprCtlOutputFormat, std::string request) {
            const auto at = request.find(' ');
            if (at == std::string::npos) return std::string("missing absolute artifact path");
            auto argument = request.substr(at + 1);
            const bool nextFrame = argument.starts_with("next ");
            if (nextFrame) argument.erase(0, 5);
            std::string monitorName;
            if (argument.starts_with("monitor ")) {
                const auto end = argument.find(' ', 8);
                if (end == std::string::npos) return std::string("missing monitor and artifact path");
                monitorName = argument.substr(8, end - 8);
                argument.erase(0, end + 1);
                if (std::ranges::none_of(State::monitorState()->monitors(), [&](const auto& monitor) {
                        return monitor->m_name == monitorName;
                    })) return std::string("missing monitor");
            }
            const std::filesystem::path path = argument;
            std::error_code error;
            if (!path.is_absolute() || !std::filesystem::is_directory(path.parent_path(), error)) return std::string("invalid artifact path");
            pending = path;
            pendingMonitor = std::move(monitorName);
            if (!nextFrame) for (const auto& monitor : State::monitorState()->monitors()) {
                if (!pendingMonitor.empty() && monitor->m_name != pendingMonitor) continue;
                renderer->damageMonitor(monitor);
                monitor->scheduleFrame();
            }
            return std::string("ok");
        }});
    timingCommand = HyprlandAPI::registerHyprCtlCommand(handle, {"hyprliquid-test:timing", false,
        [](eHyprCtlOutputFormat, std::string) {
            auto* result = json_object_new_object();
            auto* samples = json_object_new_array();
            for (double ms : times) json_object_array_add(samples, json_object_new_double(ms));
            json_object_object_add(result, "cpu_frame_ms", samples);
            json_object_object_add(result, "frames", json_object_new_uint64(frames));
            std::string text = json_object_to_json_string_ext(result, JSON_C_TO_STRING_PLAIN);
            json_object_put(result);
            times.clear();
            return text;
        }});
    listener = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) {
        // Cursor visibility is asynchronous and can change between identical
        // scene captures. Exclude it before this frame's passes are queued.
        if (stage == RENDER_PRE && !pending.empty()) renderer->setCursorHidden(true);
        if (stage == RENDER_BEGIN) begun = std::chrono::steady_clock::now();
        if (stage == RENDER_POST) {
            ++frames;
            times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begun).count());
            if (times.size() > 1024) times.pop_front();
        }
        if (stage == RENDER_LAST_MOMENT && !pending.empty() &&
            (pendingMonitor.empty() || renderer->m_renderData.pMonitor->m_name == pendingMonitor)) {
            renderer->addPassElement(makeUnique<CCapture>(std::exchange(pending, {})));
            pendingMonitor.clear();
        }
    });
    return {"hyprliquid-test-capture", "Internal framebuffer comparison fixture", "hyprliquid", "1"};
}
APICALL EXPORT void PLUGIN_EXIT() {
    listener.reset();
    if (renderer) renderer->m_renderPass.removeAllOfType("CHyprliquidTestCapture");
    HyprlandAPI::unregisterHyprCtlCommand(handle, captureCommand);
    HyprlandAPI::unregisterHyprCtlCommand(handle, timingCommand);
    HyprlandAPI::unregisterHyprCtlCommand(handle, snapshotCommand);
    captureCommand.reset(); timingCommand.reset(); pending.clear(); pendingMonitor.clear(); times.clear();
    snapshotCommand.reset();
    renderer = nullptr;
}
