# 2026-08-11 Clip Worker 结构化日志完善

## 当前状态

- 状态：方案已于 2026-08-11 确认，功能已实施并通过 amd64 验收。
- 需求来源：现有日志缺少时间和任务处理上下文，无法快速判断任务成功、失败及具体失败原因。
- 本文档是独立的新需求 Spec，不并入 `20260807-worker-foundation.md`。

## 现状与问题定位

当前常驻 Worker 仅在以下场景输出少量非结构化文本：

1. 领取任务时控制面请求失败，只输出 HTTP 状态和异常消息；
2. 领取响应不合法时，只输出一条固定文本；
3. 任务失败后再次调用控制面上报失败时，只输出 `assetId`。

正常的任务领取、对象下载、裁剪、对象上传和任务完成均没有日志。现有文本也没有应用侧时间、日志级别、稳定事件名、Worker 标识、任务阶段、耗时和输入输出统计。因此只看 `docker compose logs` 时，无法回答以下问题：

- Worker 是否正常启动、使用了什么非敏感配置、何时退出；
- 是否领取到任务，以及当前任务执行到了哪个阶段；
- 某个 `assetId` 最终是成功、空结果还是失败；
- 失败发生在领取、下载、校验、裁剪、上传、心跳、完成回调或失败回调中的哪一步；
- 失败是否可重试、向控制面上报失败是否成功；
- 网络失败是 DNS、连接、超时还是 HTTP 非成功状态；
- 成功任务耗时、输入输出大小及裁剪前后统计是多少。

此外，心跳线程当前吞掉原始异常，只在主线程抛出固定的 `Worker lease heartbeat failed`，会丢失 HTTP/CURL 诊断信息。对象传输的 CURL 失败也只保留固定文本，未保留 `curl_easy_strerror` 的安全诊断。

## 改造目标

1. 每条运行日志包含明确时间、级别、稳定事件名和可读消息；
2. 每个已领取任务都能通过 `assetId` 串联完整处理过程；
3. 每个任务必须产生唯一、明确的最终成功或失败日志；
4. 失败日志必须包含失败阶段、业务错误码、错误消息、是否可重试、总耗时和失败上报结果；
5. 成功日志必须包含结果类型、总耗时、输入输出字节数和裁剪统计；
6. 保留足够的 HTTP/CURL 诊断信息，同时不泄露凭证、租约和预签名地址；
7. 日志适合 Docker 标准输出采集、单行检索和后续日志平台解析；
8. 不改变现有目录职责、控制面接口契约、裁剪算法和 Docker 日志轮转策略。

## 建议的日志格式（待确认）

默认采用 UTF-8 单行 JSON，每行代表一个完整事件。JSON 可避免错误消息中的引号、换行破坏日志边界，并便于按 `event`、`assetId`、`errorCode` 检索。

建议公共字段：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `timestamp` | string | UTC RFC 3339 时间，精确到毫秒，例如 `2026-08-11T12:34:56.789Z` |
| `level` | string | `DEBUG`、`INFO`、`WARN` 或 `ERROR` |
| `event` | string | 稳定事件名，供检索和告警规则使用 |
| `message` | string | 简短的人类可读消息 |
| `workerId` | string | Worker 实例标识；运行日志默认携带 |
| `assetId` | string | 已领取任务的资产标识；仅任务事件携带 |

示例（字段顺序只用于可读性，不作为接口约束）：

```json
{"timestamp":"2026-08-11T12:34:56.789Z","level":"INFO","event":"task.succeeded","message":"Clip task completed","workerId":"worker-01","assetId":"asset-123","result":"READY","durationMs":1280,"inputBytes":32768,"outputBytes":16384,"triangleCountBefore":200,"triangleCountAfter":86}
```

```json
{"timestamp":"2026-08-11T12:35:02.123Z","level":"ERROR","event":"task.failed","message":"Object download returned HTTP 403","workerId":"worker-01","assetId":"asset-456","stage":"DOWNLOAD","errorCode":"TRANSFER_FAILED","retryable":true,"durationMs":615,"failureReported":true}
```

