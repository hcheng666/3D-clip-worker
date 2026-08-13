# 3D Tiles Clip Worker

`3D Tiles Clip Worker` 是一个独立的 C++17 常驻任务进程，用于按照授权空间范围严格裁切
3D Tiles Mesh 内容。Worker 从元数据控制面领取任务，通过短期预签名 URL 下载源 B3DM、执行
几何与纹理裁切、上传结果，并向控制面报告 `READY`、`EMPTY` 或失败状态。

项目采用 **fail closed** 原则：格式不受支持、输入无效、校验失败或处理异常时，不会把原始 Tile
作为已授权结果透传。生产目标平台为 Linux `amd64` 和 Linux `arm64`；Windows 仅作为开发构建
环境。不支持 ARMv7 和其他 32 位 ARM 平台。

当前程序版本为 `0.1.3`，默认裁切算法版本为 `v4`。

## 功能范围

### 已支持能力

- 严格解析 B3DM v1 和内嵌 GLB 2.0，并对长度、Chunk、Table 和对齐进行边界校验；
- 在通过完整结构校验的前提下，有限兼容 GLB 起始位置仅按四字节对齐的旧 B3DM；
- 将 EPSG:4490 Polygon、MultiPolygon 和 Hole 投影到局部米制坐标系；
- 使用空间索引筛选授权三角形候选集，再由精确裁切器完成三角形/棱柱裁切；
- 对 `POSITION`、`TEXCOORD_0`、`NORMAL` 和 `COLOR_0` 执行边界插值；
- 对内嵌 WebP 执行 UV Mask，将授权范围外像素 RGBA 全部清零并无损重编码；
- 仅重建仍被活动 Scene 引用的 glTF 资源，并按扩展白名单输出最小 GLB/B3DM；
- 下载时校验源对象 ETag、大小和 SHA-256，上传时校验输出大小限制；
- 支持 claim、heartbeat、complete、fail 的长轮询任务生命周期；
- 支持 `SIGINT`、`SIGTERM` 优雅停止和 JSON Lines 结构化日志；
- 提供合成 B3DM/GLB 测试数据，不在仓库中保存生产数据集。

### 当前输入限制

- 任务内容格式必须为受支持的 B3DM + glTF 2.0，且 `gltfUpAxis` 必须显式为 `Z`；
- 仅支持未压缩的 `TRIANGLES` Primitive 和内嵌 Buffer；
- B3DM 仅支持 `BATCH_LENGTH=1`，不支持额外的二进制 Feature/Batch Table 语义；
- 纹理裁切使用 `TEXCOORD_0` 和内嵌 WebP；
- 已知 WebGL 枚举值的非标准 `sampler.wrapR` 会被校验、记录兼容性告警并从输出移除；
- `wrapS`、`wrapT` 仍按二维纹理 Mask 的受支持模式严格校验。

Draco、Meshopt、KTX2/BasisU、外部 glTF 资源、未知必需扩展及其他未明确支持的内容均会
fail closed，不会生成 `READY` 结果。

## 处理流程

1. Worker 携带自身 ID 和算法版本向控制面发送 `claim` 请求。
2. 无任务时按轮询周期等待；领取任务后启动租约 `heartbeat`。
3. 使用控制面下发的短期预签名 URL 下载源对象，并校验大小、ETag 和 SHA-256。
4. 校验 B3DM/GLB、任务 `gltfUpAxis` 和授权范围，执行几何、属性及纹理裁切。
5. 无授权内容时直接上报 `EMPTY`，不创建零字节占位对象。
6. 有授权内容时重建并复核 B3DM，通过预签名 URL 上传，然后上报 `READY` 及对象摘要。
7. 任一阶段失败时按错误类型和重试语义调用 `fail`；无法安全解析的 claim 响应不会继续处理，
   其租约由控制面的超时机制恢复。

Worker 自身不监听端口，仅主动访问元数据控制面和控制面下发的对象存储预签名 URL。所有控制面
请求都会携带内部来源 Header `from-source: inner`。

## 项目结构

