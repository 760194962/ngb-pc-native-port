// Virtual network for online play (net/vnet.h). Only what an Xbox needs to reach Xbox Live: ARP for the gateway, a
// DHCP lease, DNS relayed to Insignia's server (or to a PC running InsigniaDNS, black.ini online_dns_mode = insigniadns,
// or where the console's settings send it, = console), and UDP through host sockets (Kerberos on 88, the security gateway
// on 3074: everything after sign-in travels inside that UDP tunnel). IP fragments are reassembled before they leave
// and large replies are fragmented for the Xbox. Nothing of the payloads is logged.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "net/vnet.h"

#include "common.h"
#include "d3d/perfhud.h"
#include "kernel/xboxkrnl.h"
#include "net/nvnet.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

void note_frame(const char *dir, const uint8_t *f, size_t len);

namespace {

constexpr uint32_t NET = 0x0A000200;               // 10.0.2.0/24
constexpr uint32_t GUEST_IP = NET | 15, GW_IP = NET | 2, DNS_IP = NET | 3;
const uint8_t kGwMac[6] = {0x52, 0x55, 0x0A, 0x00, 0x02, 0x02};
constexpr int MTU = 1500;

uint8_t g_guest_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
uint32_t g_dns_server;  // host byte order
bool g_dns_pc;          // g_dns_server is the InsigniaDNS of online_dns_mode = insigniadns (not Insignia's instead)
WSAEVENT g_sock_event;

struct Flow {
    SOCKET s = INVALID_SOCKET;
    uint16_t guest_port = 0;
    bool dns = false;     // a DNS relay: replies come from the server the Xbox asked (dns_server:53)
    uint32_t dns_server = 0;
    uint64_t last_use = 0;
    uint64_t asked = 0; // a DNS query sent and not answered yet: when (kclock_ms)
    uint32_t sent = 0, received = 0;
};
std::map<uint16_t, Flow> g_flows;     // guest source port -> socket
std::map<uint16_t, Flow> g_dns_flows; // the same for DNS queries (relayed to Insignia's server)

struct Frag {
    std::vector<uint8_t> data;
    std::vector<bool> have;
    size_t total = 0;
    uint64_t started = 0;
};
std::map<uint64_t, Frag> g_frags; // (src, id, proto) -> reassembly

uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; }
void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }

// A message for the player at the picture's bottom-right (d3d/perfhud.cpp), each kind once a run, and in the log
void tell(int kind, const wchar_t *text)
{
    static bool told[4];
    if (told[kind]) return;
    told[kind] = true;
    logf("[net] shown to the player: %s", narrow(text).c_str());
    perfhud::notice(text, 8000);
}
enum { TELL_FAKE_DNS, TELL_DNS_SILENT, TELL_CONNECTED };

std::string ip_str(uint32_t ip)
{
    char b[16];
    snprintf(b, sizeof b, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return b;
}

uint32_t csum_add(uint32_t sum, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i + 1 < n; i += 2) sum += rd16(p + i);
    if (n & 1) sum += p[n - 1] << 8;
    return sum;
}

