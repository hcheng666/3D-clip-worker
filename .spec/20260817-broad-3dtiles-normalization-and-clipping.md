# 2026-08-17 广覆盖 3D Tiles 规范化与裁切 Worker Spec

## 1. 文档状态与来源

- 状态：用户已确认，正在按 OpenSpec 分阶段实施。
- 总体方案：`D:/code/api-platform/.spec/20260817-broad-3dtiles-ingest-and-clipping.md`。
- OpenSpec Change：`D:/code/api-platform/openspec/changes/support-broad-3dtiles-clipping/`。
- 本文只约束 Worker/隔离规范化进程的输入、输出、编解码、裁切和验证；持久化编排及网关行为由 `api-platform` 负责。

## 2. 已确认的不变量

1. 授权几何是 EPSG:4490 Polygon/MultiPolygon 沿高度方向无限延伸的棱柱。
2. 任何不能证明安全的边界内容都不能输出源内容或部分伪成功结果。
3. 能全局预览不等于能有限授权；Worker 的 `UNSUPPORTED` 必须被上游聚合为 `GLOBAL_PREVIEW_ONLY`。
4. 支持的元数据必须随保留 feature 完整重建；无法安全重建时拒绝有限授权。
5. 原始对象和旧算法制品不可变，新行为使用新的 normalization/algorithm version。

## 3. 结构边界

保持当前项目结构和 Worker 主程序职责，逐步抽取 typed scene contract，而不是把全部格式分支堆进现有 `B3dmClipper`：

- package/resource inspector：有界读取、magic/JSON 识别和资源闭包验证。
- normalizer：把源内容转换为已验证的 Mesh、Point、Instance canonical artifact。
- clipper：只处理规范化 typed scene 和授权棱柱。
- metadata rewriter：重建 feature ID 和支持的属性存储。
- validator/writer：确定性写出并在上传前复验。

可以作为同一镜像内不同命令或独立进程部署，但任务类型、资源上限、日志和版本身份必须分离。

## 4. 任务输入与资源访问

- Worker 只接收批准的对象清单、对象 identity、大小、hash/ETag、包内逻辑名、短期下载/上传授权和具名限制。
- 禁止获得桶级长期凭据、任意 key 列表权限或任意网络访问。
- 只解析 manifest 允许的相对 URI；禁止 HTTP(S)、file、协议相对和越界路径。
- 所有字节读取、JSON/binary 表、accessor、image 和 decoder 分配先做 checked arithmetic 与配置上限检查。
- 心跳、租约丢失、取消和超时必须中止发布；临时上传只有完整校验后才能原子发布。

## 5. Canonical contract

### 5.1 `MESH_GLTF2`

- GLB 2.0，所需 buffer/image 已内嵌或封装在同一不可变资源闭包。
- Draco/Meshopt 已解码为受支持的 Attribute/index；不保留运行时必须依赖但未验证的压缩扩展。
- 明确 up-axis、tileset/tile transform、RTC、node/scene transform 的职责，不重复 bake。
- primitive topology、component type、stride、normalized、sparse、morph/skin 支持情况均有稳定能力结果。
- 支持的 feature identity 和 metadata 映射可被后续裁切重写。

### 5.2 `POINT_GLTF2`

- 表达位置、颜色、法线、feature identity 和支持属性。
- 量化输入在规范化时以明确精度策略处理，裁切判定使用可验证世界坐标。
- 所有按点 Attribute/metadata 必须同长度、同重排。

### 5.3 `INSTANCE_GLTF2`

- 表达共享模型、每实例 translation/rotation/scale 或矩阵、feature identity 和支持属性。
- 外部 glTF 只能来自批准资源闭包。
- 实例 bounds 的计算必须包含模型、实例和 Tileset 全部变换。

### 5.4 CMPT

CMPT 不作为单一几何场景处理；递归拆成有序 inner content canonical artifacts。嵌套深度、child count 和总字节有配置限制。

## 6. 格式支持规则

### 6.1 GLB/glTF 与 B3DM

- 支持 GLB 2.0 和 glTF 2.0 JSON 及批准的外部 buffer/image。
- B3DM 解析所有 header/table byte length 时使用 checked arithmetic。
- 从已验证的单 batch 子集扩展到多 primitive、多 feature 和支持的 Batch Table，但每个子能力独立启用。
- 不支持的 topology、animation、skin、morph、extension 或 metadata 组合必须明确返回 `UNSUPPORTED_*`，不能忽略后继续裁切。

### 6.2 PNTS

- 解析常见位置、量化位置、RGB/RGBA/RGB565、法线/八面体法线、batch/feature identity 及支持属性。
- 每个点按世界坐标精确测试；保留点的所有 Attribute 与 metadata 同步 compact。
- 空结果返回确定性 EMPTY 语义，不输出原 PNTS 或非法零 bounds。

### 6.3 I3DM

- 支持内嵌和批准资源闭包内的 glTF 模型。
- 完全位于授权范围内的实例可整实例保留，完全相离则删除。
- 跨边界实例必须展开到 `MESH_GLTF2` 精确裁切；在此路径验证前该组合保持不支持。
- 禁止仅凭实例 origin/center 在范围内就放行完整实例。

### 6.4 Draco 与 Meshopt

- 使用固定版本的成熟库，不自行实现压缩算法。
- Draco 支持矩阵按 primitive mode、Attribute semantic/type、point mapping、face topology 和 quantization 声明。
- Meshopt 支持矩阵按 mode、filter、stride、count 和目标 buffer view 声明。
- 解码器运行在内存、时间和输出字节限制内；异常、超限、映射不一致均 fail closed。
- 规范化/裁切输出默认不重新有损压缩，避免新边界顶点被量化移出权限边界。

### 6.5 Texture 与 KTX2

- 输入目标覆盖 PNG、JPEG、WebP、KTX2/BasisU。
- 先验证像素维度、mipmap、layer/face、颜色通道和解码后内存，再转换到 RGBA 裁切空间。
- 授权纹理以保留三角形 UV 覆盖生成透明 mask；不能只裁几何而保留可读取范围外内容的整张属性/业务纹理。
- canonical 输出编码由 PNG/lossless WebP spike 决定，并固定 encoder 版本和参数以保证确定性。

## 7. 坐标与几何裁切

1. 继续沿用并扩展已验证的 up-axis 和 transform 组合，不允许为新格式再写一套坐标约定。
2. 授权 Polygon/MultiPolygon 含洞；必须正确处理跨面、边界共线、退化三角形和浮点容差。
3. 新交点的 position、normal、tangent、UV、color、feature-related Attribute 按各自语义插值或选择；不能把整数 feature ID 做普通线性插值。
4. 重建 index/accessor/buffer view、min/max、primitive/node/scene bounds，并移除不可达资源。
5. 输出经 glTF/3D Tiles 结构验证、有限值检查、bounds 检查和授权几何复验后才上传。

## 8. 元数据和隐私

- 为保留 feature 建立 old-to-new ID 映射。
- 支持项逐步覆盖 Batch Table JSON/binary、property table、property attribute、property texture 和必要 schema。
- 字符串 offset、binary column、null/default、array 和 hierarchy reference 必须同步 compact。
- 删除 feature 对应的行、字符串、binary region、property texture pixel 和关系节点不能残留为可访问资源。
- 对未知 required metadata extension、无法重建的关系或无法隔离的属性纹理返回专用不支持原因。
- 不能用“删除所有 metadata”作为有限授权兼容方案。

## 9. 输出与确定性

- 输出 identity 包含 source closure hash、normalization version、scope hash、index generation 和 clip algorithm version 中适用的部分。
- 同一输入、版本和范围重复执行产生字节一致输出或经过明确定义的 canonical comparison 一致。
- 终态至少区分 READY、EMPTY、UNSUPPORTED、FAILED、CANCELLED/STALE；原因码使用枚举常量。
- 上传采用临时对象加校验后发布，不覆盖既有 immutable artifact。
- 日志不记录 token、签名 URL、完整业务属性、授权 Polygon 坐标或可复用对象凭据。

## 10. 分阶段实施和启用门槛

1. 先抽象 typed mesh scene，保证现有 B3DM/Draco 行为无回归。
2. 完成 direct GLB/glTF、广义 B3DM、Meshopt 和 texture/KTX2 的 Mesh 路径。
3. 完成 PNTS Point 路径。
4. 完成 I3DM Instance 路径和边界实例展开。
5. 完成 CMPT 递归组合。
6. 每个格式/decoder 单独 feature flag，只有对应语料全部通过后才由上游标成 clip-ready。

## 11. 测试与验收

- 每类输入包含最小、正常、边界相交、洞、空结果、损坏 header/table/accessor、超限和未知扩展用例。
- 压缩与未压缩同源 fixture 的世界几何和裁切结果等价。
- texture mask 通过像素级检查，不允许范围外非透明像素。
- metadata 测试证明点选 ID/属性正确，且删除数据无法从 buffer/image/table 恢复。
- fuzz/恶意用例覆盖整数溢出、越界、循环资源、超大解码输出和 decoder 失败。
- x64/arm64 Release、CTest、 sanitizer/fuzz（适用时）、确定性复跑、真实语料离线回放均通过。
- 性能和内存上限由基准结果固化为具名配置，不在代码中散落魔法数字。

## 12. 明确非目标

- 不改变二维无限高授权模型。
- 不首期支持任意 vendor content、draft 3D Tiles 2.0、Gaussian splat 或全部 glTF 扩展。
- 不在 Worker 中实现对象存储目录遍历、任意 URL 下载或权限策略计算。
- 不承诺所有复杂 metadata/style 在首期可裁切；未验证组合保持全局预览。

## 13. 任务 1.2 实施记录：官方语料清单

### 13.1 来源与固定版本

- CesiumJS 官方 `Specs/Data/Cesium3DTiles`，固定提交
  `6d5d8b1f0725b6f831b336463f4b11c98023427b`，Apache-2.0。
- Cesium 3D Tiles Validator 官方 `specs/data`，固定提交
  `7fa62c5f792069b077f174b477aab85dd7fecf22`，Apache-2.0。
- Khronos glTF Sample Assets，固定提交
  `0ae347c92c4db782a544325bfb8bf7fa3e95c4ec`；许可证按模型记录，包含
  CC0-1.0、CC-BY-4.0 以及必须保留的归属/商标说明。

### 13.2 清单和下载规则

- `tests/corpus/official-corpus-manifest.json` 是机器可读的唯一来源清单；保存完整
  40 位提交、上游路径、许可证路径、归属信息和能力覆盖标签。
- 必须覆盖 directory、ZIP、3TZ、explicit/implicit hierarchy、external Tileset、
  multiple contents、GLB/glTF、B3DM、PNTS、I3DM、CMPT、Draco、Meshopt、
  KTX2/BasisU、PNG、JPEG、WebP、Batch Table、Feature ID 和 structural metadata。
- 官方二进制不提交 Git。`fetch_official_corpus.ps1` 先校验完整清单，再使用 sparse
  checkout 获取固定提交，并把 payload、许可证和来源记录写入被忽略的
  `tests/corpus/downloaded/<entry-id>`。
- 下载器拒绝非 HTTPS GitHub 地址、非完整小写提交、不规范/越界路径、重复来源或条目、
  未知派生来源以及覆盖缺口；缓存 remote 和实际 checkout 提交必须与清单一致。
- ZIP 用例从已授权的 Cesium 目录语料确定性派生，具体字节生成由任务 1.3 的 fixture
  generator 负责；官方下载器不隐式重打包样例。

### 13.3 验证结果

- `fetch_official_corpus.ps1 -ValidateOnly` 通过：3 个来源、19 个条目、22 个覆盖标签。
- JSON 结构化复核通过：required coverage 无缺失。
- `git diff --check` 通过。

## 14. 任务 1.3 实施记录：确定性边界与恶意夹具

- `fixture-cases.json` 声明 9 类用例和稳定期望：boundary crossing、polygon hole、
  antimeridian adjacent、malformed byte length、URI traversal、archive bomb、unknown
  required extension、metadata leakage 和 empty authorization。
- `generate_fixtures.ps1` 使用 UTF-8 无 BOM、LF、固定 ZIP 时间戳、固定生成器版本和
  SHA-256 清单生成 14 个小型离线文件；`-VerifyOnly` 在系统临时目录重建并逐字节比对，
  不修改已提交夹具。
- URI 穿越同时覆盖明文 `../` 和 percent-encoded `%2e%2e`；未知 required extension
  明确携带不能被静默忽略的 payload。
- archive-bomb 是可读取的单条目 ZIP，压缩后 1167 bytes、展开 1048576 bytes，实测
  entry ratio 约 1015；用于在解压前验证 expanded-size/ratio 上限。
- metadata fixture 使用两个 feature row，feature 0 为 `public`、删除目标 feature 1
  为 `secret`。有限授权输出既要保留 feature 0，又必须证明 `secret` 及未引用字节不可恢复。
- EMPTY fixture 的完成协议只含 `result` 和原因，不允许 output ETag、SHA-256、size 或
  object key。
- 生成器自检确认 B3DM 为完整 28-byte header 加 12-byte GLB header，但两层声明长度
  均大于实际长度；本次生成与 `-VerifyOnly` 均通过，14 个文件哈希无差异。

## 15. 任务 1.5 实施记录：PNG 与 exact lossless WebP 策略

### 15.1 可复现基准

- 新增 `tests/benchmarks/texture_codec_benchmark.py`，确定性生成 512x512 的 opaque smooth、
  opaque high entropy、sparse transparent mask 和 feathered alpha boundary 四类 RGBA。
- PNG 固定 `compress_level=9`、`optimize=false`；WebP 固定 `lossless=true`、`quality=100`、
  `method=6`、`exact=true`，与现有 Worker 的 exact lossless 意图一致。
- 每类每种编码执行 7 次；每次都必须得到完全相同字节，并从实际编码结果解码为与输入
  完全一致的 RGBA。透明区 RGB 和 Alpha 都预先清零，防止透明像素隐藏源数据。
- 机器可读结果写入 `tests/benchmarks/results/texture-codec-baseline.json`；环境为 Pillow
  12.3.0、libwebp 1.6.0 和 zlib-ng 1.3.1。

### 15.2 结果

| case | PNG bytes | WebP bytes | WebP/PNG | PNG encode median | WebP encode median |
|---|---:|---:|---:|---:|---:|
| opaque smooth | 2,351 | 506 | 21.52% | 8.352 ms | 2,789.053 ms |
| opaque high entropy | 603,659 | 63,704 | 10.55% | 292.879 ms | 2,700.280 ms |
| sparse transparent mask | 4,193 | 2,644 | 63.06% | 8.402 ms | 1,122.827 ms |
| feathered alpha boundary | 10,264 | 6,158 | 60.00% | 17.314 ms | 1,299.436 ms |

- 两种编码在全部 case 上均为 byte deterministic、exact RGBA round-trip。
- WebP 体积显著更小，但 exact method-6 编码成本高；时间只用于当前机器策略判断，不作为
  Worker SLA 数字。
- glTF 2.0 原生定义 PNG/JPEG；无 fallback 的 WebP 需要把 `EXT_texture_webp` 同时列入
  `extensionsUsed` 和 `extensionsRequired`，因此通用 renderer 互操作面小于 PNG。

### 15.3 选定策略

- 第一阶段凡经解码、授权 mask 或重建的 canonical texture 统一输出 PNG `image/png`。
- 输出不保留 timestamp、ICC、Exif、text 等源 metadata；编码后必须再解码，验证尺寸、
  全 RGBA 和授权 mask 外全零，验证通过后才发布。
- exact lossless WebP 作为显式 renderer capability 的可选优化；只有 corpus、后解码验证、
  codec/encoder 版本化 identity 均通过后才能启用，不作为通用授权路由默认值。
- 现有 legacy B3DM/WebP 行为在版本化 canonical mesh 路径落地前保持不变，避免本任务
  只做策略基准却暗中改变线上字节契约。
- Worker CI 在任务 6.6 启用生产 PNG encoder 前，必须用固定 libpng/libwebp 版本复跑同一
  case；本地 Pillow 结果不能替代生产 codec 验收。

## 16. 任务 1.6 阶段记录：资源上限缩放基准

### 16.1 已完成测量

- 新增 `tests/benchmarks/resource_limit_benchmark.py`，使用固定生成数据测量 ZIP 中央目录
  安全遍历、canonical resource closure 排序/hash、三角形对矩形裁切和 PNG exact 回读。
- ZIP inspection 从 1,000/10,000/50,000 entries 增长时，中位耗时为 41.364/408.306/
  2,199.847 ms，Python heap 峰值约 1.0/6.6/31.3 MiB，呈近线性增长。
- resource closure 从 10,000/50,000/100,000 resources 增长时，中位耗时为 67.929/
  353.341/706.946 ms，Python heap 峰值约 2.0/10.0/20.1 MiB。
- 边界相交三角形从 10,000/100,000/500,000 增长时，中位耗时为 117.064/1,184.072/
  6,099.551 ms；这是 Python 算法缩放基准，不代表现有 C++ Worker 的绝对性能。
- 512/1024/2048 PNG 生成、method-9 编码和 exact RGBA 回读中位耗时约 1.179/4.502/
  18.490 秒，Python heap 峰值约 5.2/16.0/64.1 MiB。耗时包含 Python 逐像素生成，不能
  当作 libpng 单独编码耗时，但可用于验证像素数和内存近似平方增长。
- 完整结果写入 `tests/benchmarks/results/resource-limit-baseline.json`。

### 16.2 已确认标准资源档位

- 用户确认采用 `STANDARD_4CPU_8GIB_SINGLE_TASK_V1` 作为首版生产默认档位：4 vCPU、
  8 GiB 容器内存、每个 Worker 进程最多 1 个活动任务。
- 单任务常驻内存预算为 6 GiB，保留 2 GiB 给进程、动态库、网络和对象传输缓冲；所有
  decoder 合计最多占 4 GiB，单个 decoder scratch 最多占 1 GiB。限额是累计预算，不能
  让多个 decoder 分别使用完整上限。
- package compressed bytes 上限为 2 GiB；流式展开后的总字节上限为 8 GiB；单 entry
  上限为 1 GiB；entry 数上限为 100,000；路径 UTF-8 字节数上限为 1,024；archive
  nesting depth 上限为 1；单 entry 与总包 compression ratio 上限为 100。
- URI/resource closure depth 上限为 32，resource count 上限为 100,000，bounded data URI
  解码后上限为 64 MiB。
- 单 content 的 geometry 上限为 5,000,000 vertices、2,000,000 triangles、
  10,000,000 points 或 250,000 instances；CMPT depth 上限为 8、直接 child 上限为
  1,024。超过任一维度即返回稳定 resource-limit 原因，不能靠另一维度尚有余量继续处理。
- 单 texture 最大宽高均为 8,192，最大像素数为 67,108,864，单张解码 RGBA 上限为
  256 MiB；所有纹理和 geometry 解码仍共同受 4 GiB aggregate decoder budget 约束。
- inspection 最长 120 秒，normalization 和 clipping 各最长 900 秒；超时后不得发布临时
  或部分制品。以上时间是 task deadline，不替代 lease heartbeat 和取消检查。
- 首版 profile 不直接放宽现有 legacy B3DM Worker 的 64 MiB input/output 契约；后续安全
  inspector、normalizer 和 typed clipper 分别接入 profile 时，才按对应 OpenSpec task
  启用新限额。

### 16.3 配置与调优规则

- 机器可读 profile 是默认值唯一来源；compose 只负责选择 profile、CPU、内存和并发，
  不复制一套格式限额常量。
- 所有字节和计数使用整数，配置加载时验证数量级、乘法关系和内存预算关系；任何缺失、
  溢出、负值或不一致配置都拒绝启动，不回退到无界处理。
- 4 vCPU/8 GiB/单任务是部署前提，不代表已经完成 native decoder RSS 验收。任务 3.3、
  5.x、6.x 启用对应能力前，必须在该档位使用固定 Worker/codec 版本和官方语料复测；调小
  可直接发布，调大必须新建 profile version、更新基准和 OpenSpec 证据。
- `config/resource-limit-profiles-v1.json` 已成为机器可读默认来源，独立校验脚本通过；
  compose 已应用 4 CPU/8 GiB 资源约束，Task 1.6 基线工作完成。

