// SPDX-License-Identifier: MIT
// Standalone D3D12 + Official NGX DLSS-RR (nvngx_dlssd.dll v310.9.1.0) Reference Runner.
// Supports:
//   --self-test
//   [--suite <v1|v2_motion>] --dump-fixture-hashes <cases.json>
//   [--suite <v1|v2_motion>] --verify-fixtures <cases.json>
//   [--suite <v1|v2_motion>] --smoke --dlssd <nvngx_dlssd.dll> --out <dir>
//   [--suite <v1|v2_motion>] --pass <quality-core|timing-core|quality-ext|probes> --dlssd <nvngx_dlssd.dll> --out <dir> [--case <id>]
//   --profile-target <1080p_qual|4k_qual|4k_perf> --dlssd <nvngx_dlssd.dll> --out <dir> [--profile-frame 32]

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <winreg.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#ifdef RR_USE_OFFICIAL_NGX_SDK
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs_dlssd.h>
#else
#include "rr_ngx_abi.hpp"
#endif
#include "rr_ref_scenes.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace
{

bool g_process_exit_teardown = false;

inline constexpr const char* kExpectedDlssdVersion = "310.9.1.0";
inline constexpr uint64_t kExpectedDlssdBytes = 48343664ull;
inline constexpr const char* kExpectedDlssdSha256 =
    "4bc7ea5fcb2f32cf86bc2cb072e8d2860914a0be511cbdcb2fc206c1d9d83b80";
inline constexpr const char* kOpenRrWeightSliceNote =
    "OpenRR internal rr2 weight+immediates+LUT slice SHA-256 (not whole PE file): "
    "c7a2361f990391f3589601619d77f15ce92d46ad2a459235f9cfca5913fad05a";

constexpr D3D12_RESOURCE_STATES kSrvReadState = static_cast<D3D12_RESOURCE_STATES>(
    static_cast<unsigned>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) |
    static_cast<unsigned>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

void RuntimeTrace(const char* message) noexcept
{
    HANDLE f = CreateFileW(L"rr_runtime_trace.log", FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD n = 0; WriteFile(f, message, static_cast<DWORD>(std::strlen(message)), &n, nullptr);
        WriteFile(f, "\r\n", 2, &n, nullptr); CloseHandle(f);
    }
}

void Require(bool ok, const std::string& msg)
{
    if (!ok)
        throw std::runtime_error(msg);
}

void CheckHr(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s failed (HRESULT=0x%08lX)", what, static_cast<unsigned long>(hr));
        throw std::runtime_error(buf);
    }
}

std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

fs::path Utf8ToPath(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return fs::path(s);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return fs::path(w);
}

std::string JsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                out += buf;
            }
            else
            {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return out;
}

std::string HexU32(uint32_t v)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08X", v);
    return buf;
}

std::string HexU64(uint64_t v)
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%016llX", static_cast<unsigned long long>(v));
    return buf;
}

std::string Sha256FileHex(const fs::path& p, uint64_t* out_size = nullptr)
{
    std::ifstream f(p, std::ios::binary);
    Require(static_cast<bool>(f), "Failed to open file for SHA-256: " + WideToUtf8(p.wstring()));
    rr_ref::Sha256 sha;
    std::vector<char> buf(1 << 20);
    uint64_t total = 0;
    while (f)
    {
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize got = f.gcount();
        if (got > 0)
        {
            sha.Update(buf.data(), static_cast<size_t>(got));
            total += static_cast<uint64_t>(got);
        }
    }
    if (out_size) *out_size = total;
    return sha.FinalHex();
}

std::string QueryPeFileVersion(const fs::path& p)
{
    DWORD dummy = 0;
    const std::wstring ws = p.wstring();
    const DWORD sz = GetFileVersionInfoSizeW(ws.c_str(), &dummy);
    if (sz == 0) return "unknown";
    std::vector<uint8_t> data(sz);
    if (!GetFileVersionInfoW(ws.c_str(), 0, sz, data.data()))
        return "unknown";
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT ffi_len = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<LPVOID*>(&ffi), &ffi_len) || !ffi)
        return "unknown";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                  static_cast<unsigned>(HIWORD(ffi->dwFileVersionMS)),
                  static_cast<unsigned>(LOWORD(ffi->dwFileVersionMS)),
                  static_cast<unsigned>(HIWORD(ffi->dwFileVersionLS)),
                  static_cast<unsigned>(LOWORD(ffi->dwFileVersionLS)));
    return buf;
}

int QueryWindowsHagsMode()
{
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return -1;
    DWORD val = 0, sz = sizeof(val), type = 0;
    const LONG rc = RegQueryValueExW(hk, L"HwSchMode", nullptr, &type, reinterpret_cast<LPBYTE>(&val), &sz);
    RegCloseKey(hk);
    if (rc != ERROR_SUCCESS || type != REG_DWORD)
        return -1; // OS default (typically enabled on Win11)
    return static_cast<int>(val); // 2 = Enabled, 1 = Disabled
}

std::wstring ReadRegString(HKEY root, const wchar_t* subkey, const wchar_t* value_name)
{
    HKEY hk = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return {};
    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW(hk, value_name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t))
    {
        RegCloseKey(hk);
        return {};
    }
    std::wstring raw(bytes / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(hk, value_name, nullptr, &type, reinterpret_cast<LPBYTE>(raw.data()), &bytes) != ERROR_SUCCESS)
    {
        RegCloseKey(hk);
        return {};
    }
    RegCloseKey(hk);
    while (!raw.empty() && raw.back() == L'\0')
        raw.pop_back();
    if (type == REG_EXPAND_SZ && !raw.empty())
    {
        const DWORD need = ExpandEnvironmentStringsW(raw.c_str(), nullptr, 0);
        if (need > 0)
        {
            std::wstring exp(need, L'\0');
            ExpandEnvironmentStringsW(raw.c_str(), exp.data(), need);
            while (!exp.empty() && exp.back() == L'\0')
                exp.pop_back();
            return exp;
        }
    }
    return raw;
}

fs::path LocateNgxCoreLoader(const fs::path& explicit_loader)
{
    if (!explicit_loader.empty())
    {
        Require(fs::exists(explicit_loader), "Explicit --ngx-loader does not exist: " + WideToUtf8(explicit_loader.wstring()));
        return fs::absolute(explicit_loader);
    }

    // 1. Official NGXCore registry key
    const std::wstring reg_ngx = ReadRegString(HKEY_LOCAL_MACHINE,
                                               L"SYSTEM\\CurrentControlSet\\Services\\nvlddmkm\\NGXCore",
                                               L"NGXPath");
    if (!reg_ngx.empty())
    {
        const fs::path p1 = fs::path(reg_ngx) / L"_nvngx.dll";
        if (fs::exists(p1)) return fs::absolute(p1);
        const fs::path p2 = fs::path(reg_ngx) / L"nvngx.dll";
        if (fs::exists(p2)) return fs::absolute(p2);
    }

    // 2. DriverStore scan (pick most recently modified _nvngx.dll in nv* folders)
    wchar_t sys_dir[MAX_PATH] {};
    if (GetSystemDirectoryW(sys_dir, MAX_PATH) > 0)
    {
        const fs::path sys_p(sys_dir);
        const fs::path direct = sys_p / L"_nvngx.dll";
        if (fs::exists(direct)) return fs::absolute(direct);

        const fs::path repo = sys_p / L"DriverStore" / L"FileRepository";
        std::error_code ec;
        if (fs::exists(repo, ec))
        {
            fs::path best;
            fs::file_time_type best_time {};
            bool have_best = false;
            for (const auto& entry : fs::directory_iterator(repo, ec))
            {
                if (!entry.is_directory(ec)) continue;
                const std::wstring dn = entry.path().filename().wstring();
                if (dn.size() < 2 || (dn[0] != L'n' && dn[0] != L'N') || (dn[1] != L'v' && dn[1] != L'V'))
                    continue;
                const fs::path cand = entry.path() / L"_nvngx.dll";
                if (fs::exists(cand, ec))
                {
                    const auto wt = fs::last_write_time(cand, ec);
                    if (!have_best || wt > best_time)
                    {
                        best = cand;
                        best_time = wt;
                        have_best = true;
                    }
                }
            }
            if (have_best) return fs::absolute(best);
        }
    }

    throw std::runtime_error(
        "Could not locate NVIDIA _nvngx.dll via HKLM\\...\\NGXCore\\NGXPath or DriverStore. "
        "Pass --ngx-loader <path-to-_nvngx.dll> or build with -NgxSdkDir.");
}

// =====================================================================================================================
// Canonical Benchmark & Capture Suite Definition
// =====================================================================================================================

enum class PassGroup
{
    QualityCore,
    TimingCore,
    QualityExt,
    Probes,
};

inline const char* PassGroupName(PassGroup g) noexcept
{
    switch (g)
    {
    case PassGroup::QualityCore: return "quality-core";
    case PassGroup::TimingCore:  return "timing-core";
    case PassGroup::QualityExt:  return "quality-ext";
    case PassGroup::Probes:      return "probes";
    }
    return "unknown";
}

struct SuiteCase
{
    std::string id;
    PassGroup group = PassGroup::QualityCore;
    rr_ref::SceneId scene = rr_ref::SceneId::S0_BenchParity;
    uint32_t guide_mask = rr_ref::kGuideNone;
    uint32_t alloc_rw = 1280;
    uint32_t alloc_rh = 720;
    uint32_t steady_rw = 1280;
    uint32_t steady_rh = 720;
    uint32_t out_w = 1920;
    uint32_t out_h = 1080;
    int32_t perf_quality = NVSDK_NGX_PerfQuality_Value_MaxQuality;
    uint32_t preset_hint = NVSDK_NGX_RayReconstruction_Hint_Render_Preset_F;
    bool has_drs = false;
    uint32_t warmup_frames = 0;   // Timing only
    uint32_t steady_frames = 64;  // Quality: steady frames (0..63); Timing: measured steady frames
    uint32_t drs_frames = 0;      // Quality: 16 frames (64..71 Reset=0, 72..79 Reset=1); Timing: 64 frames
    uint32_t rounds = 1;          // Timing rounds
    std::vector<uint32_t> save_frames;
    rr_ref::V2FrameModifiers v2_mod {};
    std::string pair_ref_id;
    bool expect_equiv_p00 = false;
};

rr_ref::FixtureSuite ParseFixtureSuite(const std::string& s)
{
    if (s == "v1" || s == rr_ref::kFixtureVersion)
        return rr_ref::FixtureSuite::V1;
    if (s == "v2_motion" || s == "v2-motion" || s == rr_ref::kFixtureVersionV2Motion)
        return rr_ref::FixtureSuite::V2_Motion;
    throw std::runtime_error("Unknown --suite value: " + s + " (expected v1 or v2_motion)");
}

std::vector<SuiteCase> BuildCanonicalSuite()
{
    using rr_ref::SceneId;
    const std::vector<uint32_t> k10 = {0, 1, 2, 4, 8, 15, 31, 63, 67, 75};
    const std::vector<uint32_t> k8  = {0, 1, 2, 4, 8, 15, 31, 63};
    const std::vector<uint32_t> k6  = {0, 1, 4, 15, 31, 63};
    const std::vector<uint32_t> k4  = {0, 4, 15, 63};
    const std::vector<uint32_t> kp  = {15};

    std::vector<SuiteCase> c;

    // 1. Quality Core (S0 / g0_core7, Preset F = 6)
    c.push_back({"Q_S0_1080p_qual", PassGroup::QualityCore, SceneId::S0_BenchParity, 0u,
                 1280, 720, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, true, 0, 64, 16, 1, k10});
    c.push_back({"Q_S0_1440p_qual", PassGroup::QualityCore, SceneId::S0_BenchParity, 0u,
                 1706, 960, 1706, 960, 2560, 1440, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, true, 0, 64, 16, 1, k10});
    c.push_back({"Q_S0_4k_qual", PassGroup::QualityCore, SceneId::S0_BenchParity, 0u,
                 2560, 1440, 2560, 1440, 3840, 2160, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, true, 0, 64, 16, 1, k10});
    c.push_back({"Q_S0_4k_perf", PassGroup::QualityCore, SceneId::S0_BenchParity, 0u,
                 1920, 1080, 1920, 1080, 3840, 2160, NVSDK_NGX_PerfQuality_Value_MaxPerf, 6u, true, 0, 64, 16, 1, k10});
    c.push_back({"Q_S0_1080p_dlaa", PassGroup::QualityCore, SceneId::S0_BenchParity, 0u,
                 1920, 1080, 1920, 1080, 1920, 1080, NVSDK_NGX_PerfQuality_Value_DLAA, 6u, false, 0, 64, 0, 1, k8});
    c.push_back({"Q_S0_4k_ultrap", PassGroup::QualityCore, SceneId::S0_BenchParity, 0u,
                 1280, 720, 1280, 720, 3840, 2160, NVSDK_NGX_PerfQuality_Value_UltraPerformance, 6u, false, 0, 64, 0, 1, k8});

    // 2. Timing Core (S0 / g0_core7, Preset F = 6, zero readback)
    c.push_back({"T_S0_1080p_qual", PassGroup::TimingCore, SceneId::S0_BenchParity, 0u,
                 1280, 720, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, true, 120, 240, 64, 2, {}});
    c.push_back({"T_S0_1440p_qual", PassGroup::TimingCore, SceneId::S0_BenchParity, 0u,
                 1706, 960, 1706, 960, 2560, 1440, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, true, 120, 240, 64, 2, {}});
    c.push_back({"T_S0_4k_qual", PassGroup::TimingCore, SceneId::S0_BenchParity, 0u,
                 2560, 1440, 2560, 1440, 3840, 2160, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, true, 120, 240, 64, 2, {}});
    c.push_back({"T_S0_4k_perf", PassGroup::TimingCore, SceneId::S0_BenchParity, 0u,
                 1920, 1080, 1920, 1080, 3840, 2160, NVSDK_NGX_PerfQuality_Value_MaxPerf, 6u, true, 120, 240, 64, 2, {}});
    c.push_back({"T_S0_1080p_dlaa", PassGroup::TimingCore, SceneId::S0_BenchParity, 0u,
                 1920, 1080, 1920, 1080, 1920, 1080, NVSDK_NGX_PerfQuality_Value_DLAA, 6u, false, 120, 240, 0, 1, {}});
    c.push_back({"T_S0_4k_ultrap", PassGroup::TimingCore, SceneId::S0_BenchParity, 0u,
                 1280, 720, 1280, 720, 3840, 2160, NVSDK_NGX_PerfQuality_Value_UltraPerformance, 6u, false, 120, 240, 0, 1, {}});

    // 3. Quality Extended (S1 temporal Halton + S2 HDR stress, g0_core7)
    c.push_back({"Q_S1_1080p_qual", PassGroup::QualityExt, SceneId::S1_TemporalHalton, 0u,
                 1280, 720, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, true, 0, 64, 16, 1, k10});
    c.push_back({"Q_S1_4k_qual", PassGroup::QualityExt, SceneId::S1_TemporalHalton, 0u,
                 2560, 1440, 2560, 1440, 3840, 2160, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, false, 0, 64, 0, 1, k6});
    c.push_back({"Q_S2_1080p_qual", PassGroup::QualityExt, SceneId::S2_HdrStress, 0u,
                 1280, 720, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, false, 0, 64, 0, 1, k8});
    c.push_back({"Q_S2_4k_perf", PassGroup::QualityExt, SceneId::S2_HdrStress, 0u,
                 1920, 1080, 1920, 1080, 3840, 2160, NVSDK_NGX_PerfQuality_Value_MaxPerf, 6u, false, 0, 64, 0, 1, k4});

    // 4. Single-variable Non-blocking Capability & Guide Probes (16 frames 0..15, save f015)
    c.push_back({"P00_baseline_s1_g0", PassGroup::Probes, SceneId::S1_TemporalHalton, rr_ref::kGuideNone,
                 1280, 720, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, false, 0, 16, 0, 1, kp});
    c.push_back({"P01_guide_disocc", PassGroup::Probes, SceneId::S1_TemporalHalton, rr_ref::kGuideDisocclusion,
                 1280, 720, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, false, 0, 16, 0, 1, kp});
    c.push_back({"P02_guide_transp_rgba", PassGroup::Probes, SceneId::S1_TemporalHalton, rr_ref::kGuideTranspLayer,
                 1280, 720, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, false, 0, 16, 0, 1, kp});
    c.push_back({"P03_guide_transp_tlmv", PassGroup::Probes, SceneId::S1_TemporalHalton,
                 rr_ref::kGuideTranspLayer | rr_ref::kGuideTranspMvecs,
                 1280, 720, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, false, 0, 16, 0, 1, kp});
    c.push_back({"P04_guide_sss_cbt", PassGroup::Probes, SceneId::S1_TemporalHalton, rr_ref::kGuideSssAndCbt,
                 1280, 720, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, false, 0, 16, 0, 1, kp});
    c.push_back({"P05_guide_spec_mvec", PassGroup::Probes, SceneId::S1_TemporalHalton, rr_ref::kGuideSpecularMvec,
                 1280, 720, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, false, 0, 16, 0, 1, kp});
    c.push_back({"P06_guide_resp_r16f", PassGroup::Probes, SceneId::S1_TemporalHalton, rr_ref::kGuideResponsivity,
                 1280, 720, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, false, 0, 16, 0, 1, kp});
    c.push_back({"P07_guide_dof_presetF", PassGroup::Probes, SceneId::S1_TemporalHalton, rr_ref::kGuideDepthOfField,
                 1280, 720, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, false, 0, 16, 0, 1, kp});
    c.push_back({"P08_guide_dof_presetE", PassGroup::Probes, SceneId::S1_TemporalHalton, rr_ref::kGuideDepthOfField,
                 1280, 720, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 5u, false, 0, 16, 0, 1, kp});
    c.push_back({"P09_no_hit_with_spec_mv", PassGroup::Probes, SceneId::S1_TemporalHalton,
                 rr_ref::kGuideOmitHitDist | rr_ref::kGuideSpecularMvec,
                 1280, 720, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, false, 0, 16, 0, 1, kp});
    c.push_back({"P10_no_hit_plain", PassGroup::Probes, SceneId::S1_TemporalHalton, rr_ref::kGuideOmitHitDist,
                 1280, 720, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, false, 0, 16, 0, 1, kp});
    c.push_back({"P11_odd_subrect_1281x721", PassGroup::Probes, SceneId::S1_TemporalHalton, rr_ref::kGuideNone,
                 1288, 728, 1281, 721, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, false, 0, 16, 0, 1, kp});
    c.push_back({"P12_preset_default_hint0", PassGroup::Probes, SceneId::S1_TemporalHalton, rr_ref::kGuideNone,
                 1280, 720, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 0u, false, 0, 16, 0, 1, kp});

    return c;
}

