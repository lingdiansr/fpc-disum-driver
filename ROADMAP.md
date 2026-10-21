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
| [CLEANROOM_SPEC.md](docs/CLEANROOM_SPEC.md) | `e1a5d7d58c6903f9bcb4d862ef69d00cf45d2be1fd3d9b582c1f7bad586b08dc` |
| [CLEANROOM_TEST_VECTORS.json](docs/CLEANROOM_TEST_VECTORS.json) | `ce04ce43665f2b2d27142f2c2ecd917b09219a87a82d4b3009c023baecfeda94` |

上游代码只提供通用框架、API、算法和编码惯例；其中其他设备的驱动、公开录制样本和图像不是本传感器的协议依据。不得访问研究侧仓库、真实 PSK/图像、专有二进制、抓包或反编译产物来补实现。

历史评审记录已移除（git 历史可查），不覆盖本表基线。规格发现缺漏或规范冲突时只记录现象与缺失条件，交回维护方修订并复核；不自行猜测设备行为，也不修改交付包来迎合代码。

## 范围与硬边界

**纳入**：单次采图、112×88 灰度像素、初始化/句柄迁移、TLS-PSK、分帧与应用消息解析、取消/超时/错误及资源回收、合成输入与离线状态机验收。

**不纳入**：设备侧 `ARM wValue=2`、`END_ENROL` 驱动流程、完整抬指/重按录入生命周期、reset/设备重现后的自动恢复认证、DFU、其他 VID/PID 或固件、Windows 模板/算法和 Windows Hello 协议。

真实采集、取得设备密钥或改变设备状态的实验必须另获明确授权。本轮的构建和测试不读取或控制目标传感器；默认开发配置只编译上游虚拟驱动。不得安装到系统、替换系统 libfprint、改动 fprintd/PAM/udev 或修改 USB 电源策略来证明初始化成功。

**命名（2026-10-18 决定）**：项目/仓库标签与驱动同名 —— 仓库 `fpc-disum-driver`，驱动、模块、测试与注册 ID 一律用**设备族**名 `fpc_disum` / `fpc-disum`。依据：目标设备的厂商标识是 **Fingerprint Cards AB**（§1：VID `10A5`），本侧只覆盖这一颗传感器与固件 `0x0250`；上游 libfprint 按传感器族命名驱动（`fpcmoc`、`egismoc`、`goodixmoc`、`elanmoc`、`focaltech_moc`…），不存在按整机品牌命名的先例；规格（冻结）也以 “FPC Disum” 称呼该设备，同名便于逐条对照。被验证的那台机器是 XIAOMI Redmi Book 14（主板 TM2205，SKU TM2205-56978，内置传感器），这只作为**来源说明**，不进任何标识符。

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

XIAOMI 只作为**来源说明**存在（“目标设备出现在 Redmi Book 14 / TM2205 上”），不进入任何标识符；驱动侧名字表达的是“支持哪颗传感器”。

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
- [x] 使用系统 Meson 加 `dev/native/common.ini`、`llvm.ini`、`gcc.ini`、`sanitizers.ini` 定义独立构建；保留 GNU C99 和原有虚拟驱动安全范围，不再依赖 uv/项目内 Meson 包装。
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

> 门控说明（2026-10-18 复核）：Meson 内置的 `clang-format`／`clang-format-check` 目标是**全树**范围，而上游文件并不满足本仓库 `.clang-format`，
> 因此它们**不是**本仓库的格式门（误用会重排大量上游文件）。本仓库的门是 **定向** `clang-format --dry-run --Werror` 跑驱动与新增测试；
> `clang-tidy` 目标同样只对本仓库自身目标（驱动与新增测试）有意义。另：`libfprint:metainfo-validate`（`appstreamcli validate` 上游 metainfo）
> 在全部构建目录下失败，报告 `description-first-para-too-short`（info）、`tag-empty provides`（warning）、`developer-info-missing`（info）；
> 该文件与其生成输入都不在本次改动范围内，属既有的主机工具/上游数据问题，不计入本驱动验收。

实现要点：`fpc_disum.c` 是 `FpImageDevice` 适配层——`img_open` 建立**新**会话并在 §6+§8 完成后报告；`activate` 就绪即完成，取图由 `change_state(AWAIT_FINGER_ON)` 启动；设备报触摸时上报"手指在场"，取到图像后上报图像与"手指离开"；`deactivate` 取消在途采集并在其完成回调里完成动作；`img_close` 先取消再走 §16.3 清理，随后释放 TLS 通道、PSK、传输，重复 close 由状态守卫拦住。取消路径明确：open 阶段由动作 cancellable 直接驱动（图像设备核心不会为 open 发 `deactivate`），采集阶段由核心的 `deactivate` 驱动。错误映射集中在 `fpc_disum_error.c`。

