# ============================================================================
#  Test/ —— PC 侧单测（host gcc 编译，不需要板子）
#
#  ⚠️ 本文件**刻意不写 GLOB** —— 加一个测试用例必须**手工加两行**：
#        1) 在 OP_SRC 里加被测算法的 .c
#        2) 加一条 add_executable + add_test
#
#     为什么故意不自动化：漏加的表现是"测试全过，但那个模块根本没被测" ——
#     这类**静默失效**没有任何报错，只能靠"加测试时多做一个动作"来防。
#     代价是要手工加两行，收益是"没测"不会伪装成"测过了"。
# ============================================================================
cmake_minimum_required(VERSION 3.21)

# ⚠️ 本文件是**独立 configure** 的（`cmake -S Test -B build/host`），
#    不挂在顶层 CMakeLists.txt 下 —— 那边跑的是交叉工具链，会把测试编成 ARM
#    代码，根本没法在电脑上运行。所以这里必须自己 project()。
project(proj_tests C)

# ⚠️ enable_testing() 不能省：少了它 add_test() **不报错、不注册**，
#    `ctest` 跑出"0 个测试"却退出码 0 —— 门禁静默失效的典型形态。
enable_testing()

# ---- 被测的 Operation 源码（与 MCU 侧**同一份文件**，不是副本）--------------
set(OP_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/../Operation/Src/op_blink.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Operation/Src/op_flash.c
)

# ---- 共用件 ----------------------------------------------------------------
# log.c 是 Common 层的格式化实现；log_put() 由 stubs/log_host.c 提供（PC 侧注入）
set(TEST_COMMON_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/../Common/Src/log.c
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs/log_host.c
)

# ⚠️ 与 MCU 侧**同一套告警**：测试代码的告警也是失败。
#    ★ 列表来自 `cmake/proj_warnings.cmake`，**不是各抄一份** ——
#      原先两边各写一遍、靠注释"改一处必须改另一处"约束，那是双真相源，迟早漂。
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/proj_warnings.cmake)

set(TEST_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/../Operation/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Common/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}                 # test_util.h
    ${CMAKE_CURRENT_SOURCE_DIR}/..              # project_config.h
)

# ---- 用例清单（加用例就加在这里，见文件头）----------------------------------
# 1) op_blink
add_executable(test_op_blink
    ${CMAKE_CURRENT_SOURCE_DIR}/test_op_blink.c
    ${OP_SRC}
    ${TEST_COMMON_SRC})
target_include_directories(test_op_blink PRIVATE ${TEST_INCLUDES})
target_compile_options(test_op_blink PRIVATE ${PROJ_WARNING_FLAGS})
# ★ 标准必须与 MCU 侧一致（顶层 CMakeLists 是 C11）。用 c_std_99 的话会出现
#   "PC 上编得过、MCU 上编不过"（或反过来）—— 那是最费时间的一类困惑。
target_compile_features(test_op_blink PRIVATE c_std_11)
add_test(NAME op_blink COMMAND test_op_blink)

# 2) op_flash —— SPI Flash 页写拆分的纯逻辑
add_executable(test_op_flash
    ${CMAKE_CURRENT_SOURCE_DIR}/test_op_flash.c
    ${OP_SRC}
    ${TEST_COMMON_SRC})
target_include_directories(test_op_flash PRIVATE ${TEST_INCLUDES})
target_compile_options(test_op_flash PRIVATE ${PROJ_WARNING_FLAGS})
target_compile_features(test_op_flash PRIVATE c_std_11)
add_test(NAME op_flash COMMAND test_op_flash)

# 3) 下一个用例照抄上面四行，改名字即可。