## 日志级别与配置（待确认）

新增环境变量 `CLIP_WORKER_LOG_LEVEL`，允许值通过枚举统一定义，大小写不敏感：

- `INFO`：默认值；输出启动退出、任务阶段完成、任务最终成功及所有警告/错误；
- `DEBUG`：在 `INFO` 基础上输出无任务轮询和心跳成功，便于短期排障；
- `WARN`：只输出可恢复异常、协议异常和错误；
- `ERROR`：只输出任务失败、失败上报失败和致命启动错误。

非法值在进程启动时直接失败，并通过结构化 `application.start_failed` 日志说明配置项不合法。默认 `INFO` 不记录每次空轮询和每次心跳成功，避免空闲 Worker 持续刷屏。

对应更新 `.env.example`、`compose.yaml` 和 README 配置说明。

## 事件设计

### Worker 生命周期

| 事件 | 级别 | 必需字段 | 触发条件 |
| --- | --- | --- | --- |
| `worker.started` | INFO | `workerId`、`algorithmVersion`、输入/输出上限、轮询/心跳间隔、日志级别 | 配置校验完成并准备进入轮询 |
| `worker.stop_requested` | INFO | `workerId` | 收到停止信号且轮询循环结束 |
| `application.start_failed` | ERROR | `errorType`、`errorMessage` | 配置解析、依赖初始化等导致进程无法启动 |
| `application.command_failed` | ERROR | `command`、`errorType`、`errorMessage` | `inspect` 等一次性命令失败 |

启动日志不得输出控制面授权头、完整控制面 URL或其他凭证。输入/输出限制和轮询参数属于非敏感运行配置，可以输出。

### 领取与协议错误

| 事件 | 级别 | 必需字段 | 触发条件 |
| --- | --- | --- | --- |
| `claim.empty` | DEBUG | `workerId` | 控制面返回 204，无任务可领 |
| `claim.failed` | WARN | `workerId`、`httpStatus`（如有）、`errorMessage` | 领取请求发生 CURL 或 HTTP 错误，Worker 将继续轮询 |
| `claim.invalid_response` | ERROR | `workerId`、固定 `errorCode` | 领取响应无法按任务契约解析 |
| `task.claimed` | INFO | `workerId`、`assetId`、`contentFormat`、`leaseExpireTime` | 成功解析领取响应 |

`claim.invalid_response` 不输出响应正文、解析异常原文、租约、预签名 URL 或 WKB。继续沿用现有 fail-closed 脱敏策略，只通过固定错误码和固定消息表明响应协议不合法。

### 已领取任务的处理阶段

为失败定位引入内部 `TaskStage` 枚举，至少包含：

- `DOWNLOAD`：下载源对象及其 HTTP 处理；
- `SOURCE_VALIDATION`：源 ETag、大小和必要字段校验；
- `CLIP`：B3DM/GLB 解析、坐标转换、几何和纹理裁剪；
- `OUTPUT_VALIDATION`：输出大小和摘要计算；
- `UPLOAD`：上传非空结果；
- `HEARTBEAT`：租约心跳；
- `COMPLETE_REPORT`：向控制面上报成功；
- `FAILURE_REPORT`：向控制面上报失败。

成功阶段日志：

| 事件 | 级别 | 主要字段 |
| --- | --- | --- |
| `download.completed` | INFO | `assetId`、`durationMs`、`inputBytes` |
| `clip.completed` | INFO | `assetId`、`durationMs`、`result`、裁剪前后顶点/三角形/纹理字节统计 |
| `upload.completed` | INFO | `assetId`、`durationMs`、`outputBytes` |
| `task.succeeded` | INFO | `assetId`、`result`、`durationMs`、输入输出字节数、完整裁剪统计 |

