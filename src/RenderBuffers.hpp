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

// Transient window samples and bounded backdrop/blur caches. Layers and scene
// passes retain their own framebuffer references independently.
class CRenderBuffers {
  public:
    using Clock = std::chrono::steady_clock;
    static constexpr uint64_t RETAINED_BYTES = 128ULL * 1024 * 1024;
    static constexpr size_t RETAINED_PAIRS = 8;
    static constexpr auto IDLE_TIMEOUT = std::chrono::seconds(5);
    static constexpr uint64_t SCRATCH_RETAINED_BYTES = 32ULL * 1024 * 1024;
    static constexpr size_t SCRATCH_RETAINED_SETS = 16;
    static constexpr uint64_t BACKDROP_RETAINED_BYTES = 128ULL * 1024 * 1024;
    static constexpr size_t BACKDROP_RETAINED_PAIRS = 8;

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
        SP<Render::IFramebuffer> sharp, blurred, halfSharp;
        Clock::time_point used;
        uint64_t sequence = 0;
        uint64_t id = 0;
        uint64_t sceneSerial = 0;
        std::string cacheOwner;
        std::vector<uint64_t> contentKey;
        Vector2D paddingRatio;
        bool contentValid = false, hasBlur = false;
    };
    class CLease {
      public:
        CLease(CRenderBuffers& pool, std::shared_ptr<SSamples> samples);
        CLease(const CLease&) = delete;
        CLease& operator=(const CLease&) = delete;
        CLease(CLease&&) = delete;
        ~CLease();
        SSamples* operator->() const { return m_samples.get(); }
        explicit operator bool() const { return bool(m_samples); }
      private:
        CRenderBuffers& m_pool;
        std::shared_ptr<SSamples> m_samples;
    };

    struct SBlurScratch {
        SKey key;
        size_t levels;
        std::vector<SP<Render::IFramebuffer>> pyramid;
        SP<Render::IFramebuffer> temporary;
        SP<Render::IFramebuffer> reduction;
        Clock::time_point used;
        uint64_t sequence = 0;
    };
    class CBlurLease {
      public:
        CBlurLease(CRenderBuffers& pool, std::shared_ptr<SBlurScratch> scratch);
        CBlurLease(const CBlurLease&) = delete;
        CBlurLease& operator=(const CBlurLease&) = delete;
        ~CBlurLease();
        SBlurScratch* operator->() const { return m_scratch.get(); }
      private:
        CRenderBuffers& m_pool;
        std::shared_ptr<SBlurScratch> m_scratch;
    };

    ~CRenderBuffers();
    CLease acquire(int width, int height, const SP<Render::IFramebuffer>& source,
                   const std::string& owner = {}, const std::vector<uint64_t>& contentKey = {});
    CLease acquireBackdrop(int width, int height, const SP<Render::IFramebuffer>& source,
                           const std::string& owner, const std::vector<uint64_t>& contentKey, bool transientOnBypass = true);
    void discardBackdrop(const std::string& owner);
    CBlurLease acquireBlur(int width, int height, size_t levels, const SP<Render::IFramebuffer>& source);
    bool prepare(SP<Render::IFramebuffer>& fb, int width, int height, DRMFormat format,
                 const std::string& role, const std::string& owner = "shared");
    void identify(const SP<Render::IFramebuffer>& fb, const std::string& role, const std::string& owner);
    void touch(const SP<Render::IFramebuffer>& fb);
    void release(SP<Render::IFramebuffer>& fb);
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
    std::vector<std::shared_ptr<SSamples>> m_backdropCache;
    std::vector<std::shared_ptr<SBlurScratch>> m_scratchPool;
    std::unordered_map<Render::IFramebuffer*, SRecord> m_records;
    SP<CEventLoopTimer> m_cleanup;
    uint64_t m_nextId = 0, m_sequence = 0, m_hits = 0, m_misses = 0, m_evictions = 0;
    uint64_t m_allocations = 0, m_reallocations = 0, m_failures = 0;
    uint64_t m_peakRetainedBytes = 0;
    uint64_t m_scratchHits = 0, m_scratchMisses = 0, m_scratchEvictions = 0;
    uint64_t m_backdropHits = 0, m_backdropMisses = 0, m_backdropBypasses = 0, m_backdropEvictions = 0;
    uint64_t m_frame = 0;
    int64_t m_monitor = -1;
    Clock::time_point m_frameStart;
    std::deque<double> m_frameTimes;
};
