# sysrepo-gnmi

[![CI](https://github.com/vjardin/sysrepo-gnmi/actions/workflows/ci.yml/badge.svg)](https://github.com/vjardin/sysrepo-gnmi/actions/workflows/ci.yml)

The smallest, most portable, pure C gNMI server for sysrepo.

sysrepo-gnmi is a lightweight, C-based gNMI server that plugs directly into
the [sysrepo](https://github.com/sysrepo/sysrepo) ecosystem as a first-class
peer alongside [netopeer2](https://github.com/CESNET/netopeer2) (NETCONF) and
[Rousette](https://github.com/CESNET/rousette) (RESTCONF). It is a design with
a single-threaded event loop, it is the leanest gNMI-to-sysrepo
bridge available - designed from the ground up for embedded systems,and any
 platform where footprint and portability matter.

It includes a wide test suite for all the scope of its feature sets.

## Motivation

sysrepo-gnmi is a standalone frontend process that talks directly to sysrepo via
the C API. Each frontend should own its transport; sysrepo owns the datastore.

- Pure C - no C++ runtime, no libyang-cpp/sysrepo-cpp version mismatch
- Single-threaded libevent - no worker threads; gRPC CQ, timers, and
  sysrepo callbacks all driven by one `event_base`
- protobuf-c for serialization - compiles gNMI `.proto` to C structs
- cJSON for JSON manipulation - no ad-hoc string surgery
- ON_CHANGE deltas via `sr_get_changes_iter()` - sends only what changed
- NACM via `sr_session_set_user()` - same access control as NETCONF

## Architecture

```
    +--------------+  +--------------+   +--------------+
    |  NETCONF     |  |  gNMI        |   |  RESTCONF    |
    |  client      |  |  client      |   |  client      |
    |  (ncclient,  |  |  (gnmic,     |   |  (curl,      |
    |   yangcli)   |  |   gNMIc)     |   |   Postman)   |
    +------+-------+  +------+-------+   +------+-------+
           |                 |                  |
           | NETCONF/SSH     | gNMI/gRPC        | RESTCONF/HTTP
           |                 |                  |
    +------+-------+  +------+-------+   +------+-------+
    |  netopeer2   |  | sysrepo-gnmi |   |  Rousette    |
    |  (C)         |  | (C)          |   |  (C++)       |
    |  CESNET      |  | Free Mobile  |   |  CESNET      |
    +------+-------+  +------+-------+   +------+-------+
           |                 |                  |
           +-----------------+------------------+
                             |
                     +-------+--------+
                     |    sysrepo     |
                     |  (C, CESNET)   |
                     |                |
                     |  YANG-based    |
                     |  datastore     |
                     |  (running,     |
                     |   startup,     |
                     |   operational) |
                     +-------+--------+
                             |
                     +-------+--------+
                     |    libyang     |
                     |  (C, CESNET)   |
                     |                |
                     |  YANG parser   |
                     |  & validator   |
                     +----------------+
```

All three management servers share the same sysrepo datastore. A
configuration change made via gNMI is immediately visible to NETCONF and
RESTCONF clients, and vice versa. `sysrepo` handles locking, transactions, and
change notifications across all consumers.

| Server                                              | Protocol | Transport    | Standard                                                  |
|-----------------------------------------------------|----------|--------------|-----------------------------------------------------------|
| [netopeer2](https://github.com/CESNET/netopeer2)    | NETCONF  | SSH / TLS    | RFC 6241                                                  |
| **sysrepo-gnmi**                                    | gNMI     | gRPC / HTTP2 | [OpenConfig gNMI](https://github.com/openconfig/gnmi)     |
| [Rousette](https://github.com/CESNET/rousette)      | RESTCONF | HTTP / TLS   | RFC 8040                                                  |

## Supported RPCs

| RPC          | Description                                      |
|--------------|--------------------------------------------------|
| Capabilities | Enumerate YANG modules and supported encodings   |
| Get          | Read from Running or Operational datastore       |
| Set          | Write/delete with delete, replace, update ops    |
| Subscribe    | STREAM (on-change + sample), ONCE, POLL modes    |
| Rpc          | Invoke YANG RPCs and actions                     |
| Confirm      | Confirmed-commit with timeout-based rollback     |

## Dependencies

### Build dependencies

| Library      | Package (Ubuntu/Debian)                   | Purpose                   |
|--------------|-------------------------------------------|---------------------------|
| gRPC C core  | `libgrpc-dev`                             | gRPC transport            |
| protobuf-c   | `libprotobuf-c-dev` `protobuf-c-compiler` | Protobuf serialization    |
| libevent     | `libevent-dev` (runtime: `libevent-pthreads`) | Event loop             |
| cJSON        | `libcjson-dev`                            | JSON manipulation         |
| PCRE2        | `libpcre2-dev`                            | Required by libyang       |
| libyang      | meson subproject, built via cmake           | YANG parsing, JSON_IETF   |
| sysrepo      | meson subproject, built via cmake           | YANG datastore            |

Four version sets are supported via `-Dcesnet_version=`:

| Option                    | libyang | sysrepo |
|---------------------------|---------|---------|
| `-Dcesnet_version=v2`     | 2.1.148 | 2.2.36  |
| `-Dcesnet_version=v3`     | 3.13.6  | 3.7.11  |
| `-Dcesnet_version=v4`     | 4.2.2   | 4.2.10  |
| `-Dcesnet_version=v5`     | 5.4.9   | 4.5.4   |

libyang and sysrepo are downloaded by `meson subprojects download` and
compiled automatically via cmake during the build. They install into
`builddir/deps/` - no system-level installation is required.

### Test dependencies

| Package      | Install                    | Purpose                        |
|--------------|----------------------------|--------------------------------|
| grpcio       | `pip install grpcio`       | Python gRPC client             |
| grpcio-tools | `pip install grpcio-tools` | Python protobuf stub generation|
| pytest       | `pip install pytest`       | Test framework                 |

## Building

### Install system dependencies

```sh
sudo apt install -y \
    build-essential meson ninja-build cmake \
    libgrpc-dev \
    libprotobuf-c-dev protobuf-c-compiler \
    libevent-dev libcjson-dev libpcre2-dev
```

### Download subprojects

```sh
meson subprojects download
```

This fetches the libyang and sysrepo source trees into `subprojects/`.

### Build with libyang v4 + sysrepo v4 (default)

```sh
meson setup builddir
meson compile -C builddir
```

### Build with another CESNET version

```sh
meson setup builddir -Dcesnet_version=v3
meson compile -C builddir
```

A compile-time compatibility layer (`src/compat.h`) handles the API
differences between libyang/sysrepo v2, v3, v4, and v5.

### Build with AddressSanitizer

```sh
rm -rf builddir
meson setup builddir -Dsanitize=address
meson compile -C builddir
```

Can be combined with any `-Dcesnet_version=` to test with ASAN.

Then run the tests (see below).

## Running

### Quick start (insecure, for testing)

```sh
export LD_LIBRARY_PATH=$(pwd)/builddir/deps/lib
./builddir/gnmi_server --insecure --bind localhost:50051 --log-level 3
```

### All options

```
Usage: gnmi_server [OPTIONS]

Options:
  -b, --bind ADDR:PORT    Bind address (default: localhost:50051)
  -k, --key FILE          TLS server private key PEM
  -c, --cert FILE         TLS server certificate PEM
  -a, --ca FILE           TLS root CA PEM (mutual TLS)
  -u, --username USER     Username for authentication / NACM
  -p, --password PASS     Password for authentication
  -l, --log-level LEVEL   Log level 0-4 (default: 2=warning)
  -f, --insecure          No TLS, no authentication
  -S, --syslog            Log to syslog (in addition to stderr)
  -L, --log-dir DIR       Transaction data log directory
  -v, --version           Print version and exit
  -h, --help              Print this help
```

### Environment variables

Using setenv for the following,

| Variable                  | Default        | Description                       |
|---------------------------|----------------|-----------------------------------|
| `GNMI_MAX_MSG_SIZE_KB`    | 65536 (64 MB)  | Max gRPC message size in KB       |
| `SYSREPO_REPOSITORY_PATH` | system default | sysrepo repository path           |
| `SYSREPO_SHM_PREFIX`      | (none)         | Shared memory prefix for isolation|

## Running the tests

### Setup Python virtual environment

```sh
python3 -m venv .venv
.venv/bin/pip install grpcio grpcio-tools pytest
```

### Generate Python gRPC stubs (once)

```sh
mkdir -p tests/proto
.venv/bin/python -m grpc_tools.protoc \
    -Iproto --python_out=tests/proto --grpc_python_out=tests/proto \
    proto/gnmi.proto proto/gnmi_ext.proto
```

### Run all tests

```sh
export LD_LIBRARY_PATH=$(pwd)/builddir/deps/lib
export GNMI_BUILD_DIR=$(pwd)/builddir
export GNMI_YANG_DIR=$(pwd)/tests/yang
export no_proxy=localhost,127.0.0.1

.venv/bin/pytest tests/ -v
```

### Run with AddressSanitizer

Requires a build with `-Dsanitize=address` (see above).

```sh
ASAN_OPTIONS=detect_leaks=0 .venv/bin/pytest tests/ -v
```

Leak detection is disabled because gRPC's C core leaks internally on
shutdown; the ASAN run still catches heap-buffer-overflow,
use-after-free, and stack-buffer-overflow.

### Run with Valgrind

```sh
meson setup builddir -Dsanitize=none
meson compile -C builddir

GNMI_VALGRIND=1 .venv/bin/pytest tests/ -s
```

### Run a specific test

```sh
.venv/bin/pytest tests/test_get.py::test_get_composite_key -v
```

### Run with strace

```sh
GNMI_STRACE=1 .venv/bin/pytest tests/ -v
# strace output: builddir/strace.log
```

### Run all CESNET versions in Docker

The `ci-local.sh` script builds and tests all supported CESNET version
combinations in Docker containers (requires Docker):

```sh
# Test all versions (v2, v3, v4, v5) in parallel
./ci-local.sh all

# Test a single version
./ci-local.sh v5
```

Supported version matrix:

| Option | libyang | sysrepo |
|--------|---------|---------|
| `v2`   | 2.1.148 | 2.2.36  |
| `v3`   | 3.13.6  | 3.7.11  |
| `v4`   | 4.2.2   | 4.2.10  |
| `v5`   | 5.4.9   | 4.5.4   |

## Installing on a system with sysrepo

### Prerequisites

sysrepo and libyang must be installed on the system. If using the versions
built by the meson subprojects:

```sh
# Install libyang
sudo cmake --install builddir/deps_build/libyang

# Install sysrepo
sudo cmake --install builddir/deps_build/sysrepo
sudo ldconfig
```

### Install gnmi_server

```sh
sudo install -m 755 builddir/gnmi_server /usr/sbin/gnmi_server
```

### Install systemd service

```sh
sudo cp conf/gnmi-server.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now gnmi-server
```

Edit `/etc/systemd/system/gnmi-server.service` to configure TLS certificates,
bind address, and log level.

### Install YANG modules

If your YANG modules are not yet installed in sysrepo:

```sh
sysrepoctl -i /path/to/your-module.yang
```

### Verify

```sh
# Check service status
sudo systemctl status gnmi-server

# Test with gnmic
gnmic -a localhost:50051 --insecure capabilities
```

## Design

The server uses a single-threaded event loop architecture based on libevent:

- gRPC completion queue polled by a 2ms `evtimer`
- Subscribe SAMPLE timers via `evtimer_new()`
- Subscribe `ON_CHANGE` via `sr_module_change_subscribe()` + `event_active()`
  or thread-safe wakeup from sysrepo's callback thread
- Confirmed-commit timeout via `evtimer`
- Signal handling via `evsignal_new()` (SIGTERM/SIGINT)

No worker threads are needed. All gRPC I/O is serialized in the main loop.
