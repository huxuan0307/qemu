---
name: qemu-tcg-plugin
user-invocable: true
description: "适用于：在 QEMU 中构建和验证 TCG 插件，尤其是实现 RISC-V 指令频次统计时总结问题、排坑经验、性能取舍和正确性验证方法。"
argument-hint: "要实现或复盘的 QEMU TCG 插件问题/经验主题"
---

# QEMU TCG 插件经验 Skill

## 目标
记录在 QEMU 中实现 RISC-V 指令频次统计插件时遇到的典型问题、可复用的解决方法，以及构建、运行、验证这类 TCG 插件的工程经验。

## 适用场景
- 需要在 QEMU 中新增或审查一个 `contrib/plugins` / `tests/tcg/plugins` 下的 TCG 插件。
- 需要为 RISC-V linux-user workload 做动态统计、追踪或采样。
- 需要复盘一个插件为什么统计不准、运行太慢、输出不稳定或难以批量使用。
- 需要给后续同类插件沉淀一份“少走弯路”的实施经验。

## 核心经验

### 1. 先明确统计语义，不要一开始就写代码
- 最容易出错的地方不是 API 调用本身，而是统计口径。
- 必须先回答：
  - 统计“翻译到 TB 的指令”，还是“真正执行过的指令”？
  - 统计键是 PC、opcode、助记符，还是归一化后的语义类别？
  - 输出口径是 per-vCPU、全局求和，还是按函数/地址聚合？
- 本轮已验证的口径：
  - 统计已执行指令
  - 键为反汇编首 token（助记符）
  - 退出时跨 vCPU 求和输出 CSV

### 2. 插件热路径必须尽量“无锁、无分配、无字符串处理”
- 典型错误做法：
  - 每次执行指令都查哈希表
  - 每次执行指令都拼接/复制字符串
  - 在执行回调里使用全局锁
- 可复用做法：
  - 在 TB 翻译阶段建立助记符到计数器的映射
  - 在执行阶段只做 `inline add`
  - 把字符串提取、哈希建表、排序全部放在冷路径

### 3. 统计插件的推荐结构
1. `qemu_plugin_install`：解析参数、初始化全局状态
2. `qemu_plugin_register_vcpu_tb_trans_cb`：在翻译期遍历指令
3. `qemu_plugin_insn_disas`：只在翻译期提取助记符
4. `qemu_plugin_register_vcpu_insn_exec_inline_per_vcpu(... ADD_U64 ...)`：执行期计数
5. `qemu_plugin_register_atexit_cb`：统一汇总并输出

### 4. QEMU 插件 API 的易错点
- `qemu_plugin_tb_*` 和 `qemu_plugin_insn_*` 的透明句柄不要跨回调保存。
- `qemu_plugin_insn_disas()` 返回的字符串要及时释放。
- 若需要多 vCPU 汇总，优先使用 scoreboard + `qemu_plugin_u64_sum()`，不要自己维护共享原子表。
- 执行回调能不用普通 callback 就不用，优先选 inline 路径。

## 本轮已踩过的问题与结论

### 问题 1：用 TB 粗粒度统计会把“翻译数量”误当成“执行数量”
- 症状：结果明显偏小或与真实总指令数不一致。
- 根因：只在 TB 翻译回调里计数，而不是对每条实际执行指令插桩。
- 解法：必须逐指令注册执行期计数。

### 问题 2：执行热路径若查哈希或加锁，性能会明显劣化
- 症状：插件一开，程序运行时间显著上升。
- 根因：把查表/聚合留在执行回调。
- 解法：
  - 翻译期完成助记符建键
  - 执行期只做 inline 自增

### 问题 3：输出成功不代表统计口径正确
- 症状：CSV 已生成，但无法确定是否真统计到了“执行过的指令”。
- 解法：必须做一个独立参考对比。
- 本轮采用的参考：`tests/tcg/plugins/libinsn.so`
- 关键结论：
  - 自研 CSV 总和与 `libinsn.so` 的 `total insns` 完全相等
  - 这一步是插件正确性验证的关键闭环

