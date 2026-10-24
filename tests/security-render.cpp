// Test-only fault injection, loaded only by scripts/test-nested.py.
#include "../src/GlassRenderer.hpp"
#include "../src/RenderBuffers.hpp"

#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <climits>

namespace {
HANDLE handle;
SP<SHyprCtlCommand> command;
Hyprutils::Signal::CHyprSignalListener listener;
std::string result = "idle";
bool pending = false;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class CFailedAllocation : public Render::GL::CGLFramebuffer {
  public:
    bool alloc(int, int, DRMFormat) override {
        ++calls;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(1, 2, 3, 4);
        return false;
    }
    int calls = 0;
};

class CSecurityPass : public IPassElement {
  public:
    std::vector<UP<IPassElement>> draw() override {
        const auto source = g_pHyprRenderer->m_renderData.currentFB;
        GLint read = 0, draw = 0, viewport[4]{};
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
        glGetIntegerv(GL_VIEWPORT, viewport);
        try {
            CRenderBuffers buffers;
            auto failed = makeShared<CFailedAllocation>();
            SP<Render::IFramebuffer> target = failed;
            require(!buffers.prepare(target, 0, 20, DRM_FORMAT_ARGB8888, "test"), "accepted zero dimension");
            require(!buffers.prepare(target, INT_MAX, 20, DRM_FORMAT_ARGB8888, "test"), "accepted overflowing dimension");
            GLint limit = 0;
            glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
            require(!buffers.prepare(target, limit + 1, 20, DRM_FORMAT_ARGB8888, "test"), "accepted oversized texture");
            require(!buffers.prepare(target, 20, 20, DRM_FORMAT_INVALID, "test"), "accepted invalid format");
            require(failed->calls == 0, "invalid size reached native allocator");
            require(!buffers.prepare(target, 20, 20, DRM_FORMAT_ARGB8888, "test"), "lost allocation failure");
            GLint restored = 0, restoredViewport[4]{};
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &restored);
            glGetIntegerv(GL_VIEWPORT, restoredViewport);
            require(restored == draw, "failed allocation changed framebuffer binding");
            require(std::equal(std::begin(viewport), std::end(viewport), restoredViewport), "failed allocation changed viewport");
            Vector2D padding;
            require(!GlassRenderer::sampleBackground(target, source, CBox{0, 0, 2147483647.0, 100}, padding), "accepted oversized sample");
            require(!GlassRenderer::blurBackground(source, target, 5, source), "blur ignored failed allocation");
            require(!GlassRenderer::prepareMaterialBackdrop(source, target, source), "material reduction ignored failed allocation");
            glGetIntegerv(GL_VIEWPORT, restoredViewport);
            require(std::equal(std::begin(viewport), std::end(viewport), restoredViewport), "failed material reduction changed viewport");
            SP<Render::IFramebuffer> invalid;
            require(!GlassRenderer::prepareMaterialBackdrop(invalid, target, source), "accepted missing material source");
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &restored);
            require(restored == GLint(dynamic_cast<Render::GL::CGLFramebuffer*>(source.get())->getFBID()), "failed blur did not restore target");
            require(CRenderBuffers::bytes(INT_MAX, INT_MAX, DRM_FORMAT_ARGB8888) == uint64_t(INT_MAX) * INT_MAX * 4, "byte accounting overflowed");
            {
                auto cached = buffers.acquireBackdrop(20, 20, source, "receiver", {1});
                require(!cached->contentValid, "new backdrop was already valid");
                require(buffers.prepare(cached->sharp, 20, 20, source->m_drmFormat, "test-cache"), "backdrop allocation failed");
                require(buffers.prepare(cached->blurred, 10, 10, source->m_drmFormat, "test-cache-blur"), "cached blur allocation failed");
                cached->contentValid = true;
                auto nested = buffers.acquireBackdrop(20, 20, source, "receiver", {1});
                require(nested->cacheOwner.empty() && nested->sharp != cached->sharp, "overwrote a leased backdrop");
            }
            {
                auto cached = buffers.acquireBackdrop(20, 20, source, "receiver", {1});
                require(cached->contentValid && cached->sharp->isAllocated(), "lost a reusable backdrop");
            }
            {
                auto changed = buffers.acquireBackdrop(20, 20, source, "receiver", {2});
                require(!changed->contentValid, "reused a stale backdrop generation");
                changed->contentValid = true;
            }
            {
                auto resized = buffers.acquireBackdrop(10, 10, source, "receiver", {2});
                require(!resized->contentValid && !resized->blurred, "zero-blur resize retained unreserved storage");
                resized->contentValid = true;
            }
            for (size_t i = 1; i <= CRenderBuffers::BACKDROP_RETAINED_PAIRS; ++i) {
                auto cached = buffers.acquireBackdrop(20, 20, source, std::to_string(i), {1});
                require(cached->cacheOwner.empty() == (i == CRenderBuffers::BACKDROP_RETAINED_PAIRS), "backdrop pair limit failed");
            }
            {
                auto oversized = buffers.acquireBackdrop(INT_MAX, INT_MAX, source, "receiver", {3});
                require(oversized->cacheOwner.empty(), "oversized backdrop bypassed admission limit");
            }
            {
                auto cached = buffers.acquireBackdrop(10, 10, source, "receiver", {2});
                require(cached->contentValid, "cache pressure evicted the admitted working set");
            }
            result = "pass";
        } catch (const std::exception& error) {
            result = std::string("FAIL: ") + error.what();
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
        Render::GL::g_pHyprOpenGL->setViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        return {};
    }
    bool needsLiveBlur() override { return false; }
    bool needsPrecomputeBlur() override { return false; }
    bool disableSimplification() override { return true; }
    const char* passName() override { return "CHyprliquidSecurityTest"; }
    ePassElementType type() override { return EK_CUSTOM; }
};
}

APICALL EXPORT std::string PLUGIN_API_VERSION() { return HYPRLAND_API_VERSION; }
APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE h) {
    handle = h;
    if (std::string(__hyprland_api_get_hash()) != __hyprland_api_get_client_hash())
        throw std::runtime_error("Security test ABI mismatch");
    command = HyprlandAPI::registerHyprCtlCommand(handle, {"hyprliquid-test:security", false,
        [](eHyprCtlOutputFormat, std::string request) {
            if (request.ends_with(" lock")) {
                g_pSessionLockManager->forceLock();
                for (const auto& monitor : State::monitorState()->monitors()) g_pHyprRenderer->damageMonitor(monitor);
                return std::string("ok");
            }
            if (request.ends_with(" unlock")) { g_pSessionLockManager->forceUnlock(); return std::string("ok"); }
            if (request.ends_with(" locked")) return std::string(g_pSessionLockManager->isSessionLocked() ? "yes" : "no");
            if (request.ends_with(" run")) {
                pending = true;
                result = "pending";
                for (const auto& monitor : State::monitorState()->monitors()) {
                    g_pHyprRenderer->damageMonitor(monitor);
                    monitor->scheduleFrame();
                }
            }
            return result;
        }});
    listener = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) {
        if (stage == RENDER_LAST_MOMENT && std::exchange(pending, false))
            g_pHyprRenderer->addPassElement(makeUnique<CSecurityPass>());
    });
    return {"hyprliquid-security-test", "Disposable security regression fixture", "hyprliquid", "1"};
}
APICALL EXPORT void PLUGIN_EXIT() {
    listener.reset();
    g_pHyprRenderer->m_renderPass.removeAllOfType("CHyprliquidSecurityTest");
    HyprlandAPI::unregisterHyprCtlCommand(handle, command);
    command.reset();
}
