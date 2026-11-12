# OpenRR WSL Mesa RADV (ACO) 离线编译与 PAL ELF 接入计划

> 状态：按源码审查修订，待执行（2026-11-09）。首期目标为 gfx1201 的 D3D12 Direct 路径。
>
> 实施原则：直接复用 Mochi 的 ELF、MessagePack、记录格式和前导码实现，在此基础上做 OpenRR ABI 所需的最小改动。以源码函数为准，行号用于定位。
>
> 本文位于 .gitignore 忽略的 docs/local-plans/，仅供本地开发使用。

关联计划：[FP8](C:/Users/Kenny/Downloads/OpenRR/docs/local-plans/fp8-perf-plan.md)、[Mochi 性能借鉴](C:/Users/Kenny/Downloads/OpenRR/docs/local-plans/mochi-perf-borrow.md)、[RDNA3](C:/Users/Kenny/Downloads/OpenRR/docs/local-plans/rdna3-adapt-plan.md)。

## 0. 源码复用与实施落点

| 模块 | 借鉴源 / 当前实现 | 实施落点 |
|---|---|---|
| PAL ELF、MessagePack、NRPAL002、前导码 | [nr_pal_binary.hpp](C:/Users/Kenny/Downloads/DLSSNR-AMD-mochizuki/windows/src/core/nr_pal_binary.hpp:108) | 复制到 C:/Users/Kenny/Downloads/OpenRR/gpu/src/rr_pal_binary.hpp；改命名空间、路径辅助函数及 §2 的 ABI 差异，保留原有编解码与指令编码实现。 |
| 记录匹配 | [nr_pipeline_binary.hpp 的 create_aco](C:/Users/Kenny/Downloads/DLSSNR-AMD-mochizuki/windows/src/core/nr_pipeline_binary.hpp:293) | 将按 SPIR-V 哈希查找记录并比较 record.spv_hash 的逻辑复用到 D3D12 加载端。这部分是首期必需项。 |
| D3D12 ELF 获取与创建 | [elf_of / load](C:/Users/Kenny/Downloads/OpenRR/gpu/src/d3d12.cpp:825) | 保留 elf_of() 的 LLPC 模板缓存，在 load() 中拼接 ACO ELF，再调用现有 CreateComputePipelineFromElf()。 |
| 根常量绑定 | [root_constants](C:/Users/Kenny/Downloads/OpenRR/gpu/src/d3d12.cpp:178)、[bind_kernel](C:/Users/Kenny/Downloads/OpenRR/gpu/src/d3d12.cpp:894) | 为 Kernel 增加 aco 标志，分别绑定线性 ACO 参数或现有 LLPC 参数。 |
| SPIR-V 构建 | [build_shaders.py](C:/Users/Kenny/Downloads/OpenRR/asi/build/build_shaders.py)、[Mochi build_aco_network.py](C:/Users/Kenny/Downloads/DLSSNR-AMD-mochizuki/windows/build/build_aco_network.py:22) | 沿用 OpenRR 的 SPIR-V 产物。Mochi 脚本作为构建组织参考，它不生成 ACO 记录。 |
| ACO 捕获与打包 | 新增 C:/Users/Kenny/Downloads/OpenRR/asi/build/aco_capture.cpp、build_aco_records.py，以及同目录 mesa/ 下的版本说明、补丁和构建脚本 | 用固定 Mesa 源码版本的 RADV 捕获补丁生成 NRPAL002，步骤见 Phase 1。 |
| Vulkan 原生二进制导入（后续） | [nr_pipeline_binary.hpp](C:/Users/Kenny/Downloads/DLSSNR-AMD-mochizuki/windows/src/core/nr_pipeline_binary.hpp:1) | 按需复制到 C:/Users/Kenny/Downloads/OpenRR/gpu/src/rr_pipeline_binary.hpp，适配日志、路径、设备配置和缓存依赖；Vulkan 使用自己的参数偏移。 |

## 1. 目标与现有链路

OpenRR 构建阶段将 GLSL 编译为 SPIR-V。D3D12 Direct 的 D3dDevice::elf_of() 通过同 LUID 的辅助 Vulkan 设备获取 Windows 驱动生成的 PAL ELF，load() 经 patch_elf() 调整 user-data 映射后，将内存中的 ELF 交给 AMD 的 CreateComputePipelineFromElf()。

ACO 接入复用这个创建点：以 LLPC ELF 为容器，用离线 ACO 记录替换机器码和资源元数据，并以前导码适配参数，无需增加 amdxc64.dll Hook。优先测量寄存器占用、LDS、scratch 和实际 dispatch 耗时，按算子选择编译器。收益以 OpenRR 相同输入的 A/B 结果确定。

首期聚焦 gfx1201、D3D12 Direct 和当前发布着色器。Vulkan 原生导入及其他 GPU 架构后续扩展；着色器数量以构建脚本产出的清单为准。

## 2. ABI 与运行时接入

### 2.1 Push constants 与 user data

