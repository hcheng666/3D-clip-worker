# 2026-08-13 Clip Worker README 文档整理

## 任务背景

用户要求为 `3d-tiles-clip-worker` 项目生成 README。

项目根目录当前已经存在 `README.md`，内容为英文，约 14 KB；现有文档并非空白占位，已经覆盖
项目定位、支持范围、本地构建、Worker 配置、结构化日志、在线 Docker 构建、离线依赖包、
Docker Compose 部署和副本扩缩容。为避免未经确认覆盖已有文档，本次任务先校验代码和部署文件，
再按用户确认的语言与改写范围处理现有 README。

当前目录不是独立 Git 仓库，因此本任务不存在自动执行 `git add` 的条件。

## 文档目标

README 应成为开发、构建、部署和运维该 Worker 的统一入口，并且只描述当前代码已经实现、配置文件
已经提供或既有 Spec 已经明确验收的能力，不把未完成的真实数据联调和生产压测写成已完成事项。

目标读者包括：

1. 需要理解裁切能力和输入限制的后端及 GIS 开发人员；
2. 需要在本地编译、运行测试或检查 B3DM 文件的 C++ 开发人员；
3. 需要通过 Docker Compose 部署、扩容和排障的运维人员；
4. 需要在隔离网络中交付 amd64/arm64 镜像的实施人员。

## 已核对的项目事实

1. 工程使用 C++17、CMake 3.24+、Ninja、vcpkg manifest 和 GoogleTest。
2. 当前工程版本为 `0.1.3`，默认裁切算法版本为 `v4`。
3. 正式运行平台为 Linux amd64 和 Linux arm64；Windows 仅作为开发构建环境，不支持
   ARMv7 和其他 32 位 ARM。
4. 可执行程序支持 `run`、`inspect <b3dm>` 和 `--version` 三种命令入口；容器默认执行
   `run`。
5. Worker 通过内部 REST 接口完成 claim、heartbeat、complete 和 fail，通过预签名 URL
   下载源对象、上传 READY 结果；服务自身不监听入站端口。
6. `compose.yaml` 已配置非 root 用户、只读根文件系统、删除全部 capabilities、禁止新增权限、
   日志轮转和停止宽限期。
7. `.env.example` 包含控制面地址、Worker 标识、授权 Header、算法版本、输入输出上限、轮询与
   心跳周期、API/对象传输超时、日志级别、在线构建镜像源和离线基础镜像等配置。
8. 在线镜像构建由 `Dockerfile` 提供；离线交付由 `Dockerfile.base`、
   `Dockerfile.offline`、`compose.offline.yaml` 和 `docker/offline/*.sh` 提供。
9. 当前裁切范围包括严格 B3DM v1/GLB 2.0 解析、EPSG:4490 授权范围投影、三角形/棱柱裁切、
   顶点属性插值、WebP UV Mask 与输出重建；不支持的压缩或外部资源按 fail closed 处理。
10. 现有 Spec 记录最近一次 amd64 离线 Release 构建共有 61 项 CTest 通过；该记录不能替代
    当前环境重新执行测试，也不能表述为真实生产数据吞吐验收。

## 拟定 README 结构

确认后，README 将按以下顺序整理：

1. **项目简介**：说明 Worker 的职责、控制面关系和 fail closed 安全原则。
2. **功能与限制**：分别列出已支持能力、输入前提和明确不支持的格式。
3. **处理流程**：简述 claim、下载、裁切、上传、complete/fail 的完整生命周期。
4. **项目结构**：说明 `include/`、`src/`、`tests/`、`docker/` 和关键构建文件用途。
5. **本地开发**：列出依赖、vcpkg 配置、Debug/Release 构建、测试及 `inspect`/`--version`
   命令。
6. **配置说明**：用表格区分必填、可选运行变量和构建变量，注明默认值、单位及敏感信息要求。
7. **Docker 构建**：覆盖当前架构构建和 amd64/arm64 多平台发布命令。
8. **Docker Compose 部署**：覆盖配置校验、启动、日志、状态、停止、扩容和回滚。
9. **离线交付**：覆盖联网准备依赖包、离线校验导入、断网构建和基础包失效条件。
10. **日志与排障**：说明 JSON 行日志、日志级别、关键终态事件和不记录的敏感字段。
11. **安全与生产注意事项**：说明非 root、无入站端口、算法版本一致性和生产验收边界。

## 拟定修改范围

建议在保留现有技术事实和可执行命令的基础上，重组并完善当前 `README.md`，而不是删除已存在的
离线构建、结构化日志和扩缩容说明。具体调整包括：

1. 增加当前缺少的项目目录结构和 Worker 处理流程；
2. 补充 `inspect <b3dm>`、`--version` 的本地使用方法；
3. 把零散的运行变量整理为完整表格，并与 `.env.example` 逐项核对；
4. 清晰区分“已实现能力”“已执行验证”和“仍需在目标生产环境验收”的内容；
5. 保留既有在线、离线和 Compose 操作命令，修正与当前代码或版本不一致之处；
6. 不引入徽章、外部截图、未经证实的性能指标、真实服务地址、凭据或预签名 URL。