阶段开始事件默认不在 `INFO` 输出，避免同一任务产生过多重复日志；如需要细粒度排障，可在 `DEBUG` 输出 `download.started`、`clip.started` 和 `upload.started`。空裁剪结果不执行上传，因此只输出 `clip.completed` 和最终 `task.succeeded`，其中 `result=EMPTY`、`outputBytes=0`。

### 心跳

1. `DEBUG` 级别可输出 `heartbeat.succeeded`；
2. 心跳异常必须保留原始异常类型、安全错误消息和 HTTP 状态（如有），输出 `heartbeat.failed`；
3. 心跳线程保存 `std::exception_ptr`，停止时在任务线程重新抛出原异常，保证最终任务日志能够正确归类为 `CONTROL_PLANE_FAILED` 和可重试失败，而不是丢失原始原因后归类为普通处理失败；
4. 不输出 `leaseToken`。

第 3 点会纠正当前明显不合理的异常丢失和错误分类，但不改变心跳失败时任务不能成功完成的 fail-closed 行为。

### 任务最终结果

每个 `task.claimed` 之后必须恰好出现一个最终结果事件：

- 成功：`task.succeeded`，包含 `READY` 或 `EMPTY`；
- 失败：`task.failed`。

`task.failed` 至少包含：

| 字段 | 说明 |
| --- | --- |
| `stage` | 失败发生的任务阶段枚举值 |
| `errorCode` | 上报控制面的稳定错误码 |
| `errorMessage` | 经单行 JSON 转义后的安全异常消息 |
| `errorType` | `FORMAT`、`TRANSFER`、`CONTROL_PLANE` 或 `PROCESSING` |
| `retryable` | 是否建议控制面重试 |
| `durationMs` | 从领取任务进入处理到失败的总耗时 |
| `failureReported` | 是否成功调用控制面 fail 接口 |
| `reportHttpStatus` | fail 接口失败时的 HTTP 状态（如有） |
| `reportErrorMessage` | fail 接口失败时的安全错误消息（如有） |

任务原始异常先归类，再尝试调用 fail 接口，最后只输出一条信息完整的 `task.failed`。这样即使 fail 接口也失败，本地日志仍同时保留原始任务错误和二次上报错误。

## 错误诊断完善

1. 控制面 CURL 错误继续保留 `curl_easy_strerror`；非成功 HTTP 保留操作名和状态码；
2. 对象下载/上传 CURL 错误补充 `curl_easy_strerror`，例如 DNS 解析失败、连接失败或超时；
3. 对象传输非 200 响应记录 HTTP 状态码；
4. 格式错误记录现有 `FormatErrorCode` 映射后的稳定错误码和 `what()`；
5. 标准异常记录 `what()`，但不尝试输出原生堆栈。当前 C++17 工程没有可移植堆栈能力，引入堆栈库会扩大依赖、离线包和双架构构建范围；本次通过“阶段 + 类型 + 错误码 + 消息 + HTTP/CURL 原因”满足定位需求；
6. 捕获未知异常时输出固定 `Unknown non-standard exception`，避免静默失败。

## 敏感信息与日志边界

任何级别均禁止输出：

- `CLIP_WORKER_AUTHORIZATION_HEADER` 及其值；
- `leaseToken`；
- `sourceDownloadUrl` 和 `outputUploadUrl`；
- 控制面错误响应正文；
- claim 原始响应正文；
- `scopeWkbBase64` 和完整 `worldTransform`；
- SHA-256、ETag 等当前排障不需要且可能形成对象关联的值。

允许输出：`workerId`、`assetId`、内容格式、租约过期时间、阶段、错误码、HTTP 状态、CURL 公共错误描述、字节数、耗时和聚合裁剪统计。

日志消息中的换行、回车、制表符和引号由 JSON 序列化器转义，确保一条事件只占一行，避免日志注入和检索断行。

## 代码结构设计

保持现有项目目录结构，只新增符合当前职责的日志模块：

