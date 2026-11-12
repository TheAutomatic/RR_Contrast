# 给 OpenRR 的 RDNA4 FP8 / 性能建议（来自 DLSSNR-AMD）

读了 `fp8ornot.md` 和 `fp8-perf-plan.md`。下面是我们在 RX 9070 XT（gfx1201）上做 NR 网络优化时实测过的东西和踩过的坑，
挑和你们 FP8 计划相关的写。数字都是我们自己的测量（Linux RADV/ACO 为主，Windows LLPC 的另外标明），
你们的 kernel 不同，数字只作量级参考，结论请用你们自己的 A/B 验证。

---

## 0. 先说结论

1. **RDNA4 上 FP8 WMMA 的矩阵吞吐是 FP16 的两倍。** 指令形状都是 16×16×16，条数一样，但每条占用的周期 fp8 是 8、f16 是 16。
   `fp8ornot.md` 第 ③ 点"FP8 的核心收益只是权重带宽减半"是错的，FP8 主要省的是矩阵单元周期。
2. **FP32 累加本身不花额外的钱。** `v_wmma_f32_16x16x16_f16` 和 `v_wmma_f16_16x16x16_f16` 同为 16 周期，fp8 只有 f32 累加，8 周期。
   文档列的代价（每步 `acc = MC(MH(acc))`、累加器寄存器翻倍撑爆 VGPR）都是 kernel 写法带来的，不是 FP32 累加慢。
3. **FP8 在实际 kernel 里变慢，几乎总是因为激活值的重量化（requant）。** gfx12 上 VALU 和 WMMA 完全串行，
   转换、clamp、NaN 处理全算在 kernel 时间里。重量化做对了，FP8 在 C=32 这种 VALU 很重的层也能赢；做不好，在哪一层都会输。

---

## 1. RDNA4 硬件事实（实测 + 编译器/官方代价表）

### 1.1 WMMA 周期

| 指令 | 周期（每条，wave32） | 来源 |
|---|---|---|
| `v_wmma_f32_16x16x16_f16` / `_bf16` | 16 | Mesa ACO `src/amd/compiler/aco_statistics.cpp`（gfx12 分支）；AMD `amd_matrix_instruction_calculator` 的 rdna4 表 |
| `v_wmma_f16_16x16x16_f16`（f16 累加） | 16 | 同上，**和 f32 累加一样** |
| `v_wmma_f32_16x16x16_fp8_fp8`（及 bf8 组合） | 8 | 同上；我们微基准实测 7.4–8.1 |
| `v_wmma_i32_16x16x16_iu8` | 8 | 同上 |
| `v_wmma_i32_16x16x32_iu4` | 每 MAC 约为 fp8 的 2 倍速率 | 实测 sustained：Vulkan 657 vs 312 TOPS |

ACO 源码里那段注释原文："Half the cost of GFX11, int4/8 and (b)f8 twice as fast as (b)f16"。
wave64 下 fp8 WMMA 同样是 8 周期（同样的 MAC 数），wave64 本身不吃亏。

### 1.2 WMMA 和 VALU 完全不重叠（最重要的一条）

- AMD 的 matrix instruction calculator 对 rdna4 所有 WMMA/SWMMAC 都标 `coexec False`（CDNA3 的 MFMA 是 True）。
  LLVM 只在 gfx1250 上建模 WMMA 并行执行。ACO/LLVM 都把 gfx12 的 WMMA 算作占用 VALU 资源。
- **同一个 SIMD 上，跨 wave 也不重叠**：一组只做 WMMA 的 wave 加一组只做 VALU 的 wave，放在同一 SIMD 上，时间直接相加（1.34 + 1.52 → 2.98 ms）；分到不同 SIMD 上才是取最大值。所以"wave specialization"（一部分 wave 专做矩阵、一部分专做激活）在 gfx1201 上不提供额外吞吐。
- 真实 kernel 上的验证（我们的 C=32 Swin 层）：把每条 WMMA 复制一遍（VALU 不变）→ +53%（相加模型预测 +46%，重叠模型 +80–100%）；
  删 896 条 VALU → −18.5%（相加模型 −20%）。
