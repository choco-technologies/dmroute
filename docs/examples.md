# dmroute Usage Examples

All examples assume this helper for building an IPv4 `dmroute_addr_t`:

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
```

---

## Example 1: Add a route and look it up

The simplest case: one directly-connected subnet, no gateway.

```c
void basic_example(void)
{
    dmroute_addr_t net  = make_v4(10, 0, 0, 0);
    dmroute_addr_t mask = make_v4(255, 0, 0, 0);

    dmroute_route_t route = dmroute_add(&net, &mask, NULL, "eth0",
                                         DMROUTE_DEFAULT_METRIC,
                                         dmroute_origin_connected);
    if (route == NULL)
    {
        return; /* invalid arguments or out of memory */
    }

    dmroute_addr_t target = make_v4(10, 1, 2, 3);
    dmroute_route_t found = dmroute_lookup(&target);
    /* found == route, since 10.1.2.3 is inside 10.0.0.0/8 */

    dmroute_remove(route);
}
```

---

## Example 2: Destinations are masked automatically

You never need to mask a destination yourself before calling `dmroute_add()`
- passing a host address alongside a network's netmask is equivalent to
passing the network address itself.

```c
void masking_example(void)
{
    /* 10.1.2.3/8 is stored as the network address 10.0.0.0 */
    dmroute_addr_t host = make_v4(10, 1, 2, 3);
    dmroute_addr_t mask = make_v4(255, 0, 0, 0);
    dmroute_route_t route = dmroute_add(&host, &mask, NULL, "eth0",
                                         DMROUTE_DEFAULT_METRIC,
                                         dmroute_origin_static);

    dmroute_addr_t stored = { 0 };
    dmroute_get_destination(route, &stored);
    /* stored == 10.0.0.0, not 10.1.2.3 */

    dmroute_remove(route);
}
```

---

## Example 3: Longest-prefix match with a default route

A wide default route (`0.0.0.0/0`) and a more specific subnet route can
coexist; dmroute always prefers the most specific match, falling back to the
default route for everything else.

```c
void longest_prefix_example(void)
{
    dmroute_addr_t any_addr = make_v4(0, 0, 0, 0);
    dmroute_addr_t any_mask = make_v4(0, 0, 0, 0);
    dmroute_addr_t gateway  = make_v4(192, 168, 1, 1);
    dmroute_route_t default_route = dmroute_add(&any_addr, &any_mask,
                                                 &gateway, "eth0", 100,
                                                 dmroute_origin_static);

    dmroute_addr_t lan_net  = make_v4(192, 168, 1, 0);
    dmroute_addr_t lan_mask = make_v4(255, 255, 255, 0);
    dmroute_route_t lan_route = dmroute_add(&lan_net, &lan_mask, NULL,
                                             "eth0", DMROUTE_DEFAULT_METRIC,
                                             dmroute_origin_connected);

    dmroute_addr_t local_host = make_v4(192, 168, 1, 42);
    /* dmroute_lookup(&local_host) == lan_route: /24 beats /0 */

    dmroute_addr_t internet_host = make_v4(8, 8, 8, 8);
    /* dmroute_lookup(&internet_host) == default_route: only the /0 matches */

    dmroute_remove(lan_route);
    dmroute_remove(default_route);
}
```

---

## Example 4: Breaking ties with metric

When two routes match a destination equally specifically, the one with the
lower `metric` wins - useful for e.g. preferring a wired link over a Wi-Fi
fallback to the same subnet.

```c
void metric_tie_break_example(void)
{
    dmroute_addr_t net  = make_v4(10, 0, 0, 0);
    dmroute_addr_t mask = make_v4(255, 0, 0, 0);

    dmroute_route_t via_wifi = dmroute_add(&net, &mask, NULL, "wlan0", 100,
                                            dmroute_origin_static);
    dmroute_route_t via_eth  = dmroute_add(&net, &mask, NULL, "eth0", 10,
                                            dmroute_origin_static);

    dmroute_addr_t target = make_v4(10, 1, 2, 3);
    /* dmroute_lookup(&target) == via_eth: same /8 prefix, lower metric wins */

    dmroute_remove(via_wifi);
    dmroute_remove(via_eth);
}
```

---

## Example 5: Listing the whole table (`ip route show`-style)

`dmroute_for_each()` visits every route in insertion order without the
instability of an index-based loop across intervening add/remove calls.

```c
#include <stdio.h>

