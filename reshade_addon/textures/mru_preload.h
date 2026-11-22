#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Persists recently used character mods (and the exact .dds resources their active outfit loaded) to
// `<wwmi_dx12>\recent_mods.txt` and gently preloads them in the background on future launches while
// VRAM headroom allows, yielding immediately to live on-screen switches.
class MruPreload {
public:
    void Init(const std::wstring& dir);
    void NoteModOnScreen(const std::wstring& ns);
    void NoteLiveTexture(const std::wstring& fq_name);
    void Tick(); // Present thread, once per frame
    void SaveOnExit();

private:
    struct Entry {
        std::wstring ns;
        std::wstring ns_lower;
        std::vector<std::wstring> tex_fqs;
        bool session_refreshed = false;
    };

    static std::wstring Lower(const std::wstring& s);
    void SaveNow(const std::vector<Entry>& snap);

    std::mutex mtx_;
    std::wstring file_path_;
    std::vector<Entry> mru_;
    std::unordered_map<std::wstring, std::vector<std::wstring>> session_tex_; // ns_lower -> live fq_names seen this session
    bool dirty_ = false;
    uint64_t last_dirty_ms_ = 0;

    // Present-thread scheduler state
    unsigned preload_gen_ = 0;
    size_t cursor_ = 0;
    bool done_ = false;
    uint64_t next_step_ms_ = 0;
    std::unordered_set<std::wstring> preloaded_ns_;
};

extern MruPreload g_mru_preload;
