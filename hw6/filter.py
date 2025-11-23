from netfilterqueue import NetfilterQueue
from scapy.all import IP, ICMP, UDP, DNS, DNSRR, Raw, send, dnsqtypes, icmptypes
import sys

TRIGGER = "rerand0m.ru"
FAKE_RESPONSES_SUBNET = "10.0.0."

MAX_TTL = 255

POEM_LINES = []
POEM_NUM_LINES = 0

def read_poem(path):
    lines = []
    with open(path, "r") as f:
        for line in f:
            line = line.strip()
            if line:
                lines.append(line)
    return lines


def poem_line_to_hostname(line):
    return ".".join(line.strip().lower().split())


def retrieve_ip_from_ptr_name(ptr_name):
    stripped_ptr_name = ptr_name.rstrip(".")
    suffix = ".in-addr.arpa"
    assert stripped_ptr_name.endswith(suffix)

    return '.'.join(reversed(stripped_ptr_name[:-len(suffix)].split(".")))

def cur_ret_ip(index):
    return FAKE_RESPONSES_SUBNET + str(index)


def dest_ip():
    return cur_ret_ip(POEM_NUM_LINES - 1)


def response_dns_on_A_type(ip_pkt, udp, dns, rdata_ip):
    return(
        IP(src=ip_pkt.dst, dst=ip_pkt.src)    / 
        UDP(sport=udp.dport, dport=udp.sport) / 
        DNS(id=dns.id,
            qr=1,
            qd=dns.qd,
            an=DNSRR(
                rrname=dns.qd.qname,
                type="A",
                rclass="IN",
                rdata=rdata_ip,
            ),
        )
    )


def response_dns_on_PTR_type(ip_pkt, udp, dns, hostname):
    return (
        IP(src=ip_pkt.dst, dst=ip_pkt.src)      / 
        UDP(sport=udp.dport, dport=udp.sport)   / 
        DNS(id=dns.id,
            qr=1,
            qd=dns.qd,
            an=DNSRR(
                rrname=dns.qd.qname,
                type="PTR",
                rclass="IN",
                rdata=hostname,
            ),
        )
    )


def handle_dns(pkt, ip_pkt):
    udp = ip_pkt[UDP]
    assert udp is not None
    dns = ip_pkt[DNS]
    assert dns is not None

    if dns.qr != 0:
        pkt.accept()
        return

    qname = dns.qd.qname.decode().rstrip(".")
    qtype = dns.qd.qtype

    if qtype == 1 and qname == TRIGGER:
        send(response_dns_on_A_type(ip_pkt, udp, dns, dest_ip()))
        pkt.drop()
        return

    if qtype == 12:
        ip_str = retrieve_ip_from_ptr_name(qname)
        if ip_str and ip_str.startswith(FAKE_RESPONSES_SUBNET):
            index = int(ip_str.split(".")[-1])
            if index < POEM_NUM_LINES:
                poem_based_hostname = poem_line_to_hostname(POEM_LINES[index])
                send(response_dns_on_PTR_type(ip_pkt, udp, dns, poem_based_hostname))
                pkt.drop()
                return


    pkt.accept()


def handle_icmp(pkt, ip_pkt):
    icmp = ip_pkt[ICMP]
    if icmp.type != 8:
        pkt.accept()
        return

    if ip_pkt.dst != dest_ip():
        pkt.accept()
        return

    if ip_pkt.ttl < POEM_NUM_LINES:
        src_ip = cur_ret_ip(ip_pkt.ttl - 1)
        reply = (
            IP(src=src_ip, dst=ip_pkt.src) / 
            ICMP(type=11, code=0)          / 
            Raw(bytes(ip_pkt))
        )
    else:
        src_ip = dest_ip()
        reply = (
            IP(src=src_ip, dst=ip_pkt.src)          / 
            ICMP(type=0, id=icmp.id, seq=icmp.seq)  /
            icmp.payload
        )

    send(reply)
    pkt.drop()

def handle_udp_traceroute(pkt, ip_pkt):
    udp = ip_pkt[UDP]

    if udp.dport == 53 or udp.sport == 53:
        pkt.accept()
        return

    if ip_pkt.dst != dest_ip():
        pkt.accept()
        return

    if ip_pkt.ttl < POEM_NUM_LINES:
        src_ip = cur_ret_ip(ip_pkt.ttl - 1)
        reply = (
            IP(src=src_ip, dst=ip_pkt.src) /
            ICMP(type=11, code=0)          / 
            Raw(bytes(ip_pkt))
        )
    else:
        src_ip = dest_ip()
        reply = (
            IP(src=src_ip, dst=ip_pkt.src) / 
            ICMP(type=3, code=3)           /
            Raw(bytes(ip_pkt))
        )

    send(reply)
    pkt.drop()


def handle_packet(pkt):
    ip_payload = IP(pkt.get_payload())

    if ip_payload.haslayer(DNS):
        handle_dns(pkt, ip_payload)
    elif ip_payload.haslayer(ICMP):
        handle_icmp(pkt, ip_payload)
    elif ip_payload.haslayer(UDP):
        handle_udp_traceroute(pkt, ip_payload)
    else:
        pkt.accept()


def main():
    global POEM_LINES, POEM_NUM_LINES

    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <poem_file>")
        sys.exit(1)

    POEM_LINES = read_poem(sys.argv[1])
    POEM_NUM_LINES = len(POEM_LINES)

    assert POEM_NUM_LINES > 0
    assert POEM_NUM_LINES <= MAX_TTL

    nfq = NetfilterQueue()
    nfq.bind(1, handle_packet)
    nfq.run()


if __name__ == "__main__":
    main()
