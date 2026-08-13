# 2026-08-07 3D Tiles Clip Worker 工程与基础格式解析

## 任务来源

本工程实现 `D:\code\api-platform\.spec\20260706-service-auth-policy-ip-temporal-scope-audit.md`
第 30 节确认的 3D Tiles 边界 Mesh 真实裁切方案。

## 已确认约束

1. 返回字节中不得包含授权范围外几何、纹理像素或 Feature/Batch 属性。
2. 所有与边界相交且带 Mesh content 的 LOD 层级都必须生成裁切资产。
3. 非 READY、失败或不支持时由网关删除边界 content，不得透传原始 Tile。
4. 二进制成果持久化到私有 MinIO，Redis 只缓存小型状态映射。
5. 正式目标为 Linux x86_64/amd64 和 Linux ARM64/aarch64 Docker；Windows 仅用于开发构建，不支持 ARMv7/32 位。
6. 使用 C++17、CMake、vcpkg manifest 和 GoogleTest。
7. 线上样本不提交本工程；测试只使用最小化合成数据。

## 本阶段结构

- CMake 和 vcpkg 工程骨架；
- 显式区分 `x64-linux`、`arm64-linux` triplet 的多平台 Docker 构建；
- B3DM v1 包头、Table 范围、GLB 对齐和 `BATCH_LENGTH` 校验；
- GLB 2.0 包头、Chunk 顺序、Chunk 对齐、总长度和 JSON 校验；
- Worker claim、heartbeat、complete、fail 内部 REST 数据契约和客户端；
- `complete` 结果显式区分 `READY` 和 `EMPTY`：只有 `READY` 携带并强制校验输出对象摘要，
  `EMPTY` 不创建零字节占位对象；
- 内存生成的合成 B3DM/GLB fixture 与异常输入单元测试；
- `inspect` 命令输出不包含二进制和签名 URL 的结构摘要。

## 安全行为

解析器只接受明确支持的 B3DM v1 和 GLB 2.0。未知版本、未知或重复 GLB Chunk、长度越界、
对齐错误、非法 JSON、缺失 `BATCH_LENGTH` 均抛出结构化错误，后续 Worker 将其映射为
`UNSUPPORTED` 或 `FAILED`，不得上传或发布结果。

REST 客户端不在异常中输出响应正文，避免服务端错误正文意外携带预签名 URL。授权 Header
由部署配置传入，不固化密钥或 Token。

## 未完成范围

本阶段不声称已经实现 Mesh 裁切。后续仍需实现：

1. 下载/上传和 SHA-256、ETag 校验；
2. glTF Mesh/Accessor/Buffer 重建；
3. EPSG:4490、ECEF 和局部 ENU 转换；
4. Polygon/MultiPolygon/Hole 棱柱裁切与属性插值；
5. WebP UV Mask、RGBA 清零和 lossless 重编码；
6. Worker 租约心跳、重试和常驻循环；
7. Java 控制面、动态 Tileset 改写和裁切资产二次鉴权。

## 验证要求

- `cmake --build` 无警告升级为错误前的基础编译错误；
- `ctest` 覆盖正常与异常 B3DM/GLB 结构；
- Docker 分别完成 `linux/amd64`、`linux/arm64` 构建，产物及依赖架构正确；
- 发布镜像 manifest 同时包含 `linux/amd64` 和 `linux/arm64`，不包含 ARMv7/32 位变体；
- 不提交线上样本或任何 MinIO 凭据、预签名 URL。

## 2026-08-07 运行平台范围澄清（待编码确认）

一期正式运行平台同时包括 Linux x86_64/amd64 和 Linux ARM64/aarch64。Windows 仍只作为
开发环境；明确排除的是 ARMv7 和其他 32 位 ARM，而不是 Linux amd64。

确认后按以下范围维护工程骨架：

1. 保留 `x64-linux`、`arm64-linux` CMake preset 和对应 vcpkg triplet。
2. `docker-bake.hcl` 和 Docker/buildx 同时构建 `linux/amd64`、`linux/arm64`。
3. Dockerfile 只接受 amd64、arm64 两种生产目标架构，对 ARMv7/32 位 ARM 显式失败。
4. README、验证命令和后续 CI 同时覆盖 Linux amd64、Linux arm64。
5. 分别在两种架构镜像内运行 `ctest`，并核验可执行文件与依赖架构正确。

## 2026-08-07 继续实施记录

用户已确认上述平台范围并要求继续编码。本轮在既有裁切方案内完成以下闭环，不扩大一期格式范围：

1. 新增 `run` 常驻命令，从环境变量读取控制面地址、Worker 标识、算法版本、轮询/心跳/传输超时和输入大小上限，并响应 `SIGINT`/`SIGTERM` 安全退出。
2. 控制面客户端和预签名对象传输共用显式的 libcurl 全局运行时，消除依赖对象构造顺序的隐式初始化。
3. B3DM 重建只输出活动 Scene、仍被引用的 Node/Mesh/Material/Texture/Image/Sampler；不得复制原 Scene 的 `name`、`extras` 或非活动 Scene。
4. glTF 顶层、Material 和 Texture 扩展均按白名单校验。未知 `extensionsUsed`、`extensionsRequired` 或扩展载荷返回 `UNSUPPORTED`，不得进入 READY。
5. WebP Mask 编码后再次解码，校验尺寸，并保证 Mask 外每个像素的 RGBA 四通道均为零。
6. 使用内存构造的最小 Mesh + WebP B3DM fixture 增加裁切端到端测试，覆盖输出二次解析、几何范围、纹理清零和未知扩展 fail closed。
7. Docker 运行镜像显式携带 vcpkg 动态库和 PROJ 数据目录；`linux/amd64`、`linux/arm64` 构建阶段都执行完整 `ctest`，运行镜像默认执行 `run`。

