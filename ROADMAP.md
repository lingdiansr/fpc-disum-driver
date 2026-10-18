# FPC Disum libfprint 内置驱动 ROADMAP

## 目标与已选路线

在 libfprint 源码树内独立实现 `10a5:9201 / bcdDevice=0x0250` 的 Linux **单次图像采集**驱动，覆盖初始化、安全通道、事件触发采图以及失败/取消/关闭。不是独立守护进程，也不复刻 Windows 匹配引擎。

- **Architecture**：沿用上游 `FpImageDevice`、GLib 主循环、`FpiSsm`、`FpiUsbTransfer`/GUsb；目标设备协议仅来自净室交付包。TLS 使用进程内 OpenSSL memory BIO，不增加网络服务或 OpenSSL 子进程。
- **Tech stack**：保留上游 GNU C99/C++、GLib/GObject/GIO、GUsb 和 Meson/Ninja；LLVM/LLD 为日常开发工具，ASan/UBSan 与定向 clang-tidy 为质量检查，GCC 为兼容构建。密码学阶段接入 OpenSSL 3；Python 3.11+ 仅用于基线检查和测试驱动。
- **上游基线**：官方 `https://gitlab.freedesktop.org/libfprint/libfprint.git`，tag `v1.94.100`，commit `80a4b5ec612892c5c056c48dddaa561452cf37ec`。保留上游 `COPYING` 和原文件版权，沿用 LGPL-2.1-or-later。
- **当前交付范围**：M0 源码树初始化与 M0.1 质量工具链；不注册没有实现的 `fpc_disum` 驱动，不伪造采集成功。

## 唯一设备协议输入

| 文件 | 固定 SHA-256 |
|---|---|
| [CLEANROOM_SPEC.md](docs/CLEANROOM_SPEC.md) | `f609db776be1fb340a7ee399d6bdc852a455ebc25d53ddfe941c9760cd0b341a` |
| [CLEANROOM_TEST_VECTORS.json](docs/CLEANROOM_TEST_VECTORS.json) | `ce04ce43665f2b2d27142f2c2ecd917b09219a87a82d4b3009c023baecfeda94` |

上游代码只提供通用框架、API、算法和编码惯例；其中其他设备的驱动、公开录制样本和图像不是本传感器的协议依据。不得访问研究侧仓库、真实 PSK/图像、专有二进制、抓包或反编译产物来补实现。

`IMPLEMENTATION_REVIEW.md` 为历史评审记录，不覆盖本表基线。规格发现缺漏或规范冲突时只记录现象与缺失条件，交回维护方修订并复核；不自行猜测设备行为，也不修改交付包来迎合代码。

## 范围与硬边界

**纳入**：单次采图、112×88 灰度像素、初始化/句柄迁移、TLS-PSK、分帧与应用消息解析、取消/超时/错误及资源回收、合成输入与离线状态机验收。

**不纳入**：设备侧 `ARM wValue=2`、`END_ENROL` 驱动流程、完整抬指/重按录入生命周期、reset/设备重现后的自动恢复认证、DFU、其他 VID/PID 或固件、Windows 模板/算法和 Windows Hello 协议。

真实采集、取得设备密钥或改变设备状态的实验必须另获明确授权。本轮的构建和测试不读取或控制目标传感器；默认开发配置只编译上游虚拟驱动。不得安装到系统、替换系统 libfprint、改动 fprintd/PAM/udev 或修改 USB 电源策略来证明初始化成功。

**命名（2026-10-18 决定）**：项目/仓库标签与驱动同名 —— 仓库 `fpc-disum-driver`，驱动、模块、测试与注册 ID 一律用**设备族**名 `fpc_disum` / `fpc-disum`。依据：目标设备的厂商标识是 **Fingerprint Cards AB**（§1：VID `10A5`），本侧只覆盖这一颗传感器与固件 `0x0250`；上游 libfprint 按传感器族命名驱动（`fpcmoc`、`egismoc`、`goodixmoc`、`elanmoc`、`focaltech_moc`…），不存在按整机品牌命名的先例；规格（冻结）也以 “FPC Disum” 称呼该设备，同名便于逐条对照。被验证的那台机器是小米笔记本（设备为其内置传感器），这只作为**来源说明**，不进任何标识符。

按上游惯例逐层落定（供 M5 照此填写）：