**未验证与范围（诚实记录，不得读作已验证）**：
- 适配层与核心的交互（真正的 `FpImageDevice` 生命周期）**无法在本源码树离线驱动**：上游没有可注入的假 `GUsbDevice`，umockdev 回放也承载不了交互式 TLS 握手（握手字节依赖主机随机数，无法复现）。本轮离线验收覆盖**会话层**（含全部挂起点取消、清理、close/reopen、重复 close），适配层由代码评审与 M6 真机覆盖，不记作"已离线验证"。
- `bz3_threshold` 采用框架默认（40）：规格未给出阈值依据，留给 M6 真机评估。
- 主机侧录入按 §16.4 指引由上层重复调用已验证的采集模式实现（框架默认 5 段）；**未在真机验证**，不得宣称录入生命周期已验证。
- 三个开发配置继续只编译虚拟驱动；新增的 `device` 配置才会编译本驱动，任何会枚举设备的命令在该配置下都可能认领真实传感器，因此它只用于编译与不枚举设备的用例。

## M6：受控真机验收与交付（已完成 —— 声明范围内）

> **2026-10-21 完成记录（优先于下方历史轮次）**
>
> **根因与修复**：初始化/重枚举期间没有持续读 bulk，设备复位的 `EVT_INIT_RESULT` 被丢弃；该偏差单独即可造成 ARM 后零上报（[`docs/IMPLEMENTATION_REQUIREMENTS.md`](docs/IMPLEMENTATION_REQUIREMENTS.md) 的 §2；逐阶段契约见 §2.1）。后端已改为在 `CMD_INIT` 前启动连续读 URB，跨复位保留帧；reopen 等旧读自然结束（2.5 s 有界 grace），新 handle claim 后先重启读再完成回调。
>
> **真机验收（全部 exit 0，cleanup 完成）**：
>
> | 场景 | run_id | 结果 |
> |---|---|---|
> | 单次采集 | `93bd3886-8074-4109-be17-1adeb51fa00d` | 17 帧 / 10,878 B；112×88 / 9856 B |
> | 无手指负例 | `63808bb3-6c28-492e-8fbc-1bc297d2b0d2` | `no-touch`，无图像 |
> | 取消 | `af7a0146-b185-4f72-b688-ddcd3723ace8` | `cancelled`，无图像 |
> | 取消后再采集 | `0361489d-1389-46a3-bcd6-0f762bca8791` | 首轮取消、次轮图像 |
> | close/reopen 两次采集 | `efe74e89-a7c0-453b-a348-efb626e28ce5` | 两轮图像，`image_distinct=true` |
>
> **离线**：llvm / gcc / sanitizers / device 四 profile 均 cleanroom 4/4、驱动与核心 12/12；USB smoke 通过。
>
> **支持范围/未支持项/偏离**：见 [`docs/IMPLEMENTATION_REQUIREMENTS.md`](docs/IMPLEMENTATION_REQUIREMENTS.md) 的 §6。M6 声明范围外的录入/模板匹配已由 M7.2 的 SIGFM 路径在 in-process 探针中验证（见 M7.2）；系统 fprintd/PAM/桌面、同会话第二次采集、异常复位恢复、其他设备/固件、DFU 与上游接受度仍未验证。
>
> 下方 2026-10-18 的验证回顾与各轮记录保留为历史，其中“根因未定/失败点不在驱动侧/不得宣称 M6 通过”的结论已被本轮证据取代。

> **验证回顾更正（2026-10-18，源码基点 `81c03c3`）**：当前结论与交回问题以
> 历史验证回顾文档已移除（git 历史可查），当前结论以顶部完成记录为准。
> 下方各轮保留为历史记录；其中“失败点不在驱动侧”“同毫秒打印证明实际提示/保持”
> “304 次都是窗口内空读”“空闲 10 分钟即可排除残留”等判断已撤回。
> 本次重核：带提示且 100 ms 的可比组是 5 个完整超时轮、3 个约 25 ms 保留值失败轮；
> 304 为全轮读提交数，采集区间为 299。仍有接收完成语义、TLS 错误消费、
> 反馈 flags 与核心 USB 身份/取消清理生命周期问题，不能宣称实现侧已排除。
> 四配置指定离线组各 14/14 通过，安全 CLI 用法分支 smoke 退出 2；本次未启动真实采集，
> 未修改生产代码。请将该复核文档交给规格侧，并按其中 Q1–Q6 补充去敏对照元数据。

**状态（原始 2026-10-18 记录，已被顶部 2026-10-21 完成块取代）**：前置条件已满足（`cleanroom` 4/4 含 22 条向量重建；原生用例在 llvm／sanitizers／gcc 下全绿）。下方首次真机失败记录仅作历史保留；当前完成范围以顶部表格与 [`docs/IMPLEMENTATION_REQUIREMENTS.md`](docs/IMPLEMENTATION_REQUIREMENTS.md) 的 §6 为准。

