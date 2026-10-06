# Terrain P2 #120：混合源精度最终接缝

## 中文 PR 描述

标题：烘焙统一公共边与角点，使混合精度 tile 共享连续的最终高度

原先不同过滤档的相邻 tile 在边中间会产生不同曲面。本次先确定全部邻接 tile 共用的角点和较粗档边折线，再在有限边带用 smoothstep 与角点扣除修正最终 float32 高度。逐 tile 源规则保持原值，mesh、驻留 query 和碰撞样本适配器读取这些最终样本。

校验覆盖公共边的全部断点、裁剪端点与密集 canonical probes；不连续的 candidate 无法发布，原 current 保留。调整超过 0.5 m 时 stderr 与 JSON 显著告警但允许发布。v2 fingerprint 纳入最终算法/权重/带宽，sync 会重烘上一阶段未经接缝调整的 v2 current。P1 recipe v1 高度和身份不变。完成四配置完整构建和测试，未 push。

## 方案与参数

本分支 `codex/terrain-p2-120` 从本次总集成 `0c4516d`（已完成 #145/#119；原始 P1 #118 为 `3a6df0d`）创建。实现只参考本次 fresh worktrees 和批准的 P1 设计，没有读取 C 盘旧实现。

每个 tile 仍保留唯一 source spacing。角点高度取全部地图内 incident tiles 中最粗档的原始过滤样本；边的内部折点取两侧较粗档的原始样本，并先用该公共角点替换端点。这样同档 1 m 邻边遇到另外一个 8 m tile 时，双方第一段仍是同一条曲线；不只覆盖最后那个角点。

内部边以及有必要延续角点的外边都形成目标边曲线。四条边的高度差用 `w=t²(3−2t)` 混合，`t=clamp(1−distance/width,0,1)`，再扣除角点差乘相邻两边权重的四项，避免角点调整重复计入。最终边样本显式写入公共折线在该源网格位置的 float32 值。普通粗边保持原始粗样本，仅必要角点端点及相邻有限带变化。所有边界在任何 tile 改写前快照，保证逆序烘焙不改变结果。

正式参数为 `min(2×edgeSpacing, halfTileSize)`，权重 smoothstep，接缝算法 version 1。实验比较 linear/smoothstep 与 1/2/4 倍带宽。以直接 1↔8 m 的 256 m tile 为例，最大网格坡度：

| 权重 | 1 倍 / 8 m | 2 倍 / 16 m | 4 倍 / 32 m |
|---|---:|---:|---:|
| linear | 0.878807 | 0.800468 | 0.761299 |
| smoothstep | 0.792191 | 0.751495 | 0.741104 |

两倍 smoothstep 比两倍 linear 降低最大坡度，带的边界导数归零；四倍继续改善约 0.0104，但改变的区域加倍。因此采用两倍作为原型默认，不把它视为永久产品常量。边/角折线本身并不保证 C1 连续。

`maxGridSlope` 定义为所有 tile 内水平或垂直相邻样本高度差绝对值 / source spacing 的最大值；`maxAddedSlope` 是 final 与 raw 的该样本差增量 / spacing。角点局部指标仅包括公共角点 ±16 m 方形内且两个端点均在该区域的样本边。它们是离散网格指标，不是三角形梯度范数或碰撞速度测量。

## 连续性与质量门禁

两个源边都是分段线性的。在双方断点并集及地图裁剪端点验证最大高度差，覆盖整段折线；每段另外验证 1/4、1/2、3/4 canonical surface 探针。共同源顶点和四角要求 float32 bit 一致（包含正负零）。不可读 tile 的相关检查报告 skipped，同时文件/覆盖检查失败。

每个探针使用四个 cell 支撑样本的最大绝对值 M，避免过零高度掩盖插值误差。声明容差为 `SurfaceTolerance(max(1,M)) + 32×epsilon64×max(1,M)×(1+max(|x|,|z|)/minSpacing)`。第一项允许最终 float32 插值样本的一次舍入，第二项为坐标减法/除法、两边三角公式及比较的 double 运算提供保守余量；32 是按这些运算的数量与相消传播设置的数值预算，不是视觉或物理门槛。此预算随支撑幅度及坐标/spacing 增长，不宣称任意布局都有固定毫米精度。

