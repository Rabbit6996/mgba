# Wi-Fi link cable tests (development aids)

These tools exercise the network link driver (`src/gba/sio/netlink.c`) and the
GUI flow (`src/feature/gui/netlink.c`) on a Linux PC, without a 3DS.

* `rom/` – a tiny GBA test ROM (build with `arm-none-eabi-gcc`, see below) that
  runs a configurable number of multiplayer (or normal-32) transfers and checks
  every word on both sides.
* `harness.c` – headless runner for one side of a session (`host` or `join`).
* `mock-gui.c` – headless mGUI frontend. It stubs the font/input hooks a port
  provides, prints every screen it "draws" and clicks through the pause menu,
  hosts/joins, pauses mid-session and exits, like a user would.
* `proxy.py` – TCP relay that adds latency/jitter.

```sh
# test ROM
arm-none-eabi-gcc -mcpu=arm7tdmi -marm -O2 -nostdlib -ffreestanding -T rom/rom.ld \
    rom/crt0.s rom/main.c -o rom/linktest.elf
arm-none-eabi-objcopy -O binary rom/linktest.elf rom/linktest.gba

# libmgba with the feature (from the repository root)
mkdir build-linux && cd build-linux
cmake .. -DLIBMGBA_ONLY=ON -DBUILD_STATIC=ON -DBUILD_SHARED=OFF -DENABLE_NETLINK=ON \
    -DUSE_FFMPEG=OFF -DUSE_ZLIB=OFF -DUSE_PNG=OFF -DUSE_LIBZIP=OFF -DUSE_SQLITE3=OFF \
    -DUSE_ELF=OFF -DUSE_LZMA=OFF -DENABLE_SCRIPTING=OFF -DUSE_EDITLINE=OFF
make -j8 && cd ../test-netlink

./build.sh
./run.sh basic 5738 5738 2000 1000 0 50        # count gap mode(0=multi,1=normal32) childDelay
THROTTLE=1 ./latency.sh 2 0 600 1000 0 50       # 2 ms each way, frame-limited
PAUSE_AT=20 ./run.sh pause 5738 5738 2000 1000 0 50
```

The mock GUI needs a libmgba built with PNG support in `build-gui/`
(normal CMake configure with `-DBUILD_QT=OFF -DBUILD_SDL=OFF -DENABLE_NETLINK=ON`,
then `make mgba`), then `./build-mock.sh` and run `./mock-gui host rom/linktest.gba`
and `./mock-gui join rom/linktest.gba` in two terminals.
