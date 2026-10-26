#!/usr/bin/env python3
"""
gen_traces.py - Synthetic Memory Trace Generator
Generates realistic memory access streams with temporal/spatial locality,
configurable working-set sizes, and read/write ratios for cache co-simulation.
"""

import argparse
import random
import sys

def generate_traces(num_accesses, working_set_kb, locality_prob, read_ratio, output_file):
    max_addr = working_set_kb * 1024
    hot_set_size = max(64, max_addr // 16)
    
    addresses = []
    print(f"[*] Generating {num_accesses} memory accesses...")
    print(f"    Working set: {working_set_kb} KB | Locality: {locality_prob:.2f} | Read ratio: {read_ratio:.2f}")

    with open(output_file, 'w') as f:
        f.write(f"# Synthetic Memory Trace: {num_accesses} accesses, {working_set_kb}KB working set\n")
        f.write(f"# Temporal/Spatial Locality: {locality_prob}, Read/Write Ratio: {read_ratio}\n")

        recent_addrs = [random.randint(0, hot_set_size) & ~0x3 for _ in range(16)]

        for i in range(num_accesses):
            is_read = random.random() < read_ratio
            op = 'R' if is_read else 'W'

            if random.random() < locality_prob and recent_addrs:
                base = random.choice(recent_addrs)
                offset = random.choice([-64, -32, 0, 4, 8, 16, 32, 64])
                addr = max(0, min(max_addr - 4, base + offset)) & ~0x3
            else:
                addr = random.randint(0, max_addr - 4) & ~0x3

            recent_addrs.pop(0)
            recent_addrs.append(addr)

            if is_read:
                f.write(f"R 0x{addr:08X}\n")
            else:
                val = random.randint(0, 0xFFFFFFFF)
                f.write(f"W 0x{addr:08X} 0x{val:08X}\n")

    print(f"[+] Successfully wrote {num_accesses} trace entries to {output_file}")

def main():
    parser = argparse.ArgumentParser(description="Synthetic Memory Trace Generator")
    parser.add_argument("--num", type=int, default=10000, help="Total number of memory accesses")
    parser.add_argument("--working-set", type=int, default=64, help="Working set size in KB")
    parser.add_argument("--locality", type=float, default=0.75, help="Probability of referencing recent address")
    parser.add_argument("--read-ratio", type=float, default=0.70, help="Read operation ratio (0.0 to 1.0)")
    parser.add_argument("--output", type=str, default="traces/synthetic_10k.trace", help="Output trace filename")

    args = parser.parse_args()
    generate_traces(args.num, args.working_set, args.locality, args.read_ratio, args.output)

if __name__ == "__main__":
    main()
