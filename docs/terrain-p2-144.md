# 地形 P2 #144：Lua 驻留高度查询

为 World 增加 `world:terrain_height_at(position, requiredSpacing)`，让脚本读取与 C++ 相同的最终地形曲面。结果完整保留状态、失败原因、地形局部高度、世界点、实际/源 spacing、可选误差、release 身份、tile 和 layer；无高度或未知误差返回 nil。输入通过现有 `WorldPosition::FromDoubleMeters` 校验，64 位身份使用十六进制字符串。

绑定每次读取 `TerrainWorld` 驻留资源，不读取文件、不等待加载、不缓存 release。Play 期间发布新的 release 仍返回原有 release，受控 Edit 安全点替换资源后返回新 release。`engine_script` 对 `terrain` 新增 PRIVATE 依赖，TerrainBindings 在 World 注册后接入；未改变运行时加载策略。

## Lua API

```lua
local result = world:terrain_height_at({x=150000.500123, y=10, z=-150000.250321}, 1)
if result.hasHeight then
    print(result.worldHeight, result.spacing, result.maxError, result.releaseId)
end
```

字段名称为 `status`、`failure`、`hasHeight`、`localHeight`、`worldPoint={x,y,z}`、`worldHeight`、`spacing`、`sourceSpacing`、`maxError`、`releaseId`、`release={assetId,sourceFingerprint,formatVersion,releaseId}`、`tile={x,z}` 和零基 `layer`。完整约定见 [script 模块说明](modules/script.md#terrain-height-query-144)。C++ 结果没有 sample/cell 字段，绑定不虚构它们。

所有状态可区分：MeetsRequirement、CoarserWhileLoading、CoarserAtSourceLimit、Loading、OutsideMap、Failed。失败原因保留 None、WrongRelease、InvalidInput、LoadFailed、InvalidSamples。无 TerrainWorld、空资源或无实例覆盖返回 OutsideMap/None。没有高度时 localHeight/worldPoint/worldHeight 为 nil；spacing/sourceSpacing 无有限值时为 nil；真实零高度为数字 0。所有 64 位身份均是 16 位小写十六进制字符串，未知身份保留 `0000000000000000`。

参数恰为两个：position 是直接包含数值 x/y/z 的表，requiredSpacing 是有限正数。不接受缺省轴、字符串数值、metatable 提供的虚拟轴或多余参数。坐标必须有限且可表示为 WorldReal；错误先于资源查询校验，每次只记录一条 script 错误并返回 Failed/InvalidInput。Double 配置不窄化为 float；float 配置保留其 WorldReal 范围与精度限制。

## 验证证据

代码及测试冻结于 2026-10-06 19:59:23 UTC；最终四配置均从显式 configure 开始，全构建后执行完整 CTest（CPU 并行 4；GPU smoke 串行锁）。Debug 使用旧 helper 的整个 CTest 全局锁；其他配置使用经验证的逐 smoke 全局 mutex 与 CTest RESOURCE_LOCK，保留原始测试名称、命令参数与失败匹配规则；外层 wrapper 等待 GPU 锁不占执行预算，原命令的 120 秒实际运行超时仍由 watchdog 严格执行，超时终止该原命令的进程树。不同配置只在上一配置完成编译后开始编译，CPU 测试可并行；所记秒数为当时调度负载下的 CTest 墙钟时间。这里只统计冻结后有新 XML、inventory、完整日志、Passed 状态及 CTest 实际退出码 0 的完整轮次。Debug/Release/float 另核对工具会话直接退出码；game-debug 的旧会话在恢复后查询过期，使用已完成的持久 Passed/exitCode=0、UTC 时间与完整 XML/日志交叉核验，不声称取得过期会话的终端回包。

| 配置 | 注册 | 通过 | 跳过 | 失败 | 禁用 | smoke | CTest 秒数 |
|---|---:|---:|---:|---:|---:|---|---:|
| Debug | 718 | 718 | 0 | 0 | 0 | 6/6 通过 | 1203.27 |
| Release | 718 | 718 | 0 | 0 | 0 | 6/6 通过 | 64.70 |
| debug-float | 718 | 682 | 36 | 0 | 0 | 6/6 通过 | 1122.97 |
| game-debug | 718 | 718 | 0 | 0 | 0 | 6/6 通过 | 771.66 |

float 的 36 项跳过包含 35 项既有远距离精度测试及本次新增的 Lua 远位置测试；跳过单独统计，不计入通过。六个冻结产品及测试文件的 SHA256 与最终验证前完全一致。

新增 7 项验收测试；冻结前 Debug 定向运行全部通过，10,214 个断言。发布的 format 2.1 测试 release 含 1/2/4/8 m 四种源精度和独立粗查询 payload；在负坐标、部分边界、公共边、四 tile 交点以及闭合地图角点组合 7×7×5=245 次探测，并验证地图外探测。每次 Lua 回答与 C++ 逐字段严格相等，文件和受控内存 byte source 的读取次数与字节数均不变。

Loading、CoarserWhileLoading、CoarserAtSourceLimit、LoadFailed、WrongRelease、InvalidSamples、未知误差及真实零高度使用明确的驻留 fixture 构造；重复 Loading 保持单个 deduplicated payload request。24 种非法调用分别在无资源和有驻留地形时验证，恰产生 48 条错误日志且不访问 release。远位置测试在正负约 150 km 的两处共验证 8 个世界点，使用不能精确表示为 float 的小数；float 配置明确跳过此项。

ScriptHost 的真实 Lua OnUpdate 在 Play 中查询 A，发布 B 后再次查询仍为 A；受控切回 Edit、替换 TerrainWorld 后同一脚本返回 B。测试使用 `fedcba9876543211` 与 `e123456789abcdef`，验证身份超出 Lua double 整数精度范围时仍完整。

Release 和 float 的首次 GPU 排队轮因将 mutex 等待计入 CTest 外层 120 秒超时而失败，均已归档到 superseded-gpu-queue，未纳入通过结果；修复仅涉及忽略的测试调度 helper，冻结产品代码不变。

证据保存在本 worktree 的 `builds/p2-evidence/`：freeze.json、terrain-binding-debug.log，以及各 preset 的 configure.log/build.log/ctest.log/ctest.xml/inventory.json/test-state.json。AGENTS 路由按 agents-md-sync 更新并通过 Validate；提交后审核作者头、CRLF、无 AI trailer 及 clean 状态。

## 风险和待审核决定

- 新 API 采用 C++ 同名枚举字符串、camelCase 字段与零基 layer；增加 hasHeight 和 worldHeight 便于脚本判断，完整 release 元数据仍可追溯。参数采用两个必填实参和直接数值字段，避免静默默认、数值字符串或 metatable 执行。这些是本次实现者的接口决定，供用户审核。
- nil 在 Lua 表中表示字段缺失；调用方应先判断 status/hasHeight，再使用高度和误差。未知身份保留零 sentinel，tile/layer 默认值按 C++ 原样返回，失败时不能据此确定有效位置。
- 绑定没有加载器；P2 保留整 release 同步驻留。受控测试可产生加载状态，但运行中的进度、IO 调度与驱逐仍属 P3。可查询状态会保留 C++ 的 deduplicated 请求队列副作用。
- 保持 `TerrainWorld` 现有线程模型和 app 的 Edit/Play 安全点；测试的资源替换是受控模拟，未增加脚本发布或切换权限。
- 每次查询创建结果和 metadata Lua 表；未对脚本频率、分配开销作性能承诺。float 世界坐标不能承诺远处亚厘米精度，精确的 ±150 km 验收限 double 配置。

本地提交；不 push、不创建远程 PR，不修改 issue 状态。