以上修正均服务于已确认的“返回字节不含范围外几何、纹理像素和元数据”以及双架构验收条件，不新增宽松透传路径。

## 2026-08-07 双架构构建与合成数据验收记录

本轮完成 Worker 的 Linux amd64 与 Linux arm64 实际构建、容器内测试和运行镜像检查，结果如下：

1. vcpkg 固定版本从 `2025.06.13` 升级到 `2025.07.25`。后者使用 OpenSSL 3.5.1，移除了旧
   port 对 GitHub commit patch 的临时下载依赖；不得通过关闭 SHA-512 校验规避下载失败。
2. vcpkg 下载器增加连接和单次请求总超时；GitHub 资产代理和 SQLite 镜像仅通过 build arg
   显式启用，默认生产 URL 仍为官方源。用于本次构建的 SQLite 镜像文件 SHA-512 与 vcpkg
   port 固定值一致。
3. Docker 依赖层只复制 `vcpkg.json`，下载目录和二进制包目录使用 BuildKit cache；项目
   `CMakeLists.txt` 与源码位于依赖层之后。CMake 和预安装步骤统一使用
   `/src/vcpkg_installed`，PROJ 数据也从同一路径复制，避免 manifest mode 重复安装和错误路径。
4. WebP 改用 `WebP::webp`、`WebP::sharpyuv` CMake targets，解决静态链接缺少 SharpYUV 的问题。
5. Mask 重编码改用 libwebp 高级 lossless 配置并设置 `exact=1`。便捷编码 API 会改写全透明
   像素的 RGB，无法满足 Mask 外 RGBA 四通道全部为零；输出仍必须通过二次解码逐像素校验。
6. `linux/amd64` 在容器内完成编译和 22 项 CTest，22/22 通过；运行镜像报告
   `amd64/linux`，容器内为 `x86_64`，版本为 `0.1.0`，PROJ `proj.db` 可读。
7. `linux/arm64` 在 aarch64 目标容器内完成全部第三方依赖、Worker 编译和 22 项 CTest，
   22/22 通过；运行镜像报告 `arm64/linux`，容器内为 `aarch64`，动态依赖路径为
   `/lib/aarch64-linux-gnu`，版本为 `0.1.0`，PROJ `proj.db` 可读。
8. 两种运行镜像的默认 `run` 命令均能以非 root 用户常驻；使用不可达测试地址时只记录脱敏
   连接错误，并能响应 `SIGTERM` 后退出。Smoke 容器均已清理。
9. `docker buildx bake --print` 只声明 `linux/amd64`、`linux/arm64`，不包含 ARMv7/32 位变体。

本轮未完成且不得宣称完成的验收项：

1. 未提供目标 registry、仓库标签和凭据，因此未执行远程 push，也未检查远程多架构 manifest。
2. 未连接真实控制面、私有 MinIO 和用户提供的 SceneGIS 数据完成 claim/download/clip/upload/
   complete 全链路；线上样本仍未写入或提交仓库。
3. 真实样本根 Tileset 声明 `gltfUpAxis=Z`，但当前 Java 索引和 Worker 任务契约不携带轴向。
   是否一期只接受 `Z` 并在索引阶段对其他轴向 fail closed 仍待用户确认，本轮不擅自修改。
4. Cesium/SceneGIS 多 LOD 视觉检查、原始 MinIO 匿名访问关闭验证、并发压测和故障恢复演练仍待联调。

## 2026-08-08 glTF Up Axis 一期规则确认

用户已确认一期 Worker 仅支持显式 `asset.gltfUpAxis=Z` 的 Tileset：

1. Java 索引必须对根和每个外部嵌套 Tileset JSON 校验区分大小写的 `Z`，缺失或其他值
   fail closed，并将已验证轴向记录到新的 index generation。
2. claim 任务契约新增 `gltfUpAxis`，控制面只允许从轴向为 `Z` 的 generation 发放任务。
3. C++ 任务解析器必须要求 `gltfUpAxis` 为字符串 `Z`；缺失、`X`、`Y`、空值或其他类型
   均拒绝，且拒绝发生在对象下载、裁切和上传之前。
4. Worker 不实现 X/Y 到 Z 的隐式转换，也不把缺失字段解释为 3D Tiles 的默认 Y-up。
5. 单元测试覆盖 `Z` 成功和非 `Z`/缺失拒绝；真实 SceneGIS 样本联调继续使用其显式 `Z`。

## 2026-08-08 glTF Up Axis 实施结果

1. `ClaimTask` 新增 `GltfUpAxis` 枚举字段，任务解析要求 `gltfUpAxis` 为显式字符串 `Z`。
2. 缺失、`X`、`Y`、小写、空值和非字符串均在 `WorkerApiClient::claim` 解析响应时拒绝，
   早于 heartbeat、对象下载、裁切和上传。
3. `linux/amd64` 目标容器内重新编译并执行 23 项 CTest，23/23 通过。
4. `linux/arm64` 目标容器内重新编译并执行 23 项 CTest，23/23 通过；受限网络构建沿用
   已验收的显式基础镜像、vcpkg/GitHub 资产和 SQLite 镜像参数，固定 SHA 校验保持启用。
5. 本地测试镜像更新为 `3d-tiles-clip-worker:amd64-test` 和
   `3d-tiles-clip-worker:arm64-test`；未提供 registry，因此仍未执行远程 push。