- [x] 先完成 22 条向量、原生状态机测试和 ASan/UBSan 检查，再申请具体范围的硬件实验授权。
- [x] 经授权验证：无手指负例、单次采图、超时/取消、取消后再采集与 close/reopen；只声明实际验证的能力。
- [x] 敏感产物使用独立 0700 目录、0600 文件；真实图像、PSK/envelope、设备日志不进入 Git、CI 附件或净室交付包。
- [x] 默认不替换系统 libfprint、不启用 PAM/登录验证；未做系统部署或认证实验。
- [x] 发布时列明净室哈希、上游 commit、实际支持范围、已运行验收和永久未支持项，维护干净侧接触声明（见 [`docs/IMPLEMENTATION_REQUIREMENTS.md`](docs/IMPLEMENTATION_REQUIREMENTS.md) 的 §6）。

**完成判据**：约定范围内的功能可由运行证据证明，仓库没有假成功路径；未验证能力没有被宣传为支持。上游提交和桌面认证扩展不与单次采图验收混为一谈。

### 首次真机运行记录（2026-10-18）

> 历史排查文档已移除（git 历史可查）；当前结论见顶部完成记录。

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
3. **已按规格修订重写（2026-10-18，见下节）**：当时的表现是换句柄**之后**的第一个传输（`0x0b` GET_TLS_KEY）在约 1 s 后报 `transfer failed`
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

### 规格侧答复与重连判据实现（2026-10-18）

规格侧已独立复现（**4/4** 次提交）并**修正契约**：`CMD_INIT` 之后设备确实复位并重新枚举（命令后 +1.0–1.2 s 掉线、
+0.5 s 以**新地址**回来、地址必定变化；实测 devnum 22→23→24→25），原 §6 括注"不是设备重枚举、地址不变"
来自一次**采样过早**的观测（断连发生在命令后约 1.1 s，当时窗口未覆盖）。修订三处：§6 第 4 步子项、§10 行 6、附录 A。
逐问答复：① 参考实现按"新枚举结果 + 探活 + 两次身份一致（含 address）+ 先释放旧句柄 + 重试 open/claim"处理，**不用固定延时**；
② 契约已改；③ `CMD_INIT` 载荷**与复位无关**（全零继续可用）。历史答复文档已移除（git 历史可查）。

- 新基线（第 11 轮隔离复核：可实现、0 阻塞）：`docs/CLEANROOM_SPEC.md` = 76,166 B / `3968405f…`；向量文件未被改动（`ce04ce43…`）。
  本仓库已把 `tools/check-cleanroom.py` 与本文档的固定哈希更新到该基线；`tools/check-cleanroom.py` 与 `sha256sum -c docs/SHA256SUMS` 均通过。
- 传输层按判据重写（`fpc_disum_usb.c`，`FPC_DISUM_USB_REOPEN_*`）：① `CMD_INIT` 后**先释放旧句柄**（否则新句柄的 bulk 传输可能 EBUSY）；
  ② 每 **0.2 s** 取一次**新的枚举结果**，要求连续两次身份（bus/address/port）一致且 address 非空（仍在稳定过程中的候选被拒）；
  ③ 打开后用 `GET_DESCRIPTOR` **探活**；④ 只读当前配置（绝不发 `SET_CONFIGURATION`，§16.2）后重新 `claim interface 0`。
  任何一步失败都重试到 **20 s** 上限；超时上报 USB/IO 失败（不静默继续、不退回旧句柄）。设备回来后一瞬间 `open`/`claim` 仍可能报 `-4`/`-5`，重试即覆盖。
- 每一次 `CMD_INIT` 必然消耗一次掉线重枚举（devnum 递增），这是设备固有行为、不是缺陷；真机脚手架的账目按"一次 init = 一个 devnum"记。
- 规格侧提醒（已遵守）：复位窗口内也**不得**重发标准配置请求（冗余 `SET_CONFIGURATION` 会触发 `-5` 并导致掉线）；
  接收按 `len` 累积，不假设 512 B 包（内核对本设备报 `bulk endpoint 0x82 has invalid maxpacket 64`，与 §1 的"全速描述符 / 高速连接"一致）。
- 改动后的离线复核：`cleanroom` 4/4、原生 14 项通过；`llvm`／`gcc`／`sanitizers` 三套构建通过；
  定向 `clang-format --dry-run --Werror`（驱动与新增测试，17 个文件）干净；定向 `clang-tidy`（驱动文件，`clang-analyzer-*`/`bugprone-*` 为错误级）零诊断。
- 真机重跑见下一节（本节全部条目需在重跑中逐条通过）。

### 真机复跑（2026-10-18 第二轮）：判据生效 + 两个新问题

起点是提交 `e528b6a`（按 §6 判据重写换句柄）。离线复核：`cleanroom` 4/4、原生 14 项通过；`llvm`／`gcc`／`sanitizers` 三套构建通过；
定向 `clang-format --dry-run --Werror`（17 个文件）干净；定向 `clang-tidy`（驱动文件）零诊断。

