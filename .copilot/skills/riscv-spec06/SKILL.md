---
name: riscv-spec06
user-invocable: true
description: "适用于：在 QEMU RISC-V linux-user 环境中排查 SPEC CPU2006 运行失败、短时假成功、输入准备缺失、目录布局不匹配，并修复批量统计流程。"
argument-hint: "要记录或排查的 SPEC06 benchmark / 失败现象"
---

# RISC-V SPEC06 故障排查 Skill

## 目标
沉淀一套可复用的排查与修复流程，用于在 QEMU RISC-V linux-user 环境中定位 SPEC CPU2006 运行失败原因，区分真实成功与短时假成功，并修复输入准备、目录布局和批量 runner 行为。

## 适用场景
- 需要批量运行 SPEC CPU2006 并统计指令频次。
- 某个 benchmark 返回码异常，或返回码为 `0` 但运行时间明显异常偏短。
- 程序报错涉及输入文件缺失、控制文件缺失、运行目录布局不匹配。
- 同一程序在 SPEC harness 下应能运行，但手工/批量 runner 下失败。
- 需要把一次故障定位结论整理成可复用修复步骤。

## 已验证的典型故障模式
1. 短时假成功
- 现象：返回码为 `0`，CSV 也已生成，但程序运行时间只有数秒。
- 处理：不能仅用返回码判成功，必须结合运行时间和日志内容复核。

2. 输入文件缺失
- 现象：程序启动即报 `No such file`、`Unable to read ...`、`Cannot open file ...`。
- 处理：先核对 `cwd` 与命令，再核对 SPEC harness 是否本来会在运行前动态生成或搬运输入文件。

3. 目录布局不匹配
- 现象：命令本身没错，但程序把某个位置参数当作真实文件名，当前目录里却没有该文件。
- 处理：对照 benchmark 自带 `Spec/object.pm`、源码或输入说明，复现 harness 的前处理逻辑。

4. 二进制输入变体选错
- 现象：文件已存在，但运行时从“找不到文件”推进为“读到 EOF / 格式错误 / 读单元失败”。
- 处理：检查端序、记录头大小、不同数据变体（如 `le/32`、`le/64`）是否与当前构建实际匹配。

## 本轮会话中已确认的故障与解法

### 1. sphinx3/default
- 失败现象：`Unable to read ctlfile`
- 根因：`./sphinx3 ctlfile . args.an4` 中的 `ctlfile` 会被程序当作真实文件名直接 `fopen()`；当前运行目录缺少该文件，且缺少由 harness 生成的标准 `*.raw`。
- 关键证据：
  - `spec_main_live_pretend.c` 里直接执行 `ctlfile = argv[1]` 和 `fopen(ctlfile, "r")`
  - `Spec/object.pm` 会在运行前生成 `ctlfile` 并把 `*.le.raw`/`*.be.raw` 复制为标准 `*.raw`
- 解法：
  - 生成 `ctlfile`
  - 生成标准 `basename.raw`
  - 已落地脚本：`myScript/prepare-sphinx3-inputs.py`

### 2. wrf/default
- 首轮失败现象：`Cannot open file 'LANDUSE.TBL'`
- 后续失败现象：`module_ra_rrtm: error reading unit 10`
- 根因：
  - 运行目录未完整复现 SPEC harness 的 `post_setup`
  - `RRTM_DATA` 虽有候选文件，但默认选到的变体与当前 riscv 构建不匹配
- 关键证据：
  - `481.wrf/Spec/object.pm` 会把 `be/le` 及 `32/64` 子目录中的数据搬到运行目录根下
  - `module_ra_rrtm.F90` 明确打开 `RRTM_DATA`
  - `le/64/RRTM_DATA` 会触发 `End of file`
  - `le/32/RRTM_DATA` 可使程序越过原失败点并最终跑通
- 解法：
  - 补齐 `LANDUSE.TBL`、`SOILPARM.TBL`、`VEGPARM.TBL`、`GENPARM.TBL`
  - 把正确的 `RRTM_DATA` 变体放到运行目录根下
  - 已落地脚本：`myScript/prepare-wrf-inputs.py`
  - 已接入批量 runner：`myScript/run-spec06-insn-freq.py` 会在执行 `wrf` 前自动准备输入

## 排查流程

