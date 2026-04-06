#!/usr/bin/env python3
"""
SOEM Benchmark Suite - Suite completa de benchmarks para evaluar optimizaciones

Mide:
- Latencia de ciclo completo (TX + RX)
- Jitter máximo y promedio
- Throughput de paquetes EtherCAT
- Tiempo de respuesta en fallos de red
- Comparativa antes/después de optimizaciones

Cumplimiento norma IEC 61158:
- Jitter máximo permitido: 1µs para ciclo < 1ms
- Latencia máxima: 10ms para aplicaciones críticas
"""

import subprocess
import time
import statistics
import json
import os
import sys
from datetime import datetime
from typing import Dict, List, Tuple
import argparse

# Configuración
DEFAULT_CYCLE_COUNT = 10000
DEFAULT_TIMEOUT_US = 1000
IEC61158_MAX_JITTER_US = 1.0  # 1 microsegundo para ciclos < 1ms
IEC61158_MAX_LATENCY_MS = 10.0  # 10 milisegundos

class SOEMBenchmark:
    def __init__(self, interface: str = "eth0", optimized: bool = False):
        self.interface = interface
        self.optimized = optimized
        self.results = {
            'timestamp': datetime.now().isoformat(),
            'interface': interface,
            'optimized': optimized,
            'cycle_latency_us': [],
            'jitter_us': [],
            'throughput_pps': [],
            'error_recovery_us': [],
        }
        
    def run_sample_app(self, cycle_count: int = DEFAULT_CYCLE_COUNT) -> bool:
        """Ejecutar aplicación de sample modificada para benchmarking"""
        try:
            cmd = [
                './ec_benchmark',
                '-i', self.interface,
                '-c', str(cycle_count),
                '-o' if self.optimized else '',
            ]
            cmd = [c for c in cmd if c]  # Remover strings vacíos
            
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
            
            if result.returncode == 0:
                # Parsear output
                self._parse_benchmark_output(result.stdout)
                return True
            else:
                print(f"Error ejecutando benchmark: {result.stderr}")
                return False
                
        except subprocess.TimeoutExpired:
            print("Timeout ejecutando benchmark")
            return False
        except FileNotFoundError:
            print("ec_benchmark no encontrado, compilando...")
            self._compile_benchmark()
            return self.run_sample_app(cycle_count)
    
    def _parse_benchmark_output(self, output: str):
        """Parsear output del benchmark para extraer métricas"""
        lines = output.split('\n')
        
        for line in lines:
            if 'CYCLE_LATENCY:' in line:
                value = float(line.split(':')[1].strip())
                self.results['cycle_latency_us'].append(value)
            elif 'JITTER:' in line:
                value = float(line.split(':')[1].strip())
                self.results['jitter_us'].append(value)
            elif 'THROUGHPUT:' in line:
                value = float(line.split(':')[1].strip())
                self.results['throughput_pps'].append(value)
            elif 'ERROR_RECOVERY:' in line:
                value = float(line.split(':')[1].strip())
                self.results['error_recovery_us'].append(value)
    
    def _compile_benchmark(self):
        """Compilar programa de benchmark"""
        compile_cmd = [
            'gcc', '-O3', '-march=native', '-lrt', '-lpthread',
            '-I./include', '-I./oshw/linux', '-I./osal/linux',
            '-o', 'ec_benchmark',
            'benchmarks/ec_benchmark.c',
            'src/*.c', 'oshw/linux/*.c', 'osal/linux/*.c'
        ]
        
        if self.optimized:
            compile_cmd.insert(2, '-DUSE_EC_MEMOPT')
            compile_cmd.extend(['optimized/src/ec_nicdrv_opt.c'])
        
        try:
            subprocess.run(compile_cmd, check=True)
        except subprocess.CalledProcessError as e:
            print(f"Error compilando: {e}")
            raise
    
    def calculate_statistics(self) -> Dict:
        """Calcular estadísticas completas"""
        stats = {}
        
        # Latencia de ciclo
        if self.results['cycle_latency_us']:
            latencies = self.results['cycle_latency_us']
            stats['cycle_latency'] = {
                'min_us': min(latencies),
                'max_us': max(latencies),
                'avg_us': statistics.mean(latencies),
                'std_us': statistics.stdev(latencies) if len(latencies) > 1 else 0,
                'p50_us': statistics.median(latencies),
                'p95_us': sorted(latencies)[int(len(latencies) * 0.95)] if len(latencies) >= 20 else max(latencies),
                'p99_us': sorted(latencies)[int(len(latencies) * 0.99)] if len(latencies) >= 100 else max(latencies),
            }
        
        # Jitter
        if self.results['jitter_us']:
            jitters = self.results['jitter_us']
            stats['jitter'] = {
                'min_us': min(jitters),
                'max_us': max(jitters),
                'avg_us': statistics.mean(jitters),
                'std_us': statistics.stdev(jitters) if len(jitters) > 1 else 0,
                'iec61158_compliant': max(jitters) <= IEC61158_MAX_JITTER_US,
            }
        
        # Throughput
        if self.results['throughput_pps']:
            throughputs = self.results['throughput_pps']
            stats['throughput'] = {
                'min_pps': min(throughputs),
                'max_pps': max(throughputs),
                'avg_pps': statistics.mean(throughputs),
            }
        
        # Error recovery
        if self.results['error_recovery_us']:
            recoveries = self.results['error_recovery_us']
            stats['error_recovery'] = {
                'min_us': min(recoveries),
                'max_us': max(recoveries),
                'avg_us': statistics.mean(recoveries),
            }
        
        return stats
    
    def run_full_benchmark(self, cycle_count: int = DEFAULT_CYCLE_COUNT) -> Dict:
        """Ejecutar benchmark completo y retornar resultados"""
        print(f"\n{'='*60}")
        print(f"SOEM Benchmark - {'Optimized' if self.optimized else 'Standard'}")
        print(f"Interface: {self.interface}, Cycles: {cycle_count}")
        print(f"{'='*60}\n")
        
        success = self.run_sample_app(cycle_count)
        
        if not success:
            print("Benchmark falló")
            return None
        
        stats = self.calculate_statistics()
        self.results['statistics'] = stats
        
        # Imprimir resultados
        self._print_results(stats)
        
        return self.results
    
    def _print_results(self, stats: Dict):
        """Imprimir resultados formateados"""
        print("\n" + "="*60)
        print("RESULTADOS DEL BENCHMARK")
        print("="*60)
        
        if 'cycle_latency' in stats:
            cl = stats['cycle_latency']
            print(f"\n📊 Latencia de Ciclo:")
            print(f"   Mínimo:  {cl['min_us']:8.2f} µs")
            print(f"   Máximo:  {cl['max_us']:8.2f} µs")
            print(f"   Promedio:{cl['avg_us']:8.2f} µs ± {cl['std_us']:.2f} µs")
            print(f"   P50:     {cl['p50_us']:8.2f} µs")
            print(f"   P95:     {cl['p95_us']:8.2f} µs")
            print(f"   P99:     {cl['p99_us']:8.2f} µs")
        
        if 'jitter' in stats:
            jt = stats['jitter']
            compliant = "✅ YES" if jt['iec61158_compliant'] else "❌ NO"
            print(f"\n⏱️  Jitter:")
            print(f"   Mínimo:  {jt['min_us']:8.2f} µs")
            print(f"   Máximo:  {jt['max_us']:8.2f} µs")
            print(f"   Promedio:{jt['avg_us']:8.2f} µs")
            print(f"   IEC 61158 Compliant: {compliant} (<={IEC61158_MAX_JITTER_US} µs)")
        
        if 'throughput' in stats:
            tp = stats['throughput']
            print(f"\n📈 Throughput:")
            print(f"   Mínimo:  {tp['min_pps']:8.0f} pps")
            print(f"   Máximo:  {tp['max_pps']:8.0f} pps")
            print(f"   Promedio:{tp['avg_pps']:8.0f} pps")
        
        if 'error_recovery' in stats:
            er = stats['error_recovery']
            print(f"\n🔧 Error Recovery:")
            print(f"   Mínimo:  {er['min_us']:8.2f} µs")
            print(f"   Máximo:  {er['max_us']:8.2f} µs")
            print(f"   Promedio:{er['avg_us']:8.2f} µs")
        
        print("\n" + "="*60)


