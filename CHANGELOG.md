# Clixon Controller Changelog

## 0.3.0
Expected: February 2024

### New features

* New: Added template (formal) variables which includes cli autocompletion and sanity checks
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

* In configure mode, reordered apply template/services to:
  * `apply [template|services]`
* New `clixon-controller@2024-01-01.yang` revision
  * Added warning field to transaction
  * Added created-by-service grouping
  * Added service-instance parameter to rpc controller-commit
  * Added ssh-stricthostkey

### Corrected Bugs

* Fixed: [Lock-denied if device connection fails](https://github.com/clicon/clixon-controller/issues/98)
* Fixed: [Device connection errors could be more informative](https://github.com/clicon/clixon-controller/issues/7)
* Fixed: [apply services does not allow dry-run](https://github.com/clicon/clixon-controller/issues/94)
* Fixed: [old device config wrongly pushed back](https://github.com/clicon/clixon-controller/issues/93)
* Fixed: [services reapply does not allow specific service instance](https://github.com/clicon/clixon-controller/issues/80)
* Fixed: [Device pull from configure only sync running](https://github.com/clicon/clixon-controller/issues/91)
* Fixed: [Notification is not sent when a service parameter is changed](https://github.com/clicon/clixon-controller/issues/89)
* Fixed: [Commit/connect transaction may lock datastore with no info or method to break it]([Commit/connect transaction may lock datastore with no info or method to break)
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
