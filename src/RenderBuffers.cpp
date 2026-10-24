#include "RenderBuffers.hpp"
#include "Globals.hpp"
#include "WindowScene.hpp"
#include "RenderSafety.hpp"
#include "PixelTransfer.hpp"

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
        // Allocators may use raw GL calls, leaving Hyprland's viewport cache
        // unchanged. Restore the actual state even when the cache already agrees.
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        g_pHyprOpenGL->setViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    }
};
void number(json_object* o, const char* k, uint64_t n) { json_object_object_add(o, k, json_object_new_uint64(n)); }
void string(json_object* o, const char* k, const std::string& s) { json_object_object_add(o, k, json_object_new_string(s.c_str())); }
uint64_t size(const SP<Render::IFramebuffer>& fb) {
    return fb && fb->isAllocated() ? CRenderBuffers::bytes(fb->m_size.x, fb->m_size.y, fb->m_drmFormat) : 0;
}
uint64_t size(const CRenderBuffers::SSamples& samples) { return size(samples.sharp) + size(samples.blurred) + size(samples.halfSharp); }
uint64_t size(const CRenderBuffers::SBlurScratch& scratch) {
    uint64_t result = size(scratch.temporary);
    for (const auto& fb : scratch.pyramid) result += size(fb);
    return result;
}
}

uint64_t CRenderBuffers::bytes(int width, int height, DRMFormat format) {
    const auto* info = Hyprgraphics::Egl::getPixelFormatFromDRM(format);
    if (!info || width <= 0 || height <= 0)
        return 0;
    const int blockHeight = std::max(1, static_cast<int>(info->blockSize.y));
    const auto blockWidth = std::max(1, Hyprgraphics::Egl::pixelsPerBlock(info));
    return RenderSafety::bufferBytes(width, height, blockWidth, blockHeight, info->bytesPerBlock).value_or(0);
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

CRenderBuffers::CBlurLease::CBlurLease(CRenderBuffers& pool, std::shared_ptr<SBlurScratch> scratch)
    : m_pool(pool), m_scratch(std::move(scratch)) {}

CRenderBuffers::CBlurLease::~CBlurLease() {
    m_scratch.reset();
    m_pool.trim();
    m_pool.scheduleCleanup();
}

CRenderBuffers::CBlurLease CRenderBuffers::acquireBlur(int width, int height, size_t levels,
                                                     const SP<Render::IFramebuffer>& source) {
    const auto color = source->imageDescription();
    const SKey key{width, height, source->m_drmFormat, color ? color->id() : 0,
                   eglGetCurrentDisplay(), eglGetCurrentContext()};
    for (auto& scratch : m_scratchPool) {
        if (scratch.use_count() != 1 || scratch->key != key || scratch->levels != levels) continue;
        scratch->used = Clock::now();
        scratch->sequence = ++m_sequence;
        ++m_scratchHits;
        return CBlurLease(*this, scratch);
    }
    auto scratch = std::make_shared<SBlurScratch>();
    scratch->key = key;
    scratch->levels = levels;
    scratch->pyramid.resize(levels - 1);
    scratch->used = Clock::now();
    scratch->sequence = ++m_sequence;
    ++m_scratchMisses;
    m_scratchPool.push_back(scratch);
    return CBlurLease(*this, std::move(scratch));
}

bool CRenderBuffers::prepare(SP<Render::IFramebuffer>& fb, int width, int height, DRMFormat format,
                             const std::string& role, const std::string& owner) {
    if (!RenderSafety::dimension(width) || !RenderSafety::dimension(height) ||
        !Hyprgraphics::Egl::getPixelFormatFromDRM(format)) {
        ++m_failures;
        return false;
    }
    // Hyprland's GL allocator asserts on incomplete framebuffers. Reject sizes
    // a client can request but the GPU cannot support before calling into it.
    if (!fb || !fb->isAllocated() || fb->m_size != Vector2D(width, height) || fb->m_drmFormat != format) {
        GLint maxTexture = 0, maxViewport[2]{};
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexture);
        glGetIntegerv(GL_MAX_VIEWPORT_DIMS, maxViewport);
        if (width > maxTexture || height > maxTexture || width > maxViewport[0] || height > maxViewport[1]) {
            ++m_failures;
            return false;
        }
    }
    if (!fb)
        fb = g_pHyprRenderer->createFB("hyprliquid-" + role);
    if (!fb) {
        ++m_failures;
        return false;
    }
    auto& record = m_records[fb.get()];
    if (!record.fb) {
        record = {.fb = fb, .id = ++m_nextId, .role = role, .owner = owner};
    }
    if (!fb->isAllocated() || fb->m_size != Vector2D(width, height) || fb->m_drmFormat != format) {
        SBindings restore;
        CPixelTransferGuard<true> unpack;
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

void CRenderBuffers::release(SP<Render::IFramebuffer>& fb) {
    if (!fb) return;
    SBindings restore;
    fb.reset();
}

void CRenderBuffers::trim(bool expired) {
    if (m_pool.empty() && m_scratchPool.empty()) return;
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
    // Exact dimensions, format, context and level count preserve every filter
    // tap and texel center. Only unleased sets may be evicted; an auxiliary or
    // nested blur cannot overwrite an invocation that still owns its scratch.
    if (expired) {
        const auto now = Clock::now();
        std::erase_if(m_scratchPool, [&](const auto& entry) {
            if (entry.use_count() != 1 || now - entry->used < IDLE_TIMEOUT) return false;
            if (!restore) restore.emplace();
            ++m_scratchEvictions;
            return true;
        });
    }
    while (true) {
        uint64_t retained = 0;
        size_t count = 0;
        auto oldest = m_scratchPool.end();
        for (auto it = m_scratchPool.begin(); it != m_scratchPool.end(); ++it) {
            if (it->use_count() != 1) continue;
            retained += size(**it);
            ++count;
            if (oldest == m_scratchPool.end() || (*it)->sequence < (*oldest)->sequence) oldest = it;
        }
        if (count <= SCRATCH_RETAINED_SETS && retained <= SCRATCH_RETAINED_BYTES) break;
        if (!restore) restore.emplace();
        m_scratchPool.erase(oldest);
        ++m_scratchEvictions;
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
            if (!m_pool.empty() || !m_scratchPool.empty() || WindowScene::hasRetired()) scheduleCleanup();
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
    m_scratchPool.clear();
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
    auto* scratch = json_object_new_object();
    uint64_t scratchBytes = 0, scratchLeased = 0;
    for (const auto& entry : m_scratchPool) {
        scratchBytes += size(*entry);
        if (entry.use_count() > 1) scratchLeased += size(*entry);
    }
    number(scratch, "sets", m_scratchPool.size());
    number(scratch, "bytes", scratchBytes);
    number(scratch, "leased_bytes", scratchLeased);
    number(scratch, "hits", m_scratchHits);
    number(scratch, "misses", m_scratchMisses);
    number(scratch, "evictions", m_scratchEvictions);
    number(scratch, "retained_limit_bytes", SCRATCH_RETAINED_BYTES);
    number(scratch, "retained_limit_sets", SCRATCH_RETAINED_SETS);
    json_object_object_add(result, "scratch_pool", scratch);
    auto* times = json_object_new_array();
    for (double ms : m_frameTimes) json_object_array_add(times, json_object_new_double(ms));
    json_object_object_add(result, "cpu_frame_ms", times);
    return result;
}