## 2026-08-08 畸形 claim 响应隔离

最终运行时复核发现，任务契约解析会对缺失或非法 `gltfUpAxis` 抛出
`std::invalid_argument`，而常驻循环原先只捕获 `WorkerApiError`，会导致 Worker 进程退出。
按以下规则修正：

1. `WorkerRuntime::run` 捕获任务契约的 `std::invalid_argument`，放弃当前响应并继续轮询。
2. 拒绝仍发生在 heartbeat、对象下载、裁切和上传之前，不放宽严格 `Z-only` 行为。
3. 日志使用固定脱敏文本，不输出异常 `what()`、响应正文、租约令牌或预签名 URL。
4. Worker 不对无法安全解析的任务调用 `fail`；该租约由控制面现有超时恢复机制处理。
5. 生产 C++ 代码变更后重新构建 Linux amd64 和 Linux arm64 镜像，并要求两端继续通过
   23/23 CTest、架构和版本检查，且不得遗留验收容器。

实施与验收结果：

1. `WorkerRuntime::run` 已捕获 `std::invalid_argument`，仅输出固定的
   `Worker received an invalid claim task response`，随后按现有轮询间隔继续运行。
2. `3d-tiles-clip-worker:amd64-test` 重新构建成功，容器内 23/23 CTest 通过；镜像为
   `linux/amd64`，机器架构为 `x86_64`，版本为 `0.1.0`。
3. `3d-tiles-clip-worker:arm64-test` 重新构建成功，容器内 23/23 CTest 通过；镜像为
   `linux/arm64`，机器架构为 `aarch64`，版本为 `0.1.0`。
4. 固定 vcpkg 版本和下载摘要校验保持启用；本轮构建继续使用此前验收的显式镜像代理参数。
5. 架构与版本检查使用 `--rm` 临时容器，完成后未遗留任何 Docker 容器。

## 2026-08-10 Docker Compose 部署方案（已确认）

用户确认使用 Docker Compose 部署 Worker。现有 Dockerfile 已通过 Linux amd64、Linux arm64
双架构构建和 23/23 CTest，不新建重复 Dockerfile；本轮按以下范围增加部署入口：

1. 新增 `compose.yaml`，仅编排 Worker；同时支持 Registry 镜像和本机原生架构源码构建。
2. 不固定 Compose `platform`，由 Docker 按宿主架构选择 amd64/arm64 manifest；多架构发布
   仍由 buildx 完成。
3. 控制面 URL 必填；Worker ID 默认使用 Docker 唯一 HOSTNAME，以支持 Compose 横向扩容。
4. 不映射端口、不挂载 MinIO 密钥或数据目录；启用非 root、只读根文件系统、删除 capabilities、
   `no-new-privileges`、日志轮转、自动重启和可配置停止宽限期。
5. 新增不含密钥的 `.env.example`，真实 `.env` 加入忽略规则；README 记录配置校验、本机构建、
   Registry 拉取、启动、日志、扩容、停止和清理命令。
6. 验收覆盖缺失必填变量时配置失败、`docker compose config`、当前架构重新构建与 23/23 CTest、
   非 root/只读/无端口运行检查、SIGTERM 停止和容器清理。

实施与验收结果：

1. 保留原有 `Dockerfile`，新增 `compose.yaml` 和 `.env.example`；`.gitignore` 已忽略真实
   `.env`，README 已补充配置校验、本机构建、Registry 拉取、启动、日志、扩容、停止和清理命令。
2. 未设置 `CLIP_WORKER_CONTROL_PLANE_URL` 时，`docker compose config` 按预期以必填变量错误
   失败；使用不含凭据的 `.env.example` 时配置展开成功，且未创建真实 `.env`。
3. 使用 Compose 在当前 Linux amd64 目标执行无缓存构建，Dockerfile 内 23/23 CTest 全部通过，
   生成 `3d-tiles-clip-worker:0.1.0`；镜像元数据为 `linux/amd64`，容器内架构为 `x86_64`，
   Worker `--version` 返回 `0.1.0`。
4. `docker compose up -d --no-build` 后，控制面主机不可解析时 Worker 保持常驻轮询，日志只包含
   固定连接错误，不包含授权 Header、响应正文或预签名 URL。
5. 运行时元数据和容器内检查确认：UID 为 10001、根文件系统只读、`CapDrop=[ALL]`、
   `no-new-privileges=true`、无 `ExposedPorts` 和端口绑定、`init=true`、重启策略为
   `unless-stopped`，停止宽限期为 600 秒。
6. 临时扩容到两个副本后，两者 `CLIP_WORKER_ID` 均为空且 Docker `HOSTNAME` 不同，验证了
   Worker ID 回退和 Compose 横向扩容契约；两个副本均未开放端口。
7. `docker compose stop` 在约 9 秒内使两个副本均以退出码 0 停止；随后执行
   `docker compose down`，最终 `docker compose ps --all` 无容器，Compose 网络也已删除。
8. 本轮未重复构建 arm64，也未推送 Registry；Linux arm64 Dockerfile 路径沿用此前 23/23 CTest
   验收结果。生产发布仍须在取得 registry 地址、tag 和凭据后推送并检查远程 manifest 同时包含
   `linux/amd64`、`linux/arm64`，且不包含 ARMv7/32 位变体。

## 2026-08-10 双基础镜像离线构建方案（已实施，amd64 已验收）

### 目标与已确认边界

