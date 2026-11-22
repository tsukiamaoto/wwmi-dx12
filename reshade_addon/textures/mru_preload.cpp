#include "mru_preload.h"
#include "tex_defer.h"
#include "tex_gc.h"
#include "../core/frame_clock.h"
#include "../core/log.h"
#include "../migoto/port_api.h"
#include "../mods/mod_set.h"
#include <windows.h>
#include <algorithm>
#include <cwctype>

MruPreload g_mru_preload;

namespace {

constexpr size_t kMaxRecentMods = 12;
constexpr size_t kMaxTexPerMod = 64;
constexpr uint64_t kSaveDebounceMs = 5000;
constexpr uint64_t kStepIntervalMs = 400;
constexpr uint64_t kMinHeadroomFloorBytes = 1536ull << 20; // >= 1.5 GiB floor on smaller GPUs
constexpr uint64_t kMinHeadroomCeilBytes = 3ull << 30;     // cap headroom reservation at 3 GiB on large GPUs

std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Utf8ToWide(const char* p, size_t len)
{
    if (!p || !len) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, p, (int)len, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, p, (int)len, w.data(), n);
    return w;
}

bool ContainsInsensitive(const std::vector<std::wstring>& v, const std::wstring& s)
{
    for (const std::wstring& x : v)
        if (_wcsicmp(x.c_str(), s.c_str()) == 0) return true;
    return false;
}

} // namespace

std::wstring MruPreload::Lower(const std::wstring& s)
{
    std::wstring r = s;
    for (wchar_t& c : r) c = (wchar_t)towlower(c);
    return r;
}

void MruPreload::Init(const std::wstring& dir)
{
    if (dir.empty()) return;
    const std::wstring path = dir + L"\\recent_mods.txt";

    std::vector<Entry> loaded;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz{};
        if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < (4ll << 20)) {
            std::string buf((size_t)sz.QuadPart, '\0');
            DWORD got = 0;
            if (ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr) && got > 0) {
                buf.resize(got);
                size_t pos = 0;
                while (pos < buf.size()) {
                    size_t end = buf.find('\n', pos);
                    if (end == std::string::npos) end = buf.size();
                    size_t line_end = end;
                    if (line_end > pos && buf[line_end - 1] == '\r') --line_end;
                    if (line_end - pos > 2 && buf[pos + 1] == '\t') {
                        const char tag = buf[pos];
                        std::wstring val = Utf8ToWide(buf.data() + pos + 2, line_end - pos - 2);
                        if (!val.empty()) {
                            if (tag == 'M' && loaded.size() < kMaxRecentMods) {
                                Entry e;
                                e.ns = std::move(val);
                                e.ns_lower = Lower(e.ns);
                                loaded.push_back(std::move(e));
                            } else if (tag == 'T' && !loaded.empty() && loaded.back().tex_fqs.size() < kMaxTexPerMod) {
                                if (!ContainsInsensitive(loaded.back().tex_fqs, val))
                                    loaded.back().tex_fqs.push_back(std::move(val));
                            }
                        }
                    }
                    pos = end + 1;
                }
            }
        }
        CloseHandle(h);
    }

    std::lock_guard<std::mutex> l(mtx_);
    file_path_ = path;
    mru_ = std::move(loaded);
    if (!mru_.empty())
        logger::write("PRELOAD: loaded %zu recent character mod(s) from %ls", mru_.size(), file_path_.c_str());
}

void MruPreload::NoteModOnScreen(const std::wstring& ns)
{
    if (ns.empty()) return;
    const std::wstring low = Lower(ns);
    std::lock_guard<std::mutex> l(mtx_);

    auto it = std::find_if(mru_.begin(), mru_.end(), [&](const Entry& e) { return e.ns_lower == low; });
    Entry cur;
    if (it != mru_.end()) {
        cur = std::move(*it);
        mru_.erase(it);
    } else {
        cur.ns = ns;
        cur.ns_lower = low;
    }

    auto sit = session_tex_.find(low);
    if (sit != session_tex_.end() && !sit->second.empty()) {
        if (!cur.session_refreshed) {
            cur.tex_fqs.clear();
            cur.session_refreshed = true;
        }
        for (const std::wstring& fq : sit->second) {
            if (cur.tex_fqs.size() >= kMaxTexPerMod) break;
            if (!ContainsInsensitive(cur.tex_fqs, fq)) cur.tex_fqs.push_back(fq);
        }
    }

    mru_.insert(mru_.begin(), std::move(cur));
    if (mru_.size() > kMaxRecentMods) mru_.resize(kMaxRecentMods);
    dirty_ = true;
    last_dirty_ms_ = GetTickCount64();
}

