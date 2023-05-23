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

## How do I configure JunOS and the Clixon controller?

JunOS must be configured with SSH-keys and a few other settings before being used with Clixon. The SSH-key belongs to the user which clixon_backend run as. We must also configure the rfc-compliant option for the netconf server:

```
root@junos> show configuration
## Last commit: 2023-05-22 13:04:40 UTC by admin
version 20220909.043510_builder.r1282894;
system {
    login {
        user admin {
            uid 2000;
            class super-user;
            authentication {
                ssh-rsa "ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABgQDF7BB+hkfqtLiwSvPNte72vQSzeF/KRtAEQywJtqrBAiRJBalZ30EyTMwXydPROHI5VBcm6hN28N89HtEBmKcrg8kU7qVLmmrBOJKYWI1aAWTgfwrPbnSOuo4sRu/jUClSHryOidEtUoh+SJ30X1yvm+S2rP0TM8W5URk0KqLvr4c/m1ejePhpg4BElicFwG6ogZYRWPAJZcygXkGil6N2SMJqFuPYC+IWnyh1l9t3C1wg3j1ldcbvagKSp1sH8zywPCfvly14qIHn814Y1ojgI+z27/TG2Y+svfQaRs6uLbCxy98+BMo2OqFQ1qSkzS5CyEis5tdZR3WW917aaEOJvxs5VVIXXb5RuB925z/rM/DwlSXKzefAzpj0hsrY365Gcm0mt/YfRv0hVAa0dOJloYnZwy7ZxcQKaEpDarPLlXhcb13oEGVFj0iQjAgdXpECk40MFXe//EAJyf4sChOoZyd6MNBlSTTOSLyM4vorEnmzFl1WeJze5bERFOsHjUM="; ## SECRET-DATA
            }
        }
    }
    services {
        ssh;
        netconf {
            ssh;
            rfc-compliant;
        }
    }
}
```

## How do I add a device in Clixon?

The device should be configured to use the same user as in the configuration above. 

```
set devices device test enabled true
set devices device test conn-type NETCONF_SSH
set devices device test user admin
set devices device test addr 1.2.3.4
