# Escape Hatches, Shell Equivalents & Experiments 🔬

This guide connects the C syscalls in `cbox` to everyday Linux commands, explains common container escape vectors, and provides hands-on experiments.

---

## 1. Shell Equivalents for Every Kernel Primitive

| What `cbox` does in C | Equivalent Linux CLI Command |
| :--- | :--- |
| `clone(CLONE_NEWPID \| CLONE_NEWUTS \| CLONE_NEWNS \| CLONE_NEWUSER)` | `unshare --fork --pid --uts --mount --user --map-root-user --mount-proc /bin/sh` |
| `setns(fd, CLONE_NEWNET)` | `nsenter --target <pid> --net /bin/sh` |
| `memory.max` & `pids.max` in `/sys/fs/cgroup` | `systemd-run --user --scope -p MemoryMax=64M -p TasksMax=20 /bin/sh` |
| `setup_loopback_interface()` (`SIOCSIFFLAGS`) | `ip link set lo up` |

---

## 2. "Prove It" Experiments

### Experiment 1: Compare Namespace Inodes (Host vs Container)
Run a container and inspect `/proc/$$/ns` in another terminal:
```bash
# Terminal 1 (container)
./cbox run -- /bin/sh -c 'ls -l /proc/self/ns; sleep 10'

# Terminal 2 (host)
ls -l /proc/self/ns
```
Notice that `pid:[...]`, `uts:[...]`, `ipc:[...]`, `net:[...]`, and `user:[...]` have different inode numbers!

### Experiment 2: Test the PID Limit (`-p`) Against a Fork Bomb
```bash
# With root / delegated cgroup v2:
sudo ./cbox run -V -p 10 -- /bin/sh -c 'for i in $(seq 1 20); do sleep 2 & done; wait'
```
Once 10 processes exist inside the cgroup, `fork()` fails with `Resource temporarily unavailable` (`EAGAIN`), protecting the host.

### Experiment 3: Test the Memory Limit (`-m`)
```bash
sudo ./cbox run -V -m 32M -- /bin/sh -c 'head -c 64M /dev/zero | tail'
```
The kernel OOM killer terminates the container process (`Killed`, exit code 137) once RSS + page cache exceeds 32MB.

### Experiment 4: Verify `PR_SET_NO_NEW_PRIVS`
```bash
./cbox run --no-new-privs -- /bin/sh -c 'grep NoNewPrivs /proc/self/status'
# Output: NoNewPrivs:	1
```
Even if a `setuid-root` binary existed inside the rootfs, `execve` will not grant elevated privileges.

---

## 3. Escape Hatches & Security Lessons

1. **Why `chroot` alone is NOT a container**:
   - `chroot()` only changes the `/` path resolution for the process; it does not isolate PIDs, mounts, network, or IPC.
   - A root process inside `chroot` can `mkdir` a subdirectory, `chroot` into it without `chdir`, and walk `../../../` out to the real host root.
   - `pivot_root` + `umount2("/.old_root", MNT_DETACH)` actually detaches the host root filesystem from the container's mount namespace.

2. **Why `MS_PRIVATE` is mandatory before mounting**:
   - Systemd marks `/` as `MS_SHARED` by default. Without `mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL)`, mounting `/proc` or `-v` inside the container propagates back onto the host mount table.

3. **Why `-v` without `-r` is dangerous**:
   - If you bind-mount `-v /host/dir:/app/data` without switching rootfs (`-r`), `mkdir("/app/data")` mutates the host's real `/`! `cbox` explicitly refuses `-v` unless `-r <rootfs>` is provided.

---

## 4. Exercises for Readers

1. **Exercise 1 (Easy)**: Add a `--hostname-from-id` flag that defaults the hostname to the first 12 characters of the OCI container ID.
2. **Exercise 2 (Medium)**: Add OverlayFS support (`--overlay lower:upper:work`) using `mount("overlay", target, "overlay", 0, opts)` in `src/filesystem.c`.
3. **Exercise 3 (Advanced)**: Implement `--console-socket` PTY allocation using `posix_openpt()`, `grantpt()`, `unlockpt()`, and `SCM_RIGHTS` ancillary socket passing.