- 例外：`v_exp` 等 TRANS 指令能盖住约一半 WMMA 周期；SALU 和 LDS 完全重叠；VOPD 少量。所有普通 VALU（fma / add / mul / max / maxmin / fma_mix / pk_fma / `v_cvt_pk_fp8_f32`）都和 WMMA 串行。

**推论：kernel 时间 ≈ VALU 周期 + 8 × fp8 WMMA 条数（f16 则 16 ×）+ load 回写周期。**
我们的 C=32 kernel 实测落在这个模型的 94–97%（VALU 约 45%，WMMA 约 45%，load 回写约 8%）。
用它可以在跑 GPU 之前就算出 FP8 改动能不能赢：省掉的 WMMA 周期 = 8 × 条数，新加的转换/clamp 是一条一周期（VOPD 可成对）。

### 1.3 Load 回写也占发射周期

VMEM/LDS load 的数据写回 VGPR 要占 SIMD 发射周期，同样不和 WMMA/VALU 重叠：
`buffer_load_b128` 约 5.2 周期，`b64` 约 4，`ds_load_b64` 约 1.6。
我们把 GEMM K 循环里的 load 挪走后：一个 kernel 25.3 → 20.8 µs，另一个 27.1 → 18.7 µs。
所以 FP8 权重让 load 从 `dwordx4` 变 `dwordx2`，省下的主要是这部分回写周期，而不是显存带宽。

### 1.4 FP8 转换只吃 f32

gfx12 的 `v_cvt_pk_fp8_f32` 只接受 f32 源，一次转两个（第二对用 op_sel 写进高半）。
f16 激活要先 `v_cvt_f32_f16` 再转。累加器本来就是 f32，**直接从 f32 转 fp8**（一次舍入），不要先收窄成 f16 再展开回 f32。

### 1.5 VALU 搬上 WMMA 的盈亏线

fp8 WMMA 每周期 512 MAC，VALU（VOPD/packed）约 64 MAC。同一个发射口，所以把一个线性运算从 VALU 挪到 WMMA，
只有在 WMMA 里至少 1/8 的 MAC 是有用的才划算（f16 WMMA 是 1/4）。用全 1 矩阵求和这类做法通常算下来更贵。

### 1.6 Wave 调度是"最老优先"

persistent workgroup 里最后启动的那些 wave 会被饿死：同一 kernel 里早启动的 WG 处理一项 6.9 µs，最后启动的 126 µs。
写 persistent / work-stealing kernel 时要考虑这一点。

### 1.7 其他

- SALU 在 gfx1201 上基本免费（额外加 1197 条相依 SALU，时间不变）。
- wave64：fp8 WMMA 不吃亏，但我们把一个 C=32 层移植到 wave64 反而慢了 3.7%（跨半的 reduction 要 `v_permlane64`）。
- 9070 XT：Linux 下 WMMA 用到满时功耗卡在默认上限，持续负载下时间约等于能耗，见第 5 节。

---

## 2. 对 `fp8ornot.md` 的逐条意见

