#!/bin/sh
# Lets the Nexus test driver into a Linux test VM: adds its public key and starts the SSH server.
#   curl -fsSL https://raw.githubusercontent.com/limburatorul/nexus/main/tests/setup-vm.sh | sh
set -e
key='ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIJEgXqFjoViro5Zg6FOkC7i2v4qUoKpUNuCbl8hMiwaG claude-nexus-tests'
mkdir -p ~/.ssh && chmod 700 ~/.ssh
grep -qF "$key" ~/.ssh/authorized_keys 2>/dev/null || echo "$key" >> ~/.ssh/authorized_keys
chmod 600 ~/.ssh/authorized_keys
if command -v pacman >/dev/null; then
    sudo pacman -S --needed --noconfirm openssh && sudo systemctl enable --now sshd
else
    sudo apt-get install -y openssh-server && sudo systemctl enable --now ssh
fi
ip -4 -br addr
echo NEXUS-SSH-READY
