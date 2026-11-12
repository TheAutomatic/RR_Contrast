// SPDX-License-Identifier: MIT
// Cross-compiler bit-deterministic synthetic scenes, IEEE-754 RNE FP16 conversion,
// and canonical frame-input SHA-256 hashing for DLSS-RR / OpenRR parity benchmarking.
//
// Design guarantees (fixture_version = "rr_contrast_v1"):
//   1. Zero transcendental libm calls (no sinf/cosf/sqrtf/ldexpf) in input generation.
//   2. All procedural coordinates, normals, lighting, motion vectors, and Halton jitters are
//      computed in exact 64-bit integer / Q20 fixed-point arithmetic (|q| <= 2^24) and converted
//      to IEEE-754 binary32 via exact integer cast multiplied by 2^-20 (0x35800000), eliminating
//      FMA contraction and compiler-specific rounding drift across MSVC, Clang, and GCC/MinGW.
//   3. FloatBitsToHalfRNE implements exact IEEE-754 Round-to-Nearest-Even with proper exponent
//      carry propagation (fixing the 1.9999f -> 0x3c00 carry-drop bug when '|' is used).
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace rr_ref
{

inline constexpr const char* kFixtureVersion = "rr_contrast_v1";
inline constexpr const char* kFixtureVersionV2Motion = "rr_contrast_v2_motion";

enum class FixtureSuite : uint32_t
{
    V1 = 0,
    V2_Motion = 1,
};

inline const char* FixtureVersionForSuite(FixtureSuite s) noexcept
{
    return (s == FixtureSuite::V2_Motion) ? kFixtureVersionV2Motion : kFixtureVersion;
}

// Exact power-of-two scale 2^-20 = 1.0f / 1048576.0f (IEEE-754 bits 0x35800000).
inline float Q20ToFloat(int32_t q20) noexcept
{
    const float inv2p20 = std::bit_cast<float>(0x35800000u);
    return static_cast<float>(q20) * inv2p20;
}

// Exact IEEE-754 binary32 -> binary16 Round-to-Nearest-Even (pure integer operations).
inline uint16_t FloatBitsToHalfRNE(uint32_t u) noexcept
{
    const uint32_t sign = (u >> 16) & 0x8000u;
    const uint32_t exp8 = (u >> 23) & 0xFFu;
    const uint32_t mant = u & 0x007FFFFFu;

    if (exp8 == 0xFFu)
    {
        if (mant == 0u)
            return static_cast<uint16_t>(sign | 0x7C00u);
        const uint16_t payload = static_cast<uint16_t>((mant >> 13) & 0x03FFu);
        return static_cast<uint16_t>(sign | 0x7E00u | (payload ? payload : 1u));
    }

    const int32_t exp16 = static_cast<int32_t>(exp8) - 127 + 15;
    if (exp16 >= 31)
        return static_cast<uint16_t>(sign | 0x7C00u);

    if (exp16 <= 0)
    {
        if (exp16 < -10)
            return static_cast<uint16_t>(sign);
        const uint32_t sig = mant | 0x00800000u;
        const uint32_t shift = static_cast<uint32_t>(14 - exp16); // in [14, 24]
        const uint32_t half_mant = sig >> shift;
        const uint32_t rem_mask = (1u << shift) - 1u;
        const uint32_t halfway = 1u << (shift - 1u);
        const uint32_t rem = sig & rem_mask;
        const uint32_t round_up = (rem > halfway || (rem == halfway && (half_mant & 1u) != 0u)) ? 1u : 0u;
        // Note: if half_mant == 0x3FF and round_up == 1 (when exp16 == 0), + promotes cleanly to 0x0400 (min normal).
        return static_cast<uint16_t>(sign | static_cast<uint16_t>(half_mant + round_up));
    }

    const uint32_t half_word = (static_cast<uint32_t>(exp16) << 10) | (mant >> 13);
    const uint32_t rem = mant & 0x1FFFu;
    const uint32_t round_up = (rem > 0x1000u || (rem == 0x1000u && (half_word & 1u) != 0u)) ? 1u : 0u;
    // '+' propagates mantissa overflow (0x3FF + 1) into exponent cleanly (e.g. 1.9999f -> 0x4000, or 65520.0f -> 0x7C00).
    return static_cast<uint16_t>(sign | static_cast<uint16_t>(half_word + round_up));
}

inline uint16_t FloatToHalfRNE(float v) noexcept
{
    return FloatBitsToHalfRNE(std::bit_cast<uint32_t>(v));
}

inline uint16_t Q20ToHalfRNE(int32_t q20) noexcept
{
    return FloatToHalfRNE(Q20ToFloat(q20));
}

inline bool IsFiniteHalf(uint16_t h) noexcept
{
    return ((h >> 10) & 0x1Fu) != 0x1Fu;
}

inline double HalfToDouble(uint16_t h) noexcept
{
    const uint32_t s = (h >> 15) & 1u;
    const uint32_t e = (h >> 10) & 0x1Fu;
    const uint32_t m = h & 0x03FFu;
    if (e == 0x1Fu)
        return std::bit_cast<double>(0x7FF8000000000000ull);
    uint64_t fbits = 0;
    if (e == 0u)
    {
        if (m == 0u)
            return s ? -0.0 : 0.0;
        // m * 2^-24
        const double val = static_cast<double>(m) * (1.0 / 16777216.0);
        return s ? -val : val;
    }
    const uint64_t exp_d = static_cast<uint64_t>(static_cast<int32_t>(e) - 15 + 1023);
    fbits = (static_cast<uint64_t>(s) << 63) | (exp_d << 52) | (static_cast<uint64_t>(m) << 42);
    return std::bit_cast<double>(fbits);
}

// Exact floor(sqrt(n)) for uint64_t using integer Newton / bit-by-bit refinement.
inline uint32_t Isqrt64(uint64_t n) noexcept
{
    uint64_t rem = n;
    uint64_t res = 0;
    uint64_t bit = 1ull << 62;
    while (bit > rem)
        bit >>= 2;
    while (bit != 0)
    {
        if (rem >= res + bit)
        {
            rem -= res + bit;
            res = (res >> 1) + bit;
        }
        else
        {
            res >>= 1;
        }
        bit >>= 2;
    }
    return static_cast<uint32_t>(res);
}

// Self-contained FIPS 180-4 SHA-256.
class Sha256
{
public:
    Sha256() noexcept { Reset(); }

    void Reset() noexcept
    {
        state_[0] = 0x6a09e667u;
        state_[1] = 0xbb67ae85u;
        state_[2] = 0x3c6ef372u;
        state_[3] = 0xa54ff53au;
        state_[4] = 0x510e527fu;
        state_[5] = 0x9b05688cu;
        state_[6] = 0x1f83d9abu;
        state_[7] = 0x5be0cd19u;
        total_bytes_ = 0;
        buf_len_ = 0;
    }

    void Update(const void* data, size_t len) noexcept
    {
        const auto* p = static_cast<const uint8_t*>(data);
        total_bytes_ += len;
        if (buf_len_ > 0)
        {
            const size_t take = std::min(len, size_t(64) - buf_len_);
            std::memcpy(buf_ + buf_len_, p, take);
            buf_len_ += take;
            p += take;
            len -= take;
            if (buf_len_ == 64)
            {
                Transform(buf_);
                buf_len_ = 0;
            }
        }
        while (len >= 64)
        {
            Transform(p);
            p += 64;
            len -= 64;
        }
        if (len > 0)
        {
            std::memcpy(buf_, p, len);
            buf_len_ = len;
        }
    }

    void UpdateU32LE(uint32_t v) noexcept
    {
        uint8_t b[4] = {
            static_cast<uint8_t>(v & 0xFFu),
            static_cast<uint8_t>((v >> 8) & 0xFFu),
            static_cast<uint8_t>((v >> 16) & 0xFFu),
            static_cast<uint8_t>((v >> 24) & 0xFFu),
        };
        Update(b, 4);
    }

    void UpdateF32LE(float f) noexcept
    {
        UpdateU32LE(std::bit_cast<uint32_t>(f));
    }

    std::array<uint8_t, 32> FinalBytes() noexcept
    {
        const uint64_t total_bits = total_bytes_ * 8ull;
        uint8_t pad = 0x80u;
        Update(&pad, 1);
        uint8_t zero = 0u;
        while (buf_len_ != 56)
            Update(&zero, 1);
        uint8_t len_be[8];
        for (int i = 0; i < 8; ++i)
            len_be[i] = static_cast<uint8_t>((total_bits >> ((7 - i) * 8)) & 0xFFu);
        Update(len_be, 8);

        std::array<uint8_t, 32> out {};
        for (int i = 0; i < 8; ++i)
        {
            out[4 * i + 0] = static_cast<uint8_t>((state_[i] >> 24) & 0xFFu);
            out[4 * i + 1] = static_cast<uint8_t>((state_[i] >> 16) & 0xFFu);
            out[4 * i + 2] = static_cast<uint8_t>((state_[i] >> 8) & 0xFFu);
            out[4 * i + 3] = static_cast<uint8_t>(state_[i] & 0xFFu);
        }
        return out;
    }

    std::string FinalHex()
    {
        const auto d = FinalBytes();
        static constexpr char kHex[] = "0123456789abcdef";
        std::string s(64, '\0');
        for (size_t i = 0; i < 32; ++i)
        {
            s[2 * i + 0] = kHex[(d[i] >> 4) & 0xFu];
            s[2 * i + 1] = kHex[d[i] & 0xFu];
        }
        return s;
    }

private:
    static uint32_t Ror32(uint32_t x, uint32_t n) noexcept { return (x >> n) | (x << (32u - n)); }

    void Transform(const uint8_t block[64]) noexcept
    {
        static constexpr uint32_t kK[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffaU, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
        };
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
        {
            w[i] = (static_cast<uint32_t>(block[4 * i + 0]) << 24) |
                   (static_cast<uint32_t>(block[4 * i + 1]) << 16) |
                   (static_cast<uint32_t>(block[4 * i + 2]) << 8) |
                   static_cast<uint32_t>(block[4 * i + 3]);
        }
        for (int i = 16; i < 64; ++i)
        {
            const uint32_t s0 = Ror32(w[i - 15], 7) ^ Ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = Ror32(w[i - 2], 17) ^ Ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (int i = 0; i < 64; ++i)
        {
            const uint32_t S1 = Ror32(e, 6) ^ Ror32(e, 11) ^ Ror32(e, 25);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t temp1 = h + S1 + ch + kK[i] + w[i];
            const uint32_t S0 = Ror32(a, 2) ^ Ror32(a, 13) ^ Ror32(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = S0 + maj;
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    uint32_t state_[8] {};
    uint64_t total_bytes_ = 0;
    size_t buf_len_ = 0;
    uint8_t buf_[64] {};
};

inline uint64_t Fnv1a64(const void* data, size_t len) noexcept
{
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < len; ++i)
    {
        h ^= static_cast<uint64_t>(p[i]);
        h *= 1099511628211ull;
    }
    return h;
}

inline std::string Sha256BytesHex(const void* data, size_t len)
{
    Sha256 s;
    s.Update(data, len);
    return s.FinalHex();
}

enum class SceneId : uint32_t
{
    S0_BenchParity = 0,
    S1_TemporalHalton = 1,
    S2_HdrStress = 2,
};

inline const char* SceneName(SceneId s) noexcept
{
    switch (s)
    {
    case SceneId::S0_BenchParity: return "s0_bench_parity";
    case SceneId::S1_TemporalHalton: return "s1_temporal_halton";
    case SceneId::S2_HdrStress: return "s2_hdr_stress";
    }
    return "unknown";
}

inline SceneId ParseSceneId(const std::string& s)
{
    if (s == "s0" || s == "s0_bench_parity") return SceneId::S0_BenchParity;
    if (s == "s1" || s == "s1_temporal_halton") return SceneId::S1_TemporalHalton;
    if (s == "s2" || s == "s2_hdr_stress") return SceneId::S2_HdrStress;
    throw std::runtime_error("Unknown scene id: " + s);
}

enum GuideMask : uint32_t
{
    kGuideNone          = 0u,
    kGuideDisocclusion  = 1u << 0, // DLSS.DisocclusionMask (R16F)
    kGuideTranspLayer   = 1u << 1, // DLSS.TransparencyLayer (RGBA16F) + DLSS.TransparencyLayerOpacity (RGBA16F)
    kGuideTranspMvecs   = 1u << 2, // DLSS.TransparencyLayerMvecs (RG16F, out_w x out_h)
    kGuideSssAndCbt     = 1u << 3, // DLSSD.ScreenSpaceSubsurfaceScatteringGuide (R16F) + DLSSD.ColorBeforeTransparency (RGBA16F)
    kGuideSpecularMvec  = 1u << 4, // GBuffer.SpecularMvec (RG16F)
    kGuideResponsivity  = 1u << 5, // DLSSD.ResponsivityMask (R16F, [-1, 1])
    kGuideDepthOfField  = 1u << 6, // DLSSD.DepthOfFieldGuide (R16F)
    kGuideOmitHitDist   = 1u << 7, // Omit DLSSD.SpecularHitDistance
    // v2_motion isolated & extended capability flags:
    kGuideSssOnly       = 1u << 8,  // DLSSD.ScreenSpaceSubsurfaceScatteringGuide (R16F) without CBT
    kGuideCbtOnly       = 1u << 9,  // DLSSD.ColorBeforeTransparency (RGBA16F, cbt != color) without SSS
    kGuideUnpackedRough = 1u << 10, // GBuffer.Roughness (R16F) with Roughness.Mode = Unpacked (0) & normals.w = 0
    kGuideExposureTex   = 1u << 11  // ExposureTexture (1x1 R32F) with AutoExposure cleared
};

struct V2FrameModifiers
{
    int32_t create_flags = 0x4B;      // Default: IsHDR(1) | MVLowRes(2) | DepthInverted(8) | AutoExposure(64)
    uint32_t in_base_x = 0;           // Input Subrect.Base.X
    uint32_t in_base_y = 0;           // Input Subrect.Base.Y
    float mv_scale_x = 1.0f;          // NVSDK_NGX_Parameter_MV_Scale_X
    float mv_scale_y = 1.0f;          // NVSDK_NGX_Parameter_MV_Scale_Y
    int64_t mv_tex_scale_num = 1;     // Integer pre-multiplier applied to generated pixel MVs (e.g. -2 with mv_scale = -0.5f)
    int32_t mv_jitter_delta_sign = 0;     // +1 adds +(prev_jitter - curr_jitter), -1 adds +(curr_jitter - prev_jitter)
    bool manual_exposure_step = false;    // Steps Pre.Exposure=2.0f, Exposure.Scale=0.5f (f<8) -> 2.0f (f>=8), ExposureTexture=null
    bool manual_exposure_tex_step = false;// Steps 1x1 R32F ExposureTexture=0.5f (f<8) -> 2.0f (f>=8), Pre.Exposure=2.0f, Exposure.Scale=1.0f
};

// Exact integer Halton radical inverse in Q20 centered around 0: range [-524288, +524288] => [-0.5, +0.5].
inline int32_t HaltonCenteredQ20(uint32_t index_1based, uint32_t base) noexcept
{
    uint64_t num = 0;
    uint64_t denom = 1;
    uint32_t i = index_1based;
    while (i > 0)
    {
        denom *= base;
        num = num * base + (i % base);
        i /= base;
    }
    const int64_t q20 = static_cast<int64_t>((num << 20) / denom) - (1ll << 19);
    return static_cast<int32_t>(q20);
}

inline uint32_t PcgHash(uint32_t v) noexcept
{
    const uint32_t state = v * 747796405u + 2891336453u;
    const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

struct FrameScalarParams
{
    uint32_t frame_index = 0;
    uint32_t rw = 0;
    uint32_t rh = 0;
    uint32_t out_w = 0;
    uint32_t out_h = 0;
    int32_t reset = 0;
    uint32_t preset_hint = 6;       // NVSDK_NGX_RayReconstruction_Hint_Render_Preset_F
    int32_t perf_quality = 2;       // NVSDK_NGX_PerfQuality_Value_MaxQuality
    int32_t create_flags = 0x4B;    // IsHDR(1) | MVLowRes(2) | DepthInverted(8) | AutoExposure(64)
    uint32_t in_base_x = 0;         // Subrect.Base.X (0 in v1; configurable in v2_motion)
    uint32_t in_base_y = 0;         // Subrect.Base.Y (0 in v1; configurable in v2_motion)
    uint32_t roughness_mode = 1;    // 1 = Packed in Normals.w (default), 0 = Unpacked GBuffer.Roughness
    float jitter_x = 0.0f;
    float jitter_y = 0.0f;
    float mv_scale_x = 1.0f;
    float mv_scale_y = 1.0f;
    float pre_exposure = 1.0f;
    float exposure_scale = 1.0f;
    float world_to_view[16] {};
    float view_to_clip[16] {};
};

inline void FillCanonicalMatrices(FrameScalarParams& p) noexcept
{
    std::memset(p.world_to_view, 0, sizeof(p.world_to_view));
    p.world_to_view[0] = 1.0f;
    p.world_to_view[5] = 1.0f;
    p.world_to_view[10] = 1.0f;
    p.world_to_view[15] = 1.0f;

    std::memset(p.view_to_clip, 0, sizeof(p.view_to_clip));
    p.view_to_clip[0] = 1.0f;
    // Exact binary32 near 16:9 vertical cotangent: 1820 / 1024 = 1.77734375f
    p.view_to_clip[5] = Q20ToFloat(1820 * 1024);
    p.view_to_clip[11] = -1.0f;
    // Exact IEEE-754 0.05f bit pattern (0x3D4CCCCD)
    p.view_to_clip[14] = std::bit_cast<float>(0x3D4CCCCDu);
}

struct CpuFrameBuffers
{
    uint32_t alloc_rw = 0;
    uint32_t alloc_rh = 0;
    uint32_t out_w = 0;
    uint32_t out_h = 0;

    // Core 7 inputs (allocated at alloc_rw x alloc_rh, row pitch = alloc_rw * bpp)
    std::vector<uint16_t> color_rgba16f;        // 4 halves/px (8 B)
    std::vector<uint8_t>  diffuse_rgba8;        // 4 bytes/px  (4 B)
    std::vector<uint8_t>  specular_rgba8;       // 4 bytes/px  (4 B)
    std::vector<uint16_t> normals_rgba16f;      // 4 halves/px (8 B), .w = linear roughness
    std::vector<float>    depth_r32f;           // 1 float/px  (4 B), inverted HW depth [0.02..0.95]
    std::vector<uint16_t> mvec_rg16f;           // 2 halves/px (4 B), render pixels current->previous
    std::vector<float>    hit_dist_r32f;        // 1 float/px  (4 B)

    // Optional guides (allocated when needed)
    std::vector<uint16_t> disocc_r16f;          // 1 half/px   (2 B), rw x rh
    std::vector<uint16_t> transp_rgba16f;       // 4 halves/px (8 B), rw x rh (premultiplied RGBA)
    std::vector<uint16_t> transp_op_rgba16f;    // 4 halves/px (8 B), rw x rh (RGB opacity)
    std::vector<uint16_t> transp_mvec_rg16f;    // 2 halves/px (4 B), out_w x out_h
    std::vector<uint16_t> sss_r16f;             // 1 half/px   (2 B), rw x rh
    std::vector<uint16_t> cbt_rgba16f;          // 4 halves/px (8 B), rw x rh
    std::vector<uint16_t> spec_mvec_rg16f;      // 2 halves/px (4 B), rw x rh
    std::vector<uint16_t> resp_r16f;            // 1 half/px   (2 B), rw x rh (R16F in [-1, 1])
    std::vector<uint16_t> dof_r16f;             // 1 half/px   (2 B), rw x rh
    std::vector<uint16_t> rough_r16f;           // 1 half/px   (2 B), rw x rh (unpacked GBuffer.Roughness)
    float                 exposure_tex_r32f = 1.0f; // 1x1 R32F ExposureTexture scalar value

    void Allocate(uint32_t max_rw, uint32_t max_rh, uint32_t target_ow, uint32_t target_oh, uint32_t all_guides_mask)
    {
        alloc_rw = max_rw;
        alloc_rh = max_rh;
        out_w = target_ow;
        out_h = target_oh;
        const size_t n = size_t(max_rw) * max_rh;
        color_rgba16f.assign(n * 4, 0);
        diffuse_rgba8.assign(n * 4, 0);
        specular_rgba8.assign(n * 4, 0);
        normals_rgba16f.assign(n * 4, 0);
        depth_r32f.assign(n, 0.0f);
        mvec_rg16f.assign(n * 2, 0);
        hit_dist_r32f.assign(n, 0.0f);

        if (all_guides_mask & kGuideDisocclusion) disocc_r16f.assign(n, 0);
        if (all_guides_mask & kGuideTranspLayer)
        {
            transp_rgba16f.assign(n * 4, 0);
            transp_op_rgba16f.assign(n * 4, 0);
        }
        if (all_guides_mask & kGuideTranspMvecs) transp_mvec_rg16f.assign(size_t(out_w) * out_h * 2, 0);
        if (all_guides_mask & (kGuideSssAndCbt | kGuideSssOnly)) sss_r16f.assign(n, 0);
        if (all_guides_mask & (kGuideSssAndCbt | kGuideCbtOnly)) cbt_rgba16f.assign(n * 4, 0);
        if (all_guides_mask & kGuideSpecularMvec) spec_mvec_rg16f.assign(n * 2, 0);
        if (all_guides_mask & kGuideResponsivity) resp_r16f.assign(n, 0);
        if (all_guides_mask & kGuideDepthOfField) dof_r16f.assign(n, 0);
        if (all_guides_mask & kGuideUnpackedRough) rough_r16f.assign(n, 0);
    }
};

// S0: Static pattern generated once at (max_rw x max_rh).
// DRS rule (locked in rr_contrast_v1): active subrect crops top-left (0, 0 .. rw, rh) of the max_rw x max_rh pattern.
inline void PopulateStaticS0(CpuFrameBuffers& b)
{
    const uint32_t w = b.alloc_rw;
    const uint32_t h = b.alloc_rh;
    const uint16_t h_zero = 0x0000u;
    const uint16_t h_one  = 0x3C00u;
    const uint16_t h_rgh  = Q20ToHalfRNE(314573);  // 0.30000019
    const uint16_t h_mvx  = Q20ToHalfRNE(262144);  // +0.25 exact
    const uint16_t h_mvy  = Q20ToHalfRNE(-157286); // -0.14999962

    for (uint32_t y = 0; y < h; ++y)
    {
        const int64_t v_q20 = (static_cast<int64_t>(y) << 20) / std::max(1u, h);
        for (uint32_t x = 0; x < w; ++x)
        {
            const size_t i = size_t(y) * w + x;
            const int64_t u_q20 = (static_cast<int64_t>(x) << 20) / std::max(1u, w);
            const bool alt = (((x / 16u) + (y / 16u)) & 1u) != 0u;
            const int64_t chk_q20 = alt ? 786432 : 1048576; // 0.75 or 1.0
            const uint32_t lcg = static_cast<uint32_t>((static_cast<uint64_t>(i) * 1103515245ull + 12345ull) >> 16) & 0xFFu;
            // noise = (lcg / 255) * 0.15 in Q20: (lcg * 157286) / 255
            const int64_t noise_q20 = (static_cast<int64_t>(lcg) * 157286ll) / 255ll;

            const int64_t cr_q20 = (((419430ll * chk_q20) >> 20) + noise_q20) * (524288ll + (u_q20 >> 1)) >> 20;
            const int64_t cg_q20 = (((367002ll * chk_q20) >> 20) + noise_q20) * (524288ll + (v_q20 >> 1)) >> 20;
            const int64_t cb_q20 = ((262144ll * chk_q20) >> 20) + noise_q20;

            b.color_rgba16f[4 * i + 0] = Q20ToHalfRNE(static_cast<int32_t>(cr_q20));
            b.color_rgba16f[4 * i + 1] = Q20ToHalfRNE(static_cast<int32_t>(cg_q20));
            b.color_rgba16f[4 * i + 2] = Q20ToHalfRNE(static_cast<int32_t>(cb_q20));
            b.color_rgba16f[4 * i + 3] = h_one;

            b.normals_rgba16f[4 * i + 0] = h_zero;
            b.normals_rgba16f[4 * i + 1] = h_zero;
            b.normals_rgba16f[4 * i + 2] = h_one;
            b.normals_rgba16f[4 * i + 3] = h_rgh;

            b.mvec_rg16f[2 * i + 0] = h_mvx;
            b.mvec_rg16f[2 * i + 1] = h_mvy;

            b.diffuse_rgba8[4 * i + 0] = static_cast<uint8_t>(alt ? 105u : 140u);
            b.diffuse_rgba8[4 * i + 1] = static_cast<uint8_t>(alt ? 82u : 110u);
            b.diffuse_rgba8[4 * i + 2] = static_cast<uint8_t>(alt ? 60u : 80u);
            b.diffuse_rgba8[4 * i + 3] = 255u;

            b.specular_rgba8[4 * i + 0] = 16u;
            b.specular_rgba8[4 * i + 1] = 16u;
            b.specular_rgba8[4 * i + 2] = 16u;
            b.specular_rgba8[4 * i + 3] = 255u;

            // depth = 0.02 + 0.80 * u, hit = 2.5 + 5.0 * v in exact Q20
            b.depth_r32f[i] = Q20ToFloat(static_cast<int32_t>(20972ll + ((838861ll * u_q20) >> 20)));
            b.hit_dist_r32f[i] = Q20ToFloat(static_cast<int32_t>(2621440ll + ((5242880ll * v_q20) >> 20)));
        }
    }
}

// Populate scalar params and active subrect region for frame_index.
// For S0_BenchParity, static textures must already be populated via PopulateStaticS0(b).
// When suite == FixtureSuite::V1 (default), behavior and SHA-256 hashes are 100% identical to Round-1 rr_contrast_v1.
inline void PrepareFrame(
    SceneId scene,
    uint32_t guide_mask,
    uint32_t frame_idx,
    uint32_t rw,
    uint32_t rh,
    uint32_t out_w,
    uint32_t out_h,
    int32_t reset,
    uint32_t preset_hint,
    int32_t perf_quality,
    FrameScalarParams& params,
    CpuFrameBuffers& b,
    FixtureSuite suite = FixtureSuite::V1,
    const V2FrameModifiers& v2_mod = {})
{
    const bool is_v2 = (suite == FixtureSuite::V2_Motion);

    params = {};
    params.frame_index = frame_idx;
    params.rw = rw;
    params.rh = rh;
    params.out_w = out_w;
    params.out_h = out_h;
    params.reset = reset;
    params.preset_hint = preset_hint;
    params.perf_quality = perf_quality;
    params.create_flags = is_v2 ? v2_mod.create_flags : 0x4B;
    params.in_base_x = is_v2 ? v2_mod.in_base_x : 0u;
    params.in_base_y = is_v2 ? v2_mod.in_base_y : 0u;
    params.roughness_mode = (is_v2 && (guide_mask & kGuideUnpackedRough) != 0u) ? 0u : 1u;
    params.mv_scale_x = is_v2 ? v2_mod.mv_scale_x : 1.0f;
    params.mv_scale_y = is_v2 ? v2_mod.mv_scale_y : 1.0f;
    params.pre_exposure = 1.0f;
    params.exposure_scale = 1.0f;
    b.exposure_tex_r32f = 1.0f;
    if (is_v2 && v2_mod.manual_exposure_step)
    {
        params.pre_exposure = 2.0f;
        params.exposure_scale = (frame_idx < 8u) ? 0.5f : 2.0f;
    }
    if (is_v2 && v2_mod.manual_exposure_tex_step)
    {
        params.pre_exposure = 2.0f;
        params.exposure_scale = 1.0f;
        b.exposure_tex_r32f = (frame_idx < 8u) ? 0.5f : 2.0f;
    }
    FillCanonicalMatrices(params);

    if (scene == SceneId::S0_BenchParity)
    {
        params.jitter_x = Q20ToFloat(131072);  // +0.125f exact
        params.jitter_y = Q20ToFloat(-262144); // -0.25f exact
        return;
    }

    const uint32_t phase = (frame_idx & 15u) + 1u;
    const int32_t jx_q20 = HaltonCenteredQ20(phase, 2u);
    const int32_t jy_q20 = HaltonCenteredQ20(phase, 3u);
    const uint32_t prev_phase = (frame_idx == 0u) ? phase : (((frame_idx - 1u) & 15u) + 1u);
    const int32_t prev_jx_q20 = HaltonCenteredQ20(prev_phase, 2u);
    const int32_t prev_jy_q20 = HaltonCenteredQ20(prev_phase, 3u);
    params.jitter_x = Q20ToFloat(jx_q20);
    params.jitter_y = Q20ToFloat(jy_q20);

    const bool is_hdr_stress = (scene == SceneId::S2_HdrStress);
    const uint32_t pitch_px = b.alloc_rw;
    const int64_t t64 = static_cast<int64_t>(frame_idx);

    if (is_v2)
    {
        // Fix #2: Per-frame Q20 camera translation in row-major world_to_view[12..14] (v_row * M).
        // Activates reflection_mv(X, nrm) across curved sphere normals + hit_dist in enc0_attr,
        // separating P00 (reflection_mv), P05 (SpecularMvec), and P10 (OPTF_NO_HIT mv_c fallback).
        const int32_t cam_x_q20 = static_cast<int32_t>((t64 % 96ll) * 512ll);
        const int32_t cam_y_q20 = static_cast<int32_t>(-((t64 % 96ll) * 256ll));
        const int32_t cam_z_q20 = static_cast<int32_t>((t64 % 96ll) * 128ll);
        params.world_to_view[12] = -Q20ToFloat(cam_x_q20);
        params.world_to_view[13] = -Q20ToFloat(cam_y_q20);
        params.world_to_view[14] = -Q20ToFloat(cam_z_q20);

        // If testing non-zero input Subrect.Base (P14), fill the outer border with a bright magenta sentinel
        // so ignoring Subrect.Base.X/Y produces an unmistakable >400k half-float difference vs P00.
        if (params.in_base_x != 0u || params.in_base_y != 0u)
        {
            const size_t n_alloc = size_t(b.alloc_rw) * b.alloc_rh;
            const uint16_t h_four = 0x4400u; // 4.0f
            const uint16_t h_pos16 = 0x4C00u; // 16.0f
            const uint16_t h_neg16 = 0xCC00u; // -16.0f
            for (size_t i = 0; i < n_alloc; ++i)
            {
                b.color_rgba16f[4 * i + 0] = h_four;
                b.color_rgba16f[4 * i + 1] = 0x0000u;
                b.color_rgba16f[4 * i + 2] = h_four;
                b.color_rgba16f[4 * i + 3] = 0x3C00u;
                b.diffuse_rgba8[4 * i + 0] = 255u;
                b.diffuse_rgba8[4 * i + 1] = 0u;
                b.diffuse_rgba8[4 * i + 2] = 255u;
                b.diffuse_rgba8[4 * i + 3] = 255u;
                b.specular_rgba8[4 * i + 0] = 255u;
                b.specular_rgba8[4 * i + 1] = 0u;
                b.specular_rgba8[4 * i + 2] = 255u;
                b.specular_rgba8[4 * i + 3] = 255u;
                b.normals_rgba16f[4 * i + 0] = 0x0000u;
                b.normals_rgba16f[4 * i + 1] = 0x0000u;
                b.normals_rgba16f[4 * i + 2] = 0x3C00u;
                b.normals_rgba16f[4 * i + 3] = 0x3800u;
                b.depth_r32f[i] = Q20ToFloat(943718); // 0.90
                b.mvec_rg16f[2 * i + 0] = h_pos16;
                b.mvec_rg16f[2 * i + 1] = h_neg16;
                b.hit_dist_r32f[i] = Q20ToFloat(524288);
            }
        }
    }

    // Normalized per-frame UV velocities in Q20 (so physical motion scales cleanly with rw/rh):
    // Background pans slowly (+1.5 px/frame at 1280x720 -> du_bg_q20 = 1229, dv_bg_q20 = -728)
    // Foreground sphere/card moves faster (-3.25 px/frame X, +1.25 px/frame Y at 1280x720 -> du_fg_q20 = -2662, dv_fg_q20 = 1820)
    const int64_t du_bg_q20 = 1229;
    const int64_t dv_bg_q20 = -728;
    const int64_t du_fg_q20 = -2662;
    const int64_t dv_fg_q20 = 1820;
    // Independent transparency-layer UV velocity in v2_motion (-4.0 px/frame X, +0.75 px/frame Y at 1280x720)
    const int64_t du_tl_q20 = -3277;
    const int64_t dv_tl_q20 = 1092;

    // Sphere center in normalized UV Q20:
    // In v2_motion, use smooth wrap-free motion across frames [-1..95] so frame 0 has identical velocity to frames 1..79.
    const int64_t sph_cx_q20 = is_v2
        ? (680000ll + ((t64 % 96ll) * du_fg_q20))
        : (655360ll + ((t64 * du_fg_q20) % 262144ll));
    const int64_t sph_cy_q20 = is_v2
        ? (475136ll + ((t64 % 96ll) * dv_fg_q20))
        : (491520ll + ((t64 * dv_fg_q20) % 196608ll));
    const int64_t prev_sph_cx_q20 = is_v2
        ? (sph_cx_q20 - du_fg_q20)
        : (655360ll + (((t64 > 0 ? t64 - 1 : 0) * du_fg_q20) % 262144ll));
    const int64_t prev_sph_cy_q20 = is_v2
        ? (sph_cy_q20 - dv_fg_q20)
        : (491520ll + (((t64 > 0 ? t64 - 1 : 0) * dv_fg_q20) % 196608ll));
    const int64_t sph_r_q20 = 229376ll; // radius = 0.21875 in vertical-normalized units
    const int64_t sph_r2_q40 = sph_r_q20 * sph_r_q20;
    const int64_t aspect_q20 = (static_cast<int64_t>(rw) << 20) / std::max(1u, rh);

    // Fix #1: du_bg_q20 * rw is ALREADY Q20 pixels (Q20 * int = Q20).
    // v1 had an extra '>> 20' that shrunk S1/S2 MVs by 2^-20 (~1e-6 px); v2_motion preserves true pixel-scale Q20 MVs.
    const int64_t mv_mul = is_v2 ? v2_mod.mv_tex_scale_num : 1ll;
    const int64_t jit_dx_q20 = is_v2 ? (static_cast<int64_t>(v2_mod.mv_jitter_delta_sign) * static_cast<int64_t>(prev_jx_q20 - jx_q20)) : 0ll;
    const int64_t jit_dy_q20 = is_v2 ? (static_cast<int64_t>(v2_mod.mv_jitter_delta_sign) * static_cast<int64_t>(prev_jy_q20 - jy_q20)) : 0ll;

    const int32_t mv_bg_x_q20 = is_v2
        ? static_cast<int32_t>((-(du_bg_q20 * static_cast<int64_t>(rw)) + jit_dx_q20) * mv_mul)
        : static_cast<int32_t>(-((du_bg_q20 * rw) >> 20));
    const int32_t mv_bg_y_q20 = is_v2
        ? static_cast<int32_t>((-(dv_bg_q20 * static_cast<int64_t>(rh)) + jit_dy_q20) * mv_mul)
        : static_cast<int32_t>(-((dv_bg_q20 * rh) >> 20));
    const int32_t mv_fg_x_q20 = is_v2
        ? static_cast<int32_t>((-((sph_cx_q20 - prev_sph_cx_q20) * static_cast<int64_t>(rw)) + jit_dx_q20) * mv_mul)
        : static_cast<int32_t>(-(((sph_cx_q20 - prev_sph_cx_q20) * rw) >> 20));
    const int32_t mv_fg_y_q20 = is_v2
        ? static_cast<int32_t>((-((sph_cy_q20 - prev_sph_cy_q20) * static_cast<int64_t>(rh)) + jit_dy_q20) * mv_mul)
        : static_cast<int32_t>(-(((sph_cy_q20 - prev_sph_cy_q20) * rh) >> 20));
    const int32_t spec_sphere_mvx_q20 = is_v2
        ? static_cast<int32_t>((((-((sph_cx_q20 - prev_sph_cx_q20) * static_cast<int64_t>(rw))) / 2ll) + jit_dx_q20) * mv_mul)
        : (mv_fg_x_q20 / 2);
    const int32_t spec_sphere_mvy_q20 = is_v2
        ? static_cast<int32_t>((((-((sph_cy_q20 - prev_sph_cy_q20) * static_cast<int64_t>(rh))) / 2ll) + jit_dy_q20) * mv_mul)
        : (mv_fg_y_q20 / 2);

    const bool has_sss = (guide_mask & (kGuideSssAndCbt | kGuideSssOnly)) != 0u;
    const bool has_cbt = (guide_mask & (kGuideSssAndCbt | kGuideCbtOnly)) != 0u;
    const bool unpacked_rough = is_v2 && ((guide_mask & kGuideUnpackedRough) != 0u);

    for (uint32_t y = 0; y < rh; ++y)
    {
        // Unjittered pixel center oy = y + 0.5 - jy (matches OpenRR enc0_attr.comp)
        const int64_t oy_q20 = (static_cast<int64_t>(y) << 20) + (1ll << 19) - jy_q20;
        const int64_t uv_y_q20 = oy_q20 / std::max(1u, rh);

        for (uint32_t x = 0; x < rw; ++x)
        {
            const size_t idx = size_t(y + params.in_base_y) * pitch_px + (x + params.in_base_x);
            const int64_t ox_q20 = (static_cast<int64_t>(x) << 20) + (1ll << 19) - jx_q20;
            const int64_t uv_x_q20 = ox_q20 / std::max(1u, rw);

            // Check glossy sphere intersection (aspect-corrected)
            const int64_t dx_q20 = ((uv_x_q20 - sph_cx_q20) * aspect_q20) >> 20;
            const int64_t dy_q20 = uv_y_q20 - sph_cy_q20;
            const int64_t d2_q40 = dx_q20 * dx_q20 + dy_q20 * dy_q20;
            const bool hit_sphere = (d2_q40 < sph_r2_q40);

            // Check previous frame sphere footprint at reprojected background coord to flag disocclusion
            const int64_t prev_bg_uvx_q20 = uv_x_q20 - du_bg_q20;
            const int64_t prev_bg_uvy_q20 = uv_y_q20 - dv_bg_q20;
            const int64_t pdx_q20 = ((prev_bg_uvx_q20 - prev_sph_cx_q20) * aspect_q20) >> 20;
            const int64_t pdy_q20 = prev_bg_uvy_q20 - prev_sph_cy_q20;
            const bool was_occluded = (!hit_sphere) && ((pdx_q20 * pdx_q20 + pdy_q20 * pdy_q20) < sph_r2_q40);

            // Deterministic 1-spp spatio-temporal zero-mean Monte Carlo noise in [-1, +1] Q20
            const uint32_t rng = PcgHash(x * 1973u + y * 9277u + frame_idx * 26699u + (is_hdr_stress ? 0x9E3779B9u : 0x13579BDFu));
            const int64_t xi_q20 = static_cast<int64_t>(rng & 0x1FFFFFu) - 1048576ll; // [-1.0, +1.0) in Q20

            int32_t nx_q20 = 0, ny_q20 = 0, nz_q20 = 1048576, rough_q20 = 367002;
            int32_t depth_q20 = 125829; // ~0.12
            int32_t hit_q20 = 3145728;  // ~3.0
            int32_t mvx_q20 = mv_bg_x_q20, mvy_q20 = mv_bg_y_q20;
            uint8_t diff_r = 120, diff_g = 115, diff_b = 110;
            uint8_t spec_r = 20, spec_g = 20, spec_b = 20;
            int64_t rad_r_q20 = 0, rad_g_q20 = 0, rad_b_q20 = 0;

            if (hit_sphere)
            {
                const uint64_t z2_q40 = static_cast<uint64_t>(sph_r2_q40 - d2_q40);
                const int64_t dz_q20 = static_cast<int64_t>(Isqrt64(z2_q40));
                nx_q20 = static_cast<int32_t>((dx_q20 << 20) / sph_r_q20);
                ny_q20 = static_cast<int32_t>((-dy_q20 << 20) / sph_r_q20);
                nz_q20 = static_cast<int32_t>((dz_q20 << 20) / sph_r_q20);
                // Smoothly varying roughness across sphere [0.06 .. 0.65]
                rough_q20 = static_cast<int32_t>(62915ll + (((dx_q20 + sph_r_q20) * 300000ll) / (2ll * sph_r_q20)));
                depth_q20 = static_cast<int32_t>(471859ll + ((dz_q20 * 262144ll) >> 20)); // closer = larger inverted Z (~0.45..0.70)
                hit_q20 = static_cast<int32_t>(629145ll + ((dz_q20 * 1572864ll) >> 20));  // 0.6 .. 2.1
                mvx_q20 = mv_fg_x_q20;
                mvy_q20 = mv_fg_y_q20;

                diff_r = 185; diff_g = 112; diff_b = 76;
                spec_r = 56;  spec_g = 54;  spec_b = 52;

                // Directional key light L = (0.5, 0.5, 0.707) -> dot in Q20
                const int64_t ndotl_q20 = std::max<int64_t>(65536ll, (int64_t(nx_q20) * 524288ll + int64_t(ny_q20) * 524288ll + int64_t(nz_q20) * 741455ll) >> 20);
                const int64_t spec_lobe_q20 = (ndotl_q20 * ndotl_q20) >> 20;
                const int64_t spec_peak_q20 = (spec_lobe_q20 * spec_lobe_q20) >> 20;
                const int64_t hdr_boost = is_hdr_stress ? 14ll : 2ll; // Up to ~16.0 linear HDR on S2 specular highlight!

                // Unbiased-like 1-spp MC noisy estimate around true shading (converges when temporally accumulated)
                const int64_t mc_scale_q20 = 1048576ll + ((xi_q20 * 393216ll) >> 20); // 1.0 +/- 0.375
                rad_r_q20 = (((786432ll * ndotl_q20) >> 20) + hdr_boost * spec_peak_q20) * mc_scale_q20 >> 20;
                rad_g_q20 = (((480000ll * ndotl_q20) >> 20) + hdr_boost * spec_peak_q20) * mc_scale_q20 >> 20;
                rad_b_q20 = (((330000ll * ndotl_q20) >> 20) + hdr_boost * spec_peak_q20) * mc_scale_q20 >> 20;
            }
            else
            {
                // World-locked background UV (subtract frame * velocity so patterns match MV exactly!)
                const int64_t wx_q20 = uv_x_q20 - t64 * du_bg_q20;
                const int64_t wy_q20 = uv_y_q20 - t64 * dv_bg_q20;

                // Slanted subpixel high-frequency bar test + checker floor
                const int64_t diag_q20 = wx_q20 * 24ll + wy_q20 * 7ll;
                const bool thin_bar = ((diag_q20 >> 17) & 7ll) == 0ll;
                const bool floor_region = (uv_y_q20 > 680000ll);
                const bool tile_alt = (((wx_q20 >> 16) + (wy_q20 >> 16)) & 1ll) != 0ll;

                rough_q20 = floor_region ? (is_hdr_stress ? 41943 : 157286) : 629145; // 0.04 mirror floor on S2!
                depth_q20 = floor_region
                    ? static_cast<int32_t>(104858ll + (((uv_y_q20 - 680000ll) * 524288ll) >> 20))
                    : 83886;
                hit_q20 = floor_region ? 1258291 : 6291456;

                if (thin_bar)
                {
                    diff_r = 220; diff_g = 210; diff_b = 180;
                    spec_r = 40;  spec_g = 40;  spec_b = 40;
                }
                else if (tile_alt)
                {
                    diff_r = 72; diff_g = 88; diff_b = 108;
                }
                else
                {
                    diff_r = 132; diff_g = 140; diff_b = 152;
                }

                const int64_t base_luma_q20 = is_hdr_stress
                    ? (thin_bar ? 12582912ll : (tile_alt ? 18000ll : 280000ll)) // 12.0 HDR emissive bars vs 0.017 dark shadow!
                    : (thin_bar ? 1887437ll : (tile_alt ? 262144ll : 589824ll));
                const int64_t mc_scale_q20 = 1048576ll + ((xi_q20 * 327680ll) >> 20);
                rad_r_q20 = (((base_luma_q20 * diff_r) / 160ll) * mc_scale_q20) >> 20;
                rad_g_q20 = (((base_luma_q20 * diff_g) / 160ll) * mc_scale_q20) >> 20;
                rad_b_q20 = (((base_luma_q20 * diff_b) / 160ll) * mc_scale_q20) >> 20;
            }

            rad_r_q20 = std::clamp<int64_t>(rad_r_q20, 0ll, 16777216ll);
            rad_g_q20 = std::clamp<int64_t>(rad_g_q20, 0ll, 16777216ll);
            rad_b_q20 = std::clamp<int64_t>(rad_b_q20, 0ll, 16777216ll);

            b.diffuse_rgba8[4 * idx + 0] = diff_r;
            b.diffuse_rgba8[4 * idx + 1] = diff_g;
            b.diffuse_rgba8[4 * idx + 2] = diff_b;
            b.diffuse_rgba8[4 * idx + 3] = 255u;

            b.specular_rgba8[4 * idx + 0] = spec_r;
            b.specular_rgba8[4 * idx + 1] = spec_g;
            b.specular_rgba8[4 * idx + 2] = spec_b;
            b.specular_rgba8[4 * idx + 3] = 255u;

            b.normals_rgba16f[4 * idx + 0] = Q20ToHalfRNE(nx_q20);
            b.normals_rgba16f[4 * idx + 1] = Q20ToHalfRNE(ny_q20);
            b.normals_rgba16f[4 * idx + 2] = Q20ToHalfRNE(nz_q20);
            b.normals_rgba16f[4 * idx + 3] = unpacked_rough ? 0x0000u : Q20ToHalfRNE(rough_q20);
            if (unpacked_rough)
                b.rough_r16f[idx] = Q20ToHalfRNE(rough_q20);

            b.depth_r32f[idx] = Q20ToFloat(depth_q20);
            b.mvec_rg16f[2 * idx + 0] = Q20ToHalfRNE(mvx_q20);
            b.mvec_rg16f[2 * idx + 1] = Q20ToHalfRNE(mvy_q20);
            b.hit_dist_r32f[idx] = Q20ToFloat(hit_q20);

            if (has_cbt)
            {
                // Fix #4: In v1, cbt == color made (col - cbt) == 0 everywhere (leaving enc0_attr attr[30] = 0).
                // In v2_motion, cbt represents pre-veil opaque radiance (5/8 of color above the floor) while color
                // remains bit-identical to P00, cleanly isolating DLSSD.ColorBeforeTransparency sensitivity.
                const bool veil_region = is_v2 && (uv_y_q20 < 680000ll);
                const int32_t cbt_r = static_cast<int32_t>(veil_region ? ((rad_r_q20 * 5ll) >> 3) : rad_r_q20);
                const int32_t cbt_g = static_cast<int32_t>(veil_region ? ((rad_g_q20 * 5ll) >> 3) : rad_g_q20);
                const int32_t cbt_b = static_cast<int32_t>(veil_region ? ((rad_b_q20 * 5ll) >> 3) : rad_b_q20);
                b.cbt_rgba16f[4 * idx + 0] = Q20ToHalfRNE(cbt_r);
                b.cbt_rgba16f[4 * idx + 1] = Q20ToHalfRNE(cbt_g);
                b.cbt_rgba16f[4 * idx + 2] = Q20ToHalfRNE(cbt_b);
                b.cbt_rgba16f[4 * idx + 3] = 0x3C00u;
            }
            if (has_sss)
            {
                b.sss_r16f[idx] = Q20ToHalfRNE(hit_sphere ? 367002 : 0);
            }

            if (guide_mask & kGuideTranspLayer)
            {
                const bool glass_band = (uv_x_q20 > 262144ll && uv_x_q20 < 458752ll && uv_y_q20 > 262144ll && uv_y_q20 < 786432ll);
                if (!is_v2)
                {
                    const int32_t alpha_q20 = glass_band ? 262144 : 0; // 0.25 alpha glass panel
                    const int32_t tl_r = glass_band ? 65536 : 0;
                    const int32_t tl_g = glass_band ? 131072 : 0;
                    const int32_t tl_b = glass_band ? 196608 : 0;
                    b.transp_rgba16f[4 * idx + 0] = Q20ToHalfRNE(tl_r);
                    b.transp_rgba16f[4 * idx + 1] = Q20ToHalfRNE(tl_g);
                    b.transp_rgba16f[4 * idx + 2] = Q20ToHalfRNE(tl_b);
                    b.transp_rgba16f[4 * idx + 3] = Q20ToHalfRNE(alpha_q20);

                    const uint16_t op_h = Q20ToHalfRNE(1048576 - alpha_q20);
                    b.transp_op_rgba16f[4 * idx + 0] = op_h;
                    b.transp_op_rgba16f[4 * idx + 1] = op_h;
                    b.transp_op_rgba16f[4 * idx + 2] = op_h;
                    b.transp_op_rgba16f[4 * idx + 3] = 0x3C00u;
                }
                else if (glass_band)
                {
                    // Fix #3: Spatially textured moving etched-glass ramp (period ~16 render px) + proper Opacity semantics.
                    // Ensures 3x3 neighborhood [lo4, hi4] / [olo, ohi] is non-degenerate (lo4 < hi4) in downsample_tl,
                    // so wrong MV (P02) vs exact tlmv (P03) produces distinct temporal accumulation.
                    const int64_t gx_q20 = uv_x_q20 - t64 * du_tl_q20;
                    const int64_t gy_q20 = uv_y_q20 - t64 * dv_tl_q20;
                    const int64_t phase_mod = (((gx_q20 + (gy_q20 >> 1)) % 26214ll) + 26214ll) % 26214ll;
                    const int64_t tri_q20 = (phase_mod < 13107ll ? phase_mod : (26214ll - phase_mod)) * 80ll; // [0, 1048560]

                    const int32_t alpha_q20 = static_cast<int32_t>(131072ll + ((tri_q20 * 327680ll) >> 20)); // [0.125 .. 0.4375]
                    const int32_t tl_r = static_cast<int32_t>(32768ll + ((tri_q20 * 262144ll) >> 20));
                    const int32_t tl_g = static_cast<int32_t>(65536ll + (((1048576ll - tri_q20) * 196608ll) >> 20));
                    const int32_t tl_b = static_cast<int32_t>(98304ll + ((tri_q20 * 327680ll) >> 20));
                    b.transp_rgba16f[4 * idx + 0] = Q20ToHalfRNE(tl_r);
                    b.transp_rgba16f[4 * idx + 1] = Q20ToHalfRNE(tl_g);
                    b.transp_rgba16f[4 * idx + 2] = Q20ToHalfRNE(tl_b);
                    b.transp_rgba16f[4 * idx + 3] = Q20ToHalfRNE(alpha_q20);

                    // Per-channel colored opacity (0 = transparent pass-through outside glass; ~alpha inside glass)
                    b.transp_op_rgba16f[4 * idx + 0] = Q20ToHalfRNE(static_cast<int32_t>((int64_t(alpha_q20) * 891289ll) >> 20));
                    b.transp_op_rgba16f[4 * idx + 1] = Q20ToHalfRNE(alpha_q20);
                    b.transp_op_rgba16f[4 * idx + 2] = Q20ToHalfRNE(static_cast<int32_t>((int64_t(alpha_q20) * 1153434ll) >> 20));
                    b.transp_op_rgba16f[4 * idx + 3] = 0x3C00u;
                }
                else
                {
                    b.transp_rgba16f[4 * idx + 0] = 0x0000u;
                    b.transp_rgba16f[4 * idx + 1] = 0x0000u;
                    b.transp_rgba16f[4 * idx + 2] = 0x0000u;
                    b.transp_rgba16f[4 * idx + 3] = 0x0000u;
                    b.transp_op_rgba16f[4 * idx + 0] = 0x0000u;
                    b.transp_op_rgba16f[4 * idx + 1] = 0x0000u;
                    b.transp_op_rgba16f[4 * idx + 2] = 0x0000u;
                    b.transp_op_rgba16f[4 * idx + 3] = 0x3C00u;
                }
            }

            b.color_rgba16f[4 * idx + 0] = Q20ToHalfRNE(static_cast<int32_t>(rad_r_q20));
            b.color_rgba16f[4 * idx + 1] = Q20ToHalfRNE(static_cast<int32_t>(rad_g_q20));
            b.color_rgba16f[4 * idx + 2] = Q20ToHalfRNE(static_cast<int32_t>(rad_b_q20));
            b.color_rgba16f[4 * idx + 3] = 0x3C00u;

            if (guide_mask & kGuideDisocclusion)
                b.disocc_r16f[idx] = was_occluded ? 0x3C00u : 0x0000u;

            if (guide_mask & kGuideSpecularMvec)
            {
                // Reflected motion differs from surface motion on floor/sphere
                b.spec_mvec_rg16f[2 * idx + 0] = Q20ToHalfRNE(hit_sphere ? spec_sphere_mvx_q20 : mv_fg_x_q20);
                b.spec_mvec_rg16f[2 * idx + 1] = Q20ToHalfRNE(hit_sphere ? spec_sphere_mvy_q20 : mv_fg_y_q20);
            }

            if (guide_mask & kGuideResponsivity)
                b.resp_r16f[idx] = Q20ToHalfRNE(was_occluded ? 786432 : 0); // +0.75 in R16F [-1, 1]

            if (guide_mask & kGuideDepthOfField)
                b.dof_r16f[idx] = Q20ToHalfRNE(hit_sphere ? 0 : 209715); // 0.0 in-focus foreground, 0.2 CoC background
        }
    }

    if (guide_mask & kGuideTranspMvecs)
    {
        const uint16_t tmv_x = is_v2
            ? Q20ToHalfRNE(static_cast<int32_t>(-(du_tl_q20 * static_cast<int64_t>(out_w)) * mv_mul))
            : Q20ToHalfRNE(static_cast<int32_t>(-((du_bg_q20 * out_w) >> 21)));
        const uint16_t tmv_y = is_v2
            ? Q20ToHalfRNE(static_cast<int32_t>(-(dv_tl_q20 * static_cast<int64_t>(out_h)) * mv_mul))
            : 0x0000u;
        const size_t n_out = size_t(out_w) * out_h;
        for (size_t i = 0; i < n_out; ++i)
        {
            b.transp_mvec_rg16f[2 * i + 0] = tmv_x;
            b.transp_mvec_rg16f[2 * i + 1] = tmv_y;
        }
    }
}

// Canonical frame-input SHA-256: hashes fixture version, all scalar/matrix/preset/reset/subrect fields,
// and the active subrect bytes (row-by-row, tight rw * bpp) of every bound input texture.
inline std::string ComputeCanonicalFrameInputSha256(
    const FrameScalarParams& p,
    uint32_t guide_mask,
    const CpuFrameBuffers& b,
    FixtureSuite suite = FixtureSuite::V1)
{
    const bool is_v2 = (suite == FixtureSuite::V2_Motion);
    const char* fv = FixtureVersionForSuite(suite);

    Sha256 h;
    h.Update(fv, std::strlen(fv) + 1);
    h.UpdateU32LE(p.frame_index);
    h.UpdateU32LE(p.rw);
    h.UpdateU32LE(p.rh);
    h.UpdateU32LE(p.out_w);
    h.UpdateU32LE(p.out_h);
    h.UpdateU32LE(static_cast<uint32_t>(p.reset));
    h.UpdateU32LE(p.preset_hint);
    h.UpdateU32LE(static_cast<uint32_t>(p.perf_quality));
    h.UpdateU32LE(static_cast<uint32_t>(p.create_flags));
    h.UpdateU32LE(guide_mask);
    if (is_v2)
    {
        h.UpdateU32LE(p.in_base_x);
        h.UpdateU32LE(p.in_base_y);
        h.UpdateU32LE(p.roughness_mode);
    }
    h.UpdateF32LE(p.jitter_x);
    h.UpdateF32LE(p.jitter_y);
    h.UpdateF32LE(p.mv_scale_x);
    h.UpdateF32LE(p.mv_scale_y);
    h.UpdateF32LE(p.pre_exposure);
    h.UpdateF32LE(p.exposure_scale);
    for (int i = 0; i < 16; ++i) h.UpdateF32LE(p.world_to_view[i]);
    for (int i = 0; i < 16; ++i) h.UpdateF32LE(p.view_to_clip[i]);

    auto hashSubrect = [&](uint32_t slot_tag, const void* base, uint32_t pitch_elems, uint32_t bx, uint32_t by, uint32_t w, uint32_t rows, uint32_t bpp) {
        h.UpdateU32LE(slot_tag);
        h.UpdateU32LE(w);
        h.UpdateU32LE(rows);
        h.UpdateU32LE(bpp);
        const auto* bytes = static_cast<const uint8_t*>(base);
        const size_t src_row_bytes = size_t(pitch_elems) * bpp;
        const size_t col_off_bytes = size_t(bx) * bpp;
        const size_t active_row_bytes = size_t(w) * bpp;
        for (uint32_t y = 0; y < rows; ++y)
            h.Update(bytes + size_t(by + y) * src_row_bytes + col_off_bytes, active_row_bytes);
    };

    const uint32_t bx = is_v2 ? p.in_base_x : 0u;
    const uint32_t by = is_v2 ? p.in_base_y : 0u;

    hashSubrect(1u, b.color_rgba16f.data(),   b.alloc_rw, bx, by, p.rw, p.rh, 8u);
    hashSubrect(2u, b.diffuse_rgba8.data(),   b.alloc_rw, bx, by, p.rw, p.rh, 4u);
    hashSubrect(3u, b.specular_rgba8.data(),  b.alloc_rw, bx, by, p.rw, p.rh, 4u);
    hashSubrect(4u, b.normals_rgba16f.data(), b.alloc_rw, bx, by, p.rw, p.rh, 8u);
    hashSubrect(5u, b.depth_r32f.data(),      b.alloc_rw, bx, by, p.rw, p.rh, 4u);
    hashSubrect(6u, b.mvec_rg16f.data(),      b.alloc_rw, bx, by, p.rw, p.rh, 4u);
    if ((guide_mask & kGuideOmitHitDist) == 0u)
        hashSubrect(7u, b.hit_dist_r32f.data(), b.alloc_rw, bx, by, p.rw, p.rh, 4u);

    if (guide_mask & kGuideDisocclusion)
        hashSubrect(10u, b.disocc_r16f.data(), b.alloc_rw, bx, by, p.rw, p.rh, 2u);
    if (guide_mask & kGuideTranspLayer)
    {
        hashSubrect(11u, b.transp_rgba16f.data(),    b.alloc_rw, bx, by, p.rw, p.rh, 8u);
        hashSubrect(12u, b.transp_op_rgba16f.data(), b.alloc_rw, bx, by, p.rw, p.rh, 8u);
    }
    if (guide_mask & kGuideTranspMvecs)
        hashSubrect(13u, b.transp_mvec_rg16f.data(), b.out_w, 0u, 0u, p.out_w, p.out_h, 4u);
    if (guide_mask & (kGuideSssAndCbt | kGuideSssOnly))
        hashSubrect(14u, b.sss_r16f.data(),     b.alloc_rw, bx, by, p.rw, p.rh, 2u);
    if (guide_mask & (kGuideSssAndCbt | kGuideCbtOnly))
        hashSubrect(15u, b.cbt_rgba16f.data(),  b.alloc_rw, bx, by, p.rw, p.rh, 8u);
    if (guide_mask & kGuideSpecularMvec)
        hashSubrect(16u, b.spec_mvec_rg16f.data(), b.alloc_rw, bx, by, p.rw, p.rh, 4u);
    if (guide_mask & kGuideResponsivity)
        hashSubrect(17u, b.resp_r16f.data(), b.alloc_rw, bx, by, p.rw, p.rh, 2u);
    if (guide_mask & kGuideDepthOfField)
        hashSubrect(18u, b.dof_r16f.data(), b.alloc_rw, bx, by, p.rw, p.rh, 2u);
    if (is_v2 && (guide_mask & kGuideUnpackedRough))
        hashSubrect(19u, b.rough_r16f.data(), b.alloc_rw, bx, by, p.rw, p.rh, 2u);
    if (is_v2 && (guide_mask & kGuideExposureTex))
    {
        h.UpdateU32LE(20u);
        h.UpdateF32LE(b.exposure_tex_r32f);
    }

    return h.FinalHex();
}

inline void RunSelfTests()
{
    auto check = [](bool ok, const char* msg) {
        if (!ok) throw std::runtime_error(std::string("SelfTest failed: ") + msg);
    };

    // 1. FloatToHalfRNE regression tests (including exponent carry propagation!)
    check(FloatToHalfRNE(0.0f) == 0x0000u, "+0.0f -> 0x0000");
    check(FloatToHalfRNE(-0.0f) == 0x8000u, "-0.0f -> 0x8000");
    check(FloatToHalfRNE(1.0f) == 0x3C00u, "1.0f -> 0x3C00");
    check(FloatToHalfRNE(-1.0f) == 0xBC00u, "-1.0f -> 0xBC00");
    check(FloatToHalfRNE(1.9999f) == 0x4000u, "1.9999f carry into exponent -> 0x4000 (2.0)");
    check(FloatBitsToHalfRNE(0x3F801000u) == 0x3C00u, "RNE tie to even (LSB 0) rounds down");
    check(FloatBitsToHalfRNE(0x3F803000u) == 0x3C02u, "RNE tie to even (LSB 1) rounds up");
    check(FloatToHalfRNE(65504.0f) == 0x7BFFu, "65504.0f -> max normal 0x7BFF");
    check(FloatToHalfRNE(65520.0f) == 0x7C00u, "65520.0f -> +Inf 0x7C00");

    // 2. Integer sqrt tests
    check(Isqrt64(0ull) == 0u, "Isqrt64(0)");
    check(Isqrt64(1ull) == 1u, "Isqrt64(1)");
    check(Isqrt64(1048576ull * 1048576ull) == 1048576u, "Isqrt64(2^40)");
    check(Isqrt64(999999999999ull) == 999999u, "Isqrt64(10^12 - 1)");

    // 3. FIPS 180-4 SHA-256 test vector ("abc")
    check(Sha256BytesHex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA-256('abc') vector mismatch");

    // 4. v2_motion regression checks (pixel-scale MV, moving camera, non-flat transparency, cbt != color)
    {
        CpuFrameBuffers b;
        b.Allocate(64, 36, 96, 54, kGuideTranspLayer | kGuideTranspMvecs | kGuideCbtOnly | kGuideSpecularMvec);
        FrameScalarParams p0 {}, p1 {};
        PrepareFrame(SceneId::S1_TemporalHalton, kGuideTranspLayer | kGuideTranspMvecs | kGuideCbtOnly | kGuideSpecularMvec,
                     0u, 64, 36, 96, 54, 1, 6u, 2, p0, b, FixtureSuite::V2_Motion);
        PrepareFrame(SceneId::S1_TemporalHalton, kGuideTranspLayer | kGuideTranspMvecs | kGuideCbtOnly | kGuideSpecularMvec,
                     1u, 64, 36, 96, 54, 0, 6u, 2, p1, b, FixtureSuite::V2_Motion);
        // Background MV at 64px width: -1229 * 64 / 2^20 = -0.075 px (far above 2^-20!), and at 1280px = -1.50 px
        check(std::abs(HalfToDouble(b.mvec_rg16f[0])) > 0.05, "v2_motion mvec_rg16f must be pixel-scale (not >>20)");
        check(p1.world_to_view[12] != p0.world_to_view[12], "v2_motion world_to_view[12] must move per frame");
        check(HalfToDouble(b.transp_mvec_rg16f[0]) > 0.25, "v2_motion transp_mvec must be pixel-scale");
        check(b.transp_op_rgba16f[0] == 0x0000u, "v2_motion transp_op outside glass must be 0.0 opacity (not 1.0 transmittance)");
        check(b.cbt_rgba16f[0] != b.color_rgba16f[0], "v2_motion cbt_rgba16f must differ from color_rgba16f in veil region");
    }
}

} // namespace rr_ref
