#pragma once

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <EGL/egl.h>
#include <chrono>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct json_object;

// Only window samples are transient. Layers keep their independently cached
// samples, and scene passes retain their own strong framebuffer references.
class CRenderBuffers {
  public:
    using Clock = std::chrono::steady_clock;
    static constexpr uint64_t RETAINED_BYTES = 128ULL * 1024 * 1024;
    static constexpr size_t RETAINED_PAIRS = 8;
    static constexpr auto IDLE_TIMEOUT = std::chrono::seconds(5);

    struct SKey {
        int width, height;
        DRMFormat format;
        uint64_t color;
        EGLDisplay display;
        EGLContext context;
        bool operator==(const SKey&) const = default;
    };
    struct SSamples {
        SKey key;
        SP<Render::IFramebuffer> sharp, blurred;
        Clock::time_point used;
        uint64_t sequence = 0;
        uint64_t id = 0;
    };
    class CLease {
      public:
        CLease(CRenderBuffers& pool, std::shared_ptr<SSamples> samples);
        CLease(const CLease&) = delete;
        CLease& operator=(const CLease&) = delete;
        CLease(CLease&&) = delete;
        ~CLease();
        SSamples* operator->() const { return m_samples.get(); }
      private:
        CRenderBuffers& m_pool;
        std::shared_ptr<SSamples> m_samples;
    };

    ~CRenderBuffers();
    CLease acquire(int width, int height, const SP<Render::IFramebuffer>& source);
    bool prepare(SP<Render::IFramebuffer>& fb, int width, int height, DRMFormat format,
                 const std::string& role, const std::string& owner = "shared");
    void identify(const SP<Render::IFramebuffer>& fb, const std::string& role, const std::string& owner);
    void touch(const SP<Render::IFramebuffer>& fb);
    void beginFrame(PHLMONITOR monitor);
    void endFrame();
    uint64_t frame() const { return m_frame; }
    void scheduleCleanup();
    void trim(bool expired = false);
    void shutdown();
    json_object* diagnostics() const;
    static uint64_t bytes(int width, int height, DRMFormat format);

  private:
    struct SRecord {
        WP<Render::IFramebuffer> fb;
        uint64_t id = 0, allocations = 0, reallocations = 0, frame = 0;
        std::string role, owner;
    };
    std::vector<std::shared_ptr<SSamples>> m_pool;
    std::unordered_map<Render::IFramebuffer*, SRecord> m_records;
    SP<CEventLoopTimer> m_cleanup;
    uint64_t m_nextId = 0, m_sequence = 0, m_hits = 0, m_misses = 0, m_evictions = 0;
    uint64_t m_allocations = 0, m_reallocations = 0, m_failures = 0;
    uint64_t m_peakRetainedBytes = 0;
    uint64_t m_frame = 0;
    int64_t m_monitor = -1;
    Clock::time_point m_frameStart;
    std::deque<double> m_frameTimes;
};