| 观测 | 结果 |
|---|---|
| §6 重连判据 | **通过**：`CMD_INIT` → 先释放旧句柄 → +1.08 s 设备离总线（探活失败）→ +0.71 s 后 `reconnected after CMD_INIT: bus 3 address 28 port 7`（地址 26→27→28，每次递增）→ 重新 claim |
| `0x0b`(GET_TLS_KEY) | **通过**：在新句柄上立即成功（首轮是"约 1 s 后 IO 失败"） |
| §6/§8 其余 | **通过**：`0x05`(TLS_INIT) → `0x06`(TLS_DATA) 分片握手 → `Image device open completed` |
| 无手指负例 | **通过**：`activate` → `0x02`(ARM) → 驱动自身 30 s 触摸窗口到期（`no touch event arrived within the window`）→ 清理 `ARM_STOP`→`ABORT`→`SESSION_OFF` 全部发出 → `close completed` |

**缺陷 4（本次发现并修复）：控制 OUT 上界写错。** 传输层把控制 OUT 数据阶段的上界定为 **4 B**（对 §3.1 的过度概括），
而 §4.1 明确 `CMD_TLS_DATA`(0x06) 的 `wLength` 是**变长 6-64 B**，§8 要求宿主→设备的 TLS 记录按 **≤64 B** 分片。
症状：`fpc_disum_usb_control_out: assertion 'len <= FPC_DISUM_USB_CONTROL_OUT_MAX' failed` ⇒ 握手首个分片被丢 ⇒
`the handshake made no progress`。修复：上界改为跟随 `FPC_DISUM_TLS_DATA_MAX`（同一常量，不会再漂移），并更正
`fpc_disum_transport.h` 的契约注释。**该边界离线不可覆盖**（假传输没有这个上界），只有真机能暴露 —— 这也是首轮没走到握手的原因被掩盖的方式。

**缺陷 5（已修 —— 交付方选择"fork 内最小核心扩展"）：设备自复位与核心"设备已移除"模型冲突。**
`CMD_INIT` 让设备离总线后，`FpContext` 的 GUsbContext `device-removed` 信号经 `usb_device_removed_cb`（`fp-context.c:263`）
把该 `FpDevice` 永久标记为已移除；随后 `fp_device_task_return_in_idle_cb`（`fpi-device.c:1076`，核心注释自述"除成功 open 这一 odd corner case"）
会把**所有非 open 动作的结果强制改写成 `FP_DEVICE_ERROR_REMOVED`**。本次运行日志证据：open 成功、ARM 成功、驱动按契约报"窗口到期"，
但上层拿到的**采集**与 **close** 结果都是 `REMOVED`（`capture: error domain=fp-device-error-quark code=256 message="This device has been removed from the system."`）。
⇒ 现状下**任何真实采集都无法交付给上层**（成功采集同样会被改写成 REMOVED）；而 §16.2 要求"下一次 `open` 必须新建 session 并重新初始化+TLS"，
即**每次 open 必然复位设备**。这不是设备行为未知，也不是规格缺口：研究侧 Linux 参考工具是独立 libusb 程序，不经过 libfprint 的移除模型，所以从未遇到。
交付方选择 **(A)**，实现如下（本次提交）：

- 核心新增 `fpi_device_usb_set_reenumeration_expected()` / `fpi_device_usb_is_reenumeration_expected()`（标志存于 `FpDevicePrivate`）；
  `fp-context.c` 的 `usb_device_removed_cb` 见到该标志时**不**调用 `fpi_device_remove()` —— 只有"没人预期的离场"才算移除。
- 传输接口新增 `reenumeration_expected`：会话在发出 `CMD_INIT` **之前**置位；USB 传输在重连操作结束（成功/失败/取消）以及传输销毁时释放
  （陈旧标志会掩盖真实拔出，故销毁时也要清）。
- 永久用例：`/session/reenumeration-expectation`（置位必须早于 `CMD_INIT`、重连后必须已释放）与
  `/session/reenumeration-expectation-released-on-failure`（重连失败同样必须释放）。
- **上游偏离记录**：本 fork 从此不只"新增一个驱动"，还自带一处核心扩展，涉及上游文件
  `fp-context.c`、`fpi-device.[ch]`、`fp-device-private.h`。语义边界：窗口内若设备真被拔掉，驱动会以"会话失败"如实上报，
  而不会被报成"设备已移除"（`spec` 未规定此情形，属工程约定）。
- 未选择 **(B)** 独立守护进程形态（README 允许）：那会失去 fprintd/PAM 集成并作废 M1–M6 的框架整合工作。

### 核心扩展后的真机复跑（2026-10-18 第三轮）