质量阈值 0.5 m 基于下表原型：普通 1↔2 m 调整最大约 0.30 m，1↔4/8 m 可达 0.71/1.82 m，后者需要人工检查地形变化。警告不会使检查失败，也不会放宽连续性要求。它量度 final 相对 raw 的曲面改变量；后续 #123 的 5 cm 是碰撞形状相对已调整 final 的误差，两者不同。

每 tile 保存最大绝对调整、位置、带宽、贡献最大边及相关邻 tile。归因将每个 Coons 角点扣除项各半分给相邻边，再选择绝对净贡献最大的边；不以最近边代替成因。零调整项没有有意义的边/极值位置。

候选摘要校验版本、正式权重/带宽/质量政策、完整 tile 覆盖及有限非负数值；正极值必须属于该 tile 的 source 样本网格（允许 padding），并位于归因边的有效带内。缺失 v2 摘要、实验参数或改写阈值不能静默消除警告。recipe v2 编码强制携带摘要；publish 没有 recipe 时也从混合源索引或 source≠reference 识别必需摘要。无法从一层全同档的匿名索引区分 v1/v2，保留 P1 的 null 兼容，此种同档没有调整。

## 数值证据

实验 recipe 为 `tools/TerrainTool/tests/fixtures/mixed-2x2.terrain.json`：256 m tile，固定参考 1 m；bounds X[-250,256]、Z[-256,250]，seed 20261001；base 15 m，tilt X=0.15、Z=0；broad 20 m/512 m/2 octaves，ridges 30 m/192 m/2，detail 1 m/24 m/2，plains height 0/blend 4 m。混合源为 (-1,-1):1、(0,-1):8、(-1,0):2、(0,0):4 m。

直接档位案例将前三/四块设为 1 m，仅 (0,-1) 设为 2/4/8 m；同档角点案例前三块 1 m，(0,0) 为 8 m。高 detail 只将 amplitude 改为 500 m。远坐标案例将全部 tile X 和 bounds 平移 ±586 tile = ±150016 m，并令 tilt X=0，避免绝对高度超过 recipe 限制；这检验 terrain-local 大坐标的数值预算，原有约 ±150 km 世界放置 query 回归同时覆盖更新后的 final 高度。

| 默认 smoothstep ×2 | max 调整 m | max 网格坡度 | 角点局部 max 坡度 | 角点局部新增坡度 | 连续性 max 差 m | max 声明容差 m | 警告数 |
|---|---:|---:|---:|---:|---:|---:|---:|
| 1↔2 | 0.296074 | 0.758621 | 0.479233 | 0.012651 | 3.814697e-6 | 3.814832e-6 | 0 |
| 1↔4 | 0.710617 | 0.755539 | 0.466297 | 0.045588 | 3.814697e-6 | 3.814832e-6 | 1 |
| 1↔8 | 1.816917 | 0.751495 | 0.473503 | 0.150148 | 3.814697e-6 | 3.814832e-6 | 2 |
| 四档混合 | 1.106300 | 0.660994 | 0.448302 | 0.081623 | 3.814697e-6 | 3.814732e-6 | 2 |
| 同档 1 m +8 m 角点 | 1.816917 | 0.745308 | 0.472975 | 0.150148 | 3.814697e-6 | 3.814835e-6 | 1 |
| detail 500 m | 101.518616 | 60.710609 | 51.488972 | 14.612458 | 7.629395e-6 | 1.525912e-5 | 3 |
| X约 +150 km | 0.410202 | 0.496696 | 0.152628 | 0.053335 | 1.907349e-6 | 1.944954e-6 | 0 |
| X约 −150 km | 0.421326 | 0.376577 | 0.241404 | 0.066146 | 9.536743e-7 | 9.790780e-7 | 0 |

四档混合最大调整位于 tile (0,0)、bottom、(188,0) m，带宽 16 m；fine tile (-1,-1) 最大 0.518036 m 位于 right、(0,-220) m。同档 1 m+8 m 角点的局部最大调整为 0.252834 m，新增网格坡度 0.150148 m/m；第一段仍可能形成陡角变化，尤其高频/高幅 recipe，需要审阅图及警告。高 detail 的局部最大调整 32.666293 m，说明连续并不自动意味着可用的地形质量。

