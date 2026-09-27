# Operation/ —— 业务层（怎么写）

> 四层里的第 2 层：**"产品要做什么"**。它吃数字、吐数字。
> 分层总览见 [`../docs/01_工程分层架构.md`](../docs/01_工程分层架构.md)。

## 一句话

**能在电脑上跑的逻辑，就写在这里。** 不能在电脑上跑的，那是 `Device/` 的事。

## 硬约束（由 CMake 强制，不是靠自觉）

| 禁止 | 强制手段 | 违反时 |
|---|---|---|
| include 任何 HAL 头 | `operation` 目标**只链 `common`** ⇒ 拿不到那些 include 路径 | 编译期 `fatal error: stm32f4xx_hal.h: No such file` |
| 调 FreeRTOS（`vTaskDelay` 等） | PC 侧构建里没有那些符号 | 链接期 `undefined reference to vTaskDelay` |
| include `Device/` 的头 | 同上（`device` 不在 `operation` 的链接列表里） | 编译期 `No such file` |

⚠️ **刻意绕过能过 MCU 侧**：自己写 `extern void vTaskDelay(unsigned);` 再调用，
编译链接都过（最终固件本来就链了 FreeRTOS）。那种写法由 **PC 侧**拦住 ——
`python Tools/fw.py test` 报 `undefined reference`。
**两道墙合起来才完整**（`fw.py selftest --full` 会逐条验证）。

## 需要时间怎么办

**让 Task 把"现在过了几毫秒"当参数传进来**：

```c
/* ✅ 时间是参数 —— 纯函数，能在 PC 上测 */
bool op_filter_step(op_filter_t *ctx, int32_t raw, uint32_t now_ms);

/* ❌ 自己去取时间 —— 立刻失去可测性 */
bool op_filter_step(op_filter_t *ctx, int32_t raw) {
    uint32_t now = bsp_tick_ms();   /* 这一行就杀死了电脑测试 */
    ...
}
```

## 需要共享状态怎么办

**不要在 Operation 里加锁**（锁属于 RTOS）。两种做法：

1. **函数指针注入**：Task 装配时传真锁，PC 测试时传空实现；
2. **C11 `<stdatomic.h>` 做无锁双缓冲** —— 原子操作属于 C 标准库，不违反规矩 2。

⚠️ 选第 2 条时记住代价：**seqlock 只允许一个写者**。加第二个写者就是改错。

## 新增业务逻辑：四步（照做，别跳）

```bash
# ① 写纯逻辑
#    Operation/Inc/op_xxx.h  +  Operation/Src/op_xxx.c
#    —— 时间是参数；参数由 init() 注入，**不读 project_config.h**

# ② 自查（关键判据，不是形式）
grep -n "HAL_\|vTask\|FreeRTOS\|bsp_" Operation/Src/op_xxx.c
#    ⇒ 必须 0 命中

# ③ 写单测
#    Test/test_op_xxx.c（含负向断言与边界值）
#    Test/CMakeLists.txt 里**手工加两行**（OP_SRC + add_executable）
python Tools/fw.py test
#    ⇒ 退出码 0

# ④ 接到 Task
#    组合根 Task/Src/app.c 注入参数；Task/Src/app_xxx.c 做适配；任务里调显式入口
python Tools/fw.py build --clean-first
#    ⇒ 零告警
```

**完整可模仿的样板**：`op_blink`（闪烁逻辑）+ `Test/test_op_blink.c` +
`Task/Src/app_blink.c` + `app.c` 的 `app_poll()`。

## 目录约定

```
Operation/
├── Inc/     公开头文件（一个模块一个 .h）
├── Src/     实现（一个模块一个 .c）
└── README.md
```

- 命名前缀 `op_`（见 [`../docs/04_命名规则.md`](../docs/04_命名规则.md)）。
- `Inc/` 与 `Src/` 是 CMake 的 `GLOB`，**新增 .c 不用改 `CMakeLists.txt`**。
  ⚠️ 但 `Test/CMakeLists.txt` **刻意不写 GLOB** —— 加测试要手工加两行，
  漏加的表现是"测试全过，但那个模块根本没被测"。