| 层 | 上游惯例（实例） | 本项目取值 |
|---|---|---|
| 驱动目录 | `libfprint/drivers/<driver>/`（`egismoc/`、`elanmoc/`、`realtek/`） | `libfprint/drivers/fpc_disum/` ✅ |
| 驱动 ID | `#define FP_COMPONENT "<driver>"` → `dev_class->id`（`fpcmoc`、`focaltech_moc`） | `fpc_disum` ✅ |
| 人读名 | `dev_class->full_name` 用“厂商 + 产品”（`"FPC MOC Fingerprint Sensor"`、`"Elan MOC Sensors"`、`"Focaltech MOC Sensors"`） | `"FPC Disum Fingerprint Sensor"`（M5 填） |
| 主源文件 | `<driver>.c`（`egismoc.c`、`elanmoc.c`）或缩写（`fpcmoc`→`fpc.c`、`goodixmoc`→`goodix.c`） | `fpc_disum/fpc_disum.c`（M5） |
| 次级模块 | 按角色命名、设备前缀（`goodix_proto.c`、`bmkt_message.c`） | `fpc_disum_proto/tls/session/usb.c` ✅ |
| 设备级测试变体目录 | `tests/<driver>-<PID>/`（`egismoc-0586`、`focaltech_moc-6553`、`realtek-5816`，后缀为 USB PID） | `tests/fpc_disum-9201/`（仅在需要设备级仿真数据时） |
| 仓库/项目标签 | **上游无此层**（只有 `libfprint` 一个仓库） | `fpc-disum-driver`（与驱动同名）✅ |

小米只作为**来源说明**存在（“目标设备出现在某台小米笔记本上”），不进入任何标识符；驱动侧名字表达的是“支持哪颗传感器”。

## 分层与代码落点

后续实现跟随上游目录约定，不另建平行驱动框架：

| 责任 | 代码落点（在对应里程碑创建） | 依赖与边界 |
|---|---|---|
| 外层/应用消息解析 | `libfprint/drivers/fpc_disum/fpc_disum_proto.c`、`fpc_disum_proto.h` | 不访问 USB，不命名未知字段；保留第三字段和未消费字节 |
| envelope 认证与 TLS | `libfprint/drivers/fpc_disum/fpc_disum_tls.c`、`fpc_disum_tls.h` | OpenSSL EVP/BIO；不得自己实现密码学或降级到未约定布局 |
| 传输边界与会话状态机 | `fpc_disum_transport.h`、`fpc_disum_session.c`、`fpc_disum_error.c` | 不依赖 `FpDevice`；一次操作恰好一个回调，调用方决定超时，超时/取消/清理按 §16 |
| 图像驱动适配层 | `libfprint/drivers/fpc_disum/fpc_disum.c` | `FpImageDevice` 入口 + 传输/会话所有权；只把会话结果翻译成核心调用，不含协议逻辑 |
| 原生验收 | `tests/test-fpc-disum-protocol.c`、`test-fpc-disum-tls.c`、`test-fpc-disum-session.c`、`test-fpc-disum-error.c` | 执行真正的 C 实现；人工向量重建器只构造输入，不代替被测实现 |
| 构建注册 | 根 `meson.build`、`libfprint/meson.build`、`tests/meson.build` | 已在 M5 注册 `fpc_disum`（目录/ID/`FP_COMPONENT`/`full_name` 见「命名」表）；驱动 ID 未做真机验证前不宣称支持 |

每个模块明确输入长度、缓冲所有权、未消费尾部和错误阶段。优先使用借用视图与有界缓冲，避免为每个 USB 包或应用帧进行不必要分配和复制。

## M0：工程初始化（已完成）

- [x] 引入固定 commit 的上游 libfprint，建立本地 Git 开发分支，保留净室文件及历史评审。
- [x] 提供独立的固定哈希检查命令；文件被改动、文件缺失或校验清单被同步改写时均失败。
- [x] Meson 配置时执行净室基线检查，并将其完整性回归检查加入 `cleanroom` 测试组。
- [x] 使用虚拟驱动配置构建真实的 libfprint 库及测试程序，不注册或声明支持 `10a5:9201`。
- [x] 运行基线检查、完整性负例和不依赖录制指纹图像的原生框架测试，记录实际结果。

**完成判据**：新开发者能按 README 完成配置、编译和上述离线检查；不需要 root，不触发真实设备，不修改系统安装。本轮结束时只把实际完成的条目标记为完成。

**本轮实际验收**：Meson 1.12.1、GCC 16.2.1，debug 配置，只编译三个虚拟驱动，显式关闭可选 GI 绑定。配置及库/示例/测试程序编译成功；`cleanroom` 两个测试目标全部通过，`fpi-device`、`fpi-ssm`、`fp-context`、`fp-device` 四个原生测试目标全部通过。独立命令确认两份已复核输入匹配固定哈希，缺失输入返回失败。未执行录制图像用例或真实采集。

