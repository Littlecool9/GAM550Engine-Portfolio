# 地形管线 P2 本地交付

工程基线为 `118-terrain-instance-play-pin`（`3a6df0d`），总集成分支为 `codex/terrain-p2-integration`。七个 issue 均使用本次新建的独立 worktree、分支和四套独立 CMake 目录；没有参考 C 盘以前的实现，没有 push、创建远程 PR 或关闭 issue。

本次完成 format 2 的稀疏载荷、逐 tile 唯一源精度、最终高度接缝、独立 LOD/法线/误差界、混合精度光栅与光追、最粗优先的统一碰撞和 Lua 驻留查询。保留 P1 同步加载，P3 才处理 streaming、多个兴趣源、区域拆分和安全替换。

## 各 issue 的本地交付

计数为“通过 + SKIP”；所有配置失败和禁用均为 0。Debug、Release、game-debug 各自的通过数相同，合列仅为减少重复。每轮均显式 reconfigure、完整构建并执行全部注册 CTest，包含 smoke；SKIP 不计为通过。

| Issue / 中文 PR 描述 | 分支 / 本地提交 | Debug / Release / game-debug 各自通过 | debug-float | smoke 各配置 | 风险及需要审核的实现决定 |
|---|---|---:|---:|---:|---|
| [#145 格式](terrain-p2-145.md) | `codex/terrain-p2-145` / `8135fe1` | 676 | 641 + 35 SKIP | 6/6 | 整份索引驻留；当前最终格式为 2.1，只接受 major 2。|
| [#119 精度图](terrain-p2-119.md) | `codex/terrain-p2-119` / `0c4516d` | 688 | 653 + 35 SKIP | 6/6 | recipe 固定过滤基准，默认值只填未赋值 tile；过滤读取全局 halo。|
| [#120 最终接缝](terrain-p2-120.md) | `codex/terrain-p2-120` / `d44df58` | 699 | 664 + 35 SKIP | 6/6 | smoothstep + Coons；边带 2×粗 spacing、最多半 tile；0.5 m 质量告警允许发布，保证 C0。|
| [#121 派生载荷](terrain-p2-121.md) | `codex/terrain-p2-121` / `618bc79` | 711 | 676 + 35 SKIP | 6/6 | 原型粗化上限 8 m，更粗源档保留自身；边界法线使用共同 stencil。float 几何不覆盖有效范围时拒绝 bake。|
| [#122 渲染](terrain-p2-122.md) | `codex/terrain-p2-122` / `35a42af` | 733 | 696 + 37 SKIP | 7/7 | 默认静态源档；同时修正 RT 材质剔面。逐像素参照针对本机 RTX，跨设备仍需 readback。|
| [#123 碰撞](terrain-p2-123.md) | `codex/terrain-p2-123` / `1a6ef1a` | 722 | 686 + 36 SKIP | 6/6 | 整实例统一精度；decimal 默认候选限制、有效范围外已知 padding 支撑条带需审核。|
| [#144 Lua](terrain-p2-144.md) | `codex/terrain-p2-144` / `8cc5486` | 718 | 682 + 36 SKIP | 6/6 | camelCase 字段、枚举字符串、零基 layer、两个必填参数；每次结果表分配尚未测性能。|

各分支计数对应各自交付点；下游加入更多回归用例，因此不能相加。#122 最终在自己的 worktree 合入总集成 `bafd620`（包含 #123/#144），再冻结、完成四配置验收和提交。总集成快进到 `35a42af` 后再次独立验证全部配置。

## 总集成验证

四轮均显式 reconfigure、完整构建并运行全部 CTest，实际退出码均为 0。

| 配置 | 注册 | 通过 | SKIP | 失败 / 禁用 | smoke | CTest 秒 |
|---|---:|---:|---:|---|---|---:|
| Debug | 733 | 733 | 0 | 0 / 0 | 7/7 | 1087.36 |
| Release | 733 | 733 | 0 | 0 / 0 | 7/7 | 102.04 |
| debug-float | 733 | 696 | 37 | 0 / 0 | 7/7 | 1135.22 |
| game-debug | 733 | 733 | 0 | 0 / 0 | 7/7 | 1079.64 |

总计 2,895 项通过、37 项精度条件跳过、零失败、零禁用；28/28 smoke 通过。新增回归用例为 60 项（P1 基线注册 673 项，最终注册 733 项）。各配置完整 inventory 与 fresh XML 一致，证据汇总 `errors: []`。

505 个产品文件在 `35a42af` 冻结。每轮保留 `configure.log`、`build.log`、完整 CTest 日志、fresh `ctest.xml`、`inventory.json` 和记录实际 CTest 退出码的 `test-state.json`，位于本 worktree 的 `builds/p2-evidence/<preset>/`。测试脚本只在生成的 CTest 文件中包装 GPU smoke；跨进程 mutex 的等待时间不消耗原 native 120 秒执行预算，原命令及参数不变。CPU 测试并行执行。

debug-float 的 37 个跳过项由 `MIR_LARGE_WORLD=OFF` 的既定精度条件触发，包括只验证 double 世界坐标的远处变换、查询、接触与 Lua 用例。float 远处场景另完成实际加载，不能据此宣称 float 的远处接触精度与 double 相同。#144 game-debug 的终端 session 在用户长时间暂停后过期；已核对 helper 持久记录的实际 CTest 0、fresh XML、完整 inventory 和未变源码，没有将“session 过期”记为执行成功证据。

## 测量证据

| 范围 | 实测结果 | 解释与限制 |
|---|---|---|
| 格式规模 | 586×586 tile；格式 2.0 的密集四层索引 164,874,088 B，内存源 Open 2319.38 ms | 非冷磁盘读取；全进程约 714 MiB 峰值包含构建、复制、解析。2.1 扩展还需另计。|
| 过滤 | 四 tile 1/8/2/4 m 原型，double 临时峰值 966,264 B；固定 nonce 的 Debug/Release 文件一致 | 仅所需块及 halo；全发布验证和同步加载仍保留 P1 成本。|
| 最终接缝 | 最大共享边差 3.814697e-6 m，小于数值容差 3.814732e-6 m | 直接 1↔8 m 最大调整 1.816917 m；高细节场景 101.518616 m 触发显著质量告警，调整量不是碰撞误差。|
| 派生载荷 | 四 tile 完整 release 5,992,792 B；payload 为源高度的 17.01 倍；Release bake/validate 5.173/2.628 s | 共边连接环和独立 LOD/Normal 有显著存储成本；不能把小原型外推为全地图预算。|
| 实际 GPU | 160 地形场景 + 32 剔面组合，73,728,000 个地形路径像素；各自未预期 missing/spurious 均为 0 | 真实光栅/RT PNG、线框及 float32 readback；五种视角、两种放置、源档/混合 LOD。|
| GPU 数值 | 各自 reverse-Z 最大误差：光栅 4.667704e-7、RT 3.196004e-8；RT 法线长度误差 1.788139e-7 | 原始跨路径 mask 差 84、距离差最大 6.815470 m，由理想/亚像素量化覆盖的可见面变化解释；未删像素或宣称原始像素相同。|
| 独立 GPU 参照 | 23 实际场景、10,598,400 路径像素；161 次故障注入均被拒绝，旧 RT 剔面缺陷也被拒绝 | 每个像素匹配所属路径 mask，reverse-Z 门槛 2e-6；本机量化行为须由其他 GPU 重新实测。|
| 动态跨界 | 最大单步速度变化 0.130298287 m/s，最大离地 0.002434194 m | 小于批准的 0.3 m/s、0.005 m；球/箱、双方向、多种航向、边及角点，阈值未放宽。|
| 丘陵接触 / 射线 | 最大绝对接触偏差 0.020225032 m，最大射线误差 0.011245949 m | 小于 5 cm；每 LWC 配置 300,060 个实际射线，float 近处 250,050 个。|
| 生产 mixed-float | 约 (150000.5,-20,-75000.25) 实际加载四 mesh、一 HeightField；1 m 总误差界 0.044749764 m | 8/4/2 m 按原预算拒绝并细化。余量约 5.25 mm，未缩小振幅、禁用碰撞或放宽门槛。|
| Lua | 7 个新增用例，10,214 个断言；245 探测点、全部状态逐字段对照 C++，查询期间零 IO | Loading 等状态由受控驻留资源构造，未提前实现 streaming。|

GPU 完整矩阵 SHA256：`d00cd5f2d904601366745b0801b30f0bd56c6850fc8d05028a800d39cd8f7138`。本次原始 GPU 文件保存在 #122 worktree 的 `builds/p2-evidence/gpu-full-float-viewport-release/`，junction 指向 `E:/CodexTerrainP2/run-20261006/issue-122`；其他 issue 的完整测试及测量证据也保留在各自本次 worktree。真实 GPU 派生图、数值分析及独立参照 proof 已随 #122 提交。

![混合 LOD 的真实 GPU 光栅深度](terrain-p2-122-raster-depth.png)

![混合 LOD 的真实 GPU 光追距离](terrain-p2-122-rt-distance.png)

上图分别为 reverse-Z 和米，颜色不可直接比较。±150 km 验收指地形与附近相机共同放置在远坐标，不是相机相对 150 km 距离，也不是 150 km×150 km 地图全域常驻验收。

## 需要审核的设计决定

1. **接缝质量策略**：采用 smoothstep + Coons、2×粗 spacing/半 tile 边带、32 epsilon 的连续性余量和 0.5 m 调整告警。连续性失败阻止发布，质量告警允许发布；C0 不承诺 C1。质量 sidecar 未进入发布 release。
2. **派生数据成本及兼容性**：所有 LOD 保留共边折线；边界法线按相邻 tile 最粗源档 stencil 共享，角点包含所有 incident tile。原型粗化上限为 8 m；源档已更粗时，只保留其自身档 LOD。运行时渲染要求带 LOD/Normal 的 2.1；读取器可读 2.0，正常 sync 会重新烘焙。LOD 17.01× payload 成本仍需后续优化。
3. **decimal 的明确失败**：bake 后的 float XY 必须覆盖有效地图；例如 0.7×256 的完整边界可能因向内舍入被拒绝。碰撞默认 8/4/2/1 m 也要求 tileSize 兼容；支持显式 `TerrainCollisionPolicy` 自定义相容档位，不悄悄增加更细档或放宽误差。
4. **地图外碰撞支撑**：统一粗网格的完整边界 cell 可延伸到已知源 padding。部分边界原型在 8 m 档有最多 6 m 支撑条带、4 m 档最多 2 m；有效地图外查询仍为 `OutsideMap`，没有相同的 render/query 范围。这个原型行为及 P3 边界所有权需要审核。
5. **整实例碰撞和策略更新**：一个细节 patch 可以迫使整实例使用细档；5 cm 总界包括几何、实际 Jolt 解码量化、水平漂移和世界坐标数值预算。World policy 修改不会自行触发重建；P3 #128 才拆形状、处理多个兴趣源和安全替换。
6. **渲染验收与成本**：2e-6 GPU 门槛是本次实现决定，物理门槛沿用批准方案。RT 剔面修复影响既有材质；逐像素参照目前只在 RTX 3070 Laptop 实测，未覆盖近裁剪面相交几何。额外 RT payload 的正常帧成本、BLAS 上传和显存尚未单独量化，静态 LOD 选择不处理运动中的跳变。
7. **Lua 结果结构**：`world:terrain_height_at({x,y,z}, requiredSpacing)` 精确接收两个参数；camelCase、枚举字符串、零基 layer、64 位 ID 的 16 位小写十六进制字符串。未知高度/误差为 nil，真实零值保留；无地形为 `OutsideMap`，非法输入记录错误并返回 `Failed/InvalidInput`。结果表分配性能尚未测。

## 审计与后续

总集成合并后的初次审计：52 个既有作者头、81 个新增或修改的 C/C++/shader 工作文件 CRLF、2 个 shader BOM 检查通过；无 AI Co-Authored-By，`git diff --check` 通过，AGENTS 路由 Validate OK。完整测试后再次核对了 505 个冻结产品文件，未变；本地最终文档提交后的审计保存在 `builds/p2-evidence/integration-author-crlf-audit.json`，测试汇总为 `integration-final-summary.json`。

本地中文 PR 描述、分支、commits、原始证据均保留；尚未 push。上述设计决定供审核，不改变本次批准的物理验收门槛。P3 后续范围仍为区域拆分、兴趣源驻留/驱逐、多形状接触和 LOD 调度。
