# pcie-driver

Some Linux PCIe driver code I wrote while learning how kernel drivers work. `pcie_prototype/` — basic driver with ioctl calls

- `pcie_prototype_ring/` — adds ring buffers
- `pcie_prototype_sim/` — same thing but can run without real hardware
- `pcie_prototype_async/` — the main one, talks to QEMU's EDU device

## Running the async driver

You need Linux with kernel headers. I tested it in a QEMU VM with the EDU device.

```bash
cd pcie_prototype_async/kernel
make
sudo insmod edu_pci.ko

cd ../userspace
make
sudo LD_LIBRARY_PATH=. ./demo
```

In QEMU, boot with `-device edu`. Should show up as `/dev/edu_pci`.

The async version does factorial math and DMA tests, plus has submission/completion queues and mmap for zero-copy.

**Tests:**

```bash
cd pcie_prototype_async/userspace
sudo PYTHONPATH=. python3 -m pytest python/test_edu_device.py -v
```

**Benchmarks:**

```bash
sudo pcie_prototype_async/scripts/run_benchmarks.sh
```



## Requirements

- Linux + kernel headers
- g++, make, python3
- QEMU with `-device edu` support