```text
.
├── include/clip_worker/       # 公共头文件
├── src/
│   ├── app/                   # CLI 与进程入口
│   ├── client/                # 控制面客户端和对象传输
│   ├── clip/                  # B3DM/GLB 裁切与重建
│   ├── formats/               # B3DM、GLB 严格解析
│   ├── geometry/              # 投影、空间索引和三角形裁切
│   ├── logging/               # JSON Lines 结构化日志
│   └── task/                  # 任务契约和 Worker 生命周期
├── tests/                     # GoogleTest 单元测试和合成 Tile
├── docker/offline/            # 离线依赖包准备、导入和构建脚本
├── CMakeLists.txt             # CMake 工程定义
├── CMakePresets.json          # 本地及 Linux 双架构 Preset
├── vcpkg.json                 # 第三方依赖清单
├── Dockerfile                 # 在线多阶段构建
├── Dockerfile.base            # 在线生成离线基础镜像
├── Dockerfile.offline         # 无网络应用镜像构建
├── compose.yaml               # 常规运行编排
└── compose.offline.yaml       # 离线构建覆盖配置
```

## 本地开发

### 环境要求

- CMake 3.24 或更高版本；
- Ninja；
- 支持 C++17 的编译器；
- vcpkg，并设置环境变量 `VCPKG_ROOT`。

项目依赖由 `vcpkg.json` 声明，包括 libcurl、nlohmann/json、OpenSSL、PROJ、libwebp、
earcut.hpp 和 GoogleTest。

### 编译与测试

Debug 构建：

```powershell
$env:VCPKG_ROOT = "<vcpkg-directory>"
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Release 构建：

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Linux 原生环境还可以显式选择生产目标 triplet：

```sh
cmake --preset x64-linux-release       # amd64 主机
cmake --build --preset x64-linux-release
ctest --preset x64-linux-release

cmake --preset arm64-linux-release     # arm64 主机
cmake --build --preset arm64-linux-release
ctest --preset arm64-linux-release
```

`x64-linux-release` 和 `arm64-linux-release` 用于各自架构的原生构建，不代表项目内置了
QEMU 交叉运行环境。

### CLI 命令

```text
3d-tiles-clip-worker --version
3d-tiles-clip-worker inspect <tile.b3dm>
3d-tiles-clip-worker run
```

- `--version`：输出程序版本；
- `inspect`：严格解析最大 512 MiB 的 B3DM，并输出不含二进制内容和签名 URL 的 JSON 结构摘要；
- `run`：读取环境变量并启动常驻 Worker。

Windows Debug 构建示例：

```powershell
& .\build\debug\3d-tiles-clip-worker.exe --version
& .\build\debug\3d-tiles-clip-worker.exe inspect .\sample.b3dm
```

## Worker 配置

### 运行变量

| 变量 | 是否必填 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `CLIP_WORKER_CONTROL_PLANE_URL` | 是 | 无 | 元数据控制面基础 URL，必须能从 Worker 容器访问 |
| `CLIP_WORKER_ID` | 否 | `HOSTNAME`/`COMPUTERNAME` | Worker 唯一标识；Compose 扩容时建议留空 |
| `CLIP_WORKER_AUTHORIZATION_HEADER` | 否 | 空 | 完整 HTTP Header，例如 `Authorization: Bearer ...` |
| `CLIP_WORKER_ALGORITHM_VERSION` | 否 | `v4` | 必须与控制面的裁切算法版本一致 |
| `CLIP_WORKER_MAX_INPUT_BYTES` | 否 | `67108864` | 单个输入对象最大字节数，必须为正整数 |
| `CLIP_WORKER_MAX_OUTPUT_BYTES` | 否 | `67108864` | 单个输出对象最大字节数，必须为正整数 |
| `CLIP_WORKER_POLL_INTERVAL_SECONDS` | 否 | `5` | 空任务或领取失败后的轮询间隔，单位为秒 |
| `CLIP_WORKER_HEARTBEAT_INTERVAL_SECONDS` | 否 | `30` | 租约心跳间隔，单位为秒 |
| `CLIP_WORKER_API_CONNECT_TIMEOUT_SECONDS` | 否 | `10` | 控制面连接超时，单位为秒 |
| `CLIP_WORKER_API_REQUEST_TIMEOUT_SECONDS` | 否 | `30` | 控制面请求超时，单位为秒 |
| `CLIP_WORKER_TRANSFER_CONNECT_TIMEOUT_SECONDS` | 否 | `10` | 对象存储连接超时，单位为秒 |
| `CLIP_WORKER_TRANSFER_REQUEST_TIMEOUT_SECONDS` | 否 | `300` | 对象下载/上传请求超时，单位为秒 |
| `CLIP_WORKER_LOG_LEVEL` | 否 | `INFO` | 可选 `DEBUG`、`INFO`、`WARN`、`ERROR` |

`CLIP_WORKER_AUTHORIZATION_HEADER` 是完整 Header，而不是只填写 Token。不要把它写入镜像层、
README、日志或提交到版本库。

### Compose 与构建变量

| 变量 | 默认值 | 说明 |
| --- | --- | --- |
| `COMPOSE_PROJECT_NAME` | `three-d-tiles-clip-worker` | Compose 项目名 |
| `CLIP_WORKER_IMAGE` | `3d-tiles-clip-worker:0.1.3` | 构建或部署的 Worker 镜像 |
| `CLIP_WORKER_STOP_GRACE_PERIOD` | `10m` | 收到 `SIGTERM` 后允许在途任务完成的时间 |
| `CLIP_WORKER_LOG_MAX_SIZE` | `10m` | 单个 Docker 日志文件大小上限 |
| `CLIP_WORKER_LOG_MAX_FILES` | `5` | Docker 日志文件保留数量 |
| `CLIP_WORKER_BASE_IMAGE` | `ubuntu:24.04` | 在线构建或离线基础镜像构建使用的 Ubuntu 镜像 |
| `CLIP_WORKER_APT_MIRROR` | 空 | 仅供 `Dockerfile.base` 使用的 Ubuntu 软件源覆盖地址 |
| `CLIP_WORKER_VCPKG_REF` | `2025.07.25` | 固定的 vcpkg 版本 |
| `CLIP_WORKER_VCPKG_REPOSITORY` | vcpkg 官方 GitHub 仓库 | vcpkg 仓库或批准的镜像地址 |
| `CLIP_WORKER_VCPKG_ASSET_PREFIX` | `https://github.com` | vcpkg tool 下载前缀 |
| `CLIP_WORKER_VCPKG_GITHUB_ASSET_PREFIX` | 空 | GitHub 依赖资产代理前缀 |
| `CLIP_WORKER_VCPKG_SQLITE_MIRROR_PREFIX` | 空 | SQLite 依赖资产镜像前缀 |
| `CLIP_WORKER_DEPS_VERSION` | `ubuntu24.04-vcpkg2025.07.25-r1` | 离线依赖版本 |
| `CLIP_WORKER_BUILD_BASE_IMAGE` | 带版本和架构的本地镜像 | 离线编译基础镜像 |
| `CLIP_WORKER_RUNTIME_BASE_IMAGE` | 带版本和架构的本地镜像 | 离线运行基础镜像 |