| 观测 | 结果 |
|---|---|
| 无手指负例 | **通过**：`open: ok`；驱动自身 30 s 窗口到期并上报**真实错误** `G_IO_ERROR_TIMED_OUT`（"no touch event arrived within the window"），不再是 `REMOVED`；清理 `ARM_STOP`→`ABORT`→`SESSION_OFF` 全部发出 |
| 取消（ARM 后 3 s） | **通过**：`G_IO_ERROR_CANCELLED`（"Operation was cancelled"），不再被 `REMOVED` 覆盖；close 保留首个根因 |
| 核心标记 | **已消失**：每轮 open 之前均打印 `re-enumeration expectation released` 且 `open: ok`，运行日志中不再出现 `FP_DEVICE_ERROR_REMOVED` |

**新观测（交规格侧，不影响驱动裁决）**：第二轮 `cancel` 运行时，设备在 ARM 后**立即**返回 `EVT_ARM_RESULT` 且第三字段 = **保留值 0x64**，
驱动按 §16.1 第 419 行如实判定"当前事务失败"（`the arm result carries the reserved state value`）。
该取值在 §10 行 3 已按"不可复现/不可达"结项，本次却在**完全干净的 Linux 路径**上复现——紧接上一轮"窗口到期 + 清理"之后，全程未做任何被禁的配置动作。
后续运行中**又出现 1 次**（`cancel-reopen` 的第一会话，同样紧接在一次以"触摸窗口到期"结束的会话之后）；夹在两次出现之间的 `cancel` 用例 ARM 正常。
⇒ 本机全部 **5 次**观测**都紧接**一次"窗口到期"的会话（规格侧同日 3 会话 / 4 次空 `ARM` 未复现该关联，已并入 §10 行 3 的双侧记录），
属**间歇性残留**、成因未定；规则不变（只表示当前事务失败，重试/新会话即恢复）。
⇒ 建议规格侧把 §10 行 3 由"不可达"修订为"间歇可达（干净路径亦可，发生在紧接前一轮会话之后）"，并记录与 §16.4 残留现象的疑似同源关系。

### 第四轮真机与 M6 结论（2026-10-18，诚实收口）

> 历史交回文档已移除（git 历史可查）；当前结论见顶部完成记录。

| 观测 | 结果 |
|---|---|
| 命令序列 | **与 §6 逐步一致**：`0x08`(CMD_INDICATE_S_STATE) → `0x50` → `0x01` → 〔重连〕 → `0x0b` → `0x05` → `0x06`×4 → `0x02`(ARM)；每轮 `CMD_INIT` 前后的"预期重枚举"置位/释放都在日志中可见 |
| 采集窗口内的读端 | **在持续读**：每个 30 s 窗口内恰有 30 次 `op bulk read`（1 s 轮询），无停摆 |
| 触摸上报 | **0 次**：连续 **8 个窗口**（操作者盲按，运行提示只出现在终端、操作者看不到）从未出现 `FP_FINGER_STATUS_PRESENT`，每轮都以驱动自身的 `G_IO_ERROR_TIMED_OUT`（窗口到期）结束 |

**结论**：**采集正向路径仍未验证**，且失败点不在驱动侧 —— 命令序列、§6 重连判据、§8 握手、ARM、30 s 窗口、清理顺序都按契约执行。
两个候选原因都需人在设备旁：(a) 按压未落在 ARM 之后的窗口内（操作者无法看到运行提示；§16.4 明确提示"按压未与 ARM 对齐"会失败）；
(b) 设备侧可能需要规格未描述的前置条件（§10 未记录）。**不得据此推断设备行为**，故不猜测、不自动重试。

**M6 状态（不得读作通过）**：
- **已验证**：枚举与匹配；§6 全序列（含 `CMD_INIT` 复位/重枚举判据）；§8 握手；`activate`→ARM；30 s 触摸窗口；无手指负例；ARM 后取消；清理顺序；close 保留首个根因；多会话各自完整初始化；核心"预期重枚举"扩展（不再把自复位误报为移除）。
- **未验证（不得宣传为支持）**：单次真实采集（含像素与几何）、取消后再采集、close/reopen 两次采集、录入生命周期。
- **下一步（需操作者在设备旁）**：由操作者自行在终端运行 `tools/fpc_disum_hw_probe`（能看到 `capture: waiting for the finger (driver window 30000 ms)` 提示后再按压），或按住并反复轻抬-重压；`cancel-reopen`／`reopen` 同法。撞上 §10 行 3 的 0x64 残留时重跑同一命令即可。

### 第五轮：按规格侧方案补齐后仍无触摸上报（2026-10-18）

按规格侧第二轮答复实现并验证：

