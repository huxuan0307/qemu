---
name: qemu-riscv-insn-freq
user-invocable: true
description: "适用于：在 QEMU 中实现/审查基于 C 的 RISC-V linux-user TCG 指令频次统计插件，并输出 CSV。"
---

# QEMU RISC-V 指令频次统计 Skill

## 目标
提供一套可复用、低开销的实现流程：在 QEMU 中编写 C 语言 TCG 插件，按助记符统计 RISC-V 指令执行频次，并输出 CSV 结果。

## 适用场景
- 需要为 RISC-V linux-user 工作负载新增 TCG 插件。
- 需要按助记符统计频次（如 addi、lw、jal）。
- 需要将结果写入可复现的 CSV 文件用于离线分析。
- 需要从正确性和性能角度审查插件实现。

## 统计范围
- 目标模式：RISC-V linux-user。
- 统计键：反汇编字符串首个 token（助记符）。
- 输出格式：CSV，表头固定为 `mnemonic,count`。
- v1 不覆盖 system-mode 专有语义（如物理地址口径等）。

## 工程规则
1. 执行语义
- 统计“已执行指令”，而不是“仅被翻译过的指令”。
- 必须使用逐指令插桩，不使用 TB 粗略近似。

2. 插件 API 约束
- 导出 `qemu_plugin_version` 与 `qemu_plugin_install`。
- 通过 `qemu_plugin_register_vcpu_tb_trans_cb` 注册翻译回调。
- 在 TB 回调内遍历 `qemu_plugin_tb_n_insns` 与 `qemu_plugin_tb_get_insn`。
- 使用 `qemu_plugin_insn_disas` 提取助记符，并及时释放临时字符串。
- 执行热路径优先使用 `qemu_plugin_register_vcpu_insn_exec_inline_per_vcpu(... ADD_U64, ..., 1)`。

3. 数据结构
- 使用 `GHashTable`，键为助记符字符串。
- 每个键对应独立 scoreboard 计数器。
- 可保留样例反汇编用于调试，但不参与统计逻辑。

4. 并发与生命周期
- 翻译阶段对哈希表增查改必须加 `GMutex`。
- 执行阶段避免全局锁与全局计数写入，依赖 per-vCPU inline 计数。
- 不跨回调保存 TB/Insn 透明句柄。

5. 输出契约
- 必须支持 `outfile=<path>`。
- CSV 表头固定为 `mnemonic,count`。
- 文件写失败时，必须回退到插件输出并打印清晰错误。

6. 风格约束
- 遵循 `contrib/plugins` 与 `tests/tcg/plugins` 既有代码风格。
- 参数解析使用 `key=value`。
- v1 不引入复杂 ISA 语义归并与解码器。

## 关键参考
- `tests/tcg/plugins/insn.c`：逐指令插桩模板。
- `contrib/plugins/howvec.c`：哈希聚合与汇总输出模板。
- `contrib/plugins/hotblocks.c`：锁与 scoreboard 使用模板。
- `include/plugins/qemu-plugin.h`：插件 API 语义。
- `docs/about/emulation.rst`：插件命令行示例。
- `myScript/run-qemu-plugins.sh`：本地 RISC-V 插件运行脚本。

## 实施步骤
1. 建立插件骨架
- 新增 C 源文件，导出版本符号与安装入口。
- 解析参数并设置默认值（至少 `outfile`，可选 `topn/sort/inline`）。

2. 建立聚合结构
- 定义记录结构：助记符 + `qemu_plugin_u64` 计数器。
- 在 install 阶段初始化哈希表与互斥锁。

3. 插桩翻译块
- 在 TB 翻译回调中遍历所有指令。
- 提取助记符并执行归一化（首 token）。
- 为新助记符创建记录；旧助记符复用记录。
- 在该指令上注册 per-vCPU inline 自增。

4. 退出汇总与输出
- 在 atexit 回调中遍历记录。
- 用 `qemu_plugin_u64_sum` 汇总并写入 CSV。
- 可选排序和 TopN 输出控制。

5. 构建接入
- 将插件文件加入目标 `meson.build`（`tests/tcg/plugins` 或 `contrib/plugins`）。
- 构建生成共享库 `.so`。

6. 运行验证
- 先跑可手算的小 RISC-V 程序进行正确性核对。
- 再跑真实 workload，检查键拆分和异常计数。

7. 性能验证
- 若支持 `inline=false` 调试路径，比较两种模式结果一致性。
- 默认保持 `inline=true` 作为主路径。

## 验收清单
- 插件可编译、可加载（`-plugin`）。
- CSV 输出存在、表头正确、行格式正确。
- 在确定性 workload 上统计结果稳定可复现。
- 无明显临时字符串泄漏。
- 执行热路径无锁竞争热点。

## v1 非目标
- 退休指令语义扩展统计。
- 助记符别名/压缩指令语义归并。
- 跨架构统一口径处理。
