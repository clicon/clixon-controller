# Clixon Controller Changelog

## 0.2.0
Expected: December 2023

### Minor features

* Added new configuration feature: device-class

### API changes on existing protocol/config features

* New `clixon-controller@2023-11-01.yang` revision
  * Added Added device-class list
  
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
