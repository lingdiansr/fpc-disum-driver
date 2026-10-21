# fpc-disum-driver

**English** | [简体中文](README.zh-CN.md)

Linux `libfprint` driver for the **FPC Disum** fingerprint sensor (`10a5:9201`), the FPC1022-based 112×88 press sensor. This repository is a `libfprint` fork; it is not a standalone daemon.

> **Status: experimental.** In-process `libfprint`, an isolated fprintd daemon, and the **system fprintd** (via a temporary systemd drop-in) have passed enroll/verify/list/delete on the tested device. The system test was rolled back; **PAM/desktop login has not been validated end-to-end.** Do not replace your system `libfprint` for daily authentication without understanding the risk.

## Device support

| Item | Value |
|---|---|
| USB ID | `10a5:9201` |
| `bcdDevice` | `0x0250` |
| Firmware tested | `021.26.2.050` |
| Sensor | FPC1022 module, 112×88, 8-bit, press |
| Transport | vendor USB control + `0x82` bulk IN, TLS-PSK |
| Tested host | XIAOMI Redmi Book 14, board `TM2205` (SKU `TM2205-56978`), built-in sensor |

Only this device/firmware combination is covered. Other FPC IDs, other firmware, DFU, abnormal-reset recovery and the unknown `0x64` command are **not** supported.

Development and validation were performed on the host above (BIOS `RMAAD4B1P0808`, CachyOS / Linux 7.2.11, built-in sensor). The driver targets the sensor, not the laptop model; other hosts and modules are untested.

## Features

- Initialisation, TLS-PSK channel, ARM → `EVT_FINGER_DWN` → `GET_IMG` → image.
- Cancel, cleanup, close/reopen, and repeated capture in one open session.
- **SIGFM** matching (OpenCV SIFT + ratio test + geometric consistency), threshold 10. NBIS is not usable here: 112×88 captures usually yield 1–5 minutiae, below NBIS's hard floor of 10.
- Coverage-guided enrollment: a near-duplicate or non-overlapping press is reported as a retry and does not consume an enrollment stage.
- SIGFM print serialization/deserialization for local `fprintd`-style storage.

## Requirements

- Linux, USB access to the sensor, C/C++ toolchain, Meson, Ninja, pkg-config.
- GLib ≥ 2.68, GUsb, libusb-1.0, OpenSSL ≥ 3, OpenCV (4 or 5), Pixman, libgudev.
- Python ≥ 3.11 for the repository's generator/check scripts.

Example package names:

```sh
# Arch
sudo pacman -S --needed base-devel meson ninja pkgconf glib2 libgusb libusb openssl opencv pixman libgudev

# Debian/Ubuntu
sudo apt install build-essential meson ninja-build pkg-config libglib2.0-dev \
  libgusb-dev libusb-1.0-0-dev libssl-dev libopencv-dev libpixman-1-dev libgudev-1.0-dev
```

## Build

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

The build produces `libfprint` with the `fpc_disum` driver. It does **not** install over the system library unless you explicitly run `meson install` with a system prefix.

## Test

Offline tests need no sensor:

```sh
meson test -C _build --print-errorlogs
```

Cleanroom vectors and protocol/state-machine tests are included. Hardware probes are manual and never registered as tests:

```sh
LD_LIBRARY_PATH="$PWD/_build/libfprint" \
  "$PWD/_build/tests/fpc-disum-hw-probe" list

LD_LIBRARY_PATH="$PWD/_build/libfprint" \
  "$PWD/_build/tests/fpc-disum-enroll-probe" --verify-attempts 3
```

The probes write de-sensitised JSONL only; they do not store images, keys, TLS payloads or raw bus captures.

No-finger fprintd smoke (private D-Bus bus, test-only polkit, no system changes, no finger):

```sh
tools/fprintd-nofinger-smoke.sh /path/to/build
```

It requires `fprintd`, `dbus-run-session`, `busctl` and Python PyGObject; it covers enumeration, empty storage, delete-with-no-prints, verify-without-print, enrollment cancellation and daemon restart. Successful enroll/verify require a finger and are manual; PAM/desktop integration remains unverified.

## Install and packaging

This project ships source and CI build/test artifacts. **AUR and distribution packaging are maintained downstream, not in this repository**; there is no package script here that replaces system `libfprint`.

If you install the built library, it affects **all** fingerprint devices using the same `libfprint`; keep the distribution package available for rollback. `fprintd`/PAM integration is the downstream/administrator's responsibility.

## Project layout

```text
libfprint/drivers/fpc_disum/   protocol, TLS, USB, session, diagnostics, image driver
libfprint/sigfm/               SIGFM matcher (OpenCV)
docs/                          reviewed cleanroom input + implementation requirements
tools/                         manual hardware and diagnostic probes
tests/                         offline protocol, state-machine and cleanroom tests
```

## Cleanroom input

The only reviewed protocol inputs are:

- `docs/CLEANROOM_SPEC.md`
- `docs/CLEANROOM_TEST_VECTORS.json`
- `docs/SHA256SUMS`

Do not edit the spec or vectors. `docs/IMPLEMENTATION_REQUIREMENTS.md` records implementation constraints and verification status; it does not replace the reviewed spec or add protocol facts without review.

## Status and limitations

- In-process results on one device: 5/5 coverage-guided enrollment, 3/3 genuine verify, 0/3 impostor verify. Isolated fprintd: 5-stage enroll, same-finger `verify-match` (SIGFM 36/10), left-thumb `verify-no-match` (all templates 0/10), list/delete passed. System fprintd (temporary drop-in, rolled back) repeated enroll/verify/list/delete successfully. These are small samples, not a security claim.
- System PAM/desktop login, `fprintd-identify` with multiple templates, multi-user storage, suspend/resume and the full daemon lifecycle remain unverified.
- Small-area sensors have limited overlap between presses; expect higher false rejects than a full-size sensor.
- SIGFM templates contain image-derived features. Treat stored templates and any captured images as biometric data.

See [`ROADMAP.md`](ROADMAP.md) for milestones, evidence and open items.

## CI/CD

CI builds the offline matrix (LLVM/GCC/sanitizers/compile-only), verifies the cleanroom hashes, runs protocol/state-machine tests and produces source/test artifacts. Hardware tests are **manual**; CI has no sensor. Release engineering does not publish AUR or distribution packages.

## Contributing

- Run the offline test suite before sending changes; hardware results must be marked as manual and include device/firmware, fprintd version and the exact scenario.
- Do not change `docs/CLEANROOM_SPEC.md`, `docs/CLEANROOM_TEST_VECTORS.json` or `docs/SHA256SUMS`; protocol changes require a new reviewed baseline.
- Never attach real fingerprint images, templates, PSK/envelope data or raw bus captures to issues, commits or CI artifacts.
- Keep the driver LGPL-2.1-or-later and follow the upstream libfprint style.

## License and attribution

This fork is licensed under **LGPL-2.1-or-later**; see [`COPYING`](COPYING). It is based on upstream `libfprint` v1.94.100 (`80a4b5ec612892c5c056c48dddaa561452cf37ec`).

SIGFM and its libfprint integration are derived from the public libfprint work in merge requests !530 and !570; copyright and license notices are retained in `libfprint/sigfm/`. NBIS/Bozorth3 remains under its upstream terms. Fingerprint Cards and FPC are trademarks of their respective owners.

This software is provided without warranty; biometric authentication is security-sensitive and this driver has not received a production security review.
