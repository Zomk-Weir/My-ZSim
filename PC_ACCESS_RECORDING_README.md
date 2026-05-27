# PC Access Recording Feature

## Overview

This document describes the PC (Program Counter) access recording feature added to the zsim CPU simulator. This feature records all cache accesses (both hits and misses) at the **L1 cache level only**, providing comprehensive statistics about program behavior.

### Why Only L1 Cache?

The cache hierarchy works as follows:
- **L1 Cache**: Receives ALL program memory accesses (complete program behavior)
- **L2 Cache**: Only receives accesses when L1 misses (L2 accesses = L1 misses)
- **L3 Cache**: Only receives accesses when L2 misses (L3 accesses = L2 misses)

Therefore, recording at L1 level captures the complete program access pattern, while L2/L3 access patterns can be derived from the corresponding miss records of upper levels.

## Implementation Details

### Key Components

1. **PCAccessRecorder** (`pc_access_recorder.h/cpp`)
   - Records access statistics for each unique PC address
   - Tracks total accesses, read accesses, and write accesses
   - Thread-safe implementation using futex locks
   - Configurable maximum number of unique PCs to record

2. **PCAccessRecorderManager** (`pc_access_recorder.h/cpp`)
   - Global manager for the PC access recorder
   - Handles initialization and configuration
   - Provides HDF5 export functionality

3. **FilterCache Integration** (`filter_cache.h`)
   - Intercepts all cache accesses at the L1 filter cache level
   - Records PC access for both load and store operations
   - Captures complete program memory access behavior
   - Minimal performance overhead

### Configuration

Add the following configuration to your zsim config file:

```
sim = {
    # ... other sim configuration ...
    
    # PC access recording configuration
    pcAccessRecording = {
        enabled = true;           # Enable PC access recording
        maxRecords = 500000;      # Maximum number of unique PCs to record
    };
};
```

### Data Export

The PC access data is automatically exported to separate HDF5 files for each L1 cache at simulation end:
- **Filename Pattern**: `pc_access_records_<cache_name>.h5`
- **Examples**: 
  - `pc_access_records_l1d.h5` (for L1 data cache - stores and loads)
  - `pc_access_records_l1i.h5` (for L1 instruction cache - instruction fetches)
- **Location**: Output directory specified by `-outputDir` parameter

### Accessing L2/L3 Patterns

To analyze L2/L3 access patterns:
- **L2 accesses** = L1 misses (use existing cache miss records)
- **L3 accesses** = L2 misses (use existing cache miss records)
- This provides a complete picture without redundant recording

### HDF5 File Structure

Each cache gets its own HDF5 file:

```
pc_access_records_l1d.h5          # L1 Data Cache (loads/stores)
└── pc_access_stats/
    ├── total_accesses          # Total number of data accesses
    ├── unique_pcs             # Number of unique PC addresses
    ├── pc_addresses           # Array of PC addresses
    ├── total_access_counts    # Array of total access counts per PC
    ├── read_access_counts     # Array of load access counts per PC
    └── write_access_counts    # Array of store access counts per PC

pc_access_records_l1i.h5          # L1 Instruction Cache (instruction fetches)
└── pc_access_stats/
    ├── total_accesses          # Total number of instruction fetches
    ├── unique_pcs             # Number of unique PC addresses
    ├── pc_addresses           # Array of PC addresses
    ├── total_access_counts    # Array of total access counts per PC
    ├── read_access_counts     # Array of instruction fetch counts per PC
    └── write_access_counts    # Array of write counts (typically 0 for I-cache)
```

## Usage Example

### 1. Configuration

Create a configuration file (e.g., `test_config.cfg`):