无 GI 配置暴露了上游 `tests/meson.build` 的字典遍历变量错误；本侧将该分支改成与正常分支一致的键、值双变量遍历，配置由失败转为成功，没有修改测试用例行为。


## M0.1：LLVM 质量工具（已完成）

- [x] 修复已复现的虚拟图像驱动与原生测试回调签名，不改变公共/内部 API，不通过强转或关闭 sanitizer 掩盖问题。
- [x] 使用 `dev/native/common.ini`、`llvm.ini`、`gcc.ini`、`sanitizers.ini` 定义三套独立 Meson 构建；保留 GNU C99 和原有虚拟驱动安全范围。
- [x] `.clangd` 使用 LLVM 开发构建的编译数据库；`.clang-format` 仅复刻 GNU/uncrustify 风格以约束自动格式化，uncrustify 仍是唯一权威格式规范。
- [x] `.clang-tidy` 及 `.clang-tidy-include` 检查新增驱动代码和本轮修正的源文件；使用 Meson 生成的 Ninja 原生分析目标，不自动应用修复。
- [x] sanitizer 测试默认在首个 UBSan 错误处失败，保留全部 `address,undefined` 检查，不增加 suppression。
- [x] LLVM、严格 sanitizer、GCC 三套构建均运行相同的 2 个 cleanroom 和 4 个原生测试目标；定向静态分析返回成功。

**完成判据**：先前会触发 UBSan 的回调路径在现有生命周期用例下通过；三个构建目录不混用编译器/插桩产物，开发命令和实际验收一致。M1 起增加了协议解析测试，M0.1 的各项门禁随之覆盖它们。

**本轮实际验收**（Meson 1.12.1；数字为该里程碑当时的测试集合，M1 新增测试后总数见 M1 记录）：

| 配置/检查 | 结果 |
|---|---|
| LLVM/LLD 23.1.1，`_build/llvm` | 配置、构建成功，6/6 指定离线测试目标通过 |
| LLVM 23.1.1 + ASan/UBSan，`_build/sanitizers` | 配置、构建成功，严格模式下同一组 6/6 目标通过，未关闭函数类型检查 |
| GCC 16.2.1/BFD，`_build/gcc` | 配置、构建成功，同一组 6/6 目标通过 |
| clang-tidy | 配置有效，Ninja 内置目标及直接指定的三个维护中编译单元分析成功 |
| clangd | 自动读取 `_build/llvm/compile_commands.json`，虚拟图像驱动检查完成、0 errors |

修复前，严格 UBSan 在虚拟 open/close、SSM 数据析构和 identify 超时测试中报告了不兼容回调；修复采用精确签名的静态适配器和通用参数类型，保留时序、所有权与原有行为断言。上游 C99 重复 typedef 编译告警仍可见，本记录不宣称全库 warning-free。


静态门禁另以临时的空指针解引用输入做过负例烟测：`clang-analyzer-core.NullDereference` 被提升为错误，clang-tidy 返回 1；临时文件已移除。该检查验证诊断会导致失败，不向仓库加入故意错误的源码或空测试入口。

## M1：纯协议解析（已完成）

- [x] 实现大端 12B 外层头；收到前 8B 立即检查 `12..65536` 长度，支持跨包、多帧、空读与超时保留部分输入。
- [x] 未知 code 和第三字段原样交付；非法长度/不完整尾部不静默丢弃、不逐字节重同步。
- [x] 应用分帧与 capture 接受检查分离：分帧可交付 22B 消息，capture 只接受 `code=8 / total_len=9880`。
- [x] 按 `12B 帧头 + 12B 内层头 + 9856B 像素` 定位数据；stride=112、8-bit、无旋转/翻转/转置。

**完成判据**：通过 10 条外层帧和 2 条应用消息向量；原生 C 测试额外证明非法输入不会越界读写、错误后不会伪报完整图像。五条像素向量按附录 B 重建并校验，相关性仅作结构筛查。

**本轮实际验收**：

