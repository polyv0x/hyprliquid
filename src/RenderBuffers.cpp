#include "RenderBuffers.hpp"
#include "Globals.hpp"
#include "WindowScene.hpp"

#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprgraphics/egl/Egl.hpp>
#include <json-c/json.h>
#include <algorithm>
#include <unordered_set>

namespace {
// Hyprland's framebuffer destructor changes bindings and the viewport. Pool
// eviction can run inside a draw, so preserve the caller's exact GL state.
struct SBindings {
    GLint read = 0, draw = 0, viewport[4]{};
    SBindings() {
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
        glGetIntegerv(GL_VIEWPORT, viewport);
    }
    ~SBindings() {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
        g_pHyprOpenGL->setViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    }
};
void number(json_object* o, const char* k, uint64_t n) { json_object_object_add(o, k, json_object_new_uint64(n)); }
void string(json_object* o, const char* k, const std::string& s) { json_object_object_add(o, k, json_object_new_string(s.c_str())); }
uint64_t size(const SP<Render::IFramebuffer>& fb) {
    return fb && fb->isAllocated() ? CRenderBuffers::bytes(fb->m_size.x, fb->m_size.y, fb->m_drmFormat) : 0;
}
uint64_t size(const CRenderBuffers::SSamples& samples) { return size(samples.sharp) + size(samples.blurred); }
}

uint64_t CRenderBuffers::bytes(int width, int height, DRMFormat format) {
    const auto* info = Hyprgraphics::Egl::getPixelFormatFromDRM(format);
    if (!info || width <= 0 || height <= 0)
        return 0;
    const int blockHeight = std::max(1, static_cast<int>(info->blockSize.y));
    return uint64_t(Hyprgraphics::Egl::minStride(info, width)) * ((height + blockHeight - 1) / blockHeight);
}

CRenderBuffers::CLease::CLease(CRenderBuffers& pool, std::shared_ptr<SSamples> samples)
    : m_pool(pool), m_samples(std::move(samples)) {}

CRenderBuffers::CLease::~CLease() {
    m_samples.reset();
    m_pool.trim();
    m_pool.scheduleCleanup();
}

CRenderBuffers::CLease CRenderBuffers::acquire(int width, int height, const SP<Render::IFramebuffer>& source) {
    const auto color = source->imageDescription();
    const SKey key{width, height, source->m_drmFormat, color ? color->id() : 0,
                   eglGetCurrentDisplay(), eglGetCurrentContext()};
    for (auto& samples : m_pool) {
        if (samples.use_count() == 1 && samples->key == key) {
            samples->used = Clock::now();
            samples->sequence = ++m_sequence;
            ++m_hits;
            return CLease(*this, samples);
        }
    }
    auto samples = std::make_shared<SSamples>();
    samples->key = key;
    samples->used = Clock::now();
    samples->sequence = ++m_sequence;
    samples->id = samples->sequence;
    ++m_misses;
    m_pool.push_back(samples);
    return CLease(*this, std::move(samples));
}

bool CRenderBuffers::prepare(SP<Render::IFramebuffer>& fb, int width, int height, DRMFormat format,
                             const std::string& role, const std::string& owner) {
    if (!fb)
        fb = g_pHyprRenderer->createFB("hyprliquid-" + role);
    auto& record = m_records[fb.get()];
    if (!record.fb) {
        record = {.fb = fb, .id = ++m_nextId, .role = role, .owner = owner};
    }
    if (!fb->isAllocated() || fb->m_size != Vector2D(width, height) || fb->m_drmFormat != format) {
        const bool reallocation = record.allocations > 0;
        if (!fb->alloc(width, height, format)) {
            ++m_failures;
            return false;
        }
        ++record.allocations;
        ++m_allocations;
        if (reallocation) {
            ++record.reallocations;
            ++m_reallocations;
        }
        uint64_t retained = 0;
        for (const auto& [_, live] : m_records) retained += size(live.fb.lock());
        m_peakRetainedBytes = std::max(m_peakRetainedBytes, retained);
    }
    record.frame = m_frame;
    // Weak records must not grow with resize/close cycles either.
    std::erase_if(m_records, [](const auto& item) { return !item.second.fb; });
    return true;
}

void CRenderBuffers::identify(const SP<Render::IFramebuffer>& fb, const std::string& role, const std::string& owner) {
    if (!fb) return;
    if (auto it = m_records.find(fb.get()); it != m_records.end()) {
        it->second.role = role;
        it->second.owner = owner;
    }
}

void CRenderBuffers::touch(const SP<Render::IFramebuffer>& fb) {
    if (fb) if (auto it = m_records.find(fb.get()); it != m_records.end()) it->second.frame = m_frame;
}

