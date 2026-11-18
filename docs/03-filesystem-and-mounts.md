# Filesystem & Mount Isolation 📁

Filesystem isolation ensures that a container process sees its own isolated root directory, mount table, `/proc`, `/sys`, and `/tmp` without interfering with the host system.

---

## 1. Mount Propagation (`MS_PRIVATE`)

When a process is cloned with `CLONE_NEWNS` (Mount Namespace), its initial mount table is copied from the parent process. 

However, by default on many systemd-based Linux systems, mounts are set to **Shared (`MS_SHARED`)**. If mount propagation remains shared, mounting or unmounting a directory inside the container will affect the host system!

To break this link, `cbox` makes all mounts in the container mount namespace private:

```c
mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
```

- `MS_REC`: Recursive flag (applies to root `/` and all sub-mounts).
- `MS_PRIVATE`: Ensures mount/unmount events inside the container never propagate to the host system.

---

## 2. Root Filesystem Switching: `pivot_root` vs `chroot`

To give the container its own root filesystem (e.g. Alpine Linux, BusyBox, or Ubuntu rootfs), `cbox` supports root switching.

```
       Host System Root                    Container System Root
         +----------+                           +----------+
         | /        |                           | /        |
         | /bin     |   pivot_root / chroot     | /bin     |
         | /usr     | ----------------------->  | /lib     |
         | /home    |                           | /proc    |
         +----------+                           +----------+
```

### `pivot_root` (Recommended & Secure)
`pivot_root(new_root, put_old)` moves the current root mount to `put_old` and makes `new_root` the new root mount point of the calling process.

#### Requirements for `pivot_root`:
1. `new_root` must be a mount point. In `cbox`, we ensure this by bind-mounting `new_root` onto itself:
   ```c
   mount(new_root, new_root, NULL, MS_BIND | MS_REC, NULL);
   ```
2. `put_old` must be a subdirectory under `new_root`.
3. `pivot_root` allows unmounting `put_old` with `umount2("/.old_root", MNT_DETACH)`, removing access to the host filesystem entirely.

### `chroot` Fallback
If `pivot_root` fails (e.g. when running on initramfs or rootfs where `pivot_root` is not permitted by kernel), `cbox` falls back to `chroot(new_root)` followed by `chdir("/")`.

---

## 3. Mounting Pseudo-Filesystems (`/proc`, `/sys`, `/tmp`)

Inside the container, utilities like `ps`, `top`, `free`, and `uptime` read process and kernel state from pseudo-filesystems.

### 1. Mounting `/proc` (Procfs)
Without mounting a fresh `/proc`, running `ps aux` inside the container would show host processes. Mounting a new instance of `procfs` ensures `ps` reads from the container's PID namespace:

```c
ensure_dir_exists("/proc");
umount2("/proc", MNT_DETACH); // Detach old host procfs
mount("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);
```

### 2. Mounting `/sys` (Sysfs)
`/sys` exposes kernel and device status. In `cbox`, `/sys` is mounted **Read-Only (`MS_RDONLY`)** for security:

```c
mount("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NOEXEC | MS_NODEV | MS_RDONLY, NULL);
```

### 3. Mounting `/tmp` (Tmpfs)
`/tmp` is mounted as an in-memory `tmpfs` RAM disk (capped at 64MB):

```c
mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "size=64M");
```

---

## 4. Ubuntu 24.04 AppArmor Restrictions & Unprivileged Mounts

### The Restriction
On **Ubuntu 24.04 LTS**, Canonical enabled `kernel.apparmor_restrict_unprivileged_userns = 1`. 

Under this policy:
- Unprivileged users can create user namespaces (`CLONE_NEWUSER`) and PID namespaces (`CLONE_NEWPID`).
- However, AppArmor blocks unprivileged user namespaces from executing `mount()` or `sethostname()` calls.

### Graceful Fallback Handling in `src/filesystem.c`
Instead of failing or crashing when running as an unprivileged user on Ubuntu 24.04, `cbox` inspects `errno` (`EPERM` / `EACCES`) and logs helpful warnings while proceeding with PID & User Namespace isolation:

```c
if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
    if (errno == EPERM || errno == EACCES) {
        log_warn("Host kernel restricted unprivileged mount namespace (AppArmor / userns restriction)");
    }
}
```

Running with `sudo ./cbox ...` grants full root capabilities on the host and enables all mount operations without warnings.
