# OpenRR (`openrr-amd.asi`) 缺陷修复 + RDNA4 性能 / 显存优化计划（`v0.1.5` 更新版）

> [!IMPORTANT]
> **执行目标与复用原则（2026-11-12 按 `v0.1.5` 合流、四锚点体系与逐核 `@idx` 修正更新）**
> 1. **目标**：基于已合流的上游 `v0.1.5` 基线，在**四锚点评测体系**（内部 `v0.1.5 Legacy` 无损锚点 + 外部 RTX 5090 官方 `nvngx_dlssd.dll 310.9.1.0` 真值锚点 + 单栈 `ACO strict` 测速 / `LLPC` 交付门禁 + `guides=0/1` 双编译器数值稳健性锚点）约束下，先消除 ACO 发布阻断项（`guides=1` 下的 `_lt` 分歧与 `flow` 深层核 VGPR 劣势），再推进剩余无损/相对无损 FP8（阶段 2）与有损激活 FP8 + `hist_` 压缩（阶段 3）。
> 2. **优先复用**：从 `DLSSNR-AMD-mochizuki` / `OptiScaler-NR-src` 复制适用的现成实现，围绕 OpenRR `v0.1.5`（含 `W1`、`W2` 与 `TR` 三套核心体）的接口、布局和数值语义做必要适配；按下方本地路径核对来源。
> 3. **本地未追踪文档**：本目录（`C:\Users\Kenny\Downloads\OpenRR\docs\local-plans\`）已在 `.gitignore` 中忽略，仅保存在本地供开发与审查使用，不进入 Git 提交历史。
>
> **本地仓库与工作树绝对路径索引**：
> - **当前主工作树（`asi-plugin` @ `0934ad6`，已合入 `v0.1.5`）**：`C:\Users\Kenny\Downloads\OpenRR`
> - **ACO 优化工作树（`codex/v015-stage2-overnight` @ `bf5820e`，待推送为 `openrr263`）**：`C:\Users\Kenny\.codex\worktrees\mesa-263-comparison\OpenRR`
> - **RTX 5090 官方参考独立评测仓（`RR_Contrast` @ `09310c8`）**：`C:\Users\Kenny\Downloads\OpenRR\build\RR_Contrast`（远端：[TheAutomatic/RR_Contrast](https://github.com/TheAutomatic/RR_Contrast)）
> - **RTX 5090 官方 `310.9.1.0` 真值落盘目录（锚点 #2，`rr_contrast_v1` 已校验入库）**：`C:\Users\Kenny\Downloads\OpenRR\build\RR_Contrast_RTX5090_310.9.1.0`（配套汇总与 `v2_motion` 方案见 [`rtx5090-anchor2-and-v2-plan.md`](file:///c:/Users/Kenny/Downloads/OpenRR/docs/local-plans/rtx5090-anchor2-and-v2-plan.md)）
> - **借鉴仓库（Mochi / DLSSNR-AMD）**：`C:\Users\Kenny\Downloads\DLSSNR-AMD-mochizuki`
> - **OptiScaler 公开源码参考**：`C:\Users\Kenny\Downloads\DLSSNR-AMD-mochizuki\artifacts\ref\OptiScaler-NR-src\OptiScaler`

> 工作分支：`asi-plugin`（LLPC 主线，`975c5f3`）与 `codex/mesa-263-comparison`（全核 ACO 线，`1f8038d`）；`main`（`d9a3b2d`）只跟上游（`upstream`）同步。

原则（全程有效）：

- **只保留实测正向提速项**；任一目标分辨率下变慢的开关不得进默认产物。
- **默认交付门禁**：全核强制 ACO（`ACO strict`）整包在目标分辨率上**跑赢同版 `v0.1.5 Fast-LLPC`（1080p `3.009 ms` / 4K `11.389 ms`）且解决 `guides=1` 数值分歧之前**，不替代 LLPC 作为面向用户的默认发布包。
- Legacy（`--fast-mode 0`）保持逐位一致（`max_ulp_diff == 0`，对 `v0.1.2` MSVC 已实测 `0 ULP`），除非某阶段明确标为有损并单独门禁。
- **画质度量统一采用逐帧曲线（`parity-all-frames`）**：禁止使用受混沌敏感性影响的单点末帧 PSNR 做跨构建结论，统一报告 `min / steady_end / drs_min` 及逐帧曲线，并保证被比较双方 `--align-aco-dispatch` 等运行参数严格同构。
- 每阶段结束跑 `test_asi_units.exe` + `openrr_bench.exe`（1080p / 1440p / 4K），写进 `docs/test-evidence/`。

---

## 历史里程碑归档（`B0 + B1 + B2` → `v0.1.5` 合流处置）— ✅ 已完成

> **当前状态：`v0.1.5` 已合流（`asi-plugin @ 975c5f3` / `codex/mesa-263-comparison @ 1f8038d`）**  
> 早期 `v0.1.2` 上的 `B0 + B1 + B2`（`90fea26` + `97be0d3`）在升级到 `upstream 0.1.4 → 0.1.5` 后的保留与替代关系如下：
> - **`B0`（移除默认 W1 `LDS_OCC_PAD=8192` + SPIR-V 哈希防回退门禁）**：**继续保留**于 [`fastmode.cmake`](file:///c:/Users/Kenny/Downloads/OpenRR/gpu/fastmode.cmake) 与 [`build_shaders.py`](file:///c:/Users/Kenny/Downloads/OpenRR/asi/build/build_shaders.py)。
> - **`B1`（`t0` 尾部无损池化 + `attr_wide_` 安全分配）**：上游 `0.1.4/0.1.5` 已引入统一的 `fast_layout` 生命周期 `arena_`（4K feature 显存由 `960 MB` 大幅降至 **`789 MB`**，1080p 由 `251 MB` 降至 **`207 MB`**），接管子缓冲池化；`attr_wide_` 继续采用独立于生命周期 `arena_` 的按需安全分配（[`engine.cpp`](file:///c:/Users/Kenny/Downloads/OpenRR/gpu/src/engine.cpp#L658-L667)）。
> - **`B2`（旧 `post` `LT_POST_EXP/NOCORNER/NETH`）**：**已被上游 `v0.1.5` 重写版 `post` 正式替代**（[`fastmode.cmake`](file:///c:/Users/Kenny/Downloads/OpenRR/gpu/fastmode.cmake#L8-L9) `LT_POST_PAIR=1 LT_POST_HW=1 LT_POST_LDS=1 LT_POST_EXPO=1`，机器码体积 `−8596 B`、寄存器由 `120 VGPR` 降至 `96 VGPR`，实测优于旧 B2 组合，旧宏不再叠加）。

<details>
<summary>展开查看早期 <code>v0.1.2 (B0 + B1 + B2)</code> 逐项归档记录</summary>

### B0. 撤回默认产物里的 W1 `LDS_OCC_PAD`（已关缺陷）

1080p W1 合计 −5.0%，**4K 合计 +5.8%**，却写进了默认 `fastmode.cmake` / `build_shaders.py`。基线默认全关，`fastmode.cmake` 与 `build_shaders.py` 已移除注入，保留 `#ifdef LDS_OCC_PAD` 供实验室单测。

### B1. 无损显存池收尾（早期 `v0.1.2` Fast 模式 `t0` 尾部池化）

在 `v0.1.2` 分立缓冲布局下将 `dec1_l_[2..4].tensor` 与 `dec1_l_[1..4].dfn` 并入 `t0` 尾部并通过 29 子缓冲零重叠审计（节省 `1.44 / 2.56 / 5.62 MB`）；合入 `0.1.4/0.1.5` 后已由上游 `fast_layout` 单一 `arena_` 整体接管（4K 直接节省 `~171 MB`）。

### B2. 早期 `v0.1.2` 逐项 A/B `LT_*` 筛选记录

| 宏组 | 1080p 3 轮中位 `[min..max]` (`test_asi_units`) | 4K 3 轮中位 `[min..max]` (`test_asi_units`) | `v0.1.2` 决策 / `v0.1.5` 状态 |
|---|---|---|---|
| `post`：`LT_POST_EXP=1 LT_POST_NOCORNER=1 LT_POST_NETH=1` | `346.3 → 324.2 us` (**−6.4%**) | `1497.1 → 1405.6 us` (**−6.1%**) | `v0.1.2` 开启；**`v0.1.5` 由上游 `LT_POST_PAIR/HW/LDS/EXPO` 替代** |
| `post`：`+LT_POST_LOG=1` | `324.2 → 349.4 us` (+7.8%) | `1405.6 → 1495.0 us` (+6.4%) | **关闭**（RDNA4 硬件 `v_log_f32`/`v_exp_f32` 更快） |
| `enc0_attr`：`LT_ATTR_RCP=1 LT_ATTR_DEPTH=1` | `176.4 → 180.6 us` (+2.4%) | `816.6 → 814.6 us` (−0.2%) | **关闭**（`v0.1.5` 上游改用 `LT_ATTR_TAB/LATE/D9`） |
| `enc0_attr`：`+LT_ATTR_SAME=1 LT_ATTR_CL=1` | `180.6 → 243.4 us` (+34.8%) | `814.6 → 985.8 us` (+21.0%) | **关闭** |
| `C=64`（×3 合计）：`LT_FAR=3` | `712.3 → 709.1 us` (−0.5%) | `2664.6 → 2697.5 us` (+1.2%) | **关闭** |

</details>

---

## 现状快照（`v0.1.5`，RX 9070 XT，6 轮配对交替中位数，2026-11-11）

