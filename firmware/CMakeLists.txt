cmake_minimum_required(VERSION 3.22)

# ============================================================================
#  f407vgt6 —— 四层架构工程模板 · 顶层构建脚本
#
#  ⚠️ 本文件由 CubeMX **只生成一次**（之后重新生成不覆盖），所以它是本工程
#     唯一允许手写构建逻辑的地方。
#     生成物（不要手改，见 docs/01 规矩 4）：
#         cmake/stm32cubemx/CMakeLists.txt   ← CubeMX 每次生成都重写
#         Core/  Drivers/  Middlewares/Third_Party/
#
#  ---------------------------- 分层 target ----------------------------
#      common     零依赖，任何层都能用（日志接口 / 工具函数）
#      operation  ★ 只链 common ⇒ **拿不到 HAL 头**（规矩 2 的机器强制）
#      device     唯一链 HAL 的层
#      task       链 operation + device（组合根 app.c 在这里）
#      segger_rtt 第三方，只给 -Wall
#      exe        全部收口
#
#  ---------------------------- 两个约定 ----------------------------
#  · 告警只有一套：proj_warnings（9 个 -W*）。生成物只给 -Wall —— 理由见 docs/11 §1。
#  · 板级事实（器件名 / 内存容量）从 Core/Inc/board_config.h 读，本文件不重复定义，
#    读出来写进 build/<预设>/board.env 供 Tools/fw.py 使用。
# ============================================================================

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME G2026)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# ============================================================================
#  构建类型 → 优化档（docs/11 §1）
#
#  ⚠️ 这一节**故意放在本文件**，不放 cmake/gcc-arm-none-eabi.cmake ——
#     那个文件在 cmake/ 下，是 CubeMX 的生成物，重新生成可能被覆盖。
#     构建口径放在"只生成一次"的本文件里才稳。
#  ⚠️ 用普通 set（不加 CACHE）：工具链文件里也是普通 set，普通变量会**遮蔽**
#     同名 cache 变量 —— 写成 CACHE FORCE 会静默不生效，实测踩过。
#  ⚠️ Release 用 -Os，**不要改回 -O3**：实测 -O3 会让 SEGGER RTT / HAL 这类按
#     -O2/-Os 写的第三方代码激进内联，同一份代码反而多 7.3 KB，膨胀全在第三方符号上。
# ============================================================================
set(CMAKE_C_FLAGS_DEBUG          "-Og -g3")  # 日常开发：-Og 比 -O0 好调（变量不会被优化掉）
set(CMAKE_C_FLAGS_RELEASE        "-Os -g0")  # 发布：体积最小
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g3")  # 现场取证：跑得快且带符号

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
#   ★ 它建两个 **OBJECT** 库（STM32_Drivers / FreeRTOS），不是 STATIC ——
#     这一点必须保持：STATIC 会在链接顺序上踩 `undefined reference to __errno`
#     （见 docs/00 §1.4）。不要改成 STATIC。
add_subdirectory(cmake/stm32cubemx)

# ============================================================================
#  日志后端：RTT（默认）或 UART，编译期二选一
#      RTT  —— 走调试探针，不占串口（`--preset Debug`）
#      UART —— 走 board_config.h 里配的串口（`--preset Debug-UART`，没探针时用）
#  实现二选一在 Device/Src/bsp_log.c，未定义会当场 #error。
# ============================================================================
set(LOG_BACKEND "RTT" CACHE STRING "日志后端：RTT 或 UART")
set_property(CACHE LOG_BACKEND PROPERTY STRINGS RTT UART)

# ============================================================================
#  零告警口径（硬门禁，docs/11 §1）
#
#  ⚠️ 告警列表在 cmake/proj_warnings.cmake —— **MCU 侧与 PC 侧共用同一份**。
#     别在本文件里再抄一遍列表：那是双真相源（`Test/` 是独立 configure，
#     拿不到这里的 target，原先只能靠注释约束"改一处必须改另一处"）。
#  ⚠️ 挂的时候必须 `PRIVATE` —— 见下面各 target。
# ============================================================================
include(${CMAKE_SOURCE_DIR}/cmake/proj_warnings.cmake)

