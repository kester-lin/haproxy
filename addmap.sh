#!/bin/bash
echo "add map ./map 192.168.126.212 dynamic_group" | sudo socat stdio /var/run/haproxy.sock