| 项 | 结果 |
|---|---|
| `tools/fpc_disum_vectors.py --verify` | 22/22 向量从声明式描述重建并与录制期望一致（10 外层帧、2 应用消息、5 像素、5 envelope 结构） |
| `test-fpc-disum-protocol` | 19 个用例：7 个边界/生命周期 + 10 个外层帧向量 + 2 个应用消息向量 |
| 三套构建 | LLVM、严格 ASan/UBSan、GCC 下 `cleanroom` 4/4 与原生 5/5 全部通过 |
| 变异验证 | 长度上界改为 `>=` ⇒ 长度边界用例失败；像素偏移改为 12 ⇒ 图像定位用例失败；去掉缓冲区上限检查 ⇒ 新增长度上限用例失败（均已还原） |
| 静态分析 | 定向 clang-tidy 覆盖 `fpc_disum_proto.c` 与测试，无启用规则诊断 |

独立审查后补齐：`feed()` 拒绝无法由 `GByteArray` 表示的超大读取（原先会被 `guint` 静默截断并报成功）；重建器要求固定 22 个向量 ID 完整存在、逐项比对 envelope 的六个结构字段、要求像素期望五个字段齐全；语料目标把交付 JSON 声明为构建输入，向量文件变化会触发重新生成。

实现要点：`fpc_disum_proto.c` 在 8 字节即可判定长度，非法长度保留全部已收字节并停止推进（不再消费后续输入）；分帧层只校验 12B 头，capture 层再用 `fpc_disum_frame_get_image()` 要求 `code=8 / total_len=9880` 且 24B 前缀与像素面精确闭合。

像素向量的五条指标（重叠区 Pearson、截断量化、三位小数）由重建器按附录 B 计算并比对；驱动不实现该结构筛查，也不把它当作指纹质量判据。

语料由构建时生成（`tests/fpc-disum-corpus.c`），C 测试执行真实解析器；`tests/test-cleanroom-vectors.py` 用篡改指标、哈希、拒绝阶段、未知 kind、非 synthetic 文档等负例保证重建器不会空过。为避免编辑器把新文件改成别种风格，新增根 `.clang-format` 复刻 GNU/uncrustify 风格，uncrustify 仍是唯一权威格式规范。

fuzzing 未纳入本轮门禁：解析器已可用于语料播种，接入 libFuzzer 需单独构建配置，列为后续工作。

## M2：envelope 与密码学拒绝路径（已完成）

- [x] 从 7 个小端 u32 读取字段，不硬编码有效样例的偏移；未定名的 16B 区间不赋予语义。
- [x] 按当前规格执行 `长度/magic → 字段范围 → AES → HMAC`，范围检查使用不会整数溢出的形式。
- [x] 采用约定的 NUL 结尾串派生密钥、AES-256-CBC NoPadding 和完整 HMAC-SHA256；只有完整验签成功才允许将 PSK 用于会话。
- [x] 为拒绝输入报告规定的第一个失败阶段；不把 `field-bounds` 失败变成后续 HMAC 失败，也不尝试短签名/72B 布局。

**完成判据**：5 条 envelope 向量通过；移除范围检查后，新越界向量必须因 `reject_stage` 不符而失败。日志、测试失败信息不输出真实 PSK。

**本轮实际验收**：

| 项 | 结果 |
|---|---|
| `test-fpc-disum-tls` | 14 个用例：9 个边界/生命周期 + 5 个 envelope 向量 |
| 三套构建 | LLVM、严格 ASan/UBSan、GCC 下 `cleanroom` 4/4 与原生 6/6 全部通过 |
| 变异 1（完成判据） | `range_within()` 改为恒真 ⇒ `signature-offset-out-of-range-rejected` 的阶段变成 `hmac`，按 `reject_stage` 断言失败 |
| 变异 2 | 去掉 `EVP_CIPHER_CTX_set_padding (ctx, 0)` ⇒ 有效向量在 AES 阶段被拒（status=4），复现规格警告的 OpenSSL 默认填充问题 |
| 静态分析 | 定向 clang-tidy 覆盖 `fpc_disum_tls.c` 与测试，无启用规则诊断 |

实现要点：7 个小端 u32 全部由头部切片；未定名区间不做任何解释（测试翻转该区间后仍接受且 PSK 不变）；HMAC 覆盖 `AAD ‖ 密文` 并用 `CRYPTO_memcmp` 常量时间比较；派生密钥、摘要与 PSK 全部用 `OPENSSL_cleanse` 擦除；任何失败都清空调用方缓冲并把 `psk_len` 置 0；不打印 PSK。envelope 只保证 key 区间按 16 字节对齐，调用方（M3）需自行要求 `psk_len == 32`。

独立审查后补齐：清零断言改用布尔形式，避免失败日志输出残留密钥字节；有效向量必须声明 `psk_b64`（删除该字段现在会被独立验证拒绝，并新增回归用例）；新增“仅翻转签名末字节”用例以直接验证完整 32 B 比较；NULL envelope 失败路径也断言输出被清零。

