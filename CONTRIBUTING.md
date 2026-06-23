# Contributing

Thank you for your interest in flow-scheduler. This project ports the
scx_flow per-CPU fair-share scheduler into the kernel's CFS/EEVDF path.
Because the patches modify core scheduler code and must compile across
multiple kernel versions (6.18, 7.0, 7.1), contributions need verification.

## Pull Requests

Pull requests are accepted. Each PR that changes a patch file must include
evidence that the change applies and compiles on all supported kernel
versions listed in the README.

To submit a PR:

1.  Make your changes to the patch files in `patches/stable/`.
2.  For each supported kernel version, apply the patch and compile
    `kernel/sched/fair.o`:
    ```bash
    git clone --depth 1 --branch v<version> https://github.com/torvalds/linux.git
    cd linux
    patch -p1 -N -F 3 < patches/stable/linux-<version>-flow/0001-*.patch
    make olddefconfig
    make kernel/sched/fair.o
    ```
3.  Include the build output or a statement that all versions compiled
    cleanly. For example:
    ```
    Verified on 6.18, 7.0, 7.1: all patches apply with -F 3, CC kernel/sched/fair.o succeeds.
    ```
4.  Open the PR against the `main` branch.

If your change only touches documentation, this verification is not
required — a brief description of the change is sufficient.

## Issues

The [issue tracker](https://github.com/galpt/flow-scheduler/issues) is open
for:

- **Bug reports** — include the kernel version, the running kernel's
- **Kernel version requests** — if you need patches for a kernel version
- **Feature suggestions** — describe the problem you want to solve.
  Features from scx_flow that were intentionally not ported are documented
  in the README; if you believe one should be reconsidered, explain why.

When reporting a bug, include:

```bash
uname -r
cat /proc/cmdline
```

And if the build failed, attach or paste the relevant compiler error
output (the last 20-30 lines of `make` is usually enough).

## Code of Conduct

This project adheres to the [Code of Conduct](CODE_OF_CONDUCT.md). By
participating, you are expected to uphold its terms.