## 17. Task 3.1 Inspector V1 协议设计（用户已确认）

- Worker 新协议固定为 `THREE_D_TILES_INSPECTOR_V1`，与当前 clip task contract 分离，放在
  新的 inspection contract 模块，不能改变既有 B3DM claim/complete/fail JSON。
- 请求使用 inspection/request/source generation/inspector version 四项身份，绑定具名
  resource profile、SHA-256 和 deadline。对象 identity 要求包内逻辑名、size、ETag 及可选
  SHA-256；读取时始终复算 SHA-256。
- 对象清单按有界 page 传输并由最终 manifest SHA-256 封口。短期 grant 只允许确定对象的
  `GET`；不接收桶级凭据、任意 header、对象列举权限或可写 prefix。
- 结果按 page 返回观察到的 identity、结构分类和依赖 ID。每页及整个结果 manifest 都校验
  SHA-256；缺页、重页、计数/Hash 不一致或对象漂移均不能发布成功终态。
- page Hash 是 key 字典序、数组顺序不变、UTF-8 无空白 canonical `records` JSON 的
  SHA-256；manifest Hash 是按页号升序的 `pageNumber:pageSha256:recordCount\n` ASCII 行摘要。
  V1 不允许浮点数进入这两类摘要，避免 Java/C++ 数字序列化差异。
- structured diagnostic 使用稳定 code/severity/stage 和有界安全消息，不得含 URL/token、
  bucket/key、query、源 payload/属性、stack trace、本机路径或授权 Polygon。
- 本任务只实现双端类型、JSON Schema、边界校验、脱敏及共享正反契约夹具；archive 枚举、
  路径规范化、root 选择、magic 分类和资源闭包遍历仍由后续 Task 3.3 至 3.8 实现。

### 17.1 实施结果

- 新增 `include/clip_worker/inspection/inspection_contract.hpp` 与对应实现，和既有 clip task
  contract 完全分离。解析器拒绝未知字段、未知 enum、非 V1 版本、错误 UTC/短期 grant、
  非规范包内名、超限计数、缺页/重页、Hash/identity 漂移及敏感诊断。
- `config/inspector-protocol-v1.schema.json` 与 Java canonical Schema 字节一致；正反 fixture
  同样逐字节镜像，并以固定 schema digest 和 canonical source/result page Hash 防止漂移。
- resource profile 新增 Inspector 协议专用上限，验证脚本已覆盖页/诊断/依赖/授权时长与
  现有 archive、closure、内存、deadline 的约束关系。
- 无网络 build-base 中使用项目 C++17 Release 告警参数独立编译新模块，4 项 GTest 全部
  通过且无告警。完整 CMake 因本地旧依赖缓存没有当前 Draco 包而未完成；没有联网重建依赖。
  Task 3.1 完成，但不表示 Worker 已开始领取 inspection task 或解析 ZIP/3TZ/3D Tiles 内容。

### 17.2 Task 3.2 Worker 异步编排边界（用户已确认）

- Inspector 继续使用控制面 pull model，但使用独立于 clip task 的
  `/api/internal/three-d-tiles/inspection-tasks` 路由、scheduling DTO 和 API client。现有
  `WorkerApiClient`、`WorkerRuntime`、B3DM claim/heartbeat/complete/fail 行为及环境变量保持
  不变，不能为复用而扩大现有 clip JSON。
- claim 请求声明 worker ID、`THREE_D_TILES_INSPECTOR_V1`、schema digest、inspector/profile/
  tool versions；响应包含外层 lease token/request ID 和内层 Task 3.1 inspection request。
  Worker 只接受完全匹配版本和 profile hash 的任务。
- 独立 inspection lease session 周期 heartbeat，维护单调资源/content 进度，接收
  `cancelRequested`，并保证旧 token、过期 lease 或 heartbeat failure 后停止 source fetch 和
  result publication。lease token、grant URL 和对象 URL 不进入结构化日志字段。
- source manifest page 通过当前 lease 读取并逐页验证；result page 在内存中有界生成、计算
  canonical hash 后逐页幂等提交，最后提交引用完整 result manifest 的 V1 envelope。控制面
  冲突、取消或 lease loss 后不得重试 terminal publish，也不得把部分结果声明成功。
- 本任务新增 scheduling envelope、inspection API client、lease/runtime seam 及 fake transport
  单测，但不在 production `main` 中领取 inspection。Task 3.3 提供 directory/ZIP/3TZ 的真实
  executor 后再新增显式运行模式/开关，避免当前占用任务后只能返回伪失败。
- 首版控制面建议 hard deadline 120 秒、lease 45 秒、heartbeat 15 秒、max attempts 3；Worker
  只读取控制面返回的 deadline/lease，不在本地另造更宽限制。容器仍保持 4 CPU、8 GiB、单
  active task。
- 验收覆盖 claim compatibility、heartbeat/cancel、lease loss、分页重放、terminal callback、
  日志脱敏和与现有 clip runtime 的回归隔离；没有真实 archive executor 时不能声明 Task 3.3
  或任何格式检测能力完成。

### 17.3 Task 3.2 实施结果

- 新增独立 `inspection_scheduling` 契约，严格解析 claim/lease/heartbeat 外层 DTO，校验协议、
  schema digest、Inspector/resource profile/tool versions、UTC hard deadline 和 lease 时间；
  V1 result 的 required tools 必须与任务逐项完全匹配。
- 新增专用 `InspectionApiClient`，固定使用
  `/api/internal/three-d-tiles/inspection-tasks` 的 claim、source page、heartbeat、result page
  和 complete 路由。client 使用可注入 HTTP transport，限制响应体大小，错误信息不携带响应
  body、URL、token 或对象标识；现有 clip `WorkerApiClient` 未修改。
- 新增 `InspectionLeaseSession` 和 `InspectionTaskRuntime` seam。session 维护 active/cancelled/
  lost/completed 状态、单调进度、source/result page identity 和唯一 terminal publish；取消或
  lease loss 后禁止继续拉取/发布。terminal 复验使用 attempt 首次 source exchange 的验证时间，
  不要求 90 秒 grant 在 120 秒 hard deadline 末尾仍有效。
- runtime 通过 callback executor 隔离后续真实检查器，当前 production `main` 未接入 inspection
  claim。Task 3.3 完成 directory/ZIP/3TZ 枚举与真实 executor 后，才允许增加显式运行模式和
  开启控制面 claim；当前能力仍是“协议和编排可用，格式检测未启用”。
- 在现有离线 build-base 内，以 C++17、`-Wall -Wextra -Wpedantic -Wconversion -Werror` 编译
  contract/scheduling/client/runtime 及测试通过；2 个 suite 共 10 项 GTest 全部通过。随后按
  17.4 生成与当前 manifest 匹配的 `r2` 基础镜像并完成完整 CMake/CTest 验收。

### 17.4 Task 3.2 完整 CMake 验收修订（用户已确认）

#### 17.4.1 已确认根因

- 当前 `vcpkg.json` 已包含 `draco`，SHA-256 为
  `16b3c7fba2428584afa7cbd36cf784acb6d89611330eafc01281a8022521b010`；
  `Dockerfile.base` 和离线脚本的默认依赖版本已经是
  `ubuntu24.04-vcpkg2025.07.25-r2`。
- 本机和 `offline-bundles/amd64-validation` 只有 `r1` build/runtime base。该基础包保存的
  manifest SHA-256 为 `fe1057dc79fa5de61e637ba668c74745129322ab4c32f3227ee8a8afd5739f47`，
  依赖列表不含 Draco，因此其 fail-closed manifest 校验正确拒绝当前源码。
- 正式 Dockerfile 的 apt/vcpkg BuildKit 层也没有可复用缓存；使用 `--network=none` 已复现为
  apt 索引不可用。该错误发生在 CMake 之前，不能证明当前完整 CMake 或测试存在源码错误。

#### 17.4.2 修复范围

- 不删除 `draco`，不降级 vcpkg，不放宽或跳过 manifest、镜像 label、平台、工具及第三方资产
  Hash 校验；这些校验是离线供应链边界，不应为通过本机构建而削弱。
- 使用现有 `docker/offline/prepare-bundle.sh` 和固定 vcpkg `2025.07.25`，为当前 manifest 生成
  独立 amd64 `r2` build-base/runtime-base。优先使用项目已记录的 `cn` mirror profile；镜像代理
  只改下载路径，vcpkg tool、第三方源码和 manifest 的摘要校验保持不变。
- 新 bundle 输出到被忽略的任务构建目录，不覆盖或删除历史 `r1` bundle。生成完成后必须校验
  architecture、dependency version、manifest hash、image ID/label、tar SHA256SUMS 和 runtime
  PROJ 数据；任务结束不把大型镜像 tar 加入 Git。
- 使用新 `r2` 基础镜像执行 `docker/offline/build.sh`。该脚本必须以 `--network none`、
  `--no-cache` 重新运行完整 Release CMake configure/build、全部 CTest、install 和最终 runtime
  镜像检查，确保结果不依赖工作站的源码编译目录或下载缓存。
- 若依赖恢复后出现真实 CMake、编译或测试错误，本节不授权猜测性升级依赖或改变 Draco 行为；
  先记录准确错误和最小修复影响，再更新本节并与用户确认后修改源码。

#### 17.4.3 完成标准

1. `r2` build/runtime base 的 manifest label 与当前 `vcpkg.json` SHA-256 完全一致。
2. 完整 `clip_worker_core`、主程序和 `clip_worker_tests` 在 Release 下无编译错误。
3. 全部 CTest 通过，而不只是 Inspector 的独立 10 项测试。
4. 最终镜像可在禁网环境执行 `--version`，UID 为 10001，包含 PROJ 数据且不包含 CMake、Ninja、
   Git 或 `/opt/vcpkg`。
5. 更新本 Spec 和 API 侧 Task 3.2 实施记录，移除“完整 CMake 未完成”的限制说明，并再次执行
   `openspec validate --strict`、`git diff --check`；只暂存文档或确有必要的最小源码修复。

#### 17.4.4 大型依赖下载超时修订（用户已确认）

- 首次 `cn` profile 在 OpenSSL 下载中于 300 秒收到约 25.8 MiB 后超时；切换到官方 GitHub
  asset 后，固定 vcpkg tool、OpenSSL、zlib、curl、Draco 1.5.7、GTest、WebP、nlohmann-json、
  LZMA、JPEG、TIFF 和 SQLite 均通过下载摘要与 post-build validation。
- 最后一个 PROJ 9.6.2 源包实际为 45,227,100 bytes。三次 300 秒窗口分别因链路波动只收到约
  9.2 MiB、26.4 MiB 和 7.0 MiB；curl retry 不断点续传，每次从零开始。因此当前阻塞是下载
  时间配置未从 bundle 构建入口透传，不是 vcpkg、Draco、PROJ 或 CMake 兼容错误。
- `docker/vcpkg-download.sh` 已支持 `VCPKG_DOWNLOAD_CONNECT_TIMEOUT_SECONDS` 和
  `VCPKG_DOWNLOAD_MAX_TIME_SECONDS`，但 `Dockerfile`、`Dockerfile.base` 与
  `prepare-bundle.sh` 没有声明/传递。最小修复将使用具名默认值：连接 15 秒，单请求最长
  3,600 秒；两个值只接受正整数，并允许可信构建环境通过
  `CLIP_WORKER_VCPKG_DOWNLOAD_*` 显式收紧或覆盖。
- `Dockerfile` 与 `Dockerfile.base` 只把 build arg 传为下载脚本环境变量，不改变 URL 重写、
  `curl --retry 5`、vcpkg tool SHA-512、第三方资产摘要、manifest SHA-256、依赖版本或产物内容。
  `prepare-bundle.sh` 从具名默认值读取、校验并传入两个 build arg，使手工构建和正式 bundle
  生成使用同一契约。
- 修复后先对 POSIX 脚本执行语法/非法参数拒绝验证，再重新无缓存生成 `r2`；完整 CMake/CTest
  和禁网 runtime 验收仍按 17.4.3 执行。不会把超时增大解释为放宽资源处理 deadline，也不会
  影响 Worker 运行时网络或 inspection 的 120 秒 hard deadline。

#### 17.4.5 离线测试上下文缺少协议配置修订（用户已确认）

- 下载超时修订后，`r2` build-base 和 runtime-base 已完整构建；16/16 个固定版本 Vcpkg 依赖
  均通过下载摘要与 post-build validation，镜像的 `linux/amd64` 平台、依赖版本、架构和
  manifest SHA-256 label 均与当前源码一致。
- 使用这两个 `r2` 镜像执行 `Dockerfile.offline --network none --no-cache` 时，Release CMake
  配置及 35 个 Ninja 编译/链接步骤全部成功；81 项 CTest 中 80 项通过，唯一失败项为
  `InspectionContractTest.SchemaDigestAndCredentialBoundaryAreStable`。
- 失败原因不是摘要常量或 Inspector 业务逻辑错误：`Dockerfile.offline` 已复制 `include/`、
  `src/`、`tests/`，但没有复制测试所需的 `config/inspector-protocol-v1.schema.json`。在同一
  build-base、禁网条件下挂载完整源码运行该测试可稳定通过。
- 拟议最小修复是在 `Dockerfile.offline` 的构建阶段增加 `COPY config config`，使正式离线构建
  与 CMake 测试声明的源码目录契约一致；不修改协议 Schema、摘要常量、Inspector 实现、
  依赖版本或运行时镜像内容。
- 修复后重新执行完整 `--network none --no-cache` 构建，要求 81/81 CTest 通过，再完成非 root、
  PROJ 数据、无构建工具残留和 Worker `--version` 验收。

#### 17.4.6 实施与验收结果

- `docker/vcpkg-download.sh`、正式联网 `Dockerfile`、`Dockerfile.base` 和 bundle 入口现统一使用
  连接 15 秒、单请求最长 3,600 秒的具名默认值；bundle 入口拒绝空值、零、负数和非整数。
  未改变依赖版本、资产 URL 选择、retry 次数或任一摘要校验。
- `r2` 的 16/16 个 Vcpkg 包全部安装并通过 post-build validation，其中包含 Draco 1.5.7 和
  PROJ 9.6.2。最终 build-base ID 为
  `sha256:ed1e37ea77b7e6b6851d100d28d686a722bf9b759a194923367c69ecb46af8bb`，runtime-base ID 为
  `sha256:3e2b4a2dc35f173a28ef48aeea2ec819b709da27357e8df1e2ea762d6f8c986b`；两者均为
  `linux/amd64`，依赖版本、架构和 manifest SHA-256 label 全部匹配。
- `Dockerfile.offline` 只补充构建阶段的 `COPY config config`。使用 `--network none`、
  `--pull=false`、`--no-cache` 完成 Release CMake 配置、35 个 Ninja 编译/链接步骤、install 和
  81/81 CTest；此前唯一失败的 Schema 摘要测试随测试配置进入构建上下文后通过。
- 最终验收镜像 `3d-tiles-clip-worker:task32-cmake-validation` ID 为
  `sha256:0a3918be85cb2cd1a100057aa0244c4252dadb792d65733dc0cd9d11dbccd33a`，版本 `0.1.7`，
  UID 为 10001；禁网检查确认 PROJ 数据可读，动态库全部可解析，且镜像不包含 `/opt/vcpkg`、
  `/opt/clip-worker-deps`、CMake、Ninja 或 Git。
- 可搬运 bundle 位于被 Git 忽略的 `build/task32-r2-bundle`，保留历史 `r1` 不变。build/runtime
  tar SHA-256 分别为 `1e7bc0370d2f85c57d99366a089ae7abbfd4475e14ec2be0157f1ad13133107f`
  和 `740d59cbff28cbd01e43c3f916fbc4a25783aa5e59de9329dadde052af6b0481`；`SHA256SUMS` 中
  两个归档、manifest 和 README 已在禁网 Linux 容器内全部校验通过。

## 18. Task 3.3 包枚举与资源限额设计（用户已确认）

### 18.1 本轮目标和跨任务边界

- 本轮实现可独立测试的 directory、ZIP、3TZ 包枚举器，以及把 Task 3.2 source manifest page
  交给枚举器的真实 Inspector executor 组件。枚举结果是确定性的包资源事实，不是有限授权
  能力结论。
- 本轮读取并计算每个逻辑资源的 SHA-256、实际展开字节数和归档计数，输出
  `PackageEntryEvidence`。不解析 Tileset/glTF/legacy tile 结构，不选择 root，不建立 URI
  dependency closure，不写 normalized artifact，也不上传任何展开后的对象。
- Task 3.4 负责把路径分析中的 traversal、absolute、backslash、duplicate、case collision、
  symlink-like、unsupported scheme 和 namespace escape 标记收敛为终态拒绝。Task 3.3 不把任一
  未经 Task 3.4 校验的 candidate path 当成可发布路径，也不把归档 entry 写入文件系统。
- Inspector V1 的 `SUCCEEDED` 结果当前强制要求已选择 root，而 root selection 属于 Task 3.6。
  因此 Task 3.3 完成后仍不能在 production `main` 打开 claim；生产领取至少等待 Task 3.4 至
  3.8 形成完整 executor。单元/集成测试通过直接调用枚举器和 executor pipeline seam 验收。
- 当前 OpenSpec design 中“Task 3.3 后开启 production claim”的表述需在用户确认后同步修订，
  不能让尚未完成 root/magic/closure 的 Worker 领取任务后返回伪成功或永久拒绝。

### 18.2 3TZ 兼容基线

- 3TZ 不是 3D Tiles 1.1 核心规范的一部分。本项目以 CesiumGS 官方 `3d-tiles-tools` 和
  `3d-tiles-validator` 当前实现为互操作基线，并把 `validTilesetPackage.3tz` 纳入原生测试。
- 兼容的 3TZ 是 ZIP/ZIP64 容器，包含保留 entry `@3dtilesIndex1@`；该 entry 必须使用 STORE 且
  是最后一个 central-directory file record。索引内容由若干 24-byte 记录组成，每条为按 3TZ
  兼容规则归一化后的 UTF-8 path 的 16-byte MD5，以及对应 ZIP local-header 的 8-byte
  little-endian offset。
- MD5 记录顺序按规范先比较 digest `[0..8]`、再比较 `[8..16]` 各自解释出的 little-endian
  unsigned 64-bit 值，不使用普通 lexicographic byte order。3TZ Hash 前的兼容归一化只用于 index
  验证；原始名称仍交给安全 analyzer，不能借此把 leading slash 或 backslash 洗成安全路径。
- 枚举器必须交叉验证索引长度、记录数、上述 MD5 顺序、offset 范围、local-header signature、
  local/central path 一致性及所有逻辑 file entry 的一一覆盖。MD5 只用于验证 3TZ 既有索引，
  不作为安全 identity 或内容摘要；资源 identity 和内容完整性始终使用 SHA-256。
- 首版读取 ZIP 的 STORE 和 DEFLATE entry，支持 ZIP64。加密 entry、多卷 ZIP、data descriptor
  无法可靠闭合的 entry、未知压缩算法和损坏的 central/local header 返回
  `PACKAGE_INVALID`，不尝试调用外部解压程序。

### 18.3 依赖和构建供应链

- 使用固定 Vcpkg `2025.07.25` 中的 `libzip 1.11.4`，关闭默认可选 codec，并显式只启用
  Zstd feature；首版接受 ZIP/ZIP64 的 STORE、DEFLATE，以及 3TZ 的 Zstandard method 93，
  不启用 AES、bzip2 或 XZ。成熟库负责 ZIP 结构和流式 entry 读取，项目
  代码负责 3TZ index 交叉验证、路径策略、checked arithmetic 和业务限额。
- `libzip` 加入 `vcpkg.json` 后 manifest SHA-256 必然变化，现有 r2 build/runtime base 会按
  供应链校验正确失效。本轮需要生成独立 r3 base 和离线 bundle，保留 r1/r2，不覆盖历史包。
- r3 必须重复 Task 3.2 的完整验收：固定资产摘要、post-build validation、禁网且无缓存的
  Release CMake/Ninja/全部 CTest、install、最终镜像 `ldd`、非 root UID、PROJ 数据和无构建
  工具残留检查。不能因为刚完成 r2 就复用与新 manifest 不匹配的依赖层。

