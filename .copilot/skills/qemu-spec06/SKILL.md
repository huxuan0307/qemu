---
name: qemu-spec06
user-invocable: true
description: "适用于：使用脚本在 QEMU 中批量运行 SPEC CPU2006、统计每个子任务的指令频次、识别短时假成功、修复输入准备问题，并汇总 56 个有效 CSV。"
argument-hint: "SPEC06 运行目标、输出目录或当前失败现象"
---

# QEMU SPEC06 批量运行 Skill

## 目标
把“用脚本在 QEMU 中运行 SPEC CPU2006 并产出可信指令频次结果”的流程固定成一套可复用方法。输出应包括：
- 每个子任务独立的指令频次 CSV
- 批量执行日志与汇总表
- 过滤假成功后的最终有效结果清单
- 收集后的 56 个有效 CSV
- 以指令为行、测试名为列的总矩阵文件

## 适用场景
- 需要在 `qemu-riscv64` 下批量运行 `speccpu2006_commands.yaml` 中的任务。
- 需要为每个 SPEC06 子任务单独统计指令频次。
- 需要区分“真成功”和“返回码为 0 但实际上提前退出”的假成功。
- 需要把多轮 retry 和专项修复结果收口成一套最终可信数据。

## 依赖脚本
- `myScript/run-spec06-insn-freq.py`
- `myScript/prepare-sphinx3-inputs.py`
- `myScript/prepare-wrf-inputs.py`
- `myScript/collect-spec06-valid-csvs.py`

## 输入前提
- 已构建可用的 `qemu-riscv64`。
- 已构建可用的指令频次插件 `.so`。
- 有可用的 SPEC06 根目录，且其中存在 `cmd/speccpu2006_commands.yaml`。
- 需要时能访问 benchmark 的 `Spec/object.pm`，用于复现 harness 前处理。

## 标准输出物
- 批量目录中的 `batch_*.log`
- 批量目录中的 `summary.tsv`
- 合并后的有效结果表，例如 `spec06_merged_effective_summary.tsv`
- 无效旧 CSV 清单，例如 `spec06_invalid_old_csvs.tsv`
- 收集目录，例如 `spec06_valid_csvs/`
- 聚合矩阵，例如 `spec06_valid_insn_matrix.csv`

## 核心规则

### 1. 每个子任务独立输出一个 CSV
- 命名约定：`{benchmark}_{subtask}_ref_insn_freq.csv`
- 日志与 CSV 必须一一对应，便于回看失败原因。

### 2. 不能只看返回码
- 真成功条件必须同时满足：
  - `returncode == 0`
  - `csv_ok == 1`
   - `elapsed_sec >= 阈值`
- 任一条件不满足，都按失败处理。

### 3. 短时运行阈值必须显式配置
- 默认阈值可设为 `5` 秒。
- 更稳妥的写法是把阈值视为本轮批量运行参数，而不是固定常量。
- 推荐在每次批量收口时明确记录：
   - 本轮阈值是多少
   - 为什么选这个阈值
   - 是否存在应豁免的短跑任务

### 4. 小于阈值的记录一律视为失败
- 即使 CSV 存在、返回码为 `0`，也不能直接当作有效结果。
- 这条规则用于过滤 benchmark 提前报错、QEMU 仍正常退出的假成功。

### 5. 最终分析只能使用“最终有效 CSV”
- 首轮主批次、retry、专项修复目录里的旧 CSV 可能互相冲突。
- 后续分析必须先收口，再统一使用最终有效 CSV 集合。

## 输出目录命名约定

### 1. 主批次目录
- 格式：`spec06_ref/`
- 用途：存放全量首轮运行结果。

### 2. retry 目录
- 格式：`spec06_ref_retryN/`
- 其中 `N` 为递增轮次，例如：
   - `spec06_ref_retry1/`
   - `spec06_ref_retry2/`
- 用途：只存放失败项或可疑项的重跑结果。

### 3. 专项修复验证目录
- 格式：`{benchmark}_fix_test/` 或 `{benchmark}_fix_test_{tag}/`
- 例如：
   - `sphinx3_fix_test/`
   - `wrf_fix_test_runner/`
- 用途：存放输入修复、harness 前处理复现或单任务专项验证结果。

### 4. 收口结果目录或文件
- 推荐固定放在 `.copilot/{task}/output/` 根下，task是本轮对话的标识，例如`vsetvli`。
- 例如：
   - `spec06_merged_effective_summary.tsv`
   - `spec06_invalid_old_csvs.tsv`
   - `spec06_valid_csvs/`
   - `spec06_valid_insn_matrix.csv`