uint16_t csum_fold(uint32_t sum)
{
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

void send_eth(const uint8_t dst[6], uint16_t type, const uint8_t *payload, size_t len)
{
    std::vector<uint8_t> f(14 + len);
    memcpy(&f[0], dst, 6);
    memcpy(&f[6], kGwMac, 6);
    wr16(&f[12], type);
    memcpy(&f[14], payload, len);
    note_frame("to Xbox", f.data(), f.size());
    nvnet_to_guest(f.data(), f.size());
}

uint16_t g_ip_id = 1;

// An IP datagram to the Xbox, fragmented at the MTU
void send_ip(uint32_t src, uint32_t dst, uint8_t proto, const uint8_t *payload, size_t len, const uint8_t *dst_mac = g_guest_mac)
{
    uint16_t id = g_ip_id++;
    size_t max_data = (MTU - 20) & ~7u;
    for (size_t off = 0; off < len || (off == 0 && len == 0);) {
        size_t n = std::min(max_data, len - off);
        bool more = off + n < len;
        uint8_t h[20] = {0x45, 0};
        wr16(h + 2, (uint16_t)(20 + n));
        wr16(h + 4, id);
        wr16(h + 6, (uint16_t)((more ? 0x2000 : 0) | (off / 8)));
        h[8] = 64;
        h[9] = proto;
        wr32(h + 12, src);
        wr32(h + 16, dst);
        wr16(h + 10, csum_fold(csum_add(0, h, 20)));
        std::vector<uint8_t> pkt(h, h + 20);
        pkt.insert(pkt.end(), payload + off, payload + off + n);
        send_eth(dst_mac, 0x0800, pkt.data(), pkt.size());
        off += n;
        if (!more) break;
    }
}

void send_udp(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const uint8_t *data, size_t len,
              const uint8_t *dst_mac = g_guest_mac)
{
    std::vector<uint8_t> u(8 + len);
    wr16(&u[0], sport);
    wr16(&u[2], dport);
    wr16(&u[4], (uint16_t)u.size());
    memcpy(&u[8], data, len);
    uint8_t pseudo[12];
    wr32(pseudo, src);
    wr32(pseudo + 4, dst);
    pseudo[8] = 0;
    pseudo[9] = 17;
    wr16(pseudo + 10, (uint16_t)u.size());
    uint16_t c = csum_fold(csum_add(csum_add(0, pseudo, 12), u.data(), u.size()));
    wr16(&u[6], c ? c : 0xFFFF);
    send_ip(src, dst, 17, u.data(), u.size(), dst_mac);
}

// --- ARP ---

void arp(const uint8_t *a, size_t len)
{
    if (len < 28 || rd16(a + 6) != 1) return; // requests only
    uint32_t target = rd32(a + 24), sender = rd32(a + 14);
    memcpy(g_guest_mac, a + 8, 6);
    if ((target & 0xFFFFFF00) != NET || target == GUEST_IP || target == sender) return; // a gratuitous / probe ARP
    uint8_t r[28];
    memcpy(r, a, 6);  // htype, ptype, hlen, plen
    wr16(r + 6, 2);   // reply
    memcpy(r + 8, kGwMac, 6);
    wr32(r + 14, target);
    memcpy(r + 18, a + 8, 6);
    wr32(r + 24, sender);
    send_eth(a + 8, 0x0806, r, sizeof r);
}

// --- DHCP ---

void dhcp(const uint8_t *b, size_t len)
{
    if (len < 240 || b[0] != 1 || rd32(b + 236) != 0x63825363) return;
    int type = 0;
    for (size_t i = 240; i < len && b[i] != 255;) {
        if (b[i] == 0) { i++; continue; }
        if (i + 1 >= len) break;
        if (b[i] == 53 && b[i + 1] >= 1) type = b[i + 2];
        i += 2 + b[i + 1];
    }
    if (type != 1 && type != 3) return; // DISCOVER, REQUEST
    std::vector<uint8_t> r(240, 0);
    r[0] = 2;
    r[1] = 1;
    r[2] = 6;
    memcpy(&r[4], b + 4, 4);    // xid
    memcpy(&r[10], b + 10, 2);  // flags
    wr32(&r[16], GUEST_IP);     // yiaddr
    wr32(&r[20], GW_IP);        // siaddr
    memcpy(&r[28], b + 28, 16); // chaddr
    wr32(&r[236], 0x63825363);
    auto opt = [&](uint8_t code, std::initializer_list<uint8_t> v) {
        r.push_back(code);
        r.push_back((uint8_t)v.size());
        r.insert(r.end(), v.begin(), v.end());
    };
    opt(53, {(uint8_t)(type == 1 ? 2 : 5)}); // OFFER / ACK
    opt(54, {10, 0, 2, 2});
    opt(51, {0, 1, 0x51, 0x80}); // 86400 s
    opt(1, {255, 255, 255, 0});
    opt(3, {10, 0, 2, 2});
    opt(6, {10, 0, 2, 3});
    r.push_back(255);
    r.resize(std::max<size_t>(r.size(), 300), 0);
    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    send_udp(GW_IP, 67, 0xFFFFFFFF, 68, r.data(), r.size(), bcast);
    logf("[net] DHCP %s: Xbox 10.0.2.15, gateway 10.0.2.2, DNS 10.0.2.3 (relayed to %s)", type == 1 ? "offer" : "ack",
         ip_str(g_dns_server).c_str());
}

// --- UDP NAT ---

Flow *flow_for(uint16_t guest_port, bool dns)
{
    auto &table = dns ? g_dns_flows : g_flows;
    auto it = table.find(guest_port);
    if (it != table.end()) return &it->second;
    Flow f;
    f.s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (f.s == INVALID_SOCKET) return nullptr;
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = dns ? 0 : htons(guest_port); // the Xbox's own port when the host has it free (3074 for Live)
    if (bind(f.s, (sockaddr *)&a, sizeof a) != 0) {
        a.sin_port = 0;
        bind(f.s, (sockaddr *)&a, sizeof a);
    }
    WSAEventSelect(f.s, g_sock_event, FD_READ);
    f.guest_port = guest_port;
    f.dns = dns;
    sockaddr_in local = {};
    int ll = sizeof local;
    getsockname(f.s, (sockaddr *)&local, &ll);
    logf("[net] UDP %s port %u -> host port %u", dns ? "DNS relay for Xbox" : "Xbox", guest_port, ntohs(local.sin_port));
    return &(table[guest_port] = f);
}

void dns_names(const uint8_t *d, size_t len, std::string &out) // the first question's name (logged: no payload data)
{
    if (len < 12) return;
    size_t i = 12;
    while (i < len && d[i] && d[i] < 64) {
        if (!out.empty()) out += '.';
        out.append((const char *)d + i + 1, std::min<size_t>(d[i], len - i - 1));
        i += 1 + d[i];
    }
}

void udp(uint32_t src, uint32_t dst, const uint8_t *u, size_t len)
{
    if (len < 8) return;
    uint16_t sport = rd16(u), dport = rd16(u + 2), ulen = rd16(u + 4);
    if (ulen < 8 || ulen > len) return;
    const uint8_t *data = u + 8;
    size_t n = ulen - 8;
    if (dport == 67) {
        dhcp(data, n);
        return;
    }
    if (dst == 0xFFFFFFFF || (dst & 0xFF) == 0xFF || (dst >> 28) == 0xE) return; // broadcast / multicast: system link only
    // DNS: every query goes to online_dns (Insignia's), whatever server the console's settings name; with
    // online_dns_mode = console it goes where they send it (manual DNS, e.g. the PC running InsigniaDNS) and only the
    // DHCP-given 10.0.2.3 is relayed
    bool dns = dport == 53 && (dst == DNS_IP || !g_cfg.online_dns_console);
    if ((dst & 0xFFFFFF00) == NET && !dns) return;
    Flow *f = flow_for(sport, dns);
    if (!f) return;
    if (dns) f->dns_server = dst;
    uint32_t server = dns && (dst == DNS_IP || !g_cfg.online_dns_console) ? g_dns_server : dst;
    if (dns && !f->asked) f->asked = kclock_ms();
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(server);
    to.sin_port = htons(dport);
    if (dns) {
        std::string name;
        dns_names(data, n, name);
        logf("[net] DNS query %s to %s", name.c_str(), ip_str(server).c_str());
    } else if (!f->sent) {
        logf("[net] UDP Xbox:%u -> %s:%u (first datagram, %zu bytes)", sport, ip_str(dst).c_str(), dport, n);
    }
    sendto(f->s, (const char *)data, (int)n, 0, (sockaddr *)&to, sizeof to);
    f->sent++;
    f->last_use = kclock_ms();
}

void dns_answer_log(const uint8_t *d, size_t len)
{
    if (len < 12) return;
    std::string name;
    dns_names(d, len, name);
    int answers = rd16(d + 6);
    size_t i = 12;
    while (i < len && d[i]) i += (d[i] >= 0xC0) ? 2 : 1 + d[i];
    i += 5; // root label, type, class
    std::string ips;
    for (int k = 0; k < answers && i + 12 <= len; k++) {
        if (d[i] >= 0xC0) i += 2;
        else {
            while (i < len && d[i]) i += 1 + d[i];
            i++;
        }
        if (i + 10 > len) break;
        uint16_t type = rd16(d + i), rdlen = rd16(d + i + 8);
        if (type == 1 && rdlen == 4 && i + 14 <= len) {
            ips += (ips.empty() ? "" : ", ") + ip_str(rd32(d + i + 10));
            if ((rd32(d + i + 10) & 0xFFFE0000) == 0xC6120000) // 198.18.0.0/15: a proxy's fake-ip answer, not Insignia's
                tell(TELL_FAKE_DNS, L"联网：DNS 被代理软件截走（198.18.x.x），请关闭代理的 TUN 模式");
        }
        i += 10 + rdlen;
    }
    logf("[net] DNS answer %s -> %s (rcode %u)", name.c_str(), ips.empty() ? "(no address)" : ips.c_str(), d[3] & 15);
}

void drain(std::map<uint16_t, Flow> &table)
{
    static std::vector<char> buf(65536);
    for (auto &kv : table) {
        Flow &f = kv.second;
        for (;;) {
            sockaddr_in from = {};
            int fl = sizeof from;
            int n = recvfrom(f.s, buf.data(), (int)buf.size(), 0, (sockaddr *)&from, &fl);
            if (n < 0) break;
            uint32_t ip = ntohl(from.sin_addr.s_addr);
            uint16_t port = ntohs(from.sin_port);
            if (f.dns) {
                f.asked = 0;
                dns_answer_log((const uint8_t *)buf.data(), n);
                send_udp(f.dns_server, 53, GUEST_IP, f.guest_port, (const uint8_t *)buf.data(), n);
            } else {
                if (!f.received)
                    logf("[net] UDP %s:%u -> Xbox:%u (first reply, %d bytes)", ip_str(ip).c_str(), port, f.guest_port, n);
                if (port == 3074) tell(TELL_CONNECTED, L"联网：已连接 Insignia"); // the security gateway answered
                send_udp(ip, port, GUEST_IP, f.guest_port, (const uint8_t *)buf.data(), n);
            }
            f.received++;
            f.last_use = kclock_ms();
        }
    }
}

void expire()
{
    uint64_t now = kclock_ms();
    for (auto &kv : g_dns_flows)
        if (kv.second.asked && now - kv.second.asked > 4000) { // the Xbox gives up on a server after a few seconds
            uint32_t server = kv.second.dns_server == DNS_IP || !g_cfg.online_dns_console ? g_dns_server : kv.second.dns_server;
            logf("[net] DNS: no answer from %s in 4 s", ip_str(server).c_str());
            std::wstring text = (g_cfg.online_dns_console && server != g_dns_server) || g_dns_pc
                ? L"联网：DNS " + widen(ip_str(server)) + L" 没有回应（那台电脑开着 InsigniaDNS 吗？）"
                : L"联网：Insignia 的 DNS 没有回应，请检查网络";
            tell(TELL_DNS_SILENT, text.c_str());
            kv.second.asked = 0;
        }
    for (auto *table : {&g_flows, &g_dns_flows})
        for (auto it = table->begin(); it != table->end();) {
            if (now - it->second.last_use > (it->second.dns ? 30000u : 600000u)) {
                closesocket(it->second.s);
                it = table->erase(it);
            } else {
                ++it;
            }
        }
    for (auto it = g_frags.begin(); it != g_frags.end();)
        it = now - it->second.started > 30000 ? g_frags.erase(it) : std::next(it);
}

// IPv4 from the Xbox, reassembled if fragmented
void ip(const uint8_t *p, size_t len)
{
    if (len < 20 || (p[0] >> 4) != 4) return;
    size_t ihl = (p[0] & 15) * 4u, total = rd16(p + 2);
    if (ihl < 20 || total < ihl || total > len) return;
    uint32_t src = rd32(p + 12), dst = rd32(p + 16);
    uint8_t proto = p[9];
    uint16_t frag = rd16(p + 6);
    const uint8_t *data = p + ihl;
    size_t n = total - ihl;
    std::vector<uint8_t> whole;
    if (frag & 0x3FFF) { // MF set or an offset: collect
        uint64_t key = (uint64_t)src << 32 | (uint64_t)rd16(p + 4) << 8 | proto;
        Frag &fr = g_frags[key];
        if (!fr.started) fr.started = kclock_ms();
        size_t off = (frag & 0x1FFF) * 8u;
        if (off + n > 65535) return;
        if (fr.data.size() < off + n) fr.data.resize(off + n), fr.have.resize(off + n);
        memcpy(&fr.data[off], data, n);
        for (size_t i = off; i < off + n; i++) fr.have[i] = true;
        if (!(frag & 0x2000)) fr.total = off + n;
        if (!fr.total || fr.data.size() < fr.total) return;
        for (size_t i = 0; i < fr.total; i++)
            if (!fr.have[i]) return;
        whole.assign(fr.data.begin(), fr.data.begin() + fr.total);
        g_frags.erase(key);
        data = whole.data();
        n = whole.size();
    }
    if (proto == 17) {
        udp(src, dst, data, n);
    } else if (proto == 1 && n >= 8 && data[0] == 8 && (dst & 0xFFFFFF00) == NET) { // ping the gateway / DNS: echo reply
        std::vector<uint8_t> r(data, data + n);
        r[0] = 0;
        r[2] = r[3] = 0;
        wr16(&r[2], csum_fold(csum_add(0, r.data(), r.size())));
        send_ip(dst, src, 1, r.data(), r.size());
    } else {
        static int noted;
        if (noted++ < 8)
            logf("[net] IP protocol %u from the Xbox to %s not relayed (%zu bytes)%s", proto, ip_str(dst).c_str(), n,
                 proto == 6 && n >= 4 ? (" port " + std::to_string(rd16(data + 2))).c_str() : "");
    }
}

} // namespace

