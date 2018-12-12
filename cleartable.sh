#!/bin/bash
echo "#################### Show Table #################### "
sudo iptables -L -n
sudo iptables -t nat -L -n
sudo iptables -t mangle -L -n

sudo ebtables -L
sudo ebtables -t nat -L
sudo ebtables -t broute -L



echo "  "
echo "  "
echo "  "
echo "#################### Clear Table #################### "

sudo iptables -F
sudo iptables -X
sudo iptables -Z

sudo iptables -t nat -F
sudo iptables -t nat -X
sudo iptables -t nat -Z

sudo iptables -t mangle -F
sudo iptables -t mangle -X
sudo iptables -t mangle -Z

