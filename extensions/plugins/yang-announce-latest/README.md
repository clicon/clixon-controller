# YANG announce latest

Extend device configuration with option to select among multiple
announced yangs. That is, same YANG name but multiple revisions. By
default the latest YANG is selected, but some devices (old junos-qfx)
may announce a later yang which is broken and one may want select the
older.

Note that support for device-profiles is not implemented, only single devices, when setting the flag.
That is, reconnecting the device will work, set or reset of the device-profile flag when connected does not, only device flag.

## Build and install

```bash
cd yang-announce-latest
make
make install
```
Note that the YANG is installed in the main load directory since it affects the controller top-level yang, not device yangs.

## Configure

Change value of yang-announce-latest in the CLI:
```
    cli´# set devices device openconfig1 yang-announce-latest false
    cli´# commit local
```