### 18.4 枚举器接口和确定性数据模型

- 新建独立 `inspection/package` 模块，不把 ZIP/3TZ 逻辑放入 `B3dmClipper`、clip task runtime
  或 Task 3.1 JSON parser。核心接口按 `PackageKind` 分派 `DirectoryPackageEnumerator` 和
  `ZipPackageEnumerator`；3TZ 复用 ZIP reader 并附加 3TZ index validator。
- `PackageEntryEvidence` 至少包含：source object ID、archive parent ID、原始 UTF-8 名称、
  normalized candidate path、path analysis flags、entry kind、compression method、depth、
  compressed bytes、declared expanded bytes、observed expanded bytes、source ETag、SHA-256、
  ZIP creator/external attributes、local-header offset 和稳定 ordinal。字段使用 enum/具名类型，
  不使用散落字符串或魔法数字。
- 输出按 normalized candidate path 的 UTF-8 byte order 排序；相同 candidate 保留稳定原始
  ordinal，交给 Task 3.4 判定 duplicate/collision。对象 ID 使用带 domain separator 的 SHA-256
  组合 identity，不能使用 `std::hash`、ZIP CRC32、3TZ MD5 或数组下标作为持久身份。
- 归档容器本身保留一条 `PACKAGE_ARCHIVE` evidence；entry 对该 container object ID 建立
  dependency。容器使用具名保留的虚拟 package path，entry 不得占用该保留 namespace；具体
  常量及拒绝规则与 Task 3.4 一起验收。

### 18.5 路径规范化的本轮职责

- 目录 object key 先相对已批准 root prefix 取名；ZIP 名称由 libzip 解码为 UTF-8。NUL、无效
  UTF-8、超长名称或无法无歧义解码的名称立即 `PACKAGE_INVALID`。
- `PackagePathAnalyzer` 同时保留 raw name 和 normalized candidate。candidate 使用 `/` 分段和
  UTF-8 表示，不把 `\\` 静默转换为 `/`，不静默删除 leading slash、drive prefix、dot segment
  或 percent-encoded alias；这些都产生显式 flag，避免“先改名后看起来安全”。
- Unicode normalization、RFC 3986 percent decoding、dot-segment/absolute 判定、case-fold key
  和 duplicate key 必须集中在同一 analyzer/Task 3.4 policy，不允许 ZIP、directory 和后续 URI
  resolver 各自实现一套规则。
- Task 3.3 的 standalone enumeration 可返回带 unsafe flag 的 evidence 供安全测试观察，但真实
  executor 在 Task 3.4 完成前不提交这些路径，也不启用 production claim。

### 18.6 限额的精确定义

- 所有限额只从已验证的 `STANDARD_4CPU_8GIB_SINGLE_TASK_V1` profile 加载：package 2 GiB、
  expanded 8 GiB、entry 1 GiB、entry/resource 100,000、path 1,024 UTF-8 bytes、archive depth
  1、entry 和 aggregate compression ratio 100。加载缺字段、非正值、溢出或交叉关系不成立时
  Worker 拒绝启动，不回退为默认无限值。
- `maximumPackageBytes` 对 directory 表示批准 source object 大小之和，对 ZIP/3TZ 表示 archive
  source object 实际下载字节数。每次加法使用 checked arithmetic，并在读取前使用 manifest
  size 做早期拒绝、读取后使用 observed size 再次确认。
- `maximumExpandedBytes` 对 directory 等于所有 object observed bytes 之和；对 ZIP/3TZ 等于
  所有 central-directory record 实际展开 bytes 之和，包括 3TZ index 和其他包装 metadata。
  directory marker 的展开 bytes 通常为零，但仍计入结构 record count。
- `maximumEntryBytes` 同时检查 declared uncompressed size 和流式读取 observed bytes。declared
  值不能替代实际计数；二者不一致是 `PACKAGE_INVALID`，超过上限是
  `PACKAGE_LIMIT_EXCEEDED`。
- `maximumEntryCount` 对 directory 是 source object 数；对 ZIP/3TZ 同时约束 central-directory
  record 数和最终 evidence 资源数。因为 archive container 本身占一条 V1 evidence，逻辑 file
  entry 最多为 `min(maximumEntryCount, maximumResourceCount) - 1`；3TZ index/目录 marker 计入
  archive record 防护，但不作为可解析的 3D Tiles 逻辑资源发布。
- entry ratio 使用 `observedExpandedBytes / compressedBytes`，aggregate ratio 使用所有 observed
  expanded bytes / archive actual bytes；以 checked division/comparison 实现，不做可能溢出的
  乘法。`0/0` 空 entry 按 1 处理，expanded > 0 且 compressed = 0 为损坏包。STORE entry 比率为 1。
- archive nesting depth 定义为容器层数：directory 为 0，顶层 ZIP/3TZ 为 1。当前 profile 上限
  1，因此 entry 中再次出现 ZIP magic 即为 depth 2 并返回 `PACKAGE_LIMIT_EXCEEDED`。这会把
  `MAXAR_content_3tz` 等嵌套包先归为全局预览可用、有限授权暂不支持；后续若要支持，必须用
  新 profile/version 明确提高深度并增加递归预算，不能静默放宽当前档位。
- 任一资源维度超过即停止当前 attempt，返回一个稳定且脱敏的 `PACKAGE_LIMIT_EXCEEDED`
  diagnostic，删除 request scratch，不生成部分 result page、不上传 entry、不发布 capability。

### 18.7 流式读取和 scratch 生命周期

- 新增有界 streaming download API，边写 request-scoped archive scratch file 边统计字节、
  SHA-256 和 ETag；现有 clip task 的 `ObjectTransfer::download(vector)` 契约保持不变。archive
  随机读取只针对已完整下载且 identity 复验通过的本地只读文件。
- directory object 和 archive entry 均用固定大小缓冲流式 SHA-256，单 entry 完成后释放缓冲；
  不把 1 GiB entry 或 8 GiB expanded package 聚合进内存。archive entry 不落盘，直接从
  `zip_fread` 计数、hash 并丢弃内容。
- scratch 根目录使用具名 Worker 配置，默认容器内受控目录；request 子目录只由校验后的
  inspection/request identity 生成。RAII cleanup 覆盖成功、限额、格式错误、cancel、lease loss、
  deadline 和异常退出；启动恢复只清理属于 Worker 保留前缀且超过安全 TTL 的孤儿目录。
- curl/libzip 错误信息、scratch 绝对路径、grant URL、archive path 和 entry 原始内容不得进入
  public diagnostic 或结构化日志。日志只记录安全 task ID、stage、稳定 code 和计数。

### 18.8 Directory、ZIP 和 3TZ 算法

- directory：逐页消费 source manifest，先验证 object count/page hash/manifest hash，再按稳定
  package path 排序；逐对象下载并验证 size、ETag、可选 source SHA-256，流式计算 observed
  SHA-256，生成 `PACKAGE_OBJECT` evidence。source manifest 漂移、重复 source ID 或路径超过
  上限均 fail closed。
- ZIP：先验证 archive source identity 和 package bytes，再用 libzip 读取 central directory。
  对每个 record 读取 stat、encoding、compression、encryption、external attributes 和 local
  offset，执行结构/限额检查；普通 file entry 再逐个流式展开、计算 actual bytes/SHA-256 和
  ZIP-magic nesting check。目录 marker 不展开为资源，但仍计数和分析路径。
- 3TZ：先执行全部 ZIP 步骤，再校验 `@3dtilesIndex1@` 的唯一性、最后位置、STORE、24-byte
  对齐、little-endian 双 64-bit MD5 排序、record 覆盖和 local offset/path 一致性。index 本身参与
  bytes/ratio/count 限额，不进入 root candidate 和 3D Tiles resource evidence。
- 不信任 suffix、media type、CRC32、declared uncompressed size 或 3TZ index 的任一单独来源；
  只有 source identity、ZIP structure、流式 observed bytes 和 Hash 相互闭合后枚举成功。

### 18.9 测试和完成标准

- 单测覆盖 profile 加载、checked arithmetic、空 entry、接近阈值、单 entry/aggregate ratio、
  count 预留 container、UTF-8 byte path、稳定排序/ID、取消/deadline 和 scratch cleanup。
- ZIP fixtures 覆盖 STORE、DEFLATE、ZIP64、小型高压缩比 bomb、损坏 EOCD/central/local header、
  size 漂移、encrypted/unsupported method、超 entry/count/path/depth 和读取中断。
- 3TZ 至少覆盖 Cesium Validator 官方 `validTilesetPackage.3tz`，以及 missing/not-last/duplicate
  index、index 长度非 24 倍数、MD5 未排序、offset 越界、hash/path/offset 不一致。
- directory integration test 使用 fake grants/HTTP transport 验证多页 100,000 上限、ETag/SHA
  漂移、稳定重放和失败无部分页；不依赖真实客户数据。官方样本由既有 corpus fetch 脚本按
  license/materialization 规则获取，不把临时 clone 或下载目录提交 Git。
- 完成后运行新增 GTest、完整 3d-tiles-clip-worker CTest、禁网 r3 构建、fixture/corpus 校验、
  `openspec validate --strict` 和 `git diff --check`。只有这些通过才勾选 OpenSpec 3.3；
  `claimEnabled` 仍保持 false，直到 3.4 至 3.8 完成并再次确认。

### 18.10 已确认决策（2026-08-18）

1. 采用 `libzip 1.11.4`，因此生成 r3 build/runtime base 和离线 bundle，不复用 r2。
2. archive depth 把顶层 ZIP/3TZ 计为 1，当前 profile 禁止嵌套 archive；超限只产生稳定
   `LIMIT_EXCEEDED/PACKAGE_LIMIT_EXCEEDED` 事实，不由 Task 3.3 提前决定公开 capability，现有
   全局预览链路也不在本任务中改写。
3. Task 3.3 完成枚举器、真实 manifest provider 所需的 Worker 组件和 executor seam，但生产
   claim 继续关闭到 Task 3.4 至 3.8 完成。
4. 本轮流式读取并计算所有 directory object/archive entry 的 SHA-256，但不持久化展开 entry、
   不上传 artifact；expanded-object 持久化仍归 Task 3.9。
5. 普通 ZIP 首版只接受 STORE/DEFLATE 和 ZIP64，拒绝加密、多卷及其他压缩方法；3TZ index
   必须 STORE。3TZ v1.1 同步支持 Zstandard method 93，固定 `libzip`/Zstd 依赖、能力探测和
   正反 fixture 必须进入 r3 离线供应链与完整 CTest。

### 18.11 实施与验收结果（2026-08-18）

- 已新增独立 `inspection/package` 模块，实现 directory、ZIP/ZIP64 和 3TZ 枚举器、集中式路径
  analyzer、具名限额 profile、checked arithmetic、稳定排序/identity，以及供后续完整 Inspector
  executor 使用的 seam。枚举结果只表达包资源事实，不提前产生公开 capability 结论。
- directory object 和 archive entry 均采用固定缓冲流式读取，计算 observed bytes 与 SHA-256；
  archive 先下载到 request-scoped scratch，再由 libzip 流式展开 entry，不持久化或上传展开资源。
  curl 取消回调、RAII cleanup 和仅清理过期自有前缀目录的启动恢复均已实现。
- ZIP 路径覆盖 STORE、DEFLATE、ZIP64，并拒绝加密、多卷、未知方法、central/local 不一致、损坏
  EOCD、size drift、嵌套 archive 和各类限额超标。entry/resource count 为 archive container 预留
  一条 evidence；entry 和 aggregate ratio 使用无溢出的 checked comparison。
- 3TZ 在上述 ZIP 校验基础上，完整交叉验证唯一且末尾的 `@3dtilesIndex1@`、STORE method、24-byte
  record、官方 MD5 排序规则、local-header offset/path/hash 和一一覆盖。按用户确认的方案 A，
  仅对 3TZ 支持 Zstandard method 93；普通 ZIP 仍只接受 STORE/DEFLATE。
- `vcpkg.json` 已固定 `libzip 1.11.4` 的 core+zstd feature，r3 build/runtime base labels 中的
  manifest SHA-256 为 `5b441661259b78b5975a96eab33cf024a5fa2002c2b3de8555b60fd984b1f426`。
  build/runtime image ID 分别为
  `sha256:fe3d8685938ac38f8bba376a2a939cfa8e4c3479cbc5033378e20c0f61f95015` 和
  `sha256:5a9eee320203e043bdc6a42c0145aad48d3f81535f6378e80b872bfb77b8a298`。
- 使用上述已验证依赖镜像完成 `--network none`、`--no-cache` Release build/install 和完整 CTest，
  `101/101` 通过。最终镜像 `3d-tiles-clip-worker:task33-cmake-validation` ID 为
  `sha256:1cc373f55ef6f9ba1576d8f76563dbd31659c71dcefb63e60a9b1f4eab0f3b3f`，版本 `0.1.8`，
  `linux/amd64`、UID 10001；`ldd` 无缺失库，PROJ 数据可用，镜像中无构建工具残留。
- 官方语料使用固定 commit `7fa62c5f792069b077f174b477aab85dd7fecf22` 的
  `validTilesetPackage.3tz`；正反 fixtures 覆盖 Zstd93、missing/not-last/duplicate/malformed index、
  offset/hash/order 错误、ZIP bomb、路径/尺寸/计数/取消和 scratch cleanup。fixture 校验为 9 cases/
  14 files，corpus manifest 校验为 3 sources/19 entries/22 coverage keys，资源限额 profile validator
  通过。
- r3 可搬运离线 bundle 位于 Git 忽略的 `build/task33-r3-bundle`。build/runtime tar SHA-256 分别为
  `538be6e9f81a0ff693efb3e6c277eda315e2a6c90181eca2e6e7f6237c172d88` 和
  `35baf66b277892f21c7d89c2f38ded44479edafc197092e8b0e0887d8757eac4`；manifest/README 和两个
  tar 已由 `SHA256SUMS` 全部复验通过。
- production `main` 仍未开启 inspection claim，也没有提交 result page 或发布 capability。Task 3.4
  至 3.10 与业务 submission adapter 完成并再次确认前，不宣称生产闭环可用。

## 19. Tasks 3.4-3.10 安全解析、资源闭包与 Expanded 发布设计（已确认）

### 19.1 目标、边界与实现顺序

- 本轮在 Task 3.3 package facts 上实现完整且可独立调用的 `InspectionPipelineExecutor`。它在同一
  attempt/scratch 生命周期内完成 namespace policy、bounded classification、reference graph、root
  selection、closure Hash、result pagination 和 completion DTO 构建，但 production `main` 不启用轮询。
- 依赖顺序为：3.4 namespace policy -> 3.7 structure/magic classification -> 3.8 reference extraction
  与 graph validation -> 3.6 root selection/selected closure。3.5 由 Java 仓库审计完成。Task 编号顺序
  不应迫使 root selector 在尚无分类和引用图时猜测。
- Task 3.9 在全部 namespace/classification/closure 校验成功后，把安全 logical resources 发布到独立
  expanded prefix；上传阶段仍不写 source/serving/normalization prefix。不解析 tile transforms/
  bounding volumes，不 materialize implicit availability，不做 normalization/clipping，Task 4.x 继续独立推进。

### 19.2 成熟依赖与 r4 供应链

- Unicode NFC、case folding 和 RFC 3986 URI decomposition 不手写。建议在固定 Vcpkg
  `2025.07.25` 中新增 `utf8proc` 与 `uriparser`，前者负责严格 UTF-8 scalar/NFC/case-fold key，
  后者只负责保留原始输入的 URI 语法分解与相对解析；安全策略仍由项目代码显式执行。
- parser 不得先做会隐藏攻击证据的自动 dot-segment cleanup。policy 先检查 raw URI、scheme、
  authority、encoded separator/NUL/backslash/dot traversal，再对已批准的相对 path 做一次 percent
  decode、NFC 验证和 RFC 3986 base-path resolution。
- 新依赖会改变 `vcpkg.json` 摘要，因此生成独立 r4 build/runtime base 和 bundle，保留 r1-r3；
  Worker 版本提升为 `0.1.9`。r4 重复 post-build validation、`--network none --no-cache` Release
  build、全量 CTest、install、最终 `ldd`、UID 10001、PROJ 和无构建工具残留检查。

### 19.3 Task 3.4 Canonical package namespace

- 新建 `inspection/package/package_namespace`，集中提供 `CanonicalPackagePath`、
  `PackageNamespacePolicy`、`ResolvedPackageUri` 和稳定 failure enum。ZIP、directory、root hint 和
  后续 Tileset/glTF/subtree URI resolver 必须复用同一实现。
- package entry 必须是有效 UTF-8 scalar sequence 且已经是 NFC；NFD/其他非 NFC 输入直接拒绝，
  不静默改名。拒绝 C0/C1 control、NUL、absolute/drive path、backslash、empty/dot/dot-dot segment、
  query/fragment、scheme/authority、encoded slash/backslash/NUL/dot traversal、reserved internal
  prefix 和超过 profile UTF-8 byte 上限的路径。
- archive directory marker 允许唯一的末尾 `/` 参与结构分析，但以去掉末尾分隔符后的 key 参与
  duplicate/case collision；它与同名 file 或另一个 marker 冲突。3TZ index 仅由容器 validator
  作为具名内部 record 处理，不能作为用户资源进入 namespace。
- `utf8proc` case-fold key 对每个 NFC segment 生成；exact canonical path 重复或 case-fold key 相同
  均拒绝，包括 Unicode 等价/大小写碰撞。允许大小写本身，只禁止两个对象在支持的部署/客户端上
  形成不稳定别名；不尝试处理视觉 confusable。
- ZIP creator/external attributes 只允许 regular file 或 directory。Unix symlink、hard-link-like、
  socket、FIFO、block/character device 以及任何无法证明为 regular 的 special type 全部归为
  `PACKAGE_INVALID`；绝不调用 filesystem extraction API。
- 内部 prefix `__inspection_package__/` 与 `__inspection_inline__/` 由具名常量保留。上传 entry/object
  不能占用；只有 archive container 和经过校验后生成的 inline evidence 可以使用。
- namespace policy 必须看到所有 archive record，包括 directory marker，而 result evidence 仍只
  发布 container 与 logical file。Task 3.3 result 将增加内部 `path_records` 或等价只读视图，不能
  因 marker 未发布 evidence 而跳过 collision/special-entry 检查。

### 19.4 受控资源读取与 scratch 生命周期

- Task 3.3 当前在 `enumerate()` 返回时清理 archive scratch，后续无法安全读取 entry。新建
  `PackageResourceCatalog` 和 `PackageResourceReader`，由外层 `InspectionPipelineExecutor` 持有
  `InspectionScratch`，直到 3.4-3.9、staging upload 和 complete 全部结束才 RAII cleanup。
- catalog 使用 canonical path 的有序 map，值只含 object ID、archive ordinal、size/hash/ETag 和
  package kind。lookup 不接受任意 OS path，也不从用户字符串拼 scratch path。
- reader 提供具名 `readPrefix`、`readAllBounded`、`readJsonBounded` 和 streaming hash/decode seam。
  directory 只使用 source object 的 exact grant 并复验 size/ETag/SHA；archive 只重开已验证的
  scratch ZIP 和指定 ordinal，不展开到磁盘。任何 replay drift、short read 或 hash mismatch fail closed。
- 单次只保留当前 JSON/header/data URI 的有界缓冲；不缓存全部 package。continue predicate 在每个
  buffer/chunk/JSON node/graph edge 检查 cancel、lease 和 hard deadline，阶段 heartbeat 使用 Task 3.2
  的具名 cadence，不能通过 heartbeat 延长 hard deadline。

### 19.5 Profile 新增解析限额

- 在 `resourceClosure` 增加 `maximumJsonDocumentBytes=67108864`（64 MiB）和
  `maximumJsonNestingDepth=128`。先按字节限流读取，再由 `nlohmann::json` SAX/builder 解析；
  自定义 SAX guard 拒绝 duplicate object key、超深 nesting、非 object top-level 和 parse trailing data。
- 继续使用 `maximumUriDepth=32`、`maximumResourceCount=100000`、
  `maximumDecodedDataUriBytes=67108864` 和 `maximumDependenciesPerResource=4096`。所有值加载时与
  既有 archive/protocol limit 交叉验证，缺失、零值、溢出或关系不成立则 Worker 拒绝启动。
