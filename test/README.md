Clixon controller test directory

Prereqs:
- devices run, backend runs (use INIT=true) to start them

Tests:
test-change-ctrl-push.sh   Change device config on controller and push to devices
test-change-device-diff.sh Change config on device and check diff
test-yanglib.sh            Test RFC8528 YANG Schema Mount state

Modifiers:
n=<nr>                Apply on <nr> devices (all)
BE=false              Do not start backend in script, start backend externally instead,
                      ie in a debugger (init-controller.sh only)
push=false            Only change dont sync push (change-push.sh only)
sleep=<s>             Sleep <s> seconds instead of 2 (all)
INIT=false            Only run test, dont start containers/backend (test-*.sh)