用户要求把需要联网取得且变更频率低的依赖环境，与频繁变化的 Worker 业务源码分离。联网环境先为
Linux amd64 和 Linux arm64 分别原生构建、导出基础镜像包；离线环境导入与宿主架构匹配的基础镜像
后，在 Docker 内复制当前 C++ 源码、重新编译、执行完整 23 项 CTest，并生成最终运行镜像。

已确认约束：

1. amd64 基础包只在 amd64 Linux 主机原生生成和使用，arm64 基础包只在 arm64 Linux 主机原生
   生成和使用；不在一期引入 QEMU/binfmt，也不支持 ARMv7/32 位。
2. 离线构建不是导入已经完成的 Worker 最终镜像；业务源码可以变化，最终镜像构建必须重新编译并
   执行测试。
3. `vcpkg.json`、Ubuntu、vcpkg、编译器或第三方依赖版本变化时必须在联网环境重建基础包；只修改
   `include/`、`src/`、`tests/`、CMake 工程文件时，不重建基础包。
4. 离线最终构建必须使用 `--network=none` 和 `--pull=false`，不得依赖 BuildKit 历史缓存来伪装
   离线成功；验收时还要在宿主网络不可用或被显式隔离的条件下执行无缓存构建。

### 镜像分层

新增两个相互独立、按依赖版本和架构标记的基础镜像：

1. `3d-tiles-clip-worker-build-base:<deps-version>-<arch>`：包含固定 Ubuntu 工具链、CMake、Ninja、
   vcpkg `2025.07.25`、对应架构 vcpkg tool、`vcpkg.json` 的完整开发依赖、头文件、库和 CMake
   package metadata。该镜像只参与构建阶段，不进入最终运行阶段。
2. `3d-tiles-clip-worker-runtime-base:<deps-version>-<arch>`：只包含 CA 证书、Worker 运行需要的
   动态库、PROJ 数据和 UID 10001 非 root 用户，不包含编译器、vcpkg 源码、头文件或测试程序。
3. `Dockerfile.offline` 使用上述两个基础镜像进行多阶段构建。build 阶段只复制当前 Worker 源码，
   使用预安装 vcpkg 目录且关闭 manifest 自动安装，完成 Release 编译、23 项 CTest 和安装；runtime
   阶段以 runtime-base 为起点，只复制本轮编译出的 Worker 可执行文件及确有需要的应用产物。
4. 最终镜像保持现有 `LD_LIBRARY_PATH`、`PROJ_DATA`、UID 10001、`ENTRYPOINT` 和默认 `run`
   契约；Compose 的只读根、capabilities、无新权限和无端口策略不变。

### 依赖一致性与失败策略

1. 基础镜像保存构建时使用的 `vcpkg.json` 副本和 SHA-256。离线构建开始时必须比对项目当前
   `vcpkg.json`；不一致时明确失败并要求联网重建基础包，不能尝试下载或继续使用旧依赖。
2. 基础包版本 `deps-version` 必须包含 Ubuntu 基线、vcpkg ref 和人工递增修订号，不能使用
   `latest`。amd64、arm64 使用同一逻辑依赖版本但不同架构后缀，避免错误导入或混用。
3. 基础镜像准备阶段继续执行 vcpkg tool 和第三方资产的固定摘要校验；不得为了镜像代理或离线
   打包关闭 SHA-512/SHA-256 校验。
4. 导入和构建脚本根据 `uname -m` 只接受 `x86_64` 或 `aarch64`/`arm64`，并校验 bundle
   manifest 中的平台、镜像 tag、镜像 ID/digest 和文件摘要；架构不匹配时 fail closed。
5. 离线包和镜像层不得包含 Registry 凭据、控制面授权 Header、MinIO 密钥、预签名 URL 或真实
   `.env`。

### 文件和操作入口

计划新增以下内容，编码时保持现有在线 Dockerfile 和 Compose 运行入口可用：

1. `Dockerfile.base`：联网构建 `build-base`、`runtime-base` 两个 target。
2. `Dockerfile.offline`：只使用已导入基础镜像的离线应用构建入口。
3. `compose.offline.yaml`：覆盖 Compose 的 build Dockerfile 和两个基础镜像参数，不重复定义运行
   环境、安全策略或端口。
4. `docker/offline/prepare-bundle.sh`：在当前原生架构联网构建两个基础镜像，完成检查后通过
   `docker save` 导出。
5. `docker/offline/load-bundle.sh`：先执行 `sha256sum -c` 和 manifest/架构校验，再通过
   `docker load` 导入本架构基础镜像。
6. `docker/offline/build.sh`：检查基础镜像和依赖清单后，以 `--network=none --pull=false` 构建
   最终镜像；参数负责显式传入依赖版本和最终镜像 tag。
7. `.env.example` 和 README：记录基础镜像 tag、联网准备、离线导入、Docker/Compose 构建、版本
   升级和故障定位命令；真实 `.env` 继续忽略。

每个架构的基础包独立分发，目录至少包含 build-base 镜像 tar、runtime-base 镜像 tar、机器可读
manifest、`SHA256SUMS` 和操作说明。业务源码不固化进基础包，随项目目录单独交付。

### 验收要求

1. 联网 amd64、arm64 主机分别生成匹配架构的两个基础镜像和 bundle，检查镜像平台、固定依赖
   版本、清单及所有 SHA-256 后再分发。
2. 在对应架构的干净 Docker 环境导入 bundle；导入前故意修改 tar 或 manifest 时必须校验失败，
   错误架构 bundle 也必须拒绝。
