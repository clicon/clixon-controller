# Clixon Controller FAQ

  * [What is the Clixon controller?](#what-is-the-clixon-controller)
  * [How does it differ from Clixon?](#how-does-it-differ-from-clixon)
  * [What about other protocols?](#what-about other protocols)
  * [What about other protocols?](#what-about other protocols)
  * [My devices are stuck in CONNECTING](#my-devices-are-stuck-in-connecting)

## What is the Clixon controller?

The Clixon controller is a network device controller for multiple
NETCONF devices.  Its aim is to provide a simple configuration manager
with a python API and specialized CLI for multiple devices with
different YANG schemas from different providers.

## How does it differ from Clixon?

Clixon itself is mainly used for end-systems, mostly network devices (such as firewalls, routers, switches), in some "embedded" form.
However, Clixon can also be used as a platform for applications where its semantics is implemented by plugins.
The controller is such an application: I.e., it is a Clixon application. All controller semantics is implemented by plugins: backend plugins for communicating with remote devices and CLI plugins for the specialized CLI interface.

## What about other protocols?

The clixon controller only inrefaces with devices using NETCONF, not
other protocols are supported (or CLI).  The controller itself
supports NETCONF, RESTCONF and CLI.

## My devices are stuck in CONNECTING

If a device is stuck in `CONNECTING` state when you do `show devices` in the CLI and you
see messages like the following in syslog/backend logs:
```
  Host key verification failed.
```
It is probable that the controller was unable to login to a device due to some failure with SSH keys.
The controller requires its public key to be installed on the devices and performs strict checking of host keys to avoid man-in-the-middle attacks. You need to ensure that the public key the controller uses is installed on the devices, and that the known_hosts file of the controller contains entries for the devices.