| 文档的说法 | 我们的看法 |
|---|---|
| ① FP32 累加器每个 tile 8 VGPR，FP16 累加 4 VGPR，大核里会顶到 256 / spill | 寄存器翻倍是真的。但这是 tile 划分的问题：一个 wave 同时握 6–10 个输出 tile 是按 f16 累加设计的，换 f32 累加就要重新划分（少握几个 tile、K 方向多走几次）。另外注意驱动 LLPC 的 VGPR 分配粒度是 24：240 个 VGPR 是 6 wave/SIMD，241 个就掉到 5 wave。RGA/amdllpc 报的 VGPR 数和驱动实际的不一样，以驱动的为准。 |
| ② 每步 K=16 做一次 `acc = MC(MH(acc))`，VALU 爆炸 | 这是用 f32 累加器去**模拟** f16 累加的舍入，代价是这个模拟本身，不是 f32 累加。要 f16 累加的语义就直接用 `v_wmma_f16_16x16x16_f16`（同样 16 周期）；能接受 f32 累加就不要逐步截断。fp8 没有 f16 累加的指令，要在 fp8 上模拟 f16 累加只能这样截断，这时它肯定比直接 f32 累加慢，但"慢"来自模拟。 |
| ② FP8 激活重量化编译器会生成大量转换、NaN 检查和 clamp | 对，这是 FP8 真正的成本。做法见第 3 节。 |
| ② 引用我们 `coopmm.glsl` 的"768 条 `v_maxmin` + 608 条转换 vs 256 条 WMMA = 54%" | 这里把两次不同的测量拼在了一起：768 条 `v_maxmin_num_f32` 是 C=32 某个构建（mode 0）的 clamp 数；608 条是另一个 kernel（`fswin_t` 转置版）的转换数；54% 是那个 kernel 里"NaN 分支 + f16 绕路"占 VALU 的比例（608 条 `v_cvt_pk_fp8` 对 640 条 f16↔f32 转换、525 条 NaN 分支、320 条用 f16 做的 clamp）。我们注释里也已经把 C=32 的数字更正为转换 1489 条、占 VALU 39.8%。我们从中得出的结论是"把这部分去掉"（于是有了 mode 4/5），不是"FP8 更慢"。 |
| ③ 16×16×16 下 FP8 不减少 WMMA 条数，核心收益是权重带宽减半 | **错。** 条数一样，周期减半（8 vs 16），见 1.1。 |
| 第 4 节 `FP8QV` 注释："C >= 64 更快，C = 32 更慢" | 和我们的模型一致：C=32 层 VALU/WMMA 大约各占一半，WMMA 减半最多省 kernel 的 20–25%，每个元素多一两条转换就能把它吃掉。所以 C=32 要赢，关键是让量化尽量少、尽量不在消费端做（见 3.1）。我们的 C=32 层全部用 fp8 WMMA，量化写法是我们在这些层上花功夫最多的地方。 |
| 第 3 节"Mochi 认为 FP32 累加最快" | 不是这个意思。我们说的是 FP32 累加不会比 FP16 累加慢（同样 16 周期，fp8 则 8 周期）。 |

---

## 3. FP8 量化：我们的做法

### 3.1 在生产者那里量化一次，消费者直接吃 fp8

- 一层的结果在它自己的 epilogue（累加器还是 f32 的时候）转成 e4m3 写进 arena，下一层直接 `coopMatLoad` fp8 片段，不再解压、不再转换。
- 你们计划里 Stage 2 的 `DEC_UP`、`merge` 已经是 arena e4m3 输入，先做这些是对的；Stage 3 的 `fc1`/`O` 这类，应该把量化挪到**上一层的 epilogue**，而不是在 MLP 的消费端循环里做。
- 同一个激活被多个消费者读（Q/K/V 三个投影等）时，在消费端各转一次就是成倍的 VALU。

### 3.2 从 f32 直接转，一次舍入

- 累加器是 f32：`f32 → e4m3` 一次舍入。`f32 → f16 → f32 → e4m3` 是两次舍入，多两条转换。
- **RADV/NIR 会自动删掉 `float(float16_t(x))` 这种往返（不精确的折叠）；LLPC 不删。** 我们 Windows 版一个 C=32 kernel 每个 wave 有约 760 条这样的往返，手动去掉后 Windows 上 1080p −3.6%、4K −3.9%。
- 注意：从 f32 直接转和经过 f16 再转，结果在舍入边界上可能不同。你们的 Legacy 有逐位一致门禁，这类改动要单独算是不是允许的。

### 3.3 饱和：e4m3 没有 Inf

