# Terrain P2 #145：format 2 交付记录

## 中文 PR 描述

标题：地形 release 升级为 format 2，支持稀疏层与独立 payload

旧索引要求所有 tile 使用同一精度，无法表达后续 P2 的逐 tile 源精度及独立派生数据。本次只读写 format 2：新增层表、payload 类型/级别及唯一源标记，允许细层只覆盖部分 tile，并严格校验覆盖、嵌套、范围、身份和校验和。同步更新 release 读取、驻留查询、candidate 清单、validate/sync 消费路径和测试 release；单层样本值保持 P1 一致。

Debug、Release、debug-float、game-debug 均重新配置并完整构建通过；每个配置注册 676 项测试，零失败，6 项 smoke 全通过。debug-float 的 35 项现有远坐标测试按原条件跳过，其余配置零跳过。新增稀疏四档、精确范围读取、非法元数据及逐 tile 误差界验证，并完成 343396-tile 全图索引基准。未 push。

## 格式与设计决定

- 基线为 #118 `3a6df0d`，分支为 `codex/terrain-p2-145`。全部实现只使用本次新 worktree；没有读取 C 盘旧 worktree 实现。
- format major 为 2，minor 为 0，原始 float32 高度 content version 仍为 1。格式 1 明确拒绝；重新烘焙测试 release。
- 索引 header 128 B、pack record 32 B、payload record 120 B、layer record 16 B、checksum trailer 8 B。布局顺序为 header、pack 表、payload 表、layer 表、扩展区、trailer。字段逐字节 little-endian 编解码。
- `TerrainIndex::layout` / `TerrainReleaseHandle::Layout()` 保留为稳定的最细参考网格，即 layer 0；它不能用来推断单个 tile 的源精度。layer 0 可以没有 payload，避免为表达参考网格保存全域最细高度。
- 层从细到粗排序，spacing 按整数倍嵌套，tile 大小、范围及边界坐标一致；边界坐标逐 bit 校验。此校验约束网格几何，不要求不同层高度相等。
- 每个 map tile 必须且只能有一个 `Height` 源记录，并且源记录必须是该 tile 已烘焙的最细高度层。更粗的额外高度记录不标记为源。
- payload key 为 `(z, x, kind, level, layer)`，独立 offset/length/checksum。预留 `Height`、`LodHeight`、`Normal`、`Region` 类型；本次仅解码 `Height/level=0`，其他内容明确返回 Unsupported，后续 issue 扩展读取器。
- `heightsHash` 绑定有序 tile、layer、kind、level、源标记、spacing、样本边长、误差界及 payload checksum；不绑定 chunk 分组或 payload alignment。单层高度样本保持原值，hash 因新定义而改变。
- `maxDeviation` 可选且按 tile 保存；已测值必须有限且非负，未知保持空值。它相对最终源曲面，不相对生成 recipe；#121 提供实际测量值。
- 指定层预加载缺少稀疏 payload 时返回 NotFound，不创建失败驻留槽。未指定层时跳过未烘焙 key。重复请求同一 tile 只读取一次。

上述记录布局、参考网格兼容接口、源标记与未知误差表达是本次自行确定的具体接口选择，供集成审阅。

## 下游接口

`EncodeTerrainRelease(info, layers, records, options)` 接受细到粗的 `TerrainLayout` 列表和 `TerrainTileWrite` 列表；后者包含 `heights`、`layer`、`source`、可选 `maxDeviation`。原单层重载继续可用。`TerrainHeightsHash(layers, records)` 供生成 release 身份使用，单层重载现在也需要 layout。

`TerrainIndex::Find(tile)` 找唯一源记录，`Find(tile, layer, kind, level)` 查独立 key。`ReadTile(key)` 读源，`ReadTile(key, layer)` 读指定高度层；`ReadTileRecord(record)` 要求记录引用来自该 handle 的不可变索引，并且仅读取记录声明的字节范围。

`TerrainHeightQuery::Create(handle)` 只使用索引建立稀疏层，不读取 payload。每层可有 `tileDeviations`；手动 Create 同样校验坐标与误差值。HeightAt 的 sourceSpacing 取该 tile 的最细已烘焙源，maxError 优先采用该 tile 的实测界并包含数值容差。`PreloadTerrainTiles` 用按请求 tile/layer 的二分 key 查找，`PreloadAllTerrainTiles` 直接遍历记录。`TerrainResidentRelease::Lookup(layer)` 明确指定层。