- data URI 64 MiB 是单项 decoded 上限；所有 decoded bytes 还计入本 attempt 的
  `maximumExpandedBytes=8 GiB` 总预算。解码时流式 hash，不把 100,000 个 inline payload 常驻内存。
- binary classification 最多预读具名 `maximumBinaryHeaderBytes=32`；需要 JSON chunk 的 GLB、B3DM、
  I3DM、binary subtree 再按声明长度读取相应 JSON section，但仍受 64 MiB JSON 上限。

### 19.6 Task 3.7 Bounded structure classifier

- 新建 `inspection/classification` 模块，输出 `ClassifiedResource`：object ID、canonical path、role、
  detected kind/version、actual media type、required extensions、suffix mismatch diagnostic 和受限的
  parsed reference facts。suffix 只用于 warning，不能决定 parser。
- binary magic 精确识别 `glTF`、`b3dm`、`i3dm`、`pnts`、`cmpt`、`subt`。GLB 必须 version 2；
  legacy tile/binary subtree 必须 version 1；声明 byteLength 必须等于 observed size，header/section
  长度使用 checked arithmetic。CMPT 本轮验证 outer header、tilesLength 正值及 profile child 上限，
  递归 child split 留给 Task 5.4。
- 复用现有 GLB/B3DM parser 的结构规则，并抽取 bounded header/JSON-section helper，避免出现两套
  magic/version/length 定义。I3DM/PNTS/CMPT/subtree 只新增其规范 header/parser，不调用外部程序。
- JSON 完整解析后用互斥结构 predicate 分类：Tileset 要求有效 `asset.version`、非负
  `geometricError` 和结构化 `root`；glTF 要求 `asset.version` 为 2.x 并符合 glTF top-level 形态；
  subtree 要求 tile/content/child-subtree availability 结构。匹配零个或多个均为 `CONTENT_INVALID`，
  不按 `tileset*.json`、`.gltf`、`.subtree` 猜测。
- JSON duplicate key、NaN/Infinity、非法 UTF-8、超深/超长、wrong top-level、binary magic 与声明
  长度不一致均拒绝。JSON/raw source、parser exception、local path 和 URI 不进入 public message。
- Tileset/glTF/subtree 的 `extensionsRequired` 去重排序后进入 Inspector V1 evidence；未知 required
  extension 产生 warning factual evidence，使后续 applicator 保持 global-preview-only，但不授权
  extension 自定义网络读取。

### 19.7 Task 3.8 URI resolution 与 resource graph

- 新建 `inspection/closure` 模块。graph node 使用 stable object ID，edge 使用内部 enum
  `TILESET_CONTENT`、`EXTERNAL_TILESET`、`IMPLICIT_SUBTREE_TEMPLATE`、`GLTF_BUFFER`、`GLTF_IMAGE`、
  `METADATA_SCHEMA`、`SUBTREE_BUFFER`、`I3DM_GLTF`、`CONTAINER_CHILD`；wire protocol 只发布排序后的
  dependency IDs，避免把 magic edge string 散落到 JSON。
- Tileset extractor 迭代扫描 `root` tile tree，读取 `content.uri`/legacy `content.url`、有序
  `contents[].uri`、`implicitTiling.subtrees.uri` 和 top-level metadata `schemaUri`。只提取引用，
  不在本轮持久化 transform、bounding volume、refine/geometric error 或 content ordinal。
- glTF JSON/GLB JSON/B3DM embedded GLB 提取 `buffers[].uri`、`images[].uri` 和已支持 metadata
  extension 的 `schemaUri`；I3DM 根据 `gltfFormat` 提取 external glTF URI 或验证 embedded GLB；
  binary subtree 解析 JSON chunk 并提取 external buffers。PNTS 无 package external dependency；
  CMPT child 是容器内 dependency，详细 child inventory 留给 Task 5.4。
- 仅接受无 scheme、无 authority、无 query/fragment 的 package-relative URI，或 `data:` URI。
  `http`/`https`、`//host`、`file:`、Windows drive、反斜杠、空 host trick、控制字符及其他 scheme
  在任何 fetch 前拒绝。relative URI 以引用者目录为 base，单次 percent-decode 后做 UTF-8/NFC、
  dot traversal 和 namespace policy；encoded `/`、`\\`、NUL 或越出 package root 永远拒绝。
- 支持合法 percent-encoded UTF-8/unreserved/space 引用，但解析后的 canonical path 必须在 catalog
  中 exact 命中唯一对象；entry 自身不是 NFC 或形成 exact/case-fold collision 已在 3.4 拒绝。
  query 和 fragment 首版全部拒绝，避免多个 URI 映射同一 immutable object 的不明确语义。
- data URI 只接受 RFC 2397 `data:[media-type][;base64],payload`，media type/parameter ASCII 且有界；
  base64 必须 canonical padding，非 base64 使用严格 percent decode。解码 bytes 按 SHA-256 去重为
  internal inline node，参与 resource count、expanded/data limit、dependency 和 closure Hash。
- graph 使用 iterative DFS/BFS、三色 cycle detection、URI depth 32、node 100,000、单 node edge 4,096。
  missing object、cycle、过深、edge overflow、identity drift 或同一 object 的不兼容角色均 fail closed。
- selected-root known closure 中每个 node 标记 `required=true`；package 其余对象保留
  `required=false`。source closure Hash 对完整 immutable namespace、selected root、required facts、
  detected kind 和排序 edge 做 domain-separated canonical SHA-256，ETag 不进入内容 Hash。

### 19.8 Task 3.6 Root selection

- classifier 先找出所有 structurally valid `TILESET_JSON` candidate。显式 `selectedRootHint` 必须先经
  同一 URI/path policy canonicalize，exact 命中且确为 Tileset；失败时不 fallback。
- 无 hint 时，若 package 根目录（canonical path 不含 `/`）恰有一个 valid Tileset，选择
  `UNIQUE_TOP_LEVEL`，不依据名称。否则根据 Tileset-to-Tileset external reference edge 计算入度，
  恰有一个入度为零 candidate 时选择 `UNIQUE_GRAPH_ROOT`。
- 没有 candidate 返回 `REJECTED/CONTENT_INVALID/NONE`；多个 top-level 或 graph root 返回
  `REJECTED/ROOT_SELECTION_REQUIRED/AMBIGUOUS`。candidate count 是全部 valid Tileset 数；不选择
  字典序第一项，不猜 `tileset.json`。
- ambiguous result 可以发布完整、稳定、credential-free factual pages，供后续 operator 选择；不带
  source closure Hash。显式选择产生新 request identity 后完整重跑，不能复用旧 ambiguous terminal。

### 19.9 Implicit Tileset 与 Task 4.x 边界

- Task 3.8 能识别并安全验证 `implicitTiling.subtrees.uri` template：仅允许规范 placeholder segment，
  静态 prefix 必须 package-relative 且不能穿越。它记录 typed template edge，但不展开任意坐标。
- 读取 subtree availability、验证 bitstream/count、展开 concrete subtree/content URI 和 inventory
  coordinates 属于 Task 4.3；因此含 implicit template 的 dataset 本轮只能形成 factual inspection，
  后续 applicator 必须保持 limited authorization 未就绪，不能因 root/其他 closure 成功提前 ready。
- Task 4.1 仍负责持久化 explicit tile hierarchy、transform/bounding/refine/geometric error 和 ordered
  content identity。本轮 Tileset 遍历仅用于 bounded URI extraction/root graph，不写 tile rows。

### 19.10 Result 构建与失败原子性

- `InspectionPipelineExecutor` 完成全部 policy/classification/graph/root 后才构造 pages。records 按
  canonical path UTF-8 bytes、object ID 排序；required extensions/dependency IDs 唯一排序；page size
  来自已验证 profile。page、manifest 和 closure Hash 复用 Inspector canonical serializer。成功
  result 先提交 pages 和 `prepare-expansion` envelope，再进入 3.9 upload；非成功 result 不创建 upload。
- 任一 namespace/content/closure/limit/cancel 错误先销毁 scratch，再返回一个稳定且脱敏的 terminal
  result；namespace/limit/identity 失败不提交部分页。root ambiguity 是唯一允许发布完整 non-success
  factual pages 的业务终态。
- API failure、lease loss 或 result page conflict 立即停止，不调用 complete；重试由 Task 3.2 以新
  attempt 执行。result publication 不记录 request DTO、grant URL、raw URI、source JSON 或 archive name。

### 19.11 测试矩阵与完成标准

- namespace：ASCII/Unicode NFC 正例，NFD、Unicode case-fold collision、exact duplicate、directory/
  file collision、empty segment、dot/dot-dot、absolute/drive/backslash、reserved prefix、percent alias、
  Unix symlink/special device 和 3TZ reserved index 负例。
- root：explicit valid/missing/wrong-kind、唯一 package-root、唯一 graph root、多个独立 root、cycle、
  zero candidate、misleading/no suffix，且排序/重复执行结果一致。
- classifier：每种 JSON/binary kind 正例；truncated header、bad magic/version/byteLength、JSON duplicate
  key、深度/尺寸阈值、ambiguous JSON、suffix mismatch、GLB/B3DM embedded JSON、I3DM/PNTS/CMPT/subt
  header 正反例。
- closure：Tileset content/external Tileset、multiple contents、glTF buffer/image、metadata schema、
  subtree buffer、I3DM external glTF、relative base resolution、missing/cycle/depth/count/edge limit、
  HTTP(S)/protocol-relative/file/other scheme、query/fragment、encoded traversal/separator/NUL。
- data URI：base64 与 percent 正例、invalid alphabet/padding/media type、单项 64 MiB 边界、累计 8 GiB
  预算、相同 bytes 去重和 Hash 重放。测试使用小型 generated fixtures 模拟边界，不提交巨大文件。
- 官方 corpus 至少覆盖现有固定 commit 下的 glTF content、multiple contents、valid 3TZ，并按 license
  manifest 增补 external Tileset、implicit root 和 binary subtree 的可追溯样本；下载 cache 不提交。
- 运行新增 GTest、完整 Worker CTest、fixture/corpus/profile validator、r4 amd64 禁网构建和离线
  bundle；同时运行 Java/C++共享 Schema fixture、OpenSpec strict、两个仓库 staged/unstaged
  `git diff --check`。全部通过后才勾选 3.4-3.10，production claim 仍为 false。

### 19.12 Task 3.9 Worker staging upload

- Inspector scheduling client 新增独立 `prepareExpansion`、paged `expansionUploadPlan`、
  `reportExpandedUpload` API；这些是 lease-bound scheduling DTO，不把 PUT grant 或 writable prefix 加入
  Inspector V1 evidence。所有调用绑定 inspection/request/attempt/worker/lease 和 identical result
  envelope Hash，API conflict/lease loss 立即停止。
- upload plan item 使用 enum 区分 `SERVER_COPY_SOURCE`、`WORKER_PUT_ARCHIVE_ENTRY`、
  `WORKER_PUT_INLINE_DATA`、`ALREADY_STAGED`。Worker 只处理两个 PUT mode；directory source 由 API
  applicator server-side copy，避免 Worker 重新 GET+PUT 同一对象。
- 扩展 `ObjectTransfer` 为有界 `uploadStream`：curl read callback 从 libzip entry reader 或 strict
  data-URI decoder 拉取固定 buffer，同时统计 consumed bytes/SHA-256、执行 continue predicate、捕获
  ETag。声明 size、observed size 和 expected SHA 必须全部一致；不把 entry 聚合为 vector，也不落盘。
- archive entry upload 在同一个已验证 scratch ZIP 中按稳定 ordinal 重开 `zip_file`；上传前再次核对
  central size/method，上传流结束后复核 entry size/hash。data URI 按 closure 阶段相同 parser 重放并
  流式 decode/hash，相同 inline object 只上传一次。
- Worker 只能使用 API 返回的 exact staging presigned PUT。它不解析 URL 得到 bucket/prefix，不附加
  任意用户 header，不重定向到非 broker 批准的 endpoint；grant URL、Location、raw URI、object key、
  source path 不进日志/exception/result。
- upload report 只包含 object ID、size、SHA-256、normalized ETag。API 返回 `ALREADY_STAGED` 且 identity
  相同时跳过重传；不同 identity conflict。全部 Worker PUT 闭合后才调用 `complete`，scratch 在
  complete 成功或任一异常/cancel/lease loss 后 RAII 清理。
- profile 的 inspection hard deadline 从 120 秒提升为 900 秒，以覆盖最大 2 GiB download、8 GiB
  streaming expansion/hash/upload；45 秒 lease、15 秒 heartbeat、3 attempts 不变。所有 I/O chunk
  检查 cancel/deadline，heartbeat 不延长 hard deadline，PUT grant 不超过剩余 deadline。

### 19.13 Task 3.10 Worker 安全与恢复矩阵

- archive 继续覆盖 corrupt EOCD/central/local、ZIP64 drift、encrypted/multivolume/unknown method、
  nested archive、ratio bomb、entry/count/size/path 限额；3TZ 覆盖 index placement/hash/offset/order 与
  Zstd93 正反例。失败不能调用 prepare expansion 或产生 staging upload。
- classifier/closure 覆盖 malformed/truncated JSON/binary header、duplicate JSON key、excess nesting、
  magic/version/length mismatch、ambiguous JSON、missing resource、URI/self/external Tileset cycle、
  dependency/depth/count overflow、forbidden scheme、encoded traversal 和 data URI bomb。
- fault-injection fake API/ObjectTransfer 覆盖：page N 后 task loss、prepare-expansion response loss、
  upload 中断、upload 成功但 report 丢失、report 成功但 complete 丢失、duplicate executor、旧 lease、
  cancel during zip/data stream、stale plan 和 cleanup 后重试。相同 identity 必须幂等，不同 identity
  必须 fail closed，scratch 最终均清理。
- credential boundary 测试扫描 request/result/plan DTO serializer、logger、exception、core dump-safe
  message 和 fixture，确保无 bucket credential、presigned URL、lease token、HMAC signature、source
  payload、raw URI、local scratch path。HTTP fake 只断言脱敏 host/purpose/object identity。
- official/generated corpus 新增可重复的 recovery manifest，记录 fixture SHA-256、expected stage/code、
  maximum allocation/upload count 和 no-partial-publication 断言；大限额使用 sparse/generated stream，
  不提交 64 MiB/8 GiB 二进制或临时 object-store volume。
- r4 完整 CTest 与禁网镜像之外，使用固定临时 MinIO/API integration harness 验证真实 presigned PUT
  redirect policy、流式 ETag、重复报告和 staging orphan；测试容器、volume、grant 和下载 cache 不入 Git。

### 19.14 已确认决策（2026-08-18）

1. 引入固定 Vcpkg baseline 的 `utf8proc + uriparser`，生成独立 r4 base/bundle；不手写 Unicode/RFC
   3986 核心规则，也不采用仅允许 ASCII 路径的过度限制方案。
2. package path 必须已经是 NFC，使用 Unicode case-fold key 拒绝碰撞；不静默重命名或归一化上传项。
3. 新增 JSON 64 MiB、nesting 128 限额；data URI 单项 64 MiB且累计计入现有 expanded 8 GiB 预算。
4. package-relative URI 接受安全的单次 percent-decoded UTF-8，但拒绝 encoded separator/NUL/traversal；
   query/fragment 首版全部拒绝。
5. Inspector V1 首次发布前新增 `requiredExtensions` 并同步更新 Schema 摘要；未知 required extension
   产生 global-preview-only factual warning，不允许自定义 fetch。
6. root ambiguity 发布完整 non-success factual pages，不带 closure Hash；用户选择后用新 request
   identity 全量重跑。
7. implicit URI template 本轮只做 syntax/namespace 安全验证；availability/concrete closure 由 Task 4.3
   完成，在此之前 limited authorization 保持未就绪。
8. Task 3.5 以无 Java-side 3D Tiles temp file 的审计守卫完成；API applicator 只使用 MinIO
   stream/stat/copy，不修改无关 shapefile/GDAL 临时目录。
9. 新增 attempt-scoped expanded-upload allocation 表和 resource-dependency 表；partial attempt 不直接
   写永久 inventory，accepted evidence 最终写入现有 `three_d_source_resource`。
10. 使用两阶段完成：result pages + `prepare-expansion` -> exact staging upload -> Worker complete
    -> `APPLYING` applicator -> factual READY；MinIO 大 I/O 不放在 complete 数据库事务中。
11. ZIP/3TZ entry 与 data URI 由 Worker 流式 PUT staging；directory source 由 API server-side copy；
    所有安全 logical resources 都持久化，不只 selected-root required 子集。
12. final expanded key 由 source closure/object ID/SHA-256 content identity 生成；API 对 source/staging
    强 SHA-256 复验后发布，ETag/metadata 不替代 Hash，cleanup 永不删除 source prefix。
13. inspection hard deadline 从 120 秒调整为 900 秒；lease 45 秒、heartbeat 15 秒、3 attempts 不变。
    staging 默认保留 24 小时；expanded stale 默认延迟 7 天且无引用才清理。
14. Tasks 3.4-3.10 完成后 production claim 仍关闭，等待真实 upload submission adapter；不顺带实现
    Task 4.x 或 normalization artifact。

### 19.15 实施结果与验收证据（2026-08-18）

- 用户已确认本节 1-14 全部决策，Zstd 按 A 方案实现。`utf8proc 2.10.0`、
  `uriparser 0.9.8`、`libzip 1.11.4[zstd]` 和 `zstd 1.5.7` 均来自固定 Vcpkg
  `2025.07.25` baseline；Worker 版本提升为 `0.1.9`。
- package namespace policy 统一覆盖 directory、ZIP/3TZ entry、root hint 和引用 URI：验证严格 UTF-8、
  NFC、Unicode case-fold collision、exact duplicate、directory/file collision、absolute/drive/backslash、
  dot traversal、encoded separator/NUL、reserved prefix、scheme/authority/query/fragment 和 Unix special
  entry。archive 只按稳定 ordinal 流式读取，scratch 由 RAII 和启动 orphan reconciliation 清理，不把
  entry 展开到文件系统。
- bounded classifier 已按 magic/结构识别 Tileset JSON、subtree JSON/binary、glTF JSON、GLB、B3DM、
  I3DM、PNTS 和 CMPT；suffix/media type 只生成 diagnostic。JSON duplicate key、nesting/size、binary
  version/byteLength/section arithmetic、required extension 排序均 fail closed 或形成有界 factual warning。
- closure pipeline 使用 package-relative resolver、strict data URI replay、resource/edge/depth/expanded-byte
  限额和全图 cycle detection 构建稳定 graph。root selection 只接受 explicit valid、唯一 package-root 或
  唯一 graph root；ambiguity 发布不带 closure hash 的完整 factual pages，不猜 `tileset.json`。implicit
  template 只做语法/命名空间验证，availability inventory 仍属于 Task 4.3。
- `InspectionPipelineExecutor` 只有在 namespace/classification/closure/root 全部闭合后才构造 canonical
  pages。`ExpansionUploader` 对 ZIP ordinal 和 data URI 使用重放流，上传过程中同步统计 size/SHA-256，
  只有 Hash 闭合后才 report；中断不 report，`ALREADY_STAGED` 同 identity 跳过重传。grant URL、raw
  URI、object key、lease token 和响应 body 不进入日志或 result。
- r4 supply chain 修正了 Vcpkg builtin baseline 安装前过早删除 `.git` 的问题：仓库元数据保留到
  `vcpkg install` 完成后再从 build-base 删除。最终 manifest SHA-256 为
  `2cd66f62f3fa89a7803d0c582ccfc3474902658554f5c6eed75833d78307d318`；build/runtime base image ID
  分别为 `sha256:11b221241538653c6e2b7bc90ab255d98429425010a0cb3d40c76a507ac7223e` 和
  `sha256:937757a341f2af071816d610b9323b5204eaeb9b72aa63bc4520b23fac7d8a84`。
- r4 bundle 在禁网容器中完成 `SHA256SUMS` 校验和重新加载。build/runtime tar SHA-256 分别为
  `926e488bc7bfec09d90a9d2fdaec81851d8b02642827fb89a360c47ca9ec4773`、
  `33fa522545370de8cc16970939679deabd5d9532590be2e5e77502d43d4f00c3`；bundle 位于被 Git 忽略的
  `build/task34-r4-bundle`，未提交下载 cache。
