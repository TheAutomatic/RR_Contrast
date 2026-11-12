# OpenRR 能从 mochi 借鉴的性能优化（对照 `7bcd355`）

范围：mochi `dev/0.0.8.4` @ `5bebcf8`（`/workspace/d3d12-audit-5bebcf8`）vs OpenRR tip `7bcd355`（`/workspace/openrr/patches-7bcd355/src`）。
参考：`perf-audit-review/2026-10-19-mochizuki-performance-audit*.md`、`d3d12-audit-5bebcf8-report.md`、`ACO-D3D12-HOOK-PLAN.md`、`pr-review-7bcd355.md`。

## 先说清楚架构差

两边都是「用 ACO 机器码跑 NVIDIA `nvngx_dlssd.dll` 的权重」，但图不一样：

| | mochi NR | OpenRR |
|---|---|---|
| 模型 / DLL | DLSS 5 NR 模型，来自 `nvngx_dlssnr.dll` 310.9.16（`CC_SMALL_FP8` 等，Swin/ViT transformer） | DLSS RR 模型（rr2），来自 `nvngx_dlssd.dll` 310.9.1（编码器-解码器 + DFN 头） |
| 网络图 | 按 DLL 里的网络描述生成 frame plan（`nr_graph.cpp`），约 152 层、上百次小 dispatch | 按 rr2 结构手写 `engine.cpp`（enc0 / enc_stage / dec_* / post），权重按偏移表从 DLL 读，约 20–40 次 dispatch |
| D3D12 绑定 | 每帧上传 4 KB table，根签名里 1 个 dword + 1 个 root UAV（`nr_d3d12_asd_net.cpp:1161-1174`） | 每次 dispatch 写 62 个 root constant（`d3d12.hpp:30`，`d3d12.cpp:820-826`）+ 自己的 descriptor heap |
| 屏障 | `no_barrier_after` / tchain / persistent DS·UPS（`nr_graph.cpp:4967+`） | flow 模式用 row-flag 替代小层屏障（`engine.cpp:548-550`，`runtime.cpp:834`） |
| Direct 读纹理 | D3D12 ASD 直通；Vulkan 桥有共享纹理路线 | 已有 `d3d_ingest` / `d3d_pack` 直接读游戏纹理（`runtime.cpp:757+`、`:2403`） |

所以不能把 mochi 的 tchain、4 KB table、amdxc64 hook 原样搬过来。下面只列**不改 OpenRR 架构、有文件证据、值得做或明确不做**的项。

---

## 值得做的短名单

### A. 分配时用 `D3D12_HEAP_FLAG_CREATE_NOT_ZEROED`（仅限马上会被填的 buffer）
**mochi**：注释写明靠 `HEAP_FLAG_NONE` 的零初始化保证历史/中性 MV 初值（`nr_d3d12_asd_backend.cpp:521-523`，`nr_d3d12_asd_net.cpp:14`）。
**OpenRR**：`create` 一律 `HEAP_FLAG_NONE`（`d3d12.cpp:62-67`）；`make()` / guides 分配后都会 `fill`（`engine.cpp:142-146`，`runtime.cpp:767-770`；`7bcd355` 已改成 GPU `clear_range`）。

**改法**：对「创建后立刻 `fill`」的路径（`make`、guides、clear_range 覆盖的范围）加 `CREATE_NOT_ZEROED`；历史图如果依赖「未写过的那一帧是 0」则保持 `NONE`（OpenRR 的 `hist_` 第一帧有 `fresh_` 路径，可单独核对）。
**收益**：只在 resize / 首次建网时省掉驱动清零大块 DEFAULT 堆的开销（几十 MB 级，约 0.1–数 ms 卡顿），**不是每帧**。
**风险**：低，前提是所有读之前必有 fill；和微软「almost always zeroed」的表述一致。
**是否已在 `pr-review-7bcd355`**：否。和待修第 4 条（未绑定 fill 合并提交）互补：那边减 submit 次数，这边减驱动清零。

### B. 菜单打开时不要每帧读 `OptiScaler.ini`
**mochi**：性能审计里默认日志级别和每条消息开文件都是 CPU 成本来源（`2026-10-19` 审计 §3）。
**OpenRR**：`rr_pe_menu.cpp:549` 每帧 `build()` → `read_optiscaler_menu_scale` → `GetPrivateProfileString`（`rr_pe_config.cpp:289-301`）。

**改法**：缓存 scale，分辨率或 ini mtime 变化再重读（最多 500 ms 一次）。
**收益**：菜单打开时渲染线程少一次同步磁盘读，约几十 µs～0.1 ms/帧（视磁盘而定）。
**风险**：无。
**已在待修第 6 条。**

