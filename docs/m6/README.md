# M6 文档索引

本目录只保留与当前实现和验收状态一致的文档。净室冻结包仍在 `docs/` 根下，不可移动：

- `docs/CLEANROOM_SPEC.md`
- `docs/CLEANROOM_TEST_VECTORS.json`
- `docs/SHA256SUMS`

当前 M6 文档：

- [SUPPORT_DECLARATION.md](SUPPORT_DECLARATION.md)：支持范围、五项真机验收 run、未支持项、已知偏离；§6 记录 M7.2 的 SIGFM/覆盖引导增量。
- [ARM_GATE_NOTE.md](ARM_GATE_NOTE.md)：净室侧补充——ARM 是 `EVT_FINGER_DWN` 的前提；ARM 返回 4 B 不是武装确认；ARM 时机与 CMD_INIT 载荷已排除。
- [INIT_READ_GAP_FINDING.md](INIT_READ_GAP_FINDING.md)：零触摸根因——初始化/重枚举期间必须持续读 bulk，否则 `EVT_INIT_RESULT` 丢失。

项目入口与进度：

- [README.md](../../README.md)
- [ROADMAP.md](../../ROADMAP.md)

已被 M6 完成声明取代的历史交回/复评文档已从工作树删除，仍可从 git 历史恢复：`M6_VERIFICATION_REVIEW/RESPONSE`、`M6_CAPTURE_HANDOFF_REPORT/RESPONSE`、`M6_CAPTURE_ROUND6_HANDOFF`、`M6_DEVICE_RESET_REPORT/RESPONSE`、`IMPLEMENTATION_REVIEW`。当前结论以本目录三份文档和 ROADMAP M6 顶部完成记录为准。