- **§5 读端节奏**：读端由"每次 1 个 64 B 包"改为**4096 B 分块 + 100 ms 空转超时**；实测每个 30 s 窗口 **304 次** `op bulk read`（≈10 次/秒），符合"持续排空、短超时立刻重试"。
- **§4.5 操作前提**：手动探针订阅 `finger-status`，在核心上报 `NEEDED`（即发出 `ARM` 的同一跃迁）时给出**桌面通知 + 铃声 + 终端醒目行**；日志中确认 `*** ARM SENT ***` 正是在 `NEEDED` 之后一行出现。
- **构建踩坑（已修）**：`_build/device` 的探针曾因一次插入位置错误（代码块落在 `int main()` 的返回类型与函数名之间）而**编译失败**，但构建输出被过滤器吞掉，导致后续几轮真机跑的是**17:18 的旧二进制**（无提示、无分块读）——那几轮的"无触摸"结论无效。现已修正布局、把 `press_wanted` 声明前移，并核对二进制 mtime/大小。

**结论（有效数据）**：即使提示确实发出（日志可证）、读端按 §5 排空，连续 **3 个窗口**仍是 **0 次触摸上报**（其中一个窗口撞上 `0x64`，仍遵循"紧接窗口到期会话之后"的规律，累计 6 次观测全部如此）。
⇒ 失败点**不在驱动侧**：命令序列、重连、握手、ARM 载荷/wValue、窗口、读端排空、清理都按契约执行并可在日志中逐步核对。
⇒ 现有证据无法区分"操作者的按压未落在 `ARM` 之后的窗口内"与"设备侧另有规格未描述的前置条件"。**按规则不猜测**，交回规格侧；同时建议由操作者**亲自在终端运行探针**（终端里同样打印提示行，且不受自动化回合时序限制），以便区分这两种情形。

### 第六轮：操作者本人运行、提示可证、仍零上报（2026-10-18）

> 历史交回文档已移除（git 历史可查）；当前结论见顶部完成记录。

- 操作者亲自运行（`operator-capture-181712.log`）：`open: ok` → 提示（`*** ARM SENT ***`，紧随 `NEEDED`，通知经 `quickshell` 投递、铃声正常）→ 按压 → **跑满 30 s 窗口**（18:17:14→18:17:44）→ **0 次 `FP_FINGER_STATUS_PRESENT`**、无 `EVT_FINGER_DWN`。
- 该轮读端为 §5 合规的 100 ms 分块排空（窗口内 **304 次** `op bulk read`）；`ARM` 载荷 `11 2f 11 17`、`wValue=1` 与 §4.1 逐字节一致；§6 序列/重连/§8 握手/清理均在日志中逐步可见。
- **剩余最可信差异（待规格侧确认）**：本机所有尝试都是**几十秒内连续会话**（每次 `CMD_INIT` 都复位设备），而 §16.4 记载"reset 后约 7 分钟内 `EVT_FINGER_DWN` 不可信"；规格侧成功采集很可能是**间隔较久**的单次运行。
- 本侧下一步（**不改契约、不猜设备**）：设备**空闲 ≥10 分钟**后跑一次单次采集；若成功则补齐 `cancel-reopen`／`reopen` 并更新 M6 条目。

## M7：fprintd 端到端功能补全（未完成）

M6 只证明“采集”链路。fprintd 登录/验证需要 enroll（默认 5 段采集）、verify/identify（NBIS 匹配）与完整 daemon 生命周期；这些尚未实现验收。

### M7.0 能力审计与测试矩阵

- [ ] 确认 `FpImageDevice` 基类实际提供的 features：`CAPTURE`、`IDENTIFY`、`VERIFY`、`UPDATE_PRINT`；确认本驱动没有设备存储（`STORAGE`），print 由 fprintd 主机侧保存。
- [ ] 确认 `nr_enroll_stages=5`、`bz3_threshold` 默认 40、print 类型 `FPI_PRINT_NBIS` 的实际行为。
- [ ] 建立能力矩阵：单次 capture、同会话多次 capture、enroll、verify、identify、list/delete、cancel、suspend/resume；每项标注“代码存在 / 离线测试 / 真机通过 / 未验证”。
- [ ] 明确不支持项：设备内模板、MOC、录入模式 `wValue=2`、其他固件。

**完成判据**：矩阵中不存在“能力未知”项；未验证项不得出现在支持声明中。

### M7.1 同一 open 会话内多次采集

fprintd enroll 会在一次 open 内连续取 5 张图；M6 只验证了单次采集和 close/reopen。

- [x] 审计并修正 `fpc_disum_session_capture_async()`：首次 capture 后 session 仍可再次进入 `CAPTURE_ARM`，TLS 通道复用，不重新执行 §6/§8。
- [x] 保证每轮独立：touch 窗口、ARM、GET_IMG、图像回调、finger status；`ARM_STOP` 只在 close/cleanup 执行一次。
- [x] 定义轮间失败语义：某轮 no-touch/取消/协议失败时终止本次 open；不得吞错或复用旧图。
- [x] `test-fpc-disum-session` 增加 5 次连续 capture 的离线用例，以及第 3 轮 no-touch 后 close 的用例。
- [x] 真机增加 `multi-capture N` 探针模式（同一 open，N 次按压/出图），验证 5 张图结构均合法、无会话重建。

