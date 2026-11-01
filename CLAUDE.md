## Agent skills

### Issue tracker

GitHub Issues on `FelipePrado0/SpartanPort` (`origin`); always pass `-R FelipePrado0/SpartanPort` to `gh`. See `docs/agents/issue-tracker.md`.

### Triage labels

Canonical roles mapped to this repo's labels (`needs-info` → `question`, `ready-for-agent` → `ready-to-implement`, others 1:1). See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: one `CONTEXT.md` plus `docs/adr/` at the repo root. See `docs/agents/domain.md`.

## Release policy — 发布动作默认禁止（硬规则）

**没有仓库主人在当前会话里的明确批准，不得创建/修改/删除任何 GitHub release，
不得推送任何 release 用的 tag，不得以发布为目的触发 workflow。**

发布物（Port 包 / AppImage）必须先由主人**在自己的设备上导入、实际跑过、并明确说可以发了**。
"构建成功""测试通过""CI 绿"都不是发布许可；实现自动化 ≠ 获得发布授权。

完整事故记录与细则（R1–R5）见 [`docs/incidents/2026-11-01-未授权发布-release.md`](docs/incidents/2026-11-01-未授权发布-release.md)。
本规则对所有 agent、**所有模型**生效——包括再犯的同类操作。