### 问题 4：批量使用时，路径包装脚本容易误导
- 症状：单任务脚本能跑，换到 `cmd/<benchmark>` 下批量跑就找不到 QEMU 二进制。
- 根因：包装脚本内部用了相对路径，如 `./build-riscv64-linux-user-plugin/qemu-riscv64`。
- 解法：批量 runner 统一传绝对 `--qemu` 和绝对插件路径，不依赖当前目录。

### 问题 5：返回码为 0 也可能是假成功
- 症状：CSV 存在、返回码为 `0`，但运行时间只有几秒。
- 根因：被测程序自己在早期报错退出，QEMU 仍然退出成功。
- 解法：批量场景必须同时检查：
  - 返回码
  - 运行时间
  - 日志中的 `FATAL` / `STOP` / `Error termination`

## 推荐验证链路

### 第一层：编译验证
1. 插件已接入 `meson.build`
2. 构建能产出 `.so`
3. `qemu-riscv64 -plugin ...` 能正常加载

### 第二层：功能验证
1. 先跑一个小程序，确认 CSV 存在且格式正确
2. 检查表头固定为 `mnemonic,count`
3. 观察是否有明显错误键，如空串、整句反汇编、地址字段混入

### 第三层：口径验证
1. 用同一 workload 同时跑自研插件和 `libinsn.so`
2. 汇总 CSV 总和
3. 对比 `total insns`
4. 两者不相等时，不要继续扩展功能，先修正口径

### 第四层：批量验证
1. 优先用绝对路径运行 QEMU 和插件
2. 针对重定向任务保留 shell 语义
3. 记录每个子任务的 `cwd`、命令、日志和 CSV 路径
4. 不只看返回码，必须审查异常短时退出

## 实施流程
1. 明确统计口径和输出格式。
2. 选一个最近的参考插件作为结构模板。
3. 先做最小可运行版本，只支持一个主路径和一个输出格式。
4. 在翻译期建立键，在执行期只保留 inline 计数。
5. 用小 workload 验证插件能加载和输出。
6. 用 `libinsn.so` 做总量对比。
7. 再接入批量 runner，而不是一开始就上批量。
8. 批量失败后，优先区分：插件问题、路径问题、被测程序输入问题。

## 判断分支

### 分支 A：插件编不过
- 先检查 `meson.build` 接入
- 再检查 `qemu_plugin_version` / `qemu_plugin_install` 是否导出
- 最后核对 `include/plugins/qemu-plugin.h` 中 API 签名

### 分支 B：插件能加载但统计明显不对
- 优先怀疑统计的是翻译期而不是执行期
- 或者执行路径没有逐指令插桩

### 分支 C：插件能跑但非常慢
- 优先排查执行热路径中是否还有：
  - 哈希查表
  - 字符串操作
  - 全局锁

### 分支 D：批量结果很多“成功”但不可信
- 优先看日志和耗时，不要只看返回码
- 被测 benchmark 本身的输入准备错误，比插件自身 bug 更常见

## 完成标准
- 插件可构建、可加载、可输出 CSV。
- 统计结果在至少一个 workload 上与参考总量一致。
- 执行期主路径不依赖全局锁或字符串分配。
- 批量 runner 能稳定传入绝对路径并保留必要的 shell 语义。
- 已记录本次实现中遇到的坑和修复结论，便于后续复用。

## 推荐参考
- `tests/tcg/plugins/insn.c`
- `tests/tcg/plugins/libinsn.so`
- `contrib/plugins/howvec.c`
- `contrib/plugins/hotblocks.c`
- `include/plugins/qemu-plugin.h`
- `contrib/plugins/riscv_insn_freq.c`
- `myScript/run-spec06-insn-freq.py`

## 可直接复用的经验结论
- 逐指令统计必须用执行期插桩，不能只靠 TB 翻译回调计数。
- 对于高频事件，翻译期建模、执行期 inline 计数是最稳妥的主路径。
- `libinsn.so` 是校验“总指令数口径”的高价值参考。
- 批量 SPEC 跑失败时，先怀疑 benchmark 输入准备，其次才怀疑插件本身。