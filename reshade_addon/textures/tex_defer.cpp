#include "tex_defer.h"
#include "mru_preload.h"
#include "../instrumentation/diag_gate.h"
#include "../addon/settings.h"
#include "../migoto/dx12_pipeline.h"
#include "../mods/character_recognition.h"
#include "../core/log.h"
#include <windows.h>
#include <d3d11.h>
#include <algorithm>
#include <iterator>
#include <string>
#include "../mods/mod_set.h"

TexDefer g_tex_defer;

namespace {
int ModOfNamespace(const std::wstring& ns)
{
    ModSetRef set_ref;
    for (int md = 0; md < (int)Mods().Count(); ++md)
        if (_wcsicmp(Mods().At(md).wcl_ns.c_str(), ns.c_str()) == 0) return md;
    return -1;
}
} // namespace

std::atomic<uint32_t>& TexDefer::Slot(int mod)
{
    Table* t = table_.load(std::memory_order_relaxed);
    if (!t || mod >= t->n) {
        int n = t ? t->n : 64;
        while (n <= mod) n *= 2;
        auto grown = std::make_unique<Table>();
        grown->v = std::make_unique<std::atomic<uint32_t>[]>(n);
        grown->n = n;
        for (int i = 0; i < n; ++i)
            grown->v[i].store(t && i < t->n ? t->v[i].load(std::memory_order_relaxed) : 0, std::memory_order_relaxed);
        t = grown.get();
        tables_.push_back(std::move(grown));
        table_.store(t, std::memory_order_release);
    }
    return t->v[mod];
}

std::vector<std::wstring> NamespacesOnScreen()
{
    std::vector<std::wstring> namespaces;
    ModSetRef set_ref;
    for (int md = 0; md < (int)Mods().Count(); ++md) {
        if (!Mods().OnScreen(md) || g_tex_defer.Pending(md)) continue;
        const std::wstring& ns = Mods().At(md).wcl_ns;
        if (!ns.empty() && std::find(namespaces.begin(), namespaces.end(), ns) == namespaces.end())
            namespaces.push_back(ns);
    }
    return namespaces;
}

void TexDefer::Start()
{
    PortApi::SetTextureDeferral(&TexDefer::Offer, this);
}

bool TexDefer::Offer(const PortApi::DeferredTexture& t, void* user)
{
    if (!g_settings.tex_deferred) return false;
    TexDefer* self = static_cast<TexDefer*>(user);
    const unsigned gen = PortApi::LoadGeneration();
    const int mod = ModOfNamespace(PortApi::NamespaceOfResource(t.fq_name));
    {
        std::lock_guard l(self->mtx_);
        if (self->stop_) return false;
        if (gen != self->gen_.load(std::memory_order_relaxed)) {
            self->gen_.store(gen, std::memory_order_relaxed);
            if (Table* tb = self->table_.load(std::memory_order_relaxed))
                for (int i = 0; i < tb->n; ++i) tb->v[i].store(0, std::memory_order_relaxed);
            for (auto r = self->returns_.begin(); r != self->returns_.end();)
                r = r->second.gen != gen ? self->returns_.erase(r) : std::next(r);
        }
        // Counted here, on the drawing thread, so the draw that asked already sees its mod as loading.
        if (mod >= 0) self->Slot(mod).fetch_add(1, std::memory_order_acq_rel);
        self->in_flight_.fetch_add(1, std::memory_order_acq_rel);
        Job job{ t, mod, gen };
        const auto back = self->returns_.find(t.fq_name);
        if (back != self->returns_.end()) {
            if (back->second.gen == gen) job.tex.maxsize = back->second.maxsize;
            self->returns_.erase(back);
        }
        if (job.tex.low_priority) {
            self->queue_.push_back(std::move(job));
        } else {
            // Live on-screen requests cut ahead of background preload jobs while preserving FIFO order among themselves.
            auto pos = std::find_if(
                self->queue_.begin(), self->queue_.end(), [](const Job& j) { return j.tex.low_priority; });
            self->queue_.insert(pos, std::move(job));
        }
        if (self->threads_.empty()) {
            const unsigned hw = std::thread::hardware_concurrency();
            const unsigned n_workers = (std::clamp)(hw ? hw / 2u : 2u, 2u, 4u);
            self->threads_.reserve(n_workers);
            for (unsigned i = 0; i < n_workers; ++i) self->threads_.emplace_back(&TexDefer::Run, self);
        }
    }
    self->cv_.notify_one();
    if (!t.low_priority) g_mru_preload.NoteLiveTexture(t.fq_name);
    return true;
}

