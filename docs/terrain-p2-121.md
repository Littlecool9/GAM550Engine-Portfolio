# Terrain P2 #121：最终曲面的 LOD、法线与误差

PR 描述：从 #120 的最终调整高度烘焙每 tile 的粗 LOD、兼容边界法线及保守垂直误差，所有载荷独立寻址。format 2.1 追加源高度校验和与拓扑身份；发布时从最终源重派生验证，拒绝误差、拓扑或法线顺序不符的候选。保留每 tile 唯一源精度及 P1 同步预载。

## API 与版本

- `TerrainReleaseHandle::ReadLod(TileKey, layer)` / `ReadNormals(TileKey, layer)` 各只读对应字节范围。`TerrainLodSurface` / `TerrainLodNormals` 附完整 `TerrainReleaseInfo`、tile、sourceSpacing、spacing、layer/level、源 checksum 和拓扑 hash；两者的拓扑与顶点顺序必须一致。只需已校验索引，源 Height 与相邻 tile 无需驻留。
- `EvaluateTerrainLod` 查询实际连接环三角面。`TerrainLodDeviation` 对实际 float32 位置及最终源面求 XY 覆盖交点极值；`TerrainRegularDeviation` 对 double 坐标的规则查询面另求误差。规则 Height 查询从不套用连接环误差。
- format major 2、minor 1，索引记录 144 字节（原 120 字节尾部追加 sourceChecksum、topologyHash、vertexCount、indexCount）。继续读取 format 2.0；不读取 format 1。Height 内容保持 v1；LOD/Normal 各用 80 字节 v1 头。Region 仍保留。
- tool 7、report 5（tiles 数组按 payload key 列出各 kind，source 计数独立筛选）、recipe v2 canonical 4，派生策略 v1/初始最大 spacing 8 m 纳入 v2 fingerprint。v1 源 fingerprint 与源样本 golden 不变；sync 的 minor 检查重烘旧 release。提交 fixture 更新为 2.1。

## 设计决定

1. 初始 LOD 从 source spacing 逐二倍到 8 m，要求参考网格整除且层级嵌套。源比 8 m 更粗时只烘焙源自身档。8 m 是 P2 原型上限，可通过派生版本/配置、fingerprint 和重新烘焙扩展；v1 保留源 identity 的同时也须显式触发能力版本失效/重烘，不能只改常量让旧 current 继续 no-op。不声称补回未生成细节。
2. 每个 LOD 都保存相同公共边节点：相邻源网格的节点并集（嵌套网格即最细邻接 source spacing）。粗内部沿既有对角线，边界三角形通过拆分连接细边。共同节点高度从最终面统一所有权读取，再存 float32；同级连接环也实际测误差。
3. 法线使用最终面中央差分，边/角的步长是所有 incident tile 中最粗源 spacing，内点用自身源 spacing。所有 LOD 复制相同 stencil 结果；地图外只采用合法域内单侧差分。法线统一可保持 C0 接缝光照兼容，不宣称高度面 C1 连续。
4. 独立非源规则 Height 按最终源节点降采样，并保存自身误差与源 checksum。源 Height 相对自身的误差为零；渲染 ring、规则查询面各有独立 bound。法线载荷中的 bound 是其关联几何 LOD 的 bound，不是角度误差。
5. 误差比较覆盖交叠三角形的多边形顶点、边交点和有效域裁剪端点。source-cell bbox 索引限制候选；完全相同的 canonical 三角形直接证明相等。不确定分类使用向外舍入 bbox 四角的 affine 差，病态条件退回高度范围。额外预算为 `4096 * double_epsilon * max(1, |height|) * (1 + tileSize/sourceSpacing + max(|originX|,|originZ|)/sourceSpacing) * maxTriangleCondition`，最后向上舍入；非有限计算拒绝。抽样仅作独立检验。
6. 新增需审核的限制：实际 float 网格必须覆盖有效域，否则烘焙失败并报告支持范围和缺失宽度。某些原先合法的完整 decimal tile 因向内舍入被拒绝；例如 0.7 m × 256 的 179.2 m 边会缺约 3.051758 µm。不会改源规则或添加重叠条带掩盖空缺。有效域仍在实际支持内的 partial decimal 可烘焙。

