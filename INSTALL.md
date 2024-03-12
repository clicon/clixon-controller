## Installation

### Install required packages
sudo apt install flex bison git make gcc libnghttp2-dev libssl-dev

### Clone the Git repositories

```console
$ git clone https://github.com/clicon/cligen.git
$ git clone https://github.com/clicon/clixon.git
$ git clone https://github.com/clicon/clixon-controller.git
$ git clone https://github.com/clicon/clixon-pyapi.git
```

### Build the components

Cligen:
```console
cd cligen
./configure
make
sudo make install
```

Clixon:
```console

# Add a new clicon user
sudo useradd -g clicon -m clicon

cd clixon
./configure
make
sudo make install
```

Clixon controller
```console
cd clixon-controller
./configure
make
sudo make install
```

Clixon Python API
```console

# Build and install the package
cd clixon-pyapi
./install.sh
```

### Optional: SystemD service
```console

# Copy clixon_controller.service
cp clixon_controller.service /etc/systemd/network/

# Enable and start the service
systemctl daemon-reload
systemctl enable clixon_controller
systemctl start clixon_controller