**完成判据**：一次 open 内 5/5 张合法图像，TLS/CMD_INIT 只发生一次，cleanup 只发生一次；离线与真机均有证据。

**2026-10-21 验收**：离线 `test-fpc-disum-session` 46 例通过（含 5 轮与第 3 轮失败）；真机 `multi-capture --captures 5` 得到 **5/5** 张 112×88/9856 B 图像，一次 close/cleanup，run `~/fpc-disum-runs/m7-multi-retry-20261021T041258Z`。此前一轮第 3 轮因 NBIS 未检出细节点返回可重试错误，属 M7.2 图像质量问题，不是会话重建。

### M7.2 enroll / verify / identify

- [x] **NBIS 评估**：112×88 图像只有 1 个 verify 细节点、3–5 个模板细节点；BZ3 全部 0/40。NBIS 硬门 `MIN_COMPUTABLE_BOZORTH_MINUTIAE=10` 在低于 10 时直接返回 0，调阈值无效。
- [x] **替换 matcher**：接入 SIGFM（OpenCV SIFT + ratio + 几何一致性），`score_threshold=10`；新增 `enroll_coverage_shift=12` 的覆盖引导注册。
- [x] `fprintd-enroll`：隔离 fprintd daemon 完成 5 段采集；覆盖重试有效；取消/重启后仍可 list。
- [x] `fprintd-verify`：隔离 daemon 同指 `verify-match`（SIGFM 36/10），左手拇指 `verify-no-match`（5 个模板均 0/10）。
- [ ] `fprintd-identify`：至少两个已录入 print 中选择正确者；不匹配时失败而非误报。
- [x] `fprintd-list` / `fprintd-delete`：隔离 daemon 可列出、删除；删除后回到空存储。
- [ ] 记录 FRR/FAR 的粗略样本（不是安全认证结论），至少覆盖成功/失败/重试。

**完成判据**：enroll/verify/identify 在目标设备上可重复通过；失败路径不产生假成功；阈值与图像质量有记录。

**2026-10-21 实现与真机**：覆盖引导下注册 **5/5**（太近/无重叠会重试且不消耗阶段），同指 verify **3/3 match=true**（分数 141/20/1085），异指 verify **0/3 match=true**。`examples/enroll` 写入磁盘、`examples/verify` 反序列化后曾成功 MATCH；随后修复 `fp_print_equal()` 未处理 `FPI_PRINT_SIGFM` 的 critical。离线 `--no-suite data`：17 通过、32 跳过、0 失败。仍需系统 fprintd/PAM/桌面端到端、identify 多模板与打包回归。

**历史依据**：NBIS 阈值、NCC 相关性和 8 轮扩样本的失败记录已被 SIGFM 实现取代，保留在 git 历史（commit `5ac0648`、`70adc30`、`b0d9391`）；不再作为当前实现路径。

### M7.3 fprintd daemon 生命周期与系统集成

- [x] `fprintd` 启动、停止、重启后设备可再次枚举和 open（私有 D-Bus 隔离环境，并用系统 systemd 服务复验）。
- [ ] D-Bus 取消：capture/verify 中途取消，daemon 不挂起，下一动作可用。
- [ ] suspend/resume、锁屏/解锁后设备可用；必要时补驱动/核心 suspend 处理。
- [ ] 多用户：不同用户各自 print 存储隔离，设备释放正常。
- [ ] 设备占用：enroll/verify 并发请求返回明确错误，不重复 open 同一传感器。
- [ ] 在一次性测试用户上验证 PAM 登录/解锁；不修改系统默认 PAM 配置。
- [ ] 异常：daemon 在 capture 中被杀、设备拔出、非预期 reset；不泄漏句柄/密钥，不假成功。

**2026-10-21 无手指 fprintd 冒烟**：`tools/fprintd-nofinger-smoke.sh /tmp/fpc-default-fpc` 使用私有 D-Bus + 测试专用 polkit，通过设备枚举、空存储 list/delete、无 print verify、enroll 取消、daemon 重启，共 7 项检查。未触摸的 enroll 超时仍由 fprintd 报 `enroll-unknown-error`，客户端断开时 daemon 记录 `ReleaseDevice`/`CRITICAL`，但设备随后仍可 list 和重启；这是待解决的 fprintd 错误映射/释放路径，不影响“无手指冒烟”结论。随后已做有手指隔离测试（fprintd 1.94.5）：5 段 enroll、同指 verify-match（36/10）、左手拇指 verify-no-match（0/10）、list/delete。又通过 pkexec + 临时 systemd drop-in 让**系统 fprintd** 加载测试库：设备枚举、5 段 enroll、同指 verify-match、左手拇指 verify-no-match、delete 均通过；测试后已移除 drop-in/测试库并恢复 stock libfprint。PAM/桌面登录仍未验证。

### M7.4 失败、取消与安全边界