void CRenderBuffers::trim(bool expired) {
    if (m_pool.empty()) return;
    // The hit path needs no GL queries. Capture state only if something is
    // actually being deleted (glGet* can serialize some drivers).
    std::optional<SBindings> restore;
    if (expired) {
        const auto now = Clock::now();
        std::erase_if(m_pool, [&](const auto& entry) {
            if (entry.use_count() != 1 || now - entry->used < IDLE_TIMEOUT) return false;
            if (!restore) restore.emplace();
            ++m_evictions;
            return true;
        });
    }
    // Limit idle capacity, excluding leased storage. Keep a sole oversize pair
    // so a large HDR pane does not allocate on every draw. This exception is
    // bounded by one exact-size pair, never an unbounded resize history.
    while (true) {
        uint64_t retained = 0;
        size_t count = 0;
        auto oldest = m_pool.end();
        for (auto it = m_pool.begin(); it != m_pool.end(); ++it) {
            if (it->use_count() != 1) continue;
            retained += size(**it);
            ++count;
            if (oldest == m_pool.end() || (*it)->sequence < (*oldest)->sequence) oldest = it;
        }
        if (count <= 1 || (count <= RETAINED_PAIRS && retained <= RETAINED_BYTES)) break;
        if (!restore) restore.emplace();
        m_pool.erase(oldest);
        ++m_evictions;
    }
}

void CRenderBuffers::scheduleCleanup() {
    if (!m_cleanup) {
        m_cleanup = makeShared<CEventLoopTimer>(std::nullopt, [this](SP<CEventLoopTimer>, void*) {
            // This timer retires storage without damaging a monitor or asking
            // for a frame. GL deletion defers pending GPU consumers naturally.
            g_pHyprOpenGL->makeEGLCurrent();
            {
                SBindings restore;
                trim(true);
                WindowScene::collectUnused();
            }
            if (!m_pool.empty() || WindowScene::hasRetired()) scheduleCleanup();
        }, nullptr);
        g_pEventLoopManager->addTimer(m_cleanup);
    }
    if (!m_cleanup->armed()) m_cleanup->updateTimeout(std::chrono::seconds(1));
}

void CRenderBuffers::beginFrame(PHLMONITOR monitor) {
    ++m_frame;
    m_monitor = monitor ? monitor->m_id : -1;
    m_frameStart = Clock::now();
}

void CRenderBuffers::endFrame() {
    if (m_frameStart == Clock::time_point{}) return;
    m_frameTimes.push_back(std::chrono::duration<double, std::milli>(Clock::now() - m_frameStart).count());
    if (m_frameTimes.size() > 256) m_frameTimes.pop_front();
    m_frameStart = {};
    WindowScene::finishFrame(m_monitor, m_frame);
}

void CRenderBuffers::shutdown() {
    if (m_cleanup) {
        m_cleanup->cancel();
        g_pEventLoopManager->removeTimer(m_cleanup);
        m_cleanup.reset();
    }
    m_pool.clear();
    m_records.clear();
}
CRenderBuffers::~CRenderBuffers() { shutdown(); }

json_object* CRenderBuffers::diagnostics() const {
    auto* result = json_object_new_object();
    auto* buffers = json_object_new_array();
    uint64_t retained = 0, used = 0, leased = 0, pooled = 0;
    std::unordered_set<Render::ITexture*> seen;
    for (const auto& [_, record] : m_records) {
        const auto fb = record.fb.lock();
        if (!fb || !fb->isAllocated() || !seen.insert(fb->getTexture().get()).second) continue;
        auto* item = json_object_new_object();
        const auto nominal = size(fb);
        retained += nominal;
        if (record.frame == m_frame) used += nominal;
        number(item, "id", record.id);
        string(item, "role", record.role);
        string(item, "owner", record.owner);
        number(item, "width", fb->m_size.x);
        number(item, "height", fb->m_size.y);
        number(item, "drm_format", fb->m_drmFormat);
        const auto color = fb->imageDescription();
        number(item, "color_description_id", color ? color->id() : 0);
        if (color) string(item, "color_description", std::format("{}", color->value()));
        number(item, "nominal_bytes", nominal);
        number(item, "allocations", record.allocations);
        number(item, "reallocations", record.reallocations);
        number(item, "last_used_frame", record.frame);
        json_object_array_add(buffers, item);
    }
    for (const auto& samples : m_pool) {
        pooled += size(*samples);
        if (samples.use_count() > 1) leased += size(*samples);
    }
    json_object_object_add(result, "buffers", buffers);
    number(result, "retained_bytes", retained);
    number(result, "peak_retained_bytes", m_peakRetainedBytes);
    number(result, "last_frame_bytes", used);
    number(result, "frame", m_frame);
    json_object_object_add(result, "last_monitor_id", json_object_new_int64(m_monitor));
    number(result, "allocations", m_allocations);
    number(result, "reallocations", m_reallocations);
    number(result, "allocation_failures", m_failures);
    auto* pool = json_object_new_object();
    number(pool, "pairs", m_pool.size());
    number(pool, "bytes", pooled);
    number(pool, "leased_bytes", leased);
    number(pool, "hits", m_hits);
    number(pool, "misses", m_misses);
    number(pool, "evictions", m_evictions);
    number(pool, "retained_limit_bytes", RETAINED_BYTES);
    number(pool, "retained_limit_pairs", RETAINED_PAIRS);
    number(pool, "idle_timeout_ms", std::chrono::duration_cast<std::chrono::milliseconds>(IDLE_TIMEOUT).count());
    json_object_object_add(result, "window_pool", pool);
    auto* times = json_object_new_array();
    for (double ms : m_frameTimes) json_object_array_add(times, json_object_new_double(ms));
    json_object_object_add(result, "cpu_frame_ms", times);
    return result;
}
