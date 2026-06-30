# Contributing

Thank you for your interest in infinity-scheduler. Because the patches modify
core scheduler code and must compile across multiple kernel versions,
contributions need verification.

## How to contribute

1.  **Open an issue first.** Describe what you want to change — whether it's
    a bug fix, a feature addition, or a new kernel version. This lets us
    discuss the approach and agree on which branch your pull request should
    target before you write any code.

2.  **Make your changes.** Fork the repository and create a branch based on
    the branch agreed upon in the issue discussion.

3.  **Verify compilation on all supported kernel versions.** For each version
    listed in the README, apply the patch and compile the scheduler objects:
    ```bash
    git clone --depth 1 --branch v<version> https://github.com/torvalds/linux.git
    cd linux
    patch -p1 -N -F 3 < patches/stable/linux-<version>-infinity/0001-*.patch
    make olddefconfig
    make kernel/sched/
    ```

4.  **Open a pull request** against the branch agreed upon in the issue
    discussion. Include the build output or a statement that all versions
    compiled cleanly:
    ```
    Verified on all supported versions listed in the README: patches apply
    with -F 3, CC kernel/sched/infinity_sched.o and CC kernel/sched/fair.o
    succeed.
    ```

If your change only touches documentation, the compilation step is not
required — a brief description of the change is sufficient.

## Code of Conduct

This project adheres to the [Code of Conduct](CODE_OF_CONDUCT.md). By
participating, you are expected to uphold its terms.