- 最终 `docker/offline/build.sh` 使用已重新加载的本地 base，在外层和 Docker build 均禁网且
  `--no-cache` 的条件下完成 Release 编译、111/111 CTest 和 install。最终镜像
  `3d-tiles-clip-worker:0.1.9-offline-amd64` ID 为
  `sha256:4efaa7d95bae47c74295b824c169646b6a41f6cff97b4ccb00deb7b0ca5ae903`，架构 amd64、运行 UID
  10001、PROJ data 可读，且不含 Vcpkg、CMake、Ninja 或 Git 构建工具。
- 最终补充验证为：命名档位 validator 通过；9 个 fixture case/14 个生成文件逐 Hash 验证；官方
  corpus manifest 的 3 个固定来源、19 个 entry、22 个 coverage key 全部通过；共享 Schema 字节和
  摘要与 API 完全一致。两个仓库的 secret/presigned URL 扫描无命中，production `main` 继续只运行
  现有 B3DM clip worker，不领取 inspection task。

## 20. Tasks 4.1-4.6 Hierarchy Inventory Worker 设计（已确认）

### 20.1 与 API Spec 的共同边界

- 本节与 `api-platform/.spec/20260817-broad-3dtiles-ingest-and-clipping.md` 第 21 节共同描述同一轮
  4.x 实施；协议、枚举、limit、Hash domain 和 fixture 必须双端一致。确认前不修改 Worker 业务代码，
  不勾选 OpenSpec 4.1-4.6。
- Worker 仍是唯一读取/解析不可信 Tileset、subtree 和 content header 的组件。Java 不重新解析包内
  JSON；Worker 不获得 bucket credential 或 listing 权限，只复用 Task 3 已批准的 package catalog、
  exact-object grant、archive ordinal reader、namespace resolver 和 continue predicate。
- Task 4 只产生 factual hierarchy inventory 与 per-content closure evidence，不执行 normalization、
  geometry clipping、metadata compaction 或 authorization subtree generation。production `main` 和
  inspection claim feature flag 继续关闭。

### 20.2 Inspector V1 Hierarchy Manifest

- 推荐在现有 `InspectionResult` 增加独立 `hierarchyManifest` descriptor，并增加
  `HIERARCHY_RESULT_PAGE`。resource pages 保持原 record schema，hierarchy pages 使用 discriminated
  record union；两类 page 各自 canonical SHA-256，再由 manifest domain 汇总，禁止跨类型复用 page。
- hierarchy record 的稳定 ID 使用具名 domain：root document instance 绑定 selected-root object ID，
  external document instance 绑定 parent content ID + source object ID；explicit tile 绑定 document
  instance ID + RFC 6901 pointer，implicit tile/subtree 绑定 document instance ID + scheme + level/x/y/z，
  content 再绑定 tile ID + ordinal。Hash 输入逐字段 length-prefix，禁止用易碰撞的字符串拼接。
- traversal 不能把最多 500 万 tile/1000 万 record 常驻内存。page builder 按确定性 traversal order
  写入 attempt scratch spool，同时流式计算 page/manifest Hash 和计数；全部 validation 成功后才按
  page 顺序上传。spool 路径仅由 attempt identity 构造，受 4 GiB guard，成功/失败/cancel/restart 均
  进入现有 RAII/orphan cleanup。
- duplicate identity、parent/document/source resource 不存在、content ordinal 不连续、manifest count
  不等于实际页、或同 identity 重放 bytes 不一致必须在 Worker/API 任一端拒绝。

### 20.3 Task 4.1/4.2 显式遍历与 Ordered Contents

- selected root 之后以 iterative DFS 遍历 explicit children；frame 保存 document ID、RFC 6901
  pointer、parent tile ID、depth、累计 column-major transform 和 inherited refine。解析 finite
  transform、boundingVolume、geometricError、refine 时使用 typed helper 和 checked limits，不递归
  C++ call stack。
- `content` 与 `contents` 严格互斥。单 content ordinal=0；multiple contents 保留数组顺序并要求每项
  exactly one approved URI field。目标 resource 的 detected kind 决定 ordinary content 或 external
  Tileset，不查看 suffix/media type。
- external Tileset 通过 content ordinal edge 进入新 document instance；同一 source resource 被不同父
  content 引用时分别 materialize instance，并继承各自累计 transform/refine。source resource ancestor
  chain 使用三色 cycle detection，只有真实回环才拒绝；document/tile/content evidence 全部可确定性
  重放并受 instance/nesting/node guard 约束。
- required/used extensions 在 Tileset document、content declaration 和目标 glTF/legacy payload
  分层保留。content capability 使用目标 resource classification，不因同 tile 其他 ordinal 支持而提升。

### 20.4 Task 4.3 Implicit Subtree 算法

- 支持 `QUADTREE` 与 `OCTREE`，按 level-major + Morton Z-order 读取 availability。实现 checked
  `nodesInLevel`、`levelOffset`、Morton encode/decode 和 subtree child coordinate helper；任何指数、
  位移、坐标或 byte-count 溢出在读取前终止。
- JSON/binary subtree 共用 `SubtreeView`。binary header 严格验证 magic/version/jsonByteLength/
  binaryByteLength/padding；buffer/bufferView 校验 index、声明长度、8-byte alignment、checked range。
  availability 只允许 exactly one `constant`/`bitstream`，验证所需 bit count、byteLength、
  `availableCount` 和 trailing zero bits。
- tile availability 至少一个 1，available child 必须有 available parent；content availability 的每个
  1 必须对应 available tile，stream 数必须等于 implicit root content template 数；child subtree
  availability 只对应 subtree 边界下一层。unavailable tile/content/descendant 不生成 record，也不展开
  URI。
- 从 implicit root subtree 坐标开始使用有界 deque 遍历。只有 child-subtree bit=1 才展开 concrete
  subtree URI；只有 content bit=1 才展开对应 ordinal template。每个 concrete URI 再经过 Task 3 的
  namespace resolver 并 exact 命中 catalog，随后补充 typed resource dependency 和 required facts。
- implicit tile bounding volume、geometric error、refine 和 transform 按 3D Tiles 1.1 规则派生；
  availableLevels/subtreeLevels、template placeholder、scheme 与 root bounding volume 的不兼容组合直接
  `HIERARCHY_*_INVALID`。subtree metadata/property table 本轮只分类和记录，不读取成业务属性。

### 20.5 Task 4.4 Extension Facts

- `requiredExtensions()` 重构为同时返回 required/used。两数组验证 string/UTF-8 byte length/count，
  canonical UTF-8 byte order 去重排序，且 required 必须为 used 子集；Inspector Schema 与 Java DTO
  同步加入 `usedExtensions`。
- Tileset 1.1 core multiple/implicit 不要求伪造 extension name。对于固定官方版本的
  `3DTILES_multiple_contents`/`3DTILES_implicit_tiling` adapter，wire evidence 保留原 required/used name
  以及 adapter version；未知 required extension 产生 stable factual reason，使 limited authorization
  fail closed，但不能触发自定义 fetch。
- known-extension registry 使用 enum/table 驱动，不能继续把字符串 set 留在 pipeline 函数体。registry
  区分“可解析 inventory”与“已具备 normalization/limited authorization”，Task 4 只提升前者。

### 20.6 Task 4.5 Closure Hash

- dataset closure 在 implicit concrete dependency 补齐后按既有 domain 重新计算。每个 content 另算
  `CONTENT_RESOURCE_CLOSURE_V1`：从 concrete source resource 开始 iterative graph walk，收集 reachable
  resource object ID、SHA-256、size、kind/version 和 edge；排序后 canonical hash。
- external Tileset content closure 包含子 document reachable closure；implicit content 使用坐标展开后
  的 concrete resource。ETag、URI、grant、bucket/key、scratch path 和 traversal order不进入 Hash。
- Worker 在 CONTENT record 发布 closure Hash 和 closure version；Java 必须从 permanent resource/
  dependency graph 独立重算并比较。不同则 entire hierarchy generation fail closed，不能仅把单 content
  标成 warning 后继续。

### 20.7 Limits、Fixture 与完成门槛

- 推荐在 `resource-limit-profiles-v1.json` 新增 `hierarchy`：maximumNodes=5,000,000、
  maximumNodesPerDocument=1,000,000、maximumTileDepth=512、maximumExternalDocuments=100,000、
  maximumExternalNesting=32、maximumContentsPerTile=64、maximumImplicitSubtrees=100,000、
  maximumSubtreeLevels=16、maximumRecords=10,000,000、maximumPageRecords=1,000、
  maximumPageJsonBytes=1 MiB、maximumSpoolBytes=4 GiB。loader 与协议 hard limits 交叉校验，所有代码
  只引用 typed limit，不出现 magic number。
- generated fixtures 覆盖：nested external、cycle、合法多父复用、single/multiple/external mixed
  contents、content/contents 冲突、mixed explicit/implicit、quad/oct、JSON/binary subtree、constant与
  bitstream、sparse availability、unavailable descendants、bad bufferView/count/padding/trailing bits、
  template missing/collision、unknown required extension 和 deterministic closure replay。
- 官方 corpus 固定 Cesium 3D Tiles 与 Validator 的 multiple contents、external、implicit quadtree/
  octree、sparse JSON/binary subtree 样本及 license/SHA；只提交 manifest/小型 generated fixture，下载
  cache 不进 Git。
- 4.x 功能完成前运行共享 Schema/canonical fixture、全量 CTest、profile/fixture/corpus manifest validator、
  secret scan、`git diff --check`，并与 API Java 8/PostgreSQL/OpenSpec 验收结果一并记录。完整 500 万
  available tile streaming/spool benchmark、下载型 corpus 与禁网 offline image 重建作为 production claim
  启用门槛单独保留，不阻塞 factual inventory 代码关闭，但未通过前不得打开生产 claim。

### 20.8 已确认决策（2026-08-20）

1. 采用同一 Inspector V1 的 resource + hierarchy 双 manifest，而不是另建 hierarchy scheduler。
2. API 使用独立 additive hierarchy generation/document/tile/subtree inventory，不激活旧 B3DM index。
3. 支持 3D Tiles 1.1 core，并为固定官方版本的 1.0 implicit/multiple extensions 加 adapter；其他 draft/
   vendor 变体保持 global-preview-only。
4. 接受 20.7 的首版 hierarchy limits，尤其 500 万 available tile 与 4 GiB attempt spool。

### 20.9 实施结果与验收证据（2026-08-21）

- `InspectionResult`、runtime client 和 contract validator 已支持 resource/hierarchy 双 manifest 及
  `HIERARCHY_RESULT_PAGE`。hierarchy pages 使用独立 record union、page SHA-256 和 manifest SHA-256，
  Worker 只在 resource closure 与 hierarchy inventory 全部成功后发布终态；共享 Schema SHA-256 为
  `c1f3428b8e6610bcb08a5cd6e615b0ca5174b3402634ea8744f9a23473590d9a`。
- 显式 inventory 保存 document/tile/content 分离记录，支持累计 transform、tile/content bounding
  volume、refine 继承、geometric error、ordered `content`/`contents`、external Tileset instance 和真实
  祖先回环拒绝。同一 Tileset resource 可从不同 parent content 形成独立 document instance，不把合法
  DAG 误判为 cycle；1.1 core 与固定 1.0 multiple/implicit adapter 共用同一事实模型。
- implicit inventory 已支持 JSON subtree 与 `subt` binary container，统一校验 buffer、bufferView、
  constant/bitstream、availableCount、trailing bits、parent/content availability、QUADTREE/OCTREE Morton
  坐标和 child subtree。遍历只物化 available records，缺失 concrete URI、不可用 parent、错误 stream
  数和任何 named limit 超限均返回稳定 bounded failure。
- required/used extensions 在 resource、document、content 和 dataset 汇总层保持 canonical UTF-8 顺序；
  unknown required extension 只产生 factual evidence，并确保 limited authorization fail closed。每个
  content 的 `CONTENT_RESOURCE_CLOSURE_V1` 由 object ID、observed Hash/size、detected kind/version 和
  dependency IDs 组成，不含 request ID 或存储凭据；测试确认跨 inspection/request closure Hash 不变，
  同时 record ID 仍按 generation 隔离。
- hierarchy limits 已进入 typed `PipelineLimits`/profile：500 万 available tile、1000 万 hierarchy
  records、每 tile 64 contents、10 万 subtree、document/depth/page/spool 等均在展开或写 page 前检查。
  protocol fixtures 同步覆盖双 manifest；pipeline tests 覆盖 external reuse/cycle、ordered contents、legacy
  adapter、mixed explicit/implicit、sparse JSON、binary unavailable content 和 traversal limit。
- 验收结果：容器内 Release 编译及 CTest 119/119；fixture generator `-VerifyOnly` 验证 9 cases/14 files；
  official corpus manifest `-ValidateOnly` 验证 3 sources/19 entries/22 coverage keys；resource profile
  validator、共享 Schema Hash、OpenSpec strict、`git diff --check` 和敏感凭据模式扫描通过。API 侧同步
  160 tests 通过，并完成 PostgreSQL 15 重复迁移/rollback 验证。
- production inspection claim 保持关闭。完整 500 万 available tile 压测、下载型 corpus 与 offline image
  重建仍是启用生产 claim 的前置条件；normalization、授权 subtree 重写和路由接入继续由 Tasks 5/9 实现。

## 21. Tasks 5.1-5.6 Normalizer V1 与 Canonical Pipeline Worker 设计（已确认）

> 用户于 2026-08-22 确认 21.8 的全部关键决策，允许按本节开始实现。

### 21.1 与 API Spec 的共同边界

- 本节对应 API 主 Spec 22，连续实现通用 Normalizer V1 contract/runtime、approved resource manifest
  client、lease session、task scratch、canonical artifact common envelope/validator seam、attempt staging
  upload 和 synthetic deterministic/failure tests。
- 本节不实现真实 source format normalization：GLB/glTF/B3DM mesh 属于 Tasks 6.x，PNTS/I3DM/CMPT
  属于 Tasks 7.x，复杂 metadata mapping 属于 Tasks 8.x。Task 5 只验证“调度和制品基础设施能够安全承载
  这些 family”，不注册 production family handler，不把 Worker `run` 的既有 B3DM clip loop 改成
  normalization loop。
- 与 Inspector V1 类似，Task 5 可以提供完整 `runOnce`/lease session/executor seam 和 fake transport
  tests，但 production normalizer polling 默认关闭。后续 Task 6 注册首个真实 `MESH_GLTF2` handler 并
  通过 corpus gate 后，再通过独立命令/环境开关启用。

### 21.2 目录、模块与运行时隔离

- 新增 `normalization` 模块，不把 manifest/claim/result JSON 塞进现有 `task::WorkerRuntime` 或
  `inspection` namespace：
  - `include|src/clip_worker/normalization/normalization_contract.*`
  - `include|src/clip_worker/normalization/normalization_scheduling.*`
  - `include|src/clip_worker/normalization/normalization_runtime.*`
  - `include|src/clip_worker/normalization/canonical_artifact.*`
  - `include|src/clip_worker/client/normalization_api_client.*`
  - `config/normalizer-protocol-v1.schema.json`
- `NormalizationTaskRuntime` 只负责 claim 和调用一个已注册 `INormalizationExecutor`；
  `NormalizationLeaseSession` 统一执行 manifest page、heartbeat、prepare upload、upload report 和 complete/
  fail，并在 cancel、lease lost、deadline 或 terminal callback 后拒绝继续访问资源。
- scratch 使用 task/attempt scoped directory，名称来自已校验的 opaque attempt ID；创建前校验 root 与
  resolved child 关系，成功、失败、取消、租约丢失和异常 unwind 都清理。不能用 source path、package
  URI、bucket/key 或未验证环境变量拼 scratch path。
- `src/app/main.cpp` 后续只增加明确的 normalizer command/mode 和独立环境变量前缀；Task 5 默认不启动
  production poll loop。现有 `run` clip worker、`inspect` 命令、算法 `v8` 和对象传输行为保持兼容。

### 21.3 `THREE_D_TILES_NORMALIZER_V1` wire contract

- Worker Schema 与 Java canonical Schema 逐字节镜像并固定 SHA-256。claim capabilities 包含 worker ID、
  protocol/schema Hash、normalization version、支持的 `canonicalFamily + contractVersion`、resource
  profile/version Hash 和 tool/validator versions；未知 family/contract/profile 不能 claim。
- claim task 绑定 task/attempt/request/lease/hard deadline、tile content identity、source closure Hash/
  version、normalization version、canonical family/contract、limit profile 和 credential-free resource
  manifest descriptor。bucket、object key、final key、长期凭据和任意 HTTP header 不进入 DTO。
- resource page record 使用 object ID、canonical package-relative name、role、kind/version、media type、
  size、SHA-256、required/used extensions、sorted dependency IDs 和短期 exact GET grant。page/manifest
  canonical Hash 采用 domain-separated SHA-256；未知字段、重复 object ID/path、dependency 越界、乱序、
  page count/hash 漂移或 grant purpose 不匹配均拒绝。
- phase 枚举首版为 `CLAIMED`、`MANIFEST_FETCH`、`RESOURCE_DOWNLOAD`、`DECODE`、`NORMALIZE`、
  `VALIDATE`、`UPLOAD_PREPARE`、`UPLOAD`、`COMPLETE`，只能单调前进。heartbeat 返回 lease expiry、hard
  deadline 和 cancel flag；hard deadline 不能被 heartbeat 延长。
- prepare-upload request 只在本地 canonical validation 成功后发送，包含 family/contract、declared
  output size/SHA-256、semantic Hash、validation manifest Hash 和 bounded summary。响应只给一个 exact
  attempt staging PUT URL/expiry；Worker 上传后再次计算/确认 bytes Hash，报告 observed ETag/size/SHA，
  不复制到 final、不覆盖已存在对象。

### 21.4 Common canonical artifact contract

- 三个 family 首版物理产物均为单文件 GLB 2.0。GLB JSON/BIN 闭合，所有运行时必需 buffer/image 使用
  bufferView 内嵌；不得保留 external URI、任意 data URI、网络 schema 或 source object fallback。
- 坐标 basis 为 glTF 2.0 content-local 右手坐标语义。Tileset accumulated transform、document
  `gltfUpAxis`、authorization scope 不进入 artifact；source content 内的 RTC/container/node/scene
  transform 按 family writer 规则确定性表达。后续 clip executor 显式接收 hierarchy transform evidence，
  防止重复轴变换。
- deterministic writer 固定 JSON key/array traversal、extension 排序、accessor/bufferView/buffer packing、
  4-byte alignment/zero padding、float serialization、quaternion sign、unreachable object removal 和 output
  filename。相同 manifest bytes、normalization version、contract/tool versions 必须产生 byte-identical GLB
  与相同 semantic Hash。
- common manifest 记录 source closure、family/contract、normalization version、coordinate basis、artifact
  digest/size、semantic Hash、scene/node/primitive/accessor/buffer/image/feature/metadata counts、extension
  whitelist、feature identity model、validator/tool versions 和 validation Hash；不记录 source URI、grant、
  bucket/key、用户、scope 或原始业务属性。

### 21.5 Family contract 和 validator registry

- `MESH_GLTF2` contract 只接受 registry 明确允许的 mesh primitive/topology、attribute/component type、
  node/scene transform、material/texture 和 feature mapping；输出不保留未解码的 required compression。
  Task 5 仅提供 positive/negative canonical GLB fixture，不实现 source mesh decoder。
- `POINT_GLTF2` contract 使用 point primitive，position/color/normal/feature ID 及支持属性必须同 count、
  可达且 bounds 正确；Task 5 只验证 canonical fixture，PNTS decoder/compactor 留给 Tasks 7.1/7.2。
- `INSTANCE_GLTF2` contract 使用共享 embedded model 与确定性 per-instance TRS/feature identity 表达；
  quaternion normalization/sign、array count 和 transformed model bound 证据受 validator 检查。无法无损分解
  的 shear/matrix、external model resolution 和 boundary expansion 留给 Tasks 7.3/7.4，未实现前 handler
  不注册。