敏感运行配置应写入本地 `.env` 或部署系统的 Secret。项目已忽略 `.env`，但不会阻止误写到其他
受版本控制的文件中。

## 结构化日志

Worker 每个事件输出一行 JSON，包含 UTC 毫秒时间 `timestamp`、`level`、稳定的 `event` 名称和
便于阅读的 `message`。任务相关事件还包含 `workerId` 和 `assetId`。

- `INFO`：记录任务领取、下载/裁切/上传完成、最终结果、警告和错误；
- `DEBUG`：额外记录空轮询、成功心跳和各阶段开始事件；
- 成功任务只产生一个终态 `task.succeeded`，包含 `READY` 或 `EMPTY`、耗时、对象大小和裁切统计；
- 失败任务只产生一个终态 `task.failed`，包含阶段、错误类型、错误码、可重试性、耗时以及是否成功
  上报控制面。

日志不会记录授权 Header、租约 Token、预签名 URL、claim/HTTP 响应正文、授权范围 WKB、输出
SHA-256 或 ETag。

已通过严格校验但使用旧式四字节 B3DM 对齐的输入会产生一次
`source.compatibility_warning`，兼容码为 `B3DM_NONCONFORMANT_ALIGNMENT`。合法但非标准的
`sampler.wrapR` 会产生一次兼容性告警 `GLTF_NONSTANDARD_SAMPLER_WRAP_R`。非空输出始终按标准
八字节边界重建。

查看日志：

```powershell
docker compose logs --follow clip-worker
docker compose logs --follow --timestamps clip-worker
```

## Docker 构建

`Dockerfile` 会在 build 阶段安装固定 vcpkg 依赖、编译 Release 版本并执行完整 CTest；测试失败时
不会生成运行镜像。运行镜像仅包含程序、运行库和 PROJ 数据。

