# Clixon Controller Changelog

## 1.7.0
Planned: February 2026

### New features

* Added transaction garbage-collect
  * Keep only a limited nr of transactions structures (default 100)
  * Remove "devices" struct used for device rpc and state replies after timeout (default 300s) 
* New `clixon-controller@2025-12-01.yang` revision
  * Added `service-timeout`

### Corrected Bugs

* Fixed: [Some devices announce duplicate modules leading to controller error](https://github.com/clicon/clixon-controller/issues/242)
* Fixed: [device rpc hangs in notification when devices return large amount of data](https://github.com/clicon/clixon-controller/issues/237)
* Fixed: [Candidate datastore lock prevents sequential commit operations via RESTCONF](https://github.com/clicon/clixon-controller/issues/236)
* Fixed: [Memory leak after reopening connection](https://github.com/clicon/clixon-controller/issues/169)

## 1.6.0
21 November 2025

### New features

* New: [Configure and adapt to device netconf encapsulation 1.0 or 1.1](https://github.com/clicon/clixon-controller/issues/225)
* Improved NETCONF error messages on locked datastore
* New `clixon-controller@2025-08-01.yang` revision
  * Added the following fields to device-common:
    * private-candidate
* New `clixon-controller@2025-05-01.yang` revision
  * Added the following fields to device-common:
    * private-candidate
    * netconf-framing
    * netconf-state-schemas

### Corrected Bugs

* Fixed: Allow retrieving modules with empty revisions
* Fixed: [Memory leak after reopening connection](https://github.com/clicon/clixon-controller/issues/169)
* Fixed: [commit on multiple devices with one device with not pulled local commit drops connection to subset of devices](https://github.com/clicon/clixon-controller/issues/223)
* Fixed: Device closes when device out-of-sync (Only > 50 devices)
* Fixed: set glob skip disabled devices correctly and warn of different YANGs
* Fixed: [get-schema rpc in netconf monitoring: need to decode yang module with CDATA](https://github.com/clicon/clixon-controller/issues/222)

## 1.5.0
29 July 2025

### New features

* New: [RESTCONF service delete does not work](https://github.com/clicon/clixon-controller/issues/199)
   * Add delete service by extending rpc controller-commit with DELETE
* New: [NACM for Clixon Controller](https://github.com/clicon/clixon-controller/issues/189)
  * Support for services
  * Testcases for controller nacm and nacm+restconf
  * Added cli show detail command
* New: [CLI exclusive mode for candidate](https://github.com/clicon/clixon-controller/issues/92)
  * Set `CLICON_AUTOLOCK=true`
* New: [new command "pull diff"](https://github.com/clicon/clixon-controller/issues/194)
  * Added `pull <device> diff | check`
* New: [ssh keep-alive](https://github.com/clicon/clixon-controller/issues/193)
  * Hard-coded to 300s
* New `clixon-controller@2025-05-01.yang` revision
  * Added rpc `device-rpc`

### API changes on existing protocol/config features

* New `clixon-controller-config@2025-05-01.yang` revision

### Corrected Bugs

* Fixed: [Candidate CLICON_AUTOLOCK not automatically removed](https://github.com/clicon/clixon-controller/issues/214)
* Fixed: [RESTCONF PUT/POST data does sometimes not work across mountpoint](https://github.com/clicon/clixon-controller/issues/210)
* Fixed: [Controller diff does not check for NACM read rules](https://github.com/clicon/clixon-controller/issues/207)
* Fixed: [Controller commit RPC does not check locks](https://github.com/clicon/clixon-controller/issues/206)
* Fixed: [Syntax errror when deleting leafs on Junos](https://github.com/clicon/clixon-controller/issues/203)
* Fixed: [Confusing error message if device has NACM](https://github.com/clicon/clixon-controller/issues/197)
* Fixed: [Improve error message when creator tag is malformed](https://github.com/clicon/clixon-controller/issues/191)

## 1.4.0
3 April 2025

The controller 1.4 introduces RESTCONF support and a new extensions architecture.

### New features

* Controller RESTCONF support
  * See: [Controller RESTCONF access not properly tested and documented](https://github.com/clicon/clixon-controller/issues/167)
* New extension plugin dir with ability to add device-specific plugins and YANGs
  * First junos-native plugin for non-rfc-compliant devices
* Check if part of client-rpc, if so send notifications asynchronously
* Device handle flags and dynamic mechanism to allocate flags

### API changes on existing protocol/config features

* New `clixon-controller-config@2025-02-01.yang` revision
  * Added `CONTROLLER_SSH_IDENTITYFILE`
* New `clixon-controller@2025-02-01.yang` revision
  * Added restconf
  * Added common fields to transaction state and notification

## 1.3.0
30 January 2025

The controller 1.3 release features device RPCs, device-groups, and show device state as well as cycle optimization.

### New features

* New: [device-groups to be first level objects](https://github.com/clicon/clixon-controller/issues/153)
  * Use device-groups when connecting, pull, push and show
* Optimization [Performance: commit takes long time with many devices](https://github.com/clicon/clixon-controller/issues/154)
  * Avoid copy in compare/diff
  * Optimized strip service at commit
* Show device state
  * New CLI show device state command
  * See [CLI show state does not return device state (config false) data](https://github.com/clicon/clixon-controller/issues/143)
* Execute arbitrary device RPCs using templates
  * see: [Execute arbitrary RPC commands](https://github.com/clicon/clixon-controller/issues/45)
  * docs: https://clixon-controller-docs.readthedocs.io/en/latest/cli.html#rpc-templates

### API changes on existing protocol/config features

* New `clixon-controller-config@2024-11-01.yang` revision
  * Changed RPC input parameter parameter name `devname` -> `device`:
    * `config-pull`
    * `controller-commit`
    * `connection-change`
    * `get-device-config`
    * `datastore-diff`
    * `device-template-apply`
    * This applies to any "raw" NETCONF RPC:s, CLI code is updated
  * Changed default device-common/port to 830
  * Added hide-show extension to created container
  * Added device-generic-rpc

### Corrected Bugs

* Fixed: [Not possible to remove service if "ctrl:created-by-service" is located in the top of the YANG specification](https://github.com/clicon/clixon-controller/issues/179)
* Fixed: [Running undefined RPC towards a device closes connection](https://github.com/clicon/clixon-controller/issues/173)
* Fixed: [RPC commit diff problem](https://github.com/clicon/clixon-controller/issues/171)
* Fixed: [CLI: default format text | display](https://github.com/clicon/clixon-controller/issues/165)
* Fixed: [CLI: Don't show <created>...</created> tags by default](https://github.com/clicon/clixon-controller/issues/110)
* Fixed: [YANG domains: issues if devices in different domains have identical schemas](https://github.com/clicon/clixon-controller/issues/170)
* Fixed: [Generalize default autocli.xml file](https://github.com/clicon/clixon-controller/issues/163)
* Fixed: [Commit without changes results in weird diff](https://github.com/clicon/clixon-controller/issues/161)

## 1.2.0
28 October 2024

### New features

* Added configurable port for NETCONF over SSH
  * See [make netconf ssh port to devices configurable](https://github.com/clicon/clixon-controller/issues/152)
* Added yang domains for mount-point isolation
  * See [Support isolated YANG domains](https://github.com/clicon/clixon-controller/issues/134)
* New CLI commands:
  * show device yang
  * show device capability

### API changes on existing protocol/config features

* New version string on the form: `1.1.0-1+70+gbae59f2`
* Edits to device addr, user, domain, etc causes device disconnect
* Removed `--with yang-installdir` from configure
  * Use `DATADIR` instead
* New `clixon-controller@2024-08-01.yang` revision
  * Added `device-domains`
  * Added `port` to device-common
* New `clixon-controller-config@2024-08-01.yang` revision
  * Removed defaults for:
    * `CONTROLLER_ACTION_COMMAND`
    * `CONTROLLER_PYAPI_MODULE_PATH`
    * `CONTROLLER_PYAPI_PIDFILE`
  * Obsoleted `CONTROLLER_YANG_SCHEMA_MOUNT_DIR`
* Re-arranged directories
  * Put Controller YANG under `$DATADIR/controller`, typically /usr/local/share/controller.
  * Removed `YANG_INSTALLDIR`
  * Mountpoint dirs moved to sub-levels under `/usr/local/share/controller/mounts`
    * Example: `/usr/local/share/controller/mounts/default`
    * For isolated domains
  * Put controller YANG root in `/usr/local/share/controller`
    * Moved from `/usr/local/share/clixon/controller`

### Corrected Bugs

* Fixed: [Change default NETCONF ssh port to 830](https://github.com/clicon/clixon-controller/issues/164)
* Fixed: [CLI bug w/ device domains on juniper qfx devices](https://github.com/clicon/clixon-controller/issues/145)
* Fixed: [Controller doesn't properly identify ietf-netconf-monitoring in hello statement on FS switch](https://github.com/clicon/clixon-controller/issues/137)
* Fixed: commit fail in transaction caused assertion

## 1.1.0

3 July 2024

### New features

* Added CLI command logging
* Added split config data into multi-datastore
* Added priority for north-bound sockets
* Added session kill
* CLI configurable format: [Default format should be configurable](https://github.com/clicon/clixon-controller/issues/87)

### API changes on existing protocol/config features

* Rearranged CLI `connection` command and made it blocking.

### Corrected Bugs

* Fixed: [When changing configuration for devices which are in state closed we get an error message](https://github.com/clicon/clixon-controller/issues/116)
* Fixed: ["show devices" hangs if SSH-process to any device is killed](https://github.com/clicon/clixon-controller/issues/121)
* Fixed: CLI load merge from stdin only worked once
* Fixed: [default format does not work in configure mode](https://github.com/clicon/clixon-controller/issues/118)
  But must be default format xml for pipe display to work
* Fixed: [connection open does local commit on services](https://github.com/clicon/clixon-controller/issues/119)
* Fixed: [CLI: Can't change configuration on Juniper QFX devices](https://github.com/clicon/clixon-controller/issues/109)
* Fixed: [Commit to device(s) in OPEN state even if we have other devices in CLOSED state](https://github.com/clicon/clixon-controller/issues/95)

## 1.0.0
12 March 2024

Clixon controller 1.0 is the first major release.

### New features

* Added badges for readthedocs, codecov and coverity
* New: [show version(s)](https://github.com/clicon/clixon-controller/issues/105)
* New: Added template (formal) variables which includes CLI autocompletion and sanity checks
  * See [CLI should allow autocomplete of template variables](https://github.com/clicon/clixon-controller/issues/100)
* New: [SSH StrictHostKeyChecking optional](https://github.com/clicon/clixon-controller/issues/96)
* New: [Exclusive candidate configuration](https://github.com/clicon/clixon-controller/issues/37)
* Replaced creator attributes with a configured solution
* New: [show creator paths associated to service instance](https://github.com/clicon/clixon-controller/issues/90)
  * See https://clixon-controller-docs.readthedocs.io/en/latest/cli.html#creators
* New CLI commands:
  * `show sessions [detail]`
  * `unlock <datastore>` [Add unlock CLI command](https://github.com/clicon/clixon-controller/issues/81)
* Optimization
  * [YANG memory-footprint optimization](https://github.com/clicon/clixon-controller/issues/61)
    * Optimize memory by sharing yspec between devices
  * [Performance issues with CLI tab-completion](https://github.com/clicon/clixon-controller/issues/75)
    * Optimization of `cligen_treeref_wrap`
    * Dont read device config data when read device state
    * Also in clixon:
      * Optimization of `yang_find`
      * Added mountpoint cache as yang flag `YANG_FLAG_MTPOINT_POTENTIAL`

### API changes on existing protocol/config features

* Debug level `-D 1` changed to `-D app`.
  * -D msg (-D 2) still applies for message debugging
* CLI apply service syntax changed:
  * From: `apply services <path>` to: `apply services <service> <instance>`
  * Example: `apply services testA[name='foo']` to: `apply services testA foo`
* In configure mode, reordered apply template/services to:
  * `apply [template|services]`
* New `clixon-controller@2024-01-01.yang` revision
  * Added warning field to transaction
  * Added created-by-service grouping
  * Added service-instance parameter to rpc controller-commit
  * Added ssh-stricthostkey

### Corrected Bugs

* Fixed: [CLI expand of apply services instance](https://github.com/clicon/clixon-controller/issues/106)
* Fixed: [Syntax errors must be shown in CLI](https://github.com/clicon/clixon-controller/issues/103)
* Fixed: [Lock-denied if device connection fails](https://github.com/clicon/clixon-controller/issues/98)
* Fixed: [Device connection errors could be more informative](https://github.com/clicon/clixon-controller/issues/7)
* Fixed: [apply services does not allow dry-run](https://github.com/clicon/clixon-controller/issues/94)
* Fixed: [old device config wrongly pushed back](https://github.com/clicon/clixon-controller/issues/93)
* Fixed: [services reapply does not allow specific service instance](https://github.com/clicon/clixon-controller/issues/80)
* Fixed: [Device pull from configure only sync running](https://github.com/clicon/clixon-controller/issues/91)
* Fixed: [Notification is not sent when a service parameter is changed](https://github.com/clicon/clixon-controller/issues/89)
* Fixed: [Commit/connect transaction may lock datastore with no info or method to break it](https://github.com/clicon/clixon-controller/issues/84)
* Fixed: [pull config from device also does commit local config](https://github.com/clicon/clixon-controller/issues/82)
* Fixed: [Commit don't push configuration if service is configured](https://github.com/clicon/clixon-controller/issues/78)
* Fixed: ["commit diff" after applying a service always seems to show a diff](https://github.com/clicon/clixon-controller/issues/70)
* Fixed: [Backend may exit with assertion if the device return warnings](https://github.com/clicon/clixon-controller/issues/77)
* Fixed: [apply template with multiple variables do not work in some cases](https://github.com/clicon/clixon-controller/issues/74)

## 0.2.0
6 December 2023

The clixon controller 0.2.0 release features device templates, local
yang modules, simple templates and many bugfixes.
Based on CLIgen/Clixon 6.5.0.

### New features

* [Templates](https://github.com/clicon/clixon-controller/issues/4)
  * First version uses simple variable substitution in XML bodies
  * Template is YANG anydata which means CLI editing is not possible, instead needs XML input
* CLI: Load raw XML/JSON from stdin
* [Add device / profile ignore fields](https://github.com/clicon/clixon-controller/issues/55)
* [Support a loadable mechanism for device profiles](https://github.com/clicon/clixon-controller/issues/21)
  * The mechanism loads a set of configured modules locally from disk,
  * Instead of using RFC 6022 NETCONF monitoring get-schema method to devices
  * Modified the controller yang with device-profile and device-common

### API changes on existing protocol/config features

* New `clixon-controller-config@2023-11-01.yang` revision
  * Added CONTROLLER_YANG_SCHEMA_MOUNT_DIR
* New `clixon-controller@2023-11-01.yang` revision
  * Added device-profile list
  * Added device-common grouping, for common device/device-profile fields
  * Added module-set to device-common
  * Added ignore-compare extension

### Corrected Bugs

* Fixed: [Unable to commit to device with warnings](https://github.com/clicon/clixon-controller/issues/71)
* Fixed: [cl:creator tags should survive a backend restart](https://github.com/clicon/clixon-controller/issues/66)
* Fixed: ["show compare devices diff" should display device name](https://github.com/clicon/clixon-controller/issues/64)
* Fixed: [Device configuration is removed when starting from running](https://github.com/clicon/clixon-controller/issues/65)
* Fixed: ["validate push" etc should only use the connected devices](https://github.com/clicon/clixon-controller/issues/58)
* Fixed: [Command 'show compare' broken](https://github.com/clicon/clixon-controller/issues/46)
* Fixed: [Possible to get stuck in an lock-denied state with no way to recover](https://github.com/clicon/clixon-controller/issues/47)
* Fixed: [NETCONF delete messages include extra XML](https://github.com/clicon/clixon-controller/issues/53)
* Fixed: [Validation errors not shown in CLI](https://github.com/clicon/clixon-controller/issues/48)

## 0.1.0
30 September 2023

Based on CLIgen/Clixon 6.4.0.
This release focuses on testing with openconfig, arista and juniper
routers. It is still experimental.

### API changes on existing protocol/config features

* New `clixon-controller@2023-09-01.yang` revision
  * Added identity services for NETCONF monitoring/transport

### Minor features

* Added backend memory tests and checks for file descriptor leaks
* Changed and extended CI (github actions) with openconfig devices
* Checked and fixed memeory leaks
* Added: [Add show command for processes ](https://github.com/clicon/clixon-controller/issues/42)

### Corrected Bugs

* Fixed: [Backend exited after XML error](https://github.com/clicon/clixon-controller/issues/49)
* Fixed: [Tab completion with configuration for multiple vendors](https://github.com/clicon/clixon-controller/issues/40)
* Fixed: [OpenConfig: Error when validating/committing](https://github.com/clicon/clixon-controller/issues/32)
* Fixed: [XML parse error](https://github.com/clicon/clixon-controller/issues/36)
  * Changed show output to start from root
* Fixed: ["show devices <device> detail" returns data for all devices](https://github.com/clicon/clixon-controller/issues/27)
* Fixed: [Abort transaction if more one client](https://github.com/clicon/clixon-controller/issues/35)
* Fixed: [RPC error when using pipe](https://github.com/clicon/clixon-controller/issues/34)
* Fixed: ['show devices | display cli' throws an error](https://github.com/clicon/clixon-controller/issues/25)
* Fixed: [Weird things happen when piping set command](https://github.com/clicon/clixon-controller/issues/33)

## 0.0.0
14 August 2023
First prototype version. Based on CLIgen 6.3.0 and Clixon 6.3.1