## 待用户确认

现有 README 为内容较完整的英文文档，因此开始修改前需要确认最终语言和改写方式。建议方案为：

**将现有 `README.md` 整体整理为中文，保留命令、变量名、事件名和格式名的英文原文；不另建重复的
英文 README。**

如果需要继续保留英文，也可以选择只完善现有英文 README，或采用中文主 README 加独立英文文档
的双语方案。未确认前不覆盖 `README.md`。

## 验收标准

1. README 中出现的版本、默认值、变量名、文件名和命令与当前工程文件一致；
2. Markdown 标题层级、表格、列表和代码块可以正常渲染；
3. README 提到的本地构建、Docker、Compose 和离线脚本命令均有对应工程入口；
4. 文档不泄露凭据、授权 Header 内容、预签名 URL 或生产对象信息；
5. 文档明确 Linux amd64/arm64 支持范围、fail closed 行为和未完成的生产环境验收边界；
6. 使用文本检查确认 `.env.example` 的运行变量均在 README 中有说明；
7. 本次仅修改文档，不因 README 任务重新编译 C++ 工程；如发现命令或配置存在实际错误，先记录
   差异并与用户确认是否扩大到代码或部署文件修改。

## 用户确认

用户已于 2026-08-13 确认采用建议方案：将现有 `README.md` 整体整理为中文，保留命令、环境
变量、日志事件、格式名和其他技术标识的英文原文，不另建重复的英文 README。

## 实施结果

本次在不改变项目结构和程序行为的前提下完成以下文档调整：

1. 将现有英文 README 重组为中文主文档，并保留原文档中已经存在的在线 Docker、离线交付、
   Compose 运维、结构化日志和扩缩容说明；
2. 增加 Worker 与控制面的完整任务生命周期，明确 `READY`、`EMPTY` 和失败路径；
3. 增加项目目录结构，说明应用入口、控制面客户端、裁切器、格式解析、几何索引、日志、任务运行时、
   测试和离线脚本的职责；
4. 增加 `--version`、`inspect <tile.b3dm>` 和 `run` 三个 CLI 入口及本地使用示例，并注明
   `inspect` 的 512 MiB 输入上限和脱敏结构摘要行为；
5. 将运行变量整理为必填项、默认值、单位和用途明确的表格，并单独整理 Compose、在线构建及离线
   构建变量；
6. 明确输入支持边界，包括 `gltfUpAxis=Z`、未压缩 `TRIANGLES`、内嵌 Buffer、
   `BATCH_LENGTH=1`、`TEXCOORD_0` 和内嵌 WebP；
7. 明确 Draco、Meshopt、KTX2/BasisU、外部 glTF 资源和未知必需扩展等内容 fail closed；
8. 保留并澄清结构化日志终态、敏感字段脱敏、旧 B3DM 对齐和非标准 `wrapR` 的兼容性告警；
9. 完善 Docker 当前平台构建、显式 `docker-bake.hcl` 双架构 Bake、Compose 部署、扩容、停止和
   容量回滚命令；
10. 完善联网准备依赖包、`official|cn` 镜像配置、bundle 校验导入、强制断网构建、基础镜像失效
    条件和 Compose 离线构建说明；
11. 将既有 61 项 amd64 CTest 记录与尚未完成的 arm64 同版本实构建、真实数据全链路和生产性能
    验收明确区分，未把历史验证表述为当前环境重新验收。

## 文档校验结果

1. `README.md` 已通过严格 UTF-8 解码检查；
2. Markdown 共 42 个代码围栏，开始与结束数量匹配；
3. `.env.example` 中解析到的 28 个变量均可在 README 中找到对应说明；
4. README 记录的程序/CMake 版本 `0.1.3` 与 `src/app/main.cpp`、`CMakeLists.txt` 一致；
5. README 记录的默认算法版本 `v4` 与 `WorkerRuntimeConfig` 一致；
6. 文档列出的 12 个主要目录和工程文件均真实存在；
7. `docker compose --env-file .env.example config --quiet` 配置展开成功；Docker 客户端因当前用户
   无权读取其全局 `config.json` 输出警告，但命令退出码为 0，未影响 Compose 校验；
8. `docker buildx bake -f docker-bake.hcl --print` 成功展开 `worker` target，且只包含
   `linux/amd64`、`linux/arm64`；同样存在 Docker 全局配置读取警告，但 Bake 定义校验成功；
9. 本次未修改 C++、CMake、Docker 或 Compose 行为，因此按已确认验收范围未重新执行编译、CTest
   或镜像构建；
10. `D:/code/3d-tiles-clip-worker` 当前不是独立 Git 仓库，没有执行 `git add`。