构建当前 Docker 平台：

```powershell
docker build --tag 3d-tiles-clip-worker:dev .
```

构建并推送 Linux amd64/arm64 多平台镜像：

```powershell
docker buildx build --platform linux/amd64,linux/arm64 `
  --tag <registry>/3d-tiles-clip-worker:<version> --push .
```

也可以使用项目提供的 Bake 定义：

```powershell
docker buildx bake -f docker-bake.hcl `
  --set "worker.tags=<registry>/3d-tiles-clip-worker:<version>" --push
```

多平台构建分别使用 `x64-linux` 和 `arm64-linux` vcpkg triplet，并在各自 build stage 中执行
测试。发布后应检查远程 manifest 同时包含 `linux/amd64`、`linux/arm64`，且不包含 ARMv7。

## Docker Compose 部署

服务不映射入站端口。复制示例配置并填写实际控制面地址：

```powershell
Copy-Item .env.example .env
docker compose config
```

`CLIP_WORKER_CONTROL_PLANE_URL` 缺失时，`docker compose config` 会直接失败。

### 本机原生架构构建并启动

```powershell
docker compose build
docker compose up --detach
```

Compose 不固定 `platform`：源码构建使用宿主机原生架构，从 Registry 拉取时由 Docker 选择多平台
manifest 中匹配的架构。

### 使用 Registry 镜像部署

在 `.env` 中将 `CLIP_WORKER_IMAGE` 设置为不可变的生产版本，然后执行：

```powershell
docker compose pull
docker compose up --detach --no-build
```

### 常用运维命令

```powershell
docker compose ps
docker compose logs --follow clip-worker
docker compose up --detach --no-build --scale clip-worker=3
docker compose stop
docker compose down
```

扩容时保持 `CLIP_WORKER_ID` 为空，使每个副本使用不同的 Docker `HOSTNAME`。容器以 UID `10001`
运行，根文件系统只读，删除全部 Linux capabilities，启用 `no-new-privileges`，并限制 Docker 日志
文件大小和数量。

默认停止宽限期为十分钟。收到 `SIGTERM` 后 Worker 会停止领取新任务，并尽量完成在途任务；超出
宽限期后未完成租约由控制面的既有超时路径恢复。生产环境应根据单个任务最长处理时间调整
`CLIP_WORKER_STOP_GRACE_PERIOD`。

### 副本扩容与回滚

应用默认保持一个副本。对固定任务、固定授权范围和相同总资源口径，建议依次验证：

```powershell
docker compose up --detach --no-build --scale clip-worker=1
docker compose up --detach --no-build --scale clip-worker=2
docker compose up --detach --no-build --scale clip-worker=4
```

每一级都应等待任务真实到达终态，并记录每分钟完成资产数、下载/裁切/上传 P95 与 P99、重试和失败
次数、单副本峰值内存、总 CPU、MinIO 吞吐与错误、控制面 claim/heartbeat/complete 延迟以及
PostgreSQL 锁等待。当吞吐不再明显提升或任何正确性、失败率、内存、对象存储、数据库或控制面指标
恶化时停止扩容。

回滚容量时只恢复到上一个安全副本数，例如：

```powershell
docker compose up --detach --no-build --scale clip-worker=1
```

副本回滚不需要重置任务或 `READY` 资产。控制面和全部运行中的 Worker 必须使用相同算法版本，
当前为 `v4`。

## 离线构建与交付

离线流程把稳定依赖与频繁变化的 Worker 源码分开：

- `build-base`：包含编译器、CMake、Ninja、固定 vcpkg toolchain 和 `vcpkg.json` 的开发依赖；
- `runtime-base`：仅包含 CA 证书、系统运行库、PROJ 数据和 UID `10001`；
- `Dockerfile.offline`：复制当前源码、执行编译和完整 CTest，再把安装结果复制到
  `runtime-base`。

amd64 与 arm64 依赖包必须分别在联网的原生 Linux 主机生成，并只在相同架构的离线主机使用。
该流程不依赖 QEMU/binfmt，也不支持交叉架构复用。依赖包和 Worker 项目源码是两个独立交付物。

### 在联网主机准备依赖包

从项目根目录执行。输出目录必须为空，且建议放在 Docker build context 之外：

```sh
chmod +x docker/offline/*.sh
./docker/offline/prepare-bundle.sh /srv/offline-bundles/clip-worker-deps
```

