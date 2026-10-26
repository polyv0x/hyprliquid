// Test-only fault injection, loaded only by scripts/test-nested.py.
#include "../src/GlassRenderer.hpp"
#include "../src/DamageRegion.hpp"
#include "../src/Globals.hpp"
#include "../src/PixelTransfer.hpp"
#include "../src/RenderBuffers.hpp"

#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <climits>
#include <dlfcn.h>

namespace {
HANDLE handle;
SP<SHyprCtlCommand> command;
Hyprutils::Signal::CHyprSignalListener listener;
std::string result = "idle";
bool pending = false;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<unsigned char> rawPixels(const SP<Render::IFramebuffer>& fb) {
    require(glGetError() == GL_NO_ERROR, "GL error before raw blur read");
    glBindFramebuffer(GL_FRAMEBUFFER, dynamic_cast<Render::GL::CGLFramebuffer*>(fb.get())->getFBID());
    CPixelTransferGuard<false> pack;
    GLint type = 0, format = 0;
    glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &type);
    glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &format);
    const int pixelBytes = type == GL_FLOAT || type == GL_UNSIGNED_INT || type == GL_INT ? 16 :
                           type == GL_HALF_FLOAT || type == GL_UNSIGNED_SHORT || type == GL_SHORT ? 8 : 4;
    std::vector<unsigned char> pixels(size_t(fb->m_size.x) * size_t(fb->m_size.y) * pixelBytes);
    glReadPixels(0, 0, fb->m_size.x, fb->m_size.y, format, type, pixels.data());
    const auto error = glGetError();
    if (error) throw std::runtime_error(std::format("raw blur read failed: format {}, type {}, error {}", format, type, error));
    return pixels;
}

