# 净室实现要求

状态：2026-10-21。本文是本仓库的实现约束与验证摘要，不是协议交付包的一部分。

- 已复核的协议输入仍只有 `docs/CLEANROOM_SPEC.md` 与 `docs/CLEANROOM_TEST_VECTORS.json`，校验以 `docs/SHA256SUMS` 为准；本文不改变它们的哈希。
- 本文记录实现层必须遵守的行为和已验证边界。若某条要求超出已复核规格，必须先交回规格侧复核，不能把本文当成新的设备协议来源。
- 历史实验记录、run_id 和逐轮日志在 `ROADMAP.md` 与 git 历史中，不在本文重复。

## 1. 范围

- 设备：`10a5:9201`，`bcdDevice=0x0250`，固件 `021.26.2.050`。
- 平台：Linux libfprint 内置图像驱动；112×88、8-bit、press 模式。
- 传输：USB 控制命令 + 0x82 bulk IN；TLS-PSK；单次采集会话。
- 不覆盖：其他 VID/PID 或固件、DFU、异常复位恢复、`0x64` 语义、厂商二进制/TOD、独立 D-Bus 守护进程。

## 2. 初始化与读取时序

实现必须满足：

1. **在 `CMD_INIT` 之前启动连续的 0x82 bulk reader**，而不是等 TLS 初始化后再读。
2. reader 必须**跨 `CMD_INIT` 后的重枚举持续存在**；句柄失效时换句柄，但不能停止读取或丢弃重枚举窗口内的帧。
3. 重枚举窗口内收到的 `EVT_INIT_RESULT`（`code=2`）必须保留并交付；初始化阶段应能记录到该事件。
4. 保持既有顺序：`0x08 → 0x50 → CMD_INIT(4B) → 重枚举 → GET_TLS_KEY → TLS_INIT → TLS_DATA → ARM → FINGER_DWN → GET_IMG`。
5. 旧句柄的读取应有有界结束（当前实现为 2.5 s grace）；新句柄 claim 后先恢复读取，再完成回调。
6. **不要**为对齐某个失败样本添加固定等待、port reset 或额外设备命令。`CMD_INIT` 的 4 B 载荷不是判别因素，规格允许全零，不得据此要求特定载荷。

验证依据：缺少“提前且持续读取”会单独造成 ARM 后零触摸；已在同一设备上单变量复现。详见 `ROADMAP.md` M6 与 git 历史。

### 2.1 Reader 生命周期（实现契约，与代码一一对应）

传输层独占一条 EP 0x82 bulk IN URB；会话的 `bulk_read()` 只是等待者，不自行提交 URB。

| 阶段 | 行为 | 代码落点 |
|---|---|---|
| 创建 | transport 在返回前启动 reader，因此 `0x08`/`0x50`/`CMD_INIT` 之前已有 URB 在飞 | `fpc_disum_usb_transport_new()` |
| 循环 | 同一时刻 1 条 64 B URB、200 ms 完成超时；OK/timeout 且 `actual>0` 交付或入队，`actual=0` 只唤醒等待者；终态暂停 reader 并保存状态 | `read_start` / `read_submit` / `read_completed` |
| 缓冲 | 无等待者时按 64 块 × 64 B 保存；溢出丢**最新**、保留最早，并计数 | `read_queue_push` |
| 重枚举报废 | `CMD_INIT` 后 `init_rebind(ALLOW)` 进入 reap：不先取消旧 URB，等 reset 让它自然结束；上限 `min(2.5 s grace, reopen deadline)`，到点才 cancel + reap | `reopen_reap_schedule` / `reap_expired_cb` |
| 换句柄 | 仅在旧 URB 真正完成后：释放旧 handle → 重新枚举 → bus + 完整端口链 + VID/PID/bcd 匹配 → `GET_DESCRIPTOR` 探活 → open + claim（只读配置，不发 `SET_CONFIGURATION`） | `reopen_attempt` / `open_and_claim` |
| 恢复读取 | 新 handle claim 后先 `read_start()`，再调用 reopen 完成回调 | `reopen_transfer_completed` |
| 无离场 | 旧 handle 仍能回描述符则保留；reader 重启后再完成 reopen | `REOPEN_PROBE_OLD` 分支 |
| 取消 | control（含 reopen）与 read URB 分别 cancel；只由真实完成回调交付；无 URB 的等待者直接失败 | `fpc_disum_usb_cancel` |
| 销毁 | `torn_down` 阻止回调；取消/回收 control 与 read；等待者不回调直接释放；私有状态由 URB 引用保活到最后一个完成 | `transport_free` / `priv_finalize` |

不变量（不得违反）：

1. `CMD_INIT` 之前 reader 必须已提交。
2. 旧 read URB 未完成前不得换/关句柄。
3. 新 read URB 未提交前不得完成 reopen 回调。
4. 任何路径不得发 `SET_CONFIGURATION`。
5. 队列溢出只允许丢最新，不得丢初始化窗口。

## 3. ARM 与触摸门控