void TexDefer::CompleteJobLocked(int mod, unsigned gen, unsigned made, unsigned stale, unsigned smaller,
    long long load_ticks, long long locked, BurstStats* out_done, bool* out_log)
{
    if (mod >= 0 && gen == gen_.load(std::memory_order_relaxed)) Slot(mod).fetch_sub(1, std::memory_order_acq_rel);
    const unsigned remaining = in_flight_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    burst_.made += made;
    burst_.stale += stale;
    burst_.smaller += smaller;
    burst_.locked += locked;
    if (load_ticks > burst_.longest) burst_.longest = load_ticks;
    if (remaining == 0 && queue_.empty() && ready_.empty() && burst_.active) {
        *out_done = std::move(burst_);
        burst_ = BurstStats{};
        *out_log = true;
    }
}

void TexDefer::LogBurst(const BurstStats& done_stats, long long freq)
{
    if (done_stats.namespaces.empty() || freq <= 0) return;
    LARGE_INTEGER t1{};
    QueryPerformanceCounter(&t1);
    const double ms = 1000.0 / (double)freq;
    logger::write(
        "TEXDEFER '%ls'%s: loaded %u of %u texture(s), %u of them asked for at the size they were let go at, "
        "in %.0f ms, %.0f ms of it under the engine lock (longest texture %.0f ms)%s",
        done_stats.namespaces.front().c_str(), done_stats.namespaces.size() > 1 ? " and others" : "",
        done_stats.made, done_stats.total, done_stats.smaller, (double)(t1.QuadPart - done_stats.t0) * ms,
        (double)done_stats.locked * ms, (double)done_stats.longest * ms,
        done_stats.stale ? " -- some were asked for before the inis were reloaded, not loaded" : "");
}

void TexDefer::PublishCompleted()
{
    std::vector<ReadyItem> due;
    {
        std::lock_guard l(mtx_);
        if (ready_.empty()) return;
        const unsigned live_gen = PortApi::LoadGeneration();
        std::deque<ReadyItem> keep;
        for (ReadyItem& it : ready_) {
            if (stop_ || it.gen != live_gen || MigotoPort::AsyncCopyPassed(it.copy_fence))
                due.push_back(std::move(it));
            else
                keep.push_back(std::move(it));
        }
        ready_.swap(keep);
    }
    if (due.empty()) return;

    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    for (ReadyItem& it : due) {
        unsigned made = 0;
        unsigned stale = 0;
        unsigned smaller = 0;
        LONGLONG locked = 0;
        if (PortApi::LoadGeneration() != it.gen) {
            stale = 1;
            if (it.res) it.res->Release();
        } else if (it.res) {
            LARGE_INTEGER b{};
            LARGE_INTEGER c{};
            QueryPerformanceCounter(&b);
            bool stopping = false;
            {
                std::lock_guard l(mtx_);
                stopping = stop_;
            }
            if (!stopping && PortApi::PublishDeferredTexture(it.fq_name.c_str(), it.res)) {
                made = 1;
                if (it.maxsize) smaller = 1;
            } else {
                it.res->Release();
            }
            QueryPerformanceCounter(&c);
            locked = c.QuadPart - b.QuadPart;
        }
        if (made) loaded_.fetch_add(made, std::memory_order_acq_rel);

        BurstStats done_stats{};
        bool log_burst = false;
        {
            std::lock_guard l(mtx_);
            CompleteJobLocked(it.mod, it.gen, made, stale, smaller, it.load_ticks, locked, &done_stats, &log_burst);
        }
        if (log_burst) LogBurst(done_stats, freq.QuadPart);
    }
}

