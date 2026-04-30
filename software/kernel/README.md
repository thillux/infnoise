# Infinite Noise TRNG Kernel Module

A Linux kernel module for the Infinite Noise TRNG USB device. This driver directly interfaces with the FT240X-based hardware, runs the NIST SP 800-90B Repetition Count and Adaptive Proportion tests on the raw bit stream, conditions it through SHA3-256 (kernel crypto API), and registers as an hwrng device to feed entropy to the kernel's random subsystem.

## Features

- Hardware random number generator (hwrng) interface at `/dev/hwrng`
- Automatic entropy feeding to kernel CRNG (no rngd required on Linux 3.16+)
- Character device interface at `/dev/infnoiseX`
- NIST SP 800-90B continuous health tests (RCT + APT)
- SHA3-256 conditioning with internal state and forward-secret derivation
- Configurable oversampling factor (raw transfers absorbed per output)
- Hot-plug support
- No conflicts with ftdi_sio driver

## Quick Start

### 1. Reprogram Device VID/PID (One-Time Setup)

The kernel module only recognizes devices with the Infinite Noise VID/PID (`1209:3701`). New devices ship with the default FTDI VID/PID (`0403:6015`) which must be reprogrammed.

See [Device Setup](#device-setup) for detailed instructions.

### 2. Build and Load Module

```bash
# Install prerequisites (Debian/Ubuntu)
sudo apt install linux-headers-$(uname -r) build-essential

# Build
cd software/kernel
make

# Load
sudo insmod infnoise.ko

# Verify
dmesg | grep infnoise
```

### 3. Use

```bash
# Read random data
sudo dd if=/dev/hwrng bs=64 count=10 | xxd
```

## Device Setup

### Why Reprogram?

The Infinite Noise TRNG uses an FTDI FT240X chip which ships with FTDI's default VID/PID (`0403:6015`). This causes conflicts with the kernel's `ftdi_sio` serial driver. By reprogramming to the registered Infinite Noise VID/PID (`1209:3701`), the device is uniquely identified and automatically bound to this driver.

### Prerequisites

```bash
# Debian/Ubuntu
sudo apt install libftdi1-dev ftdi-eeprom

# Fedora
sudo dnf install libftdi-devel ftdi-eeprom

# Arch
sudo pacman -S libftdi
```

### Check Current VID/PID

```bash
lsusb | grep -i "ftdi\|1209"
```

If you see `0403:6015`, the device needs reprogramming. If you see `1209:3701`, it's already configured.

### Program the EEPROM

An `infnoise-eeprom.conf` file is included in this repository.

**Warning:** This permanently modifies the device's EEPROM. Double-check the configuration before proceeding.

```bash
# First, unbind from ftdi_sio if necessary
sudo modprobe -r ftdi_sio

# Read current EEPROM (backup)
sudo ftdi_eeprom --read-eeprom --device i:0x0403:0x6015 backup.eeprom

# Flash new configuration
sudo ftdi_eeprom --flash-eeprom --device i:0x0403:0x6015 infnoise-eeprom.conf

# Unplug and replug the device
```

### Verify

```bash
# Should now show 1209:3701
lsusb | grep 1209

# Device should show as "Infinite Noise TRNG"
lsusb -d 1209:3701 -v 2>/dev/null | grep -E "idVendor|idProduct|iProduct|iManufacturer"
```

## Building

### Prerequisites

- Linux kernel headers for your running kernel
- GCC and make
- Root access for loading/testing

```bash
# Debian/Ubuntu
sudo apt install linux-headers-$(uname -r) build-essential

# Fedora/RHEL
sudo dnf install kernel-devel gcc make

# Arch Linux
sudo pacman -S linux-headers base-devel
```

### Compile

```bash
cd software/kernel
make
```

## Installation

### Manual Loading

```bash
# Load the module
sudo insmod infnoise.ko

# Verify it loaded
lsmod | grep infnoise
dmesg | grep infnoise
```

### Automatic Loading at Boot

```bash
# Copy module to kernel modules directory
sudo cp infnoise.ko /lib/modules/$(uname -r)/extra/
sudo depmod -a

# Create modprobe configuration
echo "infnoise" | sudo tee /etc/modules-load.d/infnoise.conf

# Optional: set default parameters
echo "options infnoise oversample=4" | sudo tee /etc/modprobe.d/infnoise.conf
```

### Unloading

```bash
sudo rmmod infnoise
```

## Configuration

### Module Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `debug` | bool | false | Enable verbose debug output |
| `raw_mode` | bool | false | Output raw (unwhitened) data |
| `oversample` | uint | 2 | Raw transfers absorbed per 32-byte SHA3 output (1-16) |

### Setting Parameters at Load Time

```bash
sudo insmod infnoise.ko debug=1 oversample=4
```

### Changing Parameters at Runtime

```bash
# Enable debug output
echo 1 | sudo tee /sys/module/infnoise/parameters/debug

# Set oversampling factor
echo 4 | sudo tee /sys/module/infnoise/parameters/oversample

# Check current values
cat /sys/module/infnoise/parameters/oversample
```

### Oversampling Factor

The `oversample` parameter sets how many full USB transfers of *extracted* entropy are absorbed per 32-byte SHA3-256 output.

Each USB transfer pulls 512 raw bytes from the FT240X. The extractor reads exactly **one** comparator bit per raw byte — COMP1 on odd-phase samples, COMP2 on even-phase — so each transfer yields 512 independent source bits, packed eight-to-a-byte into 64 *extracted* bytes. At the design min-entropy H = 0.88 bits/bit, that's:

```
512 source bits × 0.88 = 450.56 bits of min-entropy per 64 extracted bytes
```

NIST SP 800-90B §3.1.5.1.2 (and SP 800-90C) require the conditioning input to carry at least **n + 64 bits of min-entropy** for an n-bit output to be claimed as full entropy. For SHA3-256 (n = 256) that is **320 bits**, i.e. ⌈320 / (8·0.88)⌉ = 46 extracted bytes minimum. Even `oversample=1` (64 extracted bytes) clears that floor with margin; higher values are pure paranoia headroom and the build statically asserts the lower bound.

| Value | Extracted absorbed | Min-entropy in | Output per refill |
|-------|--------------------|---------------|-------------------|
| 1     | 64 B               | ~450 bits     | 32 B              |
| 2 (default) | 128 B        | ~900 bits     | 32 B              |
| 4     | 256 B              | ~1800 bits    | 32 B              |
| 16    | 1024 B             | ~7200 bits    | 32 B              |

Per refill, the driver computes both the user output and the next internal state from the same input under distinct domain-separator bytes:

```
O  = SHA3-256(S || 0x11 || E)   ← returned to caller
S' = SHA3-256(S || 0x00 || E)   ← replaces internal state
```

Knowledge of `O` does not allow recovery of `S'` (or vice versa) without inverting SHA3-256.

## Usage

### Reading Random Data

```bash
# From hwrng interface
sudo dd if=/dev/hwrng bs=64 count=10 | xxd

# From character device
sudo dd if=/dev/infnoise0 bs=64 count=10 | xxd

# Generate a random file
sudo dd if=/dev/hwrng of=random.bin bs=1M count=10
```

### Checking Device Status

```bash
# View kernel messages
dmesg | grep infnoise

# Check hwrng registration
cat /sys/class/misc/hw_random/rng_available

# Check current hwrng
cat /sys/class/misc/hw_random/rng_current
```

### FIPS 140-2 Testing

```bash
# Install rng-tools if needed
sudo apt install rng-tools  # Debian/Ubuntu

# Run FIPS tests
sudo dd if=/dev/hwrng bs=2500 count=100 | rngtest -c 100
```

### Automatic Entropy Feeding

Since Linux kernel 3.16+, the hwrng subsystem automatically feeds entropy to the kernel's random pool via an internal kernel thread (`hwrng_fillfn`). **No rngd daemon is required.**

The driver dynamically sets the quality parameter based on the output mode:
- **Whitened mode (default)**: quality=1000 (SHA3-256 output is full-entropy when ≥256 bits of entropy are absorbed, which always holds with `oversample` ≥ 1)
- **Raw mode**: quality=880 (reflects the ~0.88 bits/bit true entropy rate)

The hwrng core thread continuously:
1. Reads data from the device
2. Credits entropy based on the quality setting
3. Mixes it into the kernel's CRNG via `add_hwgenerator_randomness()`

To verify automatic feeding is working:
```bash
# Check hwrng kernel thread exists
ps aux | grep hwrng

# Verify the device is active
cat /sys/class/misc/hw_random/rng_current
```

### Using with rngd (Optional, Legacy)

On older kernels (<3.16) or if you prefer manual control, you can use `rngd`:

```bash
# Install rng-tools
sudo apt install rng-tools

# Start rngd (it will auto-detect /dev/hwrng)
sudo rngd -r /dev/hwrng
```

On modern kernels, this is redundant since the hwrng core already handles entropy feeding.

## Troubleshooting

### Device Not Detected

**Check VID/PID:**
```bash
lsusb | grep -i "ftdi\|1209"
```

If you see `0403:6015`, the device needs to be reprogrammed. See [Device Setup](#device-setup).

**Check module is loaded:**
```bash
lsmod | grep infnoise
```

**Check for errors:**
```bash
dmesg | grep -i "infnoise\|1209:3701"
```

### Warmup Taking Too Long

The device requires approximately 450-500 warmup rounds before producing valid data. If warmup fails:

```bash
# Check dmesg for errors
dmesg | grep -i "infnoise.*failed\|infnoise.*error"

# Try reloading the module
sudo rmmod infnoise
sudo insmod infnoise.ko debug=1
```

### Health Check Failures

Occasional health check failures during warmup are normal. Persistent failures after warmup may indicate a hardware issue:

```bash
# Monitor health status
dmesg -w | grep infnoise
```

### Permission Denied

The device files are owned by root by default. To allow non-root access:

```bash
# Create udev rule for permissions
cat << 'EOF' | sudo tee /etc/udev/rules.d/99-infnoise.rules
# Infinite Noise TRNG - allow user access
SUBSYSTEM=="usb", ATTR{idVendor}=="1209", ATTR{idProduct}=="3701", MODE="0666"
KERNEL=="infnoise*", MODE="0666"
KERNEL=="hwrng", MODE="0666"
EOF

sudo udevadm control --reload-rules
sudo udevadm trigger
```

### Reverting EEPROM Changes

If you need to restore the original FTDI VID/PID:

```bash
# Create restore configuration
cat << 'EOF' > ftdi_restore.conf
vendor_id=0x0403
product_id=0x6015
manufacturer="FTDI"
product="FT240X"
EOF

# Flash (may need to specify current VID/PID)
sudo ftdi_eeprom --flash-eeprom --device i:0x1209:0x3701 ftdi_restore.conf
```

## USB Device ID

| VID | PID | Description |
|-----|-----|-------------|
| 0x1209 | 0x3701 | Infinite Noise TRNG (pid.codes registered) |

The VID 0x1209 is provided by [pid.codes](https://pid.codes/), a registry for USB PID/VID for open source projects.

## Technical Details

### Health Monitoring (NIST SP 800-90B 4.4)

Each test carries two thresholds, computed from per-sample design entropy H = 0.88 bits/bit:

| Test | Transient (α = 2⁻³⁰) | Permanent (α = 2⁻⁶⁰) |
|------|----------------------|----------------------|
| **RCT** — consecutive identical samples | 36 | 70 |
| **APT** — matches in 1024-sample window | 656 | 700 |

- **Transient** trips latch `INFNOISE_HEALTH_FAIL`. The current batch is dropped and the existing recovery path (`infnoise_try_recovery`) re-arms RCT/APT and continues.
- **Permanent** trips latch `INFNOISE_HEALTH_DEAD`. The device is condemned: recovery refuses to run, all read paths return `-EIO`, and the only way back is to unbind the driver and replug.
- The two thresholds share counters: a stuck source that crosses the transient line keeps counting bits within the same batch, so it escalates from transient (low evidence) to permanent (overwhelming evidence) inside a single transfer rather than churning recoveries forever.
- Warmup completes after the first clean APT window (~1024 raw bits, two USB transfers).

## License

- Driver code: GPL-2.0-or-later

## See Also

- [Infinite Noise TRNG Project](https://github.com/leetronics/infnoise)
- [Wayward Geek's Original Design](https://github.com/waywardgeek/infnoise)