void verifyBlurRegions(const SP<Render::IFramebuffer>& caller) {
    while (glGetError() != GL_NO_ERROR) {}
    // Plugins are loaded locally. Resolve the renderer's owner from its own
    // library, rather than the fixture's separate inline global definition.
    Dl_info library{};
    require(dladdr(reinterpret_cast<const void*>(&GlassRenderer::blurBackground), &library), "renderer library unavailable");
    auto* loaded = dlopen(library.dli_fname, RTLD_LAZY | RTLD_NOLOAD);
    require(loaded, "renderer library is not loaded");
    Hyprutils::Utils::CScopeGuard close([&] { dlclose(loaded); });
    const auto* state = static_cast<const decltype(g_pGlobalState)*>(dlsym(loaded, "g_pGlobalState"));
    require(state && *state, "renderer owner unavailable");
    (*state)->shaderManager.initializeIfNeeded();
    require((*state)->shaderManager.isInitialized(), "region shaders unavailable");
    CRenderBuffers buffers;
    const bool blend = glIsEnabled(GL_BLEND), scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLint scissorBox[4];
    GLfloat clear[4];
    glGetIntegerv(GL_SCISSOR_BOX, scissorBox);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
    Hyprutils::Utils::CScopeGuard restore([&] {
        Render::GL::g_pHyprOpenGL->setCapStatus(GL_BLEND, blend);
        Render::GL::g_pHyprOpenGL->scissor(CBox(scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]), false);
        Render::GL::g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, scissor);
        glClearColor(clear[0], clear[1], clear[2], clear[3]);
    });
    Render::GL::g_pHyprOpenGL->setCapStatus(GL_BLEND, false);
    const auto paint = [&](const SP<Render::IFramebuffer>& target, const CRegion& region, float r, float g, float b) {
        glBindFramebuffer(GL_FRAMEBUFFER, dynamic_cast<Render::GL::CGLFramebuffer*>(target.get())->getFBID());
        Render::GL::g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, true);
        glClearColor(r, g, b, 1);
        region.forEachRect([&](const auto& box) {
            Render::GL::g_pHyprOpenGL->scissor(CBox(box.x1, box.y1, box.x2 - box.x1, box.y2 - box.y1), false);
            glClear(GL_COLOR_BUFFER_BIT);
        });
        Render::GL::g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, false);
    };
    const CBox nativeScissor(13, 17, 607, 439);
    const auto checkNativeScissor = [&](const char* operation) {
        // Native drawing can request the same rectangle it used before our
        // offscreen work. Hyprland must not skip this update using stale state.
        Render::GL::g_pHyprOpenGL->scissor(nativeScissor, false);
        GLint actual[4];
        glGetIntegerv(GL_SCISSOR_BOX, actual);
        if (actual[0] != nativeScissor.x || actual[1] != nativeScissor.y ||
            actual[2] != nativeScissor.w || actual[3] != nativeScissor.h)
            throw std::runtime_error(std::format("{} left native scissor cache stale: {},{},{},{}", operation,
                                                actual[0], actual[1], actual[2], actual[3]));
        Render::GL::g_pHyprOpenGL->scissor(nullptr);
    };
    for (const auto format : {DRM_FORMAT_ARGB8888, DRM_FORMAT_ARGB2101010, DRM_FORMAT_ABGR16161616F}) {
        constexpr int width = 641, height = 481;
        SP<Render::IFramebuffer> input, poison, retained, reference, discarded;
        require(buffers.prepare(input, width, height, format, "test-input"), "region input allocation failed");
        require(buffers.prepare(poison, width, height, format, "test-poison"), "region poison allocation failed");
        paint(poison, CRegion(CBox{0, 0, width, height}), 0.13f, 0.79f, 0.27f);
        for (int mode = 0; mode < 3; ++mode) for (const float sigma : {0.5f, 3.0f, 7.0f, 19.8f, 60.0f}) {
            SP<Render::IFramebuffer> half, fullHalf;
            GlassRenderer::SNeighborBackdrop composition{poison, nullptr, {0, 0}, {100, 80, 500, 400}, {}, nullptr, input};
            const auto* clean = mode == 1 ? &composition : nullptr;
            const auto reduce = [&](SP<Render::IFramebuffer>& target, const CRegion* damage = nullptr) {
                return mode != 2 || GlassRenderer::prepareMaterialBackdrop(input, target, caller, &composition, 0.37f, damage);
            };
            for (int y = 0; y < height; y += 23)
                for (int x = 0; x < width; x += 29)
                    paint(input, CRegion(CBox(x, y, std::min(29, width - x), std::min(23, height - y))),
                        float((x * 13 + y * 7) % 251) / 200, float((x + y * 17) % 233) / 255, float((x * 3 + y) % 199) / 231);
            require(reduce(half), "initial material reduction failed");
            require(GlassRenderer::blurBackground(input, retained, sigma, caller, clean, half), "initial region blur failed");
            CRegion fragmented;
            for (int y = 0; y < 5; ++y) for (int x = 0; x < 8; ++x)
                fragmented.add(CBox(15 + x * 75, 13 + y * 90, 1, 1));
            const std::array<CRegion, 7> changes{
                CRegion(CBox{207, 163, 7, 11}), CRegion(CBox{0, 0, 1, 1}),
                CRegion(CBox{width - 1, height - 1, 1, 1}), CRegion(CBox{319, 0, 1, height}),
                CRegion(CBox{4, 112, 23, 13}).add(CBox{width - 41, height - 30, 19, 17}), fragmented, CRegion{}};
            for (size_t index = 0; index < changes.size(); ++index) {
                SP<Render::IFramebuffer> captured;
                Vector2D padding;
                Render::GL::g_pHyprOpenGL->scissor(nativeScissor, false);
                require(GlassRenderer::sampleBackground(captured, input, CBox{0, 0, width, height}, padding,
                                                       0, nullptr, nullptr, 1, &changes[index]), "partial capture failed");
                checkNativeScissor("partial capture");
                const auto previous = rawPixels(retained);
                paint(input, changes[index], 0.93f - index * 0.1f, index * 0.12f, 0.17f);
                // The next pane can overwrite every shared intermediate. A
                // partial repair must reconstruct all of its own dependencies.
                require(GlassRenderer::blurBackground(poison, discarded, sigma, caller), "scratch poison blur failed");
                Render::GL::g_pHyprOpenGL->scissor(nativeScissor, false);
                require(reduce(half, &changes[index]) && reduce(fullHalf), "partial material reduction failed");
                checkNativeScissor("material reduction");
                if (mode == 2) require(rawPixels(half) == rawPixels(fullHalf), "partial material reduction differs");
                Render::GL::g_pHyprOpenGL->scissor(nativeScissor, false);
                require(GlassRenderer::blurBackground(input, retained, sigma, caller, clean, half, &changes[index]), "partial blur failed");
                checkNativeScissor("partial blur");
                require(GlassRenderer::blurBackground(input, reference, sigma, caller, clean, fullHalf), "reference blur failed");
                if (rawPixels(retained) != rawPixels(reference))
                    throw std::runtime_error(std::format("partial blur differs: format {}, mode {}, sigma {}, region {}", format, mode, sigma, index));
                const auto pixels = rawPixels(reference);
                const int w = reference->m_size.x, h = reference->m_size.y;
                const size_t pixelBytes = pixels.size() / (w * h);
                const double support = DamageRegion::blurSupport(width, height, sigma, mode == 2) + 4;
                const auto affected = DamageRegion::map(changes[index], input->m_size, reference->m_size, support, support);
                for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                    const size_t offset = (y * w + x) * pixelBytes;
                    if (!std::equal(pixels.begin() + offset, pixels.begin() + offset + pixelBytes, previous.begin() + offset))
                        require(affected.containsPoint({double(x), double(y)}), "blur escaped its material damage bound");
                }
            }
        }
    }
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
                CRenderBuffers reservations;
                auto first = reservations.acquireBackdrop(4096, 4096, source, "liquid-first", {1}, false, true);
                require(bool(first), "rejected a Liquid backdrop within budget");
                // Neither lease has allocated textures yet. Admission must
                // already reserve both half-size images, including nested work.
                auto second = reservations.acquireBackdrop(4096, 3072, source, "liquid-second", {1}, false, true);
                require(!second, "unallocated Liquid material input escaped the backdrop budget");
            }
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
                auto declined = buffers.acquireBackdrop(20, 20, source, "uncached-monitor", {1}, false);
                require(!declined, "full backdrop cache admitted an optional scene image");
            }
            {
                auto oversized = buffers.acquireBackdrop(INT_MAX, INT_MAX, source, "receiver", {3});
                require(oversized->cacheOwner.empty(), "oversized backdrop bypassed admission limit");
            }
            {
                auto cached = buffers.acquireBackdrop(10, 10, source, "receiver", {2});
                require(cached->contentValid, "cache pressure evicted the admitted working set");
            }
            {
                auto first = buffers.acquire(20, 20, source, "first-pane", {1});
                require(buffers.prepare(first->sharp, 20, 20, source->m_drmFormat, "test-input-cache"), "input cache allocation failed");
                first->contentValid = true;
                auto second = buffers.acquire(20, 20, source, "second-pane", {1});
                auto transient = buffers.acquire(20, 20, source);
                require(!second->contentValid && !transient->contentValid, "shared another pane's cached input");
            }
            {
                auto first = buffers.acquire(20, 20, source, "first-pane", {1});
                require(first->contentValid && first->sharp->isAllocated(), "lost retained pane input");
            }
            {
                auto changed = buffers.acquire(20, 20, source, "first-pane", {2});
                require(!changed->contentValid, "reused changed pane input");
            }
            verifyBlurRegions(source);
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