// Focused Round-2 motion & capability supplement (rr_contrast_v2_motion):
// Skips all static S0 cases, timing-core, and Nsight; runs ONLY fixed true-motion QualityExt (4 cases)
// plus single-variable 16-frame capability/guide probes (<15s total on RTX 5090).
std::vector<SuiteCase> BuildV2MotionSuite()
{
    using rr_ref::SceneId;
    const std::vector<uint32_t> k10 = {0, 1, 2, 4, 8, 15, 31, 63, 67, 75};
    const std::vector<uint32_t> k8  = {0, 1, 2, 4, 8, 15, 31, 63};
    const std::vector<uint32_t> k6  = {0, 1, 4, 15, 31, 63};
    const std::vector<uint32_t> k4  = {0, 4, 15, 63};
    const std::vector<uint32_t> kp  = {15};

    std::vector<SuiteCase> c;

    // 1. Quality Extended under true pixel-scale motion + moving camera (S1 temporal Halton + S2 HDR ~16.0 stress)
    c.push_back({"Q_S1_1080p_qual", PassGroup::QualityExt, SceneId::S1_TemporalHalton, 0u,
                 1280, 720, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, true, 0, 64, 16, 1, k10});
    c.push_back({"Q_S1_4k_qual", PassGroup::QualityExt, SceneId::S1_TemporalHalton, 0u,
                 2560, 1440, 2560, 1440, 3840, 2160, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, false, 0, 64, 0, 1, k6});
    c.push_back({"Q_S2_1080p_qual", PassGroup::QualityExt, SceneId::S2_HdrStress, 0u,
                 1280, 720, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, false, 0, 64, 0, 1, k8});
    c.push_back({"Q_S2_4k_perf", PassGroup::QualityExt, SceneId::S2_HdrStress, 0u,
                 1920, 1080, 1920, 1080, 3840, 2160, NVSDK_NGX_PerfQuality_Value_MaxPerf, 6u, false, 0, 64, 0, 1, k4});

    // 2. Single-variable Motion, Guide & ABI Equivalence Probes (16 frames 0..15, save f015)
    c.push_back({"P00_baseline_s1_g0", PassGroup::Probes, SceneId::S1_TemporalHalton, rr_ref::kGuideNone,
                 1280, 720, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, false, 0, 16, 0, 1, kp});
    c.push_back({"P01_guide_disocc", PassGroup::Probes, SceneId::S1_TemporalHalton, rr_ref::kGuideDisocclusion,
                 1280, 720, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, false, 0, 16, 0, 1, kp});
    c.push_back({"P02_guide_transp_rgba", PassGroup::Probes, SceneId::S1_TemporalHalton, rr_ref::kGuideTranspLayer,
                 1280, 720, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, false, 0, 16, 0, 1, kp});
    c.push_back({"P03_guide_transp_tlmv", PassGroup::Probes, SceneId::S1_TemporalHalton,
                 rr_ref::kGuideTranspLayer | rr_ref::kGuideTranspMvecs,
                 1280, 720, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, false, 0, 16, 0, 1, kp,
                 {}, "P02_guide_transp_rgba", false});
    c.push_back({"P04a_guide_sss_only", PassGroup::Probes, SceneId::S1_TemporalHalton, rr_ref::kGuideSssOnly,
                 1280, 720, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, false, 0, 16, 0, 1, kp});
    c.push_back({"P04b_guide_cbt_only", PassGroup::Probes, SceneId::S1_TemporalHalton, rr_ref::kGuideCbtOnly,
                 1280, 720, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, false, 0, 16, 0, 1, kp});
    c.push_back({"P05_guide_spec_mvec", PassGroup::Probes, SceneId::S1_TemporalHalton, rr_ref::kGuideSpecularMvec,
                 1280, 720, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, false, 0, 16, 0, 1, kp});
    c.push_back({"P09_no_hit_with_spec_mv", PassGroup::Probes, SceneId::S1_TemporalHalton,
                 rr_ref::kGuideOmitHitDist | rr_ref::kGuideSpecularMvec,
                 1280, 720, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, false, 0, 16, 0, 1, kp,
                 {}, "P05_guide_spec_mvec", false});
    c.push_back({"P10_no_hit_plain", PassGroup::Probes, SceneId::S1_TemporalHalton, rr_ref::kGuideOmitHitDist,
                 1280, 720, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, false, 0, 16, 0, 1, kp});

    // P13: Manual Pre.Exposure / Exposure.Scale step (AutoExposure flag cleared: 0x0B)
    {
        rr_ref::V2FrameModifiers m {};
        m.create_flags = 0x0B; // IsHDR(1) | MVLowRes(2) | DepthInverted(8), AutoExposure=0
        m.manual_exposure_step = true;
        c.push_back({"P13_exposure_no_auto", PassGroup::Probes, SceneId::S1_TemporalHalton, rr_ref::kGuideNone,
                     1280, 720, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, false, 0, 16, 0, 1, kp,
                     m, {}, false});
    }
    // P14: Non-zero input Subrect.Base = (32, 24) inside 1344x768 D3D12 textures with magenta sentinel border (should match P00)
    {
        rr_ref::V2FrameModifiers m {};
        m.in_base_x = 32u;
        m.in_base_y = 24u;
        c.push_back({"P14_input_subrect_base_32_24", PassGroup::Probes, SceneId::S1_TemporalHalton, rr_ref::kGuideNone,
                     1344, 768, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, false, 0, 16, 0, 1, kp,
                     m, {}, true});
    }
    // P15: MV.Scale = (-0.5, -0.5) with -2x pre-scaled MV texture (exact IEEE-754 power-of-2 inverse -> should match P00)
    {
        rr_ref::V2FrameModifiers m {};
        m.mv_scale_x = -0.5f;
        m.mv_scale_y = -0.5f;
        m.mv_tex_scale_num = -2ll;
        c.push_back({"P15_mv_scale_neg0p5", PassGroup::Probes, SceneId::S1_TemporalHalton, rr_ref::kGuideNone,
                     1280, 720, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, false, 0, 16, 0, 1, kp,
                     m, {}, true});
    }
    // P16a/b/c: MVJittered flag (0x4F) raw vs +(prev_j - curr_j) vs +(curr_j - prev_j) to lock exact C(0x328)/C(0x32c) sign
    {
        rr_ref::V2FrameModifiers ma {};
        ma.create_flags = 0x4F; // 0x4B | NVSDK_NGX_DLSS_Feature_Flags_MVJittered(4)
        ma.mv_jitter_delta_sign = 0;
        c.push_back({"P16a_mv_jittered_raw", PassGroup::Probes, SceneId::S1_TemporalHalton, rr_ref::kGuideNone,
                     1280, 720, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, false, 0, 16, 0, 1, kp,
                     ma, {}, false});

        rr_ref::V2FrameModifiers mb = ma;
        mb.mv_jitter_delta_sign = +1;
        c.push_back({"P16b_mv_jittered_prev_minus_curr", PassGroup::Probes, SceneId::S1_TemporalHalton, rr_ref::kGuideNone,
                     1280, 720, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, false, 0, 16, 0, 1, kp,
                     mb, "P16a_mv_jittered_raw", true});

        rr_ref::V2FrameModifiers mc = ma;
        mc.mv_jitter_delta_sign = -1;
        c.push_back({"P16c_mv_jittered_curr_minus_prev", PassGroup::Probes, SceneId::S1_TemporalHalton, rr_ref::kGuideNone,
                     1280, 720, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, false, 0, 16, 0, 1, kp,
                     mc, "P16a_mv_jittered_raw", true});
    }
    // P17: Unpacked Roughness (Roughness.Mode = 0, GBuffer.Roughness R16F bound, Normals.w = 0 -> should match P00)
    c.push_back({"P17_roughness_unpacked_r16f", PassGroup::Probes, SceneId::S1_TemporalHalton, rr_ref::kGuideUnpackedRough,
                 1280, 720, 1280, 720, 1920, 1080, NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, false, 0, 16, 0, 1, kp,
                 {}, {}, true});

    return c;
}

std::vector<SuiteCase> BuildSuite(rr_ref::FixtureSuite suite)
{
    return (suite == rr_ref::FixtureSuite::V2_Motion) ? BuildV2MotionSuite() : BuildCanonicalSuite();
}

struct CaseFixtureHashes
{
    std::string f000_sha256;
    std::string f015_sha256;
    std::string drs_f064_reset0_sha256;
    std::string drs_f072_reset1_sha256;
};

CaseFixtureHashes ComputeCaseFixtureHashes(const SuiteCase& sc, rr_ref::FixtureSuite suite = rr_ref::FixtureSuite::V1)
{
    rr_ref::CpuFrameBuffers bufs;
    bufs.Allocate(sc.alloc_rw, sc.alloc_rh, sc.out_w, sc.out_h, sc.guide_mask);
    if (sc.scene == rr_ref::SceneId::S0_BenchParity)
        rr_ref::PopulateStaticS0(bufs);

    rr_ref::FrameScalarParams p {};
    CaseFixtureHashes out {};

    rr_ref::PrepareFrame(sc.scene, sc.guide_mask, 0u, sc.steady_rw, sc.steady_rh, sc.out_w, sc.out_h,
                         1, sc.preset_hint, sc.perf_quality, p, bufs, suite, sc.v2_mod);
    out.f000_sha256 = rr_ref::ComputeCanonicalFrameInputSha256(p, sc.guide_mask, bufs, suite);

    rr_ref::PrepareFrame(sc.scene, sc.guide_mask, 15u, sc.steady_rw, sc.steady_rh, sc.out_w, sc.out_h,
                         0, sc.preset_hint, sc.perf_quality, p, bufs, suite, sc.v2_mod);
    out.f015_sha256 = rr_ref::ComputeCanonicalFrameInputSha256(p, sc.guide_mask, bufs, suite);

    if (sc.has_drs)
    {
        const uint32_t drs_w = (sc.steady_rw * 85u / 100u) & ~1u;
        const uint32_t drs_h = (sc.steady_rh * 85u / 100u) & ~1u;
        rr_ref::PrepareFrame(sc.scene, sc.guide_mask, 64u, drs_w, drs_h, sc.out_w, sc.out_h,
                             0, sc.preset_hint, sc.perf_quality, p, bufs, suite, sc.v2_mod);
        out.drs_f064_reset0_sha256 = rr_ref::ComputeCanonicalFrameInputSha256(p, sc.guide_mask, bufs, suite);

        rr_ref::PrepareFrame(sc.scene, sc.guide_mask, 72u, drs_w, drs_h, sc.out_w, sc.out_h,
                             1, sc.preset_hint, sc.perf_quality, p, bufs, suite, sc.v2_mod);
        out.drs_f072_reset1_sha256 = rr_ref::ComputeCanonicalFrameInputSha256(p, sc.guide_mask, bufs, suite);
    }
    return out;
}

void DumpFixtureHashesJson(const fs::path& json_path, rr_ref::FixtureSuite suite)
{
    if (json_path.has_parent_path())
        fs::create_directories(json_path.parent_path());

    const auto cases = BuildSuite(suite);
    const char* fv = rr_ref::FixtureVersionForSuite(suite);
    std::ofstream out(json_path, std::ios::binary);
    Require(static_cast<bool>(out), "Failed to open output JSON: " + WideToUtf8(json_path.wstring()));

    out << "{\n"
        << "  \"fixture_version\": \"" << fv << "\",\n"
        << "  \"expected_dlssd_version\": \"" << kExpectedDlssdVersion << "\",\n"
        << "  \"expected_dlssd_bytes\": " << kExpectedDlssdBytes << ",\n"
        << "  \"expected_dlssd_sha256\": \"" << kExpectedDlssdSha256 << "\",\n"
        << "  \"openrr_weight_slice_note\": \"" << kOpenRrWeightSliceNote << "\",\n"
        << "  \"cases\": [\n";

    for (size_t i = 0; i < cases.size(); ++i)
    {
        const auto& sc = cases[i];
        const auto fh = ComputeCaseFixtureHashes(sc, suite);
        const uint32_t drs_w = sc.has_drs ? ((sc.steady_rw * 85u / 100u) & ~1u) : 0u;
        const uint32_t drs_h = sc.has_drs ? ((sc.steady_rh * 85u / 100u) & ~1u) : 0u;

        out << "    {\n"
            << "      \"id\": \"" << sc.id << "\",\n"
            << "      \"pass\": \"" << PassGroupName(sc.group) << "\",\n"
            << "      \"scene\": \"" << rr_ref::SceneName(sc.scene) << "\",\n"
            << "      \"guide_mask\": " << sc.guide_mask << ",\n"
            << "      \"alloc_render\": [" << sc.alloc_rw << ", " << sc.alloc_rh << "],\n"
            << "      \"steady_render\": [" << sc.steady_rw << ", " << sc.steady_rh << "],\n"
            << "      \"drs_render\": [" << drs_w << ", " << drs_h << "],\n"
            << "      \"output\": [" << sc.out_w << ", " << sc.out_h << "],\n"
            << "      \"perf_quality\": " << sc.perf_quality << ",\n"
            << "      \"preset_hint\": " << sc.preset_hint << ",\n";
        if (suite == rr_ref::FixtureSuite::V2_Motion)
        {
            out << "      \"create_flags\": " << sc.v2_mod.create_flags << ",\n"
                << "      \"in_subrect_base\": [" << sc.v2_mod.in_base_x << ", " << sc.v2_mod.in_base_y << "],\n"
                << "      \"mv_scale\": [" << sc.v2_mod.mv_scale_x << ", " << sc.v2_mod.mv_scale_y << "],\n"
                << "      \"mv_tex_scale_num\": " << sc.v2_mod.mv_tex_scale_num << ",\n"
                << "      \"mv_jitter_delta_sign\": " << sc.v2_mod.mv_jitter_delta_sign << ",\n"
                << "      \"manual_exposure_step\": " << (sc.v2_mod.manual_exposure_step ? "true" : "false") << ",\n";
        }
        out << "      \"warmup_frames\": " << sc.warmup_frames << ",\n"
            << "      \"steady_frames\": " << sc.steady_frames << ",\n"
            << "      \"drs_frames\": " << sc.drs_frames << ",\n"
            << "      \"rounds\": " << sc.rounds << ",\n"
            << "      \"save_frames\": [";
        for (size_t k = 0; k < sc.save_frames.size(); ++k)
            out << (k ? ", " : "") << sc.save_frames[k];
        out << "],\n"
            << "      \"f000_input_sha256\": \"" << fh.f000_sha256 << "\",\n"
            << "      \"f015_input_sha256\": \"" << fh.f015_sha256 << "\"";
        if (sc.has_drs)
        {
            out << ",\n"
                << "      \"drs_f064_reset0_input_sha256\": \"" << fh.drs_f064_reset0_sha256 << "\",\n"
                << "      \"drs_f072_reset1_input_sha256\": \"" << fh.drs_f072_reset1_sha256 << "\"\n";
        }
        else
        {
            out << "\n";
        }
        out << "    }" << (i + 1 < cases.size() ? "," : "") << "\n";
    }
    out << "  ]\n}\n";
}

