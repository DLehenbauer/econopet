# Host

On Windows 10/11, you can expose a picoprobe to a docker container using usbipd-win:

```ps
winget install --interactive --exact dorssel.usbipd-win
```

To get the serial port, you may need to `sudo modprobe cdc_acm` on Ubuntu.