def compare_results(standard: Dict, optimized: Dict) -> Dict:
    """Comparar resultados standard vs optimized"""
    if not standard or not optimized:
        return None
    
    comparison = {
        'timestamp': datetime.now().isoformat(),
        'improvements': {}
    }
    
    std_stats = standard.get('statistics', {})
    opt_stats = optimized.get('statistics', {})
    
    # Comparar latencia de ciclo
    if 'cycle_latency' in std_stats and 'cycle_latency' in opt_stats:
        std_avg = std_stats['cycle_latency']['avg_us']
        opt_avg = opt_stats['cycle_latency']['avg_us']
        improvement = ((std_avg - opt_avg) / std_avg) * 100 if std_avg > 0 else 0
        
        std_max = std_stats['cycle_latency']['max_us']
        opt_max = opt_stats['cycle_latency']['max_us']
        max_improvement = ((std_max - opt_max) / std_max) * 100 if std_max > 0 else 0
        
        comparison['improvements']['cycle_latency_avg'] = {
            'standard_us': std_avg,
            'optimized_us': opt_avg,
            'improvement_pct': improvement,
        }
        comparison['improvements']['cycle_latency_max'] = {
            'standard_us': std_max,
            'optimized_us': opt_max,
            'improvement_pct': max_improvement,
        }
    
    # Comparar jitter
    if 'jitter' in std_stats and 'jitter' in opt_stats:
        std_jitter = std_stats['jitter']['max_us']
        opt_jitter = opt_stats['jitter']['max_us']
        jitter_improvement = ((std_jitter - opt_jitter) / std_jitter) * 100 if std_jitter > 0 else 0
        
        comparison['improvements']['jitter_max'] = {
            'standard_us': std_jitter,
            'optimized_us': opt_jitter,
            'improvement_pct': jitter_improvement,
        }
    
    # Comparar throughput
    if 'throughput' in std_stats and 'throughput' in opt_stats:
        std_tp = std_stats['throughput']['avg_pps']
        opt_tp = opt_stats['throughput']['avg_pps']
        tp_improvement = ((opt_tp - std_tp) / std_tp) * 100 if std_tp > 0 else 0
        
        comparison['improvements']['throughput_avg'] = {
            'standard_pps': std_tp,
            'optimized_pps': opt_tp,
            'improvement_pct': tp_improvement,
        }
    
    return comparison