## 验证与测量

新测试覆盖平面、脊线、高频、混合 1/2/4/8、负坐标、partial/decimal/负 150 km 地形坐标、共边/角法线、单独 coarse range、规则查询面、交叉对角线极值、小尺度 double 平面、float 支持缺失与完整重封身份后的法线重排发布拒绝。快速轮最终 12 个新用例 / 44,453 个断言通过；运行时 85 个用例 / 233,734 个断言通过。新增缩减层损坏候选会明确失败，缺失源时不做邻接重派生。即使全部 typed payload 被删除，绑定源 checksum 的查询 Height 仍要求重派生，伪造误差失败。

代码/测试冻结于 **2026-10-06 18:58:34 UTC**，实现 diff SHA256 为 `92e0e32055af85c7b08eaaeabe756e35056a07f773c66bf34f76008a477a80ec`（提交前快照；文档不参与代码冻结）。最终验证显式 reconfigure + 完整构建，CPU CTest 并行 4，所有 smoke 使用 `MjolnirTerrainP2Gpu` RESOURCE_LOCK，整轮另持全局 mutex。未禁用或过滤任何测试；旧轮归档为 Superseded，不作通过证据。最终四轮 helper session 均 exit 0，状态为 Passed；仅采纳启动之后的新 JUnit/inventory，四轮均 711 项注册、0 项 disabled、0 项失败。日志位于本 worktree 的 `builds/p2-evidence/<preset>`，prototype 的 release 文件、JSON、耗时与分类型字节位于 `builds/p2-evidence/prototype`。

| 配置 | 注册 | 通过 | 跳过 | 失败 | smoke | CTest 实际用时 (s) |
|---|---:|---:|---:|---:|---|---:|
| Debug | 711 | 711 | 0 | 0 | 6/6 | 699.13 |
| Release | 711 | 711 | 0 | 0 | 6/6 | 66.98 |
| debug-float | 711 | 676 | 35 | 0 | 6/6 | 715.55 |
| game-debug | 711 | 711 | 0 | 0 | 6/6 | 953.01 |

float 的 35 项跳过沿用既有远坐标精度规则，没有新增跳过或放宽门槛。全部新 terrain-local double 用例在 float 配置仍运行。四轮统计、完成时间和 XML 时间保存在 `builds/p2-evidence/final-summary.json`，各轮 runId 由对应 test-state.json 保留。

### 固定 nonce 原型

对 mixed-2x2（256 m tile、1/2/4/8 m、负坐标与 partial 边界）使用 nonce `1210000000000121`。Debug/Release 的 releaseID 为 `2fd77229988392bc`，heightsHash 为 `b11100a30fc0c6a4`，sourceFingerprint 为 `7dd6f7b856b4d171`。源 4、规则查询 6、LOD 10、Normal 10，共 30 条索引记录。优化前后 Debug/Release 四组的全部 index/pack FNV 校验和以及包含 candidate.json 的全部候选文件 SHA256 均相同（6 个文件）；误差和法线数值路径保持确定。比较详情保存在 `prototype/determinism-comparison.json`。

| 载荷 | 字节 |
|---|---:|
| 权威源 Height | 352,016 |
| 规则查询 Height | 113,432 |
| LOD | 4,121,660 |
| Normal | 1,400,636 |
| index | 4,648 |