add_library(proj_warnings INTERFACE)
target_compile_options(proj_warnings INTERFACE ${PROJ_WARNING_FLAGS})

# ============================================================================
#  板级事实：从 Core/Inc/board_config.h 读（**唯一来源**，不在本文件重复定义）
# ============================================================================
function(proj_read_board_define out_var name)
    file(STRINGS "${CMAKE_SOURCE_DIR}/Core/Inc/board_config.h" _lines
         REGEX "^[ \t]*#[ \t]*define[ \t]+${name}[ \t]+")
    if(_lines STREQUAL "")
        message(FATAL_ERROR
            "Core/Inc/board_config.h 里找不到 ${name}。\n"
            "它是板级事实的唯一来源，构建脚本靠它填 board.env —— 别删，也别改名。")
    endif()
    list(GET _lines 0 _line)
    string(REGEX REPLACE "^[ \t]*#[ \t]*define[ \t]+${name}[ \t]+" "" _val "${_line}")
    # ⚠️ 必须先剥掉行尾注释再剥引号/后缀 —— 否则
    #    `#define BOARD_FLASH_KB 1024U /* FLASH 段 */` 会被读成
    #    `1024U /* FLASH 段`，而 `#define BOARD_MCU_NAME "STM32F407VGTx" /* ... */`
    #    会连引号一起留下。这是实测踩到的（board.env 里出现了半截注释）。
    string(REGEX REPLACE "/\\*.*" "" _val "${_val}")
    string(REGEX REPLACE "//.*" "" _val "${_val}")
    string(STRIP "${_val}" _val)
    string(REGEX REPLACE "[uUlL]+$" "" _val "${_val}") # 1024U -> 1024
    string(REPLACE "\"" "" _val "${_val}")             # "STM32F407VG" -> STM32F407VG
    string(STRIP "${_val}" _val)
    set(${out_var} "${_val}" PARENT_SCOPE)
endfunction()

proj_read_board_define(BOARD_MCU_NAME    BOARD_MCU_NAME)
proj_read_board_define(BOARD_JLINK_DEVICE BOARD_JLINK_DEVICE)
proj_read_board_define(BOARD_FLASH_KB    BOARD_FLASH_KB)
proj_read_board_define(BOARD_RAM_KB      BOARD_RAM_KB)
proj_read_board_define(BOARD_CCM_KB      BOARD_CCM_KB)

