# Terrain P2 #123：跨界查询与统一 HeightField

## PR 描述

混合源精度地形现在从已调整的最终源曲面建立整实例统一采样 HeightField，按可配置的 8→4→2→1 m 顺序选择总垂直误差不超过 5 cm 的最粗档。统一节点先按实际 tile owner 对齐；几何界覆盖真实规则三角面，再加入已创建 Jolt 每个有效解码顶点的高度误差、XZ 与 spacing 舍入和世界坐标数值预算。超限候选被销毁并继续细化；全部失败或实际 XY 支持漏掉有效地图范围时拒绝加载。

P1 同步加载、整实例形状和 Edit/Play release 固定方式保留。物理模块只暴露通用数值 metadata，Jolt 仍为私有依赖；terrain 不依赖 physics。测试专用 terrain_bake 链接复用本次真实过滤、最终接缝 fixture，不进入运行时引擎。

## 误差证明与配置

- 每个唯一 source tile 的原始最终三角面与实际均匀三角面进行 XY 覆盖比较，包含 clipping、所有交点和数值余量；不引用边圈 LOD 的误差。
- 二进制 spacing 的精确嵌套上采样保留同一 canonical 对角线，每个细三角包含于单个源三角；线性差的顶点极值加保守数值余量足以包络整面。只有同源 origin/extent、有限高度、精确 power-of-two ratio 才使用快路径，shared owner 完成后测量；全部已知 padding 纳入可能更保守。decimal 与降采样仍完整 XY overlay。高频小网格 fast/full 界均 4.76891614e-7 m，16,641 个 dense 点最大 4.76837158e-7 m。
- Jolt GetPosition 对所有有效输入节点测量 Y 解码偏差和相对于 float spacing 的 XZ 偏差；double spacing→float 的累计 scale drift 单独加入。
- 位移到高度的预算使用 source 与共享 owner 调整后均匀三角梯度的 L1 上界，覆盖所有已知 padding。
- 世界平移舍入和 WorldReal 加法预算单独计入；实际解码矩形必须覆盖原有效 bounds，缺口无法由垂直误差抵消。
- TerrainCollisionPolicy 是可选 World resource，默认 spacings={8,4,2,1}, maxError=0.05；spacing 必须正、有限且严格递减，门槛必须非负且有限。它是原型配置，在 terrain Load/reload 前设置，本阶段未提供 policy 的运行中自动重建。

## 验证

代码、头文件和测试于 2026-10-07 01:12:19 UTC 冻结，485 个文件的 SHA-256 在全部配置结束后保持不变。每个独立 builds/<preset> 显式 reconfigure，再完整 build、完整 CTest；最终新 XML、inventory、Passed state 和四个实际终端 exit 0 一致。使用本次 p2-run-gpu-lock.ps1：CPU 四路，GPU 跨 worktree 互斥；排队不占用 smoke 的原生 120 s 超时。没有缩减测试或改动产品验收阈值。

| 配置 | 注册 | 通过 | SKIP | 失败 / 禁用 | smoke | CTest 秒数 |
|---|---:|---:|---:|---:|---:|---:|
| Debug | 722 | 722 | 0 | 0 / 0 | 6 / 6 | 1420.91 |
| Release | 722 | 722 | 0 | 0 / 0 | 6 / 6 | 106.85 |
| debug-float | 722 | 686 | 36 | 0 / 0 | 6 / 6 | 1246.41 |
| game-debug | 722 | 722 | 0 | 0 / 0 | 6 / 6 | 833.12 |

新增 11 个碰撞用例；float 的 36 个 SKIP 包含既有 35 个远精度用例和新增 1 个远地形碰撞用例，不计作通过。新增 fixture 复用 mixed-2x2 recipe（tile 256 m、负坐标和 partial bounds）；丘陵由真实 BakeFinalHeights 生成，平面采用解析仿射高度的 float32 源节点并执行真实 AdjustTerrainSeams。内存 release 仅含所需源 payload，不以旧 release 或旧实现作参考。

覆盖同精度、1↔2/4/8 m、四角、整条边非共享采样点和闭合外角；验证 resident 查询零读盘、source spacing/status/release、Jolt padding/stride/holes/真实解码和 metadata 销毁生命周期。网格不兼容与预算拒绝分开验证：25.6 m tile 的默认四档均报网格不适配，适配 policy 的 0.8 m 成功；零预算的四档均创建、测量、拒绝并销毁，无 body 泄漏。另有向内收缩 decimal 支撑拒绝、非法 policy、缺失/异 release 源以及合法最高 signed tile 坐标的回归。

### 候选误差

四源 1/8/2/4 m 丘陵在 Release 的真实候选测量如下，误差单位均为 m。前三档超限后正常细化至 1 m；日志为 INFO，全部失败才报错。形状字节数只计 Jolt shape，不包含源驻留、临时采样或构建峰值。

| spacing | 几何界 | 实际量化界 | XZ 高度预算 | 数值预算 | 总界 | 结果 | shape 字节 |
|---|---:|---:|---:|---:|---:|---|---:|
| 8 | 1.503799443045 | 0.022811889648 | 3.883411e-13 | 1.140607e-12 | 1.526611332696 | 拒绝 | 16199 |
| 4 | 0.358013160225 | 0.013481140137 | 3.863185e-13 | 1.140607e-12 | 0.371494300363 | 拒绝 | 44451 |
| 2 | 0.136409775144 | 0.007965087891 | 3.853078e-13 | 1.140607e-12 | 0.144374863037 | 拒绝 | 176074 |
| 1 | 0.000006680322 | 0.003921508790 | 3.853117e-13 | 1.140607e-12 | 0.003928189112 | 选中 | 700076 |