void TexDefer::Run()
{
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    for (;;) {
        Job job{};
        {
            std::unique_lock l(mtx_);
            cv_.wait(l, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
            if (!burst_.active) {
                LARGE_INTEGER t0{};
                QueryPerformanceCounter(&t0);
                burst_ = BurstStats{};
                burst_.active = true;
                burst_.t0 = t0.QuadPart;
            }
            ++burst_.total;
            std::wstring ns = PortApi::NamespaceOfResource(job.tex.fq_name);
            if (std::find(burst_.namespaces.begin(), burst_.namespaces.end(), ns) == burst_.namespaces.end())
                burst_.namespaces.push_back(std::move(ns));
        }

        unsigned made = 0;
        unsigned stale = 0;
        unsigned smaller = 0;
        LONGLONG locked = 0;
        LONGLONG load_ticks = 0;
        if (PortApi::LoadGeneration() != job.gen) {
            stale = 1;
        } else {
            LARGE_INTEGER a{};
            LARGE_INTEGER b{};
            LARGE_INTEGER c{};
            QueryPerformanceCounter(&a);
            uint64_t copy_fv = 0;
            ID3D11Resource* res = PortApi::LoadDeferredTexture(job.tex, /*async_copy=*/true, &copy_fv);
            QueryPerformanceCounter(&b);
            load_ticks = b.QuadPart - a.QuadPart;
            if (res && copy_fv != 0 && !MigotoPort::AsyncCopyPassed(copy_fv)) {
                // Async COPY-queue DMA is still in flight: hand off to Present-thread PublishCompleted() so this
                // worker immediately grabs the next DDS file without waiting for the GPU fence.
                bool handoff = false;
                {
                    std::lock_guard l(mtx_);
                    if (!stop_) {
                        ready_.push_back({ job.tex.fq_name, res, copy_fv, job.mod, job.gen, job.tex.maxsize, load_ticks });
                        handoff = true;
                    }
                }
                if (handoff) continue;
                res->Release();
                res = nullptr;
            }
            if (res) {
                bool stopping = false;
                {
                    std::lock_guard l(mtx_);
                    stopping = stop_;
                }
                if (!stopping && PortApi::PublishDeferredTexture(job.tex.fq_name.c_str(), res)) {
                    made = 1;
                    if (job.tex.maxsize) smaller = 1;
                } else {
                    res->Release();
                }
            }
            QueryPerformanceCounter(&c);
            locked = c.QuadPart - b.QuadPart;
        }
        if (made) loaded_.fetch_add(made, std::memory_order_acq_rel);

        BurstStats done_stats{};
        bool log_burst = false;
        {
            std::lock_guard l(mtx_);
            CompleteJobLocked(job.mod, job.gen, made, stale, smaller, load_ticks, locked, &done_stats, &log_burst);
        }
        if (log_burst) LogBurst(done_stats, freq.QuadPart);
    }
}

void TexDefer::ReturnAt(const std::wstring& fq_name, unsigned maxsize)
{
    std::lock_guard l(mtx_);
    returns_[fq_name] = { maxsize, PortApi::LoadGeneration() };
}

void TexDefer::Stop()
{
    PortApi::SetTextureDeferral(nullptr, nullptr);
    std::vector<std::thread> to_join;
    {
        std::lock_guard l(mtx_);
        stop_ = true;
        queue_.clear();
        to_join.swap(threads_);
    }
    cv_.notify_all();
    for (std::thread& t : to_join)
        if (t.joinable()) t.join();
    MigotoPort::AsyncCopyShutdown();
    std::deque<ReadyItem> leftover;
    {
        std::lock_guard l(mtx_);
        leftover.swap(ready_);
    }
    for (ReadyItem& it : leftover)
        if (it.res) it.res->Release();
    in_flight_.store(0, std::memory_order_release);
}