def print_comparison(comparison: Dict):
    """Imprimir tabla comparativa"""
    if not comparison:
        return
    
    print("\n" + "="*70)
    print("COMPARATIVA: STANDARD vs OPTIMIZED")
    print("="*70)
    print(f"\n{'Métrica':<30} {'Standard':<15} {'Optimized':<15} {'Mejora':<15}")
    print("-"*70)
    
    improvements = comparison.get('improvements', {})
    
    if 'cycle_latency_avg' in improvements:
        cl = improvements['cycle_latency_avg']
        print(f"{'Latencia Ciclo Avg (µs)':<30} {cl['standard_us']:<15.2f} {cl['optimized_us']:<15.2f} {cl['improvement_pct']:+.1f}%")
    
    if 'cycle_latency_max' in improvements:
        cl = improvements['cycle_latency_max']
        print(f"{'Latencia Ciclo Max (µs)':<30} {cl['standard_us']:<15.2f} {cl['optimized_us']:<15.2f} {cl['improvement_pct']:+.1f}%")
    
    if 'jitter_max' in improvements:
        jt = improvements['jitter_max']
        print(f"{'Jitter Max (µs)':<30} {jt['standard_us']:<15.2f} {jt['optimized_us']:<15.2f} {jt['improvement_pct']:+.1f}%")
    
    if 'throughput_avg' in improvements:
        tp = improvements['throughput_avg']
        print(f"{'Throughput Avg (pps)':<30} {tp['standard_pps']:<15.0f} {tp['optimized_pps']:<15.0f} {tp['improvement_pct']:+.1f}%")
    
    print("="*70)


def main():
    parser = argparse.ArgumentParser(description='SOEM Benchmark Suite')
    parser.add_argument('-i', '--interface', default='eth0', help='Interfaz de red')
    parser.add_argument('-c', '--cycles', type=int, default=DEFAULT_CYCLE_COUNT, help='Número de ciclos')
    parser.add_argument('-o', '--optimized-only', action='store_true', help='Solo versión optimizada')
    parser.add_argument('-s', '--standard-only', action='store_true', help='Solo versión standard')
    parser.add_argument('--json', action='store_true', help='Output en JSON')
    parser.add_argument('-o', '--output', help='Archivo de salida JSON')
    
    args = parser.parse_args()
    
    results = {}
    
    # Ejecutar benchmark standard
    if not args.optimized_only:
        bench_std = SOEMBenchmark(args.interface, optimized=False)
        results['standard'] = bench_std.run_full_benchmark(args.cycles)
    
    # Ejecutar benchmark optimizado
    if not args.standard_only:
        bench_opt = SOEMBenchmark(args.interface, optimized=True)
        results['optimized'] = bench_opt.run_full_benchmark(args.cycles)
    
    # Comparar resultados
    if results.get('standard') and results.get('optimized'):
        comparison = compare_results(results['standard'], results['optimized'])
        print_comparison(comparison)
        results['comparison'] = comparison
    
    # Output JSON
    if args.json or args.output:
        json_output = json.dumps(results, indent=2)
        
        if args.output:
            with open(args.output, 'w') as f:
                f.write(json_output)
            print(f"\nResultados guardados en: {args.output}")
        else:
            print("\n" + json_output)
    
    # Retornar código de éxito
    if results.get('optimized') and 'statistics' in results['optimized']:
        jitter_compliant = results['optimized']['statistics'].get('jitter', {}).get('iec61158_compliant', False)
        return 0 if jitter_compliant else 1
    
    return 0


if __name__ == '__main__':
    sys.exit(main())
