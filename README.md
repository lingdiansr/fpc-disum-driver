# fpc-disum-driver — FPC 传感器 (10a5:9201) Linux 驱动（净室实现侧）

本仓库是**净室实现侧的 libfprint 派生源码树**：目标设备采用 **libfprint 内置图像驱动**路线，
仅依据下面两份交付文件实现设备协议，不接触研究侧材料。

项目/仓库标签与驱动同名：`fpc-disum-driver` / 驱动 ID `fpc_disum`。目标设备厂商标识是
Fingerprint Cards AB（`10A5:9201`），上游 libfprint 按传感器族命名驱动（`fpcmoc`、`egismoc`、`goodixmoc`…），
本侧只覆盖这一颗传感器与固件 `0x0250`，因此标识符里只放设备名。被验证的那台机器是小米笔记本（设备为其内置传感器），
这属于来源说明，不进入任何标识符（逐层命名对照见 ROADMAP「命名」）。

## 交付包（本仓库唯一允许的规格来源）

| 文件 | 大小 | SHA-256 |
|---|---:|---|
| `docs/CLEANROOM_SPEC.md` | 74,870 B | `f609db776be1fb340a7ee399d6bdc852a455ebc25d53ddfe941c9760cd0b341a` |
| `docs/CLEANROOM_TEST_VECTORS.json` | 12,781 B | `ce04ce43665f2b2d27142f2c2ecd917b09219a87a82d4b3009c023baecfeda94` |

校验（应与上表一致）：

```sh
sha256sum -c docs/SHA256SUMS
```

这两份文件是**已通过 10 轮隔离复核的版本**（第 10 轮结论：**可实现** —— 0 条阻塞项、无"缺少即无法实现"项；
**22/22** 向量可由包内信息独立重建；第 10 轮复核者自算的哈希与上表一致）。
> 说明：`IMPLEMENTATION_REVIEW.md` 是本侧针对**更早基线**（规格 `224aad0b…` / 向量 `a58c6a19…`）写的 CR 文档，
> 其 9 条已全部处置并计入当前版本（修订说明见研究侧 `docs/CR_RESPONSE_2026-10-17.md`）；**实现以上表基线与 `SHA256SUMS` 为准**。
**改动其中任何一份都会使该版本失去"已复核"状态** —— 若发现规格缺陷，请只描述现象（不要绕过规格猜设备行为），
由研究侧修订并重新复核后再升级基线。

## 隔离规则（保持净室有效性）

- 只依据 `docs/` 下这两份文件实现；**不要**读取研究侧仓库（`~/code/Other/fingerprint`）中的逆向记录、
  研究日志、私有抓包、Windows 产物或任何专有二进制/反编译输出。
- 遇到"规格没写、无法继续"的点：**停下并记录**（哪一步、缺什么参数），交回研究侧补齐，不要猜测设备行为。

## 从哪读起

- **契约本身**：`docs/CLEANROOM_SPEC.md`
  - 设备/端点与命令编码 §1/§3.1/§4.1；初始化序列 §6；TLS-PSK 通道与 envelope §8；
  - ARM → 等 `EVT_FINGER_DWN` → `GET_IMG` → 取图像：§4.5/§5；像素参数（偏移 24、stride 112、8-bit、坐标系为**本规格约定**）§5；
  - **失败、取消与清理契约 §16（逐条 MUST/不得）** —— 实现者的必做/禁做清单，最该先读；
  - 证据与类别索引 **附录 A**（每条要求是"设备行为/工程约定/本规格约定/静态来源"）；
  - 向量词汇与像素指标定义 **附录 B**。
- **自测（全程可不用真机）**：`docs/CLEANROOM_TEST_VECTORS.json` —— 22 个纯人工向量，含输入的重建描述与期望，
  可用于先跑通帧解析、TLS envelope 验签/解密、应用帧切分、像素判据与各类拒绝路径。

## 开发路线与当前状态

- [ROADMAP.md](ROADMAP.md)：M0 工程初始化、M0.1 质量工具链、M1 协议解析、M2 envelope、M3 TLS、M4 USB 状态机、M5 内置驱动、M6 受控验收。
- 上游：`https://gitlab.freedesktop.org/libfprint/libfprint.git`，tag `v1.94.100`，
  commit `80a4b5ec612892c5c056c48dddaa561452cf37ec`。保留上游 `COPYING`、版权及开发约定（LGPL-2.1-or-later）。
