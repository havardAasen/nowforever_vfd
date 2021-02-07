# nowforever-vfd
Nowforever D100 and E100 VFD LinuxCNC HAL userspace interface / driver using
RS485 MODBUS RTU

## Build and install
I'm assuming you already have set up LinuxCNC, if not, go to their website
<https://linuxcnc.org> to get started.

We require some additional packages, \
git - to download the repository \
libmodbus-dev - developement files for the modbus connection \
linuxcnc-uspace-dev - developement files for LinuxCNC

```
$ sudo apt-get install libmodbus-dev linuxcnc-uspace-dev git
$ git clone https://github.com/havardAasen/nowforever_vfd.git
$ cd nowforever_vfd
$ make
$ sudo make install
```

## Documentation and usage
Look at the man-page **nowforever_vfd.1** which describes the parameter to
adjust on the Nowforever VFD. It also lists the different command-line options
and pins which is used with LinuxCNC.

**custom.hal** shows an example on how to create the signals and connect
the pins in LinuxCNC. The **custom.hal** file can be copied directly into your
LinuxCNC machine config directory.

## Licence
This software is released under the **GPLv2** license. See the file COPYING
for more details.