// Lightweight line-oriented verifier for cases/cases.json or cases/cases_v2_motion.json (no external JSON library required).
void VerifyFixturesAgainstJson(const fs::path& json_path, rr_ref::FixtureSuite cli_suite, bool suite_explicit)
{
    std::ifstream in(json_path);
    Require(static_cast<bool>(in), "Cannot open cases JSON: " + WideToUtf8(json_path.wstring()));
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    auto extractStringField = [&](size_t start_pos, size_t end_pos, const std::string& key) -> std::string {
        const std::string needle = "\"" + key + "\": \"";
        const size_t p = content.find(needle, start_pos);
        if (p == std::string::npos || p >= end_pos) return {};
        const size_t v0 = p + needle.size();
        const size_t v1 = content.find('"', v0);
        if (v1 == std::string::npos || v1 > end_pos) return {};
        return content.substr(v0, v1 - v0);
    };

    const std::string fv = extractStringField(0, content.size(), "fixture_version");
    rr_ref::FixtureSuite suite = cli_suite;
    if (!suite_explicit)
    {
        if (fv == rr_ref::kFixtureVersionV2Motion)
            suite = rr_ref::FixtureSuite::V2_Motion;
        else
            suite = rr_ref::FixtureSuite::V1;
    }
    const char* expected_fv = rr_ref::FixtureVersionForSuite(suite);
    Require(fv == expected_fv,
            "fixture_version mismatch in JSON: expected " + std::string(expected_fv) + ", got " + fv);

    const auto cases = BuildSuite(suite);
    size_t verified_hashes = 0;
    for (const auto& sc : cases)
    {
        const std::string id_needle = "\"id\": \"" + sc.id + "\"";
        const size_t block_start = content.find(id_needle);
        Require(block_start != std::string::npos, "Missing case in cases JSON: " + sc.id);
        const size_t block_end = content.find("}", block_start);
        Require(block_end != std::string::npos, "Malformed case block in cases JSON: " + sc.id);

        const auto actual = ComputeCaseFixtureHashes(sc, suite);
        const std::string exp_f000 = extractStringField(block_start, block_end, "f000_input_sha256");
        const std::string exp_f015 = extractStringField(block_start, block_end, "f015_input_sha256");
        Require(actual.f000_sha256 == exp_f000,
                "Input SHA-256 mismatch for " + sc.id + " f000: expected " + exp_f000 + ", computed " + actual.f000_sha256);
        Require(actual.f015_sha256 == exp_f015,
                "Input SHA-256 mismatch for " + sc.id + " f015: expected " + exp_f015 + ", computed " + actual.f015_sha256);
        verified_hashes += 2;

        if (sc.has_drs)
        {
            const std::string exp_f064 = extractStringField(block_start, block_end, "drs_f064_reset0_input_sha256");
            const std::string exp_f072 = extractStringField(block_start, block_end, "drs_f072_reset1_input_sha256");
            Require(actual.drs_f064_reset0_sha256 == exp_f064,
                    "Input SHA-256 mismatch for " + sc.id + " drs_f064: expected " + exp_f064 + ", computed " + actual.drs_f064_reset0_sha256);
            Require(actual.drs_f072_reset1_sha256 == exp_f072,
                    "Input SHA-256 mismatch for " + sc.id + " drs_f072: expected " + exp_f072 + ", computed " + actual.drs_f072_reset1_sha256);
            verified_hashes += 2;
        }
    }
    std::printf("[PASS] Verified %zu canonical input SHA-256 hashes across %zu suite cases (fixture_version=%s)\n",
                verified_hashes, cases.size(), expected_fv);
}

// =====================================================================================================================
// NGX D3D12 Dynamic Loader / SDK Dispatch
// =====================================================================================================================

struct NgxApi
{
#ifndef RR_USE_OFFICIAL_NGX_SDK
    using PFN_Init_ProjectID = NVSDK_NGX_Result(RR_NGX_MSABI*)(
        const char*, NVSDK_NGX_EngineType, const char*, const wchar_t*, ID3D12Device*,
        NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo*);
    using PFN_Init_Ext = NVSDK_NGX_Result(RR_NGX_MSABI*)(
        unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo*);
    using PFN_Init_Pub = NVSDK_NGX_Result(RR_NGX_MSABI*)(
        unsigned long long, const wchar_t*, ID3D12Device*, const NVSDK_NGX_FeatureCommonInfo*, NVSDK_NGX_Version);
    using PFN_Shutdown1 = NVSDK_NGX_Result(RR_NGX_MSABI*)(ID3D12Device*, unsigned int*);
    using PFN_Shutdown0 = NVSDK_NGX_Result(RR_NGX_MSABI*)();
    using PFN_GetCapParams = NVSDK_NGX_Result(RR_NGX_MSABI*)(NVSDK_NGX_Parameter**);
    using PFN_AllocParams = NVSDK_NGX_Result(RR_NGX_MSABI*)(NVSDK_NGX_Parameter**);
    using PFN_DestroyParams = NVSDK_NGX_Result(RR_NGX_MSABI*)(NVSDK_NGX_Parameter*);
    using PFN_GetScratchSize = NVSDK_NGX_Result(RR_NGX_MSABI*)(NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*, size_t*);
    using PFN_CreateFeature = NVSDK_NGX_Result(RR_NGX_MSABI*)(
        ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
    using PFN_EvaluateFeature = NVSDK_NGX_Result(RR_NGX_MSABI*)(
        ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, void*);
    using PFN_ReleaseFeature = NVSDK_NGX_Result(RR_NGX_MSABI*)(NVSDK_NGX_Handle*);

    HMODULE mod = nullptr;
    fs::path loader_path;
    PFN_Init_ProjectID init_project_id = nullptr;
    bool initialized = false;
    PFN_Init_Ext init_ext = nullptr;
    PFN_Init_Pub init_pub = nullptr;
    PFN_Shutdown1 shutdown1 = nullptr;
    PFN_Shutdown0 shutdown0 = nullptr;
    PFN_GetCapParams get_cap_params = nullptr;
    PFN_AllocParams alloc_params = nullptr;
    PFN_DestroyParams destroy_params = nullptr;
    PFN_GetScratchSize get_scratch_size = nullptr;
    PFN_CreateFeature create_feature = nullptr;
    PFN_EvaluateFeature evaluate_feature = nullptr;
    PFN_ReleaseFeature release_feature = nullptr;

    void Load(const fs::path& explicit_loader)
    {
        loader_path = LocateNgxCoreLoader(explicit_loader);
        mod = LoadLibraryExW(loader_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        Require(mod != nullptr, "LoadLibraryExW failed on NGX loader: " + WideToUtf8(loader_path.wstring()));

        auto proc = [&](const char* sym) -> void* {
            return reinterpret_cast<void*>(GetProcAddress(mod, sym));
        };
        init_project_id = reinterpret_cast<PFN_Init_ProjectID>(proc("NVSDK_NGX_D3D12_Init_ProjectID"));
        init_ext = reinterpret_cast<PFN_Init_Ext>(proc("NVSDK_NGX_D3D12_Init_Ext"));
        init_pub = reinterpret_cast<PFN_Init_Pub>(proc("NVSDK_NGX_D3D12_Init"));
        shutdown1 = reinterpret_cast<PFN_Shutdown1>(proc("NVSDK_NGX_D3D12_Shutdown1"));
        shutdown0 = reinterpret_cast<PFN_Shutdown0>(proc("NVSDK_NGX_D3D12_Shutdown"));
        get_cap_params = reinterpret_cast<PFN_GetCapParams>(proc("NVSDK_NGX_D3D12_GetCapabilityParameters"));
        alloc_params = reinterpret_cast<PFN_AllocParams>(proc("NVSDK_NGX_D3D12_AllocateParameters"));
        destroy_params = reinterpret_cast<PFN_DestroyParams>(proc("NVSDK_NGX_D3D12_DestroyParameters"));
        get_scratch_size = reinterpret_cast<PFN_GetScratchSize>(proc("NVSDK_NGX_D3D12_GetScratchBufferSize"));
        create_feature = reinterpret_cast<PFN_CreateFeature>(proc("NVSDK_NGX_D3D12_CreateFeature"));
        evaluate_feature = reinterpret_cast<PFN_EvaluateFeature>(proc("NVSDK_NGX_D3D12_EvaluateFeature"));
        release_feature = reinterpret_cast<PFN_ReleaseFeature>(proc("NVSDK_NGX_D3D12_ReleaseFeature"));

        Require((init_project_id || init_ext || init_pub) && (shutdown1 || shutdown0) &&
                (get_cap_params || alloc_params) && destroy_params &&
                create_feature && evaluate_feature && release_feature,
                "NGX loader missing required D3D12 exports: " + WideToUtf8(loader_path.wstring()));
    }

    NVSDK_NGX_Result Init(unsigned long long app_id, const wchar_t* app_data, ID3D12Device* dev, const NVSDK_NGX_FeatureCommonInfo* info)
    {
        NVSDK_NGX_Result r = NVSDK_NGX_Result_Fail;
        if (init_project_id)
        {
            // Driver-core ProjectID ABI: version precedes FeatureCommonInfo (proven on RTX 5090 _nvngx.dll).
            r = init_project_id("46fc1417-66bd-5517-943a-d1993498b797", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                "RR_Contrast_reference_v1", app_data, dev, NVSDK_NGX_Version_API, info);
        }
        else if (init_ext)
        {
            r = init_ext(app_id, app_data, dev, NVSDK_NGX_Version_API, info);
        }
        else if (init_pub)
        {
            r = init_pub(app_id, app_data, dev, info, NVSDK_NGX_Version_API);
        }
        initialized = NVSDK_NGX_SUCCEED(r);
        return r;
    }

    NVSDK_NGX_Result GetParams(NVSDK_NGX_Parameter** out)
    {
        return get_cap_params ? get_cap_params(out) : alloc_params(out);
    }

    size_t QueryScratchBytes(const NVSDK_NGX_Parameter* p)
    {
        size_t bytes = 0;
        if (get_scratch_size && NVSDK_NGX_SUCCEED(get_scratch_size(NVSDK_NGX_Feature_RayReconstruction, p, &bytes)))
            return bytes;
        return 0;
    }

    void Shutdown(ID3D12Device* dev)
    {
        if (initialized)
        {
            if (shutdown1)
            {
                unsigned int remaining_instances = 0;
                RuntimeTrace("[CLEANUP] Shutdown1 driver call begin");
                shutdown1(dev, &remaining_instances);
                RuntimeTrace("[CLEANUP] Shutdown1 driver call returned");
            }
            else if (shutdown0)
            {
                shutdown0();
            }
            initialized = false;
        }
    }
#else
    fs::path loader_path = L"static_nvsdk_ngx_sdk";
    void Load(const fs::path&) {}
    NVSDK_NGX_Result Init(unsigned long long app_id, const wchar_t* app_data, ID3D12Device* dev, const NVSDK_NGX_FeatureCommonInfo* info)
    {
        return NVSDK_NGX_D3D12_Init(app_id, app_data, dev, info, NVSDK_NGX_Version_API);
    }
    NVSDK_NGX_Result GetParams(NVSDK_NGX_Parameter** out) { return NVSDK_NGX_D3D12_GetCapabilityParameters(out); }
    NVSDK_NGX_Result DestroyParams(NVSDK_NGX_Parameter* p) { return NVSDK_NGX_D3D12_DestroyParameters(p); }
    size_t QueryScratchBytes(const NVSDK_NGX_Parameter* p)
    {
        size_t bytes = 0;
        NVSDK_NGX_D3D12_GetScratchBufferSize(NVSDK_NGX_Feature_RayReconstruction, p, &bytes);
        return bytes;
    }
    void Shutdown(ID3D12Device* dev) { NVSDK_NGX_D3D12_Shutdown1(dev); }
#endif
};

// =====================================================================================================================
// D3D12 Device, Textures, Upload, Readback & Timestamp Helpers
// =====================================================================================================================

struct D3dContext
{
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter1;
    ComPtr<IDXGIAdapter3> adapter3;
    DXGI_ADAPTER_DESC1 adapter_desc {};
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> ca;
    ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    UINT64 fence_val = 0;
    UINT64 ts_freq = 1;

    ComPtr<ID3D12QueryHeap> ts_heap;
    ComPtr<ID3D12Resource> ts_readback;

    ~D3dContext()
    {
        RuntimeTrace("[CLEANUP] D3dContext destructor body");
        if (fence_event) CloseHandle(fence_event);
    }

    void Init(uint32_t preferred_vendor_id = 0x10DEu)
    {
        CheckHr(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
        for (UINT i = 0; factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter1)) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 d {};
            adapter1->GetDesc1(&d);
            if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
                continue;
            if (preferred_vendor_id == 0 || d.VendorId == preferred_vendor_id)
            {
                adapter_desc = d;
                CheckHr(adapter1.As(&adapter3), "Query IDXGIAdapter3");
                break;
            }
        }
        Require(adapter3 != nullptr, "No matching hardware GPU adapter found (expected VendorId=" + HexU32(preferred_vendor_id) + ")");

        CheckHr(D3D12CreateDevice(adapter3.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&dev)), "D3D12CreateDevice");
        D3D12_COMMAND_QUEUE_DESC qd {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        CheckHr(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
        CheckHr(queue->GetTimestampFrequency(&ts_freq), "GetTimestampFrequency");

        CheckHr(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca)), "CreateCommandAllocator");
        CheckHr(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca.Get(), nullptr, IID_PPV_ARGS(&cl)), "CreateCommandList");
        CheckHr(cl->Close(), "Close initial command list");

        CheckHr(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
        fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Require(fence_event != nullptr, "CreateEventW failed");

        D3D12_QUERY_HEAP_DESC qhd {};
        qhd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qhd.Count = 2;
        CheckHr(dev->CreateQueryHeap(&qhd, IID_PPV_ARGS(&ts_heap)), "CreateQueryHeap");

        D3D12_HEAP_PROPERTIES rbhp {};
        rbhp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rbd {};
        rbd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rbd.Width = 16;
        rbd.Height = 1;
        rbd.DepthOrArraySize = 1;
        rbd.MipLevels = 1;
        rbd.SampleDesc.Count = 1;
        rbd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        CheckHr(dev->CreateCommittedResource(&rbhp, D3D12_HEAP_FLAG_NONE, &rbd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&ts_readback)),
                "Create timestamp readback buffer");
    }

    void ExecuteAndSync(const char* where)
    {
        CheckHr(cl->Close(), where);
        ID3D12CommandList* lists[] = {cl.Get()};
        queue->ExecuteCommandLists(1, lists);
        CheckHr(queue->Signal(fence.Get(), ++fence_val), where);
        if (fence->GetCompletedValue() < fence_val)
        {
            CheckHr(fence->SetEventOnCompletion(fence_val, fence_event), where);
            const DWORD w = WaitForSingleObject(fence_event, 60000);
            Require(w == WAIT_OBJECT_0, std::string(where) + ": GPU fence timed out");
        }
        CheckHr(dev->GetDeviceRemovedReason(), where);
    }

    uint64_t QueryProcessLocalVramBytes() const
    {
        DXGI_QUERY_VIDEO_MEMORY_INFO info {};
        if (adapter3 && SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
            return info.CurrentUsage;
        return 0;
    }

    double ReadLastGpuMs() const
    {
        uint64_t ts[2] {};
        void* p = nullptr;
        D3D12_RANGE r {0, 16};
        CheckHr(ts_readback->Map(0, &r, &p), "Map timestamp buffer");
        std::memcpy(ts, p, 16);
        D3D12_RANGE empty {0, 0};
        ts_readback->Unmap(0, &empty);
        return static_cast<double>(ts[1] - ts[0]) * 1000.0 / static_cast<double>(ts_freq);
    }
};

struct GpuTex2D
{
    ComPtr<ID3D12Resource> res;
    ComPtr<ID3D12Resource> upload_buf;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    uint32_t w = 0, h = 0, bpp = 0;

    void Create(ID3D12Device* dev, uint32_t width, uint32_t height, DXGI_FORMAT fmt, uint32_t bytes_per_px, bool allow_uav, D3D12_RESOURCE_STATES init_state)
    {
        w = width;
        h = height;
        bpp = bytes_per_px;
        state = init_state;

        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = width;
        rd.Height = height;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = fmt;
        rd.SampleDesc.Count = 1;
        rd.Flags = allow_uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        CheckHr(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, init_state, nullptr, IID_PPV_ARGS(&res)),
                "CreateCommittedResource(Tex2D)");