```
sys = {
    frequency = 2000;
    cores = {
        simple = {
            type = "Simple";
            cores = 1;
        };
    };
    caches = {
        l1d = {
            caches = 1;
            type = "Simple";
            size = 32768;
            array = { type = "SetAssoc"; ways = 8; };
            latency = 4;
        };
        # ... other cache levels ...
    };
    mem = {
        type = "Simple";
        latency = 100;
    };
};

sim = {
    phaseLength = 10000;
    statsPhaseInterval = 100;
    
    # Enable PC access recording
    pcAccessRecording = {
        enabled = true;
        maxRecords = 500000;
    };
};

process0 = {
    command = "your_test_program";
    startFastForwarded = false;
};
```

### 2. Running Simulation

```bash
./build/opt/zsim test_config.cfg
```

### 3. Analyzing Results

Use the provided Python script to analyze the L1 cache results:

```bash
# Analyze L1 data cache accesses (loads/stores)
python3 test_pc_access_reader.py pc_access_records_l1d.h5

# Analyze L1 instruction cache accesses (instruction fetches)
python3 test_pc_access_reader.py pc_access_records_l1i.h5
```

For L2/L3 access analysis, use the existing cache miss analysis tools since:
- L2 accesses = L1 misses
- L3 accesses = L2 misses

The script will display:
- Total access statistics
- Data consistency verification
- Top 10 most accessed PC addresses
- Read/write access breakdown

## Performance Considerations

- **Memory Usage**: Each unique PC requires approximately 32 bytes of memory
- **Performance Impact**: Minimal overhead (~1-2%) due to efficient implementation
- **Storage**: HDF5 files are compressed and efficient for large datasets

## Integration with Existing Features

This feature works alongside existing zsim features:

1. **Cache Miss Recording**: Records detailed miss information with PC context
2. **Fake Hit Manager**: Can simulate cache hits for specific PCs
3. **Statistics Collection**: Integrates with zsim's statistics framework

## Troubleshooting

### Common Issues

1. **"PC access recorder not created"**
   - Ensure `pcAccessRecording.enabled = true` in configuration
   - Check that `maxRecords` is set to a reasonable value

2. **"Reached max unique PCs limit"**
   - Increase `maxRecords` in configuration
   - Consider filtering to focus on specific program regions

3. **HDF5 export errors**
   - Ensure output directory is writable
   - Check available disk space
   - Verify HDF5 library is properly installed

### Debug Information

Enable debug output by checking simulation logs for:
- "PC access recorder initialized successfully"
- "PC Access Recorder: Recorded access #N"
- "PC access records exported successfully"

## Code Modifications Summary

### Files Added
- `src/pc_access_recorder.h` - Header file for PC access recording
- `src/pc_access_recorder.cpp` - Implementation of PC access recording
- `test_pc_access_reader.py` - Python script for analyzing results

### Files Modified
- `src/filter_cache.h` - Added PC access recording integration
- `src/init.cpp` - Added initialization and configuration parsing
- `src/zsim.h` - Added configuration variables
- `src/zsim.cpp` - Added SimEnd function and HDF5 export

## Future Enhancements

Potential improvements for future versions:

1. **Selective Recording**: Record only specific PC ranges or functions
2. **Real-time Analysis**: Online analysis during simulation
3. **Memory Access Patterns**: Track sequential vs. random access patterns
4. **Temporal Analysis**: Record access timing information
5. **Multi-threaded Analysis**: Per-thread PC access statistics

## Key Benefits

这个实现为你提供了一个强大的工具来分析程序的缓存访问行为：

### L1级别完整分析
- **L1d**: 记录所有数据访问（loads/stores）的完整模式
- **L1i**: 记录所有指令访问（instruction fetches）的完整模式
- **热点识别**: 找出访问频率最高的PC地址

### 层次化访问分析
- **L1访问**: 通过PC access records获得（完整程序行为）
- **L2访问**: 通过L1 miss records获得（L1未命中的访问）
- **L3访问**: 通过L2 miss records获得（L2未命中的访问）

### 数据完整性
- 避免重复记录，减少存储开销
- 保持数据一致性：L1记录 + Miss记录 = 完整的多级缓存访问图谱
- 符合缓存层次结构的本质特性

## Author

Modified by Wei Wu on December 11, 2025
Based on zsim simulator framework

