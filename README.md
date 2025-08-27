# enelog

`enelog` is a lightweight command-line utility for measuring CPU power consumption using Intel's RAPL (Running Average Power Limit) interface. It is implemented in C and built with CMake.

## Features

- CPU package power & energy via Intel RAPL
- Optional DRAM domain power & energy (`-d`)
- Optional GPU total and per-GPU power & energy via NVIDIA NVML (`-g`, `-G`)
- Interval-aligned sampling using absolute sleeps (reduces drift)
- Minimal single-file C codebase
- CMake-based build

## Requirements

- **OS:** Linux with `/sys/class/powercap` (RAPL) support. You may need to have root privileges.
- **CPU:** Intel processor with RAPL package—and optionally DRAM—domains  
- **GPU (optional):** NVIDIA driver + NVML (`libnvidia-ml.so`) if using `-g`/`-G`  
- **Permissions:** Reading `energy_uj` may require `sudo` on some systems

## Build Instructions

Make sure you have CMake (version 3.22 or higher) and a C compiler installed.

```bash
git clone https://github.com/your-username/enelog.git
cd enelog
cmake -B build
cmake --build build
```

After building, the executable will be available at `build/enelog`.

## Building with NVML 

If CMake doesn't automatically find NVML, provide include/lib paths:

```bash
cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS="-I/usr/local/cuda/include" \
  -DCMAKE_EXE_LINKER_FLAGS="-L/usr/lib/x86_64-linux-gnu -lnvidia-ml"
cmake --build build
```

Adjust include/lib paths for your distribution (e.g., `/usr/lib`, `/lib/x86_64-linux-gnu`, `/usr/local/cuda/lib64`).

## Building without NVML (CPU-only)

Disable GPU features at compile time:

```bash
cmake -B build -DNO_NVML=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Usage

```bash
./enelog [-i <interval>] [-t <timeout>] [-d] [-D] [-E] [-H] [-h] [-g] [-G]
```
- `-i <interval>`: Interval in seconds between power measurements (default: 1 second)
- `-t <timeout>`: Timeout in seconds after which the program exits (default: 120 seconds)
- `-d`: Enable DRAM power/energy (if DRAM RAPL domain exists)
- `-D`: Prepend MM-DD to the timestamp (otherwise HH:MM:SS)
- `-E`: Include energy(J) fields in output (otherwise power only)
- `-H`: Print a one-line header describing the output columns
- `-h`: Show help and exit

GPU (requires NVML and a build without NO_NVML):
- `-g`: Enable GPU power/energy measurement via NVML
- `-G`: Also print per-GPU metrics(implies -g)

## Output Schema 

**Timestamp**
- Default: `HH:MM:SS`
- With `-D`: `MM-DD HH:MM:SS`

**Columns (by flags)**

- **Base (CPU only, no `-E`):**
    ```text
    [time] CPU(W)
    ```

- **CPU with energy (`-E`):**
    ```text
    [time] CPU(W) CPU(J)
    ```

- **CPU + DRAM (`-d`, no `-E`):**
    ```text
    [time] CPU(W) DRAM(W)
    ```

- **CPU + DRAM with energy (`-d -E`):**
    ```text
    [time] CPU(W) CPU(J) DRAM(W) DRAM(J)
    ```

- **Add GPU total (`-g`):**
    ```text
    [time] ... GPU(W) [GPU(J) if -E]
    ```

- **Add per-GPU (`-G`):**
    ```text
    [time] ... GPU(W) [GPU(J)] GPU00(W) [GPU00(J)] GPU01(W) [GPU01(J)] ...
    ```

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.

## Notes

- **Platform support**
  - Requires Linux with RAPL exposed under `/sys/class/powercap`.
  - Works on **Intel** CPUs that support RAPL. AMD/others are not supported.
  - Only **package 0** (`intel-rapl:0`) is read; multi-socket systems beyond pkg0 are not measured.
  - The DRAM RAPL domain may not be present on all platforms.

- **Permissions**
  - Reading `energy_uj` may require elevated privileges. Use `sudo` or adjust permissions via a udev rule (distro-specific).

- **DRAM flag (`-d`)**
  - If your system lacks a DRAM RAPL domain, using `-d` will fail. Omit `-d` in that case.

- **GPU measurement (`-g`, `-G`)**
  - Requires NVIDIA driver + NVML (`libnvidia-ml.so`) and a build **without** `-DNO_NVML=ON`.
  - `-G` includes per-GPU metrics and implies `-g`.
  - If NVML isn’t available or no NVIDIA GPUs are present, GPU measurement will not work.

- **Timing & alignment**
  - Sampling is aligned to wall-clock boundaries (e.g., 1 s at `...:00`, `...:01`, …) to reduce drift.
  - The very first wait can be shorter than the nominal interval due to alignment.

- **Units & fields**
  - Power is in **watts (W)** and energy in **joules (J)**.
  - Use `-E` to include energy columns; `-H` prints a header row; `-D` adds `MM-DD` to the timestamp.

- **Accuracy considerations**
  - CPU energy uses RAPL counters (platform-estimated). GPU energy is trapezoid-integrated from instantaneous NVML power.
  - Shorter intervals generally improve integration accuracy but increase overhead.

- **Containers/VMs**
  - Some containers or virtual machines do not expose `/sys/class/powercap` or NVML; measurements may be unavailable there.

- **More info**
  - See the Linux powercap documentation: https://www.kernel.org/doc/Documentation/power/powercap/powercap.txt
