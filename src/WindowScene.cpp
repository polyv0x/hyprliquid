#include "WindowScene.hpp"
#include "GlassDecoration.hpp"
#include "Globals.hpp"
#include "WindowGeometry.hpp"

#include <GLES3/gl32.h>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/render/pass/Pass.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#include <algorithm>
#include <unordered_map>

namespace WindowScene {
namespace {
using RenderWindow = void (*)(Render::IHyprRenderer*, PHLWINDOW, PHLMONITOR, const Time::steady_tp&,
                              bool, Render::eRenderPassMode, bool, bool);
using RenderWorkspace = void (*)(Render::IHyprRenderer*, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp&);
using Hyprutils::Utils::CScopeGuard;

CFunctionHook* workspaceHook = nullptr;
RenderWindow renderWindow = nullptr;
std::shared_ptr<SWindowScene> queuedScene;
bool queuingBase = false;

struct SBuffers {
    PHLMONITORREF monitor;
    SP<Render::IFramebuffer> background;
    SP<Render::IFramebuffer> image;
};
std::unordered_map<MONITORID, SBuffers> buffers;

GLuint fbId(const SP<Render::IFramebuffer>& fb) {
    return dynamic_cast<Render::GL::CGLFramebuffer*>(fb.get())->getFBID();
}

CGlassDecoration* decorationFor(PHLWINDOW window) {
    for (auto& decoration : g_pGlobalState->decorations)
        if (decoration && decoration->getOwner() == window)
            return decoration.get();
    return nullptr;
}

class CWindowScenePassElement : public IPassElement {
  public:
    explicit CWindowScenePassElement(std::shared_ptr<SWindowScene> scene) : m_scene(std::move(scene)) {}
    Render::CRenderPass pass;

    std::vector<UP<IPassElement>> draw() override {
        auto& renderer = *g_pHyprRenderer;
        const auto savedData = renderer.m_renderData;
        const auto monitor = savedData.pMonitor.lock();
        const auto source = savedData.currentFB;
        if (!monitor || !source)
            return {};

        const bool feedback = renderer.m_bBlockSurfaceFeedback;
        const bool snapshot = renderer.m_bRenderingSnapshot;
        const bool blur = monitor->m_blurFBShouldRender;
        GLint viewport[4];
        glGetIntegerv(GL_VIEWPORT, viewport);
        GLint scissorBox[4];
        glGetIntegerv(GL_SCISSOR_BOX, scissorBox);
        const bool scissor = glIsEnabled(GL_SCISSOR_TEST);
        CScopeGuard restore([&] {
            renderer.m_renderData = savedData;
            renderer.m_bBlockSurfaceFeedback = feedback;
            renderer.m_bRenderingSnapshot = snapshot;
            monitor->m_blurFBShouldRender = blur;
            renderer.bindFB(source);
            renderer.setViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
            glScissor(scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]);
            g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, scissor);
        });

        for (auto& fb : {m_scene->background, m_scene->image}) {
            if (fb->m_size != source->m_size || fb->m_drmFormat != source->m_drmFormat)
                if (!fb->alloc(source->m_size.x, source->m_size.y, source->m_drmFormat))
                    return {};
            fb->setImageDescription(source->imageDescription());
            g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, false);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, fbId(source));
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbId(fb));
            glBlitFramebuffer(0, 0, source->m_size.x, source->m_size.y,
                              0, 0, source->m_size.x, source->m_size.y, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }

        renderer.bindFB(m_scene->image);
        renderer.m_bBlockSurfaceFeedback = true;
        renderer.m_bRenderingSnapshot = true;
        renderer.m_renderData.noSimplify = true;
        pass.render(CRegion(CBox{{}, monitor->m_transformedSize}));
        m_scene->ready = true;
        return {};
    }

    bool needsLiveBlur() override { return true; }
    bool needsPrecomputeBlur() override { return false; }
    bool disableSimplification() override { return true; }
    std::optional<CBox> boundingBox() override {
        return CBox{{}, g_pHyprRenderer->m_renderData.pMonitor->m_transformedSize / g_pHyprRenderer->m_renderData.pMonitor->m_scale};
    }
    const char* passName() override { return "CWindowScenePassElement"; }
    ePassElementType type() override { return EK_CUSTOM; }

  private:
    std::shared_ptr<SWindowScene> m_scene;
};