### 5. 命名原则
- 不覆盖上一轮目录。
- 目录名必须能看出用途，而不是只写日期或临时编号。
- 若是专项修复，应在目录名里体现 benchmark 名称和修复语义。
- 汇总产物使用稳定名字，便于后续脚本直接引用。

## 当前环境下的 56 项运行时长参考

以下时长来自 `.copilot/inst_freq/output/spec06_merged_effective_summary.tsv` 的最终有效结果，可作为后续仿真估时参考。

使用时注意：
- 这些数值是当前 QEMU、当前插件、当前输入集和当前宿主环境下的经验值。
- 它们适合用于估算长短任务、分批并行和超时预算，不适合当作跨环境绝对基准。
- 有效 benchmark log 已统一收集到 `.copilot/inst_freq/output/log/`。
- 未被采用的 retry/fix log 已归档到 `.copilot/inst_freq/output/deprecated/`，后续不要再用它们做时长参考。

- `GemsFDTD/default`: 5788.005s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/GemsFDTD_default.log`
- `astar/biglakes`: 398.128s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/astar_biglakes.log`
- `astar/rivers`: 803.291s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/astar_rivers.log`
- `bwaves/default`: 5209.797s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/bwaves_default.log`
- `bzip2/chicken`: 179.868s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/bzip2_chicken.log`
- `bzip2/combined`: 393.781s, source=`spec06_ref_retry1`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/bzip2_combined.log`
- `bzip2/html`: 768.471s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/bzip2_html.log`
- `bzip2/liberty`: 301.935s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/bzip2_liberty.log`
- `bzip2/program`: 598.115s, source=`spec06_ref_retry1`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/bzip2_program.log`
- `bzip2/source`: 505.348s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/bzip2_source.log`
- `cactusADM/default`: 17979.672s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/cactusADM_default.log`
- `calculix/default`: 16967.323s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/calculix_default.log`
- `dealII/default`: 4771.161s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/dealII_default.log`
- `gamess/cytosine`: 6943.250s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/gamess_cytosine.log`
- `gamess/exam29`: 6.440s, source=`spec06_ref_retry1`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/gamess_exam29.log`
- `gamess/gradient`: 4933.642s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/gamess_gradient.log`
- `gamess/triazolium`: 15546.737s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/gamess_triazolium.log`
- `gcc/166`: 112.978s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/gcc_166.log`
- `gcc/200`: 261.698s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/gcc_200.log`
- `gcc/cpdecl`: 137.336s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/gcc_cpdecl.log`
- `gcc/expr`: 152.322s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/gcc_expr.log`
- `gcc/expr2`: 199.454s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/gcc_expr2.log`
- `gcc/g23`: 239.265s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/gcc_g23.log`
- `gcc/s04`: 207.280s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/gcc_s04.log`
- `gcc/scilab`: 111.544s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/gcc_scilab.log`
- `gcc/typeck`: 181.790s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/gcc_typeck.log`
- `gobmk/13x13`: 591.136s, source=`spec06_ref_retry2`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/gobmk_13x13.log`
- `gobmk/nngs`: 1540.084s, source=`spec06_ref_retry2`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/gobmk_nngs.log`
- `gobmk/score2`: 2195.141s, source=`spec06_ref_retry1`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/gobmk_score2.log`
- `gobmk/trevorc`: 607.548s, source=`spec06_ref_retry2`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/gobmk_trevorc.log`
- `gobmk/trevord`: 819.210s, source=`spec06_ref_retry2`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/gobmk_trevord.log`
- `gromacs/default`: 33248.655s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/gromacs_default.log`
- `h264ref/foreman.baseline`: 695.367s, source=`spec06_ref_retry1`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/h264ref_foreman.baseline.log`
- `h264ref/foreman.main`: 659.401s, source=`spec06_ref_retry1`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/h264ref_foreman.main.log`
- `h264ref/sss`: 5790.996s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/h264ref_sss.log`
- `hmmer/nph3`: 874.753s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/hmmer_nph3.log`
- `hmmer/retro`: 2359.401s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/hmmer_retro.log`
- `lbm/default`: 4308.942s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/lbm_default.log`
- `leslie3d/default`: 5247.986s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/leslie3d_default.log`
- `libquantum/default`: 1877.676s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/libquantum_default.log`
- `mcf/default`: 480.282s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/mcf_default.log`
- `milc/default`: 9380.421s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/milc_default.log`
- `namd/default`: 30876.784s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/namd_default.log`
- `omnetpp/default`: 1908.771s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/omnetpp_default.log`
- `perlbench/checkspam`: 2492.756s, source=`spec06_ref_retry1`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/perlbench_checkspam.log`
- `perlbench/diffmail`: 859.129s, source=`spec06_ref_retry1`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/perlbench_diffmail.log`
- `perlbench/splitmail`: 1489.195s, source=`spec06_ref_retry1`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/perlbench_splitmail.log`
- `povray/default`: 2788.728s, source=`spec06_ref_retry2`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/povray_default.log`
- `sjeng/default`: 4279.625s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/sjeng_default.log`
- `soplex/pds-50`: 599.725s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/soplex_pds-50.log`
- `soplex/ref`: 1760.463s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/soplex_ref.log`
- `sphinx3/default`: 10593.905s, source=`sphinx3_fix_test`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/sphinx3_default.log`
- `tonto/default`: 7162.694s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/tonto_default.log`
- `wrf/default`: 7595.507s, source=`wrf_fix_test_runner`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/wrf_default.log`
- `xalancbmk/default`: 4659.971s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/xalancbmk_default.log`
- `zeusmp/default`: 8675.978s, source=`spec06_ref`, log=`/nfs/home/huxuan/repos/qemu/.copilot/inst_freq/output/log/zeusmp_default.log`

## 推荐工作流

### 第一步：准备运行命令与路径
1. 确认以下四个输入：
   - `--commands-yaml`
   - `--spec-root`
   - `--qemu`
   - `--plugin`
2. 所有这些路径都尽量使用绝对路径。
3. 输出目录使用独立目录，不要覆盖已有批次。

### 第二步：先跑主批次
1. 用 `myScript/run-spec06-insn-freq.py` 跑全量任务。
2. 推荐保留较高并行度，但不要高到让宿主机完全失控。
3. 记录主批次输出目录，例如：
   - `.copilot/inst_freq/output/spec06_ref/`

### 第三步：审查主批次结果
1. 检查 `batch_*.log` 和 `summary.tsv`。
2. 对每个任务看三件事：
   - 返回码
   - CSV 是否存在
   - 运行时间是否至少达到本轮阈值
3. 把以下任务加入 retry 或专项排查列表：
   - 返回码非 0
   - CSV 缺失
   - `elapsed_sec < 阈值`

### 第四步：对失败项做局部 retry
1. 仅选择失败任务再次运行。
2. retry 目录与主批次目录分开保存。
3. 如果某些任务仍失败，不要继续盲目重跑，先定位根因。

### 第五步：专项修复输入准备问题
1. 若失败看起来与 benchmark 输入准备有关，优先查看 `Spec/object.pm`。
2. 常见分支：
   - `sphinx3/default`：生成 `ctlfile` 和标准 `.raw`
   - `wrf/default`：复制正确端序和 header-size 的输入文件
3. 修复后为单任务创建独立验证目录，不要直接混入旧批次。

### 第六步：合并多轮结果并重算最终状态
1. 汇总主批次、retry、专项修复目录的结果。
2. 重算最终状态时继续使用同一判定规则：
   - `rc == 0`
   - `csv_ok == 1`
   - `elapsed >= 阈值`
3. 输出最终有效结果表。

### 第七步：清理无效旧 CSV
1. 找出“文件存在但已被判无效”的历史 CSV。
2. 为每条旧 CSV 指定替代的最终有效 CSV。
3. 后续分析阶段禁止再使用这些旧 CSV。

### 第八步：收集 56 个有效 CSV 并生成矩阵
1. 使用 `myScript/collect-spec06-valid-csvs.py`。
2. 该脚本应：
   - 从最终有效结果表解析 56 个有效 CSV 来源
   - 收集到一个新目录
   - 汇总成统一矩阵文件
3. 矩阵格式：
   - 第一列表头：`mnemonic`
   - 其余列表头：`benchmark/subtask`
   - 某指令在某测试中不存在时填 `0`

## 关键判断分支

### 分支 A：首轮看似成功，但耗时极短
- 判定：假成功。
- 动作：直接加入失败列表，不要拿它的 CSV 做分析。

### 分支 A1：某 benchmark 天生运行很短
- 动作：不要直接放宽全局规则。
- 优先做两件事：
   - 先确认它是否真是稳定的短跑任务
   - 再决定是单独豁免，还是下调整轮阈值
- 若做豁免，必须把豁免任务名和理由写进本轮汇总说明。

### 分支 B：CSV 存在，但返回码非 0
- 判定：失败。
- 动作：查看对应 `.log`，定位 benchmark 自身报错还是输入准备缺失。

### 分支 C：同一任务多次运行结果冲突
- 动作：只保留最后收口时判定为有效的那一份。
- 不要按“哪个目录更早”或“哪个 CSV 已存在”来选。

### 分支 D：SPEC harness 前处理未复现
- 动作：读取 benchmark 的 `Spec/object.pm`。
- 重点查：
  - `post_setup`
  - `invoke`
- 若属于动态生成控制文件、标准化重命名或分层目录搬运，转到对应前处理脚本或新建脚本。

### 分支 E：最终有效结果表里 `csv_path` 为空
- 动作：按 `source/benchmark_subtask_ref_insn_freq.csv` 补全路径。
- 这是主批次结果常见的收口细节。

## 推荐命令模板

### 批量运行
```bash
python3 -u myScript/run-spec06-insn-freq.py \
  --commands-yaml /path/to/spec/cmd/speccpu2006_commands.yaml \
  --spec-root /path/to/spec \
  --qemu /path/to/qemu-riscv64 \
  --plugin /path/to/libriscv_insn_freq.so \
  --out-dir /path/to/output/spec06_ref \
  --jobs 16