- `CanonicalValidatorRegistry` 必须同时匹配 family、contract version 和 validator build。common validator
  检查 GLB header/chunk/alignment、JSON 类型、URI 闭合、finite number、node cycle、scene reachability、
  extension whitelist 和 summary count；family validator 再检查 primitive/feature/metadata 语义。缺 family
  validator 时返回 capability mismatch/unsupported，不能信任远端或 fixture 自报 `valid=true`。

### 21.6 Bounded resource materialization 与上传

- Executor 按 manifest dependency graph 和 canonical object ID 顺序下载 approved objects；每次 transfer
  前后检查 lease/cancel/deadline，验证 observed size/SHA-256，不自动跟随 manifest 外 dependency。redirect
  仍受 `ObjectTransfer` 的 scheme/host/presigned policy；禁止 fallback 到 local filesystem、bucket listing
  或 source URI 网络请求。
- source、decoded、scratch、single decoder、GLB JSON/BIN、output 和 upload bytes 使用 resource profile
  的独立 accounting；所有 count*stride、offset+length、padding、array growth 在分配前 checked。Task 5
  synthetic executor 也必须经过同一 accounting，不允许测试路径绕过 limits。
- canonical GLB 和 validation manifest 完成后先本地 reparse/revalidate，再调用 prepare-upload。上传只写
  attempt staging URL，使用 streaming SHA-256 和 cancellation predicate；partial upload、连接中断或 lease
  lost 不发送成功 report。
- API durable publisher 负责 final copy/READY，因此 Worker complete 只表示 staging bytes 与 validation
  evidence 已提交。late/duplicate complete 只有 task/attempt/request/output identity 完全相同才允许幂等，
  不同 bytes 必须作为 protocol/non-deterministic conflict。

### 21.7 日志、错误与测试矩阵

- normalizer 日志只包含 worker/task/attempt 的 opaque ID、phase、family/version、bounded counts、duration
  和 stable error code。URL、token、Authorization header、bucket/key、package path、业务 metadata、源 JSON/
  binary、stack local path 不进入 message/context；异常文本先映射为 enum + bounded safe message。
- 测试覆盖 Schema/fixture canonical Hash、claim capability mismatch、manifest page 缺失/乱序/重复、dependency
  越界、observed hash drift、phase regression、cancel/deadline/lease lost、crash before/after staging、partial
  upload、duplicate callback、resource limit 和 scratch cleanup。
- canonical fixture 覆盖三个 family 的最小正例，以及 external URI、unknown required extension、bad GLB
  length/alignment、NaN/Infinity、node cycle、unreachable resource、count/bounds/feature mapping 漂移；同一输入
  两次运行比较 GLB bytes、semantic Hash、manifest Hash 和 statistics。
- 完成门槛为 Worker Release build/全量 CTest、API/Worker Schema 与 fixture SHA-256 一致、resource profile
  validator、credential pattern scan、`git diff --check` 和 API 主 Spec 22.8 的跨仓验收。Task 5 完成后仍不
  启用 production normalizer claim。

### 21.8 待用户确认的关键决策

1. 使用独立 Normalizer V1 namespace/runtime/API client，不复用 Inspector 或旧 clip `task` contract。
2. canonical GLB 保持 content-local；tile/document transform 与 `gltfUpAxis` 不烘焙，后续 clipping 显式组合。
3. Worker 只写 attempt staging，final immutable publish/冲突检查/cleanup 全部由 API durable publisher 完成。
4. Task 5 只注册 synthetic executor/common validator seam；真实 family handler 分别到 Tasks 6/7 才注册，
   因此本节完成后 production polling 仍关闭。

### 21.9 实施结果与验收证据（2026-08-22）

- 已新增独立 `normalization` contract/runtime/canonical artifact 模块与 `normalization_api_client`，并接入
  CMake；未改写 legacy clip `run` loop，也未注册真实 GLB/B3DM/PNTS/I3DM converter。Runtime 只接受完全
  匹配 protocol/schema/profile/normalization/family/contract/fixed validator 的 claim，并把 per-family
  maximum input/output bytes、hard deadline 和 lease 贯穿 manifest、download、validation 与 staging upload。
- resource manifest/page 会验证 canonical 排序、page/manifest Hash、record count、dependency closure、grant
  purpose/expiry 和 resource byte 上限；duplicate page 仅在 bytes identity 完全相同时幂等，漂移立即丢失
  lease。tool version 不允许重复，family validator 必须存在于固定 tool inventory。
- common GLB validator 验证 GLB 2.0 header、长度、4-byte alignment、唯一 JSON/BIN chunk、embedded buffer、
  bufferView/accessor range/stride/alignment、finite float、POSITION min/max、attribute count、indices/topology、
  node transform/cycle/multi-parent、default scene reachability、external URI/schema、sorted unique extension
  allowlist，以及 point/instance 的结构和 accessor 对齐约束。
- 三个 positive synthetic fixture 分别覆盖 `MESH_GLTF2`、`POINT_GLTF2`、`INSTANCE_GLTF2`；negative fixture
  覆盖 external URI、unknown required extension 和 bad declared length。generator `-VerifyOnly` 验证 6 个
  fixture 及 manifest SHA-256 全部通过；共享 claim/page/upload fixture 与 API 镜像逐字节一致。
- 使用仓库本地离线依赖在 WSL 以 C++17 编译 normalizer 相关 production sources 和 tests，13 tests、
  0 failure；覆盖协议 round-trip/fail-closed、canonical positive/negative、version upgrade、stale lease、
  duplicate page、partial upload 和 crash retry。`git diff --check` 通过，常见 credential/private-key 模式
  扫描无命中，测试临时依赖与构建目录已删除。
- 用户启动 Docker Desktop 后，使用本机
  `3d-tiles-clip-worker-build-base:ubuntu24.04-vcpkg2025.07.25-r4-amd64` 和对应 runtime base，执行
  `docker build --network=none -f Dockerfile.offline`。vcpkg manifest Hash 校验通过，Release CMake/Ninja
  完整编译 47 个步骤，132/132 CTest 全部通过、0 failure，其中 Normalizer V1 13/13；生成镜像运行
  `--version` 返回 `0.1.9`。本轮仍未注册真实 family handler，production normalizer polling 保持关闭。

## 22. Tasks 6.1-6.9 Mesh Normalizer/Clipper Worker 设计（已确认）

> 本节与 API 主 Spec 24 一一对应。用户已于 2026-08-22 确认本节设计，后续实现、测试和 OpenSpec
> 任务勾选均以本节为准。

### 22.1 模块拆分与 legacy adapter

- 把当前 1300+ 行 `src/clip/b3dm_clipper.cpp` 拆为保持既有 namespace/入口兼容的模块：
  - `mesh/mesh_scene.*`：typed scene、primitive、attribute、material、texture、feature binding、bounds；
  - `formats/gltf/gltf_mesh_reader.*`：GLB/glTF JSON、buffer/accessor、scene/node、material/texture 读取；
  - `formats/b3dm/b3dm_mesh_adapter.*`：B3DM feature/batch table 与 RTC/container transform；
  - `formats/gltf/meshopt_decoder.*`、扩展后的 `draco_decoder.*`；
  - `formats/image/texture_codec.*`：PNG/JPEG/WebP/KTX2 -> RGBA 与 deterministic PNG；
  - `clip/mesh_scene_clipper.*`、`clip/texture_masker.*`；
  - `normalization/mesh_normalizer.*`、`normalization/mesh_canonical_writer.*`、
    `normalization/mesh_canonical_validator.*`；
  - 现有 `B3dmClipper` 只负责 legacy task contract -> typed pipeline -> legacy B3DM/WebP writer adapter。
- 拆分期间禁止改变旧 `B3dmClipper::clip` public signature、`B3dmClipResult`、observer、legacy task JSON、
  `WorkerRuntime` 的 `run` 行为和 `v8` algorithm advertisement。旧 supported fixtures 增加 output bytes Hash，
  用 golden 锁住 JSON ordering、buffer packing、WebP settings、B3DM alignment、statistics 和 compatibility logs。

### 22.2 Typed scene 与内存模型

- `MeshScene` 保存一个 canonical default scene、ordered nodes、ordered meshes、materials、samplers、textures、
  decoded images 和 optional legacy property table。Node 保存 source stable ordinal、parent/children、local matrix/TRS
  和 mesh reference；Mesh 可被多个 Node 引用，不在 normalization 阶段按 node 复制。
- `MeshPrimitive` 使用 structure-of-arrays：finite float/double POSITION，optional NORMAL/TANGENT/TEXCOORD_0/
  TEXCOORD_1/COLOR_0，uint32 indices，optional discrete legacy feature IDs；每个 array count 必须与 POSITION
  一致。所有 vector growth 经 `MeshResourceAccountant` 预核算，accountant 从 V2 profile 读取，不在 decoder/
  reader/writer 内散落 magic number。
- normalization 保留 source local node transforms；clipping 计算 accumulated node transform。共享 mesh 在不同
  accumulated transform 下需要不同裁切结果时 clone，在 transform bytes 完全相同时 deterministic reuse。
- derived bounds 分为 primitive local、mesh local、node accumulated 和 default scene world-evidence bounds；writer
  只写 glTF POSITION accessor min/max，validator 重新推导其余 bounds并比较内部 evidence。

### 22.3 Approved resource materialization 与 GLB/glTF reader

- `MeshNormalizationExecutor` 按 page number 获取并验证全部 manifest page，再按 canonical object ID streaming
  下载到 attempt scratch。每个文件名只使用 object ID 的安全编码，不使用 package path；download 完成必须同时
  匹配 declared size/SHA-256，ETag 只作诊断，不替代 SHA。
- closure 必须存在唯一 `CONTENT` record，detected kind 为 GLB/GLTF/B3DM；其 dependency graph 必须覆盖 reader
  发现的全部 buffer/image。external URI 通过从 Inspector 抽出的 shared canonical relative resolver 解析到
  `packageRelativePath -> objectId`；data URI bounded decode 后按 SHA-256 匹配 inline manifest record。任何缺失、
  歧义、manifest 外 dependency、absolute/network/file URI 或 hash drift 都在 decode 前停止。
- reader 支持 embedded GLB BIN、多 external buffers/images、interleaved bufferView、normalized core accessor、
  bounded sparse accessor、optional indices、default scene reachability、matrix 或 TRS、multiple nodes/meshes/
  primitives 和 mesh reuse。canonical reader 输出解码后的 typed values，不把 source accessor/bufferView object
  model 泄漏到 clipper/writer。
- 支持属性为 POSITION、NORMAL、TANGENT、TEXCOORD_0、TEXCOORD_1、COLOR_0 和 B3DM legacy `_BATCHID`；
  其他 custom attribute、skin/joint/weight、morph、animation、camera/light、direct feature metadata extension 在
  本轮映射为 stable unsupported，不静默丢弃。
- primitive `TRIANGLES` 原样解码；`TRIANGLE_STRIP` 按奇偶 winding 展开且跳过重复 index 形成的 degenerate
  triangle；其他 mode 返回 `CONTENT_PRIMITIVE_MODE_UNSUPPORTED`。所有 index 在 reader 后立即 range check。

### 22.4 B3DM feature 与 legacy property table

- `B3dmMeshAdapter` 继续使用严格 B3DM/GLB header parser，同时新增有界 Feature Table property reader：支持
  JSON `BATCH_LENGTH`、JSON/binary `RTC_CENTER`，其他 feature semantics 首版 unsupported。兼容但非标准的
  4-byte GLB offset/trailing zero padding只在 legacy v8 保留 warning，新 canonical writer 永远输出标准 GLB。
- legacy `_BATCHID` 接受 unsigned byte/short/int 或 finite integral float SCALAR，并在 typed scene 内统一为
  uint32。BATCH_LENGTH=1 且缺 semantic 可补 0；BATCH_LENGTH>1 缺 semantic、ID 越界或 triangle 内 feature
  不一致均 fail closed。
- Batch Table JSON 支持 property -> BATCH_LENGTH 个 number/boolean/string scalar；Batch Table Binary 支持
  byteOffset + componentType + SCALAR/VEC2/VEC3/VEC4 numeric column。reader 对 offset、alignment、row stride、
  total bytes、UTF-8/string length 和 property count 使用 profile guards，拒绝 object/array/null/hierarchy。
- canonical writer 把 geometry 引用的 old IDs 升序 dense remap 为 `_FEATURE_ID_0`，生成
  `EXT_mesh_features` featureId binding 与 `EXT_structural_metadata` inline schema/property table；clipper 再按
  surviving IDs compact。property serialization、class/property name 排序和 binary packing 固定，removed row
  bytes 不复制到 output。

### 22.5 Codec adapters 与依赖供应链

- `vcpkg.json` 在当前固定 baseline 上增加 `meshoptimizer`、`libpng`、`libjpeg-turbo`、`ktx`，保留现有
  `libwebp` 和 `draco`；CMake 只通过 imported targets 链接。offline bundle 脚本必须下载、校验并缓存新增
  ports/source archive，Docker `--network=none` build 是完成门槛。
- `DracoDecoder` 接收 `DracoDecodeLimits`，不再使用内部 hard-coded 5M/10M/512MiB；支持连续属性和 legacy
  feature ID，decoder output count/range/finite 检查后写 uncompressed typed arrays。legacy stale count
  compatibility 与 structured observer 保留，新 normalizer 通过 typed diagnostic 决定 accept/unsupported。
- `MeshoptDecoder` 根据 bufferView extension 在分配前验证 source byte range、count、byteStride、mode、filter、
  expected decoded length；分别调用 vertex/index/index-sequence decode 和 octahedral/quaternion/exponential filter。
  输出必须与引用 accessor component/type/stride 对齐，extension/fallback bytes 不复制到 canonical GLB。
- `TextureCodec` 先 probe header 再分配 RGBA；PNG/JPEG/WebP/KTX2 都返回同一 `RgbaImage`。KTX2 只接受
  single 2D Basis ETC1S/UASTC，使用 KTX-Software transcode 到 RGBA32 level 0；unsupported container shape、DFD、
  orientation 或 transfer semantics 返回 typed texture code。
- deterministic PNG 固定 RGBA8、compression level 9、default strategy、adaptive filter set、无 ancillary chunk；
  encode 后重新 decode exact compare。codec 名称、版本、build SHA 纳入 normalizer tool inventory 和 semantic
  version evidence，不能只记录通用 `validator`。

### 22.6 Mesh clipping、纹理 mask 与 output writer

- `MeshSceneClipper` 接收 typed scene、EPSG:4490 WKB、tileset transform、gltfUpAxis 和 clip options；legacy
  adapter 从旧 task 填充这些参数，未来 Task 9 从 normalized content identity 填充。normalizer 本身不接收 scope，
  只生成 content-local canonical scene。
- `AuthorizationScope` 修复 antimeridian circular center/unwrapped longitude，并把 WKB polygon/ring/point、densify
  segment 和 triangulation count 纳入 profile。现有 BVH query 仍是保守优化；测试要求其结果与 exhaustive
  authorization triangle traversal byte/area equivalent。
- `TriangleClipper` 的连续 attribute interpolation 扩展到 tangent/两个 UV/color；normal/tangent 输出归一化，
  tangent handedness 保留，feature ID 由 validated uniform source triangle 离散继承。具名 tolerance 进入独立
  policy struct，禁止在多个文件复制常数。
- `TextureMasker` 收集所有 retained material slots 对 image 的 UV coverage union，支持 TEXCOORD_0/1 和 core
  REPEAT/CLAMP/MIRRORED_REPEAT。每次 raster 前验证 repeat span、pixel bounds 和 multiplication；mask 外 RGBA
  全零，输出 PNG decode 后逐像素验证。
- `MeshCanonicalWriter` 按 source mesh/primitive/material order 处理 surviving data，使用 exact float bits、normal/
  tangent/UV/color/feature ID 组成稳定 vertex key，重建 U16/U32 indices 与 accessor min/max。empty scene 返回
  typed EMPTY result，不生成最小占位 GLB。
- `MeshCanonicalValidator` 在 common validator 后检查 index value、实际 POSITION bounds、attribute count、
  material/texture/image/sampler reference、PNG exact decode、no compression/external URI、feature/property mapping、
  node/scene derived bounds 和 unreachable bytes。legacy B3DM writer 继续单独走 B3DM reparse/8-byte alignment gate。

### 22.7 Normalizer command、错误映射与 cleanup

- 新增 `run-normalizer` command，不把 Mesh executor 塞进 legacy `WorkerRuntime`。配置统一使用
  `CLIP_WORKER_NORMALIZER_*` 前缀，至少包括 control-plane URL、worker ID、auth header、scratch root、poll/
  heartbeat/transfer timeout、resource profile path 和 enablement；缺 required config 直接启动失败且日志不输出值。
- scratch root 必须是显式配置的非根目录，attempt child 只由 validated opaque attempt ID 派生；创建/删除前
  resolve 并验证 child 位于 root 内。成功、unsupported、retryable failure、cancel、deadline、lease lost 和
  exception unwind 都只删除 exact child，不递归扫描 bucket/source 或未知目录。
- Executor 通过 `NormalizationLeaseSession` 单调 heartbeat，prepare-upload 前调用 Mesh validator，upload 使用
  `ObjectTransfer::uploadStream` 从 canonical GLB file 读取并复验 SHA；report/complete 后不自行 publish final。
- Worker failure 使用 typed `MeshProcessingError`，包含稳定 capability-compatible code、retryable、unsupported 和
  bounded safe message。format/codec/limit/metadata/validation 映射在一处完成；不把第三方 decoder 原文、URI、
  package path、scratch path 或 source JSON 直接上报。

### 22.8 Fixture、Corpus 与完成门槛

- 扩展 synthetic fixture builder，覆盖 direct GLB/glTF external resources、interleaved/sparse accessor、shared
  mesh nodes、multiple primitives、triangle strip、B3DM multi-feature JSON/binary Batch Table、real Draco、real
  Meshopt、PNG/JPEG/WebP/KTX2、two UV sets、all sampler wrap modes、nested transforms、holes/MultiPolygon/
  antimeridian/vertical triangle、empty output 和 resource limit。
- negative fixture 覆盖 manifest ambiguity/missing dependency、URI escape、bad GLB/B3DM length、accessor overflow、
  sparse duplicate/out-of-range index、Draco/Meshopt corruption/mode/filter mismatch、KTX2 array/cubemap/3D、texture
  bomb、triangle mixed feature IDs、Batch Table row/offset/type drift、unknown extension 和 metadata leakage。
- legacy tests 增加 committed `v8` B3DM SHA-256 golden；new path 同一 input 重复两次比较 GLB/PNG bytes、semantic
  Hash、validation Hash、feature map、bounds 和 statistics。geometry differential test 比较 BVH/exhaustive 与授权
  polygon expected area，texture test 验证 mask 外 RGBA 四通道严格为零。
- official corpus 只 materialize API 主 Spec 24.10 列出的固定 entries，保留 upstream license。测试清单对每个
  variant 记录 EXPECT_SUPPORTED 或 exact stable reason；未覆盖的 extension 不因 parser 成功而标记 supported。
- 完成门槛：本机/离线 Docker Release 全量 CTest、API tests、protocol/schema Hash unchanged、V1/V2 resource
  profile validator、offline bundle integrity、sanitizer/fuzz-smoke（可在非 Release gate 运行）、credential scan、
  `git diff --check` 和 OpenSpec strict 全通过，才更新 README/capability matrix 并勾选 6.1-6.9。

### 22.9 已确认的关键决策

1. 按本节拆分 typed mesh pipeline，并以 byte-level golden 强制 legacy `B3dmClipper + v8 + WebP` 不变。
2. 新 Mesh normalizer 使用 `normalization-v2`、`canonical-gltf2-v1` 和新 resource profile V2；Normalizer V1
   wire schema 不变，root content 要求 closure 中唯一。
3. 首版支持 core PBR 五类 texture slot、TEXCOORD_0/1、全部 core sampler wrap；material extension 只支持
   `KHR_materials_unlit`，texture transform 等影响 UV/appearance 的 extension fail closed。
4. 首版 metadata 仅为 B3DM legacy scalar/string JSON 与 numeric binary property table，并转换/compact 到
   attribute-backed structural metadata；direct modern metadata 留给 Task 8。
