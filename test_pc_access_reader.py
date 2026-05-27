#!/usr/bin/env python3
"""
Test script to read and verify PC access records from HDF5 file
Created by wei wu on 251211
"""

import h5py
import numpy as np
import sys
import os

def read_pc_access_records(filename):
    """Read PC access records from HDF5 file and display statistics"""
    
    if not os.path.exists(filename):
        print(f"Error: File {filename} does not exist")
        return False
    
    try:
        with h5py.File(filename, 'r') as f:
            print(f"Reading PC access records from: {filename}")
            print("=" * 60)
            
            # Check if the main group exists
            if 'pc_access_stats' not in f:
                print("Error: 'pc_access_stats' group not found in file")
                return False
            
            group = f['pc_access_stats']
            
            # Read metadata
            total_accesses = group['total_accesses'][0]
            unique_pcs = group['unique_pcs'][0]
            
            print(f"Total accesses recorded: {total_accesses:,}")
            print(f"Unique PC addresses: {unique_pcs:,}")
            print()
            
            # Read PC data arrays
            pc_addresses = group['pc_addresses'][:]
            total_access_counts = group['total_access_counts'][:]
            read_access_counts = group['read_access_counts'][:]
            write_access_counts = group['write_access_counts'][:]
            
            # Verify data consistency
            assert len(pc_addresses) == unique_pcs, "PC addresses array size mismatch"
            assert len(total_access_counts) == unique_pcs, "Total access counts array size mismatch"
            assert len(read_access_counts) == unique_pcs, "Read access counts array size mismatch"
            assert len(write_access_counts) == unique_pcs, "Write access counts array size mismatch"
            
            # Verify that read + write = total for each PC
            for i in range(len(pc_addresses)):
                calculated_total = read_access_counts[i] + write_access_counts[i]
                assert calculated_total == total_access_counts[i], f"Access count mismatch for PC 0x{pc_addresses[i]:x}"
            
            # Calculate and display statistics
            total_reads = np.sum(read_access_counts)
            total_writes = np.sum(write_access_counts)
            calculated_total = total_reads + total_writes
            
            print(f"Total read accesses: {total_reads:,}")
            print(f"Total write accesses: {total_writes:,}")
            print(f"Calculated total: {calculated_total:,}")
            print(f"Recorded total: {total_accesses:,}")
            
            assert calculated_total == total_accesses, "Total access count verification failed"
            print("✓ Data consistency check passed")
            print()
            
            # Display top 10 most accessed PCs
            sorted_indices = np.argsort(total_access_counts)[::-1]  # Sort in descending order
            print("Top 10 most accessed PC addresses:")
            print("-" * 60)
            print(f"{'Rank':<4} {'PC Address':<12} {'Total':<10} {'Reads':<10} {'Writes':<10}")
            print("-" * 60)
            
            for i in range(min(10, len(sorted_indices))):
                idx = sorted_indices[i]
                pc = pc_addresses[idx]
                total = total_access_counts[idx]
                reads = read_access_counts[idx]
                writes = write_access_counts[idx]
                print(f"{i+1:<4} 0x{pc:<10x} {total:<10} {reads:<10} {writes:<10}")
            
            print()
            print("PC access record analysis completed successfully!")
            return True
            
    except Exception as e:
        print(f"Error reading file: {e}")
        return False

def main():
    if len(sys.argv) != 2:
        print("Usage: python3 test_pc_access_reader.py <pc_access_records.h5>")
        print("Example: python3 test_pc_access_reader.py pc_access_records.h5")
        sys.exit(1)
    
    filename = sys.argv[1]
    success = read_pc_access_records(filename)
    
    if success:
        print("\n✓ PC access record verification completed successfully!")
        sys.exit(0)
    else:
        print("\n✗ PC access record verification failed!")
        sys.exit(1)

if __name__ == "__main__":
    main()
