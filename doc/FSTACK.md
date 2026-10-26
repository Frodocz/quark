# Building and tuning F-Stack and DPDK for quark

`-DQUARK_USE_FSTACK=ON` runs `quark::net` over [F-Stack](https://github.com/F-Stack/f-stack), a
FreeBSD TCP/IP stack in user space on DPDK. This guide covers building DPDK and F-Stack for quark,
preparing the host (AWS EC2 with ENA in the examples), configuring `f-stack.conf`, and the known
problems. Host settings that apply to both backends (isolated cores, IRQs, huge pages, the TSC)
are in [TUNING.md](TUNING.md).

Versions this guide was tested with:

| Component | Version | Note |
|---|---|---|
| F-Stack | v2.0 (tag `v2.0`, 2026-10-20) | plus [patches/f-stack-2.0-eal-argv-double-free.patch](patches/f-stack-2.0-eal-argv-double-free.patch) |
| DPDK | 24.11.8 LTS | bundled in the F-Stack tree (`dpdk/`); use that copy, not a separate DPDK |
| Kernel / compiler | Ubuntu 22.04, 6.8.0-aws, gcc-12 | |
| Instance | AMD EPYC 7R13 (no AVX-512), ENA | |

## 1. Prerequisites

```sh
sudo apt-get install -y build-essential gcc-12 meson ninja-build python3-pyelftools \
    libnuma-dev libpcap-dev libssl-dev zlib1g-dev linux-headers-$(uname -r)
pkg-config --version   # >= 0.28 (F-Stack's lib/Makefile needs --static --libs to be right)
```

Get the sources (one checkout per F-Stack version keeps the previous one for rollback):

```sh
git clone https://github.com/F-Stack/f-stack.git /data/f-stack
cd /data/f-stack && git worktree add --detach /data/f-stack-2.0 v2.0
cd /data/f-stack-2.0 && git apply ~/codes/quark/doc/patches/f-stack-2.0-eal-argv-double-free.patch
```

## 2. Build DPDK

```sh
cd /data/f-stack-2.0/dpdk
CC=x86_64-linux-gnu-gcc-12 meson setup build \
    -Dcpu_instruction_set=x86-64-v3 \
    -Denable_kmods=true \
    -Denable_drivers=bus/pci,bus/vdev,mempool/ring,net/ena,net/virtio,net/bonding,net/ring
ninja -C build
sudo ninja -C build install && sudo ldconfig
pkg-config --modversion libdpdk          # 24.11.8
pkg-config --cflags libdpdk              # must show -march=x86-64-v3
```

What each option does and why:

- **`cpu_instruction_set` (the important one).** DPDK's default (`platform=native`) compiles with
  `-march=native`, and that flag is exported in `libdpdk.pc`, so F-Stack inherits it. A build made
  on an AVX-512 host (Intel Xeon) then dies with `SIGILL` ("illegal hardware instruction") on any
  host without AVX-512 (e.g. AMD EPYC Milan) — even in quark's unit tests, because DPDK's static
  constructors run before `main()` in every binary that links `libfstack.a`. `x86-64-v3` (AVX2,
  BMI2, FMA) runs on every current Intel and AMD EC2 instance. Use `native` only if the build host
  and every run host are the same instance family, and rebuild DPDK, F-Stack and quark together if
  the instance type changes. Little is lost: the ENA PMD has no AVX-512 path, and DPDK selects its
  AVX-512 code (where it has any) at run time from CPU flags.
- **`CC=x86_64-linux-gnu-gcc-12`**: `igb_uio.ko` must be built with the compiler the kernel was
  built with (`cat /proc/version`).
- **`enable_kmods=true`** builds `igb_uio.ko` (F-Stack keeps it in its DPDK copy; upstream DPDK
  moved it to dpdk-kmods). The module is tied to the running kernel: **after every kernel update,
  rebuild DPDK** (`ninja -C build`), otherwise `insmod` fails with "Invalid module format".
- **`enable_drivers`** keeps only what F-Stack and EC2 need: the ENA PMD, virtio (F-Stack's KNI
  uses virtio-user), bonding (`libfstack.a` references its API), the PCI and vdev buses and the
  ring mempool. Fewer drivers mean a faster build, a smaller binary, and fewer static
  constructors (`libfstack.a` is linked whole-archive). Add drivers here for other NICs
  (`net/mlx5`, `net/iavf`, ...).

**Replacing an older DPDK.** Uninstall it from its own build directory first
(`sudo ninja -C <old>/dpdk/build uninstall`): headers of drivers the new build doesn't have would
otherwise linger in `/usr/local/include`. Then check nothing stale is left:

```sh
comm -23 <(cd /usr/local/include && ls rte_*.h | sort) \
         <(grep -o '/usr/local/include/rte_[^ ]*' build/meson-logs/install-log.txt | xargs -n1 basename | sort)
```

## 3. Build F-Stack

```sh
cd /data/f-stack-2.0/lib
export FF_PATH=/data/f-stack-2.0 PKG_CONFIG_PATH=/usr/local/lib/x86_64-linux-gnu/pkgconfig
make -j$(nproc)
sudo env FF_PATH=$FF_PATH PKG_CONFIG_PATH=$PKG_CONFIG_PATH make install
```

`sudo` drops `PKG_CONFIG_PATH`, hence the `env` (otherwise: "no installation of DPDK found").
`make install` puts `libfstack.a` (a symlink to `libfstack.a.2.0`) in `/usr/local/lib`, the
`ff_*.h` headers in `/usr/local/include`, and copies `config.ini` to `/etc/f-stack.conf` if
absent. Remove the previous `libfstack.a.<version>` by hand.

Build switches, uncommented in `lib/Makefile` or passed to `make` (`make FF_ZC_SEND=1`):

| Switch | Default | For quark |
|---|---|---|
| `FF_KNI` | on | forwards unhandled traffic to a kernel virtio interface; quark doesn't need it (`[kni]` stays disabled) |
| `FF_NETGRAPH`, `FF_IPFW` | on | FreeBSD netgraph and firewall; unused by quark |
| `FF_TCPHPTS`, `FF_EXTRA_TCP_STACKS` | on | the RACK and BBR TCP stacks (`net.inet.tcp.functions_default`) |
| `FF_LOOPBACK_SUPPORT` | on | TCP/UDP over F-Stack's `lo0` |
| `FF_INET6` | on | IPv6 |
| `FF_USE_PAGE_ARRAY` | off | zero-copy TX through a page array; not compatible with `graceful_reload` |
| `FF_ZC_SEND` | off | `ff_zc_send()`; quark doesn't use it |
| `FF_KERNEL_COEXIST` | off | per-socket `SOCK_KERNEL`: kernel sockets through the F-Stack API |
| `DEBUG` | unset | `make DEBUG="-O0 -gdwarf-2 -g3 -Wno-format-truncation"` for a debuggable library |

The defaults are fine for quark. Turning off the unused subsystems shrinks the library; nothing
was measured on quark's per-packet path.

Check that neither library contains instructions the run hosts lack (0 = no AVX-512):

```sh
for f in /usr/local/lib/libfstack.a /usr/local/lib/x86_64-linux-gnu/librte_eal.a; do
    objdump -d $f | grep -cE '%zmm|vmovdqu(8|16|64)|vpternlog|%k[1-7]'
done
```

## 4. Build quark

```sh
cmake -S . -B build-fstack -DCMAKE_BUILD_TYPE=Release -DQUARK_USE_FSTACK=ON \
    -DQUARK_FSTACK_PEER_IP=10.20.88.228        # optional, see section 7
cmake --build build-fstack -j$(nproc)
```

| CMake variable | Default | |
|---|---|---|
| `FSTACK_PREFIX` | `/usr/local` | where `include/ff_api.h` and `lib/libfstack.a` are |
| `QUARK_FSTACK_TEST_CONF` | `doc/f-stack.conf.example` | config `quark_test_fstack` runs with under ctest; empty: not registered |
| `QUARK_FSTACK_PEER_IP` | empty | kernel address of this host reachable from the DPDK port; enables the local-peer tests |

DPDK is found through pkg-config (`PKG_CONFIG_PATH` as above if CMake doesn't find `libdpdk`).
Rebuild quark after rebuilding DPDK or F-Stack: both are linked statically.

## 5. Prepare the host (every boot)

DPDK needs huge pages and the NIC bound to a user-space driver; none of this survives a reboot.

```sh
# 2 GiB of 2 MiB huge pages (F-Stack's mbuf pools; quark's arenas use them too)
echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
sudo mkdir -p /mnt/huge && (mountpoint -q /mnt/huge || sudo mount -t hugetlbfs nodev /mnt/huge)

# igb_uio with write combining (see ENA below)
sudo modprobe uio
sudo insmod /data/f-stack-2.0/dpdk/build/kernel/linux/igb_uio/igb_uio.ko wc_activate=1

# hand the DPDK ENI to igb_uio (find its PCI address with: dpdk-devbind.py -s)
sudo ip link set ens6 down
sudo dpdk-devbind.py --bind=igb_uio 0000:00:06.0
dpdk-devbind.py -s | head -5        # 0000:00:06.0 ... drv=igb_uio
```

Note the ENI's address, netmask and gateway *before* binding it: the kernel forgets them.

To make it persistent, put the commands in a script (say `/usr/local/sbin/dpdk-setup.sh`) and
run it at boot:

```ini
# /etc/systemd/system/dpdk-setup.service
[Unit]
Description=Huge pages, igb_uio and DPDK port binding
After=network-pre.target
Before=network.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/dpdk-setup.sh

[Install]
WantedBy=multi-user.target
```

`sudo systemctl enable dpdk-setup`. Huge pages can also go in `/etc/sysctl.d/`
(`vm.nr_hugepages = 1024`).

**igb_uio or vfio-pci.** F-Stack recommends igb_uio (about 5% faster by its measurements). EC2
instances usually have no IOMMU (`ls /sys/kernel/iommu_groups` is empty), so vfio-pci would need
`enable_unsafe_noiommu_mode=1`, which loses vfio's only advantage. Stay with igb_uio.

### AWS ENA specifics

- **A dedicated ENI for DPDK.** The primary ENI stays with the kernel for SSH, DNS and
  everything else (quark resolves names through the kernel: `Endpoint::resolve`). Attach a
  second ENI in the same subnet; it shows up as `ens6`.
- **Security groups apply to the DPDK ENI like to any other.** To reach hosts in the VPC
  (including this host's kernel ENI, for the local tests), the peer's security group must admit
  the DPDK ENI's address, not only the kernel ENI's.
- **Write combining (`wc_activate=1`).** ENA puts TX descriptors and packet headers straight
  into device memory (LLQ, "low-latency queue"). Without write combining on that BAR every write
  to it is uncached, which AWS's ENA DPDK documentation calls out as a large TX slowdown; igb_uio
  only maps it write-combined with `wc_activate=1`.
- **No promiscuous mode**: keep `promiscuous=0` (with 1 the PMD warns and carries on).
- **RSS**: ENA ignores the requested hash fields ("Setting RSS hash fields is not supported.
  Using default values"); harmless with one queue.
- **`ena_close(): Failed to unregister interrupt handler`** at exit is harmless: igb_uio gives
  the PMD no MSI-X vector to unregister.
- **Per-ENI limits**: `ethtool -S` is gone once the ENI belongs to DPDK; the ENA PMD exports
  the same `bw_in/bw_out/pps_allowance_exceeded` counters as extended stats
  (`sudo dpdk-proc-info -- --xstats` while the quark process runs).

## 6. Configure F-Stack

[f-stack.conf.example](f-stack.conf.example) is a complete config for quark; the full reference
is `config.ini` in the F-Stack tree. Settings that matter for quark:

**`[port0]`**: `addr`, `netmask`, `broadcast`, `gateway` of the DPDK ENI, and `if_name`.

**`lcore_mask`** — one bit: quark runs one F-Stack loop per process, on the thread that calls
`EventLoop::run()`, and that thread is pinned to the core of the bit. Use an isolated core
(TUNING.md, section 1): `lcore_mask=4` is core 2. The loop busy-polls that core at 100%.

**Latency settings** — F-Stack's defaults trade latency for throughput; set all three to 0:

| Key | F-Stack default | quark | Effect |
|---|---|---|---|
| `idle_sleep` | 0 | 0 | µs the loop sleeps when no packet arrived |
| `pkt_tx_delay` | 100 | 0 | µs a TX burst of fewer than 32 packets waits before it's sent |
| `idle_loop_interval` | 100 | 0 | **new in 2.0**: minimum µs between two `loop()` callbacks while nothing is received |

`idle_loop_interval` matters most: quark's whole step (timers, `CommandQueue`s, sends, the
application's step hook) runs inside F-Stack's `loop()` callback, so with the default anything
not triggered by an incoming packet waits up to 100 µs. A config written for F-Stack 1.x doesn't
have the key and gets the default.

**Offloads**: `tso=0`, `lro=0` (2.0 adds LRO): both batch segments, which helps throughput, not
the latency of small messages. Checksum offloads are on by default and ENA supports them.

**`[freebsd.boot]` / `[freebsd.sysctl]`**: the FreeBSD stack's tunables. `hz` is the stack's
timer tick (TCP retransmit and delayed-ACK timers run on it); `net.inet.tcp.sendspace` /
`recvspace` and the `*buf_max` limits size socket buffers; `net.inet.tcp.delayed_ack` delays
ACKs as in the kernel. quark sets `TCP_NODELAY` on its sockets. The example keeps F-Stack's
values.

**`extra_eal_args`** (new in 2.0) appends arguments to `rte_eal_init()`, e.g.
`extra_eal_args=--allow=0000:00:06.0` so DPDK probes only its port, or `--log-level=pmd.net.ena:8`
for ENA debugging.

## 7. Run the tests

```sh
cd build-fstack
ctest                       # unit tests; quark_test_fstack reports "Skipped" when not root
sudo ctest -L fstack        # the F-Stack tests over the DPDK port
sudo chown -R $USER Testing # ctest as root leaves root-owned files
# or directly:
sudo QUARK_FSTACK_PEER_IP=10.20.88.228 ./tests/quark_test_fstack --conf ../doc/f-stack.conf.example
```

`quark_test_fstack` runs HTTP, HTTPS and a WebSocket echo against public endpoints, then (with
`QUARK_FSTACK_PEER_IP`) WebSocket over TLS and HTTP keep-alive against test servers it starts on
kernel sockets at that address, each in busy-poll and epoll mode.

A live end-to-end check is the Binance example
([examples/binance_ws.cpp](../examples/binance_ws.cpp)): the same source as on kernel sockets,
subscribing to Binance's public market data over WebSocket + TLS through the DPDK port:

```sh
sudo ./examples/quark_example_binance_ws --conf ../doc/f-stack.conf.example            # Ctrl-C to stop
sudo ./examples/quark_example_binance_ws --conf ../doc/f-stack.conf.example --count 500 btcusdt@bookTicker
```

It prints the connection's states, then a line per stream every second (messages per second,
best bid/ask or last trade), and exits 0 once it has received data and closed cleanly. Binance
refuses some regions (US among them) with HTTP 451 on the upgrade; the example prints that
status.

## 8. Multiple loops: `thread_mode=1`

F-Stack 2.0's `thread_mode=1` doesn't allow several `ff_init()`/`ff_run()` calls. It is still
one `ff_init()` and one `ff_run()` per process; `ff_run()` then starts the same `loop()`
callback on *every* lcore of `lcore_mask` (`rte_eal_mp_remote_launch`), each lcore with its own
FreeBSD stack instance, socket table and RSS queue. Sockets belong to the lcore that created
them, incoming packets reach an lcore by RSS hash, and outbound connects pick a source port that
hashes back to the connecting lcore.

quark supports one F-Stack loop per process today (`EventLoop::run()` calls `ff_run()`).
Using `thread_mode=1` would mean one `EventLoop` per lcore, created inside `loop()` on that
lcore, instead of `run()` on the main thread; it's not implemented. ENA also limits the number
of queues per ENI (by instance size), and its RSS hash can't be configured. For several loops
today, run one process per DPDK ENI.

## 9. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `Illegal instruction` (SIGILL) in any binary linking `libfstack.a`, before `main()`, in `rte_log_register_*` | DPDK (and F-Stack) built with `-march=native` on a CPU with instructions this one lacks | rebuild with `-Dcpu_instruction_set=x86-64-v3` (section 2), then F-Stack, then quark |
| `free(): double free detected` or `malloc_consolidate(): invalid chunk size` when `ff_run()` returns | F-Stack 2.0 bug: `rte_eal_init()` aliases two `dpdk_argv` entries and `ff_unload_config()` frees both | apply `patches/f-stack-2.0-eal-argv-double-free.patch` |
| `insmod: ERROR: could not insert module igb_uio.ko: Invalid module format` | kernel updated since DPDK was built | `ninja -C dpdk/build` (rebuilds the module) |
| `no installation of DPDK found` from F-Stack's make | `PKG_CONFIG_PATH` unset (or dropped by `sudo`) | section 3 |
| `EAL: No available 2048 kB hugepages reported` / mempool creation fails | huge pages not reserved | section 5 |
| `No probed ethernet devices` | ENI not bound to igb_uio (after a reboot) | section 5 |
| connects to VPC hosts time out, internet works | the peer's security group doesn't admit the DPDK ENI's address | section 5, AWS |
| everything idle-triggered is ~100 µs late | `idle_loop_interval` missing from the config | section 6 |
