#ifndef SUNSTONE_AIC8800_ADDRESS_TRACE_H
#define SUNSTONE_AIC8800_ADDRESS_TRACE_H
#include <stddef.h>
#include <stdint.h>
/* The address stage's DHCP and DNS exchange as the radio owner sees its frames: DHCP discover and request and the DNS
 * query as they leave, the DHCP offer, ACK and NAK of this station's transaction and the DNS response to its query as
 * they arrive. The record copy holds counts, codes and times only: no address, MAC, host name or DNS name. The
 * observer's own state keeps the transaction identities and the offered address to compare against the ACK; that state
 * never leaves memory.
 *
 * Counts saturate at 65535. Times are milliseconds since the network started, at the first frame of each kind; a time
 * is meaningful only when its count is nonzero. ms_ack is the first ACK or NAK.
 * flags:
 *   bit 0  the lease was published (the network reported an address)
 *   bit 1  the ACK's address is private-range (RFC 1918)
 *   bit 2  the ACK carried a router option
 *   bit 3  the first router is private-range
 *   bit 4  the offer and the ACK name the same address
 *   bit 5  the offer and the ACK name the same server (option 54)
 *   bit 6  a sent DHCP message carried a host name option (12); the image sends none
 *   bit 7  the ACK carried a lease time (option 51)
 *   bit 8  a DNS response carried the identifier of the sent query
 *   bit 9  the resolve returned an address
 *   bit 10 the IP task acknowledged the offline fence: DHCP stopped and its socket closed
 *   bit 11 the address stage's bound passed
 *   bit 12 the network's random-number hook found no pre-drawn word
 * result: the resolve's reply (enum sunstone_net_result) plus 1; 0 when no reply came. */
struct aic8800_address_record {
    uint32_t flags;
    uint16_t discovers, offers, requests, acks, naks, queries, responses, ignored;
    uint32_t ms_discover, ms_offer, ms_request, ms_ack, ms_query, ms_response;
    uint32_t lease_seconds;
    uint16_t answers;
    uint8_t rcode, dns_servers, result, pad[3];
};
_Static_assert(sizeof(struct aic8800_address_record)==56,"address record");
#define AIC8800_ADDRESS_PUBLISHED 1u
#define AIC8800_ADDRESS_PRIVATE 2u
#define AIC8800_ADDRESS_ROUTER 4u
#define AIC8800_ADDRESS_ROUTER_PRIVATE 8u
#define AIC8800_ADDRESS_OFFER_MATCH 16u
#define AIC8800_ADDRESS_SERVER_MATCH 32u
#define AIC8800_ADDRESS_HOSTNAME_SENT 64u
#define AIC8800_ADDRESS_LEASE_TIME 128u
#define AIC8800_ADDRESS_DNS_MATCH 256u
#define AIC8800_ADDRESS_RESOLVED 512u
#define AIC8800_ADDRESS_FENCED 1024u
#define AIC8800_ADDRESS_DEADLINE 2048u
#define AIC8800_ADDRESS_ENTROPY_SHORT 4096u
struct aic8800_address_trace {
    struct aic8800_address_record record;
    uint64_t start_us;
    uint32_t xid, offer_address, offer_server;
    uint16_t dns_id;
    uint8_t mac[6], xid_set, dns_set;
};
/* Start the observation at start_us for the station with this MAC. */
void aic8800_address_trace_start(struct aic8800_address_trace *, const uint8_t mac[6], uint64_t start_us);
/* One Ethernet frame, sent (sent=1) or received, at now_us. Frames that are not DHCP or DNS change nothing. Owner task
 * only; the frame is read, never kept. */
void aic8800_address_observe(struct aic8800_address_trace *, int sent, const uint8_t *frame, size_t bytes, uint64_t now_us);
/* Private-range IPv4 (RFC 1918: 10/8, 172.16/12, 192.168/16), address in network byte order as four bytes. */
int aic8800_address_private(const uint8_t address[4]);
#endif