        if (!allow_uav)
        {
            UINT64 total_bytes = 0;
            dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total_bytes);
            D3D12_HEAP_PROPERTIES uhp {};
            uhp.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bd {};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = total_bytes;
            bd.Height = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            CheckHr(dev->CreateCommittedResource(&uhp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload_buf)),
                    "CreateCommittedResource(UploadBuf)");
        }
    }

    void StageAndRecordUpload(ID3D12GraphicsCommandList* cl, const void* cpu_data, uint32_t src_pitch_px, uint32_t active_w, uint32_t active_h)
    {
        if (!res || !upload_buf) return;
        uint8_t* mapped = nullptr;
        D3D12_RANGE no_read {0, 0};
        CheckHr(upload_buf->Map(0, &no_read, reinterpret_cast<void**>(&mapped)), "Map upload_buf");
        const auto* src = static_cast<const uint8_t*>(cpu_data);
        const size_t copy_row_bytes = size_t(active_w) * bpp;
        const size_t src_row_bytes = size_t(src_pitch_px) * bpp;
        for (uint32_t y = 0; y < active_h; ++y)
            std::memcpy(mapped + fp.Offset + size_t(y) * fp.Footprint.RowPitch, src + size_t(y) * src_row_bytes, copy_row_bytes);
        upload_buf->Unmap(0, nullptr);

        if (state != D3D12_RESOURCE_STATE_COPY_DEST)
        {
            D3D12_RESOURCE_BARRIER b {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition = {res.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, state, D3D12_RESOURCE_STATE_COPY_DEST};
            cl->ResourceBarrier(1, &b);
        }
        D3D12_TEXTURE_COPY_LOCATION dst {res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src_loc {upload_buf.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        src_loc.PlacedFootprint = fp;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src_loc, nullptr);

        D3D12_RESOURCE_BARRIER b2 {};
        b2.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b2.Transition = {res.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, kSrvReadState};
        cl->ResourceBarrier(1, &b2);
        state = kSrvReadState;
    }
};

struct GpuSceneResources
{
    GpuTex2D color, diffuse, specular, normals, rough, depth, mvec, hit_dist;
    GpuTex2D disocc, transp, transp_op, transp_mvec, sss, cbt, spec_mvec, resp, dof;
    GpuTex2D output;
    ComPtr<ID3D12Resource> output_readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT out_fp {};

    void Allocate(ID3D12Device* dev, uint32_t max_rw, uint32_t max_rh, uint32_t out_w, uint32_t out_h, uint32_t guide_mask, bool need_readback)
    {
        color.Create(dev, max_rw, max_rh, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, false, D3D12_RESOURCE_STATE_COPY_DEST);
        diffuse.Create(dev, max_rw, max_rh, DXGI_FORMAT_R8G8B8A8_UNORM, 4, false, D3D12_RESOURCE_STATE_COPY_DEST);
        specular.Create(dev, max_rw, max_rh, DXGI_FORMAT_R8G8B8A8_UNORM, 4, false, D3D12_RESOURCE_STATE_COPY_DEST);
        normals.Create(dev, max_rw, max_rh, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, false, D3D12_RESOURCE_STATE_COPY_DEST);
        if (guide_mask & rr_ref::kGuideUnpackedRough)
            rough.Create(dev, max_rw, max_rh, DXGI_FORMAT_R16_FLOAT, 2, false, D3D12_RESOURCE_STATE_COPY_DEST);
        depth.Create(dev, max_rw, max_rh, DXGI_FORMAT_R32_FLOAT, 4, false, D3D12_RESOURCE_STATE_COPY_DEST);
        mvec.Create(dev, max_rw, max_rh, DXGI_FORMAT_R16G16_FLOAT, 4, false, D3D12_RESOURCE_STATE_COPY_DEST);
        if ((guide_mask & rr_ref::kGuideOmitHitDist) == 0u)
            hit_dist.Create(dev, max_rw, max_rh, DXGI_FORMAT_R32_FLOAT, 4, false, D3D12_RESOURCE_STATE_COPY_DEST);

        if (guide_mask & rr_ref::kGuideDisocclusion)
            disocc.Create(dev, max_rw, max_rh, DXGI_FORMAT_R16_FLOAT, 2, false, D3D12_RESOURCE_STATE_COPY_DEST);
        if (guide_mask & rr_ref::kGuideTranspLayer)
        {
            transp.Create(dev, max_rw, max_rh, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, false, D3D12_RESOURCE_STATE_COPY_DEST);
            transp_op.Create(dev, max_rw, max_rh, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, false, D3D12_RESOURCE_STATE_COPY_DEST);
        }
        if (guide_mask & rr_ref::kGuideTranspMvecs)
            transp_mvec.Create(dev, out_w, out_h, DXGI_FORMAT_R16G16_FLOAT, 4, false, D3D12_RESOURCE_STATE_COPY_DEST);
        if (guide_mask & (rr_ref::kGuideSssAndCbt | rr_ref::kGuideSssOnly))
            sss.Create(dev, max_rw, max_rh, DXGI_FORMAT_R16_FLOAT, 2, false, D3D12_RESOURCE_STATE_COPY_DEST);
        if (guide_mask & (rr_ref::kGuideSssAndCbt | rr_ref::kGuideCbtOnly))
            cbt.Create(dev, max_rw, max_rh, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, false, D3D12_RESOURCE_STATE_COPY_DEST);
        if (guide_mask & rr_ref::kGuideSpecularMvec)
            spec_mvec.Create(dev, max_rw, max_rh, DXGI_FORMAT_R16G16_FLOAT, 4, false, D3D12_RESOURCE_STATE_COPY_DEST);
        if (guide_mask & rr_ref::kGuideResponsivity)
            resp.Create(dev, max_rw, max_rh, DXGI_FORMAT_R16_FLOAT, 2, false, D3D12_RESOURCE_STATE_COPY_DEST);
        if (guide_mask & rr_ref::kGuideDepthOfField)
            dof.Create(dev, max_rw, max_rh, DXGI_FORMAT_R16_FLOAT, 2, false, D3D12_RESOURCE_STATE_COPY_DEST);

        output.Create(dev, out_w, out_h, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        if (need_readback)
        {
            D3D12_RESOURCE_DESC od = output.res->GetDesc();
            UINT64 total = 0;
            dev->GetCopyableFootprints(&od, 0, 1, 0, &out_fp, nullptr, nullptr, &total);
            D3D12_HEAP_PROPERTIES rbhp {};
            rbhp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bd {};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = total;
            bd.Height = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            CheckHr(dev->CreateCommittedResource(&rbhp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&output_readback)),
                    "Create output readback buffer");
        }
    }

    void UploadFrame(D3dContext& dx, const rr_ref::CpuFrameBuffers& b, uint32_t active_rw, uint32_t active_rh, uint32_t guide_mask,
                     uint32_t in_base_x = 0u, uint32_t in_base_y = 0u)
    {
        CheckHr(dx.ca->Reset(), "UploadFrame ca->Reset");
        CheckHr(dx.cl->Reset(dx.ca.Get(), nullptr), "UploadFrame cl->Reset");

        const uint32_t up_w = (in_base_x != 0u || in_base_y != 0u) ? b.alloc_rw : active_rw;
        const uint32_t up_h = (in_base_x != 0u || in_base_y != 0u) ? b.alloc_rh : active_rh;

        color.StageAndRecordUpload(dx.cl.Get(), b.color_rgba16f.data(), b.alloc_rw, up_w, up_h);
        diffuse.StageAndRecordUpload(dx.cl.Get(), b.diffuse_rgba8.data(), b.alloc_rw, up_w, up_h);
        specular.StageAndRecordUpload(dx.cl.Get(), b.specular_rgba8.data(), b.alloc_rw, up_w, up_h);
        normals.StageAndRecordUpload(dx.cl.Get(), b.normals_rgba16f.data(), b.alloc_rw, up_w, up_h);
        if (guide_mask & rr_ref::kGuideUnpackedRough)
            rough.StageAndRecordUpload(dx.cl.Get(), b.rough_r16f.data(), b.alloc_rw, up_w, up_h);
        depth.StageAndRecordUpload(dx.cl.Get(), b.depth_r32f.data(), b.alloc_rw, up_w, up_h);
        mvec.StageAndRecordUpload(dx.cl.Get(), b.mvec_rg16f.data(), b.alloc_rw, up_w, up_h);
        if ((guide_mask & rr_ref::kGuideOmitHitDist) == 0u)
            hit_dist.StageAndRecordUpload(dx.cl.Get(), b.hit_dist_r32f.data(), b.alloc_rw, up_w, up_h);

        if (guide_mask & rr_ref::kGuideDisocclusion)
            disocc.StageAndRecordUpload(dx.cl.Get(), b.disocc_r16f.data(), b.alloc_rw, up_w, up_h);
        if (guide_mask & rr_ref::kGuideTranspLayer)
        {
            transp.StageAndRecordUpload(dx.cl.Get(), b.transp_rgba16f.data(), b.alloc_rw, up_w, up_h);
            transp_op.StageAndRecordUpload(dx.cl.Get(), b.transp_op_rgba16f.data(), b.alloc_rw, up_w, up_h);
        }
        if (guide_mask & rr_ref::kGuideTranspMvecs)
            transp_mvec.StageAndRecordUpload(dx.cl.Get(), b.transp_mvec_rg16f.data(), b.out_w, b.out_w, b.out_h);
        if (guide_mask & (rr_ref::kGuideSssAndCbt | rr_ref::kGuideSssOnly))
            sss.StageAndRecordUpload(dx.cl.Get(), b.sss_r16f.data(), b.alloc_rw, up_w, up_h);
        if (guide_mask & (rr_ref::kGuideSssAndCbt | rr_ref::kGuideCbtOnly))
            cbt.StageAndRecordUpload(dx.cl.Get(), b.cbt_rgba16f.data(), b.alloc_rw, up_w, up_h);
        if (guide_mask & rr_ref::kGuideSpecularMvec)
            spec_mvec.StageAndRecordUpload(dx.cl.Get(), b.spec_mvec_rg16f.data(), b.alloc_rw, up_w, up_h);
        if (guide_mask & rr_ref::kGuideResponsivity)
            resp.StageAndRecordUpload(dx.cl.Get(), b.resp_r16f.data(), b.alloc_rw, up_w, up_h);
        if (guide_mask & rr_ref::kGuideDepthOfField)
            dof.StageAndRecordUpload(dx.cl.Get(), b.dof_r16f.data(), b.alloc_rw, up_w, up_h);

        dx.ExecuteAndSync("UploadFrame");
    }

    void ReadbackOutputPixels(D3dContext& dx, std::vector<uint16_t>& out_px)
    {
        CheckHr(dx.ca->Reset(), "Readback ca->Reset");
        CheckHr(dx.cl->Reset(dx.ca.Get(), nullptr), "Readback cl->Reset");

        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {output.res.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
        dx.cl->ResourceBarrier(1, &b);

        D3D12_TEXTURE_COPY_LOCATION dst {output_readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        dst.PlacedFootprint = out_fp;
        D3D12_TEXTURE_COPY_LOCATION src {output.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        src.SubresourceIndex = 0;
        dx.cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        dx.cl->ResourceBarrier(1, &b);
        dx.ExecuteAndSync("ReadbackOutputPixels");

        const size_t total_bytes = size_t(out_fp.Offset) + size_t(out_fp.Footprint.RowPitch) * output.h;
        D3D12_RANGE rr {0, total_bytes};
        uint8_t* mapped = nullptr;
        CheckHr(output_readback->Map(0, &rr, reinterpret_cast<void**>(&mapped)), "Map output_readback");
        out_px.resize(size_t(output.w) * output.h * 4);
        for (uint32_t y = 0; y < output.h; ++y)
            std::memcpy(out_px.data() + size_t(y) * output.w * 4,
                        mapped + out_fp.Offset + size_t(y) * out_fp.Footprint.RowPitch,
                        size_t(output.w) * 8);
        D3D12_RANGE empty {0, 0};
        output_readback->Unmap(0, &empty);
    }
};

// =====================================================================================================================
// NGX Parameter Population (Aligned 1:1 with NVIDIA/DLSS v310.9.1 nvsdk_ngx_helpers_dlssd_d3d.h)
// =====================================================================================================================

NVSDK_NGX_Result CreateRrFeature(
    NgxApi& ngx,
    D3dContext& dx,
    NVSDK_NGX_Parameter* params,
    uint32_t create_rw,
    uint32_t create_rh,
    uint32_t out_w,
    uint32_t out_h,
    int32_t perf_quality,
    uint32_t preset_hint,
    NVSDK_NGX_Handle** out_handle,
    int32_t create_flags = static_cast<int32_t>(NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
                                                NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                                NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
                                                NVSDK_NGX_DLSS_Feature_Flags_AutoExposure),
    uint32_t roughness_mode = static_cast<uint32_t>(NVSDK_NGX_DLSS_Roughness_Mode_Packed))
{
    params->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    params->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    params->Set(NVSDK_NGX_Parameter_Width, static_cast<unsigned int>(create_rw));
    params->Set(NVSDK_NGX_Parameter_Height, static_cast<unsigned int>(create_rh));
    params->Set(NVSDK_NGX_Parameter_OutWidth, static_cast<unsigned int>(out_w));
    params->Set(NVSDK_NGX_Parameter_OutHeight, static_cast<unsigned int>(out_h));
    params->Set(NVSDK_NGX_Parameter_PerfQualityValue, static_cast<int>(perf_quality));
    params->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, static_cast<int>(create_flags));
    params->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
    params->Set(NVSDK_NGX_Parameter_DLSS_Denoise_Mode, static_cast<int>(NVSDK_NGX_DLSS_Denoise_Mode_DLUnified));
    params->Set(NVSDK_NGX_Parameter_DLSS_Roughness_Mode, static_cast<unsigned int>(roughness_mode));
    params->Set(NVSDK_NGX_Parameter_Use_HW_Depth, static_cast<unsigned int>(NVSDK_NGX_DLSS_Depth_Type_HW));

    const unsigned int preset_ui = static_cast<unsigned int>(preset_hint);
    params->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA, preset_ui);
    params->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality, preset_ui);
    params->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality, preset_ui);
    params->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced, preset_ui);
    params->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance, preset_ui);
    params->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance, preset_ui);

    CheckHr(dx.ca->Reset(), "CreateRrFeature ca->Reset");
    CheckHr(dx.cl->Reset(dx.ca.Get(), nullptr), "CreateRrFeature cl->Reset");
#ifdef RR_USE_OFFICIAL_NGX_SDK
    (void)ngx;
    const NVSDK_NGX_Result r = NVSDK_NGX_D3D12_CreateFeature(dx.cl.Get(), NVSDK_NGX_Feature_RayReconstruction, params, out_handle);
#else
    const NVSDK_NGX_Result r = ngx.create_feature(dx.cl.Get(), NVSDK_NGX_Feature_RayReconstruction, params, out_handle);
#endif
    if (NVSDK_NGX_SUCCEED(r))
        dx.ExecuteAndSync("CreateRrFeature");
    else
        CheckHr(dx.cl->Close(), "Close after failed CreateRrFeature");
    return r;
}

void BindEvalParams(
    NVSDK_NGX_Parameter* params,
    rr_ref::FrameScalarParams& fp, // non-const because NGX Set(void*) takes float* for matrices
    uint32_t guide_mask,
    const GpuSceneResources& gpu)
{
    auto setTex = [&](const char* key, ID3D12Resource* tex) {
        params->Set(key, tex);
    };

    const unsigned int bx = static_cast<unsigned int>(fp.in_base_x);
    const unsigned int by = static_cast<unsigned int>(fp.in_base_y);

    setTex(NVSDK_NGX_Parameter_Color, gpu.color.res.Get());
    setTex(NVSDK_NGX_Parameter_Output, gpu.output.res.Get());
    setTex(NVSDK_NGX_Parameter_Depth, gpu.depth.res.Get());
    setTex(NVSDK_NGX_Parameter_MotionVectors, gpu.mvec.res.Get());
    params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, fp.jitter_x);
    params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, fp.jitter_y);
    params->Set(NVSDK_NGX_Parameter_Reset, static_cast<int>(fp.reset));
    params->Set(NVSDK_NGX_Parameter_MV_Scale_X, fp.mv_scale_x);
    params->Set(NVSDK_NGX_Parameter_MV_Scale_Y, fp.mv_scale_y);
    params->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, fp.pre_exposure);
    params->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, fp.exposure_scale);

    params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, static_cast<unsigned int>(fp.rw));
    params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, static_cast<unsigned int>(fp.rh));
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, bx);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, by);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, bx);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, by);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, bx);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, by);
    params->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0u);
    params->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0u);

    setTex(NVSDK_NGX_Parameter_DiffuseAlbedo, gpu.diffuse.res.Get());
    setTex(NVSDK_NGX_Parameter_SpecularAlbedo, gpu.specular.res.Get());
    setTex(NVSDK_NGX_Parameter_GBuffer_Normals, gpu.normals.res.Get());
    setTex(NVSDK_NGX_Parameter_GBuffer_Roughness,
           (guide_mask & rr_ref::kGuideUnpackedRough) ? gpu.rough.res.Get() : nullptr);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_DiffuseAlbedo_Subrect_Base_X, bx);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_DiffuseAlbedo_Subrect_Base_Y, by);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_SpecularAlbedo_Subrect_Base_X, bx);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_SpecularAlbedo_Subrect_Base_Y, by);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_Normals_Subrect_Base_X, bx);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_Normals_Subrect_Base_Y, by);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_Roughness_Subrect_Base_X, bx);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_Roughness_Subrect_Base_Y, by);

    setTex(NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance,
           (guide_mask & rr_ref::kGuideOmitHitDist) ? nullptr : gpu.hit_dist.res.Get());
    params->Set(NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance_Subrect_Base_X, bx);
    params->Set(NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance_Subrect_Base_Y, by);

    params->Set(NVSDK_NGX_Parameter_DLSS_WORLD_TO_VIEW_MATRIX, static_cast<void*>(fp.world_to_view));
    params->Set(NVSDK_NGX_Parameter_DLSS_VIEW_TO_CLIP_MATRIX, static_cast<void*>(fp.view_to_clip));

    // Optional guides (always explicitly set to resource or nullptr so stale bindings never persist)
    setTex(NVSDK_NGX_Parameter_DLSS_DisocclusionMask,
           (guide_mask & rr_ref::kGuideDisocclusion) ? gpu.disocc.res.Get() : nullptr);
    params->Set(NVSDK_NGX_Parameter_DLSS_DisocclusionMask_Subrect_Base_X, bx);
    params->Set(NVSDK_NGX_Parameter_DLSS_DisocclusionMask_Subrect_Base_Y, by);

    setTex(NVSDK_NGX_Parameter_DLSS_TransparencyLayer,
           (guide_mask & rr_ref::kGuideTranspLayer) ? gpu.transp.res.Get() : nullptr);
    params->Set(NVSDK_NGX_Parameter_DLSS_TransparencyLayer_Subrect_Base_X, bx);
    params->Set(NVSDK_NGX_Parameter_DLSS_TransparencyLayer_Subrect_Base_Y, by);

    setTex(NVSDK_NGX_Parameter_DLSS_TransparencyLayerOpacity,
           (guide_mask & rr_ref::kGuideTranspLayer) ? gpu.transp_op.res.Get() : nullptr);
    params->Set(NVSDK_NGX_Parameter_DLSS_TransparencyLayerOpacity_Subrect_Base_X, bx);
    params->Set(NVSDK_NGX_Parameter_DLSS_TransparencyLayerOpacity_Subrect_Base_Y, by);

    setTex(NVSDK_NGX_Parameter_DLSS_TransparencyLayerMvecs,
           (guide_mask & rr_ref::kGuideTranspMvecs) ? gpu.transp_mvec.res.Get() : nullptr);
    params->Set(NVSDK_NGX_Parameter_DLSS_TransparencyLayerMvecs_Subrect_Base_X, 0u);
    params->Set(NVSDK_NGX_Parameter_DLSS_TransparencyLayerMvecs_Subrect_Base_Y, 0u);

    setTex(NVSDK_NGX_Parameter_DLSSD_ScreenSpaceSubsurfaceScatteringGuide,
           (guide_mask & (rr_ref::kGuideSssAndCbt | rr_ref::kGuideSssOnly)) ? gpu.sss.res.Get() : nullptr);
    setTex(NVSDK_NGX_Parameter_DLSSD_ColorBeforeTransparency,
           (guide_mask & (rr_ref::kGuideSssAndCbt | rr_ref::kGuideCbtOnly)) ? gpu.cbt.res.Get() : nullptr);
    params->Set(NVSDK_NGX_Parameter_DLSSD_ColorBeforeTransparency_Subrect_Base_X, bx);
    params->Set(NVSDK_NGX_Parameter_DLSSD_ColorBeforeTransparency_Subrect_Base_Y, by);

    setTex(NVSDK_NGX_Parameter_GBuffer_SpecularMvec,
           (guide_mask & rr_ref::kGuideSpecularMvec) ? gpu.spec_mvec.res.Get() : nullptr);

    setTex(NVSDK_NGX_Parameter_DLSSD_ResponsivityMask,
           (guide_mask & rr_ref::kGuideResponsivity) ? gpu.resp.res.Get() : nullptr);
    params->Set(NVSDK_NGX_Parameter_DLSSD_ResponsivityMask_Subrect_Base_X, bx);
    params->Set(NVSDK_NGX_Parameter_DLSSD_ResponsivityMask_Subrect_Base_Y, by);

    setTex(NVSDK_NGX_Parameter_DLSSD_DepthOfFieldGuide,
           (guide_mask & rr_ref::kGuideDepthOfField) ? gpu.dof.res.Get() : nullptr);
    params->Set(NVSDK_NGX_Parameter_DLSSD_DepthOfFieldGuide_Subrect_Base_X, bx);
    params->Set(NVSDK_NGX_Parameter_DLSSD_DepthOfFieldGuide_Subrect_Base_Y, by);
}

