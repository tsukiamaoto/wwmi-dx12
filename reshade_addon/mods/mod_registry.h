#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>
#include "wwmi_mod.h" // WwmiMod::Mod -- GeoMod
#include "../core/frame_clock.h" // FrameCounter -- GeoMod; g_frame_id -- ClaimAge
#include "../core/ns_hash.h"
#include "../migoto/port_api.h" // PortApi::SectionPreAssign

// ONE LOADED CHARACTER MOD, shared by the hook modules.
struct GeoMod {
    WwmiMod::Mod mod;
    std::wstring fwdmap;
    std::wstring wcl_ns; // THE mod key: their engine's namespace, from PortApi::NamespaceForIni
    uint64_t ns_hash = 0;
    // AST-derived from this mod's [TextureOverride] component sections (no hardcoded variable names):
    std::unordered_map<int, std::vector<PortApi::SectionPreAssign>> comp_det_assigns; // comp_idx -> presence writes
    std::unordered_map<int, std::vector<float*>> comp_en_slots; // comp_idx -> enable-gate vars
    std::vector<float*> det_slots; // deduplicated union of comp_det_assigns slots
    std::vector<float*> en_slots; // deduplicated union of comp_en_slots
    uint32_t last_char_frame = 0; // g_frame_id of the last claimed draw of this unit's character; 0 = never
    float det_at_keys = 0.f; // presence vars sampled at key-poll time, before [Present]'s post pass clears them
    bool geom_enabled = true; // enable-gate vars sampled after [Present]'s post pass; what GeomEnabled answers
    bool buffers_loaded = false; // .buf mesh/shapekey resources adopted on first appearance instead of all 80+ mods at boot
    bool on_screen_prev = false; // det_at_keys > 0 on the previous frame (for instant frame-0 texture pre-enqueue)
    // this unit's share of the heartbeat's sect= / skip= / replaced=
    FrameCounter sect_runs;
    FrameCounter skip_orig;
    FrameCounter replaced;
};

// One mod: the top-most folder under one of d3dx.ini's include_recursive roots holding an ini their engine parsed,
// with every such ini under it. An ini directly in a root is a mod of its own.
struct ModIni {
    std::wstring path; // absolute
    std::wstring ns; // their engine's namespace for it
};
struct DiscoveredMod {
    std::wstring dir; // absolute mod folder
    std::wstring name; // its folder from the root; the ini's file name when it sits in the root
    std::vector<ModIni> inis;
};

class ModRegistry {
public:
    size_t Count() const { return units_.size(); }
    GeoMod& At(size_t mod_id) { return units_[mod_id]; }
    const GeoMod& At(size_t mod_id) const { return units_[mod_id]; }
    std::deque<GeoMod>& Units() { return units_; }
    // The mod whose namespace this is (GeoMod::wcl_ns); -1 for none.
    int IdOfNs(const std::wstring& ns) const;
    int IdOfNsHash(uint64_t ns_hash) const;
    unsigned ParseGen() const { return parse_gen_; }
    const WwmiMod::Component* Comp(int mod_id, int idx) const;
    uint32_t IndexEnd(int mod_id, uint32_t ib_hash) const;
    // WWMI enable-flag compliance: is this mod's geometry meant to be applied right now?
    bool GeomEnabled(int mod_id) const;
    // Live sampling of AST-discovered presence and enable-gate variable slots.
    float SampleDetected(int mod_id) const;
    bool SampleGeomEnabled(int mod_id) const;
    bool SampleCompSwitchedOff(int mod_id, const WwmiMod::Component* wc) const;
    uint32_t ClaimAge(int mod_id) const;
    bool OnScreen(int mod_id) const { return ClaimAge(mod_id) <= 4u; }
    // The geometry units discovered and described from their engine's parse, and the component table filled.
    void Build();
    // Adopt this unit's .buf resources from 3DMigoto on first appearance (Present thread, before PresentPre / Flush).
    void EnsureModBuffersLoaded(int mod_id);
    static std::vector<DiscoveredMod> Discover();
    int DiscoveredCount() const { return discovered_; }

private:
    std::deque<GeoMod> units_;
    std::unordered_map<std::wstring, int> by_ns_;
    std::unordered_map<uint64_t, int> by_ns_hash_;
    unsigned parse_gen_ = 0;
    int discovered_ = 0;
    std::vector<std::unordered_map<uint32_t, uint32_t>> index_end_; // per mod: ib hash -> the last index it declares
};