Candidate manifest version 为 2，记录 layerCount 与 payloadCount，并与索引核对。离线 validate 按层解码并检查各层实际烘焙邻接边；细粗源之间的最终接缝由 #120 继续实现。

## 验证结果

所有配置在 CMakeLists 改动后显式 reconfigure；完整 build 与 CTest 通过。CTest 库存中均无 disabled 项。

| 配置 | 注册 | 通过 | 跳过 | 失败 | smoke | CTest 实际耗时 |
|---|---:|---:|---:|---:|---:|---:|
| windows-msvc-debug | 676 | 676 | 0 | 0 | 6/6 | 132.25 s |
| windows-msvc-release | 676 | 676 | 0 | 0 | 6/6 | 65.90 s |
| windows-msvc-debug-float | 676 | 641 | 35 | 0 | 6/6 | 85.81 s |
| windows-msvc-game-debug | 676 | 676 | 0 | 0 | 6/6 | 85.50 s |

新增验证覆盖 1/2/4/8 m 源精度、负 tile 坐标、稀疏细层、独立 range 读取、零 IO 查询、粗层选择加载、缺失/重复源、未知层、尺寸错误、不嵌套、重叠范围、格式 1 拒绝及非法逐 tile 误差界。P1 全量测试继续覆盖部分边界、四角、约 ±150 km 查询/网格/接触、身份、损坏文件、发布/sync 与 smoke。测试 fixture 的原高度样本保持固定，四个 pack 的大小仍各 260 B，索引从 960 B 变为 1240 B。

证据位于本 worktree `builds/p2-evidence/<preset>/`：configure.log、build.log、ctest.log、ctest.xml、inventory.json。最终每个配置都为 676 项，早期 675 项日志不是最终结果。未改作者文件头，保留 CRLF；`git diff --check` 通过。

## 全图索引测量

Release 构建的可选 `terrain_index_benchmark` 用有效元数据构造 586×586 个 256 m tile（150016×150016 m），16×16 tile/chunk，共 1369 packs。它不分配全图高度 payload。每种情况执行三次 `TerrainReleaseHandle::Open`，包含内存 source 的 ReadAll 复制、解析、范围及身份/覆盖校验，排除磁盘冷读、pack 校验、payload 解码和 GPU。

| 情况 | 层 | payload 数 | 索引字节 | Open 最小 | Open 平均 | Open 中位数 |
|---|---:|---:|---:|---:|---:|---:|
| 全图 8 m 源 | 1 | 343396 | 41251480 | 547.167 ms | 550.848 ms | 550.593 ms |
| 10% tile 有 1/2/4/8 m，其余 8 m | 4 | 446416 | 53613928 | 714.923 ms | 723.226 ms | 724.824 ms |
| 每 tile 均有 1/2/4/8 m | 4 | 1373584 | 164874088 | 2246.590 ms | 2305.820 ms | 2319.380 ms |

整个基准进程峰值 WorkingSet 为 748687360 B（约 714 MiB），通过每 100 ms 采样采集；它包括构造数据、source 副本和解析临时量，不能解释为单个解析器的驻留内存。运行环境为 Windows / MSVC 14.51 Release，使用本次构建工具链。证据为 `builds/p2-evidence/index-benchmark.csv`、`index-benchmark-process.json` 和空错误日志。可重现方式：为 Release 配置 `-DMJOLNIR_TERRAIN_INDEX_BENCHMARK=ON`，构建并运行 `terrain_index_benchmark.exe`。

## 风险与后续范围

全图索引当前整体读取并留存，稠密四层约 165 MB，完整校验需约 2.3 s；不能据此承诺 P3 的即时分区加载，分段索引/缓存策略仍需后续设计。当前 Open 不读取高度 pack，按 key 读 payload 的接口已独立。

固定源精度 recipe、最终边带调整、派生 LOD/法线、混合源精度运行时网格/碰撞以及 Lua 仍分别属于后续 #119/#120/#121/#122/#123/#144。本次单层运行时不变；现有整实例同步驻留继续使用，P3 再加入调度与驱逐。当前稀疏层连续性检查只覆盖同层已烘焙邻接对，不是混合源边界的最终验收。