### C. 未绑定 `fill`：一次 resize 一次提交
见 `pr-review-7bcd355` 待修第 4 条。mochi 建网时权重拷贝是一次 `CopyBufferRegion`（`nr_d3d12_asd_net.cpp:1000-1006`），不是每个 buffer 单独 submit+wait。OpenRR 现在每个 `make()` 都 `begin_own`→`submit_own`→`wait_own`（`d3d12.cpp:679-700`、`:325-337`）。
**收益**：换分辨率 / 首次建网时，游戏线程少几十次 GPU 等待，估计十几 ms 级尖峰。
**已在待修清单。**

### D. 退休跟踪表不要只增不减 + Evaluate 别误跳帧
见待修第 1、2、5 条。这些不是「从 mochi 抄来的优化」，但是当前最大的帧时间/正确性坑：跟踪表泄漏会抬高 CPU；错误的 Success-不写输出会让画面停住（体感比任何微优化都大）。
**已在待修清单。**

### E. 日志默认别每条 `fflush`；热路径少打 log
**mochi**：审计确认发行包开 `LogToFile` 时每条消息查大小+打开+写入+关闭，详细诊断约 +0.10 ms CPU。
**OpenRR**：`rr_pe_log.cpp:118-126` 每条 `log` 都 `fflush`；runtime `g_log` 也有 `fflush`（`runtime.cpp:109`）。默认是否写文件取决于配置，但一旦打开就是同步刷盘。

**改法**：默认改为按行缓冲或定时 flush（例如 100 ms / 或仅错误级别立即 flush）；热路径（每帧 Evaluate）保持现有「每 30 帧一条」节奏，不要加新的每帧日志。
**收益**：开日志时约 0.05–0.15 ms CPU/帧；关日志时为 0。
**风险**：崩溃前最后几行可能丢，可用 crash handler 里再 flush 一次抵消。
**不在待修清单，可单开。**

### F.（可选，需测量）引擎里再收紧无 RAW 危害的屏障
**mochi**：`no_barrier_after` / tchain 在图编译期标出可省略的屏障（`nr_graph.cpp:4967+`，运行时 `nr_d3d12_asd_net.cpp:1173-1174`）。
**OpenRR**：已有同类思路——flow 模式小层用 `_fl` 内核 + row flag，跳过部分 `d.barrier()`（`engine.cpp:88-91,103-106,123-125`）；非 flow 时 `record_net` 大约十来次全局 UAV barrier（`engine.cpp` 里 9 处 `d.barrier()`）。

**改法**：不要搬 tchain。在现有 `engine.cpp` 上对「写集合 ∩ 下一跳读集合 = ∅」的相邻 `run`/`dispatch` 标 `skip_barrier`，用 PIX / `per_dispatch_` 时间戳做前后对比。
**收益**：未测。乐观估计每省一次全局 UAV barrier 在 RDNA4 上可能是几十 µs；若能少 3–5 次，大约 0.1–0.3 ms。必须测，不能拍脑袋。
**风险**：标错会静默坏帧。先只动有注释保证的段，并加 harness。
**不在待修清单；优先级低于 A–E。**

---

## 已经对齐或已经超过 mochi 的（不用再搬）

1. **Direct 读游戏纹理**  
   OpenRR 的 `d3d_ingest`/`d3d_pack`（`runtime.cpp:1891+`、`:2403`）就是 mochi 性能审计里给 Vulkan 桥提的「共享纹理、少一次 buffer↔image」方向。D3D12 Direct 下 OpenRR 已经在走这条快路；不要再去搬 mochi 的 Vulkan 桥共享纹理方案。

2. **历史用 RGBA16F**  
   OpenRR `hist_img_` 固定 `R16G16B16A16_SFLOAT`（`engine.cpp:449-450`）。mochi 的 `[Network] History Precision` 可选 f16（`nr_d3d12_asd_net.cpp:820-836`）在 OpenRR 这边已经是默认。

3. **fast 模式显存打包**  
   OpenRR `sub()` 把 attr/scratch/pool 叠在同一块里（`engine.cpp:456-496`），和 mochi arena 复用是同一类做法，已经在做。

4. **小层屏障省略（flow）**  
   见上。精神上等同 tchain，实现贴合 OpenRR 自己的图。

5. **PSO 缓存**  
   `bound_ != &k` 才 `SetPipelineState`（`d3d12.cpp:822`）。mochi 每层都设 PSO（层本来就不同）。持平。