脚本会检测原生 `amd64` 或 `arm64`，构建和验证两个基础镜像，并输出：

```text
clip-worker-deps/
├── build-base-<deps-version>-<arch>.tar
├── runtime-base-<deps-version>-<arch>.tar
├── manifest.properties
├── SHA256SUMS
└── README.md
```

中国大陆网络环境可以显式选择 `cn` 镜像配置：

```sh
./docker/offline/prepare-bundle.sh --mirror-profile cn \
  /srv/offline-bundles/clip-worker-deps
```

`cn` 配置使用固定的多架构 Ubuntu 镜像索引以及明确配置的 GitHub、SQLite 和 Ubuntu 镜像，
但不会关闭 vcpkg tool、源资产、APT 签名、manifest 或 bundle 摘要校验。显式设置的
`CLIP_WORKER_BASE_IMAGE`、`CLIP_WORKER_APT_MIRROR` 和 `CLIP_WORKER_VCPKG_*` 变量优先于
profile，便于替换为批准的内部 Registry 或代理。镜像地址和构建参数不是 Secret 存储位置，不要
在其中加入凭据。

脚本兼容 Docker classic builder。没有 buildx 的主机不要强制设置 `DOCKER_BUILDKIT=1`；如果该
变量已经全局导出，请先执行 `unset DOCKER_BUILDKIT`。由于依赖准备使用 `--no-cache`，失败重试时
可能重新下载依赖。

### 在离线主机导入并构建

将匹配架构的依赖包和当前 Worker 项目目录复制到离线主机。在项目根目录执行：

```sh
./docker/offline/load-bundle.sh /media/offline/clip-worker-deps
./docker/offline/build.sh 3d-tiles-clip-worker:0.1.3
```

`load-bundle.sh` 在导入前校验 `SHA256SUMS`、bundle manifest、宿主架构、镜像 ID/标签及当前
`vcpkg.json` 的 SHA-256。`build.sh` 强制使用 `--network none --pull=false --no-cache`，完成
Release 编译、CTest、镜像架构、运行 UID、PROJ 数据和“不包含构建工具链”的检查。

只修改 `include/`、`src/`、`tests/` 或 CMake 工程文件时可以复用基础镜像。修改
`vcpkg.json`、Ubuntu 基线、vcpkg 版本、编译器或依赖构建选项后，必须提升依赖版本，并在
amd64、arm64 联网主机重新生成各自的 bundle。

### 使用 Compose 执行离线构建

在 `.env` 中设置与本机架构匹配的 `CLIP_WORKER_BUILD_BASE_IMAGE` 和
`CLIP_WORKER_RUNTIME_BASE_IMAGE`，然后执行：

```sh
docker compose -f compose.yaml -f compose.offline.yaml config
docker compose -f compose.yaml -f compose.offline.yaml build --no-cache
docker compose up --detach --no-build
```

前两条命令使用 `Dockerfile.offline`、`network: none` 和 `pull: false`；最后一条使用
`compose.yaml` 中的既有运行安全策略。

## 安全与生产注意事项

- Worker 只使用控制面下发的短期预签名 URL，不持有 MinIO 长期访问凭据；
- 不要把 `.env`、授权 Header、Registry 凭据、预签名 URL 或真实生产数据写入镜像和仓库；
- 原始 3D Tiles、索引、授权状态、审计状态和非本 Worker 对象不属于 Worker 清理范围；
- 算法升级或回滚时，控制面配置与所有 Worker 的 `CLIP_WORKER_ALGORITHM_VERSION` 必须同步；
- `READY` 输出不得被重新标记为另一算法版本；失败结果不得回退为边界原始 content；
- 生产发布前应分别验证 amd64/arm64 镜像、远程 manifest、控制面/MinIO 全链路、停止恢复和目标
  数据集下的资源与吞吐指标。

## 当前验证状态

项目既有 Spec 记录：`0.1.3` 的 amd64 离线 Release 镜像曾在禁用网络、拉取和构建缓存的条件下
完成构建，61 项 CTest 全部通过；最终镜像以 UID `10001` 运行，包含 PROJ 数据且不包含构建
toolchain。

该记录不替代目标环境验收。当前仍需在原生 arm64 环境执行同版本完整实构建，并使用代表性生产
数据验证端到端正确性、吞吐、峰值内存、对象存储压力、控制面延迟和多副本扩展效果。
