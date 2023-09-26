# Clixon Controller Changelog

## 0.1.0
Expected: October 2023

### API changes on existing protocol/config features

* New `clixon-controller@2023-09-01.yang` revision
  * Added identity services for NETCONF monitoring/transport

### Minor features

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
