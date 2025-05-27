# Older 'interface/picoprobe.cfg' is 2e8a:0004
# Newer 'interface/csmis-dap.cfp' is 2e8a:000c
sudo usbipd bind --hardware-id 2e8a:000c
usbipd attach --auto-attach --hardware-id 2e8a:000c --wsl
