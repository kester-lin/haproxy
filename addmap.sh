#!/bin/bash
echo "add map ./map 192.168.126.212 ft_group" | sudo socat stdio /var/run/haproxy.sock
echo "add map ./map 192.168.126.222 ft_group" | sudo socat stdio /var/run/haproxy.sock
echo "add map ./map 192.168.126.232 ft_group" | sudo socat stdio /var/run/haproxy.sock

echo "add map ./map 192.168.128.31 frontend_group" | sudo socat stdio /var/run/haproxy.sock
##echo "add map ./map 192.168.128.31 ft_group" | sudo socat stdio /var/run/haproxy.sock