```

### 只重跑失败项
```bash
python3 -u myScript/run-spec06-insn-freq.py \
  --commands-yaml /path/to/spec/cmd/speccpu2006_commands.yaml \
  --spec-root /path/to/spec \
  --qemu /path/to/qemu-riscv64 \
  --plugin /path/to/libriscv_insn_freq.so \
  --out-dir /path/to/output/spec06_ref_retry \
  --jobs 8 \
  --task sphinx3/default \
  --task wrf/default
```

### 收集最终有效 CSV 并生成矩阵
```bash
python3 myScript/collect-spec06-valid-csvs.py \
  --summary /path/to/spec06_merged_effective_summary.tsv \
  --collect-dir /path/to/spec06_valid_csvs \
  --aggregate-csv /path/to/spec06_valid_insn_matrix.csv
```

## 完成标准
- 全部 56 个任务都有运行记录。
- 按 `returncode/csv_ok/elapsed>=5s` 重算后，没有遗漏任务。
- 每个测试只保留一份最终有效 CSV。
- 已单独列出无效旧 CSV，避免误用。
- 已收集出 56 个最终有效 CSV。
- 已生成统一指令矩阵，缺失值补零。

## 常见产物解释
- `batch_*.log`：看单轮运行时序和即时判定。
- `summary.tsv`：看某个批次的原始结果。
- `spec06_merged_effective_summary.tsv`：看最终有效结果映射。
- `spec06_invalid_old_csvs.tsv`：看哪些旧 CSV 不能再用。
- `spec06_valid_insn_matrix.csv`：做跨测试指令分布分析的主入口。

## 相关技能
- 若问题集中在 SPEC harness 前处理，优先使用 `spec-harness-input-prep`。
- 若问题集中在单个 benchmark 的失败定位，优先使用 `riscv-spec06`。
- 若问题集中在插件实现和口径验证，优先使用 `qemu-riscv-insn-freq` 或 `qemu-tcg-plugin`。

## 可直接尝试的提示词
- `/qemu-spec06 用当前 qemu 和 plugin 跑一轮 SPEC06，并把失败项筛出来`
- `/qemu-spec06 汇总主批次、retry 和修复目录，按本轮阈值重算最终状态`
- `/qemu-spec06 收集 56 个最终有效 CSV，并生成按指令 x 测试的总表`
- `/qemu-spec06 检查某个批量输出目录里哪些任务是假成功`

## 建议在每轮运行开始前先写清楚的参数
- 本轮短时失败阈值，例如 `5` 秒。
- 本轮输出目录前缀，例如 `spec06_ref`。
- 是否存在已知豁免任务。
- 本轮是否包含专项修复目录。

## 已知可改进点
- 当前 skill 默认目标是 SPEC CPU2006，不包含 CPU2017 等其他 harness。
- 若后续需要长期复用，建议把阈值和目录前缀直接做成 runner 的命令行参数，而不是只写在流程文档里。