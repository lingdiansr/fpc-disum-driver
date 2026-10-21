# fpc-disum-driver

[English](README.md) | **简体中文**

Linux `libfprint` 驱动，支持 **FPC Disum** 指纹传感器（`10a5:9201`），即基于 FPC1022 的 112×88 按压式传感器。本仓库是 `libfprint` 的派生源码树，不是独立守护进程。

> **状态：实验性。** 在已验证设备上，in-process `libfprint` 的注册/验证可配合 SIGFM 工作；**fprintd/PAM 登录尚未完成端到端验证**。不要在未理解风险的情况下用它替换系统的 `libfprint` 来做日常认证。

## 设备支持

| 项目 | 值 |
|---|---|
| USB ID | `10a5:9201` |
| `bcdDevice` | `0x0250` |
| 已验证固件 | `021.26.2.050` |
| 传感器 | FPC1022 模块，112×88，8-bit，按压式 |
| 传输 | 厂商 USB 控制传输 + `0x82` bulk IN，TLS-PSK |

仅覆盖该设备/固件组合。其他 FPC ID、其他固件、DFU、异常复位恢复以及语义未定的 `0x64` 命令均**不支持**。

## 功能

- 初始化、TLS-PSK 通道、`ARM → EVT_FINGER_DWN → GET_IMG → 图像`。
- 取消、清理、close/reopen，以及同一次 open 会话内多次采集。
- **SIGFM** 匹配（OpenCV SIFT + ratio test + 几何一致性），阈值 10。这里不能使用 NBIS：112×88 图像通常只有 1–5 个细节点，低于 NBIS 的硬门槛 10。
- 覆盖引导注册：与已注册区域过近或完全无重叠的按压会作为重试上报，不消耗注册阶段。
- SIGFM print 的序列化/反序列化，可用于本地 `fprintd` 风格的存储。

## 依赖

- Linux、可访问传感器的 USB 权限、C/C++ 工具链、Meson、Ninja、pkg-config。
- GLib ≥ 2.68、GUsb、libusb-1.0、OpenSSL ≥ 3、OpenCV（4 或 5）、Pixman、libgudev。
- Python ≥ 3.11，用于仓库的生成/校验脚本。

示例包名：

```sh
# Arch
sudo pacman -S --needed base-devel meson ninja pkgconf glib2 libgusb libusb openssl opencv pixman libgudev

# Debian/Ubuntu
sudo apt install build-essential meson ninja-build pkg-config libglib2.0-dev \
  libgusb-dev libusb-1.0-0-dev libssl-dev libopencv-dev libpixman-1-dev libgudev-1.0-dev
```

## 构建

```sh
meson setup _build \
  -Ddrivers=fpc_disum \
  -Ddoc=false \
  -Dintrospection=false \
  -Dinstalled-tests=false \
  -Dgtk-examples=false \
  -Dudev_rules=disabled \
  -Dudev_hwdb=disabled
meson compile -C _build
```

构建产物包含带 `fpc_disum` 驱动的 `libfprint`。除非你显式用系统前缀执行 `meson install`，否则不会覆盖系统库。

## 测试

离线测试不需要传感器：

```sh
meson test -C _build --print-errorlogs
```

包含净室向量以及协议/状态机测试。硬件探针只能手动运行，且从不注册为自动测试：

```sh
LD_LIBRARY_PATH="$PWD/_build/libfprint" \
  "$PWD/_build/tests/fpc-disum-hw-probe" list

LD_LIBRARY_PATH="$PWD/_build/libfprint" \
  "$PWD/_build/tests/fpc-disum-enroll-probe" --verify-attempts 3
```

探针只写去敏 JSONL，不保存图像、密钥、TLS 载荷或原始总线抓包。

## 安装与打包

本仓库提供源码和 CI 构建/测试产物。**AUR 和发行版打包由下游维护，不在本仓库内进行**；这里没有替换系统 `libfprint` 的打包脚本。

如果安装构建出的库，会影响使用同一份 `libfprint` 的**所有**指纹设备；请保留发行版包以便回滚。`fprintd`/PAM 集成由下游或系统管理员负责。

## 项目结构

```text
libfprint/drivers/fpc_disum/   协议、TLS、USB、会话、诊断、图像驱动
libfprint/sigfm/               SIGFM 匹配器（OpenCV）
docs/                          已复核的净室输入 + 实现要求
tools/                         手动硬件与诊断探针
tests/                         离线协议、状态机与净室测试
```

## 净室输入

唯一已复核的协议输入是：

- `docs/CLEANROOM_SPEC.md`
- `docs/CLEANROOM_TEST_VECTORS.json`
- `docs/SHA256SUMS`

不要修改规格或测试向量。`docs/IMPLEMENTATION_REQUIREMENTS.md` 记录实现约束与验证状态；它不能替代已复核规格，也不能在未复核的情况下新增协议事实。

## 状态与限制

- 单设备 in-process 结果：覆盖引导注册 5/5，同指验证 3/3，异指验证 0/3。样本很小，不构成安全结论。
- `fprintd`/PAM 登录、多模板 `fprintd-identify`、多用户存储、suspend/resume 与 daemon 生命周期仍未验证。
- 小面积传感器每次按压的重叠区域有限，误拒率可能高于全尺寸传感器。
- SIGFM 模板包含可从图像重建的特征。请把存储的模板和任何采集图像视为生物识别敏感数据。

里程碑、证据与待办见 [`ROADMAP.md`](ROADMAP.md)。

## CI/CD

CI 构建离线矩阵（LLVM/GCC/sanitizers/仅编译），校验净室哈希，运行协议/状态机测试并产出源码/测试产物。硬件测试为**手动**，CI 不连接传感器。发布流程不发布 AUR 或发行版包。

## 参与贡献

- 提交改动前先运行离线测试；硬件结果必须标注为手动，并包含设备/固件、fprintd 版本和具体场景。
- 不要修改 `docs/CLEANROOM_SPEC.md`、`docs/CLEANROOM_TEST_VECTORS.json` 或 `docs/SHA256SUMS`；协议变更需要新的复核基线。
- 不要在 issue、commit 或 CI 产物中附上真实指纹图像、模板、PSK/envelope 数据或原始总线抓包。
- 驱动保持 LGPL-2.1-or-later，并遵循上游 libfprint 风格。

## 许可证与署名

本派生项目使用 **LGPL-2.1-or-later**；见 [`COPYING`](COPYING)。基于上游 `libfprint` v1.94.100（`80a4b5ec612892c5c056c48dddaa561452cf37ec`）。

SIGFM 及其 libfprint 集成来自公开的 libfprint merge request !530 与 !570；版权与许可证声明保留在 `libfprint/sigfm/`。NBIS/Bozorth3 仍遵循其上游条款。Fingerprint Cards 与 FPC 是其各自所有者的商标。

本软件不提供任何担保；生物识别认证属于安全敏感领域，本驱动尚未经过生产级安全审查。
