# nowforever-vfd
Nowforever D100 and E100 VFD LinuxCNC HAL userspace interface / driver, using
RS485 MODBUS RTU

## Build and install
I'm assuming you already have set up LinuxCNC, if not, go to their website
<https://linuxcnc.org> to get started.

To download and build the driver, we need some additional packages:

git - download the repository  
libmodbus-dev - development files for the Modbus connection  
linuxcnc-uspace-dev - development files for LinuxCNC

```
$ sudo apt-get install libmodbus-dev linuxcnc-uspace-dev git
$ git clone https://github.com/havardAasen/nowforever_vfd.git
$ cd nowforever_vfd
$ make
$ sudo make install
```

## Documentation and usage
The man-page `nowforever_vfd` describes the parameter to adjust on the
Nowforever VFD. It also lists the different command-line options if you
need to customize anything regarding Modbus The man-page also lists the
pins and signals which is used with LinuxCNC.

`custom.hal` is an example on how to create the signals and connect
the pins to LinuxCNC.

## Testing
If you want to test the VFD, you can use one of the sample configurations
that comes shipped with LinuxCNC.

- Execute the steps in **Build and install**, ending with installing
  the binaries.
- Open LinuxCNC and choose one of the sample configurations.
  `Sample configuration -> sim -> axis`
  choose one of `axis`, `axis_9axis` or `axis_mm`.
- Say yes to copy the files to your home folder.
- Exit LinuxCNC
- Copy the `custom.hal` file, from the repository into the newly created
  configuration folder
- In the configuration folder, edit the axis*.ini file you wish to use.
  It's three of these, but you only need to use one of them.
- Go to `HAL` section and comment out `HALFILE sim_spindle_encoder.hal`.
- Continuing in the `HAL` section, add `HALFILE custom.hal` as the last entry.