uint64_t QueryNgxReportedDlssdVramBytes(NVSDK_NGX_Parameter* cap_params)
{
    void* cb = nullptr;
    if (!NVSDK_NGX_SUCCEED(cap_params->Get(NVSDK_NGX_Parameter_DLSSDGetStatsCallback, &cb)) || !cb)
        return 0;
    auto fn = reinterpret_cast<PFN_NVSDK_NGX_DLSS_GetStatsCallback>(cb);
    if (!NVSDK_NGX_SUCCEED(fn(cap_params)))
        return 0;
    unsigned long long bytes = 0;
    if (!NVSDK_NGX_SUCCEED(cap_params->Get(NVSDK_NGX_Parameter_SizeInBytes, &bytes)))
        return 0;
    return static_cast<uint64_t>(bytes);
}

// =====================================================================================================================
// Output Pixel Statistics & Environment Manifest
// =====================================================================================================================

struct PixelStats
{
    uint64_t fnv1a64 = 0;
    std::string sha256;
    size_t non_finite = 0;
    size_t non_zero_rgb = 0;
    double min_luma = 0.0;
    double max_luma = 0.0;
    double mean_luma = 0.0;
};

PixelStats AnalyzeOutputRgba16f(const std::vector<uint16_t>& px)
{
    PixelStats st {};
    if (px.empty()) return st;
    st.fnv1a64 = rr_ref::Fnv1a64(px.data(), px.size() * sizeof(uint16_t));
    st.sha256 = rr_ref::Sha256BytesHex(px.data(), px.size() * sizeof(uint16_t));
    st.min_luma = 1e30;
    st.max_luma = -1e30;
    double luma_sum = 0.0;
    const size_t num_px = px.size() / 4;
    size_t valid_px = 0;

    for (size_t i = 0; i < num_px; ++i)
    {
        const uint16_t hr = px[4 * i + 0];
        const uint16_t hg = px[4 * i + 1];
        const uint16_t hb = px[4 * i + 2];
        const uint16_t ha = px[4 * i + 3];
        const bool fin = rr_ref::IsFiniteHalf(hr) && rr_ref::IsFiniteHalf(hg) &&
                         rr_ref::IsFiniteHalf(hb) && rr_ref::IsFiniteHalf(ha);
        if (!fin)
        {
            ++st.non_finite;
            continue;
        }
        if (((hr | hg | hb) & 0x7FFFu) != 0u)
            ++st.non_zero_rgb;
        const double r = rr_ref::HalfToDouble(hr);
        const double g = rr_ref::HalfToDouble(hg);
        const double b = rr_ref::HalfToDouble(hb);
        const double y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
        st.min_luma = std::min(st.min_luma, y);
        st.max_luma = std::max(st.max_luma, y);
        luma_sum += y;
        ++valid_px;
    }
    if (valid_px == 0)
    {
        st.min_luma = 0.0;
        st.max_luma = 0.0;
    }
    else
    {
        st.mean_luma = luma_sum / static_cast<double>(valid_px);
    }
    return st;
}

void SaveRawRgba16f(const fs::path& file_path, const std::vector<uint16_t>& px)
{
    if (file_path.has_parent_path())
        fs::create_directories(file_path.parent_path());
    std::ofstream f(file_path, std::ios::binary);
    Require(static_cast<bool>(f), "Failed to write raw rgba16f: " + WideToUtf8(file_path.wstring()));
    f.write(reinterpret_cast<const char*>(px.data()), static_cast<std::streamsize>(px.size() * sizeof(uint16_t)));
    Require(static_cast<bool>(f), "Short write on raw rgba16f: " + WideToUtf8(file_path.wstring()));
}

struct LoadedDlssdInfo
{
    bool module_loaded = false;
    fs::path loaded_path;
    uint64_t loaded_bytes = 0;
    std::string loaded_sha256;
    std::string loaded_version;
};

LoadedDlssdInfo InspectLoadedDlssdModule()
{
    LoadedDlssdInfo info {};
    HMODULE m = GetModuleHandleW(L"nvngx_dlssd.dll");
    if (!m) return info;
    info.module_loaded = true;
    wchar_t buf[2048] {};
    if (GetModuleFileNameW(m, buf, static_cast<DWORD>(std::size(buf))) > 0)
    {
        info.loaded_path = fs::path(buf);
        info.loaded_version = QueryPeFileVersion(info.loaded_path);
        info.loaded_sha256 = Sha256FileHex(info.loaded_path, &info.loaded_bytes);
    }
    return info;
}

void WriteEnvManifest(
    const fs::path& manifest_path,
    const D3dContext& dx,
    const NgxApi& ngx,
    const fs::path& requested_dlssd,
    const LoadedDlssdInfo& loaded,
    int rr_available,
    uint32_t preset_hint,
    rr_ref::FixtureSuite suite = rr_ref::FixtureSuite::V1)
{
    uint64_t req_bytes = 0;
    const std::string req_sha256 = Sha256FileHex(requested_dlssd, &req_bytes);
    const std::string req_ver = QueryPeFileVersion(requested_dlssd);
    const int hags = QueryWindowsHagsMode();

    DXGI_QUERY_VIDEO_MEMORY_INFO vmem {};
    if (dx.adapter3)
        dx.adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vmem);

    if (manifest_path.has_parent_path())
        fs::create_directories(manifest_path.parent_path());
    std::ofstream out(manifest_path, std::ios::binary);
    out << "{\n"
        << "  \"fixture_version\": \"" << rr_ref::FixtureVersionForSuite(suite) << "\",\n"
        << "  \"gpu\": {\n"
        << "    \"description\": \"" << JsonEscape(WideToUtf8(dx.adapter_desc.Description)) << "\",\n"
        << "    \"vendor_id\": \"" << HexU32(dx.adapter_desc.VendorId) << "\",\n"
        << "    \"device_id\": \"" << HexU32(dx.adapter_desc.DeviceId) << "\",\n"
        << "    \"subsys_id\": \"" << HexU32(dx.adapter_desc.SubSysId) << "\",\n"
        << "    \"revision\": " << dx.adapter_desc.Revision << ",\n"
        << "    \"dedicated_video_memory_mb\": " << (static_cast<double>(dx.adapter_desc.DedicatedVideoMemory) / (1024.0 * 1024.0)) << ",\n"
        << "    \"dxgi_local_budget_mb\": " << (static_cast<double>(vmem.Budget) / (1024.0 * 1024.0)) << ",\n"
        << "    \"d3d12_timestamp_freq_hz\": " << dx.ts_freq << ",\n"
        << "    \"windows_hags_hw_sch_mode\": " << hags << "\n"
        << "  },\n"
        << "  \"ngx\": {\n"
        << "    \"loader_path\": \"" << JsonEscape(WideToUtf8(ngx.loader_path.wstring())) << "\",\n"
        << "    \"loader_version\": \"" << JsonEscape(fs::exists(ngx.loader_path) ? QueryPeFileVersion(ngx.loader_path) : "static_sdk") << "\",\n"
        << "    \"supersampling_denoising_available\": " << rr_available << ",\n"
        << "    \"requested_preset_hint\": " << preset_hint << ",\n"
        << "    \"process_teardown_forced\": " << (g_process_exit_teardown ? "true" : "false") << ",\n"
        << "    \"routing_verification_status\": \"requested_preset_recorded_use_nsys_cuda_trace_for_kernel_evidence\"\n"
        << "  },\n"
        << "  \"dlssd_dll\": {\n"
        << "    \"expected_reference_version\": \"" << kExpectedDlssdVersion << "\",\n"
        << "    \"expected_reference_bytes\": " << kExpectedDlssdBytes << ",\n"
        << "    \"expected_reference_sha256\": \"" << kExpectedDlssdSha256 << "\",\n"
        << "    \"openrr_weight_slice_note\": \"" << kOpenRrWeightSliceNote << "\",\n"
        << "    \"requested_path\": \"" << JsonEscape(WideToUtf8(requested_dlssd.wstring())) << "\",\n"
        << "    \"requested_version\": \"" << JsonEscape(req_ver) << "\",\n"
        << "    \"requested_bytes\": " << req_bytes << ",\n"
        << "    \"requested_sha256\": \"" << req_sha256 << "\",\n"
        << "    \"loaded_in_process\": " << (loaded.module_loaded ? "true" : "false") << ",\n"
        << "    \"loaded_path\": \"" << JsonEscape(WideToUtf8(loaded.loaded_path.wstring())) << "\",\n"
        << "    \"loaded_version\": \"" << JsonEscape(loaded.loaded_version) << "\",\n"
        << "    \"loaded_bytes\": " << loaded.loaded_bytes << ",\n"
        << "    \"loaded_sha256\": \"" << loaded.loaded_sha256 << "\",\n"
        << "    \"matches_requested_sha256\": " << ((loaded.module_loaded && loaded.loaded_sha256 == req_sha256) ? "true" : "false") << ",\n"
        << "    \"matches_expected_310_9_1_0_sha256\": " << ((loaded.module_loaded && loaded.loaded_sha256 == kExpectedDlssdSha256) ? "true" : "false") << "\n"
        << "  }\n"
        << "}\n";
}

// =====================================================================================================================
// Execution Modes: Smoke, Quality, Timing, Probes, Profile-Target
// =====================================================================================================================

struct NgxSession
{
    NgxApi ngx;
    D3dContext dx;
    fs::path abs_dlssd;
    fs::path session_out_dir;
    fs::path ngx_log_dir;
    uint64_t req_bytes = 0;
    std::string req_sha256;
    bool allow_dll_mismatch = false;
    bool dll_verified_after_create = false;
    rr_ref::FixtureSuite suite = rr_ref::FixtureSuite::V1;
    LoadedDlssdInfo verified_loaded {};
    NVSDK_NGX_Parameter* params = nullptr;
    int rr_available = 0;

    void Start(
        const fs::path& dlssd_path,
        const fs::path& loader_path,
        const fs::path& out_dir,
        uint32_t vendor_id,
        bool allow_mismatch,
        rr_ref::FixtureSuite active_suite = rr_ref::FixtureSuite::V1)
    {
        suite = active_suite;
        abs_dlssd = fs::absolute(dlssd_path);
        Require(fs::exists(abs_dlssd), "--dlssd file does not exist: " + WideToUtf8(abs_dlssd.wstring()));
        session_out_dir = fs::absolute(out_dir);
        allow_dll_mismatch = allow_mismatch;
        req_sha256 = Sha256FileHex(abs_dlssd, &req_bytes);

        if (!allow_dll_mismatch)
        {
            Require(req_sha256 == kExpectedDlssdSha256,
                    "Requested --dlssd file (" + WideToUtf8(abs_dlssd.wstring()) +
                    ", version=" + QueryPeFileVersion(abs_dlssd) +
                    ", sha256=" + req_sha256 +
                    ") does NOT match expected official 310.9.1.0 SHA-256 (" + kExpectedDlssdSha256 +
                    "). Pass --allow-dll-mismatch only if intentionally testing another DLL.");
        }

        ngx_log_dir = session_out_dir / "ngx_appdata";
        fs::create_directories(ngx_log_dir);

        RuntimeTrace("[INIT] D3D12 begin");
        dx.Init(vendor_id);
        RuntimeTrace("[INIT] D3D12 ready, NGX Load begin");
        ngx.Load(loader_path);
        RuntimeTrace("[INIT] NGX loader ready");

        const std::wstring dll_dir_w = abs_dlssd.parent_path().wstring();
        const wchar_t* search_paths[] = {dll_dir_w.c_str()};
        NVSDK_NGX_FeatureCommonInfo fci {};
        fci.PathListInfo.Path = search_paths;
        fci.PathListInfo.Length = 1u;

        RuntimeTrace("[INIT] NGX Init begin");
        const NVSDK_NGX_Result init_res = ngx.Init(0x5252434E54525354ull, ngx_log_dir.c_str(), dx.dev.Get(), &fci);
        Require(NVSDK_NGX_SUCCEED(init_res), "NVSDK_NGX_D3D12_Init failed: " + HexU32(static_cast<uint32_t>(init_res)));

        RuntimeTrace("[INIT] NGX Init success, GetParams begin");
        const NVSDK_NGX_Result pr = ngx.GetParams(&params);
        Require(NVSDK_NGX_SUCCEED(pr) && params != nullptr, "GetCapabilityParameters failed: " + HexU32(static_cast<uint32_t>(pr)));
        RuntimeTrace("[INIT] GetParams success, Get availability begin");
        params->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &rr_available);
        std::fprintf(stderr,"[INIT] availability=%d\n",rr_available);
    }

    const LoadedDlssdInfo& EnsureLoadedDlssdVerified(uint32_t preset_hint, const std::string& manifest_filename = "env_manifest.json")
    {
        if (dll_verified_after_create)
            return verified_loaded;

        verified_loaded = InspectLoadedDlssdModule();
        WriteEnvManifest(session_out_dir / manifest_filename, dx, ngx, abs_dlssd, verified_loaded, rr_available, preset_hint, suite);

        if (!allow_dll_mismatch)
        {
            Require(verified_loaded.module_loaded,
                    "nvngx_dlssd.dll was not found in process modules after CreateFeature! Check " +
                    WideToUtf8((session_out_dir / manifest_filename).wstring()));
            Require(verified_loaded.loaded_sha256 == req_sha256,
                    "Loaded nvngx_dlssd.dll (" + WideToUtf8(verified_loaded.loaded_path.wstring()) +
                    ", sha256=" + verified_loaded.loaded_sha256 +
                    ") does NOT match requested --dlssd (" + req_sha256 +
                    ")! Driver/NVIDIA App override may have redirected it. Disable DLSS override or pass --allow-dll-mismatch.");
            Require(verified_loaded.loaded_sha256 == kExpectedDlssdSha256,
                    "Loaded nvngx_dlssd.dll (version=" + verified_loaded.loaded_version +
                    ", sha256=" + verified_loaded.loaded_sha256 +
                    ") does NOT match expected 310.9.1.0 SHA-256 (" + kExpectedDlssdSha256 +
                    ")! Pass --allow-dll-mismatch only if intentional.");
        }

        dll_verified_after_create = true;
        return verified_loaded;
    }

    ~NgxSession()
    {
        if (params)
        {
#ifdef RR_USE_OFFICIAL_NGX_SDK
            ngx.DestroyParams(params);
#else
            RuntimeTrace("[CLEANUP] destroy params begin");
            ngx.destroy_params(params);
            RuntimeTrace("[CLEANUP] destroy params done");
#endif
            params = nullptr;
        }
        if (g_process_exit_teardown) {
            RuntimeTrace("[CLEANUP] driver Shutdown skipped; explicit process-isolation teardown policy");
        } else if (dx.dev) {
            RuntimeTrace("[CLEANUP] Shutdown begin");
            ngx.Shutdown(dx.dev.Get());
            RuntimeTrace("[CLEANUP] Shutdown done");
        }
    }
};

