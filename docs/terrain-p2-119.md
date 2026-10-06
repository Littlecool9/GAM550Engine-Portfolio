# Terrain P2 #119：逐 tile 源精度交付记录

## 中文 PR 描述

标题：烘焙逐 tile 单值源精度与固定参考高度过滤层级

统一精度 recipe 无法表达直接 1↔8 m 邻接或独立编辑 tile。本次新增 recipe v2 的显式默认精度和逐 tile 单值表，严格拒绝重复、缺失、越界及不嵌套规则；从固定参考网格过滤完整高度，按指定源精度写入 format 2 的稀疏层，查询与报告保留实际源精度。recipe v1 的指纹和原样本保持不变。

Debug、Release、debug-float、game-debug 已显式重新配置、完整构建并完成全部测试。每套注册 688 项、零失败，6 项 smoke 全通过；debug-float 有 35 项现有远坐标测试跳过。新增 12 项精度用例覆盖规则、过滤、独立编辑、顺序、范围读取、元数据及边界回归。未 push。

## 基线与设计决定

分支 `codex/terrain-p2-119` 基于总集成中的 `8135fe1`（#118 功能基线加本次 #145）。没有读取 C 盘旧实现。固定方形 tile 网格和唯一源规则按已批准的架构执行；256 m 仅为 #24 原型尺寸，另有 128 m 配置验证。

- 新 recipe 必须同时声明 recipeVersion 2 和 mir.relief generator version 2。v1 继续使用 generator/canonical version 1，保留指纹 `288219ec4e313cb7` 及所有 golden 样本；v2 使用 canonical version 2，明确隔离新的过滤语义。
- `layout.sampleSpacing` 是固定过滤参考 spacing，`layout.intervalsPerTile` 决定方形 tile 大小；参考网格不随当前最细 tile 改变，也不要求将参考高度存入 release。
- `precision.defaultSpacing` 必填，数字只填未赋值 tile，null 要求显式完整覆盖。`precision.tiles` 必填，每项为 `{x,z,spacing}`；相同 tile 重复赋值即错误，不能靠顺序或优先级覆盖。
- spacing 必须为参考 spacing 的二倍整数层级，并整除参考 intervals。这是本次过滤原型的具体约束；1/2/4/8 m 是测试候选值，不是永久产品列表。每档还须实际构造 TerrainLayout 并通过坐标范围及逐 bit 网格嵌套校验。
- 单值规则按 `(z,x)` 排序。直接 1↔8 m 时索引仅需参考/1 m 与 8 m 两层，不增加中间 tile 规则；2/4 m 的过滤中间值仅为临时计算。
- 全高度过滤包含倾斜、fBm、ridged 和平滑地面结果，使用全局 halo 与逐档可分离 `[1,2,1]/4` 核；所有中间高度为 double，只在最终源样本存储时舍入 float32。没有截断或重新归一化噪声 octave。
- 过滤按最多 32×32 个目标样本的局部块计算，完成一块即释放缓存；不保存全域最高精度 release。参考网格的 32768 样本/边限制从 recipe 移除，现有碰撞构建仍保留该上限。format 2 的 uint32 源记录数量上限继续校验。

上述 schema、二倍 spacing 约束、版本兼容策略及块大小为本次自行确定的接口决定，供集成审阅。

## 下游接口

`TerrainRecipe::SourceSpacing(tile)` 返回该 tile 的规则值，地图外为空；`SourceLayout(tile)` 返回对应样本布局；`SourceLayers()` 返回参考层和声明过的源 spacing，细到粗；`SourceEstimates()` 用整数 intervals/ratio 计算每档 tile 数、样本数及 float32 高度字节，避免小数除法截断。

`TerrainGenerator::Height(x,z)` 仍为未过滤的完整高度公式。新增 `SampleHeight(layout,sample)`、`FillTile(layout,tile,out)`、`FillRange(layout,range,out)` 按传入的嵌套 spacing 过滤；全局 halo 通过原始 Height 获取，包含地图外邻域。`SourceFillResult` 记录 raw Height 调用次数和过滤临时 double buffers 峰值样本数。原单参数网格接口继续对应固定参考网格。

`BakeFinalHeights(recipe,report)` 在本阶段输出每 tile 的源样本块，块的 n 可以不同；#120 将在此接入权威最终边带调整。新增 `EncodeBakedRelease(asset,fingerprint,recipe,tiles,options,report)` 根据规则将块写到各自稀疏层，全部标记为唯一 source。旧 uniform layout 重载继续可用。源规则保存在 recipe 中，独立于后续 seam/LOD 派生。

报告版本为 3、tool version 为 5。tile 摘要分别包含 sourceSpacing、payloadSpacing、samplesPerSide、layer 和 source；sourceEstimates 与 filter 统计单独输出。validate 会核对每个源记录与 recipe 规则。