> 测量口径：`48` 帧预热 + `240` 帧稳态 + `64` 帧 DRS，6 轮 ABBA 配对交替取中位数；完整日志与证据见 [`v015-integration-results.md`](file:///c:/Users/Kenny/.codex/worktrees/mesa-263-comparison/OpenRR/docs/test-evidence/v015-integration-results.md) §9.1（注：该报告 §9.3 的逐核表存在 `@idx` 错位，已在本计划 **§4.2** 修正）。

| 分辨率 (`Render → Output`) | 模式 | **`v0.1.5 LLPC`** 稳态 `median` (ms) | **`v0.1.5 ACO strict`** 稳态 `median` (ms) | ACO 相对 LLPC 差距 | Feature 显存 / 进程增量 | Legacy 对 `v0.1.2` 精度 |
|---|---|---:|---:|---:|---|---|
| **1080p Quality** (`1280×720 → 1920×1080`) | **Fast** (`--fast-mode 1`) | **`3.009 ms`** | **`3.355 ms`**（较 `0.1.4-ACO` `−3.88%`） | **`+0.346 ms` (`+11.48%`)** | **`207.0 MB`** / `229.1 MB` | — |
| **1080p Quality** (`1280×720 → 1920×1080`) | Legacy (`--fast-mode 0`) | `3.468 ms` | `3.899 ms` | `+0.431 ms` (`+12.41%`) | `207.0 MB` / `229.1 MB` | **`0 ULP`**（逐位一致） |
| **4K Quality** (`2560×1440 → 3840×2160`) | **Fast** (`--fast-mode 1`) | **`11.389 ms`** | **`12.300 ms`**（较 `0.1.4-ACO` `−4.35%`） | **`+0.911 ms` (`+8.00%`)** | **`789.0 MB`** / `810.3 MB` | — |
| **4K Quality** (`2560×1440 → 3840×2160`) | Legacy (`--fast-mode 0`) | `13.123 ms` | `14.508 ms` | `+1.385 ms` (`+10.55%`) | `789.0 MB` / `810.3 MB` | **`0 ULP`**（逐位一致） |

---

## 推进路线总览（`v0.1.5` 合流后）

### 阶段 1.2：OptiScaler `DLSSDFeature` 宿主桥接（修复 `OptiFG (Upscaler)` 与顶部活动超分显示）— 📋 详见 [`optifg-dlssd-host-plan.md`](file:///c:/Users/Kenny/Downloads/OpenRR/docs/local-plans/optifg-dlssd-host-plan.md)

- **直接照搬来源**：[`nr_pe_optifix.cpp`](file:///C:/Users/Kenny/Downloads/DLSSNR-AMD-mochizuki/windows/src/pe/nr_pe_optifix.cpp#L31-L110)（`find_module`、`string_function`、`function_start`）+ [`OptiScaler-NR-src`](file:///C:/Users/Kenny/Downloads/DLSSNR-AMD-mochizuki/artifacts/ref/OptiScaler-NR-src/OptiScaler)。
- **方案**：在 `RayReconstruction` 激活时引导 `OptiScaler.dll` 创建零冗余显存开销的 `DLSSDFeatureDx12` / `DLSSDFeatureVk` 宿主对象，并通过 `string_function` + MinHook 直接接管 `EvaluateInternal` 到 `openrr-amd.asi`。

### 阶段 1.5：WSL Mesa RADV (ACO) 离线机器码注入 — ✅ 已完成基础接入与 `v0.1.5` 重烘（`codex/mesa-263-comparison @ 1f8038d`），详见 [`mesa-aco-plan.md`](file:///c:/Users/Kenny/Downloads/OpenRR/docs/local-plans/mesa-aco-plan.md)

- **直接照搬来源**：[`nr_pal_binary.hpp`](file:///C:/Users/Kenny/Downloads/DLSSNR-AMD-mochizuki/windows/src/core/nr_pal_binary.hpp#L1-L378) → [`rr_pal_binary.hpp`](file:///c:/Users/Kenny/Downloads/OpenRR/gpu/src/rr_pal_binary.hpp)（`push_bytes = 256`、`descriptor_slot = 65`、D3D12 spill 偏移 `+16`）。
- **当前定位**：已完成 Mesa `26.0 → 26.3-devel` 对照与 `v0.1.5` 全核 52 份 `.nrp` 重烘。按 §4.2 修正后的逐核定位，**`W1` / `W2` 六个主力大核上 ACO 已与 LLPC 持平（`1.00×–1.05×`）**，整帧 `+8.0% / +11.5%` 的差距约 **60% 集中在 `flow` 深层链（`enc2..dec2_1`，`240–256 VGPR` 占用率台阶 + `SPLIT=2`）**，其余在 `post_f_lt`（`1.13×`）与 `d3d_enc0_attr_f_lt`（`1.11×–1.19×`）。

### 阶段 2.0：ACO 发布阻断项攻坚（`guides=1` 数值分歧修复 + 深层核 ACO 代码生成追平）— 2.0A ✅ 已关闭 / 2.0B 🔥 施工中

1. **2.0A（画质发布阻断）— ✅ 已关闭（`c8ca717` + `703271f`，详见 [`v015-stage2-overnight-results.md`](file:///c:/Users/Kenny/.codex/worktrees/mesa-263-comparison/OpenRR/docs/test-evidence/v015-stage2-overnight-results.md) §2）**：通过 `ONR_ACO_OPTIONAL` 单核混跑唯一锁定 `d3d_enc0_attr_f_w_lt` 为 `32.74 dB` 分歧源（核内 `signed_log` / `enc_log` / `decode_hist` 的 `log2`/`exp2` ULP 差与 `ENC_F32` 双线性 `BLEND4` 收缩差）。在 `defined(LT_ATTR_TAB) && defined(ATTR_WIDE)` 下引入带 IEEE `NaN/Inf/overflow` 守卫的确定性多项式与 `precise`，使 **`guides=1` ACO vs LLPC 从 `32.74 dB` 拉升至 `126.76 / 126.81 dB`（`max_ulp = 1`）**，`Legacy 0 ULP`、`g0` 逐位不变、零速度代价。
2. **2.0B（速度追平 LLPC）— 🔥 日间轮已排除朴素 `[[dont_unroll]]`（`ac42b98`），转向 `c0..c7` 延迟累加降 VGPR 与 `ROW`/LDS 向量化**：
   - `ONR_RR_FLOW=0` + `@idx` 真串行表（`2c35cb6`）证实 4K `+1.49 ms` 串行差距中 **~74%（`+1.107 ms`）集中在深层 10 核**（`C=128` 三核 `1.69×–2.04×`）。
   - 日间轮（[`v015-stage2-day-results.md`](file:///c:/Users/Kenny/.codex/worktrees/mesa-263-comparison/OpenRR/docs/test-evidence/v015-stage2-day-results.md)）实测 `LT_ROLL_HD/J` 虽将静态码缩减 `18%–27%`，但 VGPR 仍锁死 `256/240` 且因破坏跨迭代预取使端到端变慢 `+1.3%–2.5%`（已回退默认关闭）。
   - **源码复核纠偏（防踩空）**：① 现有 `--cmat-address-shaders`（[`global-address.patch`](file:///c:/Users/Kenny/.codex/worktrees/mesa-263-comparison/OpenRR/asi/build/mesa/global-address.patch#L12-L16)）仅匹配全局内存 + `COL` + FP16，而 `TR` 深层核 `WSTEP` 全为 `ROW` 且不触及 LDS，盲目扩列表无效；② `C=128` 锁死 `256 VGPR` 的真因是进 `for (hd)` 前常驻 `ACC c0..c7`（64 VGPR）+ `x0..x7`（32 VGPR）+ `y0..y7`（16 VGPR），下一步应仿 `W2_DEFER=1` 将 `TR` 体的 `out` 累加器推迟至 `hd` 循环之后以释放 64 VGPR，并核查 RADV 对 `ROW`/LDS `coopMatLoad` 的向量化下沉。

### 阶段 2：剩余无损 / 相对无损原生 FP8 扩展（避开 `v0.1.5` 已落地的 `LT_UP8` / `LT_MI8`）— 🔥 `GL_EXT_shared_memory_block` 零膨胀路径已打通待施工

- **关键前置事实**：上游 `v0.1.5` 已在默认 Fast 中启用了 `dec_up_32_64_64`（`32` 条 FP8 WMMA）与 `dec_up_64_64_96`（`48` 条 FP8 WMMA）的 **`LT_UP8=1`**，以及 `enc_stage_64_64_96`（`64` 条 FP8 WMMA）的 **`LT_MI8=1`**（加上原有的 `C>=64` `FP8QV=1`），**切勿重复开发**。
- **阶段 2 各子项状态**：
  1. ❌ **`LT_E8RAW=1`（已按门禁淘汰回退，转入阶段 3 备选池）**：过夜实测三档 `-0.23% ~ -0.45%`（噪声内），且 `1080p g0 steady 59.24 dB` / `1440p g1 DRS 52.56 dB` 越过阶段 2 `60 dB` 相对无损线；
  2. 🔥 **`ENC_MERGE` 输出下采样合并（`2B`，高确定性 LDS 别名路径）**：**纠偏日间报告 §3 的“LDS 翻倍”顾虑**——[`onr_block.glsl:386-399`](file:///c:/Users/Kenny/Downloads/OpenRR/gpu/shaders/include/onr_block.glsl#L386-L399) 使用 `GL_EXT_shared_memory_block`，新增 `shared LdsE { floate4m3_t lds8[2u * LDS_A]; };` 与 `LdsH/W/D/Q` 在 offset `0` 严格重叠且同为 `2 * LDS_A` 字节（**0 字节 LDS 增量**，亦无需逆向寄存器 lane↔tile 映射）：`MST` 将 8 个通道转为 OCP E4M3 打包 `uvec2` 写入 `lds2[a_ >> 3]`，读端 `coopMatLoad(a8_, lds8, k, 4u * ENC_C, ROW)` 乘 `b.mrg_w` 尾部已由 `qv_bytes()` 转好的 OCP E4M3 权重即可；
  3. 🔥 **`C >= 96` 的 `DEC_UP`（`2A-2`）**：复用同一个 `LdsE` 零膨胀别名视图，将 `put16` 改为 `put8_ocp`（`OCP4` 后写 `lds2`）并放开 `U8(b.up_w)`（若深层 `up_w` 含非精确 E4M3 值则用离线 RNE 替代抛异常的 `qv_bytes`），无需重排 `TR` 寄存器映射即可打通 `dec_up_96/128/160` 原生 FP8；
  4. ⏳ **`#3` 大核 `dec_head_32_64_f_lt`（`2C`）**：输入 `pc.x` 已是 E4M3 arena，测试 `lr_keys` / `vo_values` 带离线列缩放的 `Q/V` FP8 化。

### 阶段 3：有损激活 FP8（MLP `fc1`/`fc2`、`ENC_EMBED`、`W2 wout`、融合后 `Q/V`）+ `hist_` 4 B/px 显存压缩 — ✅ 前置锚点 #2 首轮真值已落盘

- **前置条件已解锁**：[`RR_Contrast#1`](https://github.com/TheAutomatic/RR_Contrast/issues/1) 已交付 **RTX 5090 + `nvngx_dlssd.dll (310.9.1.0)`** 首轮完整 `rr_contrast_v1` 数据包（`1.06 GB`，286 文件 0 error，本地已落盘校验于 `build/RR_Contrast_RTX5090_310.9.1.0`；稳态 `0.87 / 1.41 / 2.91 ms`，4K 官方显存 `1.51–1.94 GB` vs OpenRR `789 MB`；详见 [`rtx5090-anchor2-and-v2-plan.md`](file:///c:/Users/Kenny/Downloads/OpenRR/docs/local-plans/rtx5090-anchor2-and-v2-plan.md)）。`v1` 已可直接支撑主干算子对账，另备 <30 秒的 `rr_contrast_v2_motion` 增量方案覆盖多像素运动/相机反射/透明层纹理重投影。
- **目标池**：主力大核剩余的 `196–472` 条 FP16 WMMA/核（MLP `fc1`/`fc2`、`enc0_stage_32_64` 的 `ENC_EMBED`、`W2` 的 `wout`、`DEC_UP` skip `fma` 后的 `Q/V`、`W1` 的 `vo_values`），以及二段折线 `LT_G4` GELU 与独立 `hist_` `8 B/px → 4 B/px` 压缩（4K 再省 `~149.3 MB`，将 4K feature 显存从 `789 MB` 压至 `~640 MB`）。

---

## 4. `v0.1.5` 逐核热点、ISA WMMA 盘点与 ACO-LLPC 真实差距定位

硬件：**AMD Radeon RX 9070 XT（RDNA4，`gfx1201`）**，Windows 驱动 **26.10.1 (LLPC)**，WSL Mesa **26.0 / 26.3-devel (ACO)**，`nvngx_dlssd.dll` **310.9.1.0**。

> [!WARNING]
> **`ONR_RR_STAMPS` 逐核计时两条硬性防坑规则（修正 `1f8038d` `v015-integration-results.md` §9.3 的解析错位）**
> 1. **必须按 `stamped dispatches` 中的 `@idx` 下标索引 `timestamp intervals`，严禁 `1:1` 顺序 zip**：[`d3d12.cpp`](file:///c:/Users/Kenny/Downloads/OpenRR/gpu/src/d3d12.cpp#L906-L908) 在每次 `dispatch()` 前后各打一个 query（`@2k` 为算子本体耗时，`@2k+1` 为 barrier/间隔），22 次 dispatch 共产生 **47 个区间**（`0..46`）。`1f8038d` 的 `noflow_summary.py` 误将 22 个核名与前 22 个区间 `0..21` 顺序配对，导致奇数位算子（如 `enc_stage_64_64_96_f_lt`、`dec_head_32_64_f_lt`）全落到 `0.0000 ms` 间隔上，偶数位算子全部张冠李戴（如把 `@16 enc_stage_128_128_160_fl` 的 `1.84×/1.56×` 错安到 `dec_up_64_64_96_f_lt` 头上，把 `@4 pooled_history2_lt` 错标成 `enc0_stage_32_64_f_lt`）；而全部 47 个区间求和恒等于首尾时间戳差 `ts[47] - ts[0]`，因此加和自检无法发现错位。
> 2. **关闭 `flow` 并发必须设置环境变量 `ONR_RR_FLOW=0`，不能只删 `*_fl_lt.spv`**：[`engine.cpp`](file:///c:/Users/Kenny/Downloads/OpenRR/gpu/src/engine.cpp#L574-L575) 检查的是基础核表 `K.has("*_fl")`（非 `lk_`）；只删 `*_fl_lt.spv` 时 `flow` 仍为 `1`，且 Fast 模式会静默回退到 Legacy 的 `*_fl.spv` 并发执行。下方 §4.1 已按真实 `@idx` 从原始日志重新提取全部 **12 个天然带 barrier 隔离的大核精确耗时**，以及 `@14..@33` `flow` 链整体墙钟跨度。

### 4.1 `v0.1.5` 真实 `@idx` 逐核耗时与 ISA WMMA 统计（600 稳态帧均值，同一 SPIR-V 下 ACO vs LLPC）

| `@idx` | 算子（Fast 模式） | 核心体架构 | Shipped `.nrp` WMMA (`fp8` / `f16`) | 1080p ACO / LLPC (ms) | 1080p `ACO/LLPC` | 4K ACO / LLPC (ms) | 4K `ACO/LLPC` (`Δms`) | 结论与定位 |
|---:|---|---|---|---:|---:|---:|---:|---|
| `@10` | **`enc0_stage_32_64_f_lt`** | `W1` (1 wave) | **`0` / `257`** | `0.3644` / `0.3484` | `1.05×` | **`1.4418` / `1.3971`** | **`1.03×`** (`+0.045`) | **#1 单核热点**；全 FP16（`ENC_EMBED` + 块体） |
| `@42` | **`post_f_lt`** | 独立后处理 | `0` / `0` | `0.3283` / `0.2897` | `1.13×` | **`1.2948` / `1.1479`** | **`1.13×`** (`+0.147`) | **#2 单核热点**；非流式核最大 ACO 差距源 |
| `@40` | **`dec_head_32_64_f_lt`** | `W1` (1 wave) | **`0` / `196`** | `0.3065` / `0.2932` | `1.05×` | **`1.2672` / `1.2105`** | **`1.05×`** (`+0.057`) | **#3 单核热点**；输入已是 E4M3 arena 但 0 条 FP8 |
| `@38` | **`dec_up_32_64_64_f_lt`** | `W1` (1 wave) | **`32` / `255`** | `0.3048` / `0.3003` | **`1.01×`** | **`1.2188` / `1.2182`** | **`1.00×`** (`+0.001`) | **#4 单核热点**；`v0.1.5` 已开 `LT_UP8=1`，**ACO 与 LLPC 完全打平** |
| `@12` | **`enc_stage_64_64_96_f_lt`** | `W2` (2 waves) | **`64` / `472`** | `0.2744` / `0.2631` | `1.04×` | **`1.0201` / `1.0149`** | **`1.01×`** (`+0.005`) | **#5 单核热点**；已开 `LT_MI8=1`，`ENC_MERGE` 仍为 FP16 |
| `@ 8` | **`d3d_enc0_attr_f_lt`** | 属性提取 | `0` / `0` | `0.2312` / `0.1942` | `1.19×` | **`0.9373` / `0.8451`** | **`1.11×`** (`+0.092`) | **#6 单核热点**；非流式核第二大 ACO 差距源 |
| `@34` | **`dec_up_64_64_96_f_lt`** | `W2` (2 waves) | **`48` / `415`** | `0.2239` / `0.2187` | **`1.02×`** | **`0.8522` / `0.8468`** | **`1.01×`** (`+0.005`) | **#7 单核热点**；已开 `LT_UP8=1`，**ACO 与 LLPC 完全打平** |
| `@36` | **`dec_dfn_64_64_f_lt`** | `W2` (2 waves) | 已含 `FP8QV` | `0.2036` / `0.2007` | **`1.01×`** | **`0.7868` / `0.7897`** | **`1.00×`** (`−0.003`) | **#8 单核热点**；已开 `FP8QV=1`，**ACO 与 LLPC 完全打平** |
| `@44` | `d3d_downsample_lt` | 历史下采样 | `0` / `0` | `0.0588` / `0.0475` | `1.24×` | `0.2649` / `0.2617` | `1.01×` (`+0.003`) | 4K 打平，1080p 差 `+0.011 ms` |
| `@2..6` | `clear_range` + `pooled_history2_lt` + `d3d_ae` | 预处理 ×3 | `0` / `0` | `0.0228` / `0.0227` | `1.00×` | `0.0936` / `0.0925` | `1.01×` (`+0.001`) | 两编译器完全一致 |
| `@14..33` | **深层 `flow` 10 核总跨度**<br>(`enc2..dec2_1`, `C=96/128/160`) | `W2` (`C=96` 2w) + `TR` (`C=96up/128` 4w) + `SPLIT=2` (`C=160` 8w) | `C>=96` 含 `FP8QV`（无 `UP8`/`MERGE8`） | `1.1194` / `0.8118`<br>*(回退 `_fl` 态)* | **`1.38×`**<br>*(无 stamps 推算 `~+0.21 ms`)* | `3.5078` / `2.5788`<br>*(回退 `_fl` 态)* | **`1.36×`** (`+0.929`)<br>*(无 stamps 推算 `~+0.53 ms`)* | **ACO 整帧落后的真正主因（占真实端到端差距约 60%）**：`C=96(W2)` 与 `C=128` 达 `240–256 VGPR`（`rsrc1=0x1d/0x1f`），叠加 `flow_wait` 自旋等待与 `C=160` `SPLIT=2`；反观 `192 VGPR` 的 `dec_up_96_128_128_fl` 为 `0.96×–1.01×` |

**三大关键事实（修正旧认识）**：
1. **ACO 在所有 `W1` / `W2` 大核（含全部已启用原生 FP8 的核）上没有任何 FP8/WMMA 劣势**：`dec_up_32_64_64`（`1.00×`）、`dec_dfn_64_64`（`1.00×`）、`enc_stage_64_64_96`（`1.01×`）、`dec_up_64_64_96`（`1.01×`）、`enc0_stage_32_64`（`1.03×`）、`dec_head_32_64`（`1.05×`）六个核心算子在 4K 下合计占 `6.59 ms`（超整帧一半），ACO 与 LLPC 总计仅差 **`+0.109 ms`（`+1.6%`）**。
2. **ACO 相对 LLPC 的端到端落后（1080p `+0.346 ms`、4K `+0.911 ms`）约 60% 来自 `enc2..dec2_1` 10 个深层核**，其余约 26% 来自 `post_f_lt`（4K `+0.147 ms`）和 `d3d_enc0_attr_f_lt`（4K `+0.092 ms`）。深层核内部呈现极清晰的寄存器台阶分化：`rsrc1 = 0x17`（192 VGPR）的 `dec_up_96_128_128` 上 ACO 达到 `0.96×–1.01×`，而冲到 `rsrc1 = 0x1d / 0x1f`（240–256 VGPR）的 `enc_stage_96_128_128`、`dec_dfn_96_128`、`C=128` 以及 256 线程的 `C=160` `SPLIT=2` 出现 `1.27×–1.87×` 退化。
3. **Top-5 大核仍有巨量未转化的 `f16` WMMA**：即便上游 `v0.1.5` 开了 `LT_UP8` 与 `LT_MI8`，五个大核仍保留 `257 + 196 + 255 + 472 + 415 = 1,595` 条静态 `v_wmma_f16_16x16x16_f16`（对比已转化的 `32 + 64 + 48 = 144` 条 `fp8` WMMA 仅占 **8.3%**），阶段 2（`ENC_MERGE`、`C>=96 UP8`、`dec_head Q/V`）与阶段 3（MLP `fc1`/`fc2`、`ENC_EMBED`、`W2 wout`）的天花板依然完整。

<details>
<summary>展开查看已归档早期 <code>v0.1.2 (B0+B1+B2)</code> 跑分表（仅供历史趋势参考）</summary>

### 4.2 早期 `v0.1.2` 端到端同进程 3 轮 A/B 历史数据（2026-11-08，LLPC）

| 档位 (`Render → Output`) | 版本 / 模式 | Feature 显存 | 进程显存增量 | 稳态 `mean` / `median` / `p95` (ms) |
|---|---|---|---|---|
| **1080p Quality** (`1280×720 → 1920×1080`) | `d6f70fb` → `B0+B1+B2` Fast | `252.0 → 251.0 MB` | `275.8 → 273.5 MB` | `3.171 → 3.124 ms` (`−1.5%`) |
| **1440p Quality** (`1706×960 → 2560×1440`) | `d6f70fb` → `B0+B1+B2` Fast | `438.0 → 435.0 MB` | `459.3 → 456.5 MB` | `5.451 → 5.391 ms` (`−1.1%`) |
| **4K Quality** (`2560×1440 → 3840×2160`) | `d6f70fb` → `B0+B1+B2` Fast | `966.0 → 960.0 MB` | `989.5 → 982.8 MB` | `11.941 → 11.812 ms` (`−1.1%`) |

</details>

---

## 5. 四锚点评测体系与阶段 2.0 / 2 / 3 执行路线（2026-11-12）

### 5.1 锁定的四锚点评测框架与逐帧画质曲线规范

单看对旧版 `v0.1.2` 末帧的单点 PSNR 既受帧间混沌放大干扰（同配置不同参数或跨帧单点波动可达 `50.5 ↔ 60.0 dB`），又无法区分“偏离旧复刻实现”与“偏离英伟达官方真值”。自 `v0.1.5` 起，所有优化实验统一锁定以下**四锚点体系**：

1. **锚点 #1（内部无损基准：`v0.1.5 Legacy`，`--fast-mode 0`）**：
   - `v0.1.5 Legacy` 与冻结 `v0.1.2 (MSVC)` 在 1080p / 4K 下已实测 **`0 ULP` 逐位一致**，且 ACO 与 LLPC 在 `guides=0`（逐位一致）和 `guides=1`（`>= 121.3 dB`）下完全重合。
   - **上游 `v0.1.5 Fast vs Legacy` 自接受损失地板**（画质预算的客观内部边界，ACO 与 LLPC 相差 `<= 0.6 dB`）：
     - `guides=0`：稳态末帧 **`49.5–59.4 dB`**（1080p 早期帧存在 `38.2 dB` 瞬态低谷，稳态回升至 `~49.5 dB`）；
     - `guides=1`（含真实 G-buffer 辅助引导）：稳态落在 **`29.0–34.0 dB`**（RMS 误差约 `3.2%`，系上游 `_lt` 近似的固有算法损失，不归编译器修）。
2. **锚点 #2（外部官方真值：RTX 5090 + `nvngx_dlssd.dll 310.9.1.0`，首轮已落盘就绪）**：
   - 由独立评测仓 [`RR_Contrast`](https://github.com/TheAutomatic/RR_Contrast)（`09310c8`，`fixture_version = rr_contrast_v1`，含 29 用例 76 组锁定输入 SHA-256 与目标 DLL SHA-256 硬门禁）在 RTX 5090 上采集官方 DLSS-RR 输出，首轮完整参考包已入库 `C:/Users/Kenny/Downloads/OpenRR/build/RR_Contrast_RTX5090_310.9.1.0`（汇总与 `v2_motion` 增量方案见 [`rtx5090-anchor2-and-v2-plan.md`](file:///c:/Users/Kenny/Downloads/OpenRR/docs/local-plans/rtx5090-anchor2-and-v2-plan.md)）。
   - **双层裁决规则**：**查实现 Bug** 对比同栈关闭态与锚点 #1（距离近、对位错误极敏感）；**裁决阶段 3 有损取舍**对比「候选 vs RTX 5090 官方输出」相对「`v0.1.5 Fast` vs RTX 5090 官方输出」的曲线变化，避免把更贴近官方真值的改动误判为退化。
3. **锚点 #3（测速 A/B 基线与默认交付门禁严格分离）**：
   - **单项开关去留 A/B**：始终在同一分支全核强制 ACO（`ACO strict`）下对比**同一父提交开关 OFF vs ON**（1080p / 1440p / 4K 配对交替运行），绝不混入跨编译器差异。
   - **默认发布替换门禁**：全核 ACO 整包必须在目标分辨率上**快于同版 `v0.1.5 Fast-LLPC`（1080p `3.009 ms` / 4K `11.389 ms`）**，方可替换 LLPC 成为默认交付方案。
4. **锚点 #4（双编译器数值稳健性：同一份 SPIR-V 下 `Candidate-ACO vs Candidate-LLPC`）**：
   - 必须同时覆盖 **`guides=0` 与 `guides=1`**！
   - 阶段 2.0A（`c8ca717` + `703271f`）已修复 `d3d_enc0_attr_f_w_lt` 内的超越函数与双线性收缩分歧，将 **`guides=1` ACO vs LLPC 从 `32.74 dB` 提升至 `126.76 / 126.81 dB`（`max_ulp = 1`）**，后续阶段 2/3 所有新改动均须守住该稳健区间。

**画质测量统一规程**：
- 统一使用 `32` 帧预热 + `32` 帧稳态 + `8` 帧 DRS，开启逐帧导出（`parity-all-frames`），同时记录 **`min`（全序列最差帧）/ `steady_end`（稳态末帧）/ `drs_min`（DRS 最差帧）** 及完整逐帧曲线（`PSNR_01`、`PSNR_tonemap`、线性 RGB `p99_abs_err`、`non_finite == 0`）。
- 凡做跨构建画质对比，两侧的 `--align-aco-dispatch`、`--guides`、尺寸序列与 `reset` 帧号必须逐项一致。

### 5.2 分级画质预算表（对接四锚点）

| 分类 | 对同栈 `ACO strict` 关闭态（单项增量） | 对锚点 #1 (`v0.1.5 Legacy`) / 锚点 #2 (`RTX 5090 NGX`) | 锚点 #4 (`ACO vs LLPC`, `guides=0 & 1`) | 准入处理 |
|---|---|---|---|---|
| **Legacy (`--fast-mode 0`)** | `differing == 0`, `max_ulp_diff == 0` | 对 `v0.1.2` / `v0.1.5 Legacy` `0 ULP` | `guides=0` `0 ULP`；`guides=1` `>= 120 dB` | 全阶段不动 Legacy 数值语义 |
| **阶段 2.0 修复项** | — | 不劣于 `v0.1.5 Fast vs Legacy` 曲线（`ΔPSNR >= -0.2 dB`） | **将 `guides=1` 从 `~33 dB` 拉回稳健区间（目标 `>= 50 dB`）** | ACO 默认发布硬前置 |
| **阶段 2 严格无损项** | Fast 逐位一致（`0 ULP`） | 与关闭态曲线完全重合 | 不扩大双编译器间距 | 最优先合入 |
| **阶段 2 相对无损项** | 稳态曲线 `PSNR_01 >= 60 dB`、`PSNR_tm >= 60 dB`、`p99 <= 0.003` | 对 `v0.1.5 Legacy` 曲线不劣于上游 `0.1.5 Fast` 地板超過 `0.5 dB` | `guides=0/1` 均不引入新病态分歧 | 仅容纳 E4M3 次正规数映射（如 `LT_E8RAW`）或单次 FP32 累加舍入差异 |
| **阶段 3 有损默认候选** | 单独记录单项 `Δms` 与 `ΔPSNR` 曲线 | **以锚点 #2（RTX 5090 官方输出）为主判据**：相对官方真值的稳态/DRS 曲线退化 `<= 0.5 dB`（内部对父版参考下限 `>= 50 dB`、`p99 <= 0.006`） | 同份 SPIR-V 下无编译器专属崩塌 | 须在 RTX 5090 参考数据到位后拍板默认开启项 |
| **超线高收益有损档（`--fast-mode 2` / `_lx` 级）** | 记录完整收益/损失比 | 超出默认预算但显著提速（如 `>= 3%` 整帧或 `>= 5%` 显存） | `non_finite == 0`，连续帧无闪烁/爆点 | 归入可选激进档（对标上游 `_lx`），不进默认 Fast |

### 5.3 `v0.1.5` 已核实的源码现状与数值边界（防重复施工清单）

下表路径均相对 `C:/Users/Kenny/Downloads/OpenRR/`（`asi-plugin @ 975c5f3`）：

| 路径 / 模块 | `v0.1.5` 已核实代码现状 | 阶段 2 / 3 执行决定 |
|---|---|---|
| **`C<=64` 的 `LT_UP8` 与 `LT_MI8`（上游已落地！）** | [`engine.cpp:286-296`](file:///c:/Users/Kenny/Downloads/OpenRR/gpu/src/engine.cpp#L286-L296) `U8()` 已对 `s.Co`（`enc1` 的整个 `mrg_w` 缓冲）及 `s.b.C <= 64` 的 `up_w` 追加 `qv_bytes` E4M3 权重；[`fastmode.cmake:5,12,23`](file:///c:/Users/Kenny/Downloads/OpenRR/gpu/fastmode.cmake#L5) 已默认开启 `dec_up_32_64_64`（[`onr_block_w1.glsl:140`](file:///c:/Users/Kenny/Downloads/OpenRR/gpu/shaders/include/onr_block_w1.glsl#L140) `32` 条 FP8 WMMA）、`dec_up_64_64_96`（[`onr_block_w2.glsl:98`](file:///c:/Users/Kenny/Downloads/OpenRR/gpu/shaders/include/onr_block_w2.glsl#L98) `48` 条 FP8 WMMA）与 `enc_stage_64_64_96`（[`onr_block_w2.glsl:154`](file:///c:/Users/Kenny/Downloads/OpenRR/gpu/shaders/include/onr_block_w2.glsl#L154) `64` 条 FP8 WMMA） | **保持上游现成实现，不再重复立项**；在其基础上测试下方的 `LT_E8RAW` 与 `C >= 96` 扩展 |
| **`LT_E8RAW` 次正规数直通宏** | [`onr_block_fast.glsl:288-294`](file:///c:/Users/Kenny/Downloads/OpenRR/gpu/shaders/include/onr_block_fast.glsl#L288-L294) 已内置 `#ifdef LT_E8RAW`（用原生字节直读替代 `ocp4(w)` 的 8 条位运算，仅影响指数域为 0、幅值 `< 2⁻⁶` 的编码），当前未在 `fastmode.cmake` 打开 | **阶段 2A 首测开关**：1 行宏即可验证在 `32_64_64`、`64_64_96` 上省去 `ocp4` VALU 后的净提速与相对无损曲线 |
| **`C >= 96` 的 `DEC_UP`（尚未启用 FP8）** | [`engine.cpp:295`](file:///c:/Users/Kenny/Downloads/OpenRR/gpu/src/engine.cpp#L295) 仅在 `s.b.C <= 64` 调用 `U8(b.up_w)`；`dec_up_96_128_128`（`TR` 体，`ENC_CP=128`）、`128_128_160`、`160_256_160` 仍走 [`onr_block_fast.glsl:508-530`](file:///c:/Users/Kenny/Downloads/OpenRR/gpu/shaders/include/onr_block_fast.glsl#L508-L530) 的 `put16` FP16 LDS 解包 + FP16 WMMA | **阶段 2A 扩展点**：放开 `U8(b.up_w)` 到全部 decoder 层，为 `TR`/`SPLIT=2` 体的 `DEC_UP` 补齐原生 FP8 并消除 `put16` FP16 往返 |
| **各级 `ENC_MERGE` 输出下采样合并（全部仍为 FP16！）** | 尽管 `enc1`（`enc_stage_64_64_96`）的 `b.mrg_w` 缓冲末尾**已经上传了完整 E4M3 编码**（[`onr_block_w2.glsl:160`](file:///c:/Users/Kenny/Downloads/OpenRR/gpu/shaders/include/onr_block_w2.glsl#L160)），但 `W2` 尾部的输出 merge（[`onr_block_w2.glsl:465-500`](file:///c:/Users/Kenny/Downloads/OpenRR/gpu/shaders/include/onr_block_w2.glsl#L465-L500)，每 subgroup `48` 条 WMMA）及 `C=96/128/160` 的输出 merge 仍把 `x` 以 FP16 写入 LDS 做 FP16 WMMA（与此同时 `XOUT2` 紧接着就把同一个 `x` 转成了 `e4m3x4` 写往 `TensorOutput`！） | **阶段 2B 核心高潜项**：复用 `XOUT2` 已算出的 `e4m3`（或按 `NR_QUANT_MODE=4` 量化），让 `ENC_MERGE` 直接吃 E4M3 激活 × `mrg_w` E4M3 权重，既把 `48` 条 FP16 WMMA 减半周期，又将 merge LDS 搬运减半；实测是否满足阶段 2 相对无损门禁（否则归入阶段 3） |
| **`C >= 64` 的 `FP8QV` vs `C = 32` 的 `dec_head_32_64`** | `FP8QV` 在 `ENC_C >= 64 && ENC_MERGE_IN == 0 && DEC_UP == 0` 默认开启（覆盖 `dec_dfn_64_64`、`enc2..enc5`、`dec2_1..dec4_1`）；而 **4K #3 大核 `dec_head_32_64_f_lt`（`1.267 ms`，`0 fp8 / 196 f16`）** 虽从 E4M3 arena 直接读 `pc.x`（[`onr_block_w1.glsl:228`](file:///c:/Users/Kenny/Downloads/OpenRR/gpu/shaders/include/onr_block_w1.glsl#L228)），但因 `W1` 默认开 `LT_Q16=1`（旋转后 `lr_keys` 为一般 FP16）与 `LT_VO=1`（CPU 预乘 `vo_values = wout_h * in_b_h` 为一般 FP16）未走 FP8 | **阶段 2C / 3 衔接项**：单独测试 `dec_head_32_64` 将 `lr_keys` / `vo_values` 做离线最优缩放 E4M3 量化（配合已在 E4M3 的 `pc.x`），按实测画质增量归入阶段 2 相对无损或阶段 3 |
| **`W1` 的 `LT_VO=1` 与 `guides=1` 分歧嫌疑面** | [`onr_w1_head.glsl:66-74`](file:///c:/Users/Kenny/Downloads/OpenRR/gpu/shaders/include/onr_w1_head.glsl#L66-L74) 在三个 `W1` 核默认开启 `LT_VO=1`，省去每 head 16 条 `wout` WMMA，但在 `guides=1` 下 `0.1.4/0.1.5` 出现 ACO vs LLPC `43.8 → 32.7 dB` 分歧 | **阶段 2.0A 首要排查对象**：保持 `LT_VO=1` 省算力框架不变，二分定位 `LT_VO`、`LT_SYM`、`LT_EXP/RCP`、`LT_ATTR_*` 与 NIR `nir_fp_exact` 交互点 |
| **上游实验档 `_lx`（`--fast-mode 2`）** | [`fastmode.cmake:32-35`](file:///c:/Users/Kenny/Downloads/OpenRR/gpu/fastmode.cmake#L32-L35) 新增 `LT_GELU=0.31`（单直线段近似）与 `LT_ENGINE_DEEP "2,4,2,4"`（隔帧复用深层，约省 8% 耗时但损失 `~5 dB`）；另 [`onr_block_fast.glsl:387`](file:///c:/Users/Kenny/Downloads/OpenRR/gpu/shaders/include/onr_block_fast.glsl#L387) 备有精度更高的二段折线 `LT_GELU4` / `LT_G4` | `LT_ENGINE_DEEP` 与单段 `LT_GELU` 不进默认 Fast；阶段 3 在 RTX 5090 真值下单独评估二段折线 `LT_G4` |

参考复用来源：[`coopmm.glsl`](file:///C:/Users/Kenny/Downloads/DLSSNR-AMD-mochizuki/windows/shaders/rdna4/include/coopmm.glsl#L26)（`:26` `16×16×16`、`:93` 权重 `ColumnMajor`、`:179` `MMA`、`:320` 起 `NR_QUANT_MODE = 4` 零 clamp 饱和转换）。

### 5.4 阶段 2.0 与阶段 2 的执行顺序

1. **2.0A（必做画质阻断修复）：根除 `guides=1` 下 Fast 模式 ACO vs LLPC `~32.7 dB` 分歧。**
   - 用统一 `32/32/8` + `parity-all-frames` 复现 `1080p guides=1` 逐帧分歧曲线，先按算子级二分（逐个将 `_lt` SPIR-V 替换为 `_f` 或在 `ACO strict` 下逐核回退 LLPC）精确定位是哪一个 `_lt` 核首次放大差异，再在该核内二分 `LT_DEFS_*` 宏（`LT_VO`、`LT_Q16`、`LT_SYM`、`LT_EXP`、`LT_RCP`、`LT_ATTR_TAB/LATE/D9`、`LT_POST_*`）与 [`prepare_capture.py`](file:///c:/Users/Kenny/Downloads/OpenRR/asi/build/mesa/prepare_capture.py) 的 NIR 舍入规则（§6.3.1 第 6 条）。修复要求不伤及 `ACO strict` 速度且双编译器 `guides=1` 曲线恢复稳健一致。
2. **2.0B（ACO 代码生成追平 LLPC）：攻克深层 `_fl_lt` 寄存器台阶与 `post` / `enc0_attr` 差距。**
   - 先设置 `ONR_RR_FLOW=0` + `ONR_RR_STAMPS=1` 按真实 `@idx` 采齐 **保留 `_f_lt` 着色器** 的 22 核精确串行基线（区分纯算子代码生成耗时 vs `flow_wait` 自旋调度放大）。
   - 核查 `enc_stage_96_128_128_fl_lt`、`dec_dfn_96_128_fl_lt`（`W2` `NT=6`）与 `C=128`（`TR`）为何在 ACO 上冲至 `240–256 VGPR`（对比 `192 VGPR` 即打平 LLPC 的 `dec_up_96_128_128`），结合 `max_waves` / 循环切片 / 寄存器生命周期收敛 VGPR 台阶，并核查 `post_f_lt`（`+0.147 ms @ 4K`）与 `d3d_enc0_attr_f_lt`（`+0.092 ms @ 4K`）的 ISA 差异。
3. **2A：`LT_E8RAW` 与 `C >= 96` `DEC_UP` 原生 FP8 补齐。**
   - 在全核 `ACO strict` 单栈下先测 `LT_E8RAW=1` 对现有 `32_64_64`、`64_64_96` 的单项提速与逐帧曲线增量。
   - 扩展 [`engine.cpp:295`](file:///c:/Users/Kenny/Downloads/OpenRR/gpu/src/engine.cpp#L295) `U8(b.up_w)` 至 `C >= 96`（统计新增固定权重显存，KB 级），打通 `dec_up_96_128_128`、`128_128_160`、`160_256_160` 的原生 FP8 `DEC_UP` 并消除 `put16` FP16 LDS 解包往返。
4. **2B：`ENC_MERGE` 输出合并原生 FP8 化（先打 `#5 enc_stage_64_64_96_f_lt`，再扩 `C=96/128/160`）。**
   - 先核实 §6.3.1 第 8 条（Mesa `nir_opt_algebraic.py` 中 `ONR_ACO_PRECISION` 与 `NR_QUANT_MODE = 4` 零 clamp `f2e4m3fn_satfn` 是否冲突，必要时按精确 `_lt` 核名放宽并用长度感知 ISA 直方图确认 `v_maxmin_num_f32 == 0`）。
   - 在 `enc_stage_64_64_96_f_lt`（`W2`）中复用 `XOUT2` 的 E4M3 输出（或零 clamp 转换）直接送入 `ENC_MERGE` 乘 `b.mrg_w` 尾部已有的 E4M3 权重；验证通过并测得正向收益后，统一推广至 `enc_stage_96_128_128`（`U8` 替换 `with_tiles` 或并存）及 `128/160`。
5. **2C：`#3 dec_head_32_64_f_lt` Arena 输入路径 `Q/V` FP8 验证 + 2D 默认组合回归。**
   - 针对 `dec_head_32_64_f_lt` 输入 `pc.x` 已在 E4M3 的优势，测试 `Q`（`lr_keys`）/ `V`（`vo_values`）带每列/每头缩放因子的 E4M3 量化收益与误差：达阶段 2 相对无损线则留阶段 2，否则转入阶段 3。
   - **阶段 2 收尾门禁检查点**：在 1080p / 1440p / 4K、Quality / Performance、`guides=0/1`、`wide`/DOF、DRS/reset、奇数尺寸上完成完整回归，并在同一份 SPIR-V 下复测一次 `ACO strict` vs `v0.1.5 Fast-LLPC` 端到端毫秒数，检验是否跨过默认交付门禁（`3.009 / 11.389 ms`）。

### 5.5 阶段 3（有损激活 FP8 + `hist_` 压缩）与失败实验留档

> [!IMPORTANT]
> **阶段 3 开工硬门禁：RTX 5090 官方参考数据（锚点 #2）就位**
> 开工前必须先拿到 [`RR_Contrast`](https://github.com/TheAutomatic/RR_Contrast)（`09310c8`）在 RTX 5090 上对同一套 `cases.json` 跑出的官方 `nvngx_dlssd.dll (310.9.1.0)` 输出，建立「`v0.1.5 Legacy` vs 5090」与「`v0.1.5 Fast` vs 5090」两条基准曲线。阶段 3 的所有有损量化均同时考核「相对同栈父版增量」与「相对 RTX 5090 官方真值的净偏离变化」。

- **主力矩阵乘有损激活 FP8（逐项独立开关、独立提交）**：
  1. **MLP `fc1` / `fc2`**（通吃 `W1`、`W2`、`TR` 全部核心，占剩余 `1,595` 条 Top-5 FP16 WMMA 的最大份额）：`x1` 在 `RESID1` 后零 clamp 饱和转 E4M3 进 `fc1`，`GELU` 后转 E4M3 进 `fc2`；严格按 §6.4 第 1 条核算新增转换 VALU 周期是否小于省下的 `8 × N_WMMA` 周期，并监控 `W1`/`W2` FP32 累加器是否触发 VGPR 跨台阶。
  2. **`enc0_stage_32_64_f_lt`（#1 大核）的 `ENC_EMBED`（`ew` / `fw`）与块内 `Q/V`**：评估抖动属性记录及 embed MLP 的 E4M3 路径。
  3. **`W2` 的 `wout` 投影与 `DEC_UP` skip 融合后的 `Q/V`**（`dec_up_32_64_64`、`dec_up_64_64_96` 中 `fma(skip, sc, U)` 之后的激活重量化）。
  4. **二段折线 `LT_G4` GELU**（对比上游 `_lx` 的粗粒度单段 `LT_GELU=0.31`）。
- **历史缓冲独立显存压缩（4K 理论 `−149.3 MB` / `−142.4 MiB`，目标 `789 MB → ~640 MB`）**：
  - [`engine.cpp:516-524`](file:///c:/Users/Kenny/Downloads/OpenRR/gpu/src/engine.cpp#L516-L524) 当前分配两份 `internal_`（输出 `1.5×` 宽高）的 `8 B/px` `VK_FORMAT_R16G16B16A16_SFLOAT` 历史缓冲。
  - [`onr_post.glsl:24`](file:///c:/Users/Kenny/Downloads/OpenRR/gpu/shaders/include/onr_post.glsl#L24) 的 `post_encode = 0.36 * log10(1 + 100 * y)` 在 HDR `y > 5.985` 时编码值 `> 1` 且可出现负值：必须按真实 HDR 动态范围设计 `4 B/px`（如 `R10G10B10A2_UNORM` 缩放偏移或 `R11G11B10_UFLOAT` / 自定义打包）编码与还原，保留 Legacy `8 B/px` 不变，同步适配 `alias_image` 与 `post` 采样/直读/写入及模式切换时的历史重置。
- **留档纪律**：所有实验（含未达标实验）均记录开关、SPIR-V/`.nrp` 指纹、`min / steady_end / drs_min` PSNR、线性 RGB `p99/max`、饱和比例、三档 GPU 耗时与显存；整帧提速 `>= 3%` 或显存下降 `>= 5%` 但超默认画质线的方案归入可选激进档候选。

### 5.6 标准验证命令模板（`v0.1.5` 口径）

#### 1. 逐帧画质曲线对比（统一 `32/32/8` + `parity-all-frames`，同时覆盖 `guides=0` 与 `guides=1`）

```powershell
$bench = 'C:/Users/Kenny/Downloads/OpenRR/build/asi/openrr_bench.exe'
$candStage = 'C:/Users/Kenny/Downloads/OpenRR/build/asi/stage'
$baseStage = 'C:/Users/Kenny/Downloads/OpenRR/build/baselines/v0.1.5-aco-base' # 同栈开关 OFF 基线（或 v0.1.5 Legacy）
$outRoot = "C:/Users/Kenny/Downloads/OpenRR/docs/test-evidence/runs/$(Get-Date -Format yyyyMMdd-HHmmss)-quality"

foreach ($guides in @(0, 1)) {
    foreach ($sz in @(@(1280,720,1920,1080,'1080p'), @(1706,960,2560,1440,'1440p'), @(2560,1440,3840,2160,'4k'))) {
        $dir = "$outRoot/$($sz[4])-g$guides"
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        & $bench --runtime "$candStage/plugins/openrr-amd.asi" --data-dir "$candStage/openrr-amd" `
            --baseline-runtime "$baseStage/plugins/openrr-amd.asi" --baseline-data-dir "$baseStage/openrr-amd" `
            --compiler aco-strict --align-aco-dispatch 1 --guides $guides --fast-mode 1 `
            --rw $sz[0] --rh $sz[1] --ow $sz[2] --oh $sz[3] `
            --warmup 32 --frames 32 --drs-frames 8 --parity-all-frames 1 --out $dir 2>&1 | Tee-Object "$dir/console.log"
    }
}
```

#### 2. 端到端纯测速配对交替 A/B（关闭逐帧回读：中间筛选 `32/120/64` × 2–3 轮，最终汇总 `48/240/64` × 3 轮；仅临界 `<0.5%` 时补至 5–6 轮）

```powershell
# 单跑或同进程 A/B 纯测速时严禁开 --parity-all-frames 或 ONR_RR_STAMPS：
Remove-Item Env:ONR_RR_STAMPS, Env:ONR_RR_FLOW -ErrorAction SilentlyContinue
& $bench --runtime "$candStage/plugins/openrr-amd.asi" --data-dir "$candStage/openrr-amd" `
    --baseline-runtime "$baseStage/plugins/openrr-amd.asi" --baseline-data-dir "$baseStage/openrr-amd" `
    --compiler aco-strict --align-aco-dispatch 1 --fast-mode 1 `
    --rw 2560 --rh 1440 --ow 3840 --oh 2160 --warmup 48 --frames 240 --drs-frames 64
```

#### 3. 串行化逐核精确计时（`ONR_RR_FLOW=0` + `ONR_RR_STAMPS=1`，严格按 `@idx` 解析）

```powershell
# 保留全部 *_fl_lt.spv 不动，通过 ONR_RR_FLOW=0 强制深层走带 barrier 的 _f_lt 路径：
$env:ONR_RR_FLOW = '0'
$env:ONR_RR_STAMPS = '1'
& $bench --runtime "$candStage/plugins/openrr-amd.asi" --data-dir "$candStage/openrr-amd" `
    --compiler aco-strict --fast-mode 1 --rw 2560 --rh 1440 --ow 3840 --oh 2160 `
    --warmup 48 --frames 600 --drs-frames 0 --out "$outRoot/stamps-4k"
Remove-Item Env:ONR_RR_STAMPS, Env:ONR_RR_FLOW -ErrorAction SilentlyContinue
# 解析 openrr_candidate.log 时，必须将 `stamped dispatches:` 中的 `kernel.spv@idx` 映射到 `timestamp intervals` 的 `intervals[int(idx)]`！
```

---

## 6. RDNA4 (`gfx1201`) FP8 硬件代价模型与编译器避坑指南（来自 DLSSNR-AMD 实测补充，2026-11-10）

> 以下为 `DLSSNR-AMD` 团队在 RX 9070 XT（`gfx1201`）上针对光线重建（NR）网络实测总结的硬件事实、量化写法、编译器（ACO / LLPC）避坑指南与测量规范。在推进阶段 1.5 收尾及阶段 2 / 3（原生 FP8）时，将其作为离线静态筛选与代码实现的指导准则。

### 6.1 核心结论与硬件事实

1. **RDNA4 上 FP8 WMMA 的矩阵吞吐是 FP16 的两倍（周期减半）**：
   - 指令形状虽然同为 `16×16×16`、指令条数相同，但每条指令占用的硬件周期：**FP8 为 8 周期，FP16 为 16 周期**（Mesa ACO `src/amd/compiler/aco_statistics.cpp` `gfx12` 分支注释：*"Half the cost of GFX11, int4/8 and (b)f8 twice as fast as (b)f16"*；微基准实测 7.4–8.1 周期）。
   - FP8 的首要收益是**省掉一半矩阵单元周期**，而非仅靠权重显存带宽减半。
2. **FP32 累加本身不花额外的矩阵周期**：
   - `v_wmma_f32_16x16x16_f16` 与 `v_wmma_f16_16x16x16_f16` 同为 **16 周期**；FP8 只有 FP32 累加（`v_wmma_f32_16x16x16_fp8_fp8`），为 **8 周期**。
   - 每步 `acc = MC(MH(acc))` 的 VALU 爆炸是用 FP32 累加器去模拟 FP16 舍入的代价，而非 FP32 累加本身慢；若接受 FP32 累加则不要逐步截断。
   - FP32 累加器每个 tile 占 8 VGPR（FP16 累加占 4 VGPR），属于 tile 划分与寄存器预算问题：若单 wave 同时握 6–10 个输出 tile 导致跨过驱动 VGPR 分配台阶（LLPC / 驱动分配粒度为 24：**192 VGPR = 8 wave/SIMD，216 VGPR = 7 wave，240 VGPR = 6 wave，241+ VGPR = 5 wave**），应调整单 wave 同时持有的输出 tile 数（或 K 方向分块），并以驱动最终报告的 VGPR 数为准。
3. **WMMA、VALU 与 Load 回写在同一个 SIMD 上完全不重叠（串行相加模型）**：
   - AMD `amd_matrix_instruction_calculator` 对 RDNA4 所有 WMMA/SWMMAC 均标 `coexec False`；同一 SIMD 上即使跨 wave（一组专做 WMMA、一组专做 VALU）时间也直接相加，wave specialization 在 `gfx1201` 上不提供额外吞吐。
   - 仅 `v_exp` 等 TRANS 指令能盖住约一半 WMMA 周期；SALU（基本免费）和 LDS 完全重叠；所有普通 VALU（`fma` / `add` / `mul` / `max` / `maxmin` / `fma_mix` / `pk_fma` / `v_cvt_pk_fp8_f32`）均与 WMMA 串行。
   - VMEM / LDS load 数据写回 VGPR 同样占用 SIMD 发射周期且不与 WMMA / VALU 重叠：`buffer_load_b128` 约 **5.2 周期**，`b64` 约 **4 周期**，`ds_load_b64` 约 **1.6 周期**。FP8 权重让 load 从 `dwordx4` 变成 `dwordx2`，省下的主要是这部分 **VGPR 回写发射周期**。
   - **静态周期估算公式**：
     $$\text{Kernel 周期} \approx \text{VALU 周期} + 8 \times N_{\text{WMMA,fp8}}\text{（或 } 16 \times N_{\text{WMMA,f16}}\text{）} + \text{Load 回写周期}$$
     每条 FP8 WMMA 省下 8 周期，而新加的转换 / clamp 是一条一周期（VOPD 可成对）。若激活重量化（requant）多出来的 VALU 周期超过省下的 $8 \times N_{\text{WMMA}}$，kernel 就会变慢；控制好重量化开销，即使在 `C=32` 这种 VALU 占比约一半的层也能取得正向收益。
4. **指令周期速查表（Wave32）**：

| 指令 / 操作 | 周期（每条，Wave32） | 备注 |
|---|---|---|
| `v_wmma_f32_16x16x16_f16` / `_bf16` | 16 | 256 MAC / 16 cyc |
| `v_wmma_f16_16x16x16_f16`（f16 累加） | 16 | 与 f32 累加周期相同 |
| `v_wmma_f32_16x16x16_fp8_fp8`（及 bf8 组合） | 8 | 512 MAC / 8 cyc（实测 7.4–8.1） |
| `v_wmma_i32_16x16x16_iu8` | 8 | 同上 |
| `v_wmma_i32_16x16x32_iu4` | 每 MAC 约为 fp8 的 2 倍速率 | 实测 sustained：Vulkan 657 vs 312 TOPS |
| `buffer_load_b128` / `b64` / `ds_load_b64` 回写 | ~5.2 / ~4.0 / ~1.6 | 占 SIMD 发射周期，不与 WMMA/VALU 重叠 |

5. **VALU 搬上 WMMA 的盈亏线与调度特性**：
   - FP8 WMMA 每周期 512 MAC，VALU（VOPD/packed）约 64 MAC，共用同一发射口；把线性运算从 VALU 挪到 WMMA 只有在至少 **1/8 的 MAC 有用**时才划算（FP16 WMMA 为 **1/4**），用全 1 矩阵求和通常更贵。
   - Wave 调度为**最老优先（oldest-first）**：persistent workgroup 中最后启动的 wave 可能被饿死（早启动 WG 6.9 µs vs 最后启动 126 µs）。

---

### 6.2 FP8 量化实现规范（对接阶段 2 / 阶段 3）

1. **在生产者 epilogue 量化一次，消费者直接吃 FP8**：
   - 上一层结果在自己的 epilogue（累加器仍为 `f32` 时）直接转成 `e4m3` 写入 arena，下一层直接加载 FP8 片段，不在消费端循环内重复解压或转换（尤其是被 Q/K/V 或多个输出 tile 同时消费的激活）。
   - 阶段 2 优先做输入已在 E4M3 arena 的 `DEC_UP` 与 `merge`；阶段 3 的 `fc1` / `O` 等应优先考虑将量化前移到上一层 epilogue。
2. **从 `f32` 直接转 `e4m3`（一次舍入，避免 `f16` 往返）**：
   - `gfx12` 的 `v_cvt_pk_fp8_f32` 只接受 `f32` 源（一次转两个，第二对用 `op_sel` 写高半）。
   - 累加器本来就是 `f32` 时，直接 `f32 → e4m3` 一次舍入；不要写 `f32 → f16 → f32 → e4m3`（多一次舍入 + 多两条转换指令）。
   - RADV/NIR 会自动折叠 `float(float16_t(x))` 往返，但 **LLPC 不会删**（手动去掉后 Windows 上曾测得 1080p −3.6%、4K −3.9%）。注意该改动可能改变舍入边界，Legacy 路径须保持不变以遵守逐位一致门禁。
3. **饱和处理（`e4m3` 无 `Inf`，`> 448` 的有限 `f16` 裸转会变 `NaN` 污染整个累加器 tile）**：
   - **ACO 路径（阶段 1.5 接入后主力方案，`coopmm.glsl` `NR_QUANT_MODE = 4`）**：
     利用硬件 `MODE` 寄存器 `FP16_OVFL` 位饱和。ACO 会把普通 `f2e4m3fn_sat` 降成 `v_minimummaximum_f32 + v_cvt_pk_fp8_f32`（代价同手写 clamp）；**只有当 `nir_opt_algebraic` 匹配到 `f2e4m3fn_sat(bcsel(feq(fabs(x), inf), NaN, x))` 形状（即 GLSL 写 `isinf(v) ? NaN : v` 再做饱和转换）时，才会生成不带 clamp 的 `f2e4m3fn_satfn`**（0 条 `v_maxmin_num_f32`，仅发 `s_setreg` + `v_cvt_pk_fp8_f32`）。每次修改后必须用编码长度感知的 ISA 直方图工具核实 `v_maxmin_num_f32` 是否真正降为 0。
   - **打包 FP16 路径（`NR_QUANT_MODE = 5`）**：
     当值已经是 `f16vec2` 时，用 `v_pk_max_f16 / v_pk_min_f16` 两条打包指令 clamp 一对，不写 `MODE` 寄存器。
   - **LLPC 路径（Windows 驱动编译器）**：
     LLPC 上**没有免费的 `e4m3` 饱和**（`saturatedConvertEXT` 会编成 `clamp + class + cndmask`，比手写 clamp 更慢）。在 LLPC 上应按 4 个一组使用 `fe4m3vec4(vec4)` 转换（一次 `MODE` 写 + 两条转换进一个 dword，参考 `DLSSNR-AMD-mochizuki/windows/build/quad_quant_glsl.py`）；注意驱动 LLPC 的 `true16` 模式会让 4 宽转换多出 `v_bfi` 合并，拆成两对还是 4 宽需按 kernel 实测。
4. **片段加载写法避坑**：
   - **LLPC 大 shader 逐字节拼装问题**：LLPC 在大 shader 里可能把从内存读的 `e4m3` 片段拆成逐字节 `v_perm 0x3020104` 并提前等待 load；改用**一个 `uvec2` + `unpack8`** 填入片段即可消除（参考 `NR_FRAG_Q`）。
   - **需要 gather / 转置的 `e4m3` 片段加载（A ColumnMajor / B RowMajor）**：从普通 storage buffer 读会退化成每 lane 8 条 `buffer_load_u8`；必须通过 **`buffer_reference`（64 位全局地址）** 读取，编译器才会生成单条硬件转置加载指令 `global_load_tr_b64`（参考 `NR_VBDA`）。
   - **标量 `e4m3 → f32`**：不要逐个字节转，按 `fe4m3vec4` 一次转 4 个（两条 `cvt_pk`）。

---

### 6.3 编译器（ACO 与 LLPC）专项避坑清单

#### 6.3.1 Mesa ACO（阶段 1.5 预编译管线）
1. **避免 FP8 片段进入 `phi` 节点**：`if (live) coopMatLoad(frag) else frag = 0` 会让 ACO 对每个 dword 插入 4 条 `v_perm_b32` 逐字节拷贝；应将整块与尾块拆成两份无分支/独立分支代码。
2. **Wave 内一致的 `if` 被展平**：ACO 常把一致分支展平为逐元素 `v_cndmask`，小块上 `[[dont_flatten]]` 未必生效，需结合实际调度实测。
3. **`tid / 32u` 标量化**：NIR 会将 `tid / 32u` 视为 divergent，导致后续 tile 判断全部落入 VGPR；用 `subgroupBroadcastFirst(tid / 32u)` 强制标量化。
4. **全展开循环内的运行时 tile 守卫**：若值来自两条路径之一，在展开体里加守卫会制造大量 `phi` 导致指令数与 VGPR 暴涨（例如 192 → 256 VGPR）；只有纯 store 副作用的守卫安全，否则应把循环复制两份。
5. **循环回边预取失效**：若循环 `phi` 携带 load 出来的寄存器，ACO 会在回边插 `s_wait_loadcnt 0` 破坏跨迭代预取；需跨迭代预取的小循环用固定 trip count + `[[unroll]]`（注意大循环展开前必须核对代码体积与指令缓存影响）。
6. **NIR 改变舍入（关乎 Legacy 逐位一致门禁与画质排查）**：
   - `f2f16(fadd(...))` 会被融合成 `v_fma_mix`（一次舍入），`precise` 无法阻止；
   - 删除死分支可能连带触发 `f2f32(f2f16(x)) → x` 消除一次 `f16` 舍入；
   - `f16vec3(textureLod(...))` 会折叠为 `image_sample ... d16`（float 纹素为 RTZ，UNORM 纹素为 RNE）；
   - RDNA4 上对 `R16G16B16A16_SFLOAT` storage image 执行 `imageStore` 硬件使用 RTZ。
7. **FP8 WMMA 的 K 顺序敏感**：K 维置换会改变浮点累加顺序与最终输出。
8. **捕获补丁 `ONR_ACO_PRECISION`（`nir_fp_exact`）与零 clamp 饱和转换（已在 Mesa `26.3.0` 核实通过）**：经核查固定 Mesa `26.3.0` 的 `src/compiler/nir/nir_opt_algebraic.py:3391-3415`，`f2e4m3fn_sat(isinf ? NaN : x) → f2e4m3fn_satfn` 规则为严格保值变换（无 `~` 前缀），**不被 `ONR_ACO_PRECISION=global` 的 `nir_fp_exact` 阻断**；且对 20 个深层核实测 `--precision local` 机器码字节零变化。每次引入 `NR_QUANT_MODE = 4` 后仍须用编码长度感知的 ISA 直方图确认 `v_maxmin_num_f32 == 0`。另须特别注意主机端 `b.mrg_w` 原始 8-bit 字节为 `sm_86` E4M3（`0x7f = 480`），送入 RDNA4 OCP E4M3 WMMA 前必须经 `ocp4()` 或主机端转换，防止 `0x7f` 被当作 `NaN`。

#### 6.3.2 Windows LLPC（历史经验留档；阶段 2/3 全程采用全核 ACO 单栈，不做算子级 LLPC 交叉调参）
1. **ACO 与 LLPC 旋钮不可盲目互通**：在 ACO 上正向的占用率上限或重映射在 LLPC 上可能严重退化；对比两编译器时必须控制输入为**同一份 SPIR-V**，每个旋钮分别 A/B。
2. **打包 `f16vec2` 运算**：LLPC 会把写成逐分量的 `f16` 对运算标量化，甚至把 `f32(h) * f32(h')` 缩成 `v_mul_f16`；统一用打包的 `f16vec2` 书写。
3. **反汇编器失步警告**：RADV 文本反汇编器在遇到 8 字节转换指令（如 `v_cvt_pk_fp8_f32`）时会失步（甚至显示为假的 `invalid + v_cndmask`），直接按文本行数统计会漏数超过 10% 的指令；**必须使用按指令编码长度解析的直方图工具统计 ISA**。

---

### 6.4 性能测量与验证补充规范

1. **静态周期与资源五项核对表（每次对比 FP16 vs FP8 构建必查）**：
   对同一层、同一形状、同一 tile 划分的 FP16 与 FP8 构建，跑 GPU 前先列出：
   - ① **WMMA 条数**（f16 按 16 周期计，fp8 按 8 周期计）；
   - ② **VALU 条数及明细**：转换（`v_cvt_*`）、clamp（`v_maxmin*` / `v_pk_max/min`）、NaN/class 检查、`v_perm`/`v_bfi` 拼装各多少；
   - ③ **`s_setreg` 条数**；
   - ④ **驱动实际 VGPR 数**、每 SIMD 的 wave 数（关注 192 / 216 / 240 台阶）、是否有 scratch；
   - ⑤ **Load 条数与宽度**（`b128` 5.2 周期 vs `b64` 4 周期）。
   先用 `VALU + 8/16 × WMMA + Load回写` 核算预期周期：若新增 VALU 超过省下的 `8 × WMMA`，优先修量化写法；若 VGPR 跨台阶，优先调 tile 划分。
2. **持续负载与功耗墙效应**：
   - RX 9070 XT 在 WMMA 跑满时运行在默认功耗上限内，持续负载下帧耗时近似与**能耗**成正比。单纯消除 `s_waitcnt` 等空闲等待不省能量（核心频率会微降 10–20 MHz 抵消单算子改善），而减少 **WMMA 周期、VALU 指令、数据搬运** 才能真正降低持续整帧耗时。因此任何算子级提速都必须以**整帧持续 A/B** 确认为准。
   - 警惕短跑落在开局频率冲高段：检查 120 帧稳态窗口中**前半段与后半段中位数是否一致**；筛选出最终默认组合后，做一次长预热（如 500–1500 帧）+ ABBA 交替验证。
3. **测试脚本防假阳性**：
   - 计时运行必须关闭逐帧回读与哈希计算，保证 GPU 满载；
   - 质量/哈希运行前**先删除旧输出文件**，并强制校验进程退出码为 `0` 且输出文件非空、时间戳更新，防止运行崩溃或 hang 后脚本误读上一次遗留的旧文件算出“通过”哈希。
   - `FP8PROTO` 模式 2 等假数据/零数据会让 WMMA 运行频率偏高约 25%，仅作粗糙探针，不可替代真实权重与真实激活数据的测量。
   - **逐核 `ONR_RR_STAMPS` 解析硬规矩**：必须按 `stamped dispatches:` 输出的 `kernel.spv@idx` 显式取 `intervals[int(idx)]`（偶数槽为算子耗时，奇数槽为间隔），严禁按 `0..N-1` 顺序配对；且需要串行化深层核时必须设 `ONR_RR_FLOW=0`，不得采用删除 `*_fl_lt.spv` 的做法（否则 `K.has("*_fl")` 仍为真，只会静默回退到 Legacy `*_fl.spv` 并发路径）。
   - **逐帧画质曲线防误判**：严禁用单点末帧 PSNR 下结论；跨构建比较必须固定同构参数（含 `--align-aco-dispatch` 与 `--guides`），并以 `parity-all-frames` 的 `min / steady_end / drs_min` 完整曲线为准。