5. canonical/new clipped Mesh 输出 GLB + PNG；legacy clipped B3DM 继续 B3DM + WebP。Task 9 才把 GLB clip
   result 接入 preparation、Tileset rewrite 和 gateway。
6. 增加独立 `run-normalizer` command，但 compose/API 默认都不启用；用户显式开启且固定 validator/tool build
   identity 后才可领取真实 Mesh task。

### 22.10 实施记录

#### 2026-08-22：完成 OpenSpec 6.1

- 新增 `mesh/mesh_scene.*` typed contract，覆盖 ordered scene/node/mesh/primitive、core PBR texture binding、
  RGBA image、legacy property table、primitive/mesh bounds、离散 feature ID 与具名 `MeshResourceLimits`。
- `MeshResourceAccountant` 在累计 vertex、index、decoded byte、texture pixel 前执行 overflow-safe 上限检查；
  `validateMeshPrimitive`/`validateMeshScene` 重新计算 POSITION bounds，并验证 finite 值、stream cardinality、
  index、scene/node/mesh/material/texture/image 引用。
- 旧 `B3dmClipper` 的 clipped primitive 已改为先写 typed `MeshPrimitive`，校验后再交给原 legacy JSON/buffer
  writer；旧 public signature、observer、task contract、WebP codec 和 B3DM alignment 路径未改变。
- glTF X/Y/Z-up 转 3D Tiles Z-up 的矩阵迁入 typed mesh 模块，legacy adapter 只做 task enum 映射；既有
  Y-up/Z-up world geometry 测试继续通过。
- 新增普通 texture、RTC、Y-up、Draco BATCH_LENGTH=0/1 共 5 组 committed SHA-256 golden。Docker Release
  构建和全量 CTest 为 137/137，所有 golden byte-identical，OpenSpec 6.1 已勾选。

#### 2026-08-22：完成 OpenSpec 6.2-6.3

- 新增 `GltfMeshReader`，以 approved in-memory resource map 为唯一依赖来源，支持 direct GLB/glTF、embedded
  BIN、多 external buffer/image、canonical relative URI、bounded data URI、interleaved/normalized/sparse accessor、
  optional indices、multiple node/mesh/primitive、shared mesh、matrix/TRS、TRIANGLES/TRIANGLE_STRIP、core PBR
  五类 texture slot、TEXCOORD_0/1、sampler、unlit，并对 manifest 外 URI、未知属性/extension 和越界引用
  fail closed。
- 新增 `MeshCanonicalWriter`，只保留 default scene 可达资源，确定性重建 embedded GLB、U16/U32 indices、
  accessor bounds、material/sampler/texture/image 引用，并把 retained RGBA image 统一编码为 deterministic PNG；
  direct external glTF 与 canonical GLB round-trip、重复 writer bytes 和 Task-5 common validator 均通过。
- 新增 `B3dmMeshAdapter`，在 strict B3DM/GLB parser 后支持 JSON/binary `RTC_CENTER`、多 primitive、
  `BATCH_LENGTH > 1`、unsigned/integral-float `_BATCHID`、triangle 内 uniform feature 校验，以及 Batch Table
  JSON number/boolean/string scalar 与 Binary SCALAR/VEC2/VEC3/VEC4 numeric column 的 offset/alignment/range guards。
- canonical writer 对 geometry 实际引用的 old feature ID 做升序 dense remap，compact property row 后输出
  `_FEATURE_ID_0`、`EXT_mesh_features` 和 inline `EXT_structural_metadata` schema/property table；未引用行不会复制。
- Docker Release 构建和全量 CTest 为 148/148，legacy `v8` output SHA-256 golden 保持 byte-identical，
  OpenSpec 6.2、6.3 已勾选。

#### 2026-08-22：完成 OpenSpec 6.4-6.9

- `DracoDecoder` 已改为 profile-backed limit，支持 approved POSITION/NORMAL/TANGENT/TEXCOORD/COLOR/feature
  attribute 并保留 legacy stale-accessor compatibility；新增 `MeshoptDecoder` 支持 ATTRIBUTES、TRIANGLES、
  INDICES 与 NONE/OCTAHEDRAL/QUATERNION/EXPONENTIAL filter，在 decoder 前完成 range、stride、decoded length
  和 allocation 检查。第三方失败统一映射为 capability-compatible stable code，6.4、6.5 已完成。
- 新增 `TextureCodec` 与 `TextureMasker`：PNG、JPEG、WebP、单 2D Basis ETC1S/UASTC KTX2 先 probe header
  再按 V2 dimension/pixel/decoded-byte 上限解码到 RGBA；canonical writer 固定输出 deterministic PNG，并在
  encode 后 exact decode 校验。mask 支持 TEXCOORD_0/1、全部 core wrap mode 和多 material slot union，授权外
  RGBA 四通道全部归零，6.6 已完成。
- `MeshSceneClipper`、`AuthorizationScope` 与 `CanonicalMeshClipStrategy` 已实现 Polygon/MultiPolygon、holes、
  antimeridian、vertical/touching/tiny fragment、RTC/up-axis/nested transform/shared instance 的 exact clipping，
  并以 indexed-vs-exhaustive differential test 证明候选优化不改变结果。裁切后稳定重建所有 vertex stream、
  indices、bounds、PNG/material/image、feature/property mapping；empty 返回 typed evidence 而不写 GLB，
  canonical validator 在上传前检查 closure、bytes、bounds、引用、PNG 和 metadata，6.7、6.8 已完成。
- 新增真实 `MeshNormalizer`、`MeshNormalizationExecutor` 和独立 `run-normalizer` command。Executor 只选择
  manifest 中唯一 CONTENT root，按 attempt scratch 下载并核验 size/SHA，完成 decode/normalize/validate/
  staging upload/complete 后精确清理 attempt 目录；错误映射集中输出脱敏 stable code。默认 compose/runtime
  不启用新 command，legacy `run` 和 `v8` advertisement 保持不变。
- 新增 `STANDARD_4CPU_8GIB_SINGLE_TASK_V2`，身份 Hash 为
  `bb326727f6b29a6cdd3532d85e2043c3c0ff212056b837d3d4a2eca7df3d349c`；V1 文件身份保持
  `a65be19950a48f15c9a0275dda5a0b8b8e9e309feab84cbc10584dd70713c093`。CMake/runtime 同时安装 V1/V2，
  `validate_mesh_resource_profile.py` 验证 identity、Hash 和跨字段资源约束。
- corpus gate 已按 manifest 固定 revision 物化 6 个官方 Mesh entry，并由
  `mesh-phase-corpus-expectations.json` 固定 10 个 supported/unsupported outcome 与 geometry、texture、
  compression、transform、metadata、empty、determinism、resource-limit 八类 slice；native suite 提供对应
  generated/differential 行为验证，materialized validator 验证 payload、SOURCE identity 和 selector evidence，
  6.9 已完成。
- 最终 r5 offline supply-chain gate 使用当前 `vcpkg.json` SHA-256
  `5d53bd3a352919ec09997c8ec825fdc007f3761ab78f855cdfeaf87f970e4704` 构建 build/runtime base，随后执行
  `docker build --network none --pull=false --no-cache -f Dockerfile.offline`：Release 70 个编译步骤、171/171
  CTest、0 failure，legacy v8 byte goldens 与 normalizer schema fixtures 全部通过。最终镜像 UID 为 10001，
  安装 V1/V2 profile，不含 Git/CMake/Ninja，版本为 `0.1.9`；OpenSpec strict、resource profile/corpus validator、
  `git diff --check` 和 credential scan 同步通过。

## 23. Tasks 7.1-7.6 Point/Instance/Composite Worker 设计（已确认并实现）

> 本节与 API 主 Spec 25 一一对应。API 主 Spec 25.12 的七项决策已经用户确认，以下设计已按确认边界实现；
> 完成记录见 23.11。

### 23.1 兼容性、版本和运行时拆分

- 现有 `run` legacy v8 和 `run-normalizer` Mesh V1 行为、CLI/env、schema Hash、artifact identity 与 171 个
  CTest 全部保持。新增 Point/Instance/Composite executor 走 `THREE_D_TILES_NORMALIZER_V2`，不在现有
  `MeshNormalizationExecutor::buildInput` 中放宽“唯一 CONTENT root”规则。
- V2 claim/task 显式包含 `rootObjectId`，普通 Point/Instance 返回单 output，Composite 返回有序 multi-output。
  Runtime 按 task canonical family 分发到独立 executor；禁止通过 `switch` 把 PNTS/I3DM/CMPT 逻辑继续堆入
  `MeshNormalizer` 或 `B3dmClipper`。
- 新增 family identity：`normalization-point-v1/canonical-point-gltf2-v1`、
  `normalization-instance-v1/canonical-instance-gltf2-v1`、
  `normalization-composite-v1/canonical-composite-children-v1`。Mesh 保持现值。
- 新增 V3 profile loader/accountant；V2 `MeshResourceProfile` 继续只解释 V2 document。Point、Instance、Composite
  limits 使用 typed structs，所有 vector/buffer expansion、membership/classification tests、recursive outputs 在分配
  前 checked，代码中不散落 point/instance/child magic number。

### 23.2 模块与 typed data model

- 新增建议模块：
  - `formats/pnts/pnts_parser.*`、`formats/i3dm/i3dm_parser.*`、`formats/cmpt/cmpt_parser.*`；
  - `metadata/legacy_property_table.*`：从 B3DM adapter 抽取 PNTS/I3DM 共用的有界 JSON/binary property reader、
    dense remap 和 deterministic writer；
  - `point/point_scene.*`、`normalization/point_normalizer.*`、`point_canonical_writer.*`、
    `clip/point_scene_clipper.*`、`canonical_point_clip_strategy.*`；
  - `instance/instance_scene.*`、`normalization/instance_normalizer.*`、`instance_canonical_writer.*`、
    `clip/instance_bounds_classifier.*`、`canonical_instance_clip_strategy.*`；
  - `normalization/composite_normalizer.*`、`composite_manifest_writer.*`；
  - `normalization/normalization_v2_contract.*`、`normalization_v2_runtime.*` 和 family executors。
- `PointScene` 使用 structure-of-arrays：FLOAT positions、optional unit normals、RGBA8 colors、uint32 feature IDs、
  shared supported property table、RTC/root transform 和 derived bounds；所有 present stream count 必须完全一致。
- `InstanceScene` 保存一个已经确定性 flatten 的 shared `MeshScene` model、aligned translation/quaternion/scale、
  feature IDs、property table、RTC/root transform 和 model/instance/scene bounds。Quaternion canonicalization 固定符号，
  避免 `q` 与 `-q` 导致字节不确定。
- `CompositeChild` 保存 integer ordinal path、depth、magic/version、source slice offset/length/SHA、family、child
  identity 和 typed result；source slice 只在 attempt 内存/scratch 使用，不写入日志或 public manifest。

### 23.3 PNTS decoder 与 canonical Point writer

- Parser 严格处理 28-byte header 和 Feature/Batch Table。POSITION 优先于 POSITION_QUANTIZED；RGBA、RGB、
  RGB565、CONSTANT_RGBA 按规范优先；NORMAL 优先于 NORMAL_OCT16P；BATCH_ID 必须与 BATCH_LENGTH 一致。
- quantized position 在 double 中按官方公式解码，验证 finite/range 后写 canonical float；oct16 normal 解码、
  normalize 并检查长度。PNTS z-up -> canonical glTF y-up 使用一个集中定义的 exact basis matrix，与 Task 6
  `upAxisToZTransform` 互为逆，禁止再复制坐标常量。
- `PointCanonicalWriter` 输出一个或多个确定性 POINTS primitive，不写 indices/external URI/source padding；accessor/
  bufferView packing、component type、4-byte zero padding、JSON ordering 和 extension ordering 固定。feature/property
  输出复用 shared metadata writer。
- `PointCanonicalValidator` 检查 POINTS mode、no indices、POSITION bounds、normal unit、color range、aligned count、
  feature/property table、extensions、reachability 和 source-free closure，并生成 Point-specific semantic/version Hash。

### 23.4 Point clipping

- `AuthorizationTriangleIndex` 扩展 conservative point query；`AuthorizationScope` 暴露 projected point covers API，
  使用同一 triangulation、antimeridian unwrap 和 tolerance policy。point on boundary 为 retained，hole 内为 removed。
- `PointSceneClipper` 先计算 canonical local -> world ECEF，再投影/判定；retained ordinal vector 一次性 compact
  positions/normals/colors/feature IDs。feature property 二次 dense remap，bounds/statistics 全量重算。
- indexed 与 exhaustive covers 的逐点结果必须一致。EMPTY 不调用 writer；超限/取消/deadline/lease lost 在 upload
  prepare 前中止并精确清理 attempt scratch。

### 23.5 I3DM decoder、model flatten 与 canonical Instance writer

- Parser 严格处理 32-byte header、gltfFormat 和全部 Feature Table semantics。POSITION/quantized、explicit/oct
  orientation、SCALE/SCALE_NON_UNIFORM、RTC、BATCH_ID 使用官方 precedence/dependency；invalid pair、非正交、
  singular/non-finite scale 稳定拒绝。
- ENU-only orientation 首版返回 typed unsupported；显式 normal pair 存在时转换为 quaternion。external model 必须
  从 V2 rootObjectId 所属 I3DM URI 相对解析，所有 model buffer/image 均在 approved manifest。
- 复用 `GltfMeshReader` 后，把 default scene reachable nodes 的 transform bake 到共享 model typed mesh；normal/
  tangent 使用 inverse-transpose，material/texture/feature binding 保持。flatten 后不保留 skin/morph/animation 或
  unreachable resources。
- InstanceZ transform 通过 `Z_TO_Y * InstanceZ * Y_TO_Z` 共轭成 glTF basis，decompose 为 canonical
  translation/quaternion/positive scale；不能稳定 TRS decomposition 的 shear/reflection 返回 unsupported。Writer 使用
  `EXT_mesh_gpu_instancing`，feature 使用 `_FEATURE_ID_0 + EXT_instance_features + EXT_structural_metadata`。
- `InstanceCanonicalValidator` 除 common GLB checks 外验证 instancing node 必须有 mesh、所有 attribute count 相同、
  quaternion unit/canonical sign、scale finite/positive、model bounds 与逐实例 transformed bounds、feature/property
  mapping 和 extension required/used 规则。

### 23.6 Conservative instance classification 与 boundary expansion

- `InstanceBoundsClassifier` 以 model local AABB 8 corners 经完整 transform 后的 projected convex hull 为保守包络；
  whole/disjoint/boundary 使用授权 triangulation 的 covers/intersection，而不是 center/origin/AABB-only shortcut。
- whole instances 进入 compacted `InstanceScene`；disjoint 删除；boundary instances clone/transform shared model 到
  `MeshScene`，再调用现有 `MeshSceneClipper + TextureMasker + MeshCanonicalWriter/Validator`。
- boundary expansion 在实际 clone 前预留 vertices/indices/decoded bytes/texture pixels/metadata rows；一个 boundary
  instance unsupported 或超限时整个 source I3DM 失败，不返回 partial whole set。
- strategy 返回有序 `AuthorizedTypedOutputs`：optional INSTANCE output、optional MESH output；统一 feature mapping
  evidence记录 source ID 到各 output dense ID。Task 9 再负责把两个 output 变成 hierarchy contents。

### 23.7 Recursive CMPT normalization

- CMPT parser 使用 iterative stack 或受 V3 depth 限制的显式 recursion，逐 child 验证 magic/version/byteLength/
  alignment；支持 B3DM、PNTS、I3DM、GLB 2.0、nested CMPT。ordinal path 使用 `vector<uint32_t>` 并以 length-
  prefixed canonical bytes参与 child identity Hash。
- parser 还需从 child B3DM/I3DM/glTF 提取相对 external dependency，base 固定为 outer CMPT package path；缺失或
  越界 URI 在任何 child normalize 前 fail closed。
- `CompositeNormalizer` 按 depth-first ordinal order 调用 Mesh/Point/Instance normalizer。每个 leaf 产生独立 typed
  success/unsupported/failure evidence；parent manifest 只写 identity/family/hash/status/reason，不写 package path、
  object key、grant 或 source payload。
- V2 executor 对 child outputs 逐一 prepare/upload/report，最后上传 parent manifest；支持 child 即使 parent 因其他
  child unsupported 而 global-preview-only，也只能作为内部 immutable evidence，不得被 limited hierarchy引用。
  retry 按 exact child identity/Hash 复用，staging conflict 或 final object Hash不一致终止 parent。

### 23.8 Protocol、错误映射与 cleanup

- V2 contract 使用独立 schema/fixture/hash；V1 headers、serialization、positive/negative fixtures逐字节不变。
  multi-output declaration 按 ordinal path 排序，禁止 duplicate output ID/path，parent manifest Hash 覆盖全部 output
  identity和状态。
- 新增 typed errors：PNTS/I3DM/CMPT header/table/semantic、point compression unsupported、quantization、normal/
  orientation、ENU、scale、instance expansion limit、composite recursion/child/output conflict、Point/Instance output
  invalid。第三方/glTF reader原文映射为最长 512 字符脱敏摘要。
- 每个 attempt scratch child 仍由 validated opaque attempt ID 派生。Composite 内部 child file name只用 child ID；
  成功、unsupported、partial child upload、cancel、deadline、lease lost、exception均只清理 exact attempt staging，
  final immutable child由 API 生命周期管理。

### 23.9 Fixture、Corpus、构建与完成门槛

- unit/golden tests覆盖 API 主 Spec 25.9 的全部 positive/negative/clip cases，并增加 quaternion canonicalization、
  z-up/y-up conjugation、model flatten shared mesh/material、instance hull classifier indexed-vs-exhaustive 和 V2
  multi-output retry/crash tests。
- official corpus gate materialize `cesium-point-cloud`、`cesium-instanced`、`cesium-composite`，expectation 文件为
  每个 selector 固定 exact output family 或 stable unsupported code。generated fixtures 不替代 official payload
  materialization，official parser success也不替代 authorization tests。
- CMake 增加模块和测试，不新增未固定运行时下载。若无需新第三方库则保持当前 vcpkg manifest SHA；如实现 convex
  hull/geometry classification 需要依赖，优先复用现有 Boost/earcut/PROJ，不自行引入算法库，确需新增时先更新
  API 主 Spec 再确认。
- 完成门槛：legacy goldens byte-identical、普通和完全离线 Docker Release 全量 CTest、V1/V2/V3 profile/schema/
  corpus validator、API tests、deterministic repeat、credential/source-path scan、`git diff --check` 和 OpenSpec strict
  全通过。生产 V2 command/family flags默认关闭，Task 9 前不声明 limited-ready。

### 23.10 已确认决策

- 本 Worker Spec 接受 API 主 Spec 25.12 的七项决策作为统一确认；任一项调整时，两仓 Spec 同步修改后再编码。

### 23.11 实现结果与验收记录

- 已实现严格 PNTS/I3DM/CMPT parser、共享 legacy property table reader、typed `PointScene`/`InstanceScene`、
  canonical writer/validator、Point exact membership clip、Instance conservative bounds classifier 与 boundary Mesh
  expansion、recursive Composite normalizer，以及 Normalizer V2 contract/client/runtime/executor 和
  `run-broad-normalizer` 入口。现有 legacy `run`、Mesh V1 normalizer 和 B3DM v8 golden 路径未改语义。
- Point 支持 float/quantized position、RTC、RGB/RGBA/RGB565/constant color、float/oct normal、feature identity 和
  已确认 legacy property 子集；无 `BATCH_ID/BATCH_LENGTH` 的逐点属性使用 point ordinal。Point clip 对 Polygon/
  MultiPolygon 做 exact membership，边界保留，并同步 compact 所有 aligned stream、feature/property rows 与 bounds。
- Instance 支持 embedded/approved external glTF、float/quantized position、explicit/oct orientation、combined
  uniform/non-uniform scale、RTC 和 metadata；无 batch identity 的逐实例属性使用 instance ordinal。ENU-only 保持
  stable unsupported。embedded GLB 按其声明长度截取，并只接受至多 7 字节零对齐 padding，避免把 I3DM 尾部填充
  泄漏进 canonical model。