void vnet_start()
{
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    g_sock_event = WSACreateEvent();
    std::string dns = g_cfg.online_dns_pc ? narrow(g_cfg.online_insigniadns)
                    : g_cfg.online_dns.empty() ? "46.101.64.175" : narrow(g_cfg.online_dns);
    in_addr a;
    g_dns_server = inet_pton(AF_INET, dns.c_str(), &a) == 1 ? ntohl(a.s_addr) : 0x2E6540AF;
    g_dns_pc = g_cfg.online_dns_pc && ip_str(g_dns_server) == dns;
    if (g_cfg.online_dns_pc && !g_dns_pc) { // no address (or not an IPv4 one): Insignia's instead
        g_dns_server = 0x2E6540AF;
        logf("[net] online_insigniadns \"%s\" is not an IPv4 address: DNS goes to Insignia's", dns.c_str());
        tell(TELL_DNS_SILENT, L"联网：InsigniaDNS 的地址无效，改用 Insignia 的 DNS");
    }
    logf("[net] virtual network up: Xbox 10.0.2.15, gateway 10.0.2.2, DNS relay to %s (%s)", ip_str(g_dns_server).c_str(),
         g_cfg.online_dns_console ? "10.0.2.3 only; other servers as the console's settings name them"
         : g_dns_pc              ? "every DNS query, to InsigniaDNS" : "every DNS query");
}