1. **`ARM` 是 `EVT_FINGER_DWN` 的必需前提**；不发送 `ARM` 时不会上报触摸。
2. `ARM` 的 4 B 返回**不是**“已武装”确认；`EVT_ARM_RESULT` 的到达或缺席也不能单独作为武装判据。
3. 手指可以在 `ARM` 之前按住；不存在“必须先 `ARM` 再按压”的要求。提示仍与 `ARM` 同步，用于记录操作时序。
4. 唯一可靠的武装判据是收到 `EVT_FINGER_DWN`；不要用返回长度、固定延时或会话间隔推断成功。
5. 成功采集后紧接的 0 上报样本不足以证明“必须冷却”；接触压力/位置未仪器化，不得写成固定冷却规则。
6. `0x64` 仍未定名，不得在实现中命名或依赖其语义。

## 4. 采集、TLS 与清理

- 图像几何、像素偏移/stride、应用帧头、TLS-PSK envelope 与像素判据以 `CLEANROOM_SPEC.md` §5/§8 为准；本文不复制常量表。
- 单次成功采集应得到 1 个 `code=8 / total_len=9880` 应用帧（12 B 内层头 + 9856 B 像素）。
- 失败、取消、关闭和清理逐条遵守规格 §16：不得把超时、取消、协议失败或安全失败折叠成假成功。
- 取消/关闭必须等待真实完成；close/reopen 后设备必须可再次采集。
- 不保存真实图像、像素摘要、PSK/envelope、TLS 载荷或原始总线全包到仓库、CI 或普通日志。

## 5. 匹配与注册

- **NBIS 不适用于本传感器**：112×88 图像通常只有 1–5 个细节点，低于 `MIN_COMPUTABLE_BOZORTH_MINUTIAE=10` 时 BZ3 直接返回 0；调 `bz3_threshold` 无效。
- 使用 **SIGFM**（OpenCV SIFT + Lowe ratio + 几何一致性），`score_threshold=10`。
- 注册必须启用覆盖门：`enroll_coverage_shift=12` px。新图与已接受模板平移小于该值时提示“移远一点”；完全无重叠时提示“移近一点”；两者都不消耗注册阶段。
- 默认 5 段注册；每段成功后才递增。匹配结果取模板集中任意一条通过阈值，不要求所有模板都通过。
- SIGFM 模板会包含可重建图像的局部特征，按敏感数据处理；不得上传、记录或用于跨设备共享。
- 以上是匹配器的工程选择，不是安全认证；FAR/FRR 结论必须附样本量，不得用 3 次异指试验宣称安全。

## 6. 支持范围

已验证（本机、单设备、少量样本）：

- 初始化、TLS-PSK、ARM、等待触摸、GET_IMG、112×88 图像、取消、取消后再采集、close/reopen 两次采集。
- SIGFM in-process enroll/verify：5 段注册、同指验证、异指拒绝；SIGFM print 的磁盘序列化/反序列化。
- 隔离 fprintd（fprintd 1.94.5，私有 D-Bus、测试 polkit、临时存储）：5 段 enroll、同指 verify-match、左手拇指 verify-no-match、list/delete、daemon 重启后重新加载模板。
- 覆盖引导注册：重复/无重叠图会重试且不消耗阶段。

未验证或未支持：

- 系统 fprintd/PAM/桌面登录端到端；`fprintd-identify` 多模板；多用户存储；suspend/resume 与完整 daemon 生命周期。
- 其他设备/固件、DFU、异常复位恢复、`0x64` 语义、上游 ABI/接受度。
- 生产级 FAR/FRR、活体检测、防伪和多人种/多设备泛化。

## 7. 验证摘要

| 项目 | 结果 |
|---|---|
| 采集/取消/重开 | 5 个场景均完成，无假成功 |
| 覆盖引导注册 | 5/5 |
| 同指 verify | 3/3 `match=true` |
| 异指 verify | 0/3 `match=true` |
| SIGFM print 存储 | 写入/读取后曾成功 MATCH；`fp_print_equal` 的 SIGFM 分支已修 |
| fprintd 无手指冒烟 | 设备枚举、空存储 list/delete、无 print verify、enroll 取消、daemon 重启均通过 |
| fprintd 有手指（隔离，1.94.5） | 5 段 enroll、同指 verify-match（36/10）、左手拇指 verify-no-match（0/10）、list/delete 通过；系统 PAM/桌面未测 |
| 离线用例 | 17 通过、32 跳过、0 失败 |

前四项是 in-process libfprint 结果；fprintd 项使用私有 D-Bus、测试 polkit 和临时存储，不是系统 PAM/桌面认证结论。复现命令与 run_id 见 `ROADMAP.md`。

## 8. 变更控制

- 规格和向量是冻结输入；修改它们必须重新隔离复核并更新 `SHA256SUMS` 与构建期哈希。
- 本文可以随实现更新，但新增设备行为要求必须先回到规格侧确认。
- 任何“支持”声明都必须同时写明设备、固件、fprintd 版本、验证范围和未支持项。