3. 断开宿主外网并清理本项目 BuildKit 构建缓存后，使用 `--network=none --pull=false --no-cache`
   完成最终镜像构建，Dockerfile 内 23/23 CTest 必须通过。
4. 修改普通 C++ 源码后可以复用同一基础包重新生成最终镜像；修改 `vcpkg.json` 后必须在编译前
   因摘要不一致失败。
5. amd64 最终镜像报告 `linux/amd64`、容器内为 `x86_64`；arm64 最终镜像报告
   `linux/arm64`、容器内为 `aarch64`；两端 Worker 版本、PROJ 数据和动态依赖检查均通过。
6. 最终镜像不包含编译器、CMake、vcpkg、开发头文件和测试二进制；默认非 root 常驻、Compose
   双副本扩容、脱敏日志、SIGTERM 退出和清理行为不得回归。

### 实施结果

1. 已新增 `Dockerfile.base`，分别提供 `build-base` 和 `runtime-base` target；新增
   `Dockerfile.offline`，使用已安装依赖关闭 vcpkg manifest 自动安装，在构建阶段重新编译当前
   源码、执行完整 CTest 并安装 Worker，运行阶段只复制安装产物。
2. 已新增 `compose.offline.yaml`，只覆盖既有 Compose 的 Dockerfile、基础镜像参数、
   `network: none` 和 `pull: false`，没有复制或放宽既有运行时安全配置。
3. 已新增 `docker/offline/common.sh`、`prepare-bundle.sh`、`load-bundle.sh`、`build.sh` 和 bundle
   README。四个脚本均通过 Ubuntu `/bin/sh -n`；加载脚本使用 BusyBox 和 GNU sha256sum 都支持的
   `sha256sum -c`。
4. `.dockerignore` 和 `.gitignore` 已排除 `offline-bundles`；`.env.example` 和 README 已记录联网
   准备、离线导入、脚本构建、Compose 构建、依赖版本升级和双架构原生构建要求，不包含真实凭据。

### Linux amd64 实际验收

1. 在 Linux amd64 Docker 引擎上实际执行 `prepare-bundle.sh`，生成
   `offline-bundles/amd64-validation`。bundle 中 build-base tar 为 841,532,416 字节，runtime-base
   tar 为 100,017,664 字节，另含 `manifest.properties`、`SHA256SUMS` 和 README。
2. build-base 镜像 ID 为
   `sha256:f313f4879837e230cd9739832888e3c429f2b510d9771289e78b3d921103c22c`，runtime-base 镜像 ID 为
   `sha256:f87637a3500dc1f67d402ebe6b5e16f32accb93a3e02d3801b3b0f80923f28e6`；二者平台、依赖版本、
   架构和 `vcpkg.json` SHA-256 标签均通过检查。
3. 删除本机两个基础镜像 tag 后，仅通过 bundle 执行 `load-bundle.sh` 恢复成功；脚本核验了全部
   文件摘要、宿主架构、manifest、镜像 ID、镜像平台、标签和项目 `vcpkg.json` 摘要。
4. 对 bundle README 做单字节级修改后，加载脚本在任何 `docker load` 前因 `SHA256SUMS` 不匹配
   明确失败；有效 bundle 保持只读且未被修改。
5. 对临时项目副本修改 `vcpkg.json` 后，`build.sh` 在调用 Docker 编译前因 build-base 的 manifest
   摘要标签不匹配明确失败；没有产生意外最终镜像。
6. 在无网络 Linux CLI 容器中实际执行 `build.sh`，最终构建使用 `--network none --pull=false
   --no-cache`，Dockerfile 内 23/23 CTest 通过。生成镜像
   `3d-tiles-clip-worker:0.1.0-offline-script-amd64`，ID 为
   `sha256:3f3a3e3b2f48f049dd3fef69af8bf97f6a7818a9aded38ee8b9c43ac471eace6`。
7. 脚本验收确认最终镜像为 `linux/amd64`、UID 10001、Worker 版本 `0.1.0`、PROJ 数据可读，且
   不包含 `/opt/vcpkg`、CMake、Ninja 或 Git。
8. 实际执行 Compose 离线覆盖文件的无缓存构建，构建网络为 `none`，23/23 CTest 再次通过；生成
   `3d-tiles-clip-worker:0.1.0`，ID 为
   `sha256:a87fed67faf49aa159a62892c1bca1ea3a887e120cbdc517bf2ad7bbb70b84a5`。
9. Compose 启动该镜像后确认 UID 10001、只读根文件系统、`CapDrop=[ALL]`、
   `no-new-privileges=true`、`init=true`、无端口映射；测试控制面不可解析时日志保持脱敏。
   `SIGTERM` 后容器退出码为 0，验收容器和 Compose 网络均已清理。

### 尚待原生环境验收

1. 当前宿主不是 ARM64，因此本轮没有生成或导入 arm64 离线 bundle，也没有执行 arm64
   `Dockerfile.offline`/Compose 原生构建。必须在 aarch64 Linux 主机按同一流程完成，不能用当前
   amd64 结果替代，也不引入 QEMU/binfmt。
2. 错误架构 bundle 的拒绝路径已由 `uname -m` 与 manifest 的严格比较实现，但仍需在两种原生
   主机互换 bundle 做实际负向验收。
3. 宿主物理断网和干净 Docker daemon 的最终交付验收仍需在目标离线服务器执行；本轮已验证
   Docker build 网络显式为 `none`、禁止 pull、关闭构建缓存，并在无网络 CLI 容器中完成脚本构建。

## 2026-08-10 ARM64 联网打包国内镜像配置（已实施，待 ARM64 实构建）