## 验证结果

| 配置 | 注册 | 通过 | 跳过 | 失败 | smoke | 完整 CTest 耗时 |
|---|---:|---:|---:|---:|---:|---:|
| windows-msvc-debug | 688 | 688 | 0 | 0 | 6/6 | 153.23 s |
| windows-msvc-release | 688 | 688 | 0 | 0 | 6/6 | 84.35 s |
| windows-msvc-debug-float | 688 | 653 | 35 | 0 | 6/6 | 92.33 s |
| windows-msvc-game-debug | 688 | 688 | 0 | 0 | 6/6 | 82.55 s |

四份 JUnit 与 inventory 均已核对，零 disabled 项。新增精度组为 12 cases /315 assertions（Debug 单独回归），包括极限 int32 范围的 coarse layout 拒绝、小数 0.1/0.8 m 网格 43 intervals→44² 样本回归、规则逆序与烘焙逆序、纯平面过滤、独立点递推对照、同档共享边 bit 一致、直接多档源索引、精确 payload range、查询零 IO、source limit 状态及约 ±150 km 放置。新增 mixed far 查询断言仅在 large-world 配置执行；float 配置继续验证规则和本地查询。

原有全量测试同时覆盖 P1 查询/网格/接触、损坏文件、发布/sync/package 及六项 GPU smoke。CMake 测试列表改动后显式 reconfigure；发现审查回归后完成重新构建和全测循环，交付仅采用最终 688 项日志。未改任何已有作者文件头，修改文件保持 CRLF，未使用 sed，`git diff --check` 通过。忽略的 AGENTS 路由说明已同步且 check.ps1 Validate 为 OK。

证据为本 worktree `builds/p2-evidence/<preset>/configure.log`、build.log、ctest.log、ctest.xml、inventory.json；它们不加入 Git。

## 原型报告与测量

`tests/fixtures/mixed-2x2.terrain.json` 使用参考 1 m、256 intervals，bounds 为 X[-250,256]、Z[-256,250]。规则为 `(-1,-1):1m`、`(0,-1):8m`、`(-1,0):2m`、`(0,0):4m`，包含负坐标、部分边界与直接 1↔8 m。

| spacing | 每 tile 样本 | 高度 payload 字节 |
|---:|---:|---:|
| 1 m | 257² = 66049 | 264196 |
| 2 m | 129² = 16641 | 66564 |
| 4 m | 65² = 4225 | 16900 |
| 8 m | 33² = 1089 | 4356 |

四块合计 352016 B 高度；索引 808 B，四 packs 含 header 后合计 352208 B，全部 release 文件 353016 B。filter.referenceSamples 为 284863，包含参考档直接生成及各过滤块重复 halo 的 raw Height 调用次数；peakDoubleBytes 为 966264 B，仅为过滤临时 double buffers 的最大尺寸和，不是进程 RSS，也不包含最终 tile 或编码数据。

固定 nonce `1190000000000119` 的 Debug/Release CLI 都得到 fingerprint `341ed20a24f3efd8`、heightsHash `ae24a2b51ca824da`、release `df2a87ce01c9cf18`，全部 release 文件 checksum 和过滤统计相同。CLI JSON 与 candidate 保存在 `builds/p2-evidence/prototype/windows-msvc-debug*`、`windows-msvc-release*`，可使用 TerrainTool bake 指定上述 fixture、id-root `games/TerrainFixture/assets` 和 source-path `terrain/mixed-2x2.terrain.json` 重现。

## 风险与后续范围

本阶段过滤源之间尚未做最终公共边/角点调整。validate 对同层实际邻接样本继续检查，对存在混合邻接的 edges/corners 明确报告 skipped，并注明由 #120 接入；这里的测试通过不代表混合曲面连续性 gate 已完成。#120 必须迁移该 pending-seams 测试到实际连续性验收，源生成独立编辑测试已直接验证 TerrainGenerator，不约束合法的相邻最终边带修改。

#122/#123 接入前，app 的原有统一参考层网格与碰撞路径不适合 mixed recipe；本次只验证源索引及 CPU query 元数据，smoke 继续使用 P1 uniform recipe。运行时渲染 LOD、接缝法线、混合碰撞、streaming 和规则绘制 GUI 均不在本 issue。

较大 tile 或 spacing 比率会扩大 halo 计算量和临时内存；原型 966264 B 不能推广为任意 recipe 的内存上限。本次没有全图烘焙性能承诺，后续可在保持纯函数结果的前提下优化离线调度与缓存。部分地图边界仍存 padding 样本，实际可见范围由 index.validExtent 与已有曲面规则决定。