std::shared_ptr<SWindowScene> collectScene(Render::IHyprRenderer* renderer, PHLMONITOR monitor, PHLWORKSPACE workspace) {
    if (!workspace || renderer->m_bRenderingSnapshot || renderer->m_renderData.projectionType != Render::RPT_MONITOR)
        return nullptr;

    auto scene = std::make_shared<SWindowScene>();
    std::vector<CBox> boxes;
    bool hasGlass = false;
    for (const auto& window : Desktop::windowState()->windows()) {
        if (!window->m_isMapped || window->isHidden() || window->m_isFloating || !renderer->shouldRenderWindow(window, monitor))
            continue;
        const bool ignoreSpecial = window->m_monitorMovedFrom != -1 && window->m_workspace && !window->m_workspace->isVisible();
        if (!ignoreSpecial && workspace->m_isSpecialWorkspace != window->onSpecialWorkspace())
            continue;

        // Overlapping/stacked tiles and transformed windows must keep native
        // occlusion. Their visible neighbours really can change on focus.
        if (!window->m_transformers.empty())
            return nullptr;
        const auto box = WindowGeometry::computeWindowBox(window, monitor);
        if (!box)
            return nullptr;
        for (const auto& other : boxes)
            if (!box->intersection(other).empty())
                return nullptr;
        boxes.push_back(*box);
        scene->windows.emplace_back(window);
        if (auto* decoration = decorationFor(window); decoration && decoration->usesGlass())
            hasGlass = true;
    }
    return hasGlass && scene->windows.size() > 1 ? scene : nullptr;
}

void renderWorkspace(Render::IHyprRenderer* renderer, PHLMONITOR monitor, PHLWORKSPACE workspace, const Time::steady_tp& now) {
    auto original = reinterpret_cast<RenderWorkspace>(workspaceHook->m_original);
    auto scene = collectScene(renderer, monitor, workspace);
    if (!scene) {
        original(renderer, monitor, workspace, now);
        return;
    }

    std::erase_if(buffers, [](const auto& item) { return !item.second.monitor; });
    auto& storage = buffers[monitor->m_id];
    storage.monitor = monitor;
    if (!storage.background)
        storage.background = renderer->createFB("hyprliquid-scene-background");
    if (!storage.image)
        storage.image = renderer->createFB("hyprliquid-scene-windows");
    scene->background = storage.background;
    scene->image = storage.image;

    auto capture = makeUnique<CWindowScenePassElement>(scene);
    const auto previousScene = queuedScene;
    queuedScene = scene;
    CScopeGuard restoreScene([&] { queuedScene = previousScene; });
    {
        const auto savedData = renderer->m_renderData;
        const bool snapshot = renderer->m_bRenderingSnapshot;
        auto redirect = renderer->redirectPass(&capture->pass);
        queuingBase = true;
        renderer->m_bRenderingSnapshot = true;
        CScopeGuard restore([&] {
            renderer->m_renderData = savedData;
            renderer->m_bRenderingSnapshot = snapshot;
            queuingBase = false;
        });
        for (const auto& window : scene->windows) {
            // Only our glass plus the client's main surfaces. Popups, shadows
            // and other plugins' decorations belong to the normal scene pass.
            if (auto* decoration = decorationFor(window.lock()))
                decoration->draw(monitor, 1.0f);
            renderWindow(renderer, window.lock(), monitor, now, false, Render::RENDER_PASS_MAIN, false, false);
        }
    }
    renderer->addPassElement(std::move(capture));

    // The auxiliary scene reads the wallpaper across the monitor. Repaint it
    // completely on this already scheduled frame, without scheduling another
    // frame or retaining last frame's windows in the backdrop.
    renderer->m_renderData.damage.add(CBox{{}, monitor->m_transformedSize});
    renderer->m_renderData.finalDamage.add(CBox{{}, monitor->m_transformedSize});
    original(renderer, monitor, workspace, now);
}
} // namespace

bool initialize() {
    for (const auto& match : HyprlandAPI::findFunctionsByName(PHANDLE, "renderWindow"))
        if (match.demangled.contains("IHyprRenderer::renderWindow(")) {
            renderWindow = reinterpret_cast<RenderWindow>(match.address);
            break;
        }
    if (!renderWindow)
        return false;
    for (const auto& match : HyprlandAPI::findFunctionsByName(PHANDLE, "renderWorkspaceWindows"))
        if (match.demangled.contains("IHyprRenderer::renderWorkspaceWindows(")) {
            workspaceHook = HyprlandAPI::createFunctionHook(PHANDLE, match.address, reinterpret_cast<void*>(renderWorkspace));
            if (workspaceHook && workspaceHook->hook())
                return true;
            break;
        }
    destroy();
    return false;
}

void destroy() {
    g_pHyprRenderer->m_renderPass.removeAllOfType("CWindowScenePassElement");
    if (workspaceHook)
        HyprlandAPI::removeFunctionHook(PHANDLE, workspaceHook);
    workspaceHook = nullptr;
    renderWindow = nullptr;
    queuedScene.reset();
    buffers.clear();
    queuingBase = false;
}

std::shared_ptr<SWindowScene> queuedFor(PHLWINDOW window) {
    if (queuedScene && std::ranges::any_of(queuedScene->windows, [&](const auto& w) { return w == window; }))
        return queuedScene;
    return nullptr;
}

bool buildingBase() { return queuingBase; }
} // namespace WindowScene