Debug/game-debug 的四源 1 m 总界为 0.003935818507 m；debug-float 近处为 0.004161237443 m（包括 0.000206862620 m 水平预算和 0.000018556318 m 数值预算）。同源 4 m 丘陵选择 4 m，Release 总界 0.013626107857 m；平面选择 8 m，Release 总界 0.003455198517 m。适配 decimal policy 的 Release 0.8 m 总界为 0.000157540718 m，包含 6.37784302e-8 m 的 spacing/XZ 高度预算。

### 查询、射线与动态接触

- 每个丘陵 fixture 有 50,010 次射线：全长两轴的 8,001 个点各取 -1e-6 / 0 / +1e-6 m 偏移，另加 2,000 个随机点和四个真实外角。每个 large-world 配置共六个 fixture、300,060 次；float 五个近场 fixture、250,050 次。命中与最终源曲面比较，同时检查保守总界和 5 cm 门槛。全配置最坏射线高度差为 0.011245949 m（同源 4 m）。CPU 跨边最坏差约 3.80659e-6 m，低于对应 3.81473e-6 m seam 数值容差。
- 平面以 12 heading、双向、sphere/box，分别穿越两个独立边和精确公共角点。每 fixture 128 条有效路线，排除与被测单边平行的路线；每条路线都断言第 45 步之后真实跨界，合计 160 次测量窗口内跨界。起点距目标 8 m，初速 8 m/s；150 步、1/60 s，保留 P1 的接触间隙测法。各 large-world 配置五个 fixture 共 640 条路线，float 四个近场 fixture 共 512 条。
- 全配置最大单步速度变化为 0.130298287 m/s（门槛 0.3），最大正离地间隙 0.002434194 m（门槛 0.005），最深平面穿入 0.011490268 m（原有穿入门槛 0.03）。丘陵 sphere/box 下落后 600 步的最后 60 步接触观测最大绝对间隙 0.020225032 m，满足 0.05 m；该值包含接触求解的穿入，不能解释为仅形状几何误差。
- large-world 另在 (150000.5,-20,-150000.25) 验证四源射线与平面动态接触。float 该用例明确 SKIP；其真实 smoke.TerrainCheck 在 (150000.5,-20,-75000.25) 仍完整加载四个网格和一个 HeightField，实际 1 m 总界 0.048488390045 m = 几何 3.82395318e-6 + 量化 0.004432678223 + 水平 0.044028310340 + 数值 2.35775290e-5，smoke 通过。其场景是本分支的同源 4 m relief；#122 四源 v2 场景需在合入依赖后重新验收。

证据位于本次 worktree 的 builds/p2-evidence（junction 到 E:/CodexTerrainP2/run-20261006/issue-123）：final-summary.json、final-freeze-check.json、freeze.json、collision-measurements.csv、support-rectangles.json、float-terraincheck-engine.log 和各配置的 ctest.xml / inventory.json / test-state.json / LastTest-full.log。CSV 的 225 行来自完整 LastTest.log，避免成功用例的 JUnit stdout 截断。freeze.json SHA-256 为 7ea897d345d515bf08e5ccecc0f10aa55b1805a0d0716dd123737c27da58a4c1。

## 部分地图的物理支撑范围

有效 bounds 为 X=[-250,256]、Z=[-256,250]。uniform builder 沿该候选网格取包含边界的完整 cell，并读取已经生成的 source padding；没有裁切 HeightField 到有效 bounds。默认 binary spacing 的实际节点与 Jolt XY 值精确一致：

| spacing | local 支撑 X | local 支撑 Z | 有效范围外的碰撞条带 |
|---|---|---|---|
| 8 m | [-256,256] | [-256,256] | minX 外 6 m、maxZ 外 6 m |
| 4 m | [-252,256] | [-256,252] | minX 外 2 m、maxZ 外 2 m |
| 2 / 1 m | [-250,256] | [-256,250] | 无 |

平面选择 8 m，同源 4 m 丘陵选择 4 m，本批四源丘陵选择 1 m。Jolt 方形存储补齐的额外节点为 hole，不再产生外圈地面；上表是已知 source padding 形成的完整 boundary-cell 支撑。TerrainHeightQuery 仍先经 AddressLocal 检查有效 bounds，地图外返回 OutsideMap；外部角色可能接触这部分没有有效查询、且可能没有对应渲染的物理条带。5 cm 几何总界适用于有效 bounds，未扩大其查询语义。保留该 P2 整实例边界行为属于需审核的限制，邻接实例和 P3 多形状须单独处理裁切与边界所有权。

## 风险与需审核决定

- 默认四个物理 spacing 必须能形成该 tileSize 的精确网格。合法 P1 decimal tileSize（例如 25.6 m）仍可烘焙，app 默认候选会明确拒绝；调用方可配置适配的 0.8/0.4/0.2/0.1 m 候选。不会在 1 m 预算失败后隐式使用更细兜底。该默认兼容约束需要审核。
- 适配 decimal spacing 的真实 Jolt XZ 支持若向内收缩，仍拒绝；0.9×256 的回归保存该行为，不添加隐藏重叠或边界扩大。
- 一处细节可能迫使整个实例采用 1 m，单形状上限只作用于碰撞构建；recipe 参考网格没有这一限制。同步构建、整实例内存和加载时间仍需 P3 #128 区域拆分解决。
- 当前同源 4 m TerrainCheck 在 float 配置的远放置处实际选择 1 m，四项总界 0.0484883900 m 距 5 cm 仅余 1.51 mm；加载成功不构成 float 的 ±150 km 接触精度承诺，远接触用例仍明确 SKIP。#122 合入四源 v2 场景后须重新实测，不放宽门槛。
- 这些自动化证据证明本批代表曲面的误差与接触；跨形状安全替换、多兴趣源、streaming 和产品长期接触调参属于 P3。
