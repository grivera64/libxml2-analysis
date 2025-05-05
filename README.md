# libxml2-analysis

Authors:
- Giovanni Rivera (@grivera64)
- Jianan Zhu (@GGAutomaton)

### Description

This is a LLVM analysis of an older bug in [libxml2](https://github.com/GNOME/libxml2)
for UCR's CS 260 course. Specifically, we study a bug found in
[commit 932cc98](https://github.com/GNOME/libxml2/tree/932cc9896ab41475d4aa429c27d9afd175959d74)
which was fixed in
[commit 897dffb](https://github.com/GNOME/libxml2/tree/897dffbae322b46b83f99a607d527058a72c51ed).

Our project uses Kaiming Huang et al.'s [Unified Memory Safety Validation](https://github.com/Lightninghkm/Unified-Memory-Safety-Validation)
analysis framework to detect unsafe heap and stack objects in libxml2's code. Please review
the following papers to learn more about their awesome work:

- K. Huang, Y. Huang, M. Payer, Z. Qian, J. Sampson, G. Tan, and T. Jaeger, "The Taming of the Stack: Isolating Stack Data from Memory Errors," in *Network and Distributed System Security Symposium*, NDSS 2022, 2022, p. 17.
- K. Huang, M. Payer, Z. Qian, J. Sampson, G. Tan, and T. Jaeger, "Top of the Heap: Efficient Memory Error Protection of Safe Heap Objects," in *CCS '24: Proceedings of the 2024 ACM SIGSAC Conference on Computer and Communications Security*, Salt Lake City, UT, USA, 2024, pp. 1330–1344. doi: 10.1145/3658644.3690310.
- K. Huang, M. Payer, Z. Qian, J. Sampson, G. Tan, and T. Jaeger, "Comprehensive Memory Safety Validation: An Alternative Approach to Memory Safety," *IEEE Security & Privacy*, vol. 22, no. 04, pp. 40–49, Jul. 2024, doi: 10.1109/MSEC.2024.3379947.

### Project setup

1. Install dependencies/Run docker

If you have Docker installed, you can build and run our project using:

```bash
chmod +x ./run_docker.sh
./run_docker.sh
```

If you do not want to use docker, you can just skip this step and install the following dependencies:
- Clang (Version 10)
- LLVM (Version 10) [Official Download](https://releases.llvm.org/download.html)
- wllvm [GitHub Repository](https://github.com/travitch/whole-program-llvm)
- Unified Pass [GitHub Repository](https://github.com/Lightninghkm/Unified-Memory-Safety-Validation)

Make sure you set your terminal environment correctly:

```bash
export UNIFIED_PATH=/path/to/libunified.so
export LLVM_COMPILER=clang
export CC=wllvm
```

2. Build the libxml2 project

```bash
./build_libxml2.sh
```

3. Run analysis on the built libxml2 project

```bash
./run_analysis.sh
```