- `include/clip_worker/logging/logger.hpp`：`LogLevel` 枚举、级别解析/名称转换、结构化日志接口；
- `src/logging/logger.cpp`：UTC 毫秒时间、JSON 序列化、级别过滤和线程安全写出；
- `tests/unit/logger_test.cpp`：日志格式、时间、级别过滤、单行转义和并发完整性测试；
- `src/task/worker_runtime.cpp`：接入任务生命周期、阶段、最终结果、心跳异常传播和失败上报结果日志；
- `src/client/object_transfer.cpp`：补全不含 URL 的 CURL 错误原因；
- `src/app/main.cpp`：解析日志级别，记录生命周期与启动/命令致命错误；
- `CMakeLists.txt`：注册新增实现和测试；
- `.env.example`、`compose.yaml`、`README.md`：补充日志级别配置、字段说明和带时间查看示例。

日志实现复用工程已有的 `nlohmann_json`，不新增第三方依赖，避免重建离线依赖 bundle。

## 测试与验收

### 单元测试

1. JSON 日志可被 `nlohmann::json` 解析，公共字段齐全；
2. `timestamp` 使用 UTC `Z` 后缀并精确到毫秒；
3. `DEBUG/INFO/WARN/ERROR` 解析和过滤符合预期，非法配置被拒绝；
4. 含引号、换行和制表符的错误消息仍只输出一个物理日志行；
5. 多线程并发写日志时，每条 JSON 均完整，不互相穿插；
6. 日志级别和任务阶段使用枚举及集中转换函数，不散落魔法字符串；
7. 对象传输 CURL 失败消息包含安全的 CURL 原因且不包含请求 URL。

### 运行链路验收

使用合成任务或可控测试服务覆盖：

1. 启动和停止各有一条带时间、级别和 `workerId` 的日志；
2. READY 任务包含 `task.claimed`、阶段完成日志和唯一 `task.succeeded`；
3. EMPTY 任务包含唯一 `task.succeeded`，`result=EMPTY` 且无上传完成日志；
4. 下载、ETag 校验、格式解析/裁剪、输出过大、上传、心跳、complete 和 fail 接口失败均能从 `task.failed.stage` 区分；
5. `task.failed` 明确展示原始失败原因、是否可重试以及 `failureReported`；
6. INFO 默认不打印空轮询和成功心跳；DEBUG 能打印；
7. 在完整日志中搜索授权头、租约令牌和预签名 URL 的测试值，必须不存在；
8. 原有 CTest 全量通过，当前可用架构的容器构建和启动 smoke test 通过；
9. `docker compose logs --follow clip-worker` 可直接看到应用时间和任务结果；README 同时补充 `--timestamps` 作为 Docker 接收时间的辅助查看方式。

## 不在本次范围

- 不接入 ELK、Loki、OpenTelemetry 或远程日志传输；
- 不增加日志文件落盘，继续由 Docker `json-file` driver 采集并轮转；
- 不记录 HTTP 请求/响应正文；
- 不增加原生堆栈回溯第三方依赖；
- 不改变任务 API 字段、重试协议、裁剪算法或结果数据；
- 不调整现有 `max-size`、`max-file` 默认值。

## 待确认项

编码前需要确认以下方案：

1. 是否接受默认“单行 JSON + UTC 毫秒时间”，而不是中文纯文本或北京时间文本；
2. 是否接受新增 `CLIP_WORKER_LOG_LEVEL`，默认 `INFO`，仅 `DEBUG` 输出空轮询和成功心跳；
3. 是否接受阶段完成日志默认 INFO，从而一个正常 READY 任务通常输出领取、下载完成、裁剪完成、上传完成、最终成功共 5 条日志；
4. 是否接受修正心跳异常传播，使心跳失败保留原始诊断并归类为可重试的 `CONTROL_PLANE_FAILED`；
5. 是否接受不引入堆栈库，本次以阶段、错误类型、错误码、异常消息、HTTP/CURL 原因和耗时完成错误定位。

确认本文档后再开始修改代码；若上述任一项需要调整，先更新本 Spec，再进入实现。

## 实施结果（2026-08-11）

### 已完成内容