| 项目 | OpenRR D3D12 约定 | 改动方式 |
|---|---|---|
| Vulkan 模板 layout range | 256B，保持当前 VkPushConstantRange | splice() 的模板范围参数支持 256B。 |
| 有效 push 数据 | 最多 244B / 61 dwords | D3D12 记录加载检查 r.push_bytes ≤ 244；绑定检查 push_size ≤ 244。捕获器将有效 push 字节数与 256B layout range 分开记录。 |
| 根常量映射 | rc[0..60] 对应 entry 4..64 | Arguments 的 inline 映射改为 dw + 4，spill load 偏移为 4 × (dw + 4)。 |
| 连续 push 指针 | spill 基址 +16 字节 | 复用原前导码，将 D3D12 路径的加法内联常数 0x84 改为 0x90。 |
| 描述符表指针 | rc[61] 对应 entry 65 | D3D12 的 descriptor_slot = 65，user-data limit 为 66。 |

原版解析器的 128B 上限可放宽到 256B；D3D12 入口另行执行 244B 的有效数据限制。根常量数据覆盖字节区间 [16, 260)，描述符指针从 byte 260 开始，二者不重叠。

将 push 起始 entry 作为 ABI 参数传给参数映射和前导码：D3D12 为 4，后续 Vulkan 路径为 1。ELF 与 MessagePack 编解码继续复用原实现。

### 2.2 纯 BDA 核与可选 set 0

OpenRR 的纯 BDA 核通过 push 中的 GPU 地址读写缓冲区，不要求描述符参数。以 clear_range 的捕获结果为首个样本，按 RADV 实际参数分配填写记录：未使用 set 0 时写 set0 = -1。

对原版做两处配套修改：

- Record::parse() 仅在 r.set0 ≥ 0 时调用 take(r.set0)。
- Arguments 在无 set 0 时不生成其搬运指令。首期保留原有 PAL 参数槽位预留和其余分配顺序，减少对寄存器搬运逻辑的改动。

保留原有参数覆盖与搬运顺序检查。代表核记录用于核对 inline、push pointer 和 workgroup 参数；全量编译时汇总仍需适配的参数形式。

### 2.3 两套描述符布局

离线创建 pipeline layout 时复用 [vk.cpp:462](C:/Users/Kenny/Downloads/OpenRR/gpu/src/vk.cpp:462) 的实际定义：

| 布局 | Vulkan 接口 | D3D12 表 |
|---|---|---|
| layout_ | set 0、binding 0：8 个 COMBINED_IMAGE_SAMPLER；BDA 由 push 中的地址提供 | table_lo_，当前每元素 48B，内容来自 Windows Vulkan 描述符。 |
| d3d_layout_ | set 0、binding 0：16 个 SAMPLED_IMAGE；binding 1：2 个 STORAGE_IMAGE | desc_lo_，由现有 SRV/UAV 写入逻辑提供。 |

在捕获端导出 RADV 的 binding offset、array stride 和 image/sampler 相对偏移，与当前 D3D12 表逐项比较。用不同内容填充多个数组元素并读回输出，验证索引和实际采样。布局一致即可复用现有表；不一致时为对应 ACO 核单独组织描述符表。在表适配完成前，该类核继续用 LLPC，纯 BDA 核可先进入性能测试。

### 2.4 记录标识与 wave32

保持 NRPAL002 的 96B 头、8 × n inline 表和代码区格式。附带 manifest.json，记录 Mesa commit、捕获补丁版本、目标 GPU、SPIR-V 哈希、有效 push 范围、布局版本、workgroup 三维尺寸、实际 wave size 及编译选项。

首期 ACO 捕获统一请求 wave32，生成 LLPC 容器时也请求 wave32。原来 subgroup 请求为 0 的辅助核，在 wave32 下完成结果比对后加入启用清单；LLPC 回退仍使用原 subgroup 请求。

记录目录按目标架构和布局版本组织，内部按 SPIR-V 内容哈希命名。加载时比较 record.spv_hash 与当前 SPIR-V，并匹配 manifest 的架构、布局和 wave32 配置。occ_*、b2_* 的同名变体因此各自使用对应记录。哈希算法直接复用 pal::hash()。

### 2.5 D3D12 加载与绑定

D3dDevice::load() 按以下顺序执行：

1. 读取 SPIR-V，按 ONR_ACO 和已验证核清单选择编译路径。
2. ACO 路径读取并匹配记录，使用 wave32 的 elf_of() 结果作为模板，调用 splice()，再将结果传给现有 CreateComputePipelineFromElf()。该路径不再调用 LLPC 的 patch_elf()。
3. 创建成功后设置 k.aco = true；无匹配记录或导入失败时，用原始 subgroup 走现有 LLPC 加载路径，记录原因。
4. ACO 绑定先清零 rc，按实际 push_size 线性复制至 rc[0..60]，最后写入 rc[61]。LLPC 绑定保持原有 root_constants() 旋转逻辑。

现有磁盘缓存继续存 LLPC 模板，ACO 拼接放在缓存读取之后。先不增加 D3D12 ACO 成品缓存。日志输出每个核实际使用的编译器、SPIR-V 哈希和记录标识，供 A/B 归档。

