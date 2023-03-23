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

  Common functions, for backend, cli, etc
  */

#ifndef _CONTROLLER_LIB_H
#define _CONTROLLER_LIB_H

/*! Transaction state
 * @see clixon-controller@2023-01-01.yang transaction-state
 * @see tsmap translation table
 */
enum transaction_state_t{
    TS_INIT = 0,  /* Started transaction */
    TS_RESOLVED,  /* The result of the transaction is set (if result == 0, this is same as CLOSED) */
    TS_DONE,      /* Terminated, inactive transaction */
};
typedef enum transaction_state_t transaction_state;

/*! Transaction result
 * @see clixon-controller@2023-01-01.yang transaction-result
 * @see trmap translation table
 */
enum transaction_result_t{
    TR_ERROR = 0,  /* Transaction failed in an inconsistent state, not recoverable */
    TR_FAILED,     /* Transaction failed but reverted successfully */
    TR_SUCCESS,    /* Transaction completed successfully */
};
typedef enum transaction_result_t transaction_result;

/*
 * Prototypes
 */
#ifdef __cplusplus
extern "C" {
#endif
    
char *transaction_state_int2str(transaction_state state);
transaction_state transaction_state_str2int(char *str);
char *transaction_result_int2str(transaction_result result);
transaction_result transaction_result_str2int(char *str);

int yang_lib2yspec_junos_patch(clicon_handle h, cxobj *yanglib, yang_stmt *yspec);
    
#ifdef __cplusplus
}
#endif

#endif /* _CONTROLLER_LIB_H */
