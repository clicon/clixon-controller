# Junos native plugin

Backend plugin for Junos native for NON rfc- and yang-compliant
Juniper PTX,MX and QFX (possibly others). The plugin rewrites
the XML config on pull/sync and push/commit.

No configuration is necessary.

The extension allocates three device flags to keep track of whether the device is a
JunOS device, a QFX JunOS device and if the check has been made or not.

It then intercepts incoming NETCONF/XML messages from the device,
detects if it is a JusOS device in need of rewrite by examining its
system compliance settings, and then potentially rewrites the XML to RFC
and YANG compliant.

If flags are set appropriately, the extension also intercepts outgoing
NETCONF/XML messages from RFC/YANG compliant to Junos non-compliant
native mode.

The interception uses the user-defined callback which in the controller is placed in
the path of incoming and outgoing NETCONF messages.