- **驱动的隐式转换会把 > 448 的有限 f16 变成 NaN**（65,536 个 f16 位模式里有 14,718 个），一个 NaN 会污染下一次 MMA 的整个累加器 tile。所以不能裸转。
- `clamp(x, -448, 448)` 就是 `min(max())`，NaN 输入会变成 −448（符号和值都错）。输入里确定没有 NaN 才能这么用。
- 我们 `coopmm.glsl` 里的几种做法（`NR_QUANT_MODE`，文件里有详细注释）：
  - **mode 4（ACO）**：让硬件饱和（MODE 寄存器 `FP16_OVFL`）。ACO 把普通的 `f2e4m3fn_sat` 降成 `v_minimummaximum_f32 + v_cvt_pk_fp8_f32`，代价和手写 clamp 一样；只有 `nir_opt_algebraic` 匹配到 `f2e4m3fn_sat(bcsel(feq(fabs(x), inf), NaN, x))` 这一个形状时才会生成不带 clamp 的 `f2e4m3fn_satfn`。所以代码里用 `isinf(v) ? NaN : v` 写法，这是为了让优化器匹配上，不是防御代码。ISA 文档对 `FP16_OVFL` 处理 Inf 的描述是错的。
    **是否真的走到了免费路径要量出来**：用 ISA 直方图数 `v_maxmin_num_f32`，没有降到 0 就是没匹配上。我们在 C=32 上测得 mode 0 有 768 条 `v_maxmin`、0 条 `s_setreg`，mode 4 有 0 条 `v_maxmin`、343 条 `s_setreg`（对应 384 条转换）。
  - **mode 5**：值已经是 `f16vec2` 时，用 `v_pk_max_f16 / v_pk_min_f16` 两条打包指令 clamp 一对，不碰 MODE 寄存器。
  - 和 NVIDIA `cvt.rn.satfinite` 的区别：硬件饱和下 Inf 变 NaN（satfinite 是 Inf → ±448、NaN 保留）。只对有限输入等价。
- **LLPC（Windows 驱动）上没有免费的 e4m3 饱和**：`saturatedConvertEXT` 在 LLPC 上是 clamp + class + cndmask，比手写 clamp 还慢。

### 3.4 LLPC 上转换的写法（你们现在用驱动 LLPC，这节最直接）

- LLPC 每条 e4m3 转换都是自己的 `s_setreg(MODE)` + `v_mov 0` + `v_cvt_pk_fp8_f32`。
  **`fe4m3vec4(vec4)` 一次转 4 个 = 一次 MODE 写 + 两条转换进一个 dword**。我们把所有量化点改成按 4 个一组转换（公开仓库 `windows/build/quad_quant_glsl.py`），Windows 上整个网络快了约 10%。
- 驱动的 LLPC 是 true16 模式：4 宽 e4m3 转换会变成"转低半 + 复制 + 转高半 + `v_bfi` 合并"，每个 dword 多 2 条 VALU（cooperative matrix 的转换也一样）。拆成两对的写法各有输赢，只在 VALU 少的两个图像块里赢，按 kernel 实测。
- `s_setreg`：我们在 Windows 上微基准测得 1 wave/SIMD 时约 15 周期，满占用时基本被掩盖。MODE 写本身通常不是大头（我们做过一次消融：616 条 MODE 写不是成本所在）。
- 片段加载：大 shader 里 LLPC 会把从内存读的 e4m3 片段拆成逐字节拼装（每个片段一条恒等 `v_perm 0x3020104`，还把 load 等待提前），从**一个 `uvec2` + `unpack8`** 填片段就没有（`NR_FRAG_Q`）。小测试 kernel 看不到这个现象，要在真 kernel 上看。
- 标量 `e4m3 → f32` 是取字节 + `v_cvt_f32_fp8` 一个一个转；按 `fe4m3vec4` 一次转（两条 `cvt_pk`）。
- 需要 gather 的 e4m3 片段加载（A ColumnMajor / B RowMajor）从 storage buffer 读是每 lane 8 条 `buffer_load_u8`；通过 `buffer_reference`（全局地址）读是一条 `global_load_tr_b64`（`NR_VBDA`）。

### 3.5 权重和操作数

- 权重放 A（src0），累加器可以直接喂下一次 MMA，不用转置。
- WMMA 的能耗主要取决于 src0（A）里的数据（同一 fp8 WMMA，随机数据 2430 MHz、常数数据 3030 MHz）。换操作数顺序、权重行重排在真实数据上只有 2–3% 量级，我们试过，基本没收益。

---

## 4. 编译器的坑

### 4.1 ACO（你们 Stage 1.5 要用）