## M3：TLS 内存通道（已完成）

- [x] 以宿主为 TLS server、设备为 client，使用 TLS 1.2 和 `PSK-AES128-CBC-SHA256`，验证 `Disum PSK` identity。
- [x] 配置非空 session_id；握手由 OpenSSL 状态驱动，不把观测到的六次发送硬编码成握手算法。
- [x] 经 memory BIO 消费 `EVT_TLS` payload，向 USB 层交付不超过 64B 的片段；USB 包边界、事件帧边界和 TLS 记录边界互不混同。
- [x] Finished/MAC、alert、无进展超时与取消均有独立失败路径；握手未完成时禁止 ARM/GET_IMG。

**完成判据**：使用人工 PSK 与本地合成 TLS client 完成握手和应用数据往返；分段、错误 PSK、alert、取消和超时不泄漏状态，不访问真实设备。

**本轮实际验收**：

| 项 | 结果 |
|---|---|
| `test-fpc-disum-channel` | 10 个用例：握手、1 字节分片握手、身份不符、PSK 不符、握手期 fatal alert、部分记录后补齐、握手期取消、PSK 长度、失败后拒绝输入、应用数据往返 |
| 三套构建 | LLVM、严格 ASan/UBSan、GCC 下 `cleanroom` 4/4 与原生 7/7 全部通过（sanitizer 覆盖中途取消的泄漏检查） |
| 变异 | 身份比较恒真 ⇒ `rejects-wrong-identity` 失败；（session 缓存与 session_id_context 的变异未改变 OpenSSL 3.6 的可观察行为，说明非空 session_id 由库默认产生，断言保留为对可观察契约的固定检查） |
| 静态分析 | 定向 clang-tidy 覆盖 `fpc_disum_tls.c` 与新测试，无启用规则诊断 |

实现要点：通道是被动的——不阻塞、不自己计时；调用方决定无进展超时，直接释放通道即为取消。握手完全由 `SSL_do_handshake` 状态驱动，不硬编码飞行次数；`feed()/take_output()` 不感知 USB 包或外层帧边界，记录可任意切分，输出按调用方给的 64 B 上限分片。PSK 必须为 32 B；失败后拒绝输入并保留可读诊断（`error_text`，不含密钥）；释放时擦除 PSK 拷贝。PSK 身份不符单独报告为 `ERROR_IDENTITY`，其余 TLS 层失败（alert、Finished/MAC、其他协议错误）归为 `ERROR_HANDSHAKE`，BIO 故障为 `ERROR_IO`。

## M4：USB 与单次采集状态机（已完成）

- [x] 在 GUsb/FpiUsbTransfer 的生命周期内实现 §6；`CMD_INIT` 后关闭/重开句柄、重新 claim 和重建 reader，再执行 GET_TLS_KEY/TLS_INIT。
- [x] 目标配置已生效时不再发送 SET_CONFIGURATION；不得以端口 reset 或固定延时“修好”初始化。
- [x] `EVT_INIT_RESULT` 只作通知，不作为额外门控；等到 `EVT_FINGER_DWN` 后才发 GET_IMG。
- [x] 不依赖一次采集恰好 11/12 个 TLS 帧，按解密流和消息边界完成图像。
- [x] 未知事件失败、已知无关事件忽略；未知第三字段不猜语义，`EVT_ARM_RESULT(value=0x64)` 按约定失败。

**完成判据**：合成传输事件驱动真实会话状态机，验证命令编码、顺序、握手门控、缺触摸不发 GET_IMG 和错误阶段。真机验证单独授权，不用它替代可确定性的离线验收。

**本轮实际验收**：

| 项 | 结果 |
|---|---|
| `test-fpc-disum-session` | 18 个用例：§6 命令顺序（含 `CMD_INIT` 4B 全零载荷与换句柄）、`EVT_INIT_RESULT` 不门控、握手无进展超时、envelope 被拒、换句柄失败、PSK 不符、采集命令顺序与像素逐字节比对、缺触摸不发 `GET_IMG`、无关事件记录并忽略、未知事件失败且第三字段原样保留、`0x64` 失败、分段无关性（单帧／TLS 字节级分帧／帧跨 bulk 包切分）、无图超时、USB 失败即时分类、非图像应用消息拒绝、等待触摸与初始化两个取消点、清理首因保留、清理失败上报 |
| 三套构建 | LLVM、严格 ASan+UBSan、GCC 下 `cleanroom` 4/4 与原生 8/8 全部通过 |
| 泄漏检查 | 同工具链的故意泄漏可被报出（LeakSanitizer 生效），会话用例零报告 |
| 变异 | 去掉 `CMD_INIT` 后换句柄、跳过等待触摸直接 `GET_IMG`、等待触摸时不设窗口：三者均使对应用例失败 |
| 静态分析 | 定向 clang-tidy 覆盖会话、传输与新测试无启用规则诊断；clang-format 干净 |

