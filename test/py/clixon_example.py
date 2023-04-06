from clixon.clixon import rpc


@rpc()
def setup(root, log):
    for device in root.devices.device:
        for service in root.services.test:
            parameter = service.parameter
            device.root.table.add(parameter)


if __name__ == "__main__":
    setup()
