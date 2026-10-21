# M6 支持范围与验收声明

日期：2026-10-21。适用于本仓库 `fpc_disum` 驱动及本次 fork 的核心 USB 重取扩展。

## 1. 结论

在下列**声明范围内**，M6 的受控真机验收已完成：单一设备、单次采集、无手指负例、取消、取消后再采集、close/reopen 两次采集均已由运行证据证明，且仓库不存在已知的假成功路径。

这不等于上游 libfprint 接受该驱动，也不等于 PAM/fprintd/桌面登录可用。M6 本身仍不声明录入/模板匹配；M7.2 的 SIGFM in-process enroll/verify 已在探针中验证，见 §6。

## 2. 声明范围

- 设备：`10a5:9201`，`bcdDevice=0x0250`（FPC Sensor Controller L:0001，FW 021.26.2.050）。
- 平台：Linux，本仓库的内置 libfprint 驱动（`FpImageDevice`）。
- 输入：本仓库交付的 `docs/CLEANROOM_SPEC.md` 与 `docs/CLEANROOM_TEST_VECTORS.json`。
- 能力：初始化（含 CMD_INIT 后重枚举）、TLS-PSK 握手、ARM、等待触摸、GET_IMG、单张图像、§16.3 清理；无手指窗口到期；取消；取消后再采集；同一 `FpDevice` 的 close/reopen 两次采集。
- 不声明：一次会话内第二次采集、fprintd/PAM/桌面登录、Windows 行为、其他 VID/PID 或固件、DFU、异常复位后的安全恢复、`0x64` 语义、上游 ABI/接受度。M7.2 的 SIGFM in-process enroll/verify 单独列在 §6。

## 3. 真机验收证据

全部为本机 `_build/device/tests/fpc-disum-hw-probe` 的去敏 run；图像只在内存在检查，未保存像素或摘要。

| 场景 | run_id | 结果 |
|---|---|---|
| 单次采集 | `93bd3886-8074-4109-be17-1adeb51fa00d` | 初始化 5 帧/247 B（含 `EVT_INIT_RESULT`）；17 帧/10,878 B；112×88/9856 B；exit 0 |
| 无手指负例 | `63808bb3-6c28-492e-8fbc-1bc297d2b0d2` | `no-touch`；cleanup 完成；无图像；exit 0 |
| 取消 | `af7a0146-b185-4f72-b688-ddcd3723ace8` | `cancelled`；cleanup 完成；无图像；exit 0 |
| 取消后再采集 | `0361489d-1389-46a3-bcd6-0f762bca8791` | 首轮 `cancelled`、次轮图像 112×88/9856 B；两轮 cleanup 完成；exit 0 |
| close/reopen 两次采集 | `efe74e89-a7c0-453b-a348-efb626e28ce5` | 两轮图像各 112×88/9856 B；`image_distinct=true`；两轮 cleanup 完成；exit 0 |

负例与取消不需要操作者按压；采集轮的操作者按压均与 ARM 提示对齐，但压力/接触面积未仪器化。成功轮不以末轮结果覆盖前轮，所有轮次独立记录。

## 4. 离线与质量门

- 四个构建配置（llvm / gcc / sanitizers / device）：`cleanroom` 4/4；驱动与核心 12/12。
- `test-fpc-disum-usb --smoke-output-dir`：`frames=1 code=6 total_len=12 partial=0`，exit 0。
- sanitizer：ASan + 全量 UBSan，`halt_on_error=1`，无泄漏报告。
- 规格基线：`docs/CLEANROOM_SPEC.md` = `b8feda4fc944d41c78347d5d08927ae86bcec38da35002360666c6bf20387b00`；向量 = `ce04ce43665f2b2d27142f2c2ecd917b09219a87a82d4b3009c023baecfeda94`；`sha256sum -c docs/SHA256SUMS` 通过。
- 上游：`v1.94.100`，commit `80a4b5ec612892c5c056c48dddaa561452cf37ec`。
- 驱动在配置期绑定已复核规格哈希（config.h），探针在 open 前核对加载中的驱动属性与固定基线。