6. **权重一次上传**  
   OpenRR `upload_stage` 在 `Engine::init`（`engine.cpp:274-282`）；mochi `CopyBufferRegion` 一次后退休 upload（`nr_d3d12_asd_net.cpp:1000-1183`）。持平。

7. **overflow / 上传环**  
   OpenRR `7bcd355` 已改成按需增长、满了跳帧不抛（待修第 2 条还要改语义）。mochi 的 upload 环不够就再分配一块（`nr_d3d12_asd_net.cpp:963-976`）。不必再抄。

---

## 明确不建议搬的

| 想法 | 为什么不搬 |
|---|---|
| amdxc64 / UMD hook 换 ELF（`ACO-D3D12-HOOK-PLAN.md`） | OpenRR 已走 `CreateComputePipelineFromElf`（支持的 PAL 路径）。UMD hook 绑驱动哈希、脆弱，和「MinGW/WSL ACO bake + 正式 API」的产品路线冲突。 |
| 4 KB dispatch table + root UAV 绑定模型 | 要重写全部着色器的资源绑定和 ELF 用户数据映射；和现有 62 dword root constant 方案是两套体系。收益主要是省命令流，相对 ~3 ms 的 RR 本身很小。 |
| NR 模型图的 tchain / persistent DS·UPS | 图都不是同一张。OpenRR 用自己的 flow/`_fl` 即可。 |
| Vulkan 桥「共享 D3D12 纹理 → VkImage」 | 那是在修 mochi **桥接**多出来的 0.28–0.31 ms（审计结论）。OpenRR Direct 不走这条桥；Vulkan host 已有 direct image 路径。 |
| VRAM ledger（`nr_vram_ledger.*`） | 记账/菜单展示，不省显存。OpenRR 已有 `vram()` 预算日志（`runtime.cpp:1088`）。 |
| 发行包默认开详细文件日志 | mochi 审计证明这是负优化。OpenRR 保持默认关、少打即可（见短名单 E）。 |

---

## 和 `pr-review-7bcd355.md` 待修 9 条的关系

| 短名单 | 待修条目 | 说明 |
|---|---|---|
| A CREATE_NOT_ZEROED | 无 | **新增**建议 |
| B 菜单 ini 缓存 | 第 6 条 | 已有 |
| C 未绑定 fill 合并提交 | 第 4 条 | 已有 |
| D 跟踪/跳帧 | 第 1、2、5 条 | 已有；正确性优先于微优化 |
| E 日志 fflush | 无 | **新增**建议 |
| F 引擎屏障再收紧 | 无 | **可选**，必须先测 |

建议落地顺序：先把待修 1–5、6 做完（正确性 + 已知卡顿）→ A + E → 有 PIX 数据后再考虑 F。

---

## 算子 / 内核层（补充，对照 mochi `5bebcf8` 着色器与 `nr_graph.cpp`）

上一版只看了 host / 绑定 / 屏障 / 日志，这里补算子侧。**INT4 / 量化权重路径按用户要求整体不纳入**（需要模型拟合，复杂度过高），下文不做任何 INT4 相关建议。

### 先说结论
两边的网络**不是同一张图**，mochi 的大部分算子优化无法原样移植：
- mochi 跑的是 NR 模型（`nvngx_dlssnr.dll`）：Swin 窗口注意力（`fswin*`）、C=512 注意力（`attn.comp`）、ViT GEMM（`gemm1x1.comp`、`vit_attn.comp`）、分组 FFN（`ffwd3_t.comp`）、token 重排（`repack_tokens.comp`）等**上百个小 dispatch**，优化大多是「把这些小 dispatch 融起来 / 重排 / 链起来」（`linux/src/core/nr_graph.cpp:52-160,269-300,1057-1309`）。
- OpenRR 跑的是 RR 模型（`nvngx_dlssd.dll`）的 rr2 图：每层一个**整块融合内核**（窗口注意力 + MLP + 残差 + DFN 头 + 2×2 pooling + merge 都在一个 `enc_stage_*` / `dec_up_*` / `dec_dfn_*` 里，见 `gpu/shaders/include/onr_block.glsl:1-30`），整网约 20–40 次 dispatch（`gpu/src/engine.cpp:629-746`）。mochi 花大力气做的「融合」，OpenRR 在图设计上已经做了。

### 逐类对照

