# Cache Miss Recording Feature

**Modified by wei wu on 250816**

## Overview

This extended version of ZSim includes a comprehensive cache miss recording system that captures detailed information about cache misses during simulation. The system records PC (Program Counter), cycle, and address information for each cache miss in a hierarchical tree structure.

## Features

- **Hierarchical Recording Structure**: PC serves as the top-level key, with cycle-address pairs stored as records under each PC
- **Per-Cache Recording**: Each cache instance maintains its own separate miss recorder
- **Configurable Limits**: Maximum number of records per cache can be configured to handle large simulations (tested for 20M+ records)
- **HDF5 Export**: All recorded data is exported to separate HDF5 files for each cache at simulation end
- **Thread-Safe**: All recording operations are protected by locks for multi-threaded safety

## Configuration

Add the following section to your ZSim configuration file:

```
sim = {
  // ... other sim configurations ...
  
  // Cache miss recording configuration
  cacheMissRecording = {
    enabled = true;                    // Enable/disable cache miss recording
    maxRecords = 20000000L;           // Maximum records per cache (default: 20M)
  };
};
```

### Configuration Parameters

- `sim.cacheMissRecording.enabled`: Boolean flag to enable/disable recording (default: false)
- `sim.cacheMissRecording.maxRecords`: Maximum number of miss records per cache (default: 20,000,000)

## Recording Points

Cache misses are recorded at three specific points in the MESI coherence protocol:

1. **mGETS**: Read misses (Invalid → Shared/Exclusive)
2. **mGETXIM**: Write misses from Invalid state (Invalid → Modified)  
3. **mGETXSM**: Upgrade misses (Shared → Modified)

## Output Format

### HDF5 File Structure

For each cache, a separate HDF5 file is created: `cache_miss_records_<cache_name>.h5`

```
cache_miss_records_l1d-0.h5
├── l1d-0/                          # Cache name
│   ├── total_records               # Total number of records
│   ├── unique_pcs                  # Number of unique PCs
│   ├── PC_0x400123/                # Program Counter (hex)
│   │   ├── mGETS                   # GETS miss type
│   │   │   ├── cycle: [1234, 5678, ...] 
│   │   │   └── address: [0x1000, 0x2000, ...]
│   │   ├── mGETXIM                 # GETX I->M miss type  
│   │   │   ├── cycle: [...]
│   │   │   └── address: [...]
│   │   └── mGETXSM                 # GETX S->M miss type
│   │       ├── cycle: [...]
│   │       └── address: [...]
│   └── PC_0x400456/
│       └── ...
```

### Example Python Analysis

```python
import h5py
import numpy as np

# Open the HDF5 file
with h5py.File('cache_miss_records_l1d-0.h5', 'r') as f:
    cache_group = f['l1d-0']
    
    # Get metadata
    total_records = cache_group['total_records'][0]
    unique_pcs = cache_group['unique_pcs'][0]
    
    print(f"Total records: {total_records}")
    print(f"Unique PCs: {unique_pcs}")
    
    # Iterate through all PCs
    for pc_group_name in cache_group.keys():
        if pc_group_name.startswith('PC_0x'):
            pc_group = cache_group[pc_group_name]
            pc_value = int(pc_group_name[5:], 16)  # Extract PC from group name
            
            print(f"\nPC 0x{pc_value:x}:")
            
            # Check each miss type
            for miss_type in ['mGETS', 'mGETXIM', 'mGETXSM']:
                if miss_type in pc_group:
                    type_group = pc_group[miss_type]
                    cycles = type_group['cycle'][:]
                    addresses = type_group['address'][:]
                    
                    print(f"  {miss_type}: {len(cycles)} misses")
                    # Analyze miss patterns by type
                    if len(cycles) > 0:
                        print(f"    Cycle range: {min(cycles)} - {max(cycles)}")
                        print(f"    Address range: 0x{min(addresses):x} - 0x{max(addresses):x}")
```

## Implementation Details

### Core Components

1. **CacheMissRecord**: Structure holding cycle-address pairs
2. **PCMissRecords**: Container for all records from a specific PC
3. **CacheMissRecorder**: Per-cache recorder managing PC→records mapping
4. **CacheMissRecorderManager**: Global manager coordinating all cache recorders

### Key Files Modified

- `src/cache_miss_recorder.h/cpp`: Core recording functionality
- `src/cache.h/cpp`: Cache integration and recorder initialization
- `src/coherence_ctrls.h/cpp`: Miss recording hooks in MESI protocol
- `src/zsim.h`: Configuration structure extension
- `src/init.cpp`: Configuration parsing and manager initialization
- `src/zsim.cpp`: HDF5 export during simulation termination

### Performance Considerations

- **Memory Usage**: Each record uses ~16 bytes (8B cycle + 8B address)
- **Lock Contention**: Per-cache locks minimize contention across different caches
- **Early Termination**: Recording stops when maxRecords limit is reached per cache
- **Efficient Storage**: std::unordered_map for fast PC lookup, std::vector for record storage

## Usage Example

1. **Configure**: Add cache miss recording section to your .cfg file
2. **Run**: Execute ZSim normally with the extended configuration
3. **Analyze**: Process the generated HDF5 files with your analysis tools

```bash
# Run simulation
./zsim cache_miss_test.cfg

# Generated files
ls *.h5
# cache_miss_records_l1d-0.h5  cache_miss_records_l1d-1.h5  cache_miss_records_l1i-0.h5  ...
```

## Limitations

- Recording stops when maxRecords limit is reached (no overwrite)
- Only MESI coherence protocol misses are recorded
- HDF5 export happens only at simulation termination
- Memory usage scales with number of unique PCs and total records

## Testing

Use the provided test configuration `tests/cache_miss_test.cfg` to verify functionality with a simple workload.