The portal visibility regression is portable and does not require the game or DirectX. Run it from the repository root:

```sh
c++ -std=c++17 -Wall -Wextra -Werror -pedantic tests/portal_visibility.cpp -o /tmp/gd3d11-portal-test
/tmp/gd3d11-portal-test
```

It checks room reachability, doorway bounds, camera transitions, multiple entrances, cycles, reversed depth, incomplete metadata, and an independent perspective projection oracle.
