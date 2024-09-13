from clixon.parser import parse_template
from clixon.helpers import get_service_instance

SERVICE = "ssh-users"

# The XML template for the new user
USER_XML = """
<user cl:creator="ssh-users[instance='{{INSTANCE_NAME}}']" nc:operation="merge" xmlns:cl="http://clicon.org/lib">
  <username>{{USERNAME}}</username>
    <config>
      <username>{{USERNAME}}</username>
        <ssh-key>{{SSH_KEY}}</ssh-key>
        <role>{{ROLE}}</role>
    </config>
</user>
"""
def setup(root, log, **kwargs):
    # Check if the service is configured
    try:
        _ = root.services.ssh_users
    except Exception:
        return

    root.services.create("ssh-users", attributes={"xmlns":"http://clicon.org/ssh-users"}).create("instance", data="bajs")

    # Get the service instance
    instance = get_service_instance(root,
                                    SERVICE,
				    instance=kwargs["instance"])

    # Check if the instance is the one we are looking for
    if not instance:
        return

    # Iterate all users in the instance
    for user in instance.username:

	# Get the data from the user
        instance_name = instance.instance.get_data()
        username = user.name.get_data()
        ssh_key = user.ssh_key.get_data()
        role = user.role.get_data()

	# Create the XML for the new user
        new_user = parse_template(USER_XML,
				  INSTANCE_NAME=instance_name,
				  USERNAME=username,
				  SSH_KEY=ssh_key,
				  ROLE=role).user

	# Add the new user to all devices
        for device in root.devices.device:
	    # Check if the device has the system element
            if not device.config.get_elements("system"):
                device.config.create("system",
                                     attributes={"xmlns": "http://openconfig.net/yang/system"})

	    # Check if the device has the aaa element
            if not device.config.system.get_elements("aaa"):
                device.config.system.create("aaa")

	    # Check if the device has the authentication element
            if not device.config.system.aaa.get_elements("authentication"):
                device.config.system.aaa.create("authentication")

	    # Check if the device has the users element
            if not device.config.system.aaa.authentication.get_elements("users"):
                device.config.system.aaa.authentication.create("users")

	    # Add the new user to the device
            device.config.system.aaa.authentication.users.add(new_user)