### 问题与验证结果

ARM64 联网主机执行 `prepare-bundle.sh` 时无法连接 Docker Hub，导致 `ubuntu:24.04` 基础镜像
解析或拉取失败。当前脚本虽然允许逐个设置镜像环境变量，但缺少单一、明确且可审计的国内网络配置
入口，直接执行文档中的命令仍会访问官方 Docker Hub 和 GitHub。

本轮只读验证结果：

1. `m.daocloud.io/docker.io/library/ubuntu:24.04` 和
   `docker.1ms.run/library/ubuntu:24.04` 均提供 `linux/arm64/v8` manifest；二者当前上游 OCI index
   digest 均为 `sha256:561618e2c15bf2397621dd04f96926663a3b5616c189cf7e38db7e82f5c538ea`。
2. `https://gh-proxy.com/https://github.com/microsoft/vcpkg.git` 可以读取固定标签
   `2025.07.25`，其 tag 对应 commit 为 `dd3097e305afa53f7b4312371f62058d2e665320`。
3. 同一 GitHub 代理可以读取该版本 `vcpkg-tool-metadata.txt`，其中 ARM64 vcpkg tool SHA-512 为
   `332a32ea26225f606bb5317037201cc868258b6dc5a8dd441175dd8cc8c77e3c34c67be32a10fc74138f36d7333db9b696ed8b9ffb35e02808e68fa13d740c89`。

### 拟实施方式

1. `prepare-bundle.sh` 新增可选参数 `--mirror-profile official|cn`；不传时继续使用现有 official
   行为，避免改变已验收的默认构建入口。
2. `cn` 配置使用以下无凭据下载入口：
   - Ubuntu 基础镜像：DaoCloud 上述多架构镜像，并固定 OCI index digest；
   - vcpkg Git 仓库：`gh-proxy.com` 转发的固定 `2025.07.25` 标签；
   - vcpkg tool：通过同一 GitHub 代理下载；
   - vcpkg 内 GitHub 资产：通过同一 GitHub 代理下载；
   - SQLite 源码：`https://mirrors.aliyun.com/macports/distfiles/sqlite3/`。
3. 环境变量仍可显式覆盖 profile 中的单项地址，以便改用企业内部 Registry/代理；不得在 profile、
   manifest、README 或镜像层写入用户名、密码、Token 或带凭据 URL。
4. `prepare-bundle.sh` 在构建前打印所选 profile 和基础镜像引用；bundle manifest 新增
   `network_profile` 和不含凭据的 `base_image`，便于追溯基础包来源。
5. 国内镜像只改变下载路径，不关闭 vcpkg tool SHA-512、第三方资产 SHA-512、`vcpkg.json`
   SHA-256、镜像平台、镜像标签或 bundle 文件摘要校验。
6. 不实现运行中自动切换多个公共镜像源，避免同一命令因网络波动产生不可追溯的上游变化；DaoCloud
   不可用时必须通过环境变量或新的已确认 profile 显式切换。
7. README 和 `.env.example` 增加 ARM64 国内网络命令及各变量说明，推荐命令调整为：

```sh
./docker/offline/prepare-bundle.sh --mirror-profile cn \
  /srv/offline-bundles/clip-worker-arm64-r1
```

### 验收要求

1. 原有单参数 official 命令仍能通过参数解析，现有调用不回归。
2. `cn` profile 展开后不得出现 `docker.io/library/ubuntu` 直接拉取地址或未代理的 GitHub 下载地址。
3. Shell 语法检查、非法 profile、重复参数、缺失输出目录和非空输出目录测试均符合 fail closed。
4. 在原生 ARM64 Linux 主机实际完成 cn profile bundle 构建，23/23 CTest、镜像平台和全部摘要
   校验仍须通过；当前 amd64 主机的配置检查不能代替该项验收。
5. 如果后续失败点是 Ubuntu `apt` 软件源而非 Docker Hub/GitHub/SQLite，再单独增加按架构区分的
   APT 镜像配置；本次不在未观察到该故障前修改容器内 Ubuntu 软件源。

### 实施与验证结果

1. `common.sh` 已集中定义 official/cn 两套无凭据下载常量；国内 Ubuntu 引用固定 DaoCloud OCI
   index digest，未使用 `latest` 或运行时自动回退。
2. `prepare-bundle.sh` 已支持
   `[--mirror-profile official|cn] <empty-output-directory>`；原有单参数调用继续选择 official，环境变量
   对 profile 默认值保持最高优先级。
3. `cn` 参数展开验证确认 Docker build 实际收到 DaoCloud 基础镜像、代理后的 vcpkg Git 仓库、
   vcpkg tool/GitHub 资产前缀和阿里云 SQLite 镜像；未直接请求 Docker Hub 基础镜像。
4. bundle manifest 已增加 `network_profile` 和 `base_image`，现有 schema 和加载脚本保持向后兼容；
   README、bundle README 和 `.env.example` 已补充 profile 命令、覆盖规则和无自动 fallback 说明。
5. 四个 shell 脚本均通过 Linux `/bin/sh -n`。非法 profile、缺失/重复参数、cn/official 非空输出
   目录均按预期在构建前失败，原单参数 official 和新 cn 参数均通过命令展开检查。
6. 固定 DaoCloud 引用已再次确认包含 `linux/arm64/v8`；使用该引用执行
   `docker build --check --platform linux/arm64 -f Dockerfile.base` 成功且无警告，Compose 合并配置
   检查通过。
