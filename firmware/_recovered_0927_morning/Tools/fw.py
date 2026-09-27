#!/usr/bin/env python3
"""fw —— 本工程的一体化工具链入口（五条链收在这一个命令里）。

    ┌ 源码 ──[1 GCC]──> .o ──[2 Ninja/CMake]──> .elf/.bin/.hex
    │                                              │
    │            ┌─────────────────────────────────┼──────────────────────────┐
    │            ↓                                 ↓                          ↓
    │       [3 J-Link SWD]                       [4 RTT]                    [5 GDB]
    │         烧进 Flash                       读日志 / 取证                断点 / 单步
    └─ 全部子命令都能被 AI 直接驱动：**退出码即结论**，不需要人肉读屏。

设计约束
--------
· 只依赖**标准库** + arm-none-eabi 工具链 + SEGGER 官方命令行工具。
· 路径与器件名**不硬编码**：全部来自 CMake 生成的 `build/<预设>/board.env`
  （那份文件又由 `Core/Inc/board_config.h` 供出器件名与内存容量）。
  ⇒ 换板子改 `board_config.h`，这里不用动。
· 不引入 fetch_deps / 代码生成 / 芯片切换这类"大模板"才需要的环节
  （取舍理由见 docs/00 §1.4）。

用法
----
    python Tools/fw.py env                       # 体检：工具链版本 / J-Link / **探针是否在线** / 各预设构建目录
    python Tools/fw.py build [--preset Debug]    # 编译
    python Tools/fw.py build --clean-first       # ★ 判"零告警"只能用它（见 docs/11 §1）
    python Tools/fw.py flash [--no-build]        # 编译(可选) + 烧录 + 复位运行
    python Tools/fw.py reset                     # 只复位（不烧录 ⇒ 不是不可逆操作）
    python Tools/fw.py rtt  [--duration 10] [--channel 0]
    python Tools/fw.py gdb  [--server-only]      # JLinkGDBServerCL + arm-none-eabi-gdb
    python Tools/fw.py verify [--duration 8]     # 闭环：lint → build → 体积门禁 → flash → 抓 RTT → 判标记
    python Tools/fw.py size                      # 只报体积（口径唯一来源：Tools/stm32_size.py）
    python Tools/fw.py sizecheck                 # 体积预算门禁
    python Tools/fw.py test                      # ★ PC 侧单测（host 编译，不需要板子）
    python Tools/fw.py lint                      # 静态检查：.ioc / board_config.h / HAL 三处配置对账
    python Tools/fw.py doccheck                  # 文档一致性：死链 + 陈旧标识符
    python Tools/fw.py selftest [--full]         # 门禁自检：基线 + 负向用例 + 正向用例
    python Tools/fw.py format [--check]          # 用 .clang-format 统一格式（只碰手写代码）
    python Tools/fw.py clean                     # 只清 build/，源码与 Drivers/ 一概不动

    --preset 写在子命令前或后都可以（内部先摘出来，绕开 argparse 的位置限制）：
        python Tools/fw.py --preset Release size  ≡  python Tools/fw.py size --preset Release

退出码约定（供自动化判断）
----
    0  成功（verify 下 = 检测到 [AI_READY]）
    1  固件主动报了 [AI_FAIL]
    2  超时：两个标记都没看到（⚠️ 不等于"固件有问题" —— 也可能是探针没插）
    3  编译失败
    4  烧录失败
    5  RTT 工具不可用 / 抓不到数据
    6  环境不完整（找不到 J-Link、缺产物、没 configure 等）
    7  静态检查未通过（lint / doccheck / selftest）
    8  资源超预算（体积门禁）
    9  PC 侧单测失败 **或一个用例都没注册**（"跳过 ≠ 通过"，见 cmd_test）

为什么 verify 把门禁排在烧录之前
----
lint / 体积门禁都是**秒级且不用碰硬件**的。配置层面就矛盾、或体积已超预算时，
烧录是白费 —— 而且烧录是不可逆操作（docs/12 §B），不该拿它当"顺便检查一下"。
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import stm32_size  # noqa: E402  （同目录共享模块，体积口径的唯一来源）

# ------------------------------------------------------------------ 常量
ROOT = Path(__file__).resolve().parent.parent
DEFAULT_PRESET = "Debug"
JLINK_SPEED = "4000"  # kHz；SWD 下 4MHz 很稳，长线/干扰大可降到 1000
# STM32 主 Flash 起始地址。这是 **Cortex-M 的架构约定**（全系 STM32 都是它），
# 不是板级事实 ⇒ 可以硬编码；板级差异（容量/器件名）才走 board.env。
FLASH_BASE = "0x08000000"
GDB_PORT = "2331"
HOST_BUILD_DIR = "host"  # build/host —— PC 侧单测，与 MCU 的 build/<预设>/ 分开

OK, FAIL_MARKER, TIMEOUT, BUILD_FAIL, FLASH_FAIL, RTT_FAIL, ENV_FAIL = 0, 1, 2, 3, 4, 5, 6
LINT_FAIL = 7
SIZE_FAIL = 8
TEST_FAIL = 9

# 只格式化**手写代码**。`Core/`（除 board_config.h）、`Drivers/`、`Middlewares/`
# 是生成物 / vendor —— 格式化它们等于给 CubeMX 添乱（下次重新生成会冲突，
# 而且 diff 噪声巨大）。
FORMAT_GLOBS = ("Common/**/*.[ch]", "Device/**/*.[ch]", "Task/**/*.[ch]",
                "Operation/**/*.[ch]", "Test/**/*.[ch]")

# ⚠️ 这两个标记是**跨文件契约**：定义在 Core/Inc/board_config.h，
#    消费方是这里、.vscode/tasks.json 与 AGENTS.md。改一处必须改全部，
#    否则自动化会静默失效（docs/11 §5）。
BOOT_OK_MARKER = "[AI_READY]"
BOOT_FAIL_MARKER = "[AI_FAIL]"


def log(msg: str) -> None:
    print(msg, flush=True)


# ------------------------------------------------------------------ 环境探测
def find_jlink_dir() -> Path | None:
    """找 J-Link 安装目录（内含 JLink.exe / JLinkRTTLogger.exe / JLinkGDBServerCL.exe）。

    查找顺序：环境变量 JLINK_DIR > 常见安装路径（取版本号最大的）> PATH。
    """
    candidates: list[Path] = []
    env = os.environ.get("JLINK_DIR")
    if env:
        candidates.append(Path(env))
    for base in (r"C:\Program Files\SEGGER", r"C:\Program Files (x86)\SEGGER"):
        p = Path(base)
        if p.is_dir():
            candidates += sorted(
                [d for d in p.iterdir() if d.is_dir() and d.name.startswith("JLink")],
                key=lambda d: d.name,
                reverse=True,
            )
    for c in candidates:
        if (c / "JLink.exe").is_file():
            return c
    exe = shutil.which("JLink.exe")
    return Path(exe).parent if exe else None


def load_board_env(preset: str) -> dict[str, str]:
    """读 cmake configure 阶段生成的 build/<preset>/board.env（路径与器件名的唯一来源）。"""
    env_file = ROOT / "build" / preset / "board.env"
    if not env_file.is_file():
        return {}
    out: dict[str, str] = {}
    for line in env_file.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.lstrip().startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        out[k.strip()] = v.strip()
    return out


def resolve_config(preset: str) -> dict[str, str]:
    """合并 board.env 与默认值，得到本次操作需要的全部路径/参数。"""
    be = load_board_env(preset)
    build_dir = ROOT / "build" / preset
    name = be.get("PROJECT_NAME", "f407vgt6")
    return {
        "device": be.get("JLINK_DEVICE", "STM32F407VG"),
        "mcu_name": be.get("MCU_NAME", ""),
        "project": name,
        "elf": be.get("ELF", str(build_dir / f"{name}.elf")),
        "hex": be.get("HEX", str(build_dir / f"{name}.hex")),
        "bin": be.get("BIN", str(build_dir / f"{name}.bin")),
        "log_backend": be.get("LOG_BACKEND", "RTT"),
        # ★ 链接脚本也走 board.env：体积口径（Tools/stm32_size.py）按它的 MEMORY 区
        #   算占用，硬编码路径在换 .ld 后会**静默**按错的内存区算数。
        "ld": be.get("LD", str(ROOT / "STM32F407XX_FLASH.ld")),
        "build_dir": str(build_dir),
    }


def _is_configured(preset: str) -> bool:
    """工程是否已 configure —— 判据是 CMake 自己写的 build/<预设>/CMakeCache.txt。

    与 load_board_env() 用同一套路径推导（build/<preset>/），别再写第三种。
    """
    return (ROOT / "build" / preset / "CMakeCache.txt").is_file()


def run(cmd: list[str], cwd: Path | None = None) -> int:
    log("  $ " + " ".join(cmd))
    try:
        return subprocess.call(cmd, cwd=str(cwd) if cwd else None)
    except FileNotFoundError:
        log(f"  !! 命令不存在: {cmd[0]}")
        return 127


def probe_list(jlink: Path) -> list[str]:
    """跑一次 ShowEmuList，返回探针条目（空列表 = 没插探针）。

    ★ 为什么把它做成独立函数并被 `env` 调用：**"探针在不在"是排查任何硬件
      问题的第一手信息**，而 docs/11 §2 的排查顺序第 1 步就是"看探针是否列出"。
      原先 `env` 只打印 J-Link 的安装路径 —— 装了软件 ≠ 插了探针，
      这两件事在"烧录失败"的场景里恰恰要分开看。
    ⚠️ 脚本要写成 **Windows 路径**：JLink.exe 不认 MSYS 的 /tmp/xxx。
    """
    tool = jlink / "JLink.exe"
    if not tool.is_file():
        return []
    scratch = ROOT / "build"
    script = scratch / "_emulist.jlink"
    try:
        scratch.mkdir(parents=True, exist_ok=True)
        script.write_text("ShowEmuList\nexit\n", encoding="ascii")
        p = subprocess.run([str(tool), "-CommanderScript", str(script)],
                           capture_output=True, text=True, timeout=60, errors="replace")
        out = (p.stdout or "") + (p.stderr or "")
    except Exception as exc:  # noqa: BLE001 —— 探针探测失败不该让 env 崩
        return [f"!! 探测失败: {exc}"]
    finally:
        try:
            script.unlink()
        except OSError:
            pass

    # ShowEmuList 的输出形如：
    #   J-Link[0]: Connection: USB, Serial number: 20090928, ProductName: J-Link V9 compiled ...
    return [ln.strip() for ln in out.splitlines() if "J-Link[" in ln]


# ------------------------------------------------------------------ 子命令
def cmd_env(args) -> int:
    """一条命令给全部答案：工具链版本 / J-Link 路径 / 器件 / 各预设构建目录。

    ⚠️ 需要环境事实时问它 —— **禁止**为了"确认环境"反复 ls / which / 读 CMakeLists。
    """
    jlink = find_jlink_dir()
    cfg = resolve_config(args.preset)

    log("== 工具链 ==")
    for name, exe in (
        ("arm-none-eabi-gcc", "arm-none-eabi-gcc"),
        ("arm-none-eabi-gdb", "arm-none-eabi-gdb"),
        ("arm-none-eabi-readelf", "arm-none-eabi-readelf"),
        ("cmake", "cmake"),
        ("ninja", "ninja"),
        ("clang-format", "clang-format"),
    ):
        path = shutil.which(exe)
        log(f"  {name:<24} {path if path else '!! 未找到'}")

    log("== 依赖（都在工程内，不需要联网拉取）==")
    deps = {
        "ST HAL": "Drivers/STM32F4xx_HAL_Driver/Inc/stm32f4xx_hal.h",
        "CMSIS": "Drivers/CMSIS/Include/core_cm4.h",
        "FreeRTOS": "Middlewares/Third_Party/FreeRTOS/Source/include/task.h",
        "SEGGER RTT": "Middlewares/RTT/SEGGER_RTT.c",
    }
    all_deps_ok = True
    for label, rel in deps.items():
        ok = (ROOT / rel).is_file()
        all_deps_ok &= ok
        log(f"  {label:<24} {'齐全' if ok else '!! 缺失: ' + rel}")

    log("== J-Link ==")
    log(f"  安装目录                 {jlink if jlink else '!! 未找到（设 JLINK_DIR 或装 J-Link 软件包）'}")
    if jlink:
        log(f"  JLink.exe                {'有' if (jlink / 'JLink.exe').is_file() else '!! 缺'}")
        log(f"  JLinkRTTLogger.exe       {'有' if (jlink / 'JLinkRTTLogger.exe').is_file() else '!! 缺'}")
        log(f"  JLinkGDBServerCL.exe     {'有' if (jlink / 'JLinkGDBServerCL.exe').is_file() else '!! 缺'}")

    # ★ 装了软件 ≠ 插了探针。这两件事必须分开看 —— 烧录失败时先看这里。
    log("== 探针（ShowEmuList）==")
    probes = probe_list(jlink) if jlink else []
    if not probes:
        log("  !! 没有探针在线（J-Link 软件装着，但没插 / 没被识别）")
        log("     ⇒ 烧录 / RTT / GDB 都做不了。接上探针与目标板（SWDIO/SWCLK/GND）再试。")
    else:
        for ln in probes:
            log(f"  {ln}")

    log("== 目标 ==")
    log(f"  器件                     {cfg['device']}（{cfg['mcu_name']}）")
    log(f"  当前预设                 {args.preset}")
    log(f"  构建目录                 {cfg['build_dir']}")
    log(f"  日志后端                 {cfg['log_backend']}")

    log("== 各预设构建目录 ==")
    for p in ("Debug", "Release", "RelWithDebInfo", "Debug-UART", HOST_BUILD_DIR):
        d = ROOT / "build" / p
        log(f"  {p:<16} {'已配置' if (d / 'CMakeCache.txt').is_file() else '未配置'}")

    log("== CubeMX ==")
    ioc = next(iter(ROOT.glob("*.ioc")), None)
    log(f"  工程文件                 {ioc.name if ioc else '!! 没找到 .ioc'}")

    ready = bool(jlink) and all_deps_ok
    log(f"\n结论: {'环境就绪' if ready else '环境不完整'}")
    return OK if ready else ENV_FAIL


def cmd_build(args) -> int:
    # ★ build 之前必须先确认"配置过了"。
    #   `cmake --build --preset X` 在 build/<X>/ 不存在时直接非 0 退出
    #   （底层报 "build/Debug is not a directory"），而这里会把它翻译成
    #   **"编译失败"** —— 看着像源码有问题，其实只是没 configure。实测踩到过。
    #   ⇒ 缺 CMakeCache.txt 就先 configure，省掉一条"必须自己记着"的前置命令
    #     （verify 的第 2 步也走这里，所以闭环在新工程上同样开箱可用）。
    #   ⚠️ 这里**不**自动加 --fresh：需要 --fresh 的场景（改了工具链 flags）
    #      语义上要求用户显式表达，静默重配会把缓存问题藏起来。
    if not _is_configured(args.preset):
        log(f"未找到 build/{args.preset}/CMakeCache.txt —— 先配置工程")
        if run(["cmake", "--preset", args.preset], cwd=ROOT) != 0:
            log("配置失败（cmake configure 未通过）")
            return BUILD_FAIL

    cmd = ["cmake", "--build", "--preset", args.preset]
    if args.clean_first:
        cmd.append("--clean-first")
    if run(cmd, cwd=ROOT) != 0:
        log("编译失败")
        return BUILD_FAIL

    elf = Path(resolve_config(args.preset)["elf"])
    log(f"编译成功: {elf}")
    if args.clean_first:
        log("（--clean-first：本次告警统计才是有意义的 —— 增量构建会漏报，见 docs/11 §1）")
    return OK


def cmd_size(args) -> int:
    cfg = resolve_config(args.preset)
    return _size_impl(cfg, check=False, preset=args.preset)


def cmd_sizecheck(args) -> int:
    cfg = resolve_config(args.preset)
    return _size_impl(cfg, check=True, preset=args.preset)


def _size_impl(cfg: dict[str, str], check: bool, preset: str) -> int:
    """体积明细 / 门禁。★ 口径唯一来源是 Tools/stm32_size.py，这里只翻译退出码。"""
    elf, ld = Path(cfg["elf"]), Path(cfg["ld"])
    if not elf.is_file():
        log(f"!! 找不到固件 {elf}，先执行：python Tools/fw.py build --preset {preset}")
        return ENV_FAIL
    if not shutil.which("arm-none-eabi-readelf"):
        log("!! 找不到 arm-none-eabi-readelf（随 gcc 工具链提供），无法算内存占用")
        return ENV_FAIL

    used, text_size = stm32_size.memory_usage(elf, ld)
    regions = stm32_size.read_memory_regions(ld)
    if not used:
        log(f"!! 解析链接脚本内存区失败：{ld}")
        return ENV_FAIL

    log(stm32_size.format_report(used, regions, text_size))
    if not check:
        return OK

    bad = stm32_size.check_budget(used, regions)
    if bad:
        log("!! 体积超预算：")
        for b in bad:
            log(f"   · {b}")
        log(f"   预算表在 Tools/stm32_size.py 的 BUDGETS。"
            f"\n   ⚠️ 调高上限 = 拆门禁。要调先把理由写进 docs/12 的授权记录。")
        return SIZE_FAIL
    log("体积门禁通过")
    return OK


def cmd_test(args) -> int:
    """PC 侧单测：configure → 编译 → ctest（**不需要板子**）。

    为什么要把它做成 fw.py 的子命令：四层架构里 `Operation/` 被设计成"纯逻辑"
    （吃数字吐数字），目的就是能在电脑上验证。如果这一步要手工敲三条命令，
    它迟早会被跳过 —— 而"被跳过的验证"等于没有验证。
    ⇒ 对 AI 而言这是**最重要的反馈回路**：写完业务逻辑立刻能自证对错。

    ⚠️ 它同时是**规矩 2 的第二道墙** —— 谁在 `Operation/` 里偷用硬件函数，
       这里会报 `undefined reference to vTaskDelay`（见 docs/01 §强制表）。

    ⚠️ **"跳过"不等于"通过"**：`Operation/Src/` 为空时 ctest 会跑出 0 个用例并
       退出码 0 —— 那意味着规矩 2 的强制**未经验证**。这里把它单独判成 9
       （与断言失败同一个码），并在输出里说清是哪种。
    """
    src = ROOT / "Test"
    bld = ROOT / "build" / HOST_BUILD_DIR
    if not (src / "CMakeLists.txt").is_file():
        log(f"!! 找不到 {src / 'CMakeLists.txt'}（Test/ 是 PC 侧测试工程）")
        return ENV_FAIL

    log("[1/3] configure（host 工具链，与 MCU 的 build/<预设>/ 完全分开）")
    cfg = ["cmake", "-S", str(src), "-B", str(bld),
           "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Debug"]
    rc = subprocess.call(cfg, cwd=str(ROOT))
    if rc != 0:
        # ⚠️ 目录搬过家（平目录 / 换机器 / 换克隆路径）之后，CMakeCache.txt 里
        #    存的是**旧绝对路径**，CMake 拒绝复用，报的却是
        #    "The source ... does not match the source ... used to generate cache"
        #    —— 那句话指向 CMake 内部，很难联想到"只是路径变了"。
        #    实测（2026-09-22 平目录）：build/host 有 6 处旧路径，
        #    Release / RelWithDebInfo / Debug-UART 各 7 处 —— 五个目录全中。
        #    ⇒ 自动 --fresh 重建缓存（需 CMake ≥ 3.24），让工具自愈。
        log("      configure 失败 —— 尝试 --fresh 重建缓存"
            "（目录搬过家时，旧 cache 里的绝对路径会让 CMake 拒绝复用）")
        rc = subprocess.call(cfg + ["--fresh"], cwd=str(ROOT))
    if rc != 0:
        log("!! configure 失败 —— 检查 host 上有没有 cmake / ninja / C 编译器")
        return BUILD_FAIL

    log("[2/3] 编译测试")
    if subprocess.call(["cmake", "--build", str(bld)], cwd=str(ROOT)) != 0:
        log("!! 测试工程编译失败（这一步的告警也是失败 —— 与 MCU 侧同一套告警）")
        return BUILD_FAIL

    log("[3/3] 运行")
    rc = subprocess.call(["ctest", "--output-on-failure"], cwd=str(bld))
    if rc != 0:
        log(f"PC 单元测试失败（退出码 {rc}）—— 看上面的输出，那是断言失败点")
        return TEST_FAIL

    # ctest 成功但一个用例都没有 = 静默失效，必须单独判死
    probe = subprocess.run(["ctest", "-N"], cwd=str(bld),
                           capture_output=True, text=True)
    if "Total Tests: 0" in probe.stdout or "0 tests" in probe.stdout.lower():
        log("!! 一个用例都没注册 —— **跳过 ≠ 通过**：")
        log("   Operation/ 的规矩 2 强制因此**未经验证**。")
        log("   去 Test/CMakeLists.txt 按文件头的说明加用例（它刻意不写 GLOB）。")
        return TEST_FAIL

    log("PC 单元测试通过（Operation 层在 host 上验证完毕）")
    return OK


def _flash_once(jlink: Path, cfg: dict[str, str], erase: bool = False) -> int:
    """用 JLink Commander 脚本模式烧录。脚本按需生成，避免路径硬编码。

    ⚠️ erase=True 会先 `erase`（全片擦除）—— 抹掉目标 Flash 上的一切。
       docs/12 把它归到「AI 不碰」档；这个开关只为人留的。
    """
    script = Path(cfg["build_dir"]) / "flash.jlink"
    script.parent.mkdir(parents=True, exist_ok=True)
    bin_ = Path(cfg["bin"]).as_posix()
    # ★ 用 loadbin + verifybin，而不是 loadfile（2026-09-23 实测教训）：
    #   `loadfile` 会做"读回比较"，内容一致就 **跳过下载**，只打印
    #   `Skipped. Contents already match`。而探针不稳时这个比较会**误判**
    #   ⇒ 根本没烧却不报错（静默失败，实测中过一次）。
    #   `loadbin` 不做比较、强制写；再跟一条 `verifybin` 回读校验，
    #   把静默失败变成显式报错。
    script.write_text(
        "si SWD\n"
        f"speed {JLINK_SPEED}\n"
        "connect\n"
        "r\n"
        "h\n"
        + ("erase\n" if erase else "")
        + f'loadbin "{bin_}", {FLASH_BASE}\n'
        + f'verifybin "{bin_}", {FLASH_BASE}\n'
        "r\n"
        "g\n"
        "qc\n",
        encoding="ascii",
    )
    cmd = [
        str(jlink / "JLink.exe"),
        "-device", cfg["device"],
        "-if", "SWD",
        "-speed", JLINK_SPEED,
        "-autoconnect", "1",
        "-ExitOnError", "1",
        "-CommanderScript", str(script),
    ]
    return run(cmd, cwd=ROOT)


def _probe_wakeup(jlink: Path, scratch: Path) -> None:
    """跑一次 ShowEmuList 把探针"叫醒"。**失败也无声** —— 它只是唤醒手段。

    为什么需要：2026-09-17 首次上板实测，J-Link 在闲置后第一次被访问时会报
        Connecting to J-Link via USB...FAILED: Cannot connect to the probe/programmer.
    而同一条命令紧接着重跑就是 O.K.。ShowEmuList 也一直能看到探针、
    VTref 正常 ⇒ 是**瞬时**的 USB 唤醒/枚举延迟，不是接线问题。
    """
    tool = jlink / "JLink.exe"
    if not tool.is_file():
        return
    script = scratch / "_probe_wakeup.jlink"
    try:
        scratch.mkdir(parents=True, exist_ok=True)
        script.write_text("ShowEmuList\nexit\n", encoding="ascii")
        subprocess.run([str(tool), "-CommanderScript", str(script)],
                       capture_output=True, timeout=60)
    except Exception:
        pass  # 唤醒失败不该影响主流程
    finally:
        try:
            script.unlink()
        except OSError:
            pass


def cmd_flash(args) -> int:
    jlink = find_jlink_dir()
    if not jlink:
        log("!! 找不到 J-Link 安装目录，无法烧录")
        return ENV_FAIL
    cfg = resolve_config(args.preset)
    if not args.no_build:
        rc = cmd_build(args)
        if rc != 0:
            return rc
    if not Path(cfg["elf"]).is_file():
        log(f"!! 找不到固件 {cfg['elf']}，先执行 build（或去掉 --no-build）")
        return ENV_FAIL

    # ⚠️ 烧录是**不可逆操作**（会覆盖目标 Flash 上现有固件）。
    #    docs/12 把它归到「必须先说再做」档 —— AI 动手前要说明烧哪个预设、连哪个探针。
    if args.erase:
        log("⚠️ --erase：先**全片擦除**再烧 —— 目标 Flash 上原有的一切都会被抹掉。")
        log("   docs/12 把这个开关归到「AI 不碰」档；它只为人留的（比如救回锁死的板子）。")
    log(f"烧录 {cfg['elf']} → {cfg['device']} (SWD){'  [含全片擦除]' if args.erase else ''}")
    scratch = Path(cfg["elf"]).parent
    _probe_wakeup(jlink, scratch)

    rc = _flash_once(jlink, cfg, erase=args.erase)
    if rc != 0:
        # ★ 重试一次。理由：2026-09-17 实测过"闲置后第一次连探针失败、
        #   紧接着重跑就成功"。而真坏了（没插/没电/线序错）重试也会失败 ⇒
        #   不会把真问题掩盖成"偶发"。
        log("  !! 第一次烧录失败 —— 重试一次（疑似探针闲置后的瞬时连接失败）...")
        _probe_wakeup(jlink, scratch)
        rc = _flash_once(jlink, cfg, erase=args.erase)
    if rc != 0:
        log("!! 烧录失败：检查探针是否插好、目标板是否供电、SWD 线序（SWDIO/SWCLK/GND）")
        log("   排查顺序见 docs/11 §2。⚠️ `--erase` 不是第一选择。")
        return FLASH_FAIL
    log("烧录完成，目标已复位运行")
    return OK


def _capture_rtt(jlink: Path, cfg: dict[str, str], channel: int,
                 duration: int, logfile: Path) -> int:
    """跑 JLinkRTTLogger 抓 N 秒，写进 logfile。返回退出码。"""
    tool = jlink / "JLinkRTTLogger.exe"
    if not tool.is_file():
        log(f"!! 缺 {tool}")
        return RTT_FAIL
    if logfile.exists():
        logfile.unlink()
    cmd = [
        str(tool),
        "-Device", cfg["device"],
        "-If", "SWD",
        "-Speed", JLINK_SPEED,
        "-RTTChannel", str(channel),
        str(logfile),
    ]
    log(f"  $ {' '.join(cmd)}   (抓 {duration}s)")
    proc = subprocess.Popen(
        cmd, cwd=str(ROOT), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    try:
        time.sleep(duration)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    return OK


def _read_rtt_log(logfile: Path) -> str:
    if not logfile.is_file():
        return ""
    # JLinkRTTLogger 会写一段文本头，再拼原始字节，用 replace 容错解码
    return logfile.read_bytes().decode("utf-8", errors="replace")


def cmd_rtt(args) -> int:
    jlink = find_jlink_dir()
    if not jlink:
        log("!! 找不到 J-Link 安装目录")
        return ENV_FAIL
    cfg = resolve_config(args.preset)
    logfile = Path(cfg["build_dir"]) / f"rtt_ch{args.channel}.log"
    if _capture_rtt(jlink, cfg, args.channel, args.duration, logfile) != OK:
        return RTT_FAIL
    text = _read_rtt_log(logfile)
    log("---- RTT 输出 ----")
    log(text.strip() if text.strip() else "(空：目标可能没在跑 / RTT 控制块没找到)")
    log(f"---- 完整日志: {logfile} ----")
    # ⚠️ 抓不到数据 ≠ 固件有问题：也可能是探针没插、或 RTT 缓冲被丢空
    #    （见 docs/11 §3 的 1024 B 静默丢弃）。
    return OK if text.strip() else RTT_FAIL


def cmd_gdb(args) -> int:
    """起 JLinkGDBServerCL + arm-none-eabi-gdb（VS Code 里等价于 launch.json）。"""
    jlink = find_jlink_dir()
    if not jlink:
        log("!! 找不到 J-Link 安装目录")
        return ENV_FAIL
    server = jlink / "JLinkGDBServerCL.exe"
    if not server.is_file():
        log(f"!! 缺 {server}")
        return ENV_FAIL
    cfg = resolve_config(args.preset)
    elf = Path(cfg["elf"])
    if not elf.is_file():
        log(f"!! 找不到固件 {elf}，先 build")
        return ENV_FAIL
    gdb = shutil.which("arm-none-eabi-gdb")
    if not gdb:
        log("!! 找不到 arm-none-eabi-gdb")
        return ENV_FAIL

    srv_cmd = [str(server), "-device", cfg["device"], "-if", "SWD",
               "-speed", JLINK_SPEED, "-port", GDB_PORT, "-singlerun"]
    log("  $ " + " ".join(srv_cmd))
    srv = subprocess.Popen(srv_cmd, cwd=str(ROOT))
    try:
        time.sleep(2)  # 等 server 把 2331 端口起好
        if args.server_only:
            log(f"GDB server 已在 :{GDB_PORT} 上跑，按 Ctrl+C 停。")
            srv.wait()
            return OK
        rc = run([gdb, "-ex", f"target extended-remote :{GDB_PORT}",
                  "-ex", "monitor reset", "-ex", "monitor halt", str(elf)], cwd=ROOT)
        return OK if rc == 0 else ENV_FAIL
    except KeyboardInterrupt:
        return OK
    finally:
        if srv.poll() is None:
            srv.terminate()


def cmd_lint(args) -> int:
    """静态检查：**.ioc 与 board_config.h 的配置对账**。

    为什么只做这一项：`.ioc` 与 `Core/Inc/board_config.h` 是描述同一批引脚/时钟
    事实的**两个文件**（双真相源）。任何一侧单独改动都会造成"打开 CubeMX 才发现
    配置过期"的漂移，而这类漂移**不会报错**。

    ⚠️ **本工程的两条硬门禁不在这里**：
      · 编译零告警 —— 由 CMake 的 proj_warnings 强制（**编译器报，不是脚本报**）
      · 体积预算 —— `fw.py sizecheck`
      · 分层（规矩 1/2）—— 由 CMake 的链接与 include 可见性强制（docs/01 §强制表）
    ⇒ "lint 通过"绝不等于"工程没问题"。它只保证一件事：两个配置源没有确定性矛盾。

    分层为什么不用检查器：编译器不会忘记运行，脚本会。见 docs/00 §1.3。
    """
    script = ROOT / "Tools" / "cubemx_sync.py"
    if not script.is_file():
        log(f"!! 找不到检查脚本 {script}")
        return ENV_FAIL
    log(f"[lint] CubeMX 对账  ← {script.name} --strict")
    rc = subprocess.call([sys.executable, "-u", str(script), "--strict"], cwd=str(ROOT))
    if rc != 0:
        log(f"[lint] 未通过（退出码 {rc}）—— 修法是改 .ioc 或 board_config.h 让两边一致")
        return LINT_FAIL
    log("[lint] 通过")
    return OK


def cmd_doccheck(args) -> int:
    """文档死链：扫反引号里的路径 + 围栏代码块里的命令。

    ⚠️ 死链是**无声的** —— 没有报错，直到某次 AI 按错的路子干了活。
       骨架用的是短引用 `docs/01`（不是合法路径，肉眼看不出对错），所以**必须靠工具**。
    """
    script = ROOT / "Tools" / "check_doc_links.py"
    if not script.is_file():
        log(f"!! 找不到检查脚本 {script}")
        return ENV_FAIL
    log(f"[doccheck] ← {script.name}")
    rc = subprocess.call([sys.executable, "-u", str(script)], cwd=str(ROOT))
    if rc != 0:
        log("[doccheck] 未通过（退出码 %d）" % rc)
        return LINT_FAIL
    log("[doccheck] 通过")
    return OK


def cmd_selftest(args) -> int:
    """门禁自检：在**工程副本**上逐条注入真违规，证明门禁会报错。

    ⚠️ **"退出码 0"不是有效判据** —— 一个什么都不做的检查器同样返回 0。
       静态检查器的失效是无声的：不报错、不影响构建、报告照样全绿。
       所以自检必须包含**负向用例**（注入违规 → 断言它报错），并先跑**基线**
       （无注入时全绿）以排除"永远报错"的假检查器。
       原工程全程只读。
    """
    script = ROOT / "Tools" / "selftest.py"
    if not script.is_file():
        log(f"!! 找不到自检脚本 {script}")
        return ENV_FAIL
    cmd = [sys.executable, "-u", str(script)]
    if args.full:
        cmd.append("--full")
    log(f"[selftest] ← {script.name}{' --full' if args.full else ''}")
    rc = subprocess.call(cmd, cwd=str(ROOT))
    if rc != 0:
        log("[selftest] 未通过（退出码 %d）" % rc)
        return LINT_FAIL
    log("[selftest] 通过")
    return OK


def cmd_verify(args) -> int:
    """AI 闭环：lint → build → 体积门禁 → flash → 抓 RTT → 判标记。

    三件事：**烧录 → 复位 → 抓 RTT → 判标记**；前两步门禁是"别浪费一次烧录"。

    | 退出码 | 含义 |
    |---|---|
    | 0 | 看到 [AI_READY] ⇒ 固件真的跑起来了 |
    | 1 | 看到 [AI_FAIL] ⇒ 固件主动报错 |
    | 2 | 超时，两个标记都没看到 ⇒ **探针/接线/固件卡死，原因未知** |

    ⚠️ **退出码 2 不等于"固件有问题"** —— 也可能是探针没插、串口被别的工具占着。
       先 `fw.py env` 确认探针在线。
    ⚠️ **探针不在线时，不要把"编译通过"说成"验证通过"**（AGENTS.md §交付口径）。
    """
    cfg = resolve_config(args.preset)
    if cfg["log_backend"].upper() != "RTT":
        log(f"!! verify 需要 RTT 日志后端；当前是 {cfg['log_backend']}。"
            f"用 --preset Debug（默认即 RTT）")
        return ENV_FAIL

    log("[1/5] 静态检查（.ioc 与 board_config.h 对账）")
    if cmd_lint(args) != OK:
        log("!! 静态检查未通过，中止在编译之前 —— 不浪费一次烧录。")
        return LINT_FAIL

    log("[2/5] 编译")
    if cmd_build(args) != OK:
        return BUILD_FAIL

    log("[3/5] 体积门禁")
    rc = cmd_sizecheck(args)
    if rc != OK:
        log("!! 体积门禁未通过，中止在烧录之前 —— 不浪费一次烧录。")
        return rc

    log("[4/5] 烧录")
    jlink = find_jlink_dir()
    if not jlink:
        log("!! 找不到 J-Link 安装目录")
        return ENV_FAIL
    _probe_wakeup(jlink, Path(cfg["elf"]).parent)
    if _flash_once(jlink, cfg) != 0:
        log("!! 烧录失败：先 `fw.py env` 看探针在不在，再查接线（docs/11 §2）")
        return FLASH_FAIL

    log(f"[5/5] 抓 RTT 通道 0（{args.duration}s）并判定标记")
    logfile = Path(cfg["build_dir"]) / "rtt_verify.log"
    if _capture_rtt(jlink, cfg, 0, args.duration, logfile) != OK:
        return RTT_FAIL
    text = _read_rtt_log(logfile)

    if BOOT_FAIL_MARKER in text:
        log(f"结果: 失败 —— 固件报告了 {BOOT_FAIL_MARKER}")
        log(text.strip()[:2000])
        return FAIL_MARKER
    if BOOT_OK_MARKER in text:
        log(f"结果: 成功 —— 检测到 {BOOT_OK_MARKER}")
        for line in text.splitlines():
            if BOOT_OK_MARKER in line:
                log("  " + line.strip())
                break
        return OK
    log(f"结果: 超时 —— {args.duration}s 内既没有 {BOOT_OK_MARKER} 也没 {BOOT_FAIL_MARKER}")
    log("  ⚠️ 这不等于「固件有问题」：先 `fw.py env` 确认探针在线（docs/11 §5）")
    log(text.strip()[:2000] if text.strip() else "(RTT 无任何输出)")
    return TIMEOUT


def cmd_format(args) -> int:
    """用 `.clang-format` 统一格式（**只碰手写代码**，生成物一行不动）。

    ⚠️ 范围刻意限定在四个层 + `Test/`：`Core/`（除 `board_config.h`）、`Drivers/`、
       `Middlewares/` 都是生成物 / vendor —— 格式化它们等于给 CubeMX 添乱
       （下次重新生成会冲突，diff 噪声也巨大）。
    ⚠️ `.clang-format` 是格式的**唯一权威**（docs/08）。本命令只负责"批量执行它"，
       不自己实现任何规则。
    ⚠️ 本工程没有把 `format --check` 挂进交付前检查 —— 格式不统一不会让固件出错，
       把它变成硬门禁只会制造噪声。要用就用 `--check` 手工看一眼。
    """
    tool = shutil.which("clang-format")
    if not tool:
        log("!! 找不到 clang-format（本机在 msys64/ucrt64/bin 下）。")
        log("   没有它也能用：VS Code 保存时自动格式化（.vscode/settings.json 已开）。")
        return ENV_FAIL

    files: list[Path] = []
    for g in FORMAT_GLOBS:
        files += sorted(ROOT.glob(g))
    files = [f for f in files if f.is_file()]
    if not files:
        log("没有可格式化的文件")
        return OK

    mode = "--check" if args.check else "写入"
    log(f"  clang-format {mode}：{len(files)} 个文件（Common/ Device/ Task/ Operation/ Test/）")
    cmd = [tool] + (["--dry-run", "--Werror"] if args.check else ["-i"]) \
        + [str(f) for f in files]
    try:
        p = subprocess.run(cmd, cwd=str(ROOT), capture_output=True, text=True,
                           timeout=300, errors="replace")
    except subprocess.TimeoutExpired:
        log("!! clang-format 超时")
        return ENV_FAIL
    # ⚠️ `--dry-run --Werror` 把"哪个文件该改"写到 **stderr**（不是 stdout）。
    #    只读 stdout 的话，报告会说"不一致"却一个文件名都不给 —— 实测踩到。
    for stream in (p.stdout, p.stderr):
        if stream.strip():
            log(stream.strip())
    if p.returncode != 0:
        if args.check:
            log("!! 上面列出的文件**不符合 .clang-format**。")
            log("   修法：`python Tools/fw.py format`（会就地改写）。")
            log("   ⚠️ 这不是错误，只是不一致 —— 想统一就跑一次。")
        else:
            log(f"!! clang-format 退出码 {p.returncode}")
            return ENV_FAIL
        return LINT_FAIL
    log("格式一致（.clang-format）。" if args.check else "格式化完成。")
    return OK


def cmd_reset(args) -> int:
    """只复位目标并让它继续跑（**不烧录、不覆盖 Flash**）。

    什么时候用：想看一次干净的启动日志、固件卡在 HardFault 想重跑一遍、
    或者改了 RAM 里的东西想恢复。比 `flash` 轻得多，而且**不是不可逆操作**
    （它不写 Flash）—— 所以不需要像烧录那样先问人。
    """
    jlink = find_jlink_dir()
    if not jlink:
        log("!! 找不到 J-Link 安装目录")
        return ENV_FAIL
    cfg = resolve_config(args.preset)
    scratch = ROOT / "build"
    scratch.mkdir(parents=True, exist_ok=True)
    script = scratch / "_reset.jlink"
    script.write_text(
        "si SWD\n"
        f"speed {JLINK_SPEED}\n"
        "connect\n"
        "r\n"    # 复位并停住
        "g\n"    # 放开运行
        "qc\n",
        encoding="ascii",
    )
    cmd = [str(jlink / "JLink.exe"), "-device", cfg["device"], "-if", "SWD",
           "-speed", JLINK_SPEED, "-autoconnect", "1", "-ExitOnError", "1",
           "-CommanderScript", str(script)]
    rc = run(cmd, cwd=ROOT)
    if rc != 0:
        log("!! 复位失败：先 `fw.py env` 看探针在不在（docs/11 §2）")
        return FLASH_FAIL
    log("已复位，目标在运行")
    return OK


def cmd_clean(args) -> int:
    """清空 build/。**只碰 build/**，Drivers/、源码、docs/ 一概不动。

    为什么连 build/ 根下的散文件也清：烧录/调试脚本会往 `build/` **根**写临时
    文件（`flash.jlink`、`_probe_wakeup.jlink` …），它们不是目录 —— 只遍历子目录
    会漏掉，于是 clean 之后 build/ 仍然不空，看着像没清干净。
    这里"目录删树、文件删本身"，一次清到位。
    """
    build_dir = ROOT / "build"
    if not build_dir.is_dir():
        log("build/ 不存在，无需清理")
        return OK
    dirs = files = 0
    for child in build_dir.iterdir():
        if child.is_dir():
            shutil.rmtree(child, ignore_errors=True)
            log(f"  已删除 build/{child.name}/")
            dirs += 1
        else:
            try:
                child.unlink()
            except OSError as exc:
                log(f"  !! 无法删除 build/{child.name}: {exc}")
                continue
            log(f"  已删除 build/{child.name}")
            files += 1
    log(f"清理完成（{dirs} 个预设目录 + {files} 个散文件）。源码与 Drivers/ 未改动。")
    return OK


# ------------------------------------------------------------------ 入口
def _extract_preset(argv: list[str]) -> tuple[list[str], str | None]:
    """把 `--preset X` / `--preset=X` 从任意位置摘出来。

    为什么要这么做：argparse 若把 --preset 挂在主 parser 上，就只能写在子命令**前面**
    （`fw.py --preset Release size`），写成 `fw.py size --preset Release` 会报
    unrecognized arguments —— 这个坑太容易踩（本工程的文档自己就写错过）。
    这里先扫一遍摘掉，再由 main 覆盖回 namespace，于是两种顺序都能用。
    """
    rest: list[str] = []
    found: str | None = None
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--preset" and i + 1 < len(argv):
            found = argv[i + 1]
            i += 2
            continue
        if a.startswith("--preset="):
            found = a.split("=", 1)[1]
            i += 1
            continue
        rest.append(a)
        i += 1
    return rest, found


def main() -> int:
    argv, preset = _extract_preset(sys.argv[1:])

    ap = argparse.ArgumentParser(
        description="STM32 工程模板工具链入口（编译 / 构建 / 烧录 / RTT / GDB）",
        epilog="--preset 写在子命令前或后都可以，"
               "例如：fw.py --preset Release size ≡ fw.py size --preset Release",
    )
    ap.add_argument("--preset", default=DEFAULT_PRESET, help="CMake 预设名（默认 Debug）")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("env", help="体检工具链 / J-Link / 各预设构建目录").set_defaults(func=cmd_env)
    sub.add_parser("lint", help="静态检查：.ioc 与 board_config.h 对账").set_defaults(func=cmd_lint)
    sub.add_parser("doccheck", help="文档死链").set_defaults(func=cmd_doccheck)
    sub.add_parser("size", help="报体积明细").set_defaults(func=cmd_size)
    sub.add_parser("sizecheck", help="体积预算门禁").set_defaults(func=cmd_sizecheck)
    sub.add_parser("test", help="★ PC 侧单测（不需要板子）").set_defaults(func=cmd_test)
    sub.add_parser("clean", help="只清 build/").set_defaults(func=cmd_clean)
    sub.add_parser("reset", help="只复位目标（不烧录、不覆盖 Flash）").set_defaults(func=cmd_reset)

    p = sub.add_parser("format", help="用 .clang-format 统一格式（只碰手写代码）")
    p.add_argument("--check", action="store_true",
                   help="只报告哪些文件不符合，不改写（退出码 7 = 有不一致）")
    p.set_defaults(func=cmd_format)

    p = sub.add_parser("build", help="编译")
    p.add_argument("--clean-first", action="store_true",
                   help="先清再编 —— ★ 判「零告警」只能用这个（增量会漏报）")
    p.set_defaults(func=cmd_build)

    p = sub.add_parser("flash", help="烧录（J-Link SWD）")
    p.add_argument("--no-build", action="store_true", help="跳过编译，直接烧现有产物")
    p.add_argument("--erase", action="store_true",
                   help="先全片擦除再烧。⚠️ 抹掉目标上的一切；docs/12 归到「AI 不碰」档")
    p.add_argument("--clean-first", action="store_true", help="（转发给 build）")
    p.set_defaults(func=cmd_flash)

    p = sub.add_parser("rtt", help="抓 SEGGER RTT 日志")
    p.add_argument("--duration", type=int, default=10, help="抓取时长（秒）")
    p.add_argument("--channel", type=int, default=0, help="RTT 通道号（0=日志 1=数据）")
    p.set_defaults(func=cmd_rtt)

    p = sub.add_parser("gdb", help="JLinkGDBServerCL + arm-none-eabi-gdb")
    p.add_argument("--server-only", action="store_true", help="只起 GDB server")
    p.set_defaults(func=cmd_gdb)

    p = sub.add_parser("verify", help="闭环：门禁 → 烧录 → 抓 RTT → 判标记")
    p.add_argument("--duration", type=int, default=8, help="抓取时长（秒）")
    p.add_argument("--clean-first", action="store_true", help="（转发给 build）")
    p.set_defaults(func=cmd_verify)

    p = sub.add_parser("selftest", help="门禁自检（注入真违规，证明检查器会报错）")
    p.add_argument("--full", action="store_true", help="追加编译期用例（较慢）")
    p.set_defaults(func=cmd_selftest)

    args = ap.parse_args(argv)
    if preset:
        args.preset = preset
    log(f"== fw {args.cmd} (preset={args.preset}) ==")
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
