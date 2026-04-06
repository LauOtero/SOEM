# CMakeLists.txt para módulo optimizado de SOEM
# Este módulo implementa técnicas avanzadas de optimización:
# - Memory pools pre-asignados
# - Algoritmos lock-free
# - Batch processing
# - Zero-copy support (opcional)
# - SIMD multi-arquitectura (x86, ARM, RISC-V)

cmake_minimum_required(VERSION 3.10)

project(ec_memopt VERSION 1.0.0 LANGUAGES C)

# Opciones de configuración
option(EC_MEMOPT_ENABLE "Habilitar optimizaciones de memoria y lock-free" ON)
option(EC_ZERO_COPY_ENABLE "Habilitar zero-copy networking (requiere TPACKET/AF_XDP)" OFF)
option(EC_BATCH_PROCESSING "Habilitar batch processing para throughput" ON)
option(EC_CACHE_ALIGNMENT "Habilitar alineación a línea de caché" ON)
option(EC_SIMD_ENABLE "Habilitar optimizaciones SIMD multi-arquitectura" ON)

# Verificar que se está compilando en Linux
if(NOT ${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
    message(WARNING "Las optimizaciones están diseñadas para Linux. Algunas características pueden no estar disponibles.")
endif()

# Detectar arquitectura del procesador
message(STATUS "Detecting architecture: ${CMAKE_SYSTEM_PROCESSOR}")

# Crear lista de fuentes
set(EC_MEMOPT_SOURCES
    src/ec_nicdrv_opt.c
)

# Agregar SIMD si está habilitado
if(EC_SIMD_ENABLE)
    list(APPEND EC_MEMOPT_SOURCES src/ec_simd.c)
    add_definitions(-DUSE_EC_SIMD=1)
    message(STATUS "SIMD optimizations enabled")
endif()

# Crear biblioteca de optimizaciones
add_library(ec_memopt STATIC ${EC_MEMOPT_SOURCES})

# Configurar include directories
target_include_directories(ec_memopt PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../osal>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../osal/linux>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../oshw/linux>
    $<INSTALL_INTERFACE:include/ec_memopt>
)

# Enlazar con SOEM principal
target_link_libraries(ec_memopt PUBLIC soem)

# Definiciones de compilación
target_compile_definitions(ec_memopt PUBLIC
    $<$<BOOL:${EC_MEMOPT_ENABLE}>:USE_EC_MEMOPT=1>
    $<$<BOOL:${EC_ZERO_COPY_ENABLE}>:USE_ZERO_COPY=1>
    $<$<BOOL:${EC_BATCH_PROCESSING}>:USE_BATCH_PROCESSING=1>
    $<$<BOOL:${EC_CACHE_ALIGNMENT}>:USE_CACHE_ALIGNMENT=1>
    $<$<BOOL:${EC_SIMD_ENABLE}>:USE_EC_SIMD=1>
)

# Flags de optimización específicos por arquitectura
set(EC_COMMON_FLAGS
    -Wall
    -Wextra
    -O3
    -ffast-math
    -funroll-loops
    -finline-functions
    -fprefetch-loop-arrays
)

# Flags específicos por arquitectura
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|i386|i686)$")
    # x86/x64: Habilitar extensiones según soporte
    set(EC_ARCH_FLAGS
        -march=native
        -mtune=native
    )
    message(STATUS "x86/x64 architecture detected - enabling SSE/AVX optimizations")
    
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(armv7l|armv7hf|armhf|arm)$")
    # ARMv7 (Raspberry Pi 2/3, BeagleBone, etc.)
    set(EC_ARCH_FLAGS
        -march=armv7-a
        -mfpu=neon-vfpv4
        -mfloat-abi=hard
        -mtune=cortex-a7
    )
    message(STATUS "ARMv7 NEON detected - enabling NEON optimizations")
    
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|armv8)$")
    # ARMv8/ARM64 (Raspberry Pi 4, Jetson, etc.)
    set(EC_ARCH_FLAGS
        -march=armv8-a+crc+simd
        -mtune=cortex-a72
    )
    message(STATUS "ARMv8/ARM64 detected - enabling NEON/SVE optimizations")
    
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(riscv|riscv64|riscv32)$")
    # RISC-V
    set(EC_ARCH_FLAGS
        -march=rv64gcv
        -mabi=lp64d
        -mtune=generic
    )
    message(STATUS "RISC-V detected - enabling RVV optimizations")
    
else()
    # Arquitectura desconocida - usar flags genéricos
    set(EC_ARCH_FLAGS)
    message(STATUS "Unknown architecture - using generic optimizations")
endif()

# Aplicar flags de compilación
target_compile_options(ec_memopt PRIVATE
    ${EC_COMMON_FLAGS}
    ${EC_ARCH_FLAGS}
)

# Enlaces necesarios
target_link_libraries(ec_memopt PUBLIC pthread rt)

# Instalar headers
install(FILES
    include/ec_memopt.h
    include/ec_simd.h
    DESTINATION include/ec_memopt
)

# Instalar biblioteca
install(TARGETS ec_memopt
    EXPORT ec_memoptConfig
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

# Exportar configuración
install(EXPORT ec_memoptConfig
    DESTINATION cmake
)

message(STATUS "=== Configuración de EC_MEMOPT ===")
message(STATUS "  Memory Pool:            ${EC_MEMOPT_ENABLE}")
message(STATUS "  Zero-Copy:              ${EC_ZERO_COPY_ENABLE}")
message(STATUS "  Batch Processing:       ${EC_BATCH_PROCESSING}")
message(STATUS "  Cache Alignment:        ${EC_CACHE_ALIGNMENT}")
message(STATUS "  SIMD Optimizations:     ${EC_SIMD_ENABLE}")
message(STATUS "  Sistema Operativo:      ${CMAKE_SYSTEM_NAME}")
message(STATUS "  Procesador:             ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "  Tipo de build:          ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler Flags:         ${EC_COMMON_FLAGS} ${EC_ARCH_FLAGS}")
message(STATUS "====================================")
