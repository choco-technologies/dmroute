# dmroute

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

dmroute is a [DMOD](https://github.com/choco-technologies/dmod) library module
implementing a longest-prefix-match IP routing table. It answers exactly one
question - "which interface (and gateway, if any) should a packet to this
destination go through?" - it never sends or receives a frame itself; that is
[dmnetif](../dmnetif)'s `dmnetif_send()`/`_receive()` job.

dmroute is the base of the network module graph: it has **no dependency on any
other in-tree module** and owns `dmroute_addr_t`, the address type
dmnetif/dmarp/dmip all build on. Every dmnetif interface reports itself here
automatically as soon as it's assigned an IP address (dmnetif calls
`dmroute_add()`/`_remove()` directly - see
[docs/dmroute.md](docs/dmroute.md#automatic-registration)). Anything else (a
TCP/IP stack, static config, the `ip` CLI) can add further routes through
`dmroute_add()` and ask "which interface should this destination go through"
with `dmroute_lookup()`.

```
┌──────────────────────────────────────────────┐
│      TCP/IP stack (networkd) / ip / netctl    │
├──────────────────────────────────────────────┤
│                  DMNETIF                      │
│   named interfaces, IP/netmask bookkeeping,   │
│   keeps its interfaces' connected routes in   │
│   sync by calling into DMROUTE directly       │
├──────────────────────────────────────────────┤
│                  DMROUTE                      │
│   add/remove routes, longest-prefix lookup,   │
│   enumeration, owns the shared dmroute_addr_t │
│   address type                                │
└──────────────────────────────────────────────┘
```

## Key design points

- **Longest-prefix match.** `dmroute_lookup()` returns the route whose netmask
  has the most bits set among those that match; ties are broken by the lowest
  `metric`, then by insertion order. There is no other route-selection
  mechanism (no per-route "preferred" flag, no source-based routing).
- **One address type for IPv4 and IPv6.** `dmroute_addr_t` is discriminated by
  `family` rather than having separate v4/v6 types, so a route entry, a
  lookup, and the masking math all branch on `family` once rather than
  needing parallel code paths.
- **Destinations are stored pre-masked.** `dmroute_add()` always applies
  `netmask` to `destination` before storing it, so passing a host address
  alongside a network's netmask behaves identically to passing the network
  address itself.
- **`iface_name` is a plain string, never validated.** dmroute has no
  dependency on dmnetif, so it cannot check that a name refers to a real,
  currently registered interface. Callers that care (e.g. `ip route add`)
  must check with `dmnetif_find_by_name()` themselves first.
- **Thread-safe.** Every add/remove/lookup/enumeration call is guarded by a
  single mutex (`dmosi`) around the shared route table (`dmlist`).

See [docs/dmroute.md](docs/dmroute.md) for the full rationale, including why
dmnetif depends on dmroute rather than the other way around, and
[docs/api-reference.md](docs/api-reference.md) for the complete API.

## Building

dmroute is a standalone DMOD module: its `CMakeLists.txt` fetches `dmod`
itself via CMake's `FetchContent` (defaulting to the `develop` branch), so no
other repository needs to be checked out first.

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

This builds both the `dmroute` library module and its `test_dmroute` test
binary (see [Testing](#testing) below). Pass `-DDMOD_DIR=/path/to/local/dmod`
to build against a local dmod checkout instead of fetching it from GitHub, and
`-DDMOD_MODULE_VERSION=x.y` to override the module version (defaults to `0.1`).

A Make-based build is also available (`make` at the repo root) for embedded
targets, driven by the same `dmod`/`paths.mk` machinery; it builds the
`dmroute` module itself and does not build the test suite.

## Testing

Tests are plain [DMOD test modules](docs/dmroute.md) built with
`dmod_add_test()`: `tests/dmroute_test.c` registers each test case with
`DMOD_TEST_STEP()` and is linked against a test-runner `main()` provided by
the DMOD framework (see [dmod's cmake-functions.md](https://github.com/choco-technologies/dmod/blob/develop/docs/cmake-functions.md)).

After building (see above), run the resulting binary directly from the build
directory:

```bash
./tests/test_dmroute
```

It discovers and runs every `DMOD_TEST_STEP()` automatically and exits with a
code equal to the number of failed steps (`0` means everything passed), which
makes it usable directly as a CI gate. `tests/dmroute_test.c` covers:

- **Add/remove** - handle validity, netmask masking, mismatched/NULL argument
  rejection.
- **Lookup** - matching, no-match, longest-prefix preference, metric
  tie-breaking, invalid destination handling.
- **Accessors** - that every added value (`gateway`, `iface_name`, `metric`,
  `origin`) is reported back correctly, including the "no gateway" and
  "connected origin" cases.
- **No dmnetif dependency** - `dmroute_add()` accepts an interface name it
  cannot and does not validate.

## Usage

```c
#include "dmroute.h"

static dmroute_addr_t make_v4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    dmroute_addr_t addr = { 0 };
    addr.family = dmroute_family_v4;
    addr.addr.v4[0] = a;
    addr.addr.v4[1] = b;
    addr.addr.v4[2] = c;
    addr.addr.v4[3] = d;
    return addr;
}

void example(void)
{
    /* Directly-connected route: 192.168.1.0/24 via eth0, no gateway */
    dmroute_addr_t net_192  = make_v4(192, 168, 1, 0);
    dmroute_addr_t mask_24  = make_v4(255, 255, 255, 0);
    dmroute_route_t lan_route =
        dmroute_add(&net_192, &mask_24, NULL, "eth0", DMROUTE_DEFAULT_METRIC,
                    dmroute_origin_connected);

    /* Default route: 0.0.0.0/0 via a gateway on that subnet */
    dmroute_addr_t any_addr = make_v4(0, 0, 0, 0);
    dmroute_addr_t any_mask = make_v4(0, 0, 0, 0);
    dmroute_addr_t gateway  = make_v4(192, 168, 1, 1);
    dmroute_route_t default_route =
        dmroute_add(&any_addr, &any_mask, &gateway, "eth0", 100,
                    dmroute_origin_static);

    /* Longest-prefix-match lookup: the more specific /24 route wins over
     * the /0 default for any address inside 192.168.1.0/24 */
    dmroute_addr_t target = make_v4(192, 168, 1, 42);
    dmroute_route_t route = dmroute_lookup(&target); /* == lan_route */

    dmroute_addr_t gw = { 0 };
    dmroute_get_gateway(route, &gw); /* gw.family == dmroute_family_none:
                                       * directly connected, no gateway hop */

    /* A destination outside every known subnet falls back to the default
     * route instead */
    dmroute_addr_t outside = make_v4(8, 8, 8, 8);
    dmroute_route_t via_default = dmroute_lookup(&outside); /* == default_route */
    (void)via_default;

    dmroute_remove(lan_route);
    dmroute_remove(default_route);
}
```

See [docs/examples.md](docs/examples.md) for more worked examples (listing
the whole table, IPv6, metric tie-breaking, cleaning up all routes).

## API Overview

| Category           | Functions                                                              |
|--------------------|-------------------------------------------------------------------------|
| Add / remove       | `dmroute_add()`, `dmroute_remove()`                                     |
| Lookup / enumerate | `dmroute_lookup()`, `dmroute_count()`, `dmroute_for_each()`              |
| Accessors          | `dmroute_get_destination()`, `dmroute_get_netmask()`, `dmroute_get_gateway()`, `dmroute_get_iface_name()`, `dmroute_get_metric()`, `dmroute_get_origin()` |
| Types              | `dmroute_addr_t`, `dmroute_route_t`, `dmroute_family_t`, `dmroute_origin_t`, `dmroute_iterator_func_t` |

Full parameter/return documentation lives in
[docs/api-reference.md](docs/api-reference.md).

## Documentation

See the `docs/` directory:

- **[dmroute.md](docs/dmroute.md)** - Overview and architecture
- **[api-reference.md](docs/api-reference.md)** - Complete API documentation
- **[examples.md](docs/examples.md)** - Worked usage examples

View documentation using `dmf-man dmroute`.

## Dependencies

- [`dmlist`](https://github.com/choco-technologies/dmlist) - backs the route
  table
- [`dmosi`](https://github.com/choco-technologies/dmosi) - mutex guarding the
  route table

## Project Structure

```
dmroute/
├── docs/              # Documentation (markdown format)
│   ├── README.md
│   ├── dmroute.md
│   ├── api-reference.md
│   └── examples.md
├── include/           # Public headers
│   └── dmroute.h
├── src/
│   └── dmroute.c
├── tests/
│   ├── CMakeLists.txt
│   └── dmroute_test.c
├── CMakeLists.txt
├── Makefile
├── dmroute.dmr
└── manifest.dmm
```

## Author

Patryk Kubiak

## License

MIT License (see [LICENSE](LICENSE) file for details)