## 3. 分阶段执行

### Phase 1：固定捕获工具链并生成三个代表记录

1. 核对 WSL 中已记录的 mesa-vulkan-drivers 26.0.8 与 libvulkan-dev 1.4.341。以 Mesa 26.0.8 对应源码为起点固定 commit；捕获构建使用独立 RADV ICD 路径，不依赖系统包内部结构。
2. 在 asi/build/mesa/ 保存版本说明、构建命令和最小 RADV 捕获补丁。通过 RADV_FORCE_FAMILY=gfx1201 使用离线编译路径，先确认能枚举目标设备并创建一个计算管线。
3. 在 RADV 编译完成、shader 参数与资源配置仍可访问的位置加入记录导出。复用 Mesa 自身结构取得 ACO 代码、有效代码长度、rsrc1/2/3、LDS、scratch、user SGPR 数以及 set0/push/grid/inline 参数映射；同处导出实际 wave size 与描述符布局。具体结构和挂载函数随固定 commit 写入版本说明。
4. 编写 aco_capture.cpp 创建与 OpenRR 相同的布局、所需功能和 wave32 管线；build_aco_records.py 逐个输入 SPIR-V、调用捕获程序、计算哈希、生成记录与 manifest。push_bytes 写实际有效数据范围，256B layout range 单独写入 manifest。
5. 首批选择 clear_range（纯 BDA）、post_f（标准采样）、d3d_downsample（视图读写）三个核。复制解析器并完成 §2 所需适配后读取记录，检查资源字段、参数表及代码校验和，输出两套布局对照表。

**本阶段产出：** 可重复运行的 Mesa 构建与捕获命令、三个 .nrp、manifest、参数及布局报告。捕获功能在此阶段实现，不将系统 RADV 包或 SPIR-V 构建脚本视为现成的记录生成器。

### Phase 2：接入代表核并验证 GPU 输出

1. 完成 rr_pal_binary.hpp 的 OpenRR 命名空间、UTF-8 路径辅助函数和 §2 ABI 适配。
2. 在 Kernel、D3dDevice::load() 和 bind_kernel() 接入 ACO，提供 ONR_ACO=0/1；初期启用清单只包含代表核。
3. 在 test_asi_units.exe 的 D3D12 测试中分别验证清零/填充值、标准采样、视图读写。用相同输入比较 ACO 与 LLPC，覆盖多个描述符数组元素和实际 push 参数。
4. 用一个现有 WMMA 网络核补上大核验证，确认 wave32、LDS 和 scratch 元数据随记录正确导入。

**本阶段产出：** 代表核输出对比、实际 ACO 加载日志，以及可供全量扩展的 D3D12 接入实现。描述符表有差异的核按 §2.3 完成适配后加入。

### Phase 3：全量生成、精度与性能 A/B

1. 从 build_shaders.py 的实际产物生成记录清单，覆盖 Legacy、Fast 和 occ_nopad/、occ_pad16k/、b2_* 变体。按哈希区分同名 SPIR-V。
2. 汇总 Top-8 算子的 ACO/LLPC vgpr_count、sgpr_count、lds_size、scratch_bytes 和代码字节数，运行现有 test_asi_units.exe 与 openrr_bench.exe 精度检查，要求 non_finite == 0 并沿用 Legacy/Fast 精度阈值。
3. 为同进程 A/B 提供测试侧的显式编译器选择：分别创建 LLPC 与 ACO kernel/context，在创建时固定配置。扩展 bench 的 baseline/candidate 参数传入选择，避免两组都继承同一个进程环境值。
4. 在 1080p、4K 开启 set_stamps(true)，按实际 dispatch 清单测量逐核耗时；对 B0/B2 复用现有旁路目录测试。报告同时列出实际 ACO 核，确保比较覆盖候选代码。
5. 在 1080p、1440p、4K 各跑同进程三轮端到端 A/B，使用相同输入、预热及历史状态重置；按中位数和波动范围择优，生成最终启用清单。

**本阶段产出：** 编译指标、逐核与端到端结果、Legacy/Fast 精度结果、最终核清单，归档至 C:/Users/Kenny/Downloads/OpenRR/docs/test-evidence/。

### Phase 4：一键构建与 FP8 合流

1. 将固定 Mesa 捕获工具的准备、SPIR-V 后置记录生成和 manifest 打包接入 [build_asi.sh](C:/Users/Kenny/Downloads/OpenRR/asi/build/build_asi.sh)，由 [BUILD_LOCAL_ASI.cmd](C:/Users/Kenny/Downloads/OpenRR/asi/build/BUILD_LOCAL_ASI.cmd) 统一调用。复用已构建的捕获工具，仅重编译内容或编译配置变化的记录。
2. 将 gfx1201 记录、manifest 和经测试选出的启用清单随 openrr-amd 数据目录打包；运行时按同一清单加载。
3. FP8 计划阶段 2/3 的新算子进入相同捕获流水线，验证所需功能与输出后追加到启用清单。其他架构单独生成记录集；Vulkan 原生导入复用记录格式和编解码器，使用 Vulkan 参数 ABI。
