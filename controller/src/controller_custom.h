/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2020-2022 Olof Hagsand and Rubicon Communications, LLC(Netgate)

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, indicate
  your decision by deleting the provisions above and replace them with the 
  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

  Custom file included by controller code
  These are compile-time options. 
  In general they are kludges and "should be removed" when code is improved
  and not proper system config options.
  */

#ifndef _CONTROLLER_CUSTOM_H
#define _CONTROLLER_CUSTOM_H

/*! Dump yang retirieved by get-schema to file
 * Debug option that dumps all YANG files retrieved by the get-scema RPC 
 * The option if set is a format string with two parameters: device and
 * YANG module names
 * Example: "/var/tmp/%s/yang/%s.yang"
 * 
 */
#undef CONTROLLER_DUMP_YANG_FILE // "/var/tmp/%s/yang/%s.yang"

/*! Workaround to get which Yang files a junos release uses
 * The option points to dir where files named as Junos release (eg 20.4R3-S2.6)
 * include a list of YANG module names
 * The controller should really get the YANGs by ietf-yang-library or
 * ietf-netconf-monitoring but I dont see them implemented by Junos
 */
#undef CONTROLLER_JUNOS_YANGS // "/var/tmp/junos/yangs"

#endif /* _CONTROLLER_CUSTOM_H */