1. **fp8 片段的 phi 会逐字节拷贝**：`if (live) coopMatLoad(frag) else frag = 0` 这种写法，ACO 每个 dword 插 4 条 `v_perm_b32`。我们一个 ViT attention kernel 每次迭代有 64 条，把整块和尾块拆成两份代码后 20.5 → 17.75 µs。
2. **wave 内一致的 `if` 被展平成逐元素 `v_cndmask`**。`[[dont_flatten]]`（GL_EXT_control_flow_attributes）对小的 then 块不一定保得住；块里放一个永远不成立的 store 才能逼出真分支。分支也会打断 ACO 调度，要按 kernel 实测。
3. **`tid / 32u` 在 NIR 看来是 divergent 的**，基于它的 tile 判断都会落进 VGPR。用 `subgroupBroadcastFirst(tid / 32u)` 让它变成标量。
4. **全展开 body 里按 tile 加运行时守卫会制造大量 phi**：我们一个 kernel 指令 5470 → 8285、VGPR 192 → 256（8 → 5 wave）。只有副作用（store）的守卫没事；值要从两条路之一来的，就把整个循环连 store 复制两份。
5. `coopMatLoad` 每次 load 留两条 VALU 算地址（常量进不了 MUBUF 的 offset 字段）。手写按 lane 的 dword 索引 + 常量步长，ACO 会生成 `offen offset:imm`。
6. **NIR 会改变舍入**（对你们的 Legacy 逐位一致门禁很重要）：
   - `f2f16(fadd(...))` 会被融合成 `v_fma_mix`（一次舍入），`precise` 挡不住。
   - 删掉一个分支可能连带删掉一次 f16 舍入（`f2f32(f2f16(x)) → x`）。我们有一次把常量为假的 tile 判断特化掉，少了 128 条转换，画面对 NVIDIA 参考掉了 1.37 dB。
   - `f16vec3(textureLod(...))` 会被折叠成 `image_sample ... d16`，由纹理单元转 f16：float 纹素是 RTZ，UNORM 纹素是 RNE。同样的值换个图像格式画面就不同。
   - RDNA4 上往 `R16G16B16A16_SFLOAT` storage image 做 `imageStore` 是 RTZ（我们穷举了 [0,1] 内全部 fp32 值）。
7. ACO 在循环回边上，如果 phi 带的是 load 出来的寄存器，会插 `s_wait_loadcnt 0`，跨迭代的预取不成立。解决办法：固定 trip count + `[[unroll]]`。
8. fp8 WMMA 的 K 顺序会影响结果（K 置换后 4K 输出变了）。

### 4.2 LLPC（驱动）

1. **在 ACO 上调好的旋钮不能直接搬到 LLPC。** 我们一个占用率上限在 ACO 上更快，在 LLPC 上 4K 从 0.74 → 2.02 ms；一个 attention 重映射让 LLPC 的下一个 GEMM 0.15 → 0.42 ms。每个旋钮都要在两个编译器上分别 A/B，而且对比 ACO 和 LLPC 时要用**同一份 SPV**。
2. LLPC 会把写成逐分量的 f16 对运算标量化，`f32(h) * f32(h')` 会被缩成 `v_mul_f16`。用打包的 `f16vec2` 运算来写。
3. LLPC 发 load 紧贴使用点（同一份 SPV，ACO 会提前约 110 条指令），SQTT 上 C=32 图像块 14% 在等 vmem。
4. 某个驱动版本上，同一个大 body 在 if/else 两个分支各实例化一次会让编译器崩溃（`vkCreateComputePipelines` 0xC0000005）；改成多个互斥的独立 `if` 就好了，输出逐位相同。
5. 想看 ISA：不要用 RADV 反汇编器的列表直接数。它在 8 字节的转换指令上会失步，我们曾因此少数了 13.7% 的指令；gfx12 的 `v_cvt_pk_fp8_f32` 还会被显示成 "invalid + v_cndmask"（假的 cndmask）。用按编码长度走的直方图工具数。

---

## 5. 测量的坑