7. 当前执行环境仍为 amd64，未运行 ARM64 编译和 23 项 CTest。用户需把本次修改同步到原生
   aarch64 Linux 主机后执行 cn profile 命令；若下一处失败发生于 Ubuntu apt 源，应保留完整错误
   输出并按本节约束新增架构对应的 APT 镜像，不得临时关闭校验。

### 国内 APT 镜像扩展（已实施，待 ARM64 实构建）

用户确认 Ubuntu 官方软件源在 ARM64 联网主机可达但速度较慢，希望 `cn` profile 同时切换国内
APT 镜像。只读探测确认阿里云 Ubuntu 24.04 Noble 的 amd64 和 ARM64 `InRelease` 均可访问。

拟实施规则：

1. `cn` profile 按 `uname -m`/Docker `TARGETARCH` 自动选择：
   - amd64：`http://mirrors.aliyun.com/ubuntu`；
   - arm64：`http://mirrors.aliyun.com/ubuntu-ports`。
2. Ubuntu 最小基础镜像在首次 `apt-get update` 前不包含 CA 根证书，因此 bootstrap 阶段使用 HTTP
   镜像；APT 仍必须验证 Ubuntu archive key 签名的 `InRelease` 和包摘要，不增加 `trusted=yes`、
   `AllowInsecureRepositories` 或任何关闭签名验证的配置。
3. `Dockerfile.base` 的 dependency-builder 和 runtime-base 两个阶段均新增 `APT_MIRROR` build arg，
   在 `apt-get update` 前同时兼容 Ubuntu 24.04 Deb822 `ubuntu.sources` 与旧式 `sources.list` URI。
4. 配置了非空 `APT_MIRROR` 时，若找不到官方 archive/security/ports URI 或替换后仍残留这些 URI，
   构建必须明确失败，不能静默回退慢速官方源。
5. `prepare-bundle.sh` 把 profile 对应的架构镜像作为 `APT_MIRROR` 传入两个基础镜像 target；新增
   `CLIP_WORKER_APT_MIRROR` 环境变量允许显式使用企业内网镜像，并保持环境变量优先于 profile。
6. bundle manifest 增加不含凭据的 `apt_mirror`；脚本启动时打印实际 APT 镜像，便于确认 ARM64
   使用的是 `ubuntu-ports` 而不是普通 `ubuntu`。
7. `Dockerfile.base` 直接构建时 `APT_MIRROR` 默认为空，official profile 和既有调用继续使用 Ubuntu
   官方软件源；本次不修改最终离线构建，因为依赖已经固化在基础镜像中。
8. README、`.env.example` 和本节实施记录同步更新。验收覆盖两个架构的配置展开、源替换脚本、
   非法 URL/未替换源失败、Dockerfile 静态检查；ARM64 完整构建和 23/23 CTest 仍在原生主机完成。

实施与验证结果：

1. 新增 `docker/offline/configure-apt-mirror.sh`，集中校验 HTTP/HTTPS 基础 URL，并兼容 Ubuntu
   Deb822 `ubuntu.sources` 和旧式 `sources.list`；找不到受支持官方 URI、替换后仍残留官方 URI
   或替换结果未落盘时均明确失败。
2. `Dockerfile.base` 的 dependency-builder/runtime-base 两阶段已接入 `APT_MIRROR`，均在第一次
   `apt-get update` 前执行同一重写脚本，安装完成后删除容器内可见的临时脚本。
3. `cn` profile 在当前 amd64 参数展开中选择 `http://mirrors.aliyun.com/ubuntu`；ARM64 常量和
   Dockerfile 检查使用 `http://mirrors.aliyun.com/ubuntu-ports`。两个基础 target 均接收同一
   `APT_MIRROR` build arg，`CLIP_WORKER_APT_MIRROR` 可显式覆盖。
4. 在 DaoCloud Ubuntu 24.04 基础镜像中实际执行阿里云 amd64 `apt-get update` 成功，Noble、
   updates、backports、security 索引共下载约 32.1 MB，APT 签名检查未放宽。
5. 一次性 Ubuntu 容器测试覆盖真实 Deb822 amd64 源和模拟 `ports.ubuntu.com/ubuntu-ports` 源，
   均成功替换；无官方源及带查询参数的非法镜像 URL 均在 `apt-get` 前失败。
6. 五个离线 shell 脚本通过 `/bin/sh -n`；固定 DaoCloud 基础镜像加 ARM64 `ubuntu-ports` 参数的
   `docker build --check --platform linux/arm64 -f Dockerfile.base` 成功且无警告，Compose 配置
   回归检查通过。
7. bundle manifest 已记录 `apt_mirror`，README、bundle README 和 `.env.example` 已更新。
   原 official 单参数命令验证仍显示 `APT mirror: official Ubuntu sources`，没有改变既有默认行为。
8. 原生 ARM64 bundle 构建和 Worker 23/23 CTest 仍待用户在 aarch64 Linux 主机运行，当前
   amd64 源替换和 ARM64 静态检查不替代该验收。

## 2026-08-10 ARM64 经典 Docker Builder 兼容（已实施，待 ARM64 完整构建）

### 连续失败与根因

ARM64 主机已成功使用国内镜像完成 Ubuntu 基础镜像解析、APT 更新和 vcpkg 固定标签克隆，但基础包
构建连续遇到两类构建器兼容问题：

1. 默认经典 builder 不自动注入 `TARGETARCH`，导致 vcpkg tool 选择阶段输出
   `Unsupported TARGETARCH:`。