- [ ] no-touch 超时、用户取消、daemon 取消、设备移除分别返回可区分错误。
- [ ] 失败后 print 不写入/不更新；部分帧不拼成成功图像。
- [ ] ARM_STOP/ABORT/SESSION_OFF 在每轮 cleanup 只执行一次；错误不覆盖首因。
- [ ] 真机日志不保存图像、PSK、envelope、TLS 载荷；证据目录 0700/0600。
- [ ] 复核 `0x64` 只按规格失败，不重试掩盖。

**完成判据**：故障注入下无假成功、无泄漏、无悬空回调；支持声明只写已验证能力。

### M7.5 支持范围更新

- [x] 将 M7 结果写回 `docs/IMPLEMENTATION_REQUIREMENTS.md`，区分“libfprint in-process”“隔离 fprintd”与“系统 PAM/桌面端到端”。
- [ ] 列出已验证的 fprintd 版本、设备/固件、未支持项。
- [ ] M7 未全部通过前，M8 的发布产物只能标记 experimental，不得描述为生产可用。

**完成判据**：支持声明与真机证据一致；不存在“代码存在即宣称支持”的条目。

## M8：CI/CD 与发布工程（下游打包不在本仓库）

> **前置**：M7.2 的 in-process 与隔离 fprintd 路径已验证；系统 PAM/桌面未完成。AUR、发行版包、PAM 策略和系统库替换由下游维护，本仓库不创建 PKGBUILD，也不发布替换系统 `libfprint` 的包。

**目标**：用可复现的 CI 构建、测试和源码发布替代手工打包；硬件验收保持手动。

### M8.0 CI 基线

- [ ] 固定 Meson、编译器、依赖和 OpenCV 版本的兼容窗口；记录已验证的版本组合。
- [ ] 构建矩阵：LLVM、GCC、ASan+UBSan、仅编译 `device` 配置；CI 不连接真实传感器。
- [ ] 发布前检查：工作树干净、tag/commit 固定、`docs/SHA256SUMS` 可验证、许可证/NOTICE 完整。

**完成判据**：同一 commit 在干净环境可重复构建。

### M8.1 Pipeline

- [ ] checkout 后先执行 `sha256sum -c docs/SHA256SUMS`，失败即停止。
- [ ] 配置、编译、cleanroom/unit/protocol/state-machine 测试；clang-tidy 与格式检查作为门禁。
- [ ] 检查 `libfprint-2.so` 的 soname/公共符号，避免破坏其他驱动或 `fprintd` ABI。
- [ ] 产出源码 tarball、SHA-256、构建/测试日志；不产出或上传图像、PSK、envelope、TLS 载荷。
- [ ] 缓存依赖但不得缓存过期构建产物；clean build 与增量构建分别验证。

**完成判据**：任一 cleanroom 哈希、测试或 ABI 门禁失败时，pipeline 失败关闭。

### M8.2 发布

- [ ] 版本号、changelog、支持范围、已知限制、回滚说明随源码归档发布。
- [ ] 发布前完成一次手动硬件冒烟（采集、取消/重开、SIGFM 注册/验证）；结果写入 release notes，不进入 CI。
- [ ] 源码归档可复现；发布说明明确“系统 fprintd/PAM/桌面未验证”和“不得用于生产认证”。
- [ ] 不发布 AUR/发行版包；下游打包必须自行声明源、版本、依赖和回滚方式。

**完成判据**：release 资产只含源码、校验和与证据摘要；无系统包或二进制替换物。

### M8.3 维护与上游化

- [ ] 上游 libfprint 升级或 OpenCV/ABI 变化时重建并复跑矩阵；记录 rebase 成本。
- [ ] 将驱动与核心 USB 重取扩展提交上游；上游接受后标记本 fork 为过渡/退役。
- [ ] 安全与隐私：不收集指纹图像；不把模板或 run 目录打包进发布物。

**完成判据**：上游状态、维护周期和退役决定有记录。

### 风险与边界

- **CI 无硬件**：自动测试只能证明编译、协议/状态机和 cleanroom 向量，不能证明真实采集或匹配成功率。
- **OpenCV 依赖**：SIGFM 增加 C++/OpenCV 依赖；不满足时构建失败关闭。
- **下游打包差异**：AUR/发行版可能使用不同 soname、fprintd 版本或 PAM 配置；本仓库不为其行为背书。
- **系统 fprintd/PAM/桌面未验证**：这是当前最大功能风险，不能用隔离 fprintd/in-process 结果替代系统端到端验收。

## 开发规则

1. 每个里程碑先写能捕获真实边界/状态错误的测试，再实现；构造人工输入不等于被测 C 实现已通过。
2. C 代码遵循上游 GLib 风格和资源所有权惯例，不引入独立线程框架、网络守护进程或第二套驱动抽象。
3. 每次修改运行受影响的离线用例；合并前检查完整净室基线及全部已实现测试。
4. 不将本 ROADMAP 当作新增设备协议来源；事实缺口必须回到规格维护流程。
