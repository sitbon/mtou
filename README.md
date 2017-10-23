mtou
=============

Multicast-unicast forwarding tool.

```
Usage: mtou <-I IFACE | -i ADDR> <-p PORT> <-o ADDR> ...

  -I, --iface=IFACE    Incoming interface.
  -i, --in=ADDR        Source address.
  -p, --port=PORT      Port to listen on.
  -O, --oface=IFACE    Outgoing interface.
  -o, --out=ADDR       Destination address.
  -P, --out-port=PORT  Port to send to.
  -v, --verbose        Verbose output.
```

Requirements
=============

This project uses CMake.

This has been lightly tested on Mac, heavily used on Linux.

Building
=============

In the project top-level folder:

    mkdir build
    cd build
    cmake ..
    make
    cd ..

The binary is then in ./build/mtou.

<a href="https://scan.coverity.com/projects/sitbon-mtou">
  <img alt="Coverity Scan Build Status"
       src="https://scan.coverity.com/projects/12210/badge.svg"/>
</a>
