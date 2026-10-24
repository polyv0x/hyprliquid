#include "HoverAngle.hpp"
#include "PointerAngle.hpp"
#include "Globals.hpp"
#include "GlassDecoration.hpp"

#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/state/MonitorState.hpp>

void CHoverAngles::clear() {
    for (auto& [_, state] : m_monitors) state.angle->resetAllCallbacks();
    m_monitors.clear(); // Native variables disconnect before the plugin unloads.
    m_pointer.reset();
}

void CHoverAngles::remove(MONITORID monitor) {
    if (auto it = m_monitors.find(monitor); it != m_monitors.end()) {
        it->second.angle->resetAllCallbacks();
        m_monitors.erase(it);
    }
    m_pointer.reset();
}

void CHoverAngles::refresh(bool force) {
    if (!runtimeAnimateHoverAngle() || g_pSessionLockManager->isSessionLocked()) {
        clear();
        return;
    }
    update(Pointer::mgr()->position(), force);
}

void CHoverAngles::update(const Vector2D& position, bool force) {
    if (!runtimeAnimateHoverAngle() || g_pSessionLockManager->isSessionLocked()) {
        clear();
        return;
    }
    if (!force && m_pointer == position) return;
    m_pointer = position;
    const auto monitor = State::monitorState()->query().vec(position).run();
    if (!monitor || monitor->isMirror()) return;
    auto [it, inserted] = m_monitors.try_emplace(monitor->m_id);
    if (inserted) {
        it->second.monitor = monitor;
        Animation::mgr()->createAnimation(0.0f, it->second.angle,
            Config::animationTree()->getAnimationPropertyConfig("windowsMove"), AVARDAMAGE_NONE);
        it->second.angle->setUpdateCallback([this, id = monitor->m_id](auto) { changed(id); });
    }
    const auto relative = position - monitor->m_position - monitor->m_size / 2.0;
    const auto heading = PointerAngle::lookup(relative.x, relative.y);
    for (auto& [id, state] : m_monitors) {
        if (force) state.angle->setConfig(Config::animationTree()->getAnimationPropertyConfig("windowsMove"));
        const float target = id == monitor->m_id ? heading.value_or(PointerAngle::wrap(state.angle->goal())) : 0.0f;
        // Periodic rebasing bounds the stored value without changing direction.
        if (!state.angle->isBeingAnimated() && std::abs(state.angle->value()) > 180.0f)
            state.angle->setValueAndWarp(PointerAngle::wrap(state.angle->value()));
        const float goal = PointerAngle::nearest(state.angle->value(), target);
        if (std::abs(goal - state.angle->goal()) >= 0.05f) *state.angle = goal;
    }
}

void CHoverAngles::changed(MONITORID id) {
    const auto it = m_monitors.find(id);
    if (it == m_monitors.end()) return;
    auto& state = it->second;
    const float value = state.angle->value();
    if (value == state.lastValue) return;
    state.lastValue = value;
    const auto monitor = state.monitor.lock();
    if (!monitor || !g_pGlobalState || !runtimeAnimateHoverAngle()) return;
    bool damaged = false;
    for (const auto& decoration : g_pGlobalState->decorations) {
        if (!decoration) continue;
        const auto window = decoration->getOwner();
        if (!window || window->m_monitor != monitor || !window->m_isMapped || window->isHidden() ||
            !g_pHyprRenderer->shouldRenderWindow(window, monitor) || !decoration->usesLiquidGlass()) continue;
        decoration->damageEntire();
        damaged = true;
    }
    if (damaged) {
        g_pGlobalState->bumpSceneGeneration(monitor);
        monitor->scheduleFrame();
    }
}

float CHoverAngles::value(const PHLMONITOR& monitor) const {
    const auto it = monitor ? m_monitors.find(monitor->m_id) : m_monitors.end();
    return it == m_monitors.end() ? 0.0f : PointerAngle::wrap(it->second.angle->value());
}

float CHoverAngles::target(const PHLMONITOR& monitor) const {
    const auto it = monitor ? m_monitors.find(monitor->m_id) : m_monitors.end();
    return it == m_monitors.end() ? 0.0f : PointerAngle::wrap(it->second.angle->goal());
}

bool CHoverAngles::animating(const PHLMONITOR& monitor) const {
    const auto it = monitor ? m_monitors.find(monitor->m_id) : m_monitors.end();
    return it != m_monitors.end() && it->second.angle->isBeingAnimated();
}