| # | 类别 | mochi 做了什么（证据） | OpenRR 现状（证据） | 能否搬 |
|---|---|---|---|---|
| 1 | 算子融合 | `fswindsp<C>` 融合下采样、`fswinfusedup<C>` 融合上采样并折进 persistent run（`nr_graph.cpp:269-300`）；QKV 投影 + QK norm 融合 epilogue（`shaders/rdna4/include/attn_qkv_epi.glsl:1-3`）；repack 折进 GEMM 的 store（`NR_REPACK_FOLD`/`NR_OUT_PERM`，`nr_graph.cpp:155-157`）；upsample view 折进生产者 store（`NR_UPSVIEW_FOLD`，`:119-123`）；输入特征提取融进 pre-block（`image_input.glsl:1-25`） | 已是「每层一个融合内核」：block + DFN 头 + pooling + merge（`onr_block.glsl:14-22`）；`dec_up` 内含 Linear + PixelShuffle + skip fma（`:21`）；post 主路径 + indirect fixup 拆分已是专门优化（`onr_post_main.glsl:6-10`） | **不需要搬**：OpenRR 粒度已比 mochi 粗。剩余未融合只有 post→downsample（`engine.cpp:746` 一道屏障），downsample 是 3:2 带邻域读，融合需 halo，收益未知，不建议先做 |
| 2 | 协作矩阵 / WMMA / subgroup | 全部 coopmat 收在 `coopmm.glsl`；B 操作数按 `[N][K]` ColumnMajor 载入，实测「每 lane 一次 `buffer_load_b64`、零 permute」，RowMajor 要 8 次 `d16_u8` + 6 次 `v_perm`（`coopmm.glsl` NR_LOAD_B 注释）；`subgroupShuffleXor(…,16)` 做半 wave 规约（`attn_qkv_epi.glsl`、`vit_attn_vt_chunk.glsl:88-143`）；`ffwd3w` 一个权重 fragment 喂两次 MMA（`nr_graph.cpp:100-104`，4K −14%/层，1080p +3%） | 已用 `GL_KHR_cooperative_matrix`，权重 `[N][K]` 以 COL 载入（`onr_block.glsl:619,650,703,718`）——**和 mochi 的 Decision 2 同一结论**；`with_tiles` 预排 16×16 tile（`engine.cpp:195-268`） | 主要已对齐。权重 fragment 复用（一个 B 喂两个 M tile）是可借鉴的**思路**，但 OpenRR 一 WG 一窗口、64 token 已固定 4 个 M tile，需看 ISA 是否已复用，**先测** |
| 3 | LDS / 寄存器 / 占用率 | `occ_pad.glsl`：故意申请无用 LDS 来**限制每 WGP 驻留 WG 数**，消除 1080p 下 8/9 wave 不均（attn 4K −14 µs）；`image_input.glsl:13-24` 实测 wave 数 vs VGPR/LDS 拐点（10→8 subgroup 无损，5 则崩） | `onr_block.glsl:24-30` 单一 fp16 LDS 数组，C160/X256 时 64 KB/WG（规避 LLPC 多 shared 数组 bug）；没有占用率调参记录 | **可借鉴方法，不可借鉴数值**：对 OpenRR 自己最慢的 3–5 个内核做 wave 数 / LDS 占用 A/B。64 KB 的大层已是 1 WG/CU 级别，主要看小层与 post |
| 4 | Persistent / 软件流水 / 多 dispatch | persistent run + 依赖驱动 claim（`nr_graph.cpp:169,1057,1278-1301`）；`NR_CHAIN`：完成计数器替代屏障，**16 份计数器副本 + SALU 退避**，避免所有 WG 轮询同一条 L2 线拖慢生产者（`include/nr_chain.glsl:1-66`）；`NR_TCHAIN` tile 级依赖（`:71-170`） | `_fl` 内核用行标志替代小层屏障（`onr_flow.glsl:1-60`，`engine.cpp:560-686`）——同类思路已有；但 `flow_wait_rows` 是 thread 0 对**单个字**紧密自旋，无副本、无退避（`onr_flow.glsl:19-27`） | **可借鉴**：把 mochi 的「副本 + 退避」用到 OpenRR 的 flow 自旋（只改 `onr_flow.glsl`，不改图）。persistent run / tchain 不搬（绑定 NR 模型拓扑） |
| 5 | 专用打包 / 引导图 / 历史 | `image_input.glsl` 输入特征融进第一层；`temporal_history.glsl`；`upsample_view.comp`；`repack` 向量化（`NR_REPACK_VEC`） | `ingest` 一次处理 8 个引导图（`ingest.comp:1-4`）；D3D12 下 `d3d_ingest` 直接读游戏纹理；`enc0_attr` 一线程一像素算 43 个属性（`enc0_attr.comp:1-7`）；`pack` 直写输出纹理（`pack.comp:9-13`） | 已对齐；无需搬 |
| 6 | ACO 机器码层 | 记录集（`windows/data/aco/rdna3/*/shaders`、`records`）+ PAL 元数据 / 前缀改写（`windows/src/core/nr_pal_binary.hpp`）；指令级手抠如 `nr_swin_exp2` 去掉 XOR，省 32 条 `v_xor_b32`/wave（`include/swin_math.glsl:50-62`） | OpenRR 从 GLSL→SPIR-V 编译（`gpu/src/kernels.cpp:18-31`，`asi/build/build_shaders.py:68`），D3D12 侧经正式 ELF 路径 | 记录集/前缀**不搬**（不同内核、不同绑定模型）。「看 ISA 抠指令」是方法，可对 OpenRR 的 softmax / GELU 位技巧（`onr_block.glsl:1-4`）做一次 ISA 审查，收益小 |
| 7 | 图级跳过 / 条件 / 分辨率相关内核 | `ffwd3w` 只在 token ≥ 阈值启用；GEMM 工作组顺序 remap 按族与分辨率实测选择（`nr_graph.cpp:4115-4127`）；4K 时部分 tchain 保留屏障（`:1278-1289`） | `flow` 仅在 ≤240 窗口行启用（`engine.cpp:560-563`）；`fast_` / `frame_lite_` / `tile_late_` 条件路径；`dfn_out` 无人读则不写（`:680-681`） | 「按分辨率选内核变体」思路可借：对 OpenRR 的 flow / 非 flow、wave 数在 1080p/1440p/4K 分别测再定阈值 |

