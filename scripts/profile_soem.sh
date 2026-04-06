#!/bin/bash
# Script de profiling para SOEM optimizado
# Usa perf y valgrind para analizar rendimiento

set -e

echo "=============================================="
echo "SOEM Profiling Suite"
echo "=============================================="

# Directorios
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_DIR="$(dirname "$SCRIPT_DIR")"
OUTPUT_DIR="$WORKSPACE_DIR/profiling_results"

mkdir -p "$OUTPUT_DIR"

echo ""
echo "Directorio de salida: $OUTPUT_DIR"
echo ""

# Función para ejecutar perf
run_perf() {
    local program=$1
    local output_name=$2
    
    echo "📊 Ejecutando perf para: $program"
    
    # Perf stat - métricas generales
    perf stat -e cycles,instructions,cache-references,cache-misses,\
branch-instructions,branch-misses,L1-dcache-loads,L1-dcache-load-misses \
    -o "$OUTPUT_DIR/${output_name}_perf_stat.txt" \
    timeout 10s ./$program || true
    
    # Perf record - para análisis detallado
    echo "   Grabando datos con perf record..."
    perf record -g -F 99 -o "$OUTPUT_DIR/${output_name}_perf.data" \
    timeout 5s ./$program 2>/dev/null || true
    
    # Generar reporte
    perf report -i "$OUTPUT_DIR/${output_name}_perf.data" --stdio \
    > "$OUTPUT_DIR/${output_name}_perf_report.txt" 2>/dev/null || true
    
    echo "   ✅ Completado: ${output_name}_perf_*"
}

# Función para ejecutar valgrind
run_valgrind() {
    local program=$1
    local output_name=$2
    
    echo "🔍 Ejecutando valgrind para: $program"
    
    # Callgrind - análisis de caché
    valgrind --tool=callgrind --callgrind-out-file="$OUTPUT_DIR/${output_name}_callgrind.out" \
    --simulate-cache=yes --cache-simulate=yes \
    timeout 30s ./$program 2>/dev/null || true
    
    # Massif - análisis de heap
    valgrind --tool=massif --massif-out-file="$OUTPUT_DIR/${output_name}_massif.out" \
    timeout 30s ./$program 2>/dev/null || true
    
    # Memcheck - detección de memory leaks
    valgrind --tool=memcheck --leak-check=full --show-leak-kinds=all \
    --log-file="$OUTPUT_DIR/${output_name}_memcheck.txt" \
    timeout 30s ./$program 2>&1 | head -100 > "$OUTPUT_DIR/${output_name}_memcheck_summary.txt" || true
    
    echo "   ✅ Completado: ${output_name}_valgrind_*"
}

# Compilar versión standard
compile_standard() {
    echo ""
    echo "🔨 Compilando versión STANDARD..."
    
    cd "$WORKSPACE_DIR"
    
    gcc -O2 -g -march=native -lrt -lpthread \
        -I./include -I./oshw/linux -I./osal/linux \
        -o "$OUTPUT_DIR/ec_sample_standard" \
        samples/ec_sample/ec_sample.c \
        src/*.c oshw/linux/*.c osal/linux/*.c \
        2>&1 | tail -5
    
    echo "   ✅ ec_sample_standard compilado"
}

# Compilar versión optimizada
compile_optimized() {
    echo ""
    echo "🔨 Compilando versión OPTIMIZED..."
    
    cd "$WORKSPACE_DIR"
    
    gcc -O3 -march=native -flto -funroll-loops -ffast-math \
        -DUSE_EC_MEMOPT -g \
        -I./include -I./oshw/linux -I./osal/linux -I./optimized/include \
        -o "$OUTPUT_DIR/ec_sample_optimized" \
        samples/ec_sample/ec_sample.c \
        src/*.c oshw/linux/*.c osal/linux/*.c \
        optimized/src/ec_nicdrv_opt.c \
        2>&1 | tail -5
    
    echo "   ✅ ec_sample_optimized compilado"
}

# Generar gráficas con flamegraph
generate_flamegraph() {
    local perf_data=$1
    local output_svg=$2
    
    echo "   🔥 Generando flamegraph..."
    
    # Verificar si está disponible FlameGraph
    if command -v flamegraph.pl &> /dev/null; then
        perf script -i "$perf_data" | flamegraph.pl > "$output_svg"
        echo "   ✅ Flamegraph generado: $output_svg"
    else
        echo "   ⚠️  flamegraph.pl no encontrado, saltando..."
    fi
}

# Main
main() {
    echo ""
    echo "=============================================="
    echo "Paso 1: Compilación"
    echo "=============================================="
    
    compile_standard
    compile_optimized
    
    echo ""
    echo "=============================================="
    echo "Paso 2: Profiling con PERF"
    echo "=============================================="
    
    run_perf "$OUTPUT_DIR/ec_sample_standard" "standard"
    run_perf "$OUTPUT_DIR/ec_sample_optimized" "optimized"
    
    echo ""
    echo "=============================================="
    echo "Paso 3: Análisis con VALGRIND"
    echo "=============================================="
    
    run_valgrind "$OUTPUT_DIR/ec_sample_standard" "standard"
    run_valgrind "$OUTPUT_DIR/ec_sample_optimized" "optimized"
    
    echo ""
    echo "=============================================="
    echo "Resumen de archivos generados"
    echo "=============================================="
    
    ls -lh "$OUTPUT_DIR"/*.{txt,out,data} 2>/dev/null | awk '{print "   " $9 " (" $5 ")"}'
    
    echo ""
    echo "=============================================="
    echo "✅ Profiling completado!"
    echo "=============================================="
    echo ""
    echo "Archivos de interés:"
    echo "  - ${output_name}_perf_stat.txt     : Métricas de CPU y caché"
    echo "  - ${output_name}_perf_report.txt   : Reporte detallado de funciones"
    echo "  - ${output_name}_callgrind.out     : Análisis de caché (usar kcachegrind)"
    echo "  - ${output_name}_massif.out        : Uso de memoria (usar ms_print)"
    echo "  - ${output_name}_memcheck.txt      : Detección de memory leaks"
    echo ""
    echo "Para visualizar callgrind:"
    echo "  kcachegrind $OUTPUT_DIR/standard_callgrind.out"
    echo ""
    echo "Para ver uso de memoria:"
    echo "  ms_print $OUTPUT_DIR/standard_massif.out"
    echo ""
}

# Ejecutar main
main "$@"