2. 临时设置 `DOCKER_BUILDKIT=1` 后，Docker CLI 明确报告 buildx 组件缺失或损坏；当前主机不能
   使用 BuildKit frontend。即使只显式补充 `TARGETARCH`，现有 `Dockerfile.base` 后续的
   `RUN --mount=type=cache` 仍不兼容经典 builder。

### 可选方案

方案 A：在 ARM64 联网主机安装与当前 Docker 版本匹配的 buildx 插件，保留 vcpkg downloads 和
binary archives 的 BuildKit cache mount。该方案需要先提供 `docker version`、操作系统发行版和
包管理器信息，才能选择兼容安装方式；安装属于主机级变更，不由 Worker 脚本自动执行。

方案 B（推荐）：只把依赖基础包和最终离线构建入口改为兼容经典 builder，不要求主机安装 buildx：

1. `prepare-bundle.sh` 在 build-base/runtime-base 两次 `docker build` 中显式传入
   `--build-arg TARGETARCH=${NATIVE_ARCH}`，确保镜像内 vcpkg tool、triplet 和架构标签不为空。
2. `Dockerfile.base` 将 vcpkg install 的 `RUN --mount=type=cache` 改为普通 `RUN`；依赖版本、
   vcpkg ref、tool/source SHA 和 bundle 摘要校验全部保留。
3. 基础包准备继续使用 `--pull --no-cache`，因此失败重试会重新下载 vcpkg 依赖；这是兼容经典
   builder 的明确代价，不通过复用不透明旧层降低正确性要求。
4. `build.sh` 显式传入 `TARGETARCH=${NATIVE_ARCH}`；`Dockerfile.offline` 同时支持显式值和
   `dpkg --print-architecture` 原生兜底，仍只接受 amd64/arm64。
5. `prepare-bundle.sh` 不再强制 `DOCKER_BUILDKIT=1`。检测到调用方显式设置
   `DOCKER_BUILDKIT=1` 但 buildx 不可用时，可提示移除该变量后使用经典兼容路径。
6. 原在线 `Dockerfile` 仍保留 BuildKit cache mount 和双架构 buildx 发布能力；本次只保证
   `Dockerfile.base`、`Dockerfile.offline` 及其脚本在原生 amd64/arm64 主机兼容经典 builder。
7. README 明确区分：原生架构离线 bundle 可使用经典 builder；跨架构/多架构 Registry 发布仍
   要求 buildx。本次不引入 QEMU/binfmt，也不改变 ARMv7/32 位排除规则。

### 验收要求

1. 使用假 Docker 参数展开验证 prepare/build 两个脚本均显式传入当前原生架构。
2. `Dockerfile.base` 和 `Dockerfile.offline` 不包含经典 builder 不支持的 Dockerfile 指令；
   BuildKit 环境继续可以构建同一文件。
3. 空、amd64、arm64 和非法 `TARGETARCH` 分支均 fail closed；基础镜像架构标签必须非空。
4. 国内镜像、APT 源改写、固定摘要、bundle 校验、最终离线 `--network none --pull=false
   --no-cache` 和 23/23 CTest 要求保持不变。
5. 当前 amd64 环境完成静态与参数回归后，ARM64 主机使用新的空输出目录重跑完整 bundle；不得
   把失败目录当作有效 bundle 继续加载。

### 实施与本地回归结果

2026-08-10 用户确认采用方案 B，已完成以下修改：

1. `prepare-bundle.sh` 的 build-base 和 runtime-base 构建均显式传入宿主检测所得
   `TARGETARCH`；`build.sh` 的最终离线构建也显式传入相同参数，不再依赖 BuildKit 自动参数。
2. `Dockerfile.base` 已移除 vcpkg install 步骤的两个 `RUN --mount=type=cache`，改为经典 builder
   可解析的普通 `RUN`；`--pull --no-cache`、固定版本和全部摘要校验保持不变。
3. `Dockerfile.offline` 优先使用显式 `TARGETARCH`，未提供时使用
   `dpkg --print-architecture` 获取原生架构；amd64 映射 `x64-linux`、arm64 映射
   `arm64-linux`，其他值明确失败。
4. 公共脚本新增 builder 前置检查。只有调用方主动设置 `DOCKER_BUILDKIT=1` 且 buildx 不可用时
   才在任何镜像构建前失败，并提示移除该变量；默认路径不要求安装 buildx。
5. README 已区分原生离线构建和多架构发布：前者支持经典 builder，后者继续使用 buildx；在线
   `Dockerfile`、QEMU/binfmt 排除规则和 ARMv7/32 位排除规则没有变化。
6. 五个离线 shell 脚本通过 Git for Windows Bash 的 POSIX 语法检查；Compose 离线覆盖配置展开
   成功，最终构建网络仍为 `none`。
7. 假 Docker 参数回归确认 amd64 和 arm64 的基础包命令均带显式平台与架构参数；ARM64 cn
   profile 同时选中 `TARGETARCH=arm64` 和阿里云 `ubuntu-ports`。最终 ARM64 离线构建参数保持
   `--network none --pull=false --no-cache`，并显式传入 `TARGETARCH=arm64`。
8. buildx 缺失分支实测以退出码 1 返回明确提示；架构解析的 amd64、arm64、空值原生兜底和非法
   值拒绝分支均通过回归。
9. 当前 Windows Docker daemon 未运行，未在本轮重新执行完整 amd64 镜像构建；此前 amd64
   23/23 CTest 结果不受本次构建入口调整替代。原生 ARM64 bundle、最终镜像及 23/23 CTest 仍须
   用户在 aarch64 Linux 主机用新的空输出目录完成。