- **当前状态：M0–M5 已完成**：驱动已实现并注册 `fpc_disum` / `10a5:9201`（`bcdDevice=0x0250`）；**尚未在真机上验证**——受控硬件验收（M6）需另行授权。未在硬件上运行过之前，不宣称设备可用。
- 已实现 `libfprint/drivers/fpc_disum/fpc_disum_proto.c`（12B 大端外层帧、应用消息分帧、图像定位），不依赖 USB，由原生测试直接驱动。
- 已实现 `libfprint/drivers/fpc_disum/fpc_disum_tls.c`（GET_TLS_KEY envelope 验签与 PSK 解密，OpenSSL 3 的 AES-256-CBC NoPadding 与 HMAC-SHA256），失败时按规格阶段拒绝并擦除密钥材料。
- 同一模块提供 TLS-PSK 通道：宿主为 server，TLS 1.2 + `PSK-AES128-CBC-SHA256`，校验 `Disum PSK` 身份，memory BIO 驱动握手，输出按 64 B 上限分片；通道不自己计时，调用方决定超时，释放即取消。
- 已实现 `libfprint/drivers/fpc_disum/fpc_disum_session.c`（单次会话状态机）：§6 初始化序列与 `CMD_INIT` 后换句柄、§8 握手、§4.5 `ARM`→触摸→`GET_IMG`→取图、§16 失败/取消/清理。它只通过 `fpc_disum_transport.h` 的传输边界访问设备，因此可在没有硬件的情况下被合成事件完整验收。
- 已实现 `libfprint/drivers/fpc_disum/fpc_disum_usb.c`：该边界的 GUsb/FpiUsbTransfer 实现（vendor 控制 OUT/IN、0x82 bulk 读、`CMD_INIT` 后的关句柄/重开/重新 claim、先读当前配置以避免冗余 `SET_CONFIGURATION`）。该文件目前只做静态验证：三套构建下编译通过，设备侧行为需 M6 授权实验。
- 已实现 `libfprint/drivers/fpc_disum/fpc_disum.c`：`FpImageDevice` 适配层（`img_open`/`img_close`/`activate`/`deactivate`/`change_state`）。open 建新会话并跑 §6+§8，取图完成时用真实图像回调上报，close 走 §16.3 清理并释放密码学与传输资源；`usb_discover` 只接受约定固件版本。
- 已实现 `libfprint/drivers/fpc_disum/fpc_disum_error.c`：会话结果→libfprint 错误映射，取消／窗口到期／USB 失败／协议与安全失败各自成类（规格 §16.2/§16.3 要求可区分）。
- 该适配层与核心的交互**无法离线驱动**（上游没有可注入的假 `GUsbDevice`，umockdev 回放承载不了交互式 TLS 握手）：离线验收覆盖会话层与错误映射，适配层由评审 + M6 真机覆盖，详见 ROADMAP M5 的"未验证与范围"。
- 上游框架及其他型号驱动不是本传感器的协议来源；原有公开录制测试数据不作为本项目的净室验收输入。
- Git 本地开发分支为 `main`，上游 remote 为 `upstream`。不自动提交本侧修改，也不执行 push。
- 本侧对上游的必要构建修正：`tests/meson.build` 的无 GI 分支使用键、值双变量遍历驱动字典，以支持固定版本 Meson 的 C-only 配置；未改变测试行为。
- 本侧对上游的回调修正：虚拟图像驱动的延迟 open/close、原生 SSM 清理及 identify 测试使用签名匹配的回调，不改变 libfprint API，不关闭 UBSan 的函数类型检查。

## 本地开发与质量检查（无真实采集）

保留上游 GNU C99、GLib 风格及 `scripts/uncrustify.cfg`，不换构建系统、不全库重排格式。日常使用 LLVM，sanitizer 和 GCC 兼容性使用独立构建目录：

| 配置 | Meson native files | 构建目录 | 用途 |
|---|---|---|---|
| LLVM | `common.ini` + `llvm.ini` | `_build/llvm` | Clang/LLD、clangd 编译数据库、静态分析 |
| 严格 sanitizer | `common.ini` + `llvm.ini` + `sanitizers.ini` | `_build/sanitizers` | ASan + 全量 UBSan，首个 UBSan 错误即失败 |
| GCC | `common.ini` + `gcc.ini` | `_build/gcc` | GNU C99/GCC/BFD 兼容构建 |
| 含本驱动 | `common.ini` + `device.ini` | `_build/device` | 编译并链接 `fpc_disum`（供 M6 真机使用）；会枚举设备的命令在此配置下可能认领真实传感器 |

上述文件位于 `dev/native/`。三个开发配置只编译虚拟驱动（隔离要求），`device.ini` 额外把它加入驱动集。全部关闭可选 GI/文档/安装测试及 udev 生成，安装前缀固定在本地 `_build/stage`，不替换系统 libfprint/fprintd。

需要 Clang/Clang++、LLD、clang-tidy、GCC/G++、Ninja、pkg-config、GLib ≥2.68、GUsb ≥0.2、OpenSSL ≥3.0；clangd 用于编辑器导航。sanitizer 配置还需要匹配的 Clang runtime。Python ≥3.11 用于基线检查，`uv` 用于运行固定版本 Meson。sccache 可选，不是质量门禁依赖。

本机缺少 `g-ir-scanner`，因此这些 C-only 配置不生成 GI 语言绑定；C 库及原生测试仍启用。如需 GI 绑定，先安装对应开发工具，再显式配置 `-Dintrospection=true`，不要把缺少可选工具改成假成功。