void RunSmokeMode(const fs::path& dlssd_path, const fs::path& loader_path, const fs::path& out_dir,
                  bool allow_dll_mismatch, uint32_t vendor_id, rr_ref::FixtureSuite suite)
{
    fs::create_directories(out_dir);
    NgxSession sess;
    sess.Start(dlssd_path, loader_path, out_dir, vendor_id, allow_dll_mismatch, suite);

    const uint32_t rw = 1280, rh = 720, ow = 1920, oh = 1080;
    const rr_ref::SceneId smoke_scene = (suite == rr_ref::FixtureSuite::V2_Motion)
        ? rr_ref::SceneId::S1_TemporalHalton
        : rr_ref::SceneId::S0_BenchParity;

    rr_ref::CpuFrameBuffers cpu;
    cpu.Allocate(rw, rh, ow, oh, rr_ref::kGuideNone);
    if (smoke_scene == rr_ref::SceneId::S0_BenchParity)
        rr_ref::PopulateStaticS0(cpu);

    GpuSceneResources gpu;
    gpu.Allocate(sess.dx.dev.Get(), rw, rh, ow, oh, rr_ref::kGuideNone, true);
    if (smoke_scene == rr_ref::SceneId::S0_BenchParity)
        gpu.UploadFrame(sess.dx, cpu, rw, rh, rr_ref::kGuideNone);

    NVSDK_NGX_Handle* handle = nullptr;
    const NVSDK_NGX_Result cr = CreateRrFeature(sess.ngx, sess.dx, sess.params, rw, rh, ow, oh,
                                                NVSDK_NGX_PerfQuality_Value_MaxQuality, 6u, &handle);
    Require(NVSDK_NGX_SUCCEED(cr) && handle != nullptr, "Smoke CreateFeature(RayReconstruction) failed: " + HexU32(static_cast<uint32_t>(cr)));

    const LoadedDlssdInfo& loaded = sess.EnsureLoadedDlssdVerified(6u, "env_manifest.json");

    std::vector<uint16_t> px_f000, px_f015_normal, px_f015_reset;
    PixelStats st_f000 {}, st_f015_normal {}, st_f015_reset {};

    for (uint32_t f = 0; f < 16; ++f)
    {
        rr_ref::FrameScalarParams fp {};
        rr_ref::PrepareFrame(smoke_scene, rr_ref::kGuideNone, f, rw, rh, ow, oh,
                             f == 0 ? 1 : 0, 6u, NVSDK_NGX_PerfQuality_Value_MaxQuality, fp, cpu, suite);
        if (smoke_scene != rr_ref::SceneId::S0_BenchParity)
            gpu.UploadFrame(sess.dx, cpu, rw, rh, rr_ref::kGuideNone);
        BindEvalParams(sess.params, fp, rr_ref::kGuideNone, gpu);

        CheckHr(sess.dx.ca->Reset(), "Smoke eval ca->Reset");
        CheckHr(sess.dx.cl->Reset(sess.dx.ca.Get(), nullptr), "Smoke eval cl->Reset");
#ifdef RR_USE_OFFICIAL_NGX_SDK
        const NVSDK_NGX_Result er = NVSDK_NGX_D3D12_EvaluateFeature(sess.dx.cl.Get(), handle, sess.params, nullptr);
#else
        const NVSDK_NGX_Result er = sess.ngx.evaluate_feature(sess.dx.cl.Get(), handle, sess.params, nullptr);
#endif
        Require(NVSDK_NGX_SUCCEED(er), "Smoke EvaluateFeature failed on frame " + std::to_string(f) + ": " + HexU32(static_cast<uint32_t>(er)));
        sess.dx.ExecuteAndSync("Smoke EvaluateFeature");

        if (f == 0)
        {
            gpu.ReadbackOutputPixels(sess.dx, px_f000);
            st_f000 = AnalyzeOutputRgba16f(px_f000);
            SaveRawRgba16f(out_dir / "f000.rgba16f", px_f000);
        }
        else if (f == 15)
        {
            gpu.ReadbackOutputPixels(sess.dx, px_f015_normal);
            st_f015_normal = AnalyzeOutputRgba16f(px_f015_normal);
            SaveRawRgba16f(out_dir / "f015_normal.rgba16f", px_f015_normal);
        }
    }

    // Re-evaluate exact same frame 15 input with Reset=1 to verify temporal history sensitivity
    {
        rr_ref::FrameScalarParams fp {};
        rr_ref::PrepareFrame(smoke_scene, rr_ref::kGuideNone, 15u, rw, rh, ow, oh,
                             1, 6u, NVSDK_NGX_PerfQuality_Value_MaxQuality, fp, cpu, suite);
        BindEvalParams(sess.params, fp, rr_ref::kGuideNone, gpu);
        CheckHr(sess.dx.ca->Reset(), "Smoke reset-probe ca->Reset");
        CheckHr(sess.dx.cl->Reset(sess.dx.ca.Get(), nullptr), "Smoke reset-probe cl->Reset");
#ifdef RR_USE_OFFICIAL_NGX_SDK
        const NVSDK_NGX_Result er = NVSDK_NGX_D3D12_EvaluateFeature(sess.dx.cl.Get(), handle, sess.params, nullptr);
#else
        const NVSDK_NGX_Result er = sess.ngx.evaluate_feature(sess.dx.cl.Get(), handle, sess.params, nullptr);
#endif
        Require(NVSDK_NGX_SUCCEED(er), "Smoke forced-reset EvaluateFeature failed: " + HexU32(static_cast<uint32_t>(er)));
        sess.dx.ExecuteAndSync("Smoke forced-reset EvaluateFeature");
        gpu.ReadbackOutputPixels(sess.dx, px_f015_reset);
        st_f015_reset = AnalyzeOutputRgba16f(px_f015_reset);
        SaveRawRgba16f(out_dir / "f015_forced_reset.rgba16f", px_f015_reset);
    }

#ifdef RR_USE_OFFICIAL_NGX_SDK
    NVSDK_NGX_D3D12_ReleaseFeature(handle);
#else
    sess.ngx.release_feature(handle);
#endif

    Require(st_f000.non_finite == 0 && st_f015_normal.non_finite == 0, "Smoke output contains NaN/Inf half-floats");
    Require(st_f015_normal.non_zero_rgb > (size_t(ow) * oh) / 4, "Smoke output image is unexpectedly blank/zero");
    const bool history_active = (st_f015_normal.sha256 != st_f015_reset.sha256);

    std::ofstream sum(out_dir / "smoke_summary.json", std::ios::binary);
    sum << "{\n"
        << "  \"status\": \"PASS\",\n"
        << "  \"fixture_version\": \"" << rr_ref::FixtureVersionForSuite(suite) << "\",\n"
        << "  \"scene\": \"" << rr_ref::SceneName(smoke_scene) << "\",\n"
        << "  \"f000_output_sha256\": \"" << st_f000.sha256 << "\",\n"
        << "  \"f015_normal_output_sha256\": \"" << st_f015_normal.sha256 << "\",\n"
        << "  \"f015_forced_reset_output_sha256\": \"" << st_f015_reset.sha256 << "\",\n"
        << "  \"temporal_history_active\": " << (history_active ? "true" : "false") << ",\n"
        << "  \"f015_mean_luma\": " << st_f015_normal.mean_luma << ",\n"
        << "  \"loaded_dlssd_version\": \"" << JsonEscape(loaded.loaded_version) << "\",\n"
        << "  \"loaded_dlssd_sha256\": \"" << loaded.loaded_sha256 << "\",\n"
        << "  \"matches_expected_310_9_1_0_sha256\": " << ((loaded.loaded_sha256 == kExpectedDlssdSha256) ? "true" : "false") << "\n"
        << "}\n";

    std::printf("[PASS] Smoke test succeeded on %s | suite=%s | DLL=%s (%s) | temporal_history_active=%s\n",
                WideToUtf8(sess.dx.adapter_desc.Description).c_str(),
                rr_ref::FixtureVersionForSuite(suite),
                loaded.loaded_version.c_str(),
                loaded.loaded_sha256.substr(0, 16).c_str(),
                history_active ? "true" : "false");
}

void RunQualityCase(NgxSession& sess, const SuiteCase& sc, const fs::path& case_dir, rr_ref::FixtureSuite suite)
{
    fs::create_directories(case_dir);
    rr_ref::CpuFrameBuffers cpu;
    cpu.Allocate(sc.alloc_rw, sc.alloc_rh, sc.out_w, sc.out_h, sc.guide_mask);
    if (sc.scene == rr_ref::SceneId::S0_BenchParity)
        rr_ref::PopulateStaticS0(cpu);

    GpuSceneResources gpu;
    gpu.Allocate(sess.dx.dev.Get(), sc.alloc_rw, sc.alloc_rh, sc.out_w, sc.out_h, sc.guide_mask, true);
    if (sc.scene == rr_ref::SceneId::S0_BenchParity)
        gpu.UploadFrame(sess.dx, cpu, sc.alloc_rw, sc.alloc_rh, sc.guide_mask);

    const uint32_t roughness_mode = (suite == rr_ref::FixtureSuite::V2_Motion && (sc.guide_mask & rr_ref::kGuideUnpackedRough) != 0u) ? 0u : 1u;
    const int32_t create_flags = (suite == rr_ref::FixtureSuite::V2_Motion) ? sc.v2_mod.create_flags : 0x4B;

    NVSDK_NGX_Handle* handle = nullptr;
    const NVSDK_NGX_Result cr = CreateRrFeature(sess.ngx, sess.dx, sess.params,
                                                sc.alloc_rw, sc.alloc_rh, sc.out_w, sc.out_h,
                                                sc.perf_quality, sc.preset_hint, &handle,
                                                create_flags, roughness_mode);
    Require(NVSDK_NGX_SUCCEED(cr) && handle != nullptr,
            sc.id + ": CreateFeature failed: " + HexU32(static_cast<uint32_t>(cr)));
    sess.EnsureLoadedDlssdVerified(sc.preset_hint);

    std::ofstream csv(case_dir / "frames_manifest.csv", std::ios::binary);
    csv << "frame,rw,rh,initial_reset,initial_ngx_result_hex,initial_input_sha256,recreated,effective_reset,effective_ngx_result_hex,effective_input_sha256,output_fnv1a64,output_sha256,non_finite,min_luma,max_luma,mean_luma\n";

    const uint32_t total_frames = sc.steady_frames + sc.drs_frames;
    const uint32_t drs_w = (sc.steady_rw * 85u / 100u) & ~1u;
    const uint32_t drs_h = (sc.steady_rh * 85u / 100u) & ~1u;

    std::vector<uint16_t> out_px;
    for (uint32_t f = 0; f < total_frames; ++f)
    {
        uint32_t rw = sc.steady_rw;
        uint32_t rh = sc.steady_rh;
        int32_t initial_reset = (f == 0) ? 1 : 0;

        if (sc.has_drs && f >= sc.steady_frames)
        {
            const uint32_t di = f - sc.steady_frames; // 0..15
            // 0..3: 85% Reset=0; 4..7: 100% Reset=0; 8..11: 85% Reset=1 on switch; 12..15: 100% Reset=1 on switch
            const bool lower = ((di / 4u) & 1u) == 0u;
            rw = lower ? drs_w : sc.steady_rw;
            rh = lower ? drs_h : sc.steady_rh;
            initial_reset = (di >= 8u && (di % 4u) == 0u) ? 1 : 0;
        }

        rr_ref::FrameScalarParams fp {};
        rr_ref::PrepareFrame(sc.scene, sc.guide_mask, f, rw, rh, sc.out_w, sc.out_h,
                             initial_reset, sc.preset_hint, sc.perf_quality, fp, cpu, suite, sc.v2_mod);
        if (sc.scene != rr_ref::SceneId::S0_BenchParity)
            gpu.UploadFrame(sess.dx, cpu, rw, rh, sc.guide_mask, fp.in_base_x, fp.in_base_y);

        const std::string initial_in_sha = rr_ref::ComputeCanonicalFrameInputSha256(fp, sc.guide_mask, cpu, suite);

        int recreated = 0;
        int32_t effective_reset = initial_reset;
        std::string effective_in_sha = initial_in_sha;
        NVSDK_NGX_Result initial_er = NVSDK_NGX_Result_FAIL_FeatureNotFound;
        NVSDK_NGX_Result effective_er = NVSDK_NGX_Result_FAIL_FeatureNotFound;

        if (handle != nullptr)
        {
            BindEvalParams(sess.params, fp, sc.guide_mask, gpu);
            CheckHr(sess.dx.ca->Reset(), "Quality eval ca->Reset");
            CheckHr(sess.dx.cl->Reset(sess.dx.ca.Get(), nullptr), "Quality eval cl->Reset");
#ifdef RR_USE_OFFICIAL_NGX_SDK
            initial_er = NVSDK_NGX_D3D12_EvaluateFeature(sess.dx.cl.Get(), handle, sess.params, nullptr);
#else
            initial_er = sess.ngx.evaluate_feature(sess.dx.cl.Get(), handle, sess.params, nullptr);
#endif
            effective_er = initial_er;
            if (NVSDK_NGX_SUCCEED(initial_er))
                sess.dx.ExecuteAndSync("Quality EvaluateFeature");
            else
                CheckHr(sess.dx.cl->Close(), "Close failed DRS eval cl");
        }

        if (!NVSDK_NGX_SUCCEED(effective_er))
        {
            // On DRS frames, record initial failure cleanly and fall back to feature recreate at (rw, rh) with Reset=1
            if (sc.has_drs && f >= sc.steady_frames)
            {
                if (handle)
                {
#ifdef RR_USE_OFFICIAL_NGX_SDK
                    NVSDK_NGX_D3D12_ReleaseFeature(handle);
#else
                    sess.ngx.release_feature(handle);
#endif
                    handle = nullptr;
                }
                const NVSDK_NGX_Result rcr = CreateRrFeature(sess.ngx, sess.dx, sess.params, rw, rh, sc.out_w, sc.out_h,
                                                             sc.perf_quality, sc.preset_hint, &handle,
                                                             create_flags, roughness_mode);
                recreated = 1;
                effective_reset = 1;
                fp.reset = 1;
                effective_in_sha = rr_ref::ComputeCanonicalFrameInputSha256(fp, sc.guide_mask, cpu, suite);

                if (NVSDK_NGX_SUCCEED(rcr) && handle != nullptr)
                {
                    BindEvalParams(sess.params, fp, sc.guide_mask, gpu);
                    CheckHr(sess.dx.ca->Reset(), "Quality recreated ca->Reset");
                    CheckHr(sess.dx.cl->Reset(sess.dx.ca.Get(), nullptr), "Quality recreated cl->Reset");
#ifdef RR_USE_OFFICIAL_NGX_SDK
                    effective_er = NVSDK_NGX_D3D12_EvaluateFeature(sess.dx.cl.Get(), handle, sess.params, nullptr);
#else
                    effective_er = sess.ngx.evaluate_feature(sess.dx.cl.Get(), handle, sess.params, nullptr);
#endif
                    if (NVSDK_NGX_SUCCEED(effective_er))
                        sess.dx.ExecuteAndSync("Quality recreated EvaluateFeature");
                    else
                        CheckHr(sess.dx.cl->Close(), "Close failed recreated cl");
                }
                else
                {
                    effective_er = rcr;
                }
            }
            else
            {
                throw std::runtime_error(sc.id + ": EvaluateFeature failed at steady frame " + std::to_string(f) + ": " + HexU32(static_cast<uint32_t>(initial_er)));
            }
        }

        PixelStats pst {};
        if (NVSDK_NGX_SUCCEED(effective_er))
        {
            gpu.ReadbackOutputPixels(sess.dx, out_px);
            pst = AnalyzeOutputRgba16f(out_px);
            if (std::find(sc.save_frames.begin(), sc.save_frames.end(), f) != sc.save_frames.end())
            {
                char fname[64];
                std::snprintf(fname, sizeof(fname), "f%03u_%ux%u_to_%ux%u.rgba16f", f, rw, rh, sc.out_w, sc.out_h);
                SaveRawRgba16f(case_dir / fname, out_px);
            }
        }

        csv << f << "," << rw << "," << rh << ","
            << initial_reset << "," << HexU32(static_cast<uint32_t>(initial_er)) << "," << initial_in_sha << ","
            << recreated << "," << effective_reset << "," << HexU32(static_cast<uint32_t>(effective_er)) << "," << effective_in_sha << ","
            << HexU64(pst.fnv1a64) << "," << pst.sha256 << ","
            << pst.non_finite << "," << std::fixed << std::setprecision(6)
            << pst.min_luma << "," << pst.max_luma << "," << pst.mean_luma << "\n";
    }

    if (handle)
    {
#ifdef RR_USE_OFFICIAL_NGX_SDK
        NVSDK_NGX_D3D12_ReleaseFeature(handle);
#else
        sess.ngx.release_feature(handle);
#endif
    }
    csv.flush();
    std::printf("[DONE] %s (%u frames, %zu saved raw frames)\n", sc.id.c_str(), total_frames, sc.save_frames.size());
}

struct DistStats
{
    double mean = 0.0, median = 0.0, p95 = 0.0, p99 = 0.0, min_v = 0.0, max_v = 0.0, stddev = 0.0;
};

DistStats ComputeDistStats(std::vector<double> v)
{
    if (v.empty()) return {};
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    const double sum = std::accumulate(v.begin(), v.end(), 0.0);
    const double mean = sum / static_cast<double>(n);
    double sq_sum = 0.0;
    for (double x : v) sq_sum += (x - mean) * (x - mean);
    return {
        mean,
        v[n / 2],
        v[std::min(n - 1, static_cast<size_t>(static_cast<double>(n) * 0.95))],
        v[std::min(n - 1, static_cast<size_t>(static_cast<double>(n) * 0.99))],
        v.front(),
        v.back(),
        std::sqrt(sq_sum / static_cast<double>(n))
    };
}

void GpuPreSoak3Seconds(D3dContext& dx)
{
    std::printf("[TIMING] Running 3.0s GPU pre-soak to stabilize boost clock state...\n");
    GpuTex2D a, b;
    a.Create(dx.dev.Get(), 2048, 2048, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, false, D3D12_RESOURCE_STATE_COPY_SOURCE);
    b.Create(dx.dev.Get(), 2048, 2048, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, false, D3D12_RESOURCE_STATE_COPY_DEST);
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 3.0)
    {
        CheckHr(dx.ca->Reset(), "Soak ca->Reset");
        CheckHr(dx.cl->Reset(dx.ca.Get(), nullptr), "Soak cl->Reset");
        for (int i = 0; i < 16; ++i)
            dx.cl->CopyResource(b.res.Get(), a.res.Get());
        dx.ExecuteAndSync("GpuPreSoak3Seconds");
    }
}