实现要点：传输边界 `fpc_disum_transport.h` 规定“一次操作恰好一个回调、每次至多一个操作在途”，会话与设备之间没有线程、没有阻塞等待；`fpc_disum_usb.c` 是它在 GUsb/FpiUsbTransfer 上的实现（控制 OUT/IN、0x82 bulk 读、`CMD_INIT` 后关句柄→重开→读当前配置→必要时才 `SET_CONFIGURATION`→claim interface 0，重试有界且可取消）；`fpc_disum_session.c` 是 §6/§8/§4.5/§16 的状态机（显式状态 + 单步循环，同步完成回调不会堆栈递归）。超时：握手 5 s（每次进展重置）、换句柄 20 s、等待触摸与等待图像各 30 s，均可配置。清理固定为 `ARM_STOP`（仅当已 ARM）→ `ABORT` → `SESSION_OFF`，每条都尝试执行，失败不回退首个根因；取消走独立状态，不伪装成“无手指”。

**范围决定（研究侧知悉项）**：
- 不发送 `GET_KPI`(0x0C) 与 `GET_DEAD_PIXELS`(0x0A)：§10 行 9 已把“非采集必需”定案（43 个 run 从未发送、其中 8 轮仍成功），其返回格式也未观测，不属于可实现契约。
- 会话使用自己的显式状态机而非 `FpiSsm`：`fpi_ssm_new()` 需要 `FpDevice`、`FpiUsbTransfer` 需要**活的** `GUsbDevice`，两者都会把离线验收绑死在设备上；本轮判据要求“合成事件驱动真实状态机”，因此把设备相关内容收到传输边界之后（`fpc_disum_usb.c` 仍严格采用上游 GUsb/FpiUsbTransfer 惯例）。M5 的设备胶接层继续沿用上游 `FpImageDevice` 入口。
- 事件记录只保留**未被消费**的帧（§16.1“记录并忽略”的诊断对象），避免上万条 `EVT_TLS` 数据帧灌满有界日志并挤掉真正需要诊断的事件。

## M5：内置图像驱动与取消/关闭（已完成）

- [x] 将采集状态机接入 `FpImageDevice` 的 open/activate/deactivate/close 入口，用真实图像完成回调，不注册空实现。
- [x] 仅匹配约定的目标设备和固件；不因相近的 FPC 型号推断兼容性，不冒充现有 `fpcmoc` 驱动。
- [x] 任意异步阶段取消后停止输入推进，取消在途请求；已 ARM 时执行 ARM_STOP，再 ABORT、SESSION_OFF。
- [x] cleanup 错误不覆盖首个根因；关闭前释放 BIO/SSL、PSK、USB 和回调所有权；下一次 open 必须新建会话。
- [x] 区分取消、无手指超时、协议失败、TLS 失败和 USB 失败；reset/设备重现不能自动恢复认证资格。

**完成判据**：在所有挂起点取消、失败后 close/reopen、重复关闭等生命周期用例不泄漏、不悬空回调、不重复完成动作；完成真实路径后才加入 Meson 驱动表和设备 ID。不得将通用上层录入 API 的存在解释为本传感器录入生命周期已验证。

**本轮实际验收**：

| 项 | 结果 |
|---|---|
| `test-fpc-disum-session` | 23 个用例。本轮新增：在 §6 的**每个挂起点**逐个取消（`0x08`/`GET_STATE`/`CMD_INIT`/换句柄/`GET_TLS_KEY`/`TLS_INIT`）；`ARM`、`GET_IMG` 在途时取消（清理是否含 `ARM_STOP` 随"ARM 是否已被接受"变化）；取图阶段取消；失败后 close 再新建会话；重复 close |
| `test-fpc-disum-error` | 状态→错误映射（取消／窗口到期／USB 失败／协议与安全失败／编程错误各自成类）、会话文本透传、每个失败都有非空且合法的文本 |
| 四个配置 | `llvm`／`sanitizers`（ASan+UBSan）／`gcc` 三个虚拟驱动配置与新增 `device` 配置（含本驱动）：`cleanroom` 4/4、原生 9/9 全部通过 |
| 变异 | 去掉触摸通知、清理无条件发 `ARM_STOP`、close 一律报成功、把窗口到期映射成通用错误：四者分别使对应用例失败 |
| 静态分析 | `clang-tidy` 目标（`llvm` 与 `device` 两套，覆盖驱动文件）无启用规则诊断；`clang-format` 干净 |
| 注册落点 | 根 `meson.build` 的 `drivers_info['fpc_disum'] = { 'helper': ['openssl'] }`；`libfprint/meson.build` 的 `driver_sources['fpc_disum']` 与 `libfpc_disum` 链接；生成的 `fpi-drivers.c` 调用 `fpi_device_fpc_disum_get_type` |
| 设备匹配 | `id_table` 只有 `10a5:9201`；`usb_discover` 仅接受 `bcdDevice=0x0250` 并给相近版本 0 分（不推断兼容性） |

