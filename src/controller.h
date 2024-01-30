/*
 *
  ***** BEGIN LICENSE BLOCK *****

  Copyright (C) 2023 Olof Hagsand

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

  * Common controller and custom definitions
  */

#ifndef _CONTROLLER_H
#define _CONTROLLER_H

/*
 * Constants
 */
#define CONTROLLER_PREFIX    "ctrl"
#define CONTROLLER_NAMESPACE "http://clicon.org/controller"

/*! Skip junos-configuration-metadata.yang
 *
 * cRPD gives error if you request it with get-schema:
 * <error-message>invalid schema identifier : junos-configuration-metadata</error-message>
 */
#define CONTROLLER_JUNOS_SKIP_METADATA

/*! Add grouping command-forwarding in junos-rpc yangs if not exists
 *
 * cRPD YANGs do not have groupimg command-grouping in junos-rpc YANGs so that
 * uses command-grouping fails.
 * Insert an empty grouping command-forwarding if it does not exist
 */
#define CONTROLLER_JUNOS_ADD_COMMAND_FORWARDING

/*! Top-symbol in clixon datastores
 *
 * Duplicate of constant in clixon_custom.h
 */
#define DATASTORE_TOP_SYMBOL "config"

/*! Add extra sync pull state after commit
 *
 * This may be necessary if device changes from config from the one the controller has actually
 * pushed, see https://github.com/clicon/clixon-controller/issues/6
 * Alternatively, filter some fields not used so often
 */
#undef CONTROLLER_EXTRA_PUSH_SYNC

/*! Enable to share same yang-spec structure for all devices
 */
#define SHARED_PROFILE_YSPEC

#define ACTION_PROCESS "Action process"

#endif /* _CONTROLLER_H */