1. **只信持续负载的数字。** 短跑（预热少、计时帧少）会落在开局的频率冲高段：同一个二进制，短跑 5.45 ms，持续 5.72 ms。我们的标准是长预热（1080p 1500 帧、4K 500 帧）+ ABBA 交替。你们计划里 32 帧预热 + 120 帧可能偏短，建议看一下 120 帧里前后半段是否一致。
2. **持续负载下，单个 dispatch 变快不等于帧变快。** WMMA 吃满时 9070 XT 在默认功耗上限内运行，时间近似和能耗成正比。去掉 load 等待这类"空闲"不省能量：我们有一个 kernel 20.6 → 14.8 µs（逐位相同），整帧在 1080p 和 4K 都持平，频率低了 10–20 MHz。真正省时间的是 WMMA、FMA 类、超越函数、数据搬运这些耗能的工作（一个 VALU 周期大约是一个 WMMA 周期能耗的一半，不是免费的）。所以每个算子的收益都要用整帧持续 A/B 确认。
3. **微基准里的数据要真实。** WMMA 能耗随 A 操作数的数据变化，常数/全零数据比随机数据频率高约 25%。用假数据估吞吐（比如你们的 `FP8PROTO` 模式 2）会高估。
4. **GPU 不忙时测的时间不可信。** 重放工具每帧读盘、回读、算哈希，GPU 只有 20% 忙，频率在 0–1300 MHz 之间乱跳，同一构建两次差 0.4 ms。计时运行要关掉回读/哈希、帧数据常驻内存，并记录功耗和频率确认 GPU 在忙。
5. **噪声比想的大**：同一构建 A/A，1080p 批次间漂移约 0.03 ms，4K 单对差到 ±0.05 ms。3 批差值小于这个量级的，什么也说明不了。
6. **计时运行也要检查画面**（哈希或通道均值）。我们有过一个坏 shader"赢了"的情况。
7. **失败的运行可能打印出通过的哈希**：脚本 `cmd || tail log` 之后再算输出文件的哈希，算的是上一次留下的旧文件。我们因此有一个 4K GPU hang 藏了三个构建。运行前删掉输出，要求退出码为 0 且文件是新的、非空的。
8. 模型只能排序，不能否决。成本模型说"没效果"的东西，实测可能有 11–31 µs；反过来也一样。不要只靠模型关掉一个方向。

---

## 6. 结构上的经验

- **persistent kernel + tile 计数器代替 barrier** 是我们收益最大的结构改动之一（1080p 5.78 → 5.62 ms，4K 22.49 → 22.32，逐位相同）。坑：
  dispatch 不保证按全局顺序启动（等待要有上限 + 错误字）；帧计数器要有专门的 tick；**一定要在多个分辨率、多帧连跑下查竞争**。我们有一次在 1080p 完全正确，换个分辨率就是 29 dB，原因是 tick 所在的 pipeline 那个尺寸下没跑。
- 试过、在 gfx1201 上没收益的：wave64（慢 3.7%）、workgroup 轮询仲裁（驱动层改动，慢 1%）、WMMA 操作数顺序、全零片段跳过（真实数据里全零 16×16 片段几乎没有）、split-K、LDS 中转的 GEMM。
- 异步计算（NR 放到 compute-only 队列）：我们做过正确性和计时，输出逐位相同，但在 GPU 已经满负载时不减少总工作量，只是把 GPU 时间挪给游戏，整体没有帧率收益。
- INT4：`v_wmma_i32_16x16x32_iu4` 是 fp8 速率的约 2 倍。驱动没有暴露 int4 cooperative matrix，我们的做法是用 int8 coopmat 写 kernel，从 pipeline binary 里把 iu8 指令改写成 iu4（结果逐位正确）。速度收益是真的，但画质要特别小心：离线静态测试看不出来的问题，到了游戏里会随时间累积（亮度漂移、暗部细节被抹掉）。必须用连续帧和真实游戏验证。

---

## 7. 建议怎么验证"FP16 比 FP8 快"

对同一层、同一形状、同一 tile 划分，分别出 FP16 和 FP8 两个构建，每个 kernel 列出：

1. WMMA 条数（f16 按 16 周期算，fp8 按 8 周期算）；
2. VALU 条数，其中转换（`v_cvt_*`）、clamp（`v_maxmin*` / `v_pk_max/min`）、NaN/class 检查、`v_perm`/`v_bfi` 拼装各多少；
3. `s_setreg` 条数；
4. VGPR 数（以驱动报告为准）、每 SIMD 的 wave 数、有没有 scratch；
5. load 条数和宽度。

用 1.2 节的模型（VALU + 8/16 × WMMA + load 回写）算两个构建的预期周期，再和持续 A/B 的实测对照。
如果 FP8 构建多出来的 VALU 周期超过省下来的 8 × WMMA 条数，问题就在量化写法上，按第 3 节改；
如果 VGPR 跨了 wave 数的台阶，问题在 tile 划分。两种情况都不是 FP8 或 FP32 累加本身的问题。
