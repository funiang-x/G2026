# final-20260930 · AFE 修复与 C14 落件（B12）快照说明

> 本轮改动：① 修复 U1A 输出并入 `+5V_A`（整条折线删 + 分段重建）
> ② 修复 `R7` 落点 AGND→VBIAS ③ 新增 `C14`（100nF 去耦）。
> 相关台账：skill `easyeda-schematic-net-fanout` 的 `LESSONS.md` T-49~T-53。

## 快照清单（按时间序）

| 文件 | 状态 | 字符数 | 回滚用途 |
|---|---|---|---|
| `src_AFE_mid_fix.txt` | **B 前**：A 已修（折线已删+3 条新线已建）、B 未做（R7 仍在 AGND） | 509149 | 回滚到"仅 B 未做" |
| `src_AFE_after_fix.esch2` | **A+B 后、C14 前** | 507380 | 回滚到"C14 未加" |
| `src_AFE_after_c14.esch2` | **最终态**（A+B+C14） | 525531 | 当前基线 |

## ⚠️ 诚实声明：真·pre 快照被覆盖（对应台账 T-53）

第 ① 步（修 A）之前的完整快照（508661 字符）在流程中被 `--apply-b` 的自动备份**覆盖**
（备份脚本每次运行都重写同一路径）。当前**没有**一份"A 之前"的整页快照。

- **若需回滚到 A 之前**：以 `src_AFE_mid_fix.txt` 为基础，手工把"U1A 输出节点的
  3 条新线"换回原折线即可——原折线几何完整记录在
  `../afe_flags_live.json`（本轮首次 dump）与 `13-底板定稿决策.md` §5.8 缺陷①表格里。
- **向前回滚**（更常用）：直接 `setDocumentSource` 写回对应快照整页文本。

## 复现命令（本机）

```powershell
cd projects/G2026/hardware
python _fix_afe_ab.py            # 干跑复核（A/B 的对象核查）
python _add_c14.py step2         # 回读 C14（pid/引脚/值）
python "C:/Users/funiang/.ai-skills/easyeda-sch-audit-fix/scripts/audit_sch.py" --page AFE
python "C:/Users/funiang/.ai-skills/easyeda-schematic-net-fanout/scripts/eda_sch.py" drc --all --baseline ../_review/drc_base_pre_afe_fix.json
```