### 算子层可执行短名单（都不改 OpenRR 的图）

**K1. flow 自旋加副本 + 退避（借 `nr_chain.glsl:28-56`）**  
改 `gpu/shaders/include/onr_flow.glsl:19-27,48-57`：生产者对行完成字写 N 份（如 8/16 份，跨 256 B 分开）；消费者按 `WorkGroupID % N` 读其中一份，轮询间插 SALU 空转退避。避免所有等待 WG 打同一条 L2 线、拖慢正在跑的生产者。  
收益：未测；mochi 为同一问题专门加了这套机制。风险：低（语义不变，只换轮询位置）。必须 harness 逐字节一致 + `per_dispatch_` 前后对比。

**K2. 对最慢内核做占用率 A/B（借 `occ_pad.glsl` 与 `image_input.glsl:13-24` 的方法）**  
先用现有 `per_dispatch_` 时间戳列出 1080p/4K 下最贵的 3–5 个 dispatch；对其试 wave 数 / LDS 填充上限，看是否存在 mochi 那种「8/9 wave 不均」尾巴。只保留逐字节一致且实测更快的配置。  
收益：mochi 单内核 1–14% 量级；OpenRR 未知。风险：低（编译期常量）。

**K3.（可选）ISA 审查 softmax / GELU 位技巧**  
导出 `enc_block_*` 的 ACO 反汇编，看 `onr_block.glsl` 的 cubic softmax / 多项式 GELU 是否有类似 `swin_math.glsl:50-62` 那种可省的 XOR / 移位。收益很小，排在 K1/K2 之后。

### 明确不搬（算子层）
| 项 | 原因 |
|---|---|
| tchain / persistent run / 依赖 claim | 绑定 NR 模型的 C=512 / ViT 拓扑；OpenRR 已有 `_fl` 同类机制 |
| `fswindsp` / `fswinfusedup` / repack fold / upsview fold | 融的是 NR 模型图里分开的 dispatch；OpenRR 这些步骤本来就在同一个内核里 |
| ACO 记录集、PAL 前缀改写、4 KB table 绑定 | 内核与绑定模型不同，见上文「明确不建议搬的」 |
| GEMM 工作组顺序 remap 具体参数 | 针对 ViT GEMM 实测；OpenRR 没有对应 GEMM 形状 |
| INT4 / 量化权重 | 按用户要求不考虑 |

---

## 一句话

OpenRR 在 Direct 读纹理、RGBA16F 历史、fast 显存打包、flow 屏障省略上已经和 mochi 同级或更好；能再挖的主要是**分配时去掉无用清零**、**少读 ini / 少 fflush**，以及把 `7bcd355` 复核里那几条卡顿/停帧修干净。不要去搬 UMD hook、4 KB table 绑定，或 NR 模型图的 tchain。算子层：OpenRR 图本身已是「每层一个融合内核」，mochi 的融合大多不适用；可借的是 flow 自旋的副本+退避（K1）和按内核做占用率实测（K2）。
