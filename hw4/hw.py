import sys
import argparse
from netfilterqueue import NetfilterQueue
from scapy.all import IP, UDP, TCP, DNS

def LoadRules(path):
    rules = []
    for line in open(path, "r"):
        line = line.strip()
        if not line:
            continue
        parts = line.split()
        rule = {}
        for part in parts:
            if "=" not in part:
                continue
            rule_key, rule_value = part.split("=", 1)
            if rule_key == "qname":
                rule_value = rule_value.strip().rstrip(".").lower()
            rule[rule_key] = rule_value

        rules.append(rule)
    return rules

def ExtractFields(payload):
    pkt = IP(payload)
    dns = pkt.getlayer(DNS)
    if dns is None:
        return None

    fields = {}
    fields["qr"] = str(int(dns.qr))
    fields["opcode"] = str(int(dns.opcode))
    fields["rcode"] = str(int(dns.rcode))
    
    qd = getattr(dns, "qd", None)
    if qd is None:
        return fields
    
    fields["qname"] = str(qd.qname.decode("ascii").strip().rstrip(".").lower())
    fields["qtype"] = str(qd.qtype)

    return fields

def GetAction(rules, fields, default_action="accept"):
    for rule in rules:
        rule_applied = True
        for key, value in rule.items():
            if key == "action":
                continue
            if key not in fields or fields[key] != value:
                rule_applied = False
                break
        if rule_applied:
            return rule["action"]
    return default_action

def Filter(rules, pkt):
    payload = pkt.get_payload()
    fields = ExtractFields(payload)

    print("Filter start")
    if fields is None:
        pkt.accept()
        return
    
    print("Rules and fields")
    print(rules)
    print(fields)
    act = GetAction(rules, fields)

    if act == "drop":
        pkt.drop()
    else:
        pkt.accept()

if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--queue", type=int, default=5)
    p.add_argument("--rules", required=True)
    args = p.parse_args()

    rules = LoadRules(args.rules)
    nfq = NetfilterQueue()
    nfq.bind(args.queue, lambda pkt: Filter(rules, pkt))
    nfq.run()