# 链接脚本：工程根下**恰好一个** .ld。
# ★ 体积门禁（Tools/stm32_size.py）按它的 MEMORY 段算占用 —— 硬编码文件名会在
#   换芯片后**静默**按错的内存区算数，所以这里找出来、写进 board.env。
# ⚠️ 同时找多个 = 二义（不知道该按哪个算），直接报错比猜一个强。
file(GLOB PROJ_LD_FILES RELATIVE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/*.ld")
list(LENGTH PROJ_LD_FILES PROJ_LD_COUNT)
if(NOT PROJ_LD_COUNT EQUAL 1)
    message(FATAL_ERROR
        "工程根下应恰好有 1 个链接脚本（.ld），实际找到 ${PROJ_LD_COUNT} 个：${PROJ_LD_FILES}\n"
        "体积门禁按它的 MEMORY 段算占用，多个会让口径变得不确定。")
endif()
set(PROJ_LD "${CMAKE_SOURCE_DIR}/${PROJ_LD_FILES}")

# ============================================================================
#  Common —— 横切层：谁都能用，但它自己零依赖
#      ★ 这里**不许**出现 HAL / FreeRTOS / Device 的头（一旦引了，所有依赖
#        它的 Operation 代码在 PC 上都会编译失败，等于把"可测试"丢掉）。
# ============================================================================
add_library(common STATIC
    Common/Src/log.c)
target_include_directories(common PUBLIC Common/Inc)
# ⚠️ 必须是 PRIVATE：写成 PUBLIC 会**传递**到链接 common 的所有目标，
#    最终把严格告警灌到可执行目标自己的 CubeMX 生成物上（实测踩过：
#    syscalls.c / sysmem.c 因此报 19 条）。四个层一律 PRIVATE，见 §可执行文件收口。
target_link_libraries(common PRIVATE proj_warnings)

# ============================================================================
#  Operation —— 业务逻辑：吃数字吐数字
#
#  ★★ 规矩 2 的机器强制就在下面这一行：**只链 common**。
#      没链 stm32cubemx ⇒ 没有 Core/Inc 与 Drivers/ 的 include 路径
#      ⇒ 谁在这里偷 include "stm32f4xx_hal.h"，编译期当场
#         `fatal error: stm32f4xx_hal.h: No such file or directory`。
#      不要为了"让它编过"给它加 include 路径 —— 那是把规矩 2 拆了。
#  ★ 参数一律由 Task 层注入，**不读 project_config.h**（那样它才能在 PC 上
#    换一套参数单测）。
# ============================================================================
add_library(operation STATIC
    Operation/Src/op_blink.c
    Operation/Src/op_flash.c
    Operation/Src/op_dsp.c
    Operation/Src/op_dsp_winh4.c
    Operation/Src/op_cal.c
    Operation/Src/op_peak.c
    Operation/Src/op_meas.c
    Operation/Src/op_wave.c
    Operation/Src/op_spec.c)
target_include_directories(operation PUBLIC Operation/Inc)
target_link_libraries(operation PUBLIC common)
# op_dsp 调 arm_cfft_f32（CMSIS-DSP，见 §CMSIS-DSP：预编译 .a 只能挂这里 ——
# PC 单测不走本文件这一段，用 OP_DSP_HOST_FFT 替身，Test/CMakeLists 定义）。
# arm_math.h 还要 CMSIS 的内核头（cmsis_compiler.h 等）—— 只给 CMSIS/Include，
# 不给 HAL 头路径：规矩 2 的机器强制（偷 include HAL 仍当场编不过）不变。
target_include_directories(operation SYSTEM PUBLIC
    "${CMAKE_SOURCE_DIR}/Drivers/CMSIS/Include")
target_link_libraries(operation PRIVATE cmsis_dsp m)
target_link_libraries(operation PRIVATE proj_warnings)

# ============================================================================
#  SEGGER RTT —— 第三方，vendor 在 Middlewares/RTT/
#      ⚠️ 只给 -Wall（proj_warnings 那 9 个会把它淹了），且**不链 proj_warnings**。
#      ⚠️ 上行缓冲大小**显式定义**：SEGGER_RTT.c 里是 `#ifndef BUFFER_SIZE_UP`
#         + 默认 1024。不显式定义的话，"1024"只是个**隐式默认** —— 升级 vendor
#         源码时默认值一变，`project_config.h` 里按 1024 调的 `APP_TRACE_DRAIN_*`
#         就悄悄失准（而那是"批量输出被静默丢弃"的直接原因，见 docs/11 §3）。
# ============================================================================
# ★ 2026-09-23：1024 → 4096。
#   原因：HardFault 取证要在**自检日志之后**再打 3 行，而 1024 B 那时已经满了
#   （NO_BLOCK_SKIP 静默丢弃）⇒ 实测只出第一行，**PC 那行丢了** ——
#   而 PC 恰恰是唯一能定位源码的信息。缓冲加大后取证完整。
# ⚠️ 这是"崩溃时能不能看清"的关键，别再调回 1024。
# ⚠️ 加 FORCE：不带 FORCE 的 `set(... CACHE ...)` **只在首次生效**，
#    已有的 build/<预设>/CMakeCache.txt 会保留旧值（实测改完还是 1024）。
set(PROJ_RTT_UP_BUFFER 4096 CACHE STRING
    "RTT 上行缓冲字节数。改动后必须同步 project_config.h 的 APP_TRACE_DRAIN_*" FORCE)
add_library(segger_rtt STATIC
    Middlewares/RTT/SEGGER_RTT.c)
target_include_directories(segger_rtt PUBLIC Middlewares/RTT)
target_compile_definitions(segger_rtt PUBLIC BUFFER_SIZE_UP=${PROJ_RTT_UP_BUFFER})
target_compile_options(segger_rtt PRIVATE -Wall)

# ============================================================================
#  Device —— 唯一直接调 HAL 的层
#      · 一个外设一个文件，HAL 句柄 static 在 .c 里，**不写进对外头文件**
#      · 中断回调写在本层，只投事件不跑业务（规矩 3）
#      · 硬件事实取自 Core/Inc/board_config.h，本层不写魔法数字
# ============================================================================
add_library(device STATIC
    Device/Src/bsp_adc.c
    Device/Src/bsp_fault.c
    Device/Src/bsp_gpio.c
    Device/Src/bsp_lcd.c
    Device/Src/bsp_log.c
    Device/Src/bsp_port_map.c
    Device/Src/bsp_rtc.c
    Device/Src/bsp_sd.c
    Device/Src/bsp_spi.c
    Device/Src/bsp_tick.c
    Device/Src/bsp_touch.c)
target_include_directories(device PUBLIC Device/Inc)
target_compile_definitions(device PUBLIC LOG_BACKEND_${LOG_BACKEND})
target_link_libraries(device PUBLIC
    common
    segger_rtt
    stm32cubemx) # HAL + FreeRTOS 的 include 路径从这里来
target_link_libraries(device PRIVATE proj_warnings)

# ============================================================================
#  FreeRTOS 头（**只给头，不给 HAL**）
#
#  ★ 为什么要单独一个 target：`Task/` 里只有组合根（app.c）该碰 `Device/`，
#    但 app_rtos.c 确实需要 FreeRTOS 的 `cmsis_os.h`。若直接链 `stm32cubemx`
#    （它把 HAL 与 FreeRTOS 的 include 路径混在一起），app_rtos.c 就顺带拿到了
#    HAL 头 ⇒ "规矩 1"只能靠 review。
#    这个 target 只暴露 FreeRTOS 的三个目录 + `Core/Inc`（放 FreeRTOSConfig.h），
#    **不含** `Drivers/STM32F4xx_HAL_Driver/Inc` ⇒ `stm32f4xx_hal.h` 找不到。
# ============================================================================
add_library(freertos_headers INTERFACE)
target_include_directories(freertos_headers INTERFACE
    Core/Inc                                                  # FreeRTOSConfig.h
    Middlewares/Third_Party/FreeRTOS/Source/include
    Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2
    Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F)
target_compile_definitions(freertos_headers INTERFACE STM32F407xx)

# ============================================================================
#  LVGL —— 第三方 GUI 库，vendor 在 Middlewares/LVGL/（版本 9.5.0）
#
#  ⚠️ 照 segger_rtt 的做法：**只给 -Wall，不链 proj_warnings** ——
#     第三方代码不该受本工程那 9 个严格告警约束，否则升级 vendor 就红一片。
#  ⚠️ 源文件用 GLOB_RECURSE 全收（LVGL 官方 env_support/cmake 也是这么做的）：
#     不用的模块由**源文件内部**的 `#if LV_USE_XXX` 守卫编成空目标文件，
#     所以"关掉 widget"靠 lv_conf.h，不靠改这里。
#  ⚠️ 增删 LVGL 源文件后要重新 configure（GLOB 只在 configure 时展开）：
#     cmake --preset <名> --fresh
#  ⚠️ SYSTEM 关键字是故意的：消费方（Task 层）include lvgl.h 时，
#     头文件里的告警会被抑制，不会污染本工程的零告警口径。
#  ⚠️ 链 freertos_headers 是因为 lv_conf.h 里 LV_USE_OS = LV_OS_FREERTOS。
# ============================================================================
file(GLOB_RECURSE LVGL_SOURCES
    ${CMAKE_SOURCE_DIR}/Middlewares/LVGL/src/*.c)

# 官方 demos：**2026-10-02 起不再进构建**（S5 清残留）。
# 理由：交付态 `APP_LVGL_USE_DEMO=0`（业务 UI），demo 无人引用；`LV_USE_DEMO_WIDGETS`
# 也已关（lv_conf.h）。要再上板验证显示/触摸时：打开这两个开关 + 把下面两行放回来。
# file(GLOB LVGL_WIDGET_DEMO_SOURCES
#     ${CMAKE_SOURCE_DIR}/Middlewares/LVGL/demos/widgets/lv_demo_widgets.c
#     ${CMAKE_SOURCE_DIR}/Middlewares/LVGL/demos/widgets/assets/*.c)
# list(APPEND LVGL_SOURCES ${LVGL_WIDGET_DEMO_SOURCES})

add_library(lvgl STATIC ${LVGL_SOURCES})
target_include_directories(lvgl SYSTEM PUBLIC
    Middlewares/LVGL
    Middlewares/LVGL/src)
target_compile_definitions(lvgl PUBLIC
    LV_CONF_INCLUDE_SIMPLE
    LV_LVGL_H_INCLUDE_SIMPLE
    LV_KCONFIG_IGNORE)
target_link_libraries(lvgl PUBLIC freertos_headers)
target_compile_options(lvgl PRIVATE -Wall)

# ============================================================================
#  Task —— 调度层：只管"什么时候做"
#
#  ★★ 拆成三个 target，把"**Task/ 下只有组合根允许碰 Device**"从
#     "约定 + review" 升级成**编译期强制**（docs/01 §强制表，规矩 1）：
#
#       task_adapters   app_blink.c     → 只链 operation      ⇒ 无 HAL / 无 FreeRTOS / 无 Device
#       task_rtos       app_rtos.c      → 链 freertos_headers ⇒ 有 FreeRTOS，无 HAL
#       task_root       app.c（组合根） → 链 device + 全部     ⇒ 唯一能同时看两侧的
#
#  ★ 2026-09-26：task_root 里多了第二个文件 `app_lvgl.c`（LVGL 适配层）。
#    它必须同时看到 `lvgl.h`（GUI 库）与 `bsp_lcd.h`/`bsp_touch.h`（Device）——
#    Operation 不许碰硬件、Device 不该依赖 GUI 库，**只有这一层容得下这个组合**。
#    它是"适配"，不是业务，所以不进 Operation/。
#    ⚠️ 代价：`task_root` 现在有两个文件，不再等于"只有组合根"。
#       若将来这类适配层变多，应该单独开一个 target（只链 device + lvgl），
#       而不是继续往 task_root 里塞。
#
#     ⇒ 在 app_blink.c 里 `#include "bsp.h"` 会当场
#       `fatal error: bsp.h: No such file or directory`。
#     ⚠️ 别为了"让它编过"给 task_adapters 加 device —— 那是把规矩 1 拆了。
#
#  ★ 周期逻辑一律做成**显式入口**（app_poll），不要写进 for(;;) 里 ——
#    将来调整任务划分时不用搬家，而且它本身就是"这一拍做什么"的清单。
# ============================================================================
add_library(task_adapters STATIC
    Task/Src/app_blink.c)
target_include_directories(task_adapters PUBLIC
    Task/Inc
    ${CMAKE_SOURCE_DIR}) # project_config.h（应用参数）在工程根
target_link_libraries(task_adapters PUBLIC operation)
target_link_libraries(task_adapters PRIVATE proj_warnings)

add_library(task_rtos STATIC
    Task/Src/app_rtos.c
    Task/Src/app_tasks.c) # ★ 任务创建（Task_Engine / Task_Ui）—— 静态栈/TCB 在本文件里
target_include_directories(task_rtos PUBLIC Task/Inc ${CMAKE_SOURCE_DIR})
target_link_libraries(task_rtos PUBLIC task_adapters freertos_headers)
target_link_libraries(task_rtos PRIVATE proj_warnings)

add_library(task_root STATIC
    Task/Src/app.c
    Task/Src/app_lvgl.c)
target_include_directories(task_root PUBLIC Task/Inc ${CMAKE_SOURCE_DIR})
target_link_libraries(task_root PUBLIC
    task_adapters
    device
    lvgl         # ★ app_lvgl.c 要用 lvgl.h（它同时依赖 GUI 库与 bsp_lcd）
    stm32cubemx) # 组合根要同时看 Device 与 Operation（后者经 task_adapters 传进来）
target_link_libraries(task_root PRIVATE proj_warnings)

# ============================================================================
#  task_ops —— 测量业务编排（app_engine.c）与测量 UI（app_ui.c）
#
#  ⚠️ CMakeLists §task_adapters 的注释预告过这一天："若适配层变多，应该单独
#     开一个 target（只链 device + lvgl），而不是继续往 task_root 里塞"。
#     现在到了：app_engine.c 要碰 bsp_adc/bsp_spi（Device），app_ui.c 要
#     lvgl + op_* —— 它们是"编排/适配"，不是组合根本体。
#  ★ 与 task_root 的分工：app.c（组合根）仍是唯一"装配"（app_init/app_poll
#    的顺序与依赖注入在这里）；task_ops 只提供显式入口给组合根调。
# ============================================================================
add_library(task_ops STATIC
    Task/Src/app_engine.c
    Task/Src/app_ui.c)
target_include_directories(task_ops PUBLIC
    Task/Inc
    ${CMAKE_SOURCE_DIR}) # project_config.h（应用参数）在工程根
target_link_libraries(task_ops PUBLIC
    operation      # op_dsp/op_peak/op_meas/op_wave/op_spec/op_cal
    device         # bsp_adc / bsp_spi（engine）；bsp.h 总入口
    freertos_headers # ★ app_engine.c/app_ui.c 的任务体要用 FreeRTOS API
                     #   （xTaskNotifyWait / vTaskDelay）—— 显式声明，
                     #   不靠 lvgl→freertos_headers 的间接传递
    lvgl)          # app_ui.c
target_link_libraries(task_ops PRIVATE proj_warnings)

# 别名：文档里一直用 `task` 指"Task 层"（docs/11 §1 的告警档位表）。
# 它只是把四个子 target 收在一起，本身没有源码。
add_library(task INTERFACE)
target_link_libraries(task INTERFACE task_adapters task_rtos task_root task_ops)

# ============================================================================
#  CMSIS-DSP：把预编译库注册成 imported target（★ 必须在本文件里做，不能靠 CubeMX）
#
#  ⚠️ 为什么 CubeMX 生成的那一行不管用（实测踩过）：
#     `cmake/stm32cubemx/CMakeLists.txt` 把 `:libarm_cortexM4lf_math.a` 放进
#     MX_LINK_LIBS，而 MX_LINK_LIBS 是在 `add_subdirectory(cmake/stm32cubemx)`
#     里挂到可执行目标上的 —— 位置在**所有层静态库之前**。`.a` 是单遍扫描：
#     链接器处理它时"还没有任何未定义符号"，于是一个成员都不拉。
#     ⇒ 从四个层里调 DSP 必然 `undefined reference to arm_sin_f32`；
#       只有 Core/ 里的生成代码能用（它们的 .obj 排在库前面）。
#     #include "arm_math.h" 是能编过的（DSP/Inc 在 stm32cubemx 的 include 里），
#     所以这个坑在编译期完全看不见，只在链接期炸。
#
#  ⇒ 在手写的本文件里挂到真正调 DSP 的层上，链接顺序由 CMake 的依赖图保证
#    （libtask_root.a 在前、libarm_cortexM4lf_math.a 紧随其后）。
#    CubeMX 重新生成多少次都不会丢。
#
#  ⚠️ `m` 必须跟着一起挂：arm_math.h 里的 arm_sqrt_f32 是 inline，
#     在 GCC 下落到 sqrtf（libm）。只挂 DSP 库不挂 m 会报 `undefined reference
#     to sqrtf` —— 那也是实测的。
#
#  ★ 将来别的层要用 DSP，只加一行：target_link_libraries(<层> PRIVATE cmsis_dsp m)
#  ⚠️ **不要**挂到 operation / task_adapters 上：`fw.py test` 在 PC 上编译
#     Operation 层，而这个 .a 是 Cortex-M4F 的机器码，PC 上链不了。
# ============================================================================
add_library(cmsis_dsp STATIC IMPORTED)
set_target_properties(cmsis_dsp PROPERTIES
    IMPORTED_LOCATION
    "${CMAKE_SOURCE_DIR}/Middlewares/ST/ARM/DSP/Lib/libarm_cortexM4lf_math.a")
target_include_directories(cmsis_dsp INTERFACE
    "${CMAKE_SOURCE_DIR}/Middlewares/ST/ARM/DSP/Inc")

# 用到 DSP 的层：组合根（Task/Src/app.c）与 Device 层
target_link_libraries(task_root PRIVATE cmsis_dsp m)
target_link_libraries(device PRIVATE cmsis_dsp m)

# ============================================================================
#  可执行文件收口
#
#  ⚠️ 这里**故意不链 proj_warnings**（docs/11 §1：生成物只给 -Wall）。
#     可执行目标自己的源码全部是 CubeMX 生成物（main.c / freertos.c / gpio.c /
#     syscalls.c / sysmem.c / startup_*.s …）。实测：套上严格告警后
#     syscalls.c 与 sysmem.c 报 19 条（newlib 桩函数天生没有原型，
#     -Wmissing-prototypes / -Wstrict-prototypes 必中），而这两个文件每次
#     CubeMX 重新生成都会原样回来 —— 修不了，只能压。
#     -Wall 由工具链文件全局提供，所以它们不是"无告警"，是"只 -Wall"。
#
#  ⇒ 需要严格告警的**手写代码一律进四个层**（common / operation / device / task），
#    它们的 target 上都挂了 proj_warnings。Core/ 里的 USER CODE 段只写胶水
#    （一个 #include + 一次调用），那是刻意的。
# ============================================================================
target_link_libraries(${CMAKE_PROJECT_NAME}
    task_adapters
    task_rtos
    task_root
    task_ops
    operation
    device
    common
    segger_rtt
    lvgl
    stm32cubemx)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# ============================================================================
#  产物：.elf 之外再出 .bin（烧录/OTA）与 .hex（J-Link 命令行最省事）
# ============================================================================
set(PROJ_ELF $<TARGET_FILE:${CMAKE_PROJECT_NAME}>)
set(PROJ_BIN ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin)
set(PROJ_HEX ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.hex)
set(PROJ_MAP ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.map)

add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary ${PROJ_ELF} ${PROJ_BIN}
    COMMAND ${CMAKE_OBJCOPY} -O ihex   ${PROJ_ELF} ${PROJ_HEX}
    COMMENT "objcopy -> .bin / .hex")

# 让 `ninja clean` 一并清掉 map（CubeMX 生成物只登记了 map，bin/hex 在这里补上）
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES
    ADDITIONAL_CLEAN_FILES "${PROJ_BIN};${PROJ_HEX}")

# ============================================================================
#  board.env —— 给 Tools/fw.py 的"构建事实"，每次 configure 重新生成
#
#  ★ 为什么要有它：烧录 / RTT / GDB / 体积门禁都需要知道"产物在哪、是什么器件"。
#    这些值**只应该有一个来源**（CMake 与 board_config.h），脚本不该自己猜路径、
#    更不该把器件名硬编码进去。fw.py 读这一个文件就够了。
#  ⚠️ 本文件是**生成物**，手改会在下次 configure 时被覆盖。
# ============================================================================
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/board.env" CONTENT
"# 由 CMakeLists.txt 生成（file(GENERATE)）—— 手改无效，下次 configure 会被覆盖。
# 器件名与内存容量来自 Core/Inc/board_config.h；路径来自 CMake。
# 消费方：Tools/fw.py
PROJECT_NAME=${CMAKE_PROJECT_NAME}
BUILD_TYPE=${CMAKE_BUILD_TYPE}
BUILD_DIR=${CMAKE_BINARY_DIR}
LOG_BACKEND=${LOG_BACKEND}
MCU_NAME=${BOARD_MCU_NAME}
JLINK_DEVICE=${BOARD_JLINK_DEVICE}
FLASH_KB=${BOARD_FLASH_KB}
RAM_KB=${BOARD_RAM_KB}
CCM_KB=${BOARD_CCM_KB}
LD=${PROJ_LD}
ELF=${PROJ_ELF}
BIN=${PROJ_BIN}
HEX=${PROJ_HEX}
MAP=${PROJ_MAP}
")

# ============================================================================
#  Test/ 不在这里 add_subdirectory
#
#  ⚠️ Test/ 是 **host 编译**（用电脑的 gcc 跑 Operation 的单测），
#     而本文件跑在交叉工具链下 —— 混进来会把测试编成 ARM 代码，根本跑不了。
#     `python Tools/fw.py test` 会单独 configure：cmake -S Test -B build/host
#     理由与设计见 Test/CMakeLists.txt 的文件头。
# ============================================================================