48 组完整数据在 [metrics.csv](terrain-p2-120/metrics.csv)；[曲线图](terrain-p2-120/seam-curves.png) 来自真实 raw/final 样本。原始 curves.csv 与每场景 report 保存在本 worktree 的 `builds/p2-evidence/seam-experiment/`。复现：Debug 配置加 `-DMJOLNIR_TERRAIN_SEAM_EXPERIMENT=ON`，构建 `terrain_seam_experiment`，传入上述 recipe 和输出目录；`experiments/plot_seams.py` 使用 matplotlib 绘制曲线。

CLI 实测 high-detail bake/publish 均 exit 0，JSON 各 3 条 warning，stderr 两端均出现 `WARNING`/`QUALITY WARNING`，publish.published=true。证据在 `builds/p2-evidence/warning-cli/verification.json`、bake/publish JSON 与 stdout/stderr 日志。另一个故意改变 fine 边非公共折点的 candidate 会失败，previous current 仍可读取。

## 下游接口与版本

`BakeFinalHeights(recipe,report)` 返回逐 tile 规则网格上的最终 `TileHeights`；`report.seams` 必须随这些最终块交给 recipe 版 `EncodeBakedRelease`。`TerrainBakedRelease::seams` 与 `MakeCandidateManifest` 携带摘要，validate/publish 独立恢复警告。`AdjustTerrainSeams` 的非正式 options 仅供实验，正式 candidate 编码拒绝它们。

format 2.0 与高度 content 1 不变；recipe v2/generator v2 保持源公式，canonical v3 加 finalSeams version/weight/bandMultiplier。v1 canonical/generator/golden 保持原值。tool version 6、report version 4、candidate version 3。

#121 应从最终块重新派生 LOD、法线、高度范围和保守误差，不把 raw 过滤缓存直接当 final LOD。#122/#123 应接入混合运行时网格/法线与统一碰撞选档；本 issue 的消费验证检查 canonical query、单档局部 mesh、collider 样本输入取最终值，混合光栅/光追和接触验收仍由对应 issue 完成。P1 同步驻留保持，未引入 streaming。

## 最终验证

| 配置 | 注册 | 通过 | 跳过 | 失败 | smoke 通过 | CTest 用时 |
|---|---:|---:|---:|---:|---:|---:|
| `windows-msvc-debug` | 699 | 699 | 0 | 0 | 6/6 | 90.81 s |
| `windows-msvc-release` | 699 | 699 | 0 | 0 | 6/6 | 68.34 s |
| `windows-msvc-debug-float` | 699 | 664 | 35 | 0 | 6/6 | 88.08 s |
| `windows-msvc-game-debug` | 699 | 699 | 0 | 0 | 6/6 | 85.13 s |

最终四次 All 均 exit 0，JUnit timestamp 与本轮日志一致；共新增 11 个接缝专项测试。汇总机器记录在 `builds/p2-evidence/final-verification.json`。

四配置显式 reconfigure，完整 build/全部 CTest 含 6 smoke；float 的现有远世界 SKIP 如实保留，没有降低验收阈值。专项覆盖公共边/四角/部分 bounds/负坐标、逆序编码、过零大幅支撑、损坏接缝、质量发布、metadata 门禁、旧 v2 current 重烘、0.1/0.8 m 约 ±150 km 网格、final query/mesh/collider 适配器。证据在 `builds/p2-evidence/<preset>/` 的 configure/build/ctest 日志、JUnit 与 inventory。

没有改任何既有作者头，CRLF 保持，未用 sed，没有 AI Co-Authored-By trailer。忽略的 AGENTS 路由已同步并验证；实验 CSV/图与本文入库，构建、CLI release 和日志不入库。

## 风险与审阅决定

- 需审阅：Coons smoothstep、两倍带宽/半 tile 上限、0.5 m 原型质量阈值、32 epsilon64 数值余量及贡献归因。实测只覆盖列出的 recipe，不构成任意地形的质量保证。
- 同档边的粗角点变化沿首段传播，保持连续性但不保证坡度/法线连续；高 detail 可出现显著坡度，需要后续编辑提示和 #122 法线验收。
- 摘要是离线审计记录，published release 不携带 candidate/report 侧车，必须保留报告复查。有限/身份/几何校验防错误元数据流入，但摘要没有与 raw 全图重新计算或防恶意篡改的独立证明。
- 离线 baker 保留全部实际 source blocks 和边界快照；没有保存全域最细 release，也没有新增全图高精度驻留缓存。本原型不能承诺 150 km 全图烘焙的 CPU/内存开销。
