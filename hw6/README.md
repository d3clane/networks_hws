# Запуск

Очередь с номером 1 захардкожена. По схеме с семинара 5 (выход в интернет на eth0, альпины соединены на eth1), на alpine 2 делаем:

```
ip addr add 10.10.0.1/24 dev eth1
ip link set eth1 up

ip addr add 10.0.2.16/24 dev eth0
ip link set eth0 up

udhcpc -i eth0

sysctl -w net.ipv4.ip_forward=1
iptables -t nat -A POSTROUTING -s 10.10.0.0/24 -o eth0 -j MASQUERADE
iptables -A FORWARD -i eth1 -o eth0 -j ACCEPT
iptables -A FORWARD -i eth0 -o eth1 -j ACCEPT

iptables -t mangle -A PREROUTING -p udp --dport 53 -j NFQUEUE --queue-num 1
iptables -t mangle -A PREROUTING -p udp --sport 53 -j NFQUEUE --queue-num 1

# Далее маска сети 10.0.0.0/24, так как в коде я полагаюсь, что я с этих айпи делаю фейк-респонсы с песенкой

iptables -t mangle -A PREROUTING -p icmp --icmp-type echo-request -d 10.0.0.0/24 -j NFQUEUE --queue-num 1

iptables -t mangle -A PREROUTING -p udp -d 10.0.0.0/24 -j NFQUEUE --queue-num 1

python3 filter.py poem.txt
```

На alpine-1:

```
ip addr add 10.10.0.3/24 dev eth1
ip link set eth1 up

ip route add 10.10.0.0/24 dev eth1

ip route add default via 10.10.0.1 dev eth1
```