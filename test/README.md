Clixon controller test directory

Base tests: 
start-devices.sh      Start clixon-test container devices
stop-devices.sh       Stop devices
init-controller.sh    Init controller config

Composite tests: (start containers and backend+)
change-ctrl-push.sh   Change device config on controller and push to devices
change-device-diff.sh Changeconfig on device and check diff

Modifiers:
n=<nr>                Apply on <nr> devices (all)
BE=0                  Do not start backend in script, start backend externally instead,
                      ie in a debugger (init-controller.sh only)
push=false            Only change dont sync push (change-push.sh only)
sleep=<s>             Sleep <s> seconds instead of 2 (all)