实现要点：`fpc_disum.c` 是 `FpImageDevice` 适配层——`img_open` 建立**新**会话并在 §6+§8 完成后报告；`activate` 就绪即完成，取图由 `change_state(AWAIT_FINGER_ON)` 启动；设备报触摸时上报"手指在场"，取到图像后上报图像与"手指离开"；`deactivate` 取消在途采集并在其完成回调里完成动作；`img_close` 先取消再走 §16.3 清理，随后释放 TLS 通道、PSK、传输，重复 close 由状态守卫拦住。取消路径明确：open 阶段由动作 cancellable 直接驱动（图像设备核心不会为 open 发 `deactivate`），采集阶段由核心的 `deactivate` 驱动。错误映射集中在 `fpc_disum_error.c`。

**未验证与范围（诚实记录，不得读作已验证）**：
- 适配层与核心的交互（真正的 `FpImageDevice` 生命周期）**无法在本源码树离线驱动**：上游没有可注入的假 `GUsbDevice`，umockdev 回放也承载不了交互式 TLS 握手（握手字节依赖主机随机数，无法复现）。本轮离线验收覆盖**会话层**（含全部挂起点取消、清理、close/reopen、重复 close），适配层由代码评审与 M6 真机覆盖，不记作"已离线验证"。
- `bz3_threshold` 采用框架默认（40）：规格未给出阈值依据，留给 M6 真机评估。
- 主机侧录入按 §16.4 指引由上层重复调用已验证的采集模式实现（框架默认 5 段）；**未在真机验证**，不得宣称录入生命周期已验证。
- 三个开发配置继续只编译虚拟驱动；新增的 `device` 配置才会编译本驱动，任何会枚举设备的命令在该配置下都可能认领真实传感器，因此它只用于编译与不枚举设备的用例。

## M6：受控真机验收与交付（进行中 —— 首次真机验收未通过）

**状态（2026-10-18）**：前置条件已满足（`cleanroom` 4/4 含 22 条向量重建；原生用例在 llvm／sanitizers／gcc 下全绿），
授权范围按本节条目执行。**首次真机运行的结论是失败的**：驱动能匹配并枚举到设备，但 `open` 在 §6 换句柄处失败，
后续传输报 EIO/ENODEV。已定位两个真实缺陷（一个已修、一个待重写），详见下方"首次真机运行记录"。
**在重写后的驱动通过本节全部条目之前，不得把 `fpc_disum` 宣传为受支持。**

- [x] 先完成 22 条向量、原生状态机测试和 ASan/UBSan 检查，再申请具体范围的硬件实验授权。
- [ ] 经授权验证：无手指负例、单次采图、超时/取消、取消后再开与 close/reopen；只声明实际验证的能力。
- [ ] 敏感产物使用独立 0700 目录、0600 文件；真实图像、PSK/envelope、设备日志不进入 Git、CI 附件或净室交付包。
- [ ] 默认不替换系统 libfprint、不启用 PAM/登录验证；任何系统部署或认证实验另行确认。
- [ ] 发布时列明净室哈希、上游 commit、实际支持范围、已运行验收和永久未支持项，维护干净侧接触声明。

**完成判据**：约定范围内的功能可由运行证据证明，仓库没有假成功路径；未验证能力没有被宣传为支持。上游提交和桌面认证扩展不与单次采图验收混为一谈。

### 首次真机运行记录（2026-10-18）

方法：`tools/fpc_disum_hw_probe.c`（手动探针，从不注册为测试）链接 `_build/device` 的本仓库 libfprint；
日志写入 `~/fpc-disum-runs/`（0700 目录、0600 文件），探针只打印非敏感摘要（设备标识、几何、像素摘要、错误文本）。
设备为内置传感器，`/etc/udev/rules.d/70-fingerprint.rules` 已对本 VID:PID 打 `uaccess`，故无需 root；
`power/control=auto`（未改电源策略）。