## 5. 已知偏离与工程约定

- 本 fork 在核心 `fp-context.c`、`fpi-device.[ch]`、`fp-device-private.h` 增加了显式 USB 初始化重取事务；该语义只对 CMD_INIT 初始化窗口生效，其他驱动保持正常移除语义。
- 驱动使用私有 libusb-1.0 异步后端；后端在工厂阶段启动连续 bulk reader，跨重枚举保留数据。控制传输与读并发。
- `reopen` 等待旧读自然结束，最多 2.5 s 后强制取消并等待真实完成；这是收敛上限，不是设备门槛。
- `EVT_ARM_RESULT` 的保留值 `0x64` 仍按规格立即失败，语义仍未定。
- 异常复位后的自动恢复不实现；对已记录异常的会话再次 init/capture 返回 `unsupported-reset-recovery`。
- 未安装/未替换系统 libfprint、未启用 PAM/fprintd、未做系统部署实验。

## 6. 净室与产物卫生

- 实现只使用仓库内交付的规格与人工向量；未读取研究侧仓库、专有二进制或原始采集材料。
- 真机证据写入 `~/fpc-disum-runs/` 的 0700 目录、0600 文件；不保存真实图像、像素摘要、PSK/envelope、TLS 载荷或原始总线全包。
- M7.2 之后 `meson test -C _build/device --no-suite data`：17 通过、32 跳过、0 失败；SIGFM 单元测试受 doctest 可选依赖控制。
- [`ARM_GATE_NOTE.md`](ARM_GATE_NOTE.md) 与 [`INIT_READ_GAP_FINDING.md`](INIT_READ_GAP_FINDING.md) 是净室侧补充说明，不改变交付包与 `SHA256SUMS`。

## 5. 复现入口

```sh
uv tool run --from meson==1.12.1 meson test -C _build/device \
  --suite cleanroom --print-errorlogs
uv tool run --from meson==1.12.1 meson test -C _build/device \
  fpc-disum-protocol fpc-disum-tls fpc-disum-channel fpc-disum-session \
  fpc-disum-error fpc-disum-usb fpc-disum-probe fp-context-usb \
  fpi-device fpi-ssm fp-context fp-device --print-errorlogs

# 真机；每次运行使用新目录。
LD_LIBRARY_PATH=_build/device/libfprint \
  _build/device/tests/fpc-disum-hw-probe negative --output-dir "$HOME/fpc-disum-runs/<new>"
# capture / cancel / cancel-reopen / reopen 同理。
```

真机运行仍需操作者在设备旁；采集轮必须等 ARM 提示后按压。负例、取消与取消后再采集的第一轮不需要按压。

## 6. M7.2 增量：SIGFM 与覆盖引导注册（2026-10-21）

在 M6 采集能力之上，`fpc_disum` 改用 SIGFM（OpenCV SIFT），`score_threshold=10`，并增加 `enroll_coverage_shift=12`：太近的重复图提示移远，完全无重叠提示移近，二者都不消耗注册阶段。

真机探针结果（同一设备、单操作者、少量样本）：

- 覆盖引导注册：5/5 成功；
- 同指 verify：3/3 `match=true`（分数 141/20/1085）；
- 异指 verify：0/3 `match=true`（3/3 拒绝）；
- `examples/enroll` 写盘 → `examples/verify` 反序列化：曾成功 MATCH；随后修复 `fp_print_equal()` 的 SIGFM 分支。

边界：这是 in-process libfprint 探针，不是 fprintd/PAM 端到端，也不是安全认证；FAR 样本只有 3 次异指尝试。fprintd daemon、PAM 登录、identify 多模板、多用户存储和打包回归仍按 M7.3/M7.4/M8 验证。
