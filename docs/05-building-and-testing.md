# Building, Testing & Memory Safety 🧪

This guide covers building `cbox`, running automated tests, validating memory safety with sanitizers, and troubleshooting common execution scenarios.

---

## 1. Build System & Compilation Options

The [`Makefile`](file:///home/elvin/Development/Repositories/elvin-mark/toy-container/Makefile) uses strict C compilation flags to enforce code safety and standard adherence:

```makefile
CC ?= gcc
CFLAGS ?= -Wall -Wextra -Wpedantic -Werror -D_GNU_SOURCE -std=c17 -Iinclude -g
```

### Compiler Flags Explained

| Flag | Description |
| :--- | :--- |
| `-std=c17` | Enforces ISO C17 standard compliance |
| `-D_GNU_SOURCE` | Exposes GNU/Linux extension headers (`clone`, `pivot_root`, `MS_PRIVATE`) |
| `-Wall -Wextra` | Enables comprehensive compiler warnings |
| `-Wpedantic` | Rejects non-standard GNU C language extensions |
| `-Werror` | Treats all compiler warnings as fatal errors |
| `-g` | Includes debug symbols for GDB and sanitizers |

---

## 2. Memory Safety Verification (Sanitizers)

To ensure zero memory leaks, zero double-frees, and zero undefined behavior, build with AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan):

```bash
# Compile binary with sanitizers
make sanitizer

# Run test suite under sanitizers
make test
```

### What AddressSanitizer & UBSan Catch:
1. **Stack & Heap Leaks**: Detects un-freed child process stack memory allocated for `clone()`.
2. **Buffer Overflows**: Catches out-of-bounds reads/writes in string formatting or path buffer operations.
3. **Use-After-Free**: Traps invalid pointer dereferences.

---

## 3. Testing Matrix

Here are key CLI test commands to verify each subsystem:

### Test 1: Basic Process Isolation
```bash
./cbox run -- /bin/echo "Hello from container!"
```

### Test 2: Process ID Isolation (PID 1)
```bash
./cbox run -V -- /bin/bash -c "echo 'PID inside container:' \$\$"
```
*Expected Result*: Output displays `PID inside container: 1`.

### Test 3: Hostname Isolation
```bash
sudo ./cbox run -V -n my-container-host -- /bin/hostname
```
*Expected Result*: Output displays `my-container-host` without changing host system hostname.

### Test 4: Resource Limits (Memory & PIDs)
```bash
sudo ./cbox run -V -m 64M -p 20 -- /bin/bash -c "ps aux"
```

### Test 5: Rootfs Creation & Execution
```bash
# Create minimal directory structure
./cbox create-rootfs ./demo-rootfs

# Inspect structure
ls -la ./demo-rootfs
```

---

## 4. Troubleshooting & FAQ

### Q1: Why do I see `Operation not permitted` or `Permission denied` warnings?
**Answer**: On **Ubuntu 24.04**, Canonical restricted unprivileged user namespace mount and UTS operations (`kernel.apparmor_restrict_unprivileged_userns = 1`). 
- When run as a normal user, PID and User Namespace isolation work, but filesystem mounts and hostname changes produce warnings.
- To allow unprivileged `userns` on Ubuntu 24.04 **without `sudo`**, install the included AppArmor profile ([`apparmor/cbox`](file:///home/elvin/Development/Repositories/elvin-mark/toy-container/apparmor/cbox)):
  ```bash
  sudo cp apparmor/cbox /etc/apparmor.d/cbox
  sudo apparmor_parser -r /etc/apparmor.d/cbox
  ```
- Or run with `sudo`:
  ```bash
  sudo ./cbox run -V -n test-host -- /bin/hostname
  ```

### Q2: How do I test with a full Linux distribution rootfs?
Run `make rootfs` to download and extract an Alpine Linux minirootfs into `./rootfs`:

```bash
make rootfs
sudo ./cbox run -r ./rootfs -V -- /bin/sh
```
