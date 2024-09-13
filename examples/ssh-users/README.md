Simple example of a service. The service consists of two files:
- `ssh-users@2023-05-22.yang` - the YANG model
- `ssh_users.py` - the service implementation

The YANG model should be copied to
`/usr/local/share/controller/main/`and the service implementation
should be copied to `/usr/local/share/controller/modules/`.

When the files have been copied the backend should be restarted and
the service can then be configured using the CLI:

```
user@test> configure
user@test# set services ssh-users test user test
user@test# set services ssh-users test class admin
user@test# set services ssh-users test ssh-key "ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABAQDQ7"
user@test# commit diff
```


