/* Test standalone para verificar compilación de SIMD */
#include "include/ec_memopt.h"
#include <stdio.h>
#include <string.h>

int main() {
    printf("=== SOEM Optimizado - Test SIMD ===\n\n");
    
    /* Reportar arquitectura detectada */
    printf("Arquitectura detectada: %s\n", EC_SIMD_ARCH);
    printf("SIMD habilitado: %s\n", EC_SIMD_ENABLED ? "SI" : "NO");
    printf("Ancho vector SIMD: %d bytes\n", EC_SIMD_WIDTH);
    printf("Tamaño línea caché: %d bytes\n", CACHE_LINE_SIZE);
    printf("\n");
    
    /* Test de funciones optimizadas */
    uint8_t src[128];
    uint8_t dst[128];
    uint8_t zero_buf[128];
    
    /* Inicializar datos de prueba */
    for (int i = 0; i < 128; i++) {
        src[i] = i & 0xFF;
        dst[i] = 0;
        zero_buf[i] = 0xAA;
    }
    
    /* Test ec_optimized_memcpy */
    printf("Test ec_optimized_memcpy (64 bytes)... ");
    ec_optimized_memcpy(dst, src, 64);
    int memcpy_ok = (memcmp(src, dst, 64) == 0) ? 1 : 0;
    printf("%s\n", memcpy_ok ? "OK" : "FAIL");
    
    /* Test ec_optimized_memzero */
    printf("Test ec_optimized_memzero (64 bytes)... ");
    ec_optimized_memzero(zero_buf, 64);
    int memzero_ok = 1;
    for (int i = 0; i < 64; i++) {
        if (zero_buf[i] != 0) {
            memzero_ok = 0;
            break;
        }
    }
    printf("%s\n", memzero_ok ? "OK" : "FAIL");
    
    /* Test ec_optimized_checksum */
    printf("Test ec_optimized_checksum... ");
    uint32_t chk1 = ec_optimized_checksum(src, 64);
    uint32_t chk2 = ec_optimized_checksum(src, 64);
    int checksum_ok = (chk1 == chk2) ? 1 : 0;
    printf("%s (checksum=0x%04X)\n", checksum_ok ? "OK" : "FAIL", chk1);
    
    /* Resumen */
    printf("\n=== Resultado ===\n");
    if (memcpy_ok && memzero_ok && checksum_ok) {
        printf("TODOS LOS TESTS PASARON\n");
        return 0;
    } else {
        printf("ALGUNOS TESTS FALLARON\n");
        return 1;
    }
}
