#!/bin/sh
sudo modprobe xt_TPROXY
sudo modprobe br_netfilter
sudo modprobe xt_socket

sudo ./cleartable.sh

#/sbin/iptables -N SILENCE_INPUT_LOG
#/sbin/iptables -I PREROUTING 1 -j SILENCE_INPUT_LOG
#/sbin/iptables -A SILENCE_INPUT_LOG -p tcp --dport 1536 -j LOG --log-prefix "iptables:"


/sbin/iptables -t mangle -N DIVERT
#/sbin/iptables -t mangle -A PREROUTING -p tcp -m socket --nowildcard -j DIVERT 
/sbin/iptables -t mangle -A PREROUTING -p tcp -m socket --nowildcard --transparent -j DIVERT 
/sbin/iptables -t mangle -A DIVERT -j MARK --set-mark 1
/sbin/iptables -t mangle -A DIVERT -j ACCEPT
/sbin/ip rule add fwmark 1 lookup 100
/sbin/ip route add local 0.0.0.0/0 dev lo table 100
