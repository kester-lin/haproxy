## HAProxy install and setup 

### requirement 
test on ubuntu 16.04 and kernel version 4.4
```shell
sudo apt install linlua5.3-dev build-essential libssl-dev libsystemd-dev zlib1g-dev libpcre3-dev
sudo apt install libnet1-dev socat
```

### environment 
先使用三台說明
```shell
A<---------->B<------SWITCH Port------>C
                     SWITCH Port------>D
```
A 為client 
B HAProxy預計設定位置
C 為Guest OS(Cuju Primary所在地)
D 為Cuju Backup所在地

B需要至少兩個網段　最容易的方式是用兩張網卡隔開

### build code

```shell
./make.sh
```


###　configure
因主要使用transparent proxy 因此需要有以下相關設定
#### iptables 
已經寫成script 基本上不太需要修改直接執行即可
但是trans.sh中會呼叫cleartable.sh　須注意是有其他iptables rule被清除
```shell
sudo ./trans.sh
``` 
#### HAProxy configure file 
分四段
主要monitor[frontend] 5000-5545與19765等port
然後確認使用的[backend]rule
而backend則為了可動態加入rule將stick-table
stick-table加入的方式則要參考addmap.sh
若不在stick-table中則對應到not_found rule
not_found rule中的white_ip_list留空使其必定回應not found

```shell 
frontend TCP-in
    bind 0.0.0.0:5000-5545 transparent
    bind 0.0.0.0:19765 transparent
    mode             tcp
    log              global
    use_backend	%[dst,map_ip(./map,not_found)]
    bind-process 1


backend ft_group
    stick-table type ip size 5k expire 1m store conn_cnt
    stick on src
    mode    tcp
    source 0.0.0.0 usesrc clientip
    server srv1 *
    bind-process 1


backend frontend_group
    stick-table type ip size 5k expire 1m store conn_cnt
    stick on src
    mode    tcp
    source 0.0.0.0 usesrc clientip
    server srv1 *


backend not_found
    tcp-request content accept if { src -f ./white_ip_list }
    tcp-request content reject
```

###　執行順序
Pre-setting after reboot (just execute once)
A. Cuju bridge and tun/tap setting
B. iptable setting for transparent 

Normal command order 
0. Cuju Primary and Backup script
1. sudo ./haproxy -d -f ha_cfg.map
2. sudo ./addmap.sh
3. FTmode script (Cuju與HAProxy間的IPC會在進FT時建立)
4. execute iperf test