1. 新增 `logging::Logger` 和 `LogLevel`，输出线程安全的 UTF-8 单行 JSON；公共字段包括 UTC 毫秒时间、级别、事件和消息，调用方字段无法覆盖公共字段；
2. 新增 `CLIP_WORKER_LOG_LEVEL`，支持大小写不敏感的 `DEBUG`、`INFO`、`WARN`、`ERROR`，默认 `INFO`，非法值在启动阶段 fail closed；
3. Worker 生命周期已接入 `worker.started` 和 `worker.stop_requested`，启动日志只包含非敏感配置；
4. 领取链路已接入 `claim.empty`、`claim.failed`、`claim.invalid_response` 和 `task.claimed`；无任务仅在 DEBUG 输出，畸形响应继续使用固定脱敏诊断；
5. 下载、裁剪和上传已接入 started/completed 事件；阶段开始仅在 DEBUG 输出，阶段完成默认 INFO，并包含耗时、字节数和裁剪统计；
6. 每个已领取任务最终输出唯一 `task.succeeded` 或 `task.failed`；成功包含 READY/EMPTY、总耗时和统计，失败包含阶段、错误类型/码/消息、可重试性、总耗时、原始 HTTP 状态和失败上报结果；
7. 心跳线程保存并重新抛出原始 `std::exception_ptr`，同时输出 `heartbeat.failed` 安全诊断；控制面心跳失败现在正确归类为可重试 `CONTROL_PLANE_FAILED`；
8. 对象下载/上传 CURL 失败消息已补充 `curl_easy_strerror`，不包含预签名 URL；
9. `main` 的启动和命令异常改为 `application.start_failed` / `application.command_failed` 结构化错误；`inspect` 正常 JSON 输出、版本和命令用法保持原 CLI 行为；
10. `.env.example`、Compose 和 README 已同步日志级别、结构化字段、安全边界及日志查看命令；Docker `json-file` 轮转参数未改变；
11. 未新增第三方依赖，既有离线依赖 bundle 可直接复用。

### 自动化验证结果

使用本机已加载的 amd64 离线基础镜像执行无网络、无缓存构建：

```text
BUILD_BASE_IMAGE=3d-tiles-clip-worker-build-base:ubuntu24.04-vcpkg2025.07.25-r1-amd64
RUNTIME_BASE_IMAGE=3d-tiles-clip-worker-runtime-base:ubuntu24.04-vcpkg2025.07.25-r1-amd64
TARGETARCH=amd64
compiler=GCC 13.3.0
CTest=29/29 passed
```

新增测试覆盖：

1. 四种日志级别解析、大小写兼容和非法值拒绝；
2. 单行 JSON 公共字段和 UTC 毫秒时间格式；
3. 最低日志级别过滤；
4. 引号、换行和制表符转义，确保错误消息不产生多行注入；
5. 四线程并发写入 100 个事件，每行均为完整可解析 JSON；
6. CURL 失败保留公共诊断且不包含测试预签名 URL。

运行镜像 smoke test 结果：

1. `--version` 输出 `0.1.0`；
2. `CLIP_WORKER_LOG_LEVEL=TRACE` 输出单行 `application.start_failed`，包含 UTC 时间、ERROR 级别和明确配置错误，进程退出码为 1；
3. 使用不可达的 `http://127.0.0.1:1` 启动 Worker，日志依次包含 `worker.started` 和 `claim.failed`，错误消息明确为 `Could not connect to server`，未输出控制面 URL；
4. 向临时容器发送 SIGTERM 后输出 `worker.stop_requested` 并正常退出；临时容器已由 `--rm` 自动清理；
5. `docker compose config --quiet` 校验通过，`CLIP_WORKER_LOG_LEVEL` 默认配置可正常展开。

### 尚未执行的跨架构验收

本轮实际编译和 CTest 使用 linux/amd64 离线基础镜像。代码未新增架构相关实现或依赖，但 linux/arm64 的完整实构建仍需在原生 ARM64 环境按既有交付流程执行；当前 amd64 验收不能替代该项。
