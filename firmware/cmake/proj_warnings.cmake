# ============================================================================
#  proj_warnings —— 全工程唯一一套严格告警（MCU 侧与 PC 侧共用）
#
#  为什么单独一个文件：`Test/` 是**独立 configure** 的（host 编译），拿不到
#  顶层 CMakeLists 里定义的 target。原先两边的告警列表是**各自抄一份**，
#  靠一句注释"改一处必须改另一处"约束 —— 那是**双真相源**，迟早会漂。
#  ⇒ 收成一个 `include()` 的文件，两边引用同一份。
#
#  ⚠️ 这一组**只加在工程自己的代码上**（common / operation / device / task
#     与 Test/）。生成物（Core/Src 全部、SEGGER_RTT、ST HAL、FreeRTOS）只给
#     `-Wall` —— 理由见 docs/11 §1（实测：`syscalls.c`/`sysmem.c` 在严格档下
#     报 19 条，且每次 CubeMX 重新生成都会原样回来）。
#
#  ⚠️ 挂的时候必须用 **PRIVATE**：写成 PUBLIC 会顺着依赖链传到可执行目标，
#     把严格告警灌到生成物上（实测踩过）。
# ============================================================================

set(PROJ_WARNING_FLAGS
    -Wall
    -Wextra
    -Wshadow
    -Wundef
    -Wstrict-prototypes
    -Wdouble-promotion
    -Wmissing-prototypes
    -Wpointer-arith
    -Wcast-align)