| tile / 源 spacing | LOD spacing | LOD 垂直界 (m) | 规则查询垂直界 (m) | 数值余量 (m) |
|---|---:|---:|---:|---:|
| (-1, -1) / 1 | 1 | 1.56225071e-08 | 源面 0 | 1.56225071e-08 |
| (-1, -1) / 1 | 2 | 0.136409775 | 0.136409775 | 1.56225071e-08 |
| (-1, -1) / 1 | 4 | 0.233193413 | 0.233193413 | 1.56225071e-08 |
| (-1, -1) / 1 | 8 | 0.51365234 | 0.51365234 | 1.56225071e-08 |
| (0, -1) / 8 | 8 | 3.8192984e-06 | 源面 0 | 4.60113749e-09 |
| (-1, 0) / 2 | 2 | 9.60822653e-07 | 源面 0 | 7.14833693e-09 |
| (-1, 0) / 2 | 4 | 0.35801316 | 0.35801316 | 7.14833693e-09 |
| (-1, 0) / 2 | 8 | 0.731335647 | 0.731335647 | 7.14833693e-09 |
| (0, 0) / 4 | 4 | 9.58242947e-07 | 源面 0 | 4.56863054e-09 |
| (0, 0) / 4 | 8 | 1.50379944 | 1.50379944 | 4.56863054e-09 |

本原型全部载荷合计 5,987,744 字节，相对 352,016 字节权威源载荷为 **17.01 倍**（新增 5,635,728 字节）；包含 pack header/alignment 与 index 的 release 文件合计 **5,992,792 字节**，candidate.json 另计。显式自包含网格和法线在多 LOD 间重复存储，换取粗档独立读取，属于 P2 实际存储风险。4 个源记录扩成 30 个 payload 记录，单记录从 120 增至 144 字节；不能直接套用 #145 的 format 2.0 大索引基准来推定 2.1 新载荷的内存规模。

原型所有三角形的 cross 条件比值为 1；余量按生产公式和实际源高度计算。同源档 ring 仍有非零界：边界重新存储和浮点插值后应实际比较。不能把 8 m 的最高 1.504 m 渲染误差当作碰撞通过条件；#123 应独立选择满足 5 cm 总误差的统一规则 HeightField。

| release 文件 | 字节 | FNV1a64 |
|---|---:|---|
| release.tindex | 4,648 | `de7290c6100d61f7` |
| chunk_-1_-1.tpack | 4,524,764 | `6af92ec549144be2` |
| chunk_0_-1.tpack | 64,508 | `d25d1a60aefe8b59` |
| chunk_-1_0.tpack | 1,120,700 | `1be34cbaafaa3d8c` |
| chunk_0_0.tpack | 278,172 | `9d9f5828ad2de276` |

Release 栈裁剪最终原型 bake/validate 为 5.173/2.628 s；优化前为 13.052/6.518 s（约 2.5 倍）。优化前 Debug 为 229.947/124.892 s，且当时与旧 CTest 并行；栈裁剪最终 Debug 为 106.732/72.326 s（与两个配置构建及新 CTest 并行，不能视为隔离微基准）。测量包含生产发布前的重派生校验，未弱化验证以减少时间。Clip 使用容量 32 的栈凸多边形，保留原运算顺序，超容量走保守 fallback。

下游契约：#122 使用 `ReadLod`/`ReadNormals` 的源档（level 0）及粗档实际网格，normal[i] 对应 positions[i]；#123 用 `TerrainRegularDeviation` 对最终源与统一碰撞规则网格另测误差，不复用 ring LOD bound。源 checksum 绑定唯一权威源记录，Normal 的独立读取不要求源 payload 驻留。

## 风险

- 索引记录增长，P2 全实例同步预载和衍生验证仍有内存/烘焙成本；没有宣称全地图生产性能，也不承诺约 150 km 整图可同步驻留。P3 需独立预算存储、resident CPU/GPU 载荷和 BLAS。
- 共边 ring 与规则查询粗面不同，消费者必须选对应解码器、对应误差；#122 接入实际网格和法线，#123 另验证实际 HeightField 误差。
- C0 源面可能存在角点坡度尖峰；兼容法线不会消除真实几何折角，须由 #122 光栅/光追验收测量。
- 部分 decimal float 覆盖限制与 8 m 原型上限需用户审核。P3 streaming/Region/每视口调度仍未实现。
