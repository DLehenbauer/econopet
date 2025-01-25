#!/bin/bash

# Note: You must enable 'Generate SPI Raw Binary Configuration File' under ~File ~Edit Project ~Bitstream Generation.
cat ../../gw/EconoPET/outflow/EconoPET.hex.bin | xxd -i > bitstream.h
ls -l bitstream.h
