#include <errno.h>
#include <libgen.h>
#include <pwd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cligen/cligen.h>
#include <clixon/clixon.h>

/*! Generic cli command to run script with arguments
 *
 * @param[in]  h     Clixon handle
 * @param[in]  cvv   Vector of command variables
 * @param[in]  argv  Vector of arguments, first is script runner, second is script path
 * @retval     0     OK
 * @retval    -1     Error
 */
int cli_command_run(clixon_handle h, cvec *cvv, cvec *argv)
{
    int      pid = 0;
    int      retval = -1;
    int      s = 0;
    int      arg_count = 0;
    int      cvv_count = 0;
    int      i = 0;
    int      status = 0;
    char     *script_path = NULL;
    char     *runner = NULL;
    char     *buf = NULL;
    char     *work_dir = NULL;
    char     *reserve_path = NULL;
    char     **args = NULL;
    size_t   bufsize = 0;
    struct   passwd pw, *pwresult = NULL;


    /* Check parameters */
    if (cvec_len(argv) == 0) {
        clixon_err(OE_PLUGIN, EINVAL, "Can not find argument");
        goto done;
    }

    /* get data */
    arg_count = cvec_len(argv);
    cvv_count = cvec_len(cvv);

    runner = cv_string_get(cvec_i(argv, 0));

    if (arg_count > 1) {
        script_path = cv_string_get(cvec_i(argv, 1));
    }

    if (script_path) {
        reserve_path = strdup(script_path);
        work_dir = dirname(reserve_path);
    }

    bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
    
    if (bufsize == -1) {
        bufsize = 16384;
    }
    
    buf = malloc(bufsize);
    
    if (buf == NULL) {
        perror("malloc");
        goto done;
    }

    s = getpwuid_r(getuid(), &pw, buf, bufsize, &pwresult);

    if (pwresult == NULL) {
        if (s == 0)
            clixon_err(OE_PLUGIN, errno, "getpwuid_r");
        else
            perror("getpwuid_r");
        goto done;
    }

    /* Prepare arguments for execlp */
    args = malloc((arg_count + cvv_count) * sizeof(char *));

    if (args == NULL) {
        perror("malloc");
        goto done;
    }

    for (i = 0; i < arg_count; i++) {
        args[i] = cv_string_get(cvec_i(argv, i));
    }

    for (i = 0; i < cvv_count; i++) {
        args[arg_count + i] = cv_string_get(cvec_i(cvv, i + 1));
    }

    /* main run */
    if ((pid = fork()) == 0) {

        /* child process */
        if ((work_dir ? chdir(work_dir) : chdir(pw.pw_dir)) < 0) {
            clixon_err(OE_PLUGIN, errno, "chdir");
        }

        execvp(runner, args);
        clixon_err(OE_PLUGIN, errno, "Error running script");

        goto done;
    } else if(pid == -1) {
        clixon_err(OE_PLUGIN, errno, "fork");
    } else {
        /* parent process */
        if (waitpid(pid, &status, 0) != pid ){
            clixon_err(OE_PLUGIN, errno, "waitpid error");
            goto done;
        } else {
            retval = WEXITSTATUS(status);
            goto done;
        }
    }

done:
    if (buf)
        free(buf);
    if (reserve_path)
        free(reserve_path);
    if (args)
      free(args);
    return retval;
}

/*! Called when plugin is loaded.
  * @param[in] h    Clixon handle
  * @retval    0    OK
  * @retval   -1    Error
*/
int controller_cli_start(clixon_handle h)
{
  return 0;
}

/*! Called just before plugin unloaded.
 *
 * @param[in] h    Clixon handle
 * @retval    0    OK
 * @retval   -1    Error
 */
int controller_cli_exit(clixon_handle h)
{
  return 0;
}

static clixon_plugin_api api = {
    "controller_test",
    clixon_plugin_init,
    controller_cli_start,
    controller_cli_exit,
};

/*! CLI plugin initialization
 *
 * @param[in]  h    Clixon handle
 * @retval     api  Pointer to API struct
 */
clixon_plugin_api *clixon_plugin_init(clixon_handle h)
{
    return &api;
}