static bool print_route(dmroute_route_t route, void* user_data)
{
    (void)user_data;

    dmroute_addr_t dest = { 0 };
    dmroute_addr_t mask = { 0 };
    dmroute_get_destination(route, &dest);
    dmroute_get_netmask(route, &mask);

    printf("%u.%u.%u.%u/%u dev %s metric %u\n",
           dest.addr.v4[0], dest.addr.v4[1], dest.addr.v4[2], dest.addr.v4[3],
           mask.addr.v4[0], /* just the first octet for brevity */
           dmroute_get_iface_name(route),
           dmroute_get_metric(route));

    return true; /* keep going */
}

void list_routes_example(void)
{
    dmroute_for_each(print_route, NULL);
    printf("total routes: %zu\n", dmroute_count());
}
```

`dmroute_for_each()` must not be used to add or remove routes from within the
callback itself - the underlying traversal advances to the next node only
after the callback returns, so removing the node currently being visited
would leave the traversal dangling. To remove everything, capture one route
at a time and remove it after the traversal has stopped (see the "remove all
routes" pattern below).

---

## Example 6: Removing every route

`dmroute_for_each()` stops early as soon as its callback returns `false`,
which makes it easy to pull routes out one at a time without walking a
half-torn-down list.

```c
static bool capture_first(dmroute_route_t route, void* user_data)
{
    *(dmroute_route_t*)user_data = route;
    return false; /* stop after the first route */
}

void remove_all_routes_example(void)
{
    while (dmroute_count() > 0)
    {
        dmroute_route_t route = NULL;
        dmroute_for_each(capture_first, &route);
        if (route == NULL)
        {
            break; /* safety net, shouldn't happen while count() > 0 */
        }
        dmroute_remove(route);
    }
}
```

---

## Example 7: IPv6

`dmroute_addr_t` covers both families through the same functions - only the
`family` and the `addr.v6` bytes differ.

```c
#include <string.h>

static dmroute_addr_t make_v6(const uint8_t bytes[DMROUTE_IPV6_ADDR_LEN])
{
    dmroute_addr_t addr = { 0 };
    addr.family = dmroute_family_v6;
    memcpy(addr.addr.v6, bytes, DMROUTE_IPV6_ADDR_LEN);
    return addr;
}

void ipv6_example(void)
{
    uint8_t net_bytes[DMROUTE_IPV6_ADDR_LEN]  = { 0x20, 0x01, 0x0d, 0xb8 };
    uint8_t mask_bytes[DMROUTE_IPV6_ADDR_LEN] = { 0xff, 0xff, 0xff, 0xff };
    dmroute_addr_t net  = make_v6(net_bytes);  /* 2001:db8::/32 */
    dmroute_addr_t mask = make_v6(mask_bytes);

    dmroute_route_t route = dmroute_add(&net, &mask, NULL, "eth0",
                                         DMROUTE_DEFAULT_METRIC,
                                         dmroute_origin_static);

    uint8_t target_bytes[DMROUTE_IPV6_ADDR_LEN] = { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 1 };
    dmroute_addr_t target = make_v6(target_bytes);
    /* dmroute_lookup(&target) == route */

    dmroute_remove(route);
}
```

A destination and its route's netmask/gateway must always share the same
`family` - mixing IPv4 and IPv6 addresses on the same route fails
`dmroute_add()` (returns `NULL`).