- Instance clipping 使用完整 transform 后 model bounds 的保守 hull 分类；whole/disjoint/boundary 分流，boundary
  只通过既有 validated Mesh path 展开和精确裁剪。输出固定为最多两个有序 family：INSTANCE 后 MESH；任一展开
  unsupported/超限使整个 source fail closed。
- Composite 支持 nested CMPT 的 depth-first ordered child identity、稳定 ordinal path、每 child typed evidence、
  多输出声明和 parent manifest。混合 supported/unsupported child 会保留 supported child evidence，但 parent 仍为
  preview-only；V2 runtime 对声明、prepare、report、complete 的 exact closure 和 family validator tool tuple 做校验。
- 已增加 generated positive/negative/clip/runtime fixtures，并物化固定 revision 的 Cesium 官方 point/instance/
  composite corpus。真实语料确认 Cesium 若干 external/RTC/scale/batch I3DM 仍为 ENU-only，因此 expectation 固定为
  `I3DM_ENU_UNSUPPORTED`；含这些 child 的 CMPT 固定为 `CMPT_CHILD_UNSUPPORTED`，explicit/oct/quantized-oct/
  transform variants 则执行真实 normalization。
- 最终使用已缓存 r5 build-base 在 `--network none` 下完成 Release 构建，CTest 201/201。额外定向官方/回归套件
  14/14；Task 6 corpus validator 为 6 entry/10 outcome，Task 7 为 3 entry/12 outcome，均要求 materialized=true。
  V1/V2/V3 resource profile validator 全通过；V2 schema Hash 为
  `a97fb3af54db27f4003f67b7b958b91c92054cd4b464cb3deec9d832b2c2f2d1`，V2/V3 profile Hash 分别为
  `bb326727f6b29a6cdd3532d85e2043c3c0ff212056b837d3d4a2eca7df3d349c` 和
  `d6097a29380375180e2cf9374eae354a9672e0589f3fb04b2f85930f16d5af78`。
- 所有 broad family/command feature flag 默认关闭；本节只交付可验证 normalization/clipping 与 durable evidence，
  不在 Task 9 前接入 limited hierarchy/gateway route。

## 24. Tasks 9.1-9.7 Clipper V2 与授权输出 Worker 设计（用户已确认）

> 本节与 API 主 Spec 27 一一对应。用户已于 2026-08-23 确认 27.15 的全部关键决策，两仓现按同一版本身份实施；
> 仅在对应验收通过后勾选 OpenSpec 9.x。

### 24.1 兼容性与运行时拆分

- legacy `run`、`THREE_D_TILES_CLIPPER_V1` 隐含 contract、B3DM v8 parser/clipper、CLI/env、API endpoint、golden bytes
  与已有 CTest 全部保持不变。Task 9 新增独立 `THREE_D_TILES_CLIPPER_V2` contract/client/runtime/command，不能在
  `task_contract` 或 `WorkerRuntime::process` 中按 optional 字段兼容两套语义。
- V2 input 永远是 Task 5-8 已验证的 canonical artifact，不再读取 source B3DM/PNTS/I3DM/CMPT 或解析 external source URI。
  Mesh/Point/Instance canonical GLB 必须闭合 embedded resource；Composite 由 API 按 parent manifest 拆成独立 leaf work item。
- V2 family strategy version 与 wire/profile/validator identity 分离：Mesh、Point、Instance、Composite leaf capability 以完整
  tuple 广告和匹配。Worker 不能只声明 `MESH_GLTF2` 就领取不同 canonical contract 或 metadata version。
- 新 runtime 建议使用 `run-authorized-clipper`，与 `run`、`run-normalizer`、`run-broad-normalizer` 独立 feature flag/loop；
  默认关闭。一个进程首版仍单 active task，复用既有 heartbeat/cancellation/scratch 清理模式。

### 24.2 Clipper V2 wire contract

- 新增 `clipper-protocol-v2.schema.json`、positive/negative fixture、API/Worker byte-identical SHA。claim request 包含 worker ID、
  protocol/schema identity、resource profile、max input/output、以及 exact family capabilities：canonical family/contract、
  clip strategy version、validator name/version/build SHA。
- claim response 绑定 work item/request/attempt/lease/deadline、tile content ID、source ordinal path integer array、source artifact
  kind/ID、input size/SHA/semantic/validation Hash、exact GET grant、scope WKB/SRID、16-element accumulated transform、canonical
  coordinate basis、family limits和最大 output count。不得包含 bucket credential、object listing grant、source URI 或 customer
  metadata。
- V2 outcome 固定为 `EMPTY`、`SAFE_WHOLE`、`CLIPPED`。`SAFE_WHOLE` 只返回 typed proof，不上传 output；`CLIPPED`
  先声明有序 outputs，再逐项 prepare/upload/report，最后 complete。Instance 最多 `INSTANCE_GLTF2` 后 `MESH_GLTF2`；普通
  Mesh/Point 最多一个；输出 ID/sort order/ordinal 不得由 object filename 推断。
- complete/proof 至少绑定 input SHA/semantic Hash、scope Hash、transform Hash、family/strategy version、geometry count、
  classifier summary、proof Hash；API 会重新校验这些字段，Worker 不返回自由 JSON evidence。
- failure 只返回 API enum 闭合的 stable code、bounded safe message 和 retryable；signed URL、WKT/WKB、object key、schema/property
  value 和第三方 parser 原文不得出现在 callback 或日志。

### 24.3 Canonical input readers

- Mesh V2 reader 复用 `GltfMeshReader` 的 canonical allowlist，但新增 `CanonicalMeshReader` wrapper：要求 exact canonical family/
  contract、GLB only、embedded resources、validator-compatible extension/metadata、input SHA/semantic evidence，并把 validation
  summary 与实际 scene counts/bounds 交叉验证。
- 新增 `CanonicalPointReader`，解析 Task 7/8 writer 生成的 POINTS primitive、POSITION/NORMAL/COLOR、feature sets、property
  table、root transform 和 bounds；禁止 indices、external URI、source extension 或非 canonical component type。
- 新增 `CanonicalInstanceReader`，解析 `EXT_mesh_gpu_instancing` shared model、TRS、feature IDs、property table、root transform
  和 model/instance bounds；要求 Task 7/8 canonical quaternion/scale/extension contract，不接受 arbitrary source glTF instancing。
- reader 必须重新计算 input SHA、semantic/validation evidence 所需的安全摘要，不能只信 API 传来的 family 字符串。输入 bytes、
  embedded image/property values 仅存在于 task memory/scratch，不写普通日志。
- Composite parent manifest 不进入 Worker；API 为每个 accepted child artifact创建 work item。`sourceOrdinalPath` 参与日志安全
  计数和 proof Hash，但日志只记录 depth/element count 或 opaque prefix，不记录可还原 package path 的 source 信息。

### 24.4 Exact relation 与 SAFE_WHOLE

- 新增 enum `AuthorizationContentRelation { EMPTY, SAFE_WHOLE, BOUNDARY }` 和 family-specific classifier；relation 是优化后的
  typed result，不使用 AABB/scene bounds/tile bounds 直接宣告 SAFE_WHOLE。
- Mesh classifier 对每个实际 triangle 使用与 `MeshSceneClipper` 相同的 complete transform、projection、triangulated scope、
  hole/antimeridian/tolerance contract。只有每个 triangle 被完整覆盖、texture/metadata closure 无需重建时 SAFE_WHOLE；
  vertex inside、triangle AABB 或 clipped triangle count 不变都不是单独充分证据。
- Point classifier 对每个 transformed point 调用 exact covers；全部 retained 为 SAFE_WHOLE，零 retained 为 EMPTY，其余 BOUNDARY。
  indexed 与 exhaustive 结果在 randomized/hole/MultiPolygon/antimeridian fixture 上逐点一致。
- Instance 复用 Task 7 model-derived transformed hull classifier。所有实例 whole 才 SAFE_WHOLE；全 disjoint 为 EMPTY；其余进入
  `CanonicalInstanceClipStrategy`，boundary expansion 任一 unsupported/limit failure 使整个 work item fail closed。
- proof Hash 使用 domain separator，绑定 input SHA/semantic Hash、scope Hash、transform canonical bytes、family contract、strategy
  version、actual geometry/point/instance counts 和 classifier result。重复执行必须相同，改变任一输入/version 必须改变 proof。

### 24.5 CLIPPED outputs 与 metadata/texture 语义

- Mesh BOUNDARY 调用现有 `MeshSceneClipper + TextureMasker + Metadata compactor + MeshCanonicalWriter/Validator`；输出 GLB
  不再包回 B3DM。EMPTY 不调用 writer，SAFE_WHOLE 不重新编码 canonical bytes。
- 新增 `CanonicalPointClipStrategy` wrapper：调用 `PointSceneClipper` 后写 `PointCanonicalWriter`、重新验证 family/metadata/
  bounds/semantic Hash；不能只返回 typed scene 给单元测试而没有可发布 bytes/evidence。
- Instance 使用现有 dual-output strategy，但补齐 Clipper V2 publication declaration、每 output独立 validator/evidence、共同
  source feature mapping proof。任一 output report/publish 失败时不接受另一 output 的 partial success。
- Task 8 metadata path必须接入 V2 reader/clip/writer：retained feature mapping、legacy hierarchy/property table、null/default/
  string/array/BOOLEAN 等保持 compact；property/feature texture、property attribute、unknown relationship/statistics 等稳定
  unsupported。SAFE_WHOLE 也必须验证 canonical metadata closure safe，不能跳过 Task 8 allowlist。
- texture masking 仅在 Mesh boundary fragment 发生；SAFE_WHOLE canonical PNG/embedded image bytes原样引用，Point 无材质纹理，
  Instance whole output保留 canonical shared model，boundary expanded Mesh沿 exact texture mask path。

### 24.6 Resource profile、scratch 与发布

- Clipper V2 首版复用 `STANDARD_4CPU_8GIB_SINGLE_TASK_V4` 的 geometry/texture/metadata limits，并增加 authorization output
  declarations、proof operations、multi-output bytes、source ordinal path 和 work-item deadline 上限；若需要改变 V4 文件 bytes，
  必须新增 V5 profile，不能原地修改 Task 8 已固定 Hash。
- 输入下载和 output staging 使用 task-scoped scratch/URL；maximum input/output/aggregate multi-output/decoded bytes 在分配前 checked。
  output prepare 之前完成 relation classification和预核算；不能上传一部分后才发现第二 output 超限。
- CLIPPED 每个 output本地完成 canonical reparse/validation、SHA/semantic/validation manifest 后再请求 PUT；上传后 report exact
  ETag/size/SHA。SAFE_WHOLE/EMPTY 不请求 PUT grant，complete 中携带 output 列表必须为空。
- lease lost、deadline、cancel、network failure、partial upload、complete conflict 全部只清理 exact attempt scratch；API 负责
  staging/final object lifecycle。Worker 不删除 normalization artifact、其他 attempt output 或 hierarchy artifact。

### 24.7 日志与稳定错误映射

- 新增阶段事件：V2 claim、canonical download/verify、relation classify、clip、output validate、output prepare/upload/report、
  complete。字段只包含 worker/work-item opaque ID、family/version、counts/bytes/duration、outcome、stable error；不记录 grants、
  input/output URL、scope、source ordinal path全文或 metadata值。
- `FormatErrorCode` 与 API `ThreeDTileCapabilityReasonCodeEnum`/Clipper V2 error enum 一一闭合。新增至少覆盖 canonical input
  mismatch、SAFE_WHOLE proof invalid、Point output invalid、multi-output declaration conflict、output validation/publish conflict、
  metadata reconstruction unsafe 和 resource profile mismatch。
- retryable 只用于 transport/control-plane/lease类暂态错误；unsupported format/metadata/resource-contract、proof mismatch、
  deterministic collision 均 terminal，不通过自由 `UNSUPPORTED_` 前缀猜测状态。

### 24.8 Fixture、official corpus 与完成门槛

- generated tests 覆盖 Mesh/Point/Instance 的 EMPTY/SAFE_WHOLE/BOUNDARY，holes、MultiPolygon、antimeridian、boundary touching、
  transform、metadata、texture、dual output、proof Hash version drift、resource limit、partial upload、lease loss 和 repeat bytes。
- bounding-only adversarial fixture 必须构造“tile/content/scene bounds被 scope covers 但实际 transform/geometry越界”或“vertices inside
  但 triangle 穿越 concave/hole boundary”，证明 classifier 不会 SAFE_WHOLE。
- official Task 9 runner 使用现有 pinned Cesium/Khronos payload实际执行 canonical reader和production clip strategy；Mesh/Point/
  Instance/Composite supported/unsupported outcome、proof、determinism、metadata lookup 和 source canary exclusion 必须由运行结果生成，
  不静态复制 expectation。
- API hierarchy/subtree generation在 Java 仓实现，Worker corpus仍需输出可供其消费的 exact outcomes/output ordering/evidence。
  两仓 end-to-end fixture会把 Worker结果灌入 planner/hierarchy writer，验证 multiple contents和implicit availability。
- 完成门槛：legacy v8 goldens byte-identical、V1 worker tests不变、Clipper V2 schema/fixtures Hash一致、native + Docker/offline
  Release CTest、V1-V4/V5 profile validator、official materialized corpus、credential/source URI/property canary scan、API/forward
  tests、OpenSpec strict和两仓 `git diff --check` 全通过。

### 24.9 预计修改范围

- `include/src/client`：Clipper V2 API client、exact output prepare/report callbacks；
- `include/src/task` 或新 `authorization` runtime：V2 typed contract、lease/runtime/executor，保持 legacy runtime不变；
- `include/src/normalization`：canonical Mesh/Point/Instance readers和validation registry复用；
- `include/src/clip`：exact relation classifier、`CanonicalPointClipStrategy`、Mesh SAFE_WHOLE proof、Instance V2 output adapter；
- `tests`/`config`：schema/profile、generated fixtures、official expectations/runner、CMake/Docker/offline bundle与测试。
- Worker 不生成 Tileset JSON/subtree、不读取数据库、不持久化 route；这些职责在 API 主 Spec 27 的 metadata/forward模块。

### 24.10 已确认的关键决策

- 本 Worker Spec 接受 API 主 Spec 27.15 的九项决策作为统一确认，尤其包括：V2 与 legacy v8 并存、canonical-only
  SAFE_WHOLE、Composite leaf work item、Instance multi-output、embedded canonical dependencies、默认关闭和不修改 `spacecloud`。
- 若用户调整 Clipper contract、source whole-content policy、implicit output slot或版本策略，两仓 Spec 必须同步更新后再编码。

### 24.11 Task 9.1 API planning 基础完成（2026-08-23）

- API 仓已完成 normalized planning mode、完整 job/preparation tuple、normalization manifest、content ordinal/Composite ordinal path、
  work-item/decision/output/hierarchy artifact 持久化和默认关闭 flags；legacy Worker V1/v8 没有修改。
- 所有 geometry work item 当前仅为 `EXACT_CLASSIFICATION_REQUIRED + PENDING`，没有伪造 EMPTY/SAFE_WHOLE/CLIPPED outcome；
  Worker 在下一分片通过独立 `THREE_D_TILES_CLIPPER_V2` 领取并补齐精确证据。
- PostgreSQL 15 partial unique/JSON ordinal/并发幂等与 Java 8/JUnit 验证通过。Task 9.1 已勾选；本 Worker 仓下一步实现
  24.1-24.7 的 V2 contract/runtime/canonical reader/SAFE_WHOLE proof，legacy `run` 保持字节和协议兼容。

### 24.12 Task 9.3 Clipper V2 与 SAFE_WHOLE 核心实施结果（2026-08-24）

- 新增独立 `authorization/v2` contract、API client、lease runtime 和 executor，以及生产命令
  `run-authorized-clipper`。Mesh/Point/Instance family flags、控制面 URL、authorization header 和 Worker ID 与 legacy `run`
  分离且默认全部关闭；Docker 默认 CMD 仍为 legacy `run`。命令只广告完整 protocol/schema/profile/canonical contract/
  clip strategy/validator tuple，未启用 family 不会领取任务。
- executor 按 download -> input SHA/size verify -> canonical reader re-open -> exact classify -> optional clip -> canonical validate ->
  prepare/upload/report -> complete 执行。Mesh/Point 各最多 output ordinal 0；Instance whole ordinal 0、boundary Mesh ordinal 1。
  EMPTY/SAFE_WHOLE 不申请 PUT grant，CLIPPED 只有全部 output report 完成后才能 complete；租约冲突会停止后续 I/O。
- `CanonicalMeshReader`、`CanonicalPointReader`、`CanonicalInstanceReader` 会重新解析 GLB、核对 family/contract、validator identity、
  SHA/size、semantic Hash 和 validation manifest。Mesh classifier 逐实际 triangle exact covers，Point 逐 transformed point，Instance
  使用 model-derived transformed hull；hole/concave/bounding-only 情况不能变成 SAFE_WHOLE。proof Hash 绑定输入、scope、transform、
  strategy、source ordinal path 与 actual element counts。
- Mesh boundary 复用 canonical Mesh clip/texture/metadata path，Point boundary 写回 validated canonical Point GLB，Instance boundary
  复用 dual-output whole+expanded Mesh strategy。输出 evidence 使用 V3 metadata-safe semantic Hash；progress 使用 saturating add，
  不因 input/output byte count 加法溢出破坏单调心跳验证。失败 callback 只发送 stable enum、bounded safe message 和 retryable。
- CMake/Docker 已纳入 V2 runtime/executor/client 和聚焦测试。测试覆盖 schema digest、output/manifest/proof closure、Instance slot、
  report-before-complete、409 lease loss、三 family whole/empty/boundary、Mesh hole bounding-only negative、三 family canonical reopen 和
  semantic drift fail closed。Linux amd64 Docker Release 全量构建与 CTest 236/236 通过，legacy v8 byte golden 仍通过；运行变量和
  Instance 依赖 Mesh validator 的规则已写入 README。
- API 侧 exact proof/canonical-only persistence 已完成，但 OpenSpec 9.3 仍按主 Spec 27.14 等待 Task 9.5 的 direct source deny
  gateway tests 后统一勾选。Worker 下一步无需生成 Tileset JSON；Task 9.2 explicit hierarchy 在 API 仓从 typed evidence 重建。

## 25. Task 11.x Worker rollout 与发布验收实施结果（2026-08-25）

### 25.1 Decoder capability contract

- Normalizer V1/V2/V3 claim request 新增有序 `decoderCapabilities`，claim response 新增有序 `requiredDecoders`；允许值闭合为
  `DRACO/MESHOPT/PNG/JPEG/WEBP/KTX2`，重复、乱序、未知值或任务需求不是 Worker capability 子集均 fail closed。
- 三版 schema 与 API byte-identical，SHA 分别为 `f9adef70706c010979e0bc65306cee9692af338941e13a0a554fe6879d6319c6`、
  `4926bba1c29b47eb148fe3abdfafd22c67adabbb645e102321d6affb8c8219a6`、
  `bc8a587c664b0d12df53ae5ef5568873a932951d2738b04ef242b702dac92f17`；positive/negative fixtures 同步更新。
- 新增六个默认关闭环境变量 `CLIP_WORKER_NORMALIZER_*_ENABLED`。进程启动只广告显式开启的 decoder，并在结构化启动事件中记录
  安全 capability 数组，不记录 URL/header/object identity。

### 25.2 验证结果

- 首次完整 CTest 暴露旧 crash/retry 测试 JSON 缺 `requiredDecoders`，补齐 fixture 后重新执行通过。
- 最终使用 `Dockerfile.offline`、固定 r5 build/runtime base、`--network none --pull=false --no-cache` 构建；`vcpkg.json` SHA
  `5d53bd3a352919ec09997c8ec825fdc007f3761ab78f855cdfeaf87f970e4704` 与基础镜像 label/内部 sentinel 一致。
- Release 130 个编译步骤完成，CTest 237/237，legacy v8 byte golden、Normalizer V1/V2/V3、Clipper V2、metadata privacy official corpus
  全通过；最终运行镜像 UID 10001，版本 0.1.9。
- Worker 侧 11.2/11.4/11.6 门槛完成。Task 11.5 的唯一剩余项是客户端 Mapbox tokened browser gate，与 Worker 无关，不能用 native
  suite 替代。