// The first frames each way, by header only (evidence of the exchange; no payload bytes)
void note_frame(const char *dir, const uint8_t *f, size_t len)
{
    static int noted;
    if (noted >= 80 || len < 14) return;
    noted++;
    uint16_t type = rd16(f + 12);
    const uint8_t *p = f + 14;
    if (type == 0x0800 && len >= 34) {
        uint8_t proto = p[9];
        size_t ihl = (p[0] & 15) * 4u;
        bool ports = (proto == 17 || proto == 6) && len >= 14 + ihl + 4 && !(rd16(p + 6) & 0x1FFF);
        logf("[net] frame %s: IP %u %s:%u -> %s:%u, %zu bytes%s", dir, proto, ip_str(rd32(p + 12)).c_str(), ports ? rd16(p + ihl) : 0,
             ip_str(rd32(p + 16)).c_str(), ports ? rd16(p + ihl + 2) : 0, len, (rd16(p + 6) & 0x3FFF) ? " (fragment)" : "");
    } else if (type == 0x0806 && len >= 42) {
        logf("[net] frame %s: ARP %s %s -> %s", dir, rd16(p + 6) == 1 ? "request" : "reply", ip_str(rd32(p + 14)).c_str(),
             ip_str(rd32(p + 24)).c_str());
    } else {
        char hex[3 * 48 + 1] = {};
        for (size_t i = 0; i < 48 && i < len; i++) snprintf(hex + 3 * i, 4, "%02x ", f[i]);
        logf("[net] frame %s: type 0x%04x, %zu bytes: %s", dir, type, len, hex);
    }
}

void vnet_from_guest(const uint8_t *frame, size_t len)
{
    if (len < 14) return;
    note_frame("from Xbox", frame, len);
    uint16_t type = rd16(frame + 12);
    if (type == 0x0806) arp(frame + 14, len - 14);
    else if (type == 0x0800) {
        memcpy(g_guest_mac, frame + 6, 6);
        ip(frame + 14, len - 14);
    }
}

void vnet_wait(HANDLE wake, int timeout_ms)
{
    HANDLE h[2] = {wake, g_sock_event};
    WaitForMultipleObjects(2, h, FALSE, (DWORD)timeout_ms);
    WSAResetEvent(g_sock_event);
    drain(g_flows);
    drain(g_dns_flows);
    static uint64_t last_expire;
    uint64_t now = kclock_ms();
    if (now - last_expire > 5000) {
        last_expire = now;
        expire();
    }
}
