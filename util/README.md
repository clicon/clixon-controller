# Clixon controller utils

This directory contains utility programs, ie, programs that are
good to have for testing, analysis, etc, but not an actual part of
delivered code.

Example, get a config from a device using ssh subsystem and hello protocol:
```
controller_get_config -f /var/tmp/clixon_controller/conf_yang.xml -d root@10.0.20.3
```
