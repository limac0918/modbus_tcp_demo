cmake_minimum_required(VERSION 3.16)
project(ModbusTcpDemo VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# ---- 仅 Qt5 ----
# SerialBus 模块提供 QModbusTcpClient；Sql 模块提供 QSqlDatabase（报警日志 SQLite）。
# 最低 5.11：QModbusDevice::errorOccurred 信号自 5.11 引入；
# 建议 5.12 LTS 及以上（errorOccurred/State 枚举均已具备）。

# 未配置 CMAKE_PREFIX_PATH 时，用 $HOME 兜底（用户可随时覆盖）
if(NOT CMAKE_PREFIX_PATH)
    set(CMAKE_PREFIX_PATH "$ENV{HOME}/Qt/5.15.2/gcc_64")
endif()

find_package(Qt5 5.11 COMPONENTS Core Widgets SerialBus Sql REQUIRED)

# =====================================================================
# 产物分类输出：最终产物按类别分目录，全部集中在 build/ 下
#
#   构建后 build/ 结构：
#   build/
#   ├── bin/              可执行文件（modbus_tcp_demo）
#   ├── lib/              动态库(.so) / 静态库(.a)
#   ├── pdb/              调试符号（仅 MSVC 生成 .pdb）
#   ├── CMakeFiles/       中间产物：.o/.d/.cmake 等（CMake 自动集中于此，
#   │                     对象文件固定在 CMakeFiles/<target>.dir/ 下，
#   │                     不支持自定义到其他目录）
#   ├── Makefile / build.ninja 等构建脚本
#   └── CMakeCache.txt    配置缓存
#
# 单配置生成器（Unix Makefiles / Ninja）：路径直接用 ${CMAKE_BINARY_DIR}/xxx
# 多配置生成器（Visual Studio / Xcode）：需追加 $<CONFIG> 子目录，见下方注释
# =====================================================================
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pdb")
set(CMAKE_COMPILE_PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pdb")

# Debug 版可执行/库加 "_d" 后缀，便于区分版本：
#   modbus_tcp_demo      (Release)
#   modbus_tcp_demod     (Debug)
# set(CMAKE_DEBUG_POSTFIX "d")

# 【若用多配置生成器(VS/Xcode)，把上面 5 行替换为：】
# foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
#     set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
#     set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/lib/$<CONFIG>")
#     set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/lib/$<CONFIG>")
#     set(CMAKE_PDB_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/pdb/$<CONFIG>")
# endforeach()

add_executable(modbus_tcp_demo
    src/main.cpp
    src/modbusmanager.cpp
    src/modbusmanager.h
    src/alarmlogger.cpp
    src/alarmlogger.h
    src/mainwindow.cpp
    src/mainwindow.h
)

target_link_libraries(modbus_tcp_demo PRIVATE Qt5::Core Qt5::Widgets Qt5::SerialBus Qt5::Sql)

# =====================================================================
# 安装规则（可选）：cmake --install build --prefix <dir>
#   默认装到 <prefix>/bin/modbus_tcp_demo
#   注意：本 demo 默认定位是"直接跑 build/bin/modbus_tcp_demo"，不安装也能用。
# =====================================================================
include(GNUInstallDirs)
install(TARGETS modbus_tcp_demo
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# 【Qt 库查找方式】本工程不设置 RPATH：
#   已通过用户级 bashrc 的 LD_LIBRARY_PATH 指向 $HOME/Qt/.../lib 解决运行时找库，
#   不再把绝对路径写进二进制（避免换机/换路径后 RPATH 失效导致版本错配）。
#   如果以后想去掉对环境变量的依赖，再取消注释下面的 RPATH 配置。
# 可选（取消注释以启用 RPATH，替代 bashrc 的 LD_LIBRARY_PATH）：
# set_target_properties(modbus_tcp_demo PROPERTIES
#     BUILD_RPATH   "${CMAKE_PREFIX_PATH}/lib"
#     INSTALL_RPATH "${CMAKE_PREFIX_PATH}/lib"
# )