void MruPreload::NoteLiveTexture(const std::wstring& fq_name)
{
    if (fq_name.empty()) return;
    const std::wstring ns = PortApi::NamespaceOfResource(fq_name);
    if (ns.empty()) return;
    const std::wstring low = Lower(ns);

    std::lock_guard<std::mutex> l(mtx_);
    auto& svec = session_tex_[low];
    if (svec.size() < kMaxTexPerMod && !ContainsInsensitive(svec, fq_name)) svec.push_back(fq_name);

    auto it = std::find_if(mru_.begin(), mru_.end(), [&](const Entry& e) { return e.ns_lower == low; });
    if (it == mru_.end()) return;

    if (!it->session_refreshed) {
        it->tex_fqs.clear();
        it->session_refreshed = true;
    }
    if (it->tex_fqs.size() < kMaxTexPerMod && !ContainsInsensitive(it->tex_fqs, fq_name)) {
        it->tex_fqs.push_back(fq_name);
        dirty_ = true;
        last_dirty_ms_ = GetTickCount64();
    }
}

void MruPreload::SaveNow(const std::vector<Entry>& snap)
{
    std::wstring path;
    {
        std::lock_guard<std::mutex> l(mtx_);
        path = file_path_;
    }
    if (path.empty()) return;

    std::string out;
    out.reserve(snap.size() * 512);
    for (const Entry& e : snap) {
        out += "M\t";
        out += WideToUtf8(e.ns);
        out += "\r\n";
        for (const std::wstring& fq : e.tex_fqs) {
            out += "T\t";
            out += WideToUtf8(fq);
            out += "\r\n";
        }
    }

    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    const BOOL ok = out.empty() || (WriteFile(h, out.data(), (DWORD)out.size(), &written, nullptr) && written == out.size());
    CloseHandle(h);
    if (ok)
        MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    else
        DeleteFileW(tmp.c_str());
}

void MruPreload::SaveOnExit()
{
    std::vector<Entry> snap;
    {
        std::lock_guard<std::mutex> l(mtx_);
        if (!dirty_ || file_path_.empty()) return;
        dirty_ = false;
        snap = mru_;
    }
    SaveNow(snap);
}

void MruPreload::Tick()
{
    const uint64_t now = GetTickCount64();
    {
        std::vector<Entry> snap;
        {
            std::lock_guard<std::mutex> l(mtx_);
            if (dirty_ && now - last_dirty_ms_ >= kSaveDebounceMs) {
                dirty_ = false;
                snap = mru_;
            }
        }
        if (!snap.empty()) SaveNow(snap);
    }

    if (g_frame_id < 120u || Mods().Count() == 0) return;

    const unsigned gen = PortApi::LoadGeneration();
    if (gen != preload_gen_) {
        preload_gen_ = gen;
        cursor_ = 0;
        done_ = false;
        preloaded_ns_.clear();
        next_step_ms_ = now + 1500u;
    }
    if (done_ || now < next_step_ms_ || g_tex_defer.Loading()) return;

    const TexGc::Reading& r = g_tex_gc.Now();
    if (!r.valid) return;
    if (g_tex_gc.Tight()) {
        done_ = true;
        logger::write("PRELOAD: stopped -- VRAM pressure is tight");
        return;
    }
    const uint64_t headroom = (std::clamp)(r.budget / 5u, kMinHeadroomFloorBytes, kMinHeadroomCeilBytes);
    const uint64_t mod_cap = r.budget / 2u;
    if (r.budget <= headroom + (1ull << 30) || r.used + headroom >= r.budget || r.mods >= mod_cap) {
        done_ = true;
        constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
        logger::write("PRELOAD: paused -- VRAM ceiling reached (used %.2f / budget %.2f GiB, mod tex %.2f / cap %.2f GiB)",
            (double)r.used / kGiB, (double)r.budget / kGiB, (double)r.mods / kGiB, (double)mod_cap / kGiB);
        return;
    }

    std::vector<Entry> snap;
    {
        std::lock_guard<std::mutex> l(mtx_);
        snap = mru_;
    }

    while (cursor_ < snap.size()) {
        const Entry& e = snap[cursor_++];
        if (!preloaded_ns_.insert(e.ns_lower).second) continue;
        const int md = Mods().IdOfNs(e.ns);
        if (md < 0) continue;

        const bool had_bufs = Mods().At(md).buffers_loaded;
        if (!had_bufs) Mods().EnsureModBuffersLoaded(md);

        unsigned queued = 0;
        for (const std::wstring& fq : e.tex_fqs)
            if (PortApi::PreloadDeferredTexture(fq.c_str())) ++queued;

        if (!had_bufs || queued > 0) {
            constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
            logger::write("PRELOAD: queued recent mod '%ls' (%s, %u texture(s); VRAM %.2f/%.2f GiB)", e.ns.c_str(),
                had_bufs ? "bufs ready" : "bufs loaded", queued, (double)r.used / kGiB, (double)r.budget / kGiB);
            next_step_ms_ = now + kStepIntervalMs;
            return;
        }
    }

    done_ = true;
}