### network setting part A
因為修改過的HAProxy需要大量的fd因此建議修改/etc/sysctl.cnf
```shell
### 系統中所允許的檔案控制代碼的最大數目
##fs.file-max = 12553500
fs.file-max = 100428000
### 單個進程所允許的檔案控制代碼的最大數目
##fs.nr_open = 12453500
fs.nr_open = 100328000
### 內核允許使用的共用記憶體大 Controls the maximum number of shared memory segments, in pages
kernel.shmall = 4294967296
###單個共用記憶體段的最大值 Controls the maximum shared segment size, in bytes
kernel.shmmax = 68719476736
### 內核中訊息佇列中消息的最大值 Controls the maximum size of a message, in bytes
kernel.msgmax = 65536
### 系統救援工具
kernel.sysrq = 0
### 在每個網路介面接收資料包的速率比內核處理這些包的速率快時，允許送到緩存佇列的資料包的最大數目
net.core.netdev_max_backlog = 2000000
### 預設的TCP資料接收視窗大小（位元組）
net.core.rmem_default = 699040
### 最大的TCP資料接收視窗（位元組）
net.core.rmem_max = 50331648
### 預設的TCP資料發送視窗大小（位元組）
net.core.wmem_default = 131072
### 最大的TCP資料發送視窗（位元組）
net.core.wmem_max = 33554432
### 定義了系統中每一個埠最大的監聽佇列的長度，這是個全域的參數
net.core.somaxconn = 65535

### TCP/UDP協定允許使用的本地埠號
net.ipv4.ip_local_port_range = 15000 65000
net.ipv4.ip_nonlocal_bind = 1
### 對於本端斷開的socket連接，TCP保持在FIN-WAIT-2狀態的時間（秒）
net.ipv4.tcp_fin_timeout = 7
### TCP發送keepalive探測消息的間隔時間（秒），用於確認TCP連接是否有效
net.ipv4.tcp_keepalive_time = 300
net.ipv4.tcp_max_orphans = 3276800
### 對於還未獲得對方確認的連接請求，可保存在佇列中的最大數目
net.ipv4.tcp_max_syn_backlog = 655360
net.ipv4.tcp_max_tw_buckets = 6000000
### 確定TCP棧應該如何反映記憶體使用，每個值的單位都是記憶體頁（通常是4KB）
### 第一個值是記憶體使用的下限；第二個值是記憶體壓力模式開始對緩衝區使用應用壓力的上限；第三個值是記憶體使用的上限.
net.ipv4.tcp_mem = 94500000 915000000 927000000
### 為自動調優定義socket使用的記憶體。
### 第一個值是為socket接收緩衝區分配的最少位元組數；
### 第二個值是預設值（該值會被rmem_default覆蓋），緩衝區在系統負載不重的情況下可以增長到這個值；
### 第三個值是接收緩衝區空間的最大位元組數（該值會被rmem_max覆蓋）
net.ipv4.tcp_rmem = 32768 699040 50331648
### 為自動調優定義socket使用的記憶體。
### 第一個值是為socket發送緩衝區分配的最少位元組數；
### 第二個值是預設值（該值會被wmem_default覆蓋），緩衝區在系統負載不重的情況下可以增長到這個值；
### 第三個值是發送緩衝區空間的最大位元組數（該值會被wmem_max覆蓋）
net.ipv4.tcp_wmem = 32768 131072 33554432
net.ipv4.tcp_slow_start_after_idle = 0
net.ipv4.tcp_synack_retries = 2
### 表示是否打開TCP同步標籤（syncookie），同步標籤可以防止一個通訊端在有過多試圖連接到達時引起超載
### 內核必須打開了CONFIG_SYN_COOKIES項進行編譯，
net.ipv4.tcp_syncookies = 1
net.ipv4.tcp_syn_retries = 2
### 表示開啟TCP連接中TIME-WAIT sockets的快速回收，默認為0，表示關閉
net.ipv4.tcp_tw_recycle = 0
### 允許將TIME-WAIT sockets重新用於新的TCP連接，默認為0，表示關閉
net.ipv4.tcp_tw_reuse = 1
### 啟用RFC 1323定義的window scaling，要支持超過64KB的TCP視窗，必須啟用該值（1表示啟用），
### TCP視窗最大至1GB，TCP連接雙方都啟用時才生效，默認為1
net.ipv4.tcp_window_scaling = 1
### 最大限度使用實體記憶體
vm.swappiness = 0

##net.ipv4.conf.default.accept_all=1
##net.ipv4.conf.all.accept_all=1

##net.ipv4.conf.default.rp_filter=0
##net.ipv4.conf.all.rp_filter=0

```

### network setting part B
三台架構
```shell
A<---------->B<------SWITCH Port------>C
                     SWITCH Port------>D
```

A 為client 
B HAProxy預計設定位置
C 為Guest OS(Cuju Primary所在地)
D 為Cuju Backup所在地

雖然三台架構分工明確但實驗上叫不易
因此有了雙台架構 

A仍為client 

B HAProxy預計設定位置
C 為Cuju Primary所在地 (但仍在B Host上)
D 為Cuju Backup所在地 (但仍在B Host上)

C與D之間的FT link 則由B Host內部網路負責
```shell
|Cuju  Primary|Cuju Backup|
---------------------------
|          bridge         |	
---------------------------     
|          B Host         |
---------------------------
 ``` 
### network setting part C (A Host network)
該台網路需要獨立出來 (因為route的關係)
若能設定分段gateway/route則可對外 (實際應用上有DNS server輔助)
修改/etc/network/interface
```shell
gateway [B HOST internal ip]
```
### network setting part D (B Host network)
使bridge綁訂於內部用IP
外部用IP連接至 Host A
外部用IP
修改/etc/network/interface
```shell
auto br0
iface br0 inet static
bridge_ports enp1s0f0
bridge_maxwait 0
address [B HOST internal ip]
netmask 255.255.255.0
dns-nameservers 8.8.8.8
gateway X.X.X.X

auto enp1s0f0
iface enp1s0f0 inet static
address 0.0.0.0
dns-nameservers 8.8.8.8

auto enp1s0f1
iface enp1s0f1 inet static
address [B HOST externel ip]
netmask 255.255.255.0
gateway [B HOST internal ip]

```
