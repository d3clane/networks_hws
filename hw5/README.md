# networks_hws

Запуск через cmake стандартный
Usage описан в исходнике:
```
./chat tcp server <ip> <port> <client.crt> <client.key>
./chat tcp client <ip> <port> <ca-chain.crt>
./chat udp server <ip> <port>
./chat udp client <ip> <port>
```

sslkeylog логируется клиентом.