void RunTimingCaseRound(NgxSession& sess, const SuiteCase& sc, uint32_t round_idx, const fs::path& case_dir)
{
    fs::create_directories(case_dir);
    rr_ref::CpuFrameBuffers cpu;
    cpu.Allocate(sc.alloc_rw, sc.alloc_rh, sc.out_w, sc.out_h, 0u);
    rr_ref::PopulateStaticS0(cpu);

    GpuSceneResources gpu;
    gpu.Allocate(sess.dx.dev.Get(), sc.alloc_rw, sc.alloc_rh, sc.out_w, sc.out_h, 0u, false); // ZERO readback allocation!
    gpu.UploadFrame(sess.dx, cpu, sc.alloc_rw, sc.alloc_rh, 0u);

    const uint64_t vram_before_create = sess.dx.QueryProcessLocalVramBytes();
    NVSDK_NGX_Handle* handle = nullptr;
    const NVSDK_NGX_Result cr = CreateRrFeature(sess.ngx, sess.dx, sess.params,
                                                sc.alloc_rw, sc.alloc_rh, sc.out_w, sc.out_h,
                                                sc.perf_quality, sc.preset_hint, &handle);
    Require(NVSDK_NGX_SUCCEED(cr) && handle != nullptr, sc.id + ": Timing CreateFeature failed: " + HexU32(static_cast<uint32_t>(cr)));
    sess.EnsureLoadedDlssdVerified(sc.preset_hint);

    const uint64_t vram_after_create = sess.dx.QueryProcessLocalVramBytes();
    const size_t scratch_bytes = sess.ngx.QueryScratchBytes(sess.params);

    struct TimedEvalResult
    {
        NVSDK_NGX_Result rc = NVSDK_NGX_Result_Fail;
        double gpu_ms = -1.0;
    };

    auto evalTimed = [&](uint32_t f_idx, uint32_t rw, uint32_t rh, int32_t reset) -> TimedEvalResult {
        rr_ref::FrameScalarParams fp {};
        rr_ref::PrepareFrame(rr_ref::SceneId::S0_BenchParity, 0u, f_idx, rw, rh, sc.out_w, sc.out_h,
                             reset, sc.preset_hint, sc.perf_quality, fp, cpu);
        BindEvalParams(sess.params, fp, 0u, gpu);

        CheckHr(sess.dx.ca->Reset(), "Timing ca->Reset");
        CheckHr(sess.dx.cl->Reset(sess.dx.ca.Get(), nullptr), "Timing cl->Reset");
        sess.dx.cl->EndQuery(sess.dx.ts_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
#ifdef RR_USE_OFFICIAL_NGX_SDK
        const NVSDK_NGX_Result er = NVSDK_NGX_D3D12_EvaluateFeature(sess.dx.cl.Get(), handle, sess.params, nullptr);
#else
        const NVSDK_NGX_Result er = sess.ngx.evaluate_feature(sess.dx.cl.Get(), handle, sess.params, nullptr);
#endif
        if (!NVSDK_NGX_SUCCEED(er))
        {
            CheckHr(sess.dx.cl->Close(), "Close failed Timing cl");
            return {er, -1.0};
        }
        sess.dx.cl->EndQuery(sess.dx.ts_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        sess.dx.cl->ResolveQueryData(sess.dx.ts_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, sess.dx.ts_readback.Get(), 0);
        sess.dx.ExecuteAndSync("Timing EvaluateFeature");
        return {er, sess.dx.ReadLastGpuMs()};
    };

    uint64_t vram_after_first_eval = 0;
    for (uint32_t i = 0; i < sc.warmup_frames; ++i)
    {
        const TimedEvalResult tr = evalTimed(i, sc.steady_rw, sc.steady_rh, i == 0 ? 1 : 0);
        Require(NVSDK_NGX_SUCCEED(tr.rc), sc.id + ": Timing warmup EvaluateFeature failed at frame " + std::to_string(i) + ": " + HexU32(static_cast<uint32_t>(tr.rc)));
        if (i == 0) vram_after_first_eval = sess.dx.QueryProcessLocalVramBytes();
    }

    const std::string tag = "r" + std::to_string(round_idx);
    std::ofstream csv(case_dir / ("per_frame_gpu_ms_" + tag + ".csv"), std::ios::binary);
    csv << "phase,index,rw,rh,reset,ngx_result_hex,gpu_ms\n";

    std::vector<double> steady_ms;
    steady_ms.reserve(sc.steady_frames);
    for (uint32_t i = 0; i < sc.steady_frames; ++i)
    {
        const TimedEvalResult tr = evalTimed(sc.warmup_frames + i, sc.steady_rw, sc.steady_rh, 0);
        Require(NVSDK_NGX_SUCCEED(tr.rc), sc.id + ": Timing steady EvaluateFeature failed at frame " + std::to_string(i) + ": " + HexU32(static_cast<uint32_t>(tr.rc)));
        steady_ms.push_back(tr.gpu_ms);
        csv << "steady," << i << "," << sc.steady_rw << "," << sc.steady_rh << ",0,"
            << HexU32(static_cast<uint32_t>(tr.rc)) << "," << std::fixed << std::setprecision(6) << tr.gpu_ms << "\n";
    }
    csv.flush();

    const uint64_t vram_steady_end = sess.dx.QueryProcessLocalVramBytes();
    const uint64_t ngx_stats_vram = QueryNgxReportedDlssdVramBytes(sess.params);
    const DistStats st_s = ComputeDistStats(steady_ms);
    auto toMb = [](uint64_t b) { return static_cast<double>(b) / (1024.0 * 1024.0); };
    const double delta_steady_mb = vram_steady_end > vram_before_create ? toMb(vram_steady_end - vram_before_create) : 0.0;

    auto writeSummaryJson = [&](const std::string& drs_status,
                                NVSDK_NGX_Result drs_err,
                                int drs_failed_idx,
                                const std::vector<double>& drs_samples,
                                uint64_t vram_drs) {
        const DistStats st_d = ComputeDistStats(drs_samples);
        std::ofstream js(case_dir / ("timing_summary_" + tag + ".json"), std::ios::binary);
        js << std::fixed << std::setprecision(4)
           << "{\n"
           << "  \"case_id\": \"" << sc.id << "\",\n"
           << "  \"round\": " << round_idx << ",\n"
           << "  \"resolution\": \"" << sc.steady_rw << "x" << sc.steady_rh << "->" << sc.out_w << "x" << sc.out_h << "\",\n"
           << "  \"steady_frames\": " << steady_ms.size() << ",\n"
           << "  \"steady_gpu_ms\": { \"mean\": " << st_s.mean << ", \"median\": " << st_s.median
           << ", \"p95\": " << st_s.p95 << ", \"p99\": " << st_s.p99
           << ", \"min\": " << st_s.min_v << ", \"max\": " << st_s.max_v << ", \"stddev\": " << st_s.stddev << " },\n"
           << "  \"drs_status\": \"" << drs_status << "\",\n"
           << "  \"drs_error_hex\": \"" << HexU32(static_cast<uint32_t>(drs_err)) << "\",\n"
           << "  \"drs_failed_at_index\": " << drs_failed_idx << ",\n"
           << "  \"drs_completed_frames\": " << drs_samples.size() << ",\n"
           << "  \"drs_gpu_ms\": { \"mean\": " << st_d.mean << ", \"median\": " << st_d.median
           << ", \"p95\": " << st_d.p95 << ", \"p99\": " << st_d.p99
           << ", \"min\": " << st_d.min_v << ", \"max\": " << st_d.max_v << ", \"stddev\": " << st_d.stddev << " },\n"
           << "  \"vram_mb\": {\n"
           << "    \"dxgi_process_local_before_create\": " << toMb(vram_before_create) << ",\n"
           << "    \"dxgi_process_local_after_create\": " << toMb(vram_after_create) << ",\n"
           << "    \"dxgi_process_local_after_first_eval\": " << toMb(vram_after_first_eval) << ",\n"
           << "    \"dxgi_process_local_steady_end\": " << toMb(vram_steady_end) << ",\n"
           << "    \"dxgi_process_local_drs_end\": " << toMb(vram_drs) << ",\n"
           << "    \"dxgi_process_local_delta_steady\": " << delta_steady_mb << ",\n"
           << "    \"ngx_dlssd_get_stats_mb\": " << toMb(ngx_stats_vram) << ",\n"
           << "    \"ngx_scratch_buffer_mb\": " << toMb(scratch_bytes) << "\n"
           << "  }\n"
           << "}\n";
    };

    // Persist steady summary immediately BEFORE running DRS so 240-frame steady data is never lost
    std::string drs_status = (sc.has_drs && sc.drs_frames > 0) ? "pending" : "not_applicable";
    NVSDK_NGX_Result drs_error = NVSDK_NGX_Result_Success;
    int drs_failed_idx = -1;
    std::vector<double> drs_ms;
    writeSummaryJson(drs_status, drs_error, drs_failed_idx, drs_ms, vram_steady_end);

    if (sc.has_drs && sc.drs_frames > 0)
    {
        const uint32_t drs_w = (sc.steady_rw * 85u / 100u) & ~1u;
        const uint32_t drs_h = (sc.steady_rh * 85u / 100u) & ~1u;
        drs_ms.reserve(sc.drs_frames);
        drs_status = "ok";
        for (uint32_t i = 0; i < sc.drs_frames; ++i)
        {
            const bool lower = ((i / 4u) & 1u) != 0u;
            const int32_t switched = (i % 4u) == 0u ? 1 : 0; // Matches openrr_bench.cpp:591-592
            const uint32_t rw = lower ? drs_w : sc.steady_rw;
            const uint32_t rh = lower ? drs_h : sc.steady_rh;
            const TimedEvalResult tr = evalTimed(sc.warmup_frames + sc.steady_frames + i, rw, rh, switched);
            csv << "drs," << i << "," << rw << "," << rh << "," << switched << ","
                << HexU32(static_cast<uint32_t>(tr.rc)) << "," << std::fixed << std::setprecision(6) << tr.gpu_ms << "\n";
            if (!NVSDK_NGX_SUCCEED(tr.rc))
            {
                drs_status = "unsupported";
                drs_error = tr.rc;
                drs_failed_idx = static_cast<int>(i);
                std::printf("[WARN] %s %s: DRS subrect %ux%u rejected at drs_index=%u (%s); steady 240f preserved, continuing.\n",
                            sc.id.c_str(), tag.c_str(), rw, rh, i, HexU32(static_cast<uint32_t>(tr.rc)).c_str());
                break;
            }
            drs_ms.push_back(tr.gpu_ms);
        }
        csv.flush();
    }
    const uint64_t vram_drs_end = sess.dx.QueryProcessLocalVramBytes();

#ifdef RR_USE_OFFICIAL_NGX_SDK
    NVSDK_NGX_D3D12_ReleaseFeature(handle);
#else
    sess.ngx.release_feature(handle);
#endif

    writeSummaryJson(drs_status, drs_error, drs_failed_idx, drs_ms, vram_drs_end);
    const DistStats st_d = ComputeDistStats(drs_ms);

    std::printf("[TIMING %s %s] steady median=%.3f ms (mean=%.3f, p95=%.3f) | DRS=%s (median=%.3f ms) | VRAM delta=%.1f MB (NGX stats=%.1f MB)\n",
                sc.id.c_str(), tag.c_str(), st_s.median, st_s.mean, st_s.p95,
                drs_status.c_str(), st_d.median, delta_steady_mb, toMb(ngx_stats_vram));
}

struct ImageDiffStats
{
    size_t diff_halfs = 0;
    uint32_t max_ulp = 0;
    double psnr01_db = 999.0;
};

ImageDiffStats CompareRgba16f(const std::vector<uint16_t>& ref_px, const std::vector<uint16_t>& cur_px)
{
    ImageDiffStats d {};
    if (ref_px.size() != cur_px.size() || ref_px.empty())
        return d;
    double se = 0.0;
    size_t rgb_n = 0;
    for (size_t i = 0; i < cur_px.size(); ++i)
    {
        const uint16_t a = ref_px[i], b = cur_px[i];
        if (a != b)
        {
            ++d.diff_halfs;
            d.max_ulp = std::max(d.max_ulp, static_cast<uint32_t>(a > b ? a - b : b - a));
        }
        if ((i & 3u) != 3u && rr_ref::IsFiniteHalf(a) && rr_ref::IsFiniteHalf(b))
        {
            const double da = std::clamp(rr_ref::HalfToDouble(a), 0.0, 1.0);
            const double db = std::clamp(rr_ref::HalfToDouble(b), 0.0, 1.0);
            se += (da - db) * (da - db);
            ++rgb_n;
        }
    }
    if (se > 0.0 && rgb_n > 0)
        d.psnr01_db = 10.0 * std::log10(1.0 / (se / static_cast<double>(rgb_n)));
    return d;
}

bool TryLoadRawRgba16f(const fs::path& file_path, size_t expected_halfs, std::vector<uint16_t>& out_px)
{
    std::error_code ec;
    if (!fs::exists(file_path, ec) || fs::file_size(file_path, ec) != expected_halfs * sizeof(uint16_t))
        return false;
    std::ifstream f(file_path, std::ios::binary);
    if (!f) return false;
    out_px.resize(expected_halfs);
    f.read(reinterpret_cast<char*>(out_px.data()), static_cast<std::streamsize>(expected_halfs * sizeof(uint16_t)));
    return static_cast<bool>(f);
}

void RunProbesPass(NgxSession& sess, const std::vector<SuiteCase>& probes, const fs::path& out_dir, rr_ref::FixtureSuite suite)
{
    const bool is_v2 = (suite == rr_ref::FixtureSuite::V2_Motion);
    fs::create_directories(out_dir);
    std::map<std::string, std::vector<uint16_t>> saved_f015;
    std::ofstream js(out_dir / "probes_summary.json", std::ios::binary);
    js << "{\n"
       << "  \"fixture_version\": \"" << rr_ref::FixtureVersionForSuite(suite) << "\",\n"
       << "  \"probes\": [\n";

    for (size_t idx = 0; idx < probes.size(); ++idx)
    {
        const auto& sc = probes[idx];
        const fs::path pdir = out_dir / sc.id;
        fs::create_directories(pdir);

        rr_ref::CpuFrameBuffers cpu;
        cpu.Allocate(sc.alloc_rw, sc.alloc_rh, sc.out_w, sc.out_h, sc.guide_mask);
        GpuSceneResources gpu;
        gpu.Allocate(sess.dx.dev.Get(), sc.alloc_rw, sc.alloc_rh, sc.out_w, sc.out_h, sc.guide_mask, true);

        // In v2_motion, when testing non-zero input Subrect.Base (P14), D3D12 resources are allocated at (alloc_rw, alloc_rh)
        // while CreateFeature receives the active render subrect dimensions (steady_rw, steady_rh) = 1280x720 so internal
        // network/grid dimensions match P00 1:1.
        const uint32_t create_rw = (is_v2 && (sc.v2_mod.in_base_x != 0u || sc.v2_mod.in_base_y != 0u)) ? sc.steady_rw : sc.alloc_rw;
        const uint32_t create_rh = (is_v2 && (sc.v2_mod.in_base_x != 0u || sc.v2_mod.in_base_y != 0u)) ? sc.steady_rh : sc.alloc_rh;
        const int32_t create_flags = is_v2 ? sc.v2_mod.create_flags : 0x4B;
        const uint32_t roughness_mode = (is_v2 && (sc.guide_mask & rr_ref::kGuideUnpackedRough) != 0u) ? 0u : 1u;

        NVSDK_NGX_Handle* handle = nullptr;
        const NVSDK_NGX_Result cr = CreateRrFeature(sess.ngx, sess.dx, sess.params,
                                                    create_rw, create_rh, sc.out_w, sc.out_h,
                                                    sc.perf_quality, sc.preset_hint, &handle,
                                                    create_flags, roughness_mode);
        NVSDK_NGX_Result last_er = cr;
        std::vector<uint16_t> f015_px;
        PixelStats pst {};

        if (NVSDK_NGX_SUCCEED(cr) && handle != nullptr)
        {
            sess.EnsureLoadedDlssdVerified(sc.preset_hint);
            for (uint32_t f = 0; f < sc.steady_frames; ++f)
            {
                rr_ref::FrameScalarParams fp {};
                rr_ref::PrepareFrame(sc.scene, sc.guide_mask, f, sc.steady_rw, sc.steady_rh, sc.out_w, sc.out_h,
                                     f == 0 ? 1 : 0, sc.preset_hint, sc.perf_quality, fp, cpu, suite, sc.v2_mod);
                gpu.UploadFrame(sess.dx, cpu, sc.steady_rw, sc.steady_rh, sc.guide_mask, fp.in_base_x, fp.in_base_y);
                BindEvalParams(sess.params, fp, sc.guide_mask, gpu);

                CheckHr(sess.dx.ca->Reset(), "Probe ca->Reset");
                CheckHr(sess.dx.cl->Reset(sess.dx.ca.Get(), nullptr), "Probe cl->Reset");
#ifdef RR_USE_OFFICIAL_NGX_SDK
                last_er = NVSDK_NGX_D3D12_EvaluateFeature(sess.dx.cl.Get(), handle, sess.params, nullptr);
#else
                last_er = sess.ngx.evaluate_feature(sess.dx.cl.Get(), handle, sess.params, nullptr);
#endif
                if (!NVSDK_NGX_SUCCEED(last_er))
                {
                    CheckHr(sess.dx.cl->Close(), "Close failed probe cl");
                    break;
                }
                sess.dx.ExecuteAndSync("Probe EvaluateFeature");
            }
            if (NVSDK_NGX_SUCCEED(last_er))
            {
                gpu.ReadbackOutputPixels(sess.dx, f015_px);
                pst = AnalyzeOutputRgba16f(f015_px);
                SaveRawRgba16f(pdir / "f015.rgba16f", f015_px);
                saved_f015[sc.id] = f015_px;
            }
#ifdef RR_USE_OFFICIAL_NGX_SDK
            NVSDK_NGX_D3D12_ReleaseFeature(handle);
#else
            sess.ngx.release_feature(handle);
#endif
        }

        auto findRefPx = [&](const std::string& ref_id, size_t expected_halfs) -> const std::vector<uint16_t>* {
            auto it = saved_f015.find(ref_id);
            if (it != saved_f015.end() && it->second.size() == expected_halfs)
                return &it->second;
            std::vector<uint16_t> disk_px;
            if (TryLoadRawRgba16f(out_dir / ref_id / "f015.rgba16f", expected_halfs, disk_px))
            {
                saved_f015[ref_id] = std::move(disk_px);
                return &saved_f015[ref_id];
            }
            return nullptr;
        };

        std::string status = "unsupported";
        ImageDiffStats vs_p00 {};
        ImageDiffStats vs_pair {};
        bool have_pair = false;

        if (NVSDK_NGX_SUCCEED(cr) && NVSDK_NGX_SUCCEED(last_er) && !f015_px.empty())
        {
            const auto* p00_px = findRefPx("P00_baseline_s1_g0", f015_px.size());
            if (sc.id == "P00_baseline_s1_g0" || p00_px == nullptr)
            {
                status = "baseline_ok";
            }
            else
            {
                vs_p00 = CompareRgba16f(*p00_px, f015_px);
                if (is_v2 && sc.expect_equiv_p00)
                {
                    if (vs_p00.diff_halfs == 0)
                        status = "supported_equiv_exact";
                    else if (vs_p00.psnr01_db >= 50.0)
                        status = "supported_equiv_close";
                    else
                        status = "supported_diverged";
                }
                else
                {
                    status = (vs_p00.diff_halfs > 0) ? "supported_active" : "supported_noop";
                }
            }

            if (!sc.pair_ref_id.empty())
            {
                if (const auto* pair_px = findRefPx(sc.pair_ref_id, f015_px.size()))
                {
                    vs_pair = CompareRgba16f(*pair_px, f015_px);
                    have_pair = true;
                }
            }
        }

        const char* probe_kind = (sc.id == "P00_baseline_s1_g0")
            ? "baseline"
            : (sc.expect_equiv_p00 ? "equivalence_to_p00" : "delta_from_ref");

        std::ostringstream entry;
        entry << "    {\n"
              << "      \"id\": \"" << sc.id << "\",\n"
              << "      \"probe_kind\": \"" << probe_kind << "\",\n"
              << "      \"preset_hint\": " << sc.preset_hint << ",\n"
              << "      \"create_flags\": " << create_flags << ",\n"
              << "      \"roughness_mode\": " << roughness_mode << ",\n"
              << "      \"guide_mask\": " << sc.guide_mask << ",\n"
              << "      \"alloc_render\": [" << sc.alloc_rw << ", " << sc.alloc_rh << "],\n"
              << "      \"render\": [" << sc.steady_rw << ", " << sc.steady_rh << "],\n"
              << "      \"in_subrect_base\": [" << (is_v2 ? sc.v2_mod.in_base_x : 0u) << ", " << (is_v2 ? sc.v2_mod.in_base_y : 0u) << "],\n"
              << "      \"create_result_hex\": \"" << HexU32(static_cast<uint32_t>(cr)) << "\",\n"
              << "      \"eval_result_hex\": \"" << HexU32(static_cast<uint32_t>(last_er)) << "\",\n"
              << "      \"status\": \"" << status << "\",\n"
              << "      \"f015_output_sha256\": \"" << pst.sha256 << "\",\n"
              << "      \"non_finite\": " << pst.non_finite << ",\n"
              << std::fixed << std::setprecision(6)
              << "      \"f015_mean_luma\": " << pst.mean_luma << ",\n"
              << "      \"diff_vs_p00_halfs\": " << vs_p00.diff_halfs << ",\n"
              << "      \"diff_vs_p00_max_ulp\": " << vs_p00.max_ulp << ",\n"
              << std::fixed << std::setprecision(3)
              << "      \"diff_vs_p00_psnr01_db\": " << vs_p00.psnr01_db;
        if (have_pair)
        {
            entry << ",\n"
                  << "      \"pair_ref_id\": \"" << sc.pair_ref_id << "\",\n"
                  << "      \"diff_vs_pair_halfs\": " << vs_pair.diff_halfs << ",\n"
                  << "      \"diff_vs_pair_max_ulp\": " << vs_pair.max_ulp << ",\n"
                  << "      \"diff_vs_pair_psnr01_db\": " << vs_pair.psnr01_db << "\n";
        }
        else
        {
            entry << "\n";
        }
        entry << "    }";

        {
            std::ofstream single_js(pdir / "probe_result.json", std::ios::binary);
            single_js << entry.str() << "\n";
        }
        js << entry.str() << (idx + 1 < probes.size() ? "," : "") << "\n";

        if (have_pair)
        {
            std::printf("[PROBE %s] status=%s | vs_P00=%zu halfs (%.2f dB) | vs_%s=%zu halfs (max_ulp=%u, %.2f dB)\n",
                        sc.id.c_str(), status.c_str(),
                        vs_p00.diff_halfs, vs_p00.psnr01_db,
                        sc.pair_ref_id.c_str(), vs_pair.diff_halfs, vs_pair.max_ulp, vs_pair.psnr01_db);
        }
        else
        {
            std::printf("[PROBE %s] status=%s | create=%s eval=%s | diff_vs_p00=%zu halfs (max_ulp=%u, PSNR01=%.2f dB)\n",
                        sc.id.c_str(), status.c_str(),
                        HexU32(static_cast<uint32_t>(cr)).c_str(),
                        HexU32(static_cast<uint32_t>(last_er)).c_str(),
                        vs_p00.diff_halfs, vs_p00.max_ulp, vs_p00.psnr01_db);
        }
    }
    js << "  ]\n}\n";
    Require(sess.dll_verified_after_create || sess.allow_dll_mismatch,
            "No probe succeeded CreateFeature to verify in-process nvngx_dlssd.dll!");
}

void RunProfileTargetMode(
    const std::string& target,
    uint32_t profile_frame,
    const fs::path& dlssd_path,
    const fs::path& loader_path,
    const fs::path& out_dir,
    bool allow_dll_mismatch,
    uint32_t vendor_id)
{
    uint32_t rw = 1280, rh = 720, ow = 1920, oh = 1080;
    int32_t pq = NVSDK_NGX_PerfQuality_Value_MaxQuality;
    if (target == "4k_qual") { rw = 2560; rh = 1440; ow = 3840; oh = 2160; pq = NVSDK_NGX_PerfQuality_Value_MaxQuality; }
    else if (target == "4k_perf") { rw = 1920; rh = 1080; ow = 3840; oh = 2160; pq = NVSDK_NGX_PerfQuality_Value_MaxPerf; }
    else if (target != "1080p_qual") throw std::runtime_error("Unknown --profile-target (use 1080p_qual, 4k_qual, or 4k_perf)");

    fs::create_directories(out_dir);
    NgxSession sess;
    sess.Start(dlssd_path, loader_path, out_dir, vendor_id, allow_dll_mismatch);

    rr_ref::CpuFrameBuffers cpu;
    cpu.Allocate(rw, rh, ow, oh, 0u);
    rr_ref::PopulateStaticS0(cpu);

    GpuSceneResources gpu;
    gpu.Allocate(sess.dx.dev.Get(), rw, rh, ow, oh, 0u, false);
    gpu.UploadFrame(sess.dx, cpu, rw, rh, 0u);

    NVSDK_NGX_Handle* handle = nullptr;
    const NVSDK_NGX_Result cr = CreateRrFeature(sess.ngx, sess.dx, sess.params, rw, rh, ow, oh, pq, 6u, &handle);
    Require(NVSDK_NGX_SUCCEED(cr) && handle != nullptr, "Profile CreateFeature failed: " + HexU32(static_cast<uint32_t>(cr)));
    sess.EnsureLoadedDlssdVerified(6u, "env_manifest_" + target + ".json");

    // Best-effort CUDA profiler range hooks if nvcuda.dll is loaded in process by NGX
    using PFN_cuProfiler = int(__stdcall*)();
    HMODULE nvcuda = GetModuleHandleW(L"nvcuda.dll");
    auto cu_start = nvcuda ? reinterpret_cast<PFN_cuProfiler>(reinterpret_cast<void*>(GetProcAddress(nvcuda, "cuProfilerStart"))) : nullptr;
    auto cu_stop  = nvcuda ? reinterpret_cast<PFN_cuProfiler>(reinterpret_cast<void*>(GetProcAddress(nvcuda, "cuProfilerStop")))  : nullptr;

    const uint32_t total_frames = profile_frame + 8u;
    for (uint32_t f = 0; f < total_frames; ++f)
    {
        const bool is_target = (f == profile_frame);
        if (is_target && cu_start) cu_start();

        rr_ref::FrameScalarParams fp {};
        rr_ref::PrepareFrame(rr_ref::SceneId::S0_BenchParity, 0u, f, rw, rh, ow, oh, f == 0 ? 1 : 0, 6u, pq, fp, cpu);
        BindEvalParams(sess.params, fp, 0u, gpu);

        CheckHr(sess.dx.ca->Reset(), "Profile ca->Reset");
        CheckHr(sess.dx.cl->Reset(sess.dx.ca.Get(), nullptr), "Profile cl->Reset");
        const char marker[] = "RR_Contrast_Profile_Target_Evaluate";
        if (is_target)
            sess.dx.cl->BeginEvent(1, marker, static_cast<UINT>(sizeof(marker)));
#ifdef RR_USE_OFFICIAL_NGX_SDK
        const NVSDK_NGX_Result er = NVSDK_NGX_D3D12_EvaluateFeature(sess.dx.cl.Get(), handle, sess.params, nullptr);
#else
        const NVSDK_NGX_Result er = sess.ngx.evaluate_feature(sess.dx.cl.Get(), handle, sess.params, nullptr);
#endif
        if (is_target)
            sess.dx.cl->EndEvent();
        Require(NVSDK_NGX_SUCCEED(er), "Profile EvaluateFeature failed: " + HexU32(static_cast<uint32_t>(er)));
        sess.dx.ExecuteAndSync("Profile EvaluateFeature");

        if (is_target && cu_stop) cu_stop();
    }

#ifdef RR_USE_OFFICIAL_NGX_SDK
    NVSDK_NGX_D3D12_ReleaseFeature(handle);
#else
    sess.ngx.release_feature(handle);
#endif

    std::ofstream js(out_dir / ("profile_" + target + ".json"), std::ios::binary);
    js << "{\n"
       << "  \"target\": \"" << target << "\",\n"
       << "  \"profile_frame\": " << profile_frame << ",\n"
       << "  \"total_frames\": " << total_frames << ",\n"
       << "  \"nvcuda_loaded_in_process\": " << (nvcuda ? "true" : "false") << ",\n"
       << "  \"cu_profiler_api_called\": " << ((cu_start && cu_stop) ? "true" : "false") << ",\n"
       << "  \"d3d12_pix_marker\": \"RR_Contrast_Profile_Target_Evaluate\"\n"
       << "}\n";
    std::printf("[PROFILE %s] Completed %u frames (target frame %u, nvcuda=%s, cuProfiler=%s)\n",
                target.c_str(), total_frames, profile_frame, nvcuda ? "loaded" : "not_loaded", (cu_start && cu_stop) ? "yes" : "no");
}

void PrintUsage()
{
    std::printf(
        "Usage:\n"
        "  ngx_rr_bench --self-test\n"
        "  ngx_rr_bench [--suite <v1|v2_motion>] --dump-fixture-hashes <cases.json>\n"
        "  ngx_rr_bench [--suite <v1|v2_motion>] --verify-fixtures <cases.json>\n"
        "  ngx_rr_bench [--suite <v1|v2_motion>] --smoke --dlssd <nvngx_dlssd.dll> --out <dir> [--ngx-loader <path>] [--process-exit-teardown] [--allow-dll-mismatch]\n"
        "  ngx_rr_bench [--suite <v1|v2_motion>] --pass <quality-core|timing-core|quality-ext|probes> --dlssd <nvngx_dlssd.dll> --out <dir> [--case <id>] [--process-exit-teardown] [--allow-dll-mismatch]\n"
        "  ngx_rr_bench --profile-target <1080p_qual|4k_qual|4k_perf> --dlssd <nvngx_dlssd.dll> --out <dir> [--profile-frame 32] [--allow-dll-mismatch]\n");
}

} // namespace

int main()
try
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wargv || argc <= 1)
    {
        PrintUsage();
        if (wargv) LocalFree(wargv);
        return 1;
    }

    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i)
        args.push_back(WideToUtf8(wargv[i]));
    LocalFree(wargv);

    bool do_self_test = false;
    bool do_smoke = false;
    bool allow_dll_mismatch = false;
    rr_ref::FixtureSuite suite = rr_ref::FixtureSuite::V1;
    bool suite_explicit = false;
    fs::path dump_hashes_path;
    fs::path verify_hashes_path;
    std::string pass_name;
    std::string filter_case_id;
    std::string profile_target;
    uint32_t profile_frame = 32u;
    uint32_t vendor_id = 0x10DEu;
    fs::path dlssd_path;
    fs::path loader_path;
    fs::path out_dir = "out";

    for (size_t i = 1; i < args.size(); ++i)
    {
        const std::string& a = args[i];
        auto next = [&]() -> const std::string& {
            Require(i + 1 < args.size(), "Missing argument value after " + a);
            return args[++i];
        };
        if (a == "--self-test") do_self_test = true;
        else if (a == "--suite") { suite = ParseFixtureSuite(next()); suite_explicit = true; }
        else if (a == "--dump-fixture-hashes") dump_hashes_path = Utf8ToPath(next());
        else if (a == "--verify-fixtures") verify_hashes_path = Utf8ToPath(next());
        else if (a == "--smoke") do_smoke = true;
        else if (a == "--process-exit-teardown") g_process_exit_teardown = true;
        else if (a == "--allow-dll-mismatch") allow_dll_mismatch = true;
        else if (a == "--pass") pass_name = next();
        else if (a == "--case") filter_case_id = next();
        else if (a == "--profile-target") profile_target = next();
        else if (a == "--profile-frame") profile_frame = static_cast<uint32_t>(std::stoul(next()));
        else if (a == "--vendor-id") vendor_id = static_cast<uint32_t>(std::stoul(next(), nullptr, 0));
        else if (a == "--dlssd") dlssd_path = Utf8ToPath(next());
        else if (a == "--ngx-loader") loader_path = Utf8ToPath(next());
        else if (a == "--out") out_dir = Utf8ToPath(next());
        else if (a == "--help" || a == "-h") { PrintUsage(); return 0; }
        else throw std::runtime_error("Unknown CLI option: " + a);
    }

    // Always run fast deterministic math, SHA-256 & v2_motion invariant self-tests at startup!
    rr_ref::RunSelfTests();
    if (do_self_test)
        std::printf("[PASS] Self-tests passed (FloatToHalfRNE, Isqrt64, SHA-256 FIPS-180-4, v2_motion scene invariants)\n");

    if (!dump_hashes_path.empty())
    {
        DumpFixtureHashesJson(dump_hashes_path, suite);
        std::printf("[PASS] Wrote %s suite & fixture input hashes to %s\n",
                    rr_ref::FixtureVersionForSuite(suite),
                    WideToUtf8(dump_hashes_path.wstring()).c_str());
    }

    if (!verify_hashes_path.empty())
        VerifyFixturesAgainstJson(verify_hashes_path, suite, suite_explicit);

    if (do_smoke)
    {
        Require(!dlssd_path.empty(), "--smoke requires --dlssd <path-to-nvngx_dlssd.dll>");
        RunSmokeMode(dlssd_path, loader_path, out_dir, allow_dll_mismatch, vendor_id, suite);
    }

    if (!pass_name.empty())
    {
        Require(!dlssd_path.empty(), "--pass requires --dlssd <path-to-nvngx_dlssd.dll>");
        PassGroup target_group = PassGroup::QualityCore;
        if (pass_name == "quality-core") target_group = PassGroup::QualityCore;
        else if (pass_name == "timing-core") target_group = PassGroup::TimingCore;
        else if (pass_name == "quality-ext") target_group = PassGroup::QualityExt;
        else if (pass_name == "probes") target_group = PassGroup::Probes;
        else throw std::runtime_error("Unknown --pass value: " + pass_name);

        const auto all_cases = BuildSuite(suite);
        std::vector<SuiteCase> selected;
        for (const auto& sc : all_cases)
        {
            if (sc.group == target_group && (filter_case_id.empty() || sc.id == filter_case_id))
                selected.push_back(sc);
        }
        Require(!selected.empty(), "No suite cases matched --suite " + std::string(rr_ref::FixtureVersionForSuite(suite)) +
                                   " --pass " + pass_name + (filter_case_id.empty() ? "" : (" --case " + filter_case_id)));

        NgxSession sess;
        sess.Start(dlssd_path, loader_path, out_dir, vendor_id, allow_dll_mismatch, suite);

        if (target_group == PassGroup::Probes)
        {
            RunProbesPass(sess, selected, out_dir, suite);
        }
        else if (target_group == PassGroup::TimingCore)
        {
            GpuPreSoak3Seconds(sess.dx);
            for (const auto& sc : selected)
            {
                for (uint32_t r = 1; r <= sc.rounds; ++r)
                    RunTimingCaseRound(sess, sc, r, out_dir / sc.id);
            }
        }
        else
        {
            for (const auto& sc : selected)
                RunQualityCase(sess, sc, out_dir / sc.id, suite);
        }
    }

    if (!profile_target.empty())
    {
        Require(!dlssd_path.empty(), "--profile-target requires --dlssd <path-to-nvngx_dlssd.dll>");
        RunProfileTargetMode(profile_target, profile_frame, dlssd_path, loader_path, out_dir, allow_dll_mismatch, vendor_id);
    }

    if (g_process_exit_teardown) {
        RuntimeTrace("[CLEANUP] all task outputs complete; terminating only current runner with exit=0");
        std::fflush(nullptr);
        TerminateProcess(GetCurrentProcess(), 0);
    }
    return 0;
}
catch (const std::exception& e)
{
    std::fprintf(stderr, "[ERROR] ngx_rr_bench: %s\n", e.what());
    if (g_process_exit_teardown) { std::fflush(nullptr); TerminateProcess(GetCurrentProcess(), 1); }
    return 1;
}