### 第一步：先判定是不是“真失败”
1. 记录返回码、CSV 是否生成、耗时、日志路径。
2. 若返回码非 `0`，直接按失败处理。
3. 若返回码为 `0` 但运行时间异常短，继续看日志是否有 `FATAL`、`STOP`、`Error termination`。
4. 对短时任务保留例外名单；本轮中 `gamess/exam29` 是已知短任务例外。

### 第二步：确认运行上下文
1. 记录：
  - `cwd`
  - 实际执行命令
  - 目标输入目录
  - 目标输出 CSV
2. 优先排查“是不是在错误目录下运行”。
3. 若命令依赖 shell 重定向，必须保留 shell 语义，不要直接拆 argv。

### 第三步：缩小到首个直接失败点
1. 先看日志中第一条明确错误，而不是只看最后一行。
2. 若错误包含文件名：
  - 先检查运行目录中是否存在该文件
  - 再查源码看这个文件名是硬编码、命令参数，还是由 harness 动态生成
3. 若错误是 `read unit`、`EOF`、`format`：
  - 优先怀疑二进制输入变体不匹配
  - 检查端序、头大小、文件大小是否和候选版本一致

### 第四步：对照 SPEC harness，而不是只看当前目录
1. 查 benchmark 自带的 `Spec/object.pm`。
2. 找：
  - `post_setup`
  - `invoke`
  - 输入文件生成/拷贝逻辑
3. 若 harness 会在运行前生成文件、拷贝文件或重命名文件，手工/批量 runner 必须显式复现这一步。

### 第五步：做最小判别性验证
1. 只改一个局部因素后立即重跑单任务。
2. 优先选择最便宜、最能证伪当前假设的测试：
  - 补一个缺失文件
  - 切一个输入变体
  - 改正确 `cwd`
  - 用 `test/input` 小输入冒烟
3. 若失败从“找不到文件”推进到“读到 EOF”，说明方向对了，但输入格式仍不匹配。

### 第六步：先用小输入冒烟，再回到完整输入
1. 若 benchmark 有 `data/test/input`，优先用它验证：
  - QEMU 能否正常执行该程序
  - 插件是否能稳定生成 CSV
  - 当前输入准备逻辑是否正确
2. 小输入通过后，再回到 `ref` 输入做完整验证。

### 第七步：把修复固化到 runner 或准备脚本
1. 对一次性手工修复，不要停在 shell 命令。
2. 若同类任务会重复失败，应新增准备脚本或接入批量 runner。
3. 修复后至少补一次单任务验证，再决定是否批量重跑。

## 决策分支

### 分支 A：日志里直接点名缺失文件
- 先查运行目录
- 再查 `Spec/object.pm` 是否会生成/搬运它
- 若会，优先复现 harness；若不会，再查数据包是否不完整

### 分支 B：返回码为 0 但耗时异常短
- 不直接判成功
- 看日志是否有 `FATAL`、`STOP`、`Unable to read`、`Error termination`
- 若有，按失败处理并继续定位

### 分支 C：文件存在但读失败 / EOF
- 检查是否选错 `be/le`
- 检查是否选错 `32/64`
- 比较候选文件大小、来源和实际运行结果

### 分支 D：小输入能过，大输入不过
- 说明 QEMU/插件和程序主体通常没根本问题
- 继续聚焦 `ref` 输入准备、数据变体、长流程特有配置

## 完成标准
- 能给出“首个直接失败点”的明确文字结论。
- 能说明该失败是：
  - QEMU/插件问题
  - 运行目录问题
  - 输入准备问题
  - 数据变体问题
 之一。
- 至少完成一次最小判别性验证。
- 若已找到稳定解法，必须把修复沉淀为脚本或 runner 逻辑，而不是只保留手工命令。
- 对批量场景，必须更新进度记录与成功/失败判定口径。

## 推荐产物
- 失败日志摘录
- 根因结论
- 修复脚本或 runner 改动
- 单任务验证结果
- 是否需要批量重跑的建议

## 直接可复用的经验
- `sphinx3` 失败时，优先查 `ctlfile` 和标准 `*.raw` 是否由 harness 生成。
- `wrf` 失败时，优先查 `Spec/object.pm` 的 `post_setup` 是否被复现，以及 `RRTM_DATA` 是否选对变体。
- 批量 SPEC 运行不能只看返回码，必须结合耗时与日志判断是否是假成功。