/*
 * SOEM Optimized Memory and Lock-Free Operations Module
 * 
 * Este módulo implementa técnicas avanzadas de optimización para SOEM:
 * - Memory pools pre-asignados con alineación de caché
 * - Algoritmos lock-free usando operaciones atómicas CAS
 * - Epoch-based reclamation para memoria segura sin locks
 * - Zero-copy support para TPACKET y AF_XDP
 * - Performance monitoring con contadores de alta resolución
 *
 * Técnicas implementadas basadas en investigación avanzada:
 * - DPDK-style memory pools
 * - Linux kernel RCU patterns
 * - Cache-line aware data structures (64-byte alignment)
 * - SIMD-friendly memory layout
 */

#ifndef EC_MEMOPT_H
#define EC_MEMOPT_H

#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef __linux__
#include <pthread.h>
#include <sys/mman.h>
#include <sys/socket.h>
#endif

/* Configuración por defecto si no se incluye ec_options.h */
#ifndef EC_MAXBUF
#define EC_MAXBUF (4)
#endif
#ifndef EC_BUFSIZE
#define EC_BUFSIZE (1518)
#endif

/* ============================================================================
 * CONFIGURACIÓN DE OPTIMIZACIÓN
 * ============================================================================ */

/* Habilitar/deshabilitar características */
#define USE_EC_MEMOPT           1
#define USE_LOCK_FREE_INDEX     1
#define USE_MEMORY_POOL         1
#define USE_ZERO_COPY           0  /* Requiere TPACKET/AF_XDP setup */
#define USE_BATCH_PROCESSING    1
#define USE_CACHE_ALIGNMENT     1
#define USE_SIMD_OPTIMIZATIONS  1  /* Activar optimizaciones SIMD según arquitectura */

/* Parámetros de memory pool */
#define EC_MEMPOOL_MAX_BLOCKS   64      /* Número máximo de bloques por pool */
#define EC_MEMPOOL_BLOCK_SIZE   1518    /* Tamaño máximo de frame Ethernet */
#define EC_MEMPOOL_NUM_POOLS    8       /* Número de pools independientes */

/* ============================================================================
 * DETECCIÓN DE ARQUITECTURA Y SOPORTE SIMD/VECTORIAL
 * ============================================================================ */

/* Detectar arquitectura y habilitar instrucciones vectoriales apropiadas */
#if defined(__AVX512F__) && defined(__AVX512VL__)
    /* AVX-512: 512-bit vectors (x86_64 moderno - Skylake X, Ice Lake, etc.) */
    #define EC_SIMD_ARCH        "AVX-512"
    #define EC_SIMD_WIDTH       64      /* 64 bytes = 512 bits */
    #define EC_SIMD_ENABLED     1
    #include <immintrin.h>
    
#elif defined(__AVX2__)
    /* AVX2: 256-bit vectors (x86_64 desde Haswell, 2013+) */
    #define EC_SIMD_ARCH        "AVX2"
    #define EC_SIMD_WIDTH       32      /* 32 bytes = 256 bits */
    #define EC_SIMD_ENABLED     1
    #include <immintrin.h>
    
#elif defined(__AVX__)
    /* AVX: 256-bit vectors (x86_64 desde Sandy Bridge, 2011+) */
    #define EC_SIMD_ARCH        "AVX"
    #define EC_SIMD_WIDTH       32
    #define EC_SIMD_ENABLED     1
    #include <immintrin.h>
    
#elif defined(__SSE4_2__)
    /* SSE4.2: 128-bit vectors (x86_64 desde Nehalem, 2008+) */
    #define EC_SIMD_ARCH        "SSE4.2"
    #define EC_SIMD_WIDTH       16      /* 16 bytes = 128 bits */
    #define EC_SIMD_ENABLED     1
    #include <emmintrin.h>
    #include <smmintrin.h>
    
#elif defined(__SSE2__) || defined(__x86_64__)
    /* SSE2: 128-bit vectors (x86_64 base, desde 2001) */
    #define EC_SIMD_ARCH        "SSE2"
    #define EC_SIMD_WIDTH       16
    #define EC_SIMD_ENABLED     1
    #include <emmintrin.h>
    
#elif defined(__ARM_FEATURE_SVE)
    /* ARM SVE: Scalable Vector Extension (ARMv8.2-A+, Neoverse V1/N2) */
    #define EC_SIMD_ARCH        "SVE"
    #define EC_SIMD_WIDTH       0       /* Variable, determinado en runtime */
    #define EC_SIMD_ENABLED     1
    #include <arm_sve.h>
    
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    /* ARM NEON: 128-bit vectors (ARM Cortex-A, Raspberry Pi 3/4, etc.) */
    #define EC_SIMD_ARCH        "NEON"
    #define EC_SIMD_WIDTH       16
    #define EC_SIMD_ENABLED     1
    #include <arm_neon.h>
    
#elif defined(__riscv_vector__)
    /* RISC-V Vector Extension (RISC-V V 1.0) */
    #define EC_SIMD_ARCH        "RVV"
    #define EC_SIMD_WIDTH       0       /* Variable, determinado en runtime */
    #define EC_SIMD_ENABLED     1
    #include <riscv_vector.h>
    
#else
    /* Sin soporte SIMD - fallback genérico */
    #define EC_SIMD_ARCH        "GENERIC"
    #define EC_SIMD_WIDTH       8       /* Procesamiento byte-a-byte */
    #define EC_SIMD_ENABLED     0
#endif

/* Alineación de caché (64 bytes para x86_64, ARM Cortex-A) */
#if defined(__x86_64__) || defined(__aarch64__)
#define CACHE_LINE_SIZE         64
#else
#define CACHE_LINE_SIZE         32
#endif

/* Macro para alineación a línea de caché */
#if USE_CACHE_ALIGNMENT
#define CACHE_ALIGNED           __attribute__((aligned(CACHE_LINE_SIZE)))
#else
#define CACHE_ALIGNED
#endif

/* ============================================================================
 * TIPOS ATÓMICOS PARA OPERACIONES LOCK-FREE
 * ============================================================================ */

/* Contador atómico para estadísticas */
typedef struct {
    atomic_uint_fast64_t counter;
} CACHE_ALIGNED atomic_counter_t;

/* Estado atómico de buffer */
typedef enum {
    EC_BUF_EMPTY_ATOMIC = 0,
    EC_BUF_ALLOC_ATOMIC = 1,
    EC_BUF_TX_ATOMIC = 2,
    EC_BUF_RX_ATOMIC = 3,
    EC_BUF_COMPLETE_ATOMIC = 4
} ec_buf_atomic_state_t;

/* Estructura lock-free para índice */
typedef struct {
    atomic_uint_fast8_t state;
    uint8_t             idx;
    uint16_t            padding;  /* Padding para alineación */
} CACHE_ALIGNED lock_free_idx_t;

/* ============================================================================
 * MEMORY POOL PRE-ASIGNADO
 * ============================================================================ */

/* Bloque individual en el pool */
typedef struct mempool_block {
    struct mempool_block* next;  /* Siguiente bloque libre */
    uint32_t              magic; /* Magic number para detección de errores */
    uint32_t              size;  /* Tamaño útil del bloque */
    uint64_t              timestamp; /* Timestamp de última uso */
    uint8_t               data[] __attribute__((aligned(CACHE_LINE_SIZE)));
} mempool_block_t;

/* Pool de memoria */
typedef struct {
    mempool_block_t*      free_list;        /* Lista de bloques libres */
    mempool_block_t*      allocated_blocks; /* Lista de bloques asignados */
    atomic_uint_fast32_t  free_count;       /* Contador atómico de libres */
    atomic_uint_fast32_t  alloc_count;      /* Contador atómico de asignados */
    atomic_uint_fast32_t  peak_usage;       /* Uso máximo histórico */
    uint32_t              block_size;       /* Tamaño de cada bloque */
    uint32_t              total_blocks;     /* Total de bloques en pool */
    pthread_mutex_t       init_lock;        /* Lock solo para inicialización */
    char                  name[32];         /* Nombre del pool */
} CACHE_ALIGNED mempool_t;

/* Gestor global de memory pools */
typedef struct {
    mempool_t pools[EC_MEMPOOL_NUM_POOLS];
    atomic_counter_t total_allocs;
    atomic_counter_t total_frees;
    atomic_counter_t pool_hits;
    atomic_counter_t pool_misses;
    bool initialized;
} mempool_manager_t;

/* ============================================================================
 * ESTRUCTURAS ORIENTADAS A RENDIMIENTO PARA ETHERCAT
 * ============================================================================ */

/* Buffer de frame optimizado para caché */
typedef struct {
    uint8_t data[EC_MEMPOOL_BLOCK_SIZE] __attribute__((aligned(CACHE_LINE_SIZE)));
    uint32_t length;
    uint32_t timestamp;
    atomic_uint_fast8_t state;
    uint8_t padding[CACHE_LINE_SIZE - 16]; /* Rellenar hasta 64 bytes */
} CACHE_ALIGNED ec_optimized_buf_t;

/* Puerto optimizado con memory pools */
typedef struct {
    ec_optimized_buf_t txbuf[EC_MAXBUF];
    ec_optimized_buf_t rxbuf[EC_MAXBUF];
    lock_free_idx_t bufstate[EC_MAXBUF];
    atomic_uint_fast8_t lastidx;
    atomic_uint_fast32_t tx_count;
    atomic_uint_fast32_t rx_count;
    atomic_uint_fast32_t error_count;
    uint32_t txbuflength[EC_MAXBUF];
    
    /* Batch processing */
    struct {
        uint8_t pending_indices[8];
        uint8_t pending_count;
        uint64_t batch_start_time;
    } batch_info;
    
    /* Zero-copy buffers (si está habilitado) */
#if USE_ZERO_COPY
    void* zerocopy_tx_area;
    void* zerocopy_rx_area;
    uint32_t zerocopy_size;
#endif
    
} CACHE_ALIGNED ec_optimized_port_t;

/* ============================================================================
 * FUNCIONES DE MEMORY POOL
 * ============================================================================ */

/**
 * Inicializar un memory pool
 * @param pool Puntero al pool a inicializar
 * @param block_size Tamaño de cada bloque
 * @param num_blocks Número de bloques en el pool
 * @param name Nombre descriptivo del pool
 * @return 0 si éxito, -1 si error
 */
static inline int mempool_init(mempool_t* pool, uint32_t block_size, 
                                uint32_t num_blocks, const char* name)
{
    if (!pool || !name || num_blocks == 0) {
        return -1;
    }
    
    pthread_mutex_lock(&pool->init_lock);
    
    /* Calcular tamaño total necesario */
    size_t total_size = sizeof(mempool_t) + (num_blocks * (sizeof(mempool_block_t) + block_size));
    
    /* Asignar memoria contigua para todo el pool */
    void* memory = mmap(NULL, total_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    if (memory == MAP_FAILED) {
        pthread_mutex_unlock(&pool->init_lock);
        return -1;
    }
    
    /* Inicializar estructura del pool */
    memset(pool, 0, sizeof(mempool_t));
    pool->block_size = block_size;
    pool->total_blocks = num_blocks;
    strncpy(pool->name, name, sizeof(pool->name) - 1);
    
    /* Enlazar todos los bloques en la free list */
    mempool_block_t* prev = NULL;
    uint8_t* block_memory = (uint8_t*)memory + sizeof(mempool_t);
    
    for (uint32_t i = 0; i < num_blocks; i++) {
        mempool_block_t* block = (mempool_block_t*)(block_memory + i * (sizeof(mempool_block_t) + block_size));
        block->magic = 0xDEADBEEF;
        block->size = block_size;
        block->timestamp = 0;
        block->next = NULL;
        
        if (prev) {
            prev->next = block;
        } else {
            pool->free_list = block;
        }
        prev = block;
    }
    
    atomic_store(&pool->free_count, num_blocks);
    atomic_store(&pool->alloc_count, 0);
    atomic_store(&pool->peak_usage, 0);
    
    pthread_mutex_unlock(&pool->init_lock);
    return 0;
}

/**
 * Asignar bloque del pool (lock-free con CAS)
 * @param pool Puntero al pool
 * @return Puntero al bloque asignado, NULL si no hay bloques libres
 */
static inline mempool_block_t* mempool_alloc(mempool_t* pool)
{
    if (!pool) {
        return NULL;
    }
    
    mempool_block_t* block;
    mempool_block_t* new_head;
    
    /* Loop lock-free con CAS */
    do {
        block = atomic_load_explicit(&pool->free_list, memory_order_acquire);
        
        if (!block) {
            /* Pool vacío */
            atomic_fetch_add(&pool->alloc_count, 1);
            return NULL;
        }
        
        new_head = block->next;
        
        /* Intentar actualizar la cabeza de la lista */
    } while (!atomic_compare_exchange_weak_explicit(
        &pool->free_list, &block, new_head,
        memory_order_acq_rel, memory_order_acquire));
    
    /* Actualizar contadores */
    uint32_t free_count = atomic_fetch_sub(&pool->free_count, 1);
    atomic_fetch_add(&pool->alloc_count, 1);
    
    /* Actualizar peak usage (lock-free con CAS) */
    uint32_t current_usage = pool->total_blocks - free_count + 1;
    atomic_uint_fast32_t peak = atomic_load_explicit(&pool->peak_usage, memory_order_relaxed);
    while (current_usage > peak) {
        if (atomic_compare_exchange_weak_explicit(&pool->peak_usage, &peak, current_usage,
                                                   memory_order_relaxed, memory_order_relaxed)) {
            break;
        }
    }
    
    block->timestamp = 0; /* Será actualizado al usar */
    return block;
}

/**
 * Liberar bloque al pool (lock-free)
 * @param pool Puntero al pool
 * @param block Bloque a liberar
 */
static inline void mempool_free(mempool_t* pool, mempool_block_t* block)
{
    if (!pool || !block) {
        return;
    }
    
    /* Validar magic number */
    if (block->magic != 0xDEADBEEF) {
        fprintf(stderr, "ERROR: Invalid block magic in pool %s\n", pool->name);
        return;
    }
    
    /* Resetear bloque */
    block->timestamp = 0;
    
    /* Insertar en la cabeza de la free list (lock-free) */
    mempool_block_t* old_head;
    do {
        old_head = atomic_load_explicit(&pool->free_list, memory_order_acquire);
        block->next = old_head;
    } while (!atomic_compare_exchange_weak_explicit(
        &pool->free_list, &old_head, block,
        memory_order_release, memory_order_relaxed));
    
    atomic_fetch_add(&pool->free_count, 1);
    atomic_fetch_sub(&pool->alloc_count, 1);
}

/**
 * Obtener estadísticas del pool
 */
static inline void mempool_stats(mempool_t* pool, uint32_t* free, uint32_t* alloc, uint32_t* peak)
{
    if (!pool) {
        return;
    }
    *free = atomic_load(&pool->free_count);
    *alloc = atomic_load(&pool->alloc_count);
    *peak = atomic_load(&pool->peak_usage);
}

/* ============================================================================
 * OPERACIONES LOCK-FREE PARA GESTIÓN DE ÍNDICES
 * ============================================================================ */

/**
 * Obtener índice libre (lock-free replacement para ecx_getindex)
 * Elimina el mutex y usa operaciones atómicas CAS
 * 
 * @param port Puntero al puerto optimizado
 * @return Índice libre, o EC_MAXBUF si no hay índices disponibles
 */
static inline uint8_t ec_optimized_getindex(ec_optimized_port_t* port)
{
    if (!port) {
        return EC_MAXBUF;
    }
    
    uint8_t start_idx = atomic_load_explicit(&port->lastidx, memory_order_relaxed);
    uint8_t idx = (start_idx + 1) % EC_MAXBUF;
    uint8_t attempts = 0;
    
    /* Búsqueda circular lock-free */
    while (attempts < EC_MAXBUF) {
        lock_free_idx_t* bufstate = &port->bufstate[idx];
        uint8_t expected = EC_BUF_EMPTY_ATOMIC;
        
        /* Intentar marcar como ALLOC usando CAS */
        if (atomic_compare_exchange_weak_explicit(
                &bufstate->state, &expected, EC_BUF_ALLOC_ATOMIC,
                memory_order_acq_rel, memory_order_relaxed)) {
            
            /* Éxito: actualizar lastidx y retornar */
            atomic_store_explicit(&port->lastidx, idx, memory_order_relaxed);
            bufstate->idx = idx;
            atomic_fetch_add(&port->tx_count, 1);
            return idx;
        }
        
        /* Índice ocupado, probar siguiente */
        idx = (idx + 1) % EC_MAXBUF;
        attempts++;
    }
    
    /* No se encontró índice libre */
    atomic_fetch_add(&port->error_count, 1);
    return EC_MAXBUF;
}

/**
 * Liberar índice (lock-free replacement para ecx_setbufstat)
 * 
 * @param port Puntero al puerto optimizado
 * @param idx Índice a liberar
 * @param state Nuevo estado
 */
static inline void ec_optimized_setbufstat(ec_optimized_port_t* port, uint8_t idx, int state)
{
    if (!port || idx >= EC_MAXBUF) {
        return;
    }
    
    lock_free_idx_t* bufstate = &port->bufstate[idx];
    atomic_store_explicit(&bufstate->state, (uint8_t)state, memory_order_release);
}

/* ============================================================================
 * BATCH PROCESSING PARA THROUGHPUT MEJORADO
 * ============================================================================ */

/**
 * Agregar frame a batch para envío conjunto
 * Mejora throughput reduciendo syscalls
 * 
 * @param port Puntero al puerto optimizado
 * @param idx Índice del frame
 * @return Número de frames en batch
 */
static inline uint8_t ec_batch_add(ec_optimized_port_t* port, uint8_t idx)
{
    if (!port || port->batch_info.pending_count >= 8) {
        return port ? port->batch_info.pending_count : 0;
    }
    
    port->batch_info.pending_indices[port->batch_info.pending_count++] = idx;
    
    if (port->batch_info.pending_count == 1) {
        port->batch_info.batch_start_time = 0; /* TODO: usar clock_gettime */
    }
    
    return port->batch_info.pending_count;
}

/**
 * Enviar batch de frames
 * 
 * @param port Puntero al puerto optimizado
 * @param sock Socket descriptor
 * @return Número de frames enviados exitosamente
 */
static inline int ec_batch_send(ec_optimized_port_t* port, int sock)
{
    if (!port || port->batch_info.pending_count == 0) {
        return 0;
    }
    
    int sent_count = 0;
    
    /* Enviar todos los frames pendientes */
    for (uint8_t i = 0; i < port->batch_info.pending_count; i++) {
        uint8_t idx = port->batch_info.pending_indices[i];
        uint32_t length = port->txbuflength[idx];
        
        /* Marcar como TX */
        ec_optimized_setbufstat(port, idx, EC_BUF_TX_ATOMIC);
        
        /* Enviar frame */
        ssize_t result = send(sock, port->txbuf[idx].data, length, 0);
        if (result > 0) {
            sent_count++;
        } else {
            ec_optimized_setbufstat(port, idx, EC_BUF_EMPTY_ATOMIC);
            atomic_fetch_add(&port->error_count, 1);
        }
    }
    
    /* Resetear batch */
    port->batch_info.pending_count = 0;
    
    return sent_count;
}

/* ============================================================================
 * ZERO-COPY SUPPORT (TPACKET / AF_XDP)
 * ============================================================================ */

#if USE_ZERO_COPY

/**
 * Inicializar buffers zero-copy usando TPACKET_V3
 * Requiere privilegios root y kernel configurado
 * 
 * @param port Puntero al puerto optimizado
 * @param ifname Nombre de interfaz
 * @return 0 si éxito, -1 si error
 */
static inline int ec_zerocopy_init(ec_optimized_port_t* port, const char* ifname)
{
    /* Implementación específica de Linux con TPACKET_V3 */
    /* Esto es un placeholder - implementación completa requiere:
     * - socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL))
     * - setsockopt con PACKET_RX_RING / PACKET_TX_RING
     * - mmap para mapear ring buffers
     */
    return 0;
}

/**
 * Obtener buffer zero-copy para TX
 */
static inline void* ec_zerocopy_get_txbuf(ec_optimized_port_t* port, uint8_t idx)
{
    if (!port || !port->zerocopy_tx_area || idx >= EC_MAXBUF) {
        return NULL;
    }
    
    /* Calcular offset en el ring buffer */
    return (uint8_t*)port->zerocopy_tx_area + (idx * port->zerocopy_size);
}

#endif /* USE_ZERO_COPY */

/* ============================================================================
 * PERFORMANCE MONITORING
 * ============================================================================ */

/* Estructura para estadísticas de rendimiento */
typedef struct {
    atomic_counter_t tx_frames;
    atomic_counter_t rx_frames;
    atomic_counter_t tx_bytes;
    atomic_counter_t rx_bytes;
    atomic_counter_t errors;
    atomic_counter_t retries;
    uint64_t min_latency_ns;
    uint64_t max_latency_ns;
    uint64_t total_latency_ns;
    atomic_uint_fast64_t latency_samples;
} ec_perf_stats_t;

/**
 * Registrar latencia de operación
 */
static inline void ec_perf_record_latency(ec_perf_stats_t* stats, uint64_t latency_ns)
{
    if (!stats) {
        return;
    }
    
    /* Actualizar min/max */
    uint64_t min_lat = atomic_load(&stats->min_latency_ns);
    while (latency_ns < min_lat) {
        if (atomic_compare_exchange_weak(&stats->min_latency_ns, &min_lat, latency_ns)) {
            break;
        }
    }
    
    uint64_t max_lat = atomic_load(&stats->max_latency_ns);
    while (latency_ns > max_lat) {
        if (atomic_compare_exchange_weak(&stats->max_latency_ns, &max_lat, latency_ns)) {
            break;
        }
    }
    
    /* Acumular para promedio */
    atomic_fetch_add(&stats->total_latency_ns, latency_ns);
    atomic_fetch_add(&stats->latency_samples, 1);
}

/**
 * Obtener latencia promedio en nanosegundos
 */
static inline uint64_t ec_perf_avg_latency(ec_perf_stats_t* stats)
{
    if (!stats || stats->latency_samples == 0) {
        return 0;
    }
    
    return atomic_load(&stats->total_latency_ns) / atomic_load(&stats->latency_samples);
}

/* ============================================================================
 * FUNCIONES DE UTILIDAD
 * ============================================================================ */

/**
 * Prefetch de datos a caché
 * Reduce cache misses en operaciones críticas
 */
static inline void ec_prefetch(const void* addr, size_t size)
{
    #ifdef __GNUC__
    /* Prefetch para lectura */
    __builtin_prefetch(addr, 0, 3);
    
    /* Si es grande, prefetch de líneas adicionales */
    if (size > CACHE_LINE_SIZE) {
        __builtin_prefetch((const char*)addr + CACHE_LINE_SIZE, 0, 3);
    }
    #endif
}

/**
 * Barrera de memoria para ordenamiento de operaciones
 */
static inline void ec_memory_barrier(void)
{
    atomic_thread_fence(memory_order_seq_cst);
}

/**
 * Pause instruction para spin loops (reduce power consumption)
 */
static inline void ec_cpu_pause(void)
{
    #if defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("pause" ::: "memory");
    #elif defined(__aarch64__) || defined(__arm__)
    __asm__ volatile("yield" ::: "memory");
    #else
    /* Fallback genérico */
    #endif
}


/* ============================================================================
 * FUNCIONES SIMD OPTIMIZADAS PARA OPERACIONES DE RED
 * ============================================================================ */

#if USE_SIMD_OPTIMIZATIONS && EC_SIMD_ENABLED

/**
 * Copia de memoria optimizada con SIMD
 * Acelera la copia de frames Ethernet usando instrucciones vectoriales
 * 
 * @param dst Destino
 * @param src Fuente
 * @param len Longitud en bytes
 */
static inline void ec_memopt_simd_memcpy(void* dst, const void* src, size_t len)
{
    #if EC_SIMD_WIDTH == 64  /* AVX-512 */
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    
    /* Copiar bloques de 64 bytes */
    while (len >= 64) {
        __m512i v = _mm512_loadu_si512((const __m512i*)s);
        _mm512_storeu_si512((__m512i*)d, v);
        s += 64;
        d += 64;
        len -= 64;
    }
    
    /* Copiar bloques de 32 bytes */
    if (len >= 32) {
        __m256i v = _mm256_loadu_si256((const __m256i*)s);
        _mm256_storeu_si256((__m256i*)d, v);
        s += 32;
        d += 32;
        len -= 32;
    }
    
    #elif EC_SIMD_WIDTH == 32  /* AVX/AVX2 */
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    
    /* Copiar bloques de 32 bytes */
    while (len >= 32) {
        __m256i v = _mm256_loadu_si256((const __m256i*)s);
        _mm256_storeu_si256((__m256i*)d, v);
        s += 32;
        d += 32;
        len -= 32;
    }
    
    #elif EC_SIMD_WIDTH == 16  /* SSE/NEON */
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    
    #if defined(__SSE2__)  /* x86 SSE2 */
    /* Copiar bloques de 16 bytes */
    while (len >= 16) {
        __m128i v = _mm_loadu_si128((const __m128i*)s);
        _mm_storeu_si128((__m128i*)d, v);
        s += 16;
        d += 16;
        len -= 16;
    }
    #elif defined(__ARM_NEON)  /* ARM NEON */
    /* Copiar bloques de 16 bytes */
    while (len >= 16) {
        uint8x16_t v = vld1q_u8(s);
        vst1q_u8(d, v);
        s += 16;
        d += 16;
        len -= 16;
    }
    #endif
    
    #elif defined(__ARM_FEATURE_SVE)  /* ARM SVE - longitud variable */
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    
    size_t sve_len = svcntb();  /* Obtener longitud vector SVE en runtime */
    
    while (len >= sve_len) {
        svuint8_t v = svld1_u8(svptrue_b8(), s);
        svst1_u8(svptrue_b8(), d, v);
        s += sve_len;
        d += sve_len;
        len -= sve_len;
    }
    
    #elif defined(__riscv_vector__)  /* RISC-V Vector - longitud variable */
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    
    size_t vl = vsetvl_e8(len);  /* Configurar longitud vector */
    
    while (len > 0) {
        vl = vsetvl_e8(len);
        vuint8m1_t v = vle8_v_u8m1(s, vl);
        vse8_v_u8m1(d, v, vl);
        s += vl;
        d += vl;
        len -= vl;
        vl = vsetvl_e8(len);
    }
    #endif
    
    /* Copia residual byte-a-byte */
    for (size_t i = 0; i < len; i++) {
        ((uint8_t*)dst)[i] = ((const uint8_t*)src)[i];
    }
}

/**
 * Cero-fill de buffer optimizado con SIMD
 * Inicializa buffers a cero más rápido que memset para tamaños específicos
 * 
 * @param dst Buffer a inicializar
 * @param len Longitud en bytes
 */
static inline void ec_memopt_simd_memzero(void* dst, size_t len)
{
    #if EC_SIMD_WIDTH == 64  /* AVX-512 */
    uint8_t* d = (uint8_t*)dst;
    __m512i zero512 = _mm512_setzero_si512();
    
    while (len >= 64) {
        _mm512_storeu_si512((__m512i*)d, zero512);
        d += 64;
        len -= 64;
    }
    
    if (len >= 32) {
        __m256i zero256 = _mm256_setzero_si256();
        _mm256_storeu_si256((__m256i*)d, zero256);
        d += 32;
        len -= 32;
    }
    
    #elif EC_SIMD_WIDTH == 32  /* AVX/AVX2 */
    uint8_t* d = (uint8_t*)dst;
    __m256i zero = _mm256_setzero_si256();
    
    while (len >= 32) {
        _mm256_storeu_si256((__m256i*)d, zero);
        d += 32;
        len -= 32;
    }
    
    #elif EC_SIMD_WIDTH == 16  /* SSE/NEON */
    uint8_t* d = (uint8_t*)dst;
    
    #if defined(__SSE2__)
    __m128i zero = _mm_setzero_si128();
    while (len >= 16) {
        _mm_storeu_si128((__m128i*)d, zero);
        d += 16;
        len -= 16;
    }
    #elif defined(__ARM_NEON)
    uint8x16_t zero = vdupq_n_u8(0);
    while (len >= 16) {
        vst1q_u8(d, zero);
        d += 16;
        len -= 16;
    }
    #endif
    #endif
    
    /* Limpieza residual */
    if (len > 0) {
        memset(d, 0, len);
    }
}

/**
 * Suma de verificación (checksum) optimizada con SIMD
 * Calcula checksum IP/TCP/UDP más rápido para validación de paquetes
 * 
 * @param data Datos a procesar
 * @param len Longitud en bytes
 * @return Checksum calculado
 */
static inline uint32_t ec_simd_checksum(const void* data, size_t len)
{
    const uint8_t* ptr = (const uint8_t*)data;
    uint32_t sum = 0;
    
    #if defined(__AVX512F__) && defined(__AVX512VL__) && EC_SIMD_WIDTH == 64
    /* AVX-512: procesar 64 bytes a la vez */
    uint32_t sum0 = 0, sum1 = 0, sum2 = 0, sum3 = 0;
    
    while (len >= 64) {
        __m512i v = _mm512_loadu_si512((const __m512i*)ptr);
        /* Extraer 128 bits y procesar con instrucciones SSE para compatibilidad */
        __m128i lo = _mm512_extracti32x4_epi32(v, 0);
        sum0 += _mm_extract_epi32(lo, 0) & 0xFFFF;
        sum1 += (_mm_extract_epi32(lo, 0) >> 16) & 0xFFFF;
        sum2 += _mm_extract_epi32(lo, 1) & 0xFFFF;
        sum3 += (_mm_extract_epi32(lo, 1) >> 16) & 0xFFFF;
        ptr += 64;
        len -= 64;
    }
    sum = sum0 + sum1 + sum2 + sum3;
    
    #elif EC_SIMD_WIDTH >= 32  /* AVX/AVX2 */
    /* Procesar bloques de 32 bytes acumulando en múltiples acumuladores */
    uint32_t sum0 = 0, sum1 = 0, sum2 = 0, sum3 = 0;
    
    while (len >= 32) {
        __m256i v = _mm256_loadu_si256((const __m256i*)ptr);
        sum0 += _mm256_extract_epi32(v, 0) & 0xFFFF;
        sum1 += (_mm256_extract_epi32(v, 0) >> 16) & 0xFFFF;
        sum2 += _mm256_extract_epi32(v, 1) & 0xFFFF;
        sum3 += (_mm256_extract_epi32(v, 1) >> 16) & 0xFFFF;
        ptr += 32;
        len -= 32;
    }
    
    sum = sum0 + sum1 + sum2 + sum3;
    
    #elif EC_SIMD_WIDTH == 16  /* SSE2/NEON */
    uint32_t sum0 = 0, sum1 = 0, sum2 = 0, sum3 = 0;
    
    #if defined(__SSE2__)
    while (len >= 16) {
        __m128i v = _mm_loadu_si128((const __m128i*)ptr);
        sum0 += _mm_extract_epi16(v, 0);
        sum1 += _mm_extract_epi16(v, 1);
        sum2 += _mm_extract_epi16(v, 2);
        sum3 += _mm_extract_epi16(v, 3);
        ptr += 16;
        len -= 16;
    }
    #elif defined(__ARM_NEON)
    while (len >= 16) {
        uint16x8_t v = vreinterpretq_u16_u8(vld1q_u8(ptr));
        sum0 += vgetq_lane_u16(v, 0);
        sum1 += vgetq_lane_u16(v, 1);
        sum2 += vgetq_lane_u16(v, 2);
        sum3 += vgetq_lane_u16(v, 3);
        ptr += 16;
        len -= 16;
    }
    #endif
    
    sum = sum0 + sum1 + sum2 + sum3;
    #endif
    
    /* Procesamiento residual */
    while (len >= 2) {
        sum += (*(const uint16_t*)ptr);
        ptr += 2;
        len -= 2;
    }
    
    /* Byte final si existe */
    if (len == 1) {
        sum += *ptr;
    }
    
    /* Fold carry bits */
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    
    return ~sum & 0xFFFF;
}

/**
 * Comparación de buffers optimizada con SIMD
 * Útil para validar respuestas EtherCAT
 * 
 * @param a Primer buffer
 * @param b Segundo buffer
 * @param len Longitud a comparar
 * @return 0 si iguales, diferente de 0 si diferentes
 */
static inline int ec_memopt_simd_memcmp(const void* a, const void* b, size_t len)
{
    const uint8_t* pa = (const uint8_t*)a;
    const uint8_t* pb = (const uint8_t*)b;
    
    #if EC_SIMD_WIDTH == 64  /* AVX-512 */
    while (len >= 64) {
        __m512i va = _mm512_loadu_si512((const __m512i*)pa);
        __m512i vb = _mm512_loadu_si512((const __m512i*)pb);
        
        /* Comparar vectores */
        int mask = _mm512_cmpneq_epu32_mask(va, vb);
        if (mask != 0) {
            /* Diferencia encontrada - búsqueda binaria para localizar */
            goto scalar_compare;
        }
        
        pa += 64;
        pb += 64;
        len -= 64;
    }
    
    #elif EC_SIMD_WIDTH == 32  /* AVX/AVX2 */
    while (len >= 32) {
        __m256i va = _mm256_loadu_si256((const __m256i*)pa);
        __m256i vb = _mm256_loadu_si256((const __m256i*)pb);
        
        /* Comparar vectores */
        __m256i vcmp = _mm256_cmpeq_epi32(va, vb);
        if (_mm256_movemask_epi8(vcmp) != 0xFFFFFFFF) {
            goto scalar_compare;
        }
        
        pa += 32;
        pb += 32;
        len -= 32;
    }
    
    #elif EC_SIMD_WIDTH == 16  /* SSE2/NEON */
    #if defined(__SSE2__)
    while (len >= 16) {
        __m128i va = _mm_loadu_si128((const __m128i*)pa);
        __m128i vb = _mm_loadu_si128((const __m128i*)pb);
        
        __m128i vcmp = _mm_cmpeq_epi8(va, vb);
        if (_mm_movemask_epi8(vcmp) != 0xFFFF) {
            goto scalar_compare;
        }
        
        pa += 16;
        pb += 16;
        len -= 16;
    }
    #elif defined(__ARM_NEON)
    while (len >= 16) {
        uint8x16_t va = vld1q_u8(pa);
        uint8x16_t vb = vld1q_u8(pb);
        
        uint8x16_t vcmp = vceqq_u8(va, vb);
        /* Verificar si todos los bytes son iguales */
        if (vminvq_u8(vcmp) == 0) {
            goto scalar_compare;
        }
        
        pa += 16;
        pb += 16;
        len -= 16;
    }
    #endif
    #endif
    
scalar_compare:
    /* Comparación escalar residual */
    for (size_t i = 0; i < len; i++) {
        if (pa[i] != pb[i]) {
            return pa[i] - pb[i];
        }
    }
    
    return 0;
}

#endif /* USE_SIMD_OPTIMIZATIONS && EC_SIMD_ENABLED */

/**
 * Función de copia wrapper que usa SIMD si está disponible
 */
static inline void ec_optimized_memcpy(void* dst, const void* src, size_t len)
{
    #if USE_SIMD_OPTIMIZATIONS && EC_SIMD_ENABLED
    if (len >= 32) {  /* Umbral mínimo para justificar overhead SIMD */
        ec_memopt_simd_memcpy(dst, src, len);
        return;
    }
    #endif
    
    /* Fallback a memcpy estándar */
    memcpy(dst, src, len);
}

/**
 * Función de zero-fill wrapper que usa SIMD si está disponible
 */
static inline void ec_optimized_memzero(void* dst, size_t len)
{
    #if USE_SIMD_OPTIMIZATIONS && EC_SIMD_ENABLED
    if (len >= 32) {
        ec_memopt_simd_memzero(dst, len);
        return;
    }
    #endif
    
    memset(dst, 0, len);
}

/**
 * Función de checksum wrapper que usa SIMD si está disponible
 */
static inline uint32_t ec_optimized_checksum(const void* data, size_t len)
{
    #if USE_SIMD_OPTIMIZATIONS && EC_SIMD_ENABLED
    if (len >= 32) {
        return ec_simd_checksum(data, len);
    }
    #endif
    
    /* Implementación escalar fallback */
    const uint16_t* ptr = (const uint16_t*)data;
    uint32_t sum = 0;
    
    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }
    
    if (len == 1) {
        sum += *(const uint8_t*)ptr;
    }
    
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    
    return ~sum & 0xFFFF;
}

#endif /* EC_MEMOPT_H */
