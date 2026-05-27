import h5py
import numpy as np
import argparse
import os
import sys
import glob
from collections import defaultdict, Counter
import re

class CacheMissAnalyzer:
    def __init__(self):
        self.cache_hierarchy = {
            'l1i': 'l1i',  # L1 instruction cache
            'l1d': 'l1d',  # L1 data cache  
            'l2': 'l2',    # L2 cache
            'l3': 'l3'     # L3 cache (shared)
        }
        
    def parse_cache_info(self, filename):
        """Parse cache type and instance number from filename."""
        # Extract cache type and instance from filename like cache_miss_records_l1i-0.h5
        match = re.search(r'cache_miss_records_([a-z0-9]+)-(\d+)\.h5', filename)
        if match:
            cache_type = match.group(1)
            instance = int(match.group(2))
            return cache_type, instance
        return None, None
    
    def analyze_cache_file(self, filepath):
        """Analyze a single cache miss records file."""
        filename = os.path.basename(filepath)
        cache_type, instance = self.parse_cache_info(filename)
        
        if cache_type is None:
            print(f"Warning: Could not parse cache info from {filename}")
            return None
            
        print(f"Analyzing {filename} ({cache_type}-{instance})...")
        
        try:
            with h5py.File(filepath, 'r') as f:
                # Get cache group (should be the first and only group)
                cache_groups = list(f.keys())
                if not cache_groups:
                    print(f"No cache groups found in {filename}")
                    return None
                
                cache_name = cache_groups[0]
                cache_group = f[cache_name]
                
                # Get metadata
                total_records = cache_group['total_records'][0]
                unique_pcs = cache_group['unique_pcs'][0]
                
                # Collect PC miss statistics with detailed information
                pc_miss_stats = {}
                
                # Iterate through all PCs
                for pc_group_name in cache_group.keys():
                    if pc_group_name.startswith('PC_0x'):
                        pc_group = cache_group[pc_group_name]
                        pc_value = int(pc_group_name[5:], 16)
                        
                        pc_total_misses = 0
                        miss_type_counts = {'mGETS': 0, 'mGETXIM': 0, 'mGETXSM': 0}
                        all_cycles = []
                        all_addresses = []
                        
                        # Check each miss type
                        for miss_type in ['mGETS', 'mGETXIM', 'mGETXSM']:
                            if miss_type in pc_group:
                                type_group = pc_group[miss_type]
                                cycles = type_group['cycle'][:]
                                addresses = type_group['address'][:]
                                count = len(cycles)
                                
                                miss_type_counts[miss_type] = count
                                pc_total_misses += count
                                all_cycles.extend(cycles)
                                all_addresses.extend(addresses)
                        
                        if pc_total_misses > 0:
                            pc_miss_stats[pc_value] = {
                                'total_misses': pc_total_misses,
                                'miss_types': miss_type_counts,
                                'cache_type': cache_type,
                                'instance': instance,
                                'cycles': all_cycles,
                                'addresses': all_addresses,
                                'cycle_range': (min(all_cycles), max(all_cycles)) if all_cycles else (0, 0),
                                'address_range': (min(all_addresses), max(all_addresses)) if all_addresses else (0, 0)
                            }
                
                return {
                    'filename': filename,
                    'cache_type': cache_type,
                    'instance': instance,
                    'total_records': total_records,
                    'unique_pcs': unique_pcs,
                    'pc_miss_stats': pc_miss_stats
                }
                
        except Exception as e:
            print(f"Error analyzing {filename}: {e}")
            return None

def main():
    parser = argparse.ArgumentParser(description='Pc\'s addresses from cache miss records analysis from ZSim')
    parser.add_argument('paths', nargs='*', help='Directories to analyze (default: current directory)', default='./runs.20250828_140710/CarriBot_tartan_0/cache_miss_records_l1d-0.h5')
    parser.add_argument('--PCs', type=int, default=0x0, help='PCs to show (default: 0x0)')
    # parser.add_argument('--recursive', action='store_true', help='Recursively search subdirectories for cache files')
    # parser.add_argument('--by-cache', action='store_true', help='Show top PCs by cache instance (default: True)')
    # parser.add_argument('--by-total', action='store_true', help='Show top PCs by total miss count across all caches')
    
    args = parser.parse_args()
    args.PCs = 0x4072f8
    
    analyzer = CacheMissAnalyzer()
    results = []
    results = analyzer.analyze_cache_file(args.paths)
    num_unique = len(set(results['pc_miss_stats'][args.PCs]['addresses']))
    print("PC: ", hex(args.PCs))
    print("Total misses: ", results['pc_miss_stats'][args.PCs]['total_misses'])
    print("Number of unique addresses: ", num_unique)

    # print the gap between the addresses
    addresses = results['pc_miss_stats'][args.PCs]['addresses']
    gaps = []
    for i in range(len(addresses) - 1):
        gaps.append(addresses[i+1] - addresses[i])
    print("Gaps distribution: ")
    print("Number of unique gaps: ", len(set(gaps)))
    print("unique Gaps: ", set(gaps))


if __name__ == '__main__':
    main() 