### 配置构建（三个开发配置 + 一个含驱动的配置）

```sh
python3 tools/check-cleanroom.py

uv tool run --from meson==1.12.1 meson setup _build/llvm \
  --native-file dev/native/common.ini --native-file dev/native/llvm.ini

uv tool run --from meson==1.12.1 meson setup _build/sanitizers \
  --native-file dev/native/common.ini --native-file dev/native/llvm.ini \
  --native-file dev/native/sanitizers.ini

uv tool run --from meson==1.12.1 meson setup _build/gcc \
  --native-file dev/native/common.ini --native-file dev/native/gcc.ini

# 只用于编译本驱动与不枚举设备的用例；会枚举设备的命令可能认领真实传感器。
uv tool run --from meson==1.12.1 meson setup _build/device \
  --native-file dev/native/common.ini --native-file dev/native/device.ini
```

编译器在首次配置时固定；不要在同一构建目录内切换编译器或混用插桩产物。旧 `_build/dev` 和早期诊断目录不是以上配置的别名，不再作为本文开发入口。

### 构建与同一组离线验收

```sh
for profile in llvm sanitizers gcc; do
  uv tool run --from meson==1.12.1 meson compile -C "_build/$profile" || exit $?
  uv tool run --from meson==1.12.1 meson test -C "_build/$profile" \
    --suite cleanroom --print-errorlogs || exit $?
  uv tool run --from meson==1.12.1 meson test -C "_build/$profile" \
    fpc-disum-protocol fpc-disum-tls fpc-disum-channel fpc-disum-session \
    fpc-disum-error \
    fpi-device fpi-ssm fp-context fp-device \
    --print-errorlogs || exit $?
done
```

含驱动的配置只编译与跑不枚举设备的用例（编译即验证驱动与注册，跑用例验证同一批离线断言）：

```sh
uv tool run --from meson==1.12.1 meson compile -C _build/device
uv tool run --from meson==1.12.1 meson test -C _build/device --suite cleanroom
uv tool run --from meson==1.12.1 meson test -C _build/device \
  fpc-disum-protocol fpc-disum-tls fpc-disum-channel fpc-disum-session fpc-disum-error
```

`tools/check-cleanroom.py` 同时核对**源码中固定的已复核哈希**、本地清单及实际文件；只改写 `docs/SHA256SUMS` 不能将未经复核的规格变成有效输入。Meson 配置和 `cleanroom` 测试组均执行该检查。

`tools/fpc_disum_vectors.py` 从声明式描述重建全部 22 条向量，并与录制期望逐项比对（外层帧、应用消息、像素指标、envelope 结构）；构建时再用它生成 `tests/fpc-disum-corpus.c`，供原生 `test-fpc-disum-protocol` 驱动真实解析器。重建器不访问设备、密钥或真实图像，`cleanroom-vectors` / `cleanroom-vector-regressions` 两个测试防止它空过。

sanitizer 测试默认使用 `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`；保留 `address,undefined` 检查，不使用 `-fno-sanitize` 或 suppression 隐藏诊断。`b_lundef=false` 仅用于 ASan 共享库链接，运行时仍由最终测试可执行文件提供 sanitizer runtime。

原生验收限于状态机、会话生命周期、错误映射、协议解析和虚拟上下文，不运行读取上游录制指纹图像的 assembling/capture 用例。`FpImageDevice` 适配层与真实设备**都未离线覆盖**：不要将虚拟驱动或解析器测试通过解释为 FPC 设备采图已通过。

### 定向静态分析与编辑器

```sh
clang-tidy --verify-config
ninja -C _build/llvm clang-tidy
```

Meson 生成的 Ninja 内置 `clang-tidy` 目标读取 `.clang-tidy` 和 `.clang-tidy-include`（应通过 Ninja 调用，不作为 `meson compile` 的项目目标传入）。分析范围为本次修正的虚拟图像驱动/原生测试，以及未来加入构建的 `fpc_disum` 源码和对应测试。启用的 analyzer/bugprone 诊断作为错误；不自动应用修复、不把静态分析范围冒充整个上游代码库。

`.clangd` 指向 `_build/llvm/compile_commands.json`。上游已有的编译器告警仍原样显示，不通过全局静默宣称 warning-free。**uncrustify（`scripts/uncrustify.cfg`）仍是唯一权威格式规范**；根目录 `.clang-format` 只复刻同一 GNU/GLib 风格，避免编辑器或自动格式化钩子把新文件改成别种风格，不用于全库重排。

协议解析器已经存在。下一步可按 ROADMAP 用净室向量语料接入 fuzzing（尚未接入，当前门禁是向量、原生测试与静态分析）；fuzzing 不使用真实图像或密钥。

真实采集、密钥读取、复位、电源策略、系统部署及认证实验均需单独授权。以上命令不执行安装或远程 CI 发布。