| 观测 | 结果 |
|---|---|
| 枚举与匹配 | **通过**：`driver=fpc_disum`、`name="FPC Disum Fingerprint Sensor"`，仅 1 个设备；`id_table` + `usb_discover`（`bcdDevice=0x0250`）在真机成立 |
| §6 前三条命令 | **通过**：`0x08` → `0x50`（GET_STATE）→ `0x01`（CMD_INIT，4B 载荷）均成功 |
| §6 换句柄 | **失败**：见缺陷 2 |
| §8 / §4.5 | 未到达（被上一步阻断） |
| 无手指负例、单次采图、超时/取消、close/reopen | **未验证**（未到达；也未请求用户按手指） |

缺陷（按发现顺序）：

1. **已修**：换句柄时把 `g_usb_device_get_configuration()`（=1）与 `g_usb_device_get_configuration_index()`（=4）当成同一量比较，
   于是发出 `set_configuration(4)` —— 正是 §16.2 **禁止的冗余标准配置请求**。真机症状：`Entity not found [-5]`，随后设备
   掉线重枚举（devnum 变化）。现已删除该分支：只**读**当前配置并记录，绝不发送 `SET_CONFIGURATION`（§1 该设备只有一个配置，内核已选好）。
2. **已重写（但真机症状未变）**：原实现"关闭核心句柄 + 在同一 `GUsbDevice` 对象上 `g_usb_device_open()`"。
   已改为：**不触碰核心设备对象**，改为在我们自己的 `GUsbContext` 里重新 `enumerate` → `find_by_vid_pid` → `open` →
   只读配置（绝不 `SET_CONFIGURATION`）→ `claim interface 0`，并改用 GUsb 异步 API 直接做 I/O（`FpiUsbTransfer` 绑定在核心设备对象上，
   无法承载换句柄）。
3. **未解决（阻断 M6，需规格侧确认）**：换句柄**之后**的第一个传输（`0x0b` GET_TLS_KEY）在约 1 s 后报 `transfer failed`
   （LIBUSB_ERROR_IO），紧接着的清理命令报 `No such device [-4]`，且设备**从 sysfs 消失**（`3-7` 短暂不在，随后带新 devnum 回来；
   devnum 5 → 12 → 13）。
   观测序列（两次独立运行，两种换句柄实现均相同）：

   | 步骤 | 结果 |
   |---|---|
   | `0x08` → `0x50`(GET_STATE) → `0x01`(CMD_INIT) | ✅ 每次均成功 |
   | 换句柄（新句柄已 open + claim 成功，配置读为 1） | ✅ 报告成功 |
   | `0x0b`(GET_TLS_KEY) | ❌ 约 1 s 后 IO 错误 |
   | `0x03`/`0x12`（清理） | ❌ ENODEV，且设备已不在总线上 |

   结论：**设备在 `CMD_INIT` 之后确实离开了总线并重新枚举**，与 §6 第 4 步的括注"不是设备重枚举（内核无 usb 设备事件、**地址不变**）"**冲突**。
   本侧按规则停止并交回规格侧，不猜测设备行为；需要研究侧给出的信息是：
   （a）参考实现在 `CMD_INIT` 之后如何处理这段"设备不可见"窗口（等待条件、轮询间隔、是否有额外握手/命令）；
   （b）`CMD_INIT` 的 4 B 载荷是否真的可任意（本侧用规格默认全零）——若设备侧据此决定是否复位，这一点需要写进契约；
   （c）核对参考运行的**地址是否也在变化**（若变化，§6 的括注需要按观测修正）。
   在得到（a）之前，M6 的 §8/§4.5 条目无法开始（初始化自身受阻），驱动不得声明受支持。

附带改进（本次为定位而做，保留）：传输层现在记录每个操作与失败原因（ROADMAP 要求"保留底层 errno"）；
探针的日志与卫生措施固定为 0700/0600。

## 开发规则

1. 每个里程碑先写能捕获真实边界/状态错误的测试，再实现；构造人工输入不等于被测 C 实现已通过。
2. C 代码遵循上游 GLib 风格和资源所有权惯例，不引入独立线程框架、网络守护进程或第二套驱动抽象。
3. 每次修改运行受影响的离线用例；合并前检查完整净室基线及全部已实现测试。
4. 不将本 ROADMAP 当作新增设备协议来源；事实缺口必须回到规格维护流程。
