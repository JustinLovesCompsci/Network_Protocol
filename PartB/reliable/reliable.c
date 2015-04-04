#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>

#include "rlib.h"

// Define constants
#define INT_MAX 4294967296 // 2^32
#define SIZE_ACK_PACKET 12 // size of an ack packet
#define SIZE_EOF_PACKET 16

struct reliable_state {
	rel_t *next; /* Linked list for traversing all connections */
	rel_t **prev;
	conn_t *c; /* This is the connection object */

	/* Add your own data fields below this */
	int ssthresh; // congestion window threshold
	int cwnd;

	/* For client/sender */
	int expected_ack; // increment by 1 whenever receiver receives a correct ack

	// for duplicated ack detection
	int last_received_ack_no;
	int duplicated_ack_counter;

	/* For server/receiver */
	int has_sent_EOF_packet;
};
rel_t *rel_list;

/* Packet format
 *
 * struct packet {
 * uint16_t cksum;
 * uint16_t len;
 * uint32_t ackno;
 * uint32_t rwnd;
 * uint32_t seqno; Only valid if length > 8
 * char data[1000];
 };
 */

////////////////////////////////////////////////////////////////////////
////////////////////////// Helper functions /////////////////////////////
////////////////////////////////////////////////////////////////////////
int check_acks_validity(rel_t*, packet_t*); // check whether the received ack has the ack number that we are expecting
packet_t * create_EOF_packet();
int check_acks_validity(rel_t, packet_t); // check whether the received ack has the ack number that we are expecting
void convertToHostByteOrder(packet_t*);
void convertToNetworkByteOrder(packet_t *);
uint16_t computeChecksum(packet_t *, int);

int check_acks_validity(rel_t* r, packet_t* ack) {
	assert(ack->len == SIZE_ACK_PACKET);
	return ack->ackno == r->expected_ack;
}

/* Creates a new reliable protocol session, returns NULL on failure.
 * Exactly one of c and ss should be NULL.  (ss is NULL when called
 * from rlib.c, while c is NULL when this function is called from
 * rel_demux.) */
rel_t *
rel_create(conn_t *c, const struct sockaddr_storage *ss,
		const struct config_common *cc) {
	rel_t *r;

	r = xmalloc(sizeof(*r));
	memset(r, 0, sizeof(*r));

	if (!c) {
		c = conn_create(r, ss);
		if (!c) {
			free(r);
			return NULL;
		}
	}

	r->c = c;
	rel_list = r;

	/* Do any other initialization you need here */
	r->ssthresh = INT_MAX;
	r->cwnd = 1;
	r->duplicated_ack_counter = 0;
	r->expected_ack = 1;
	r->has_sent_EOF_packet = 0;

	// NOTE: if server/receiver, send EOF packet to client. If client, start slow start.
	return r;
}

void rel_destroy(rel_t *r) {
	conn_destroy(r->c);

	/* Free any other allocated memory here */
}

void rel_demux(const struct config_common *cc,
		const struct sockaddr_storage *ss, packet_t *pkt, size_t len) {
	//leave it blank here!!!
}

void rel_recvpkt(rel_t *r, packet_t *pkt, size_t n) {
	// TODO: If receive normal ack, first check if it is a triply duplicated ack. If not,
	//	1. increment cwnd (cwnd = cwnd + 1/cwnd)
	// 	2. set last ack no. and set duplicated_ack_counter to be 1
	//	3. call conn_output etc.; probably similar to part a
	// If it is a triply duplicated acks,
	//	1. ssthresh = cwnd/2
	//	2. cwnd = ssthresh
	//	3. do fast retransmission (need to determine which packets to retransmit)

	// TODO: handle EOF and data packets
}

void rel_read(rel_t *s) {
	if (s->c->sender_receiver == RECEIVER) {
		//if already sent EOF to the sender
		//  return;
		//else
		//  send EOF to the sender

		if (s->has_sent_EOF_packet == 1) {
			return;
		} else {
			packet_t * eof_packet = make_eof_packet();
			assert(sizeof(eof_packet) == SIZE_ACK_PACKET);
			conn_sendpkt(s->c, eof_packet, (size_t) SIZE_ACK_PACKET);
			s->has_sent_EOF_packet = 1;
			free(eof_packet);
		}
	} else //run in the sender mode
	{
		//same logic as lab 1
	}
}

void rel_output(rel_t *r) {
}

void rel_timer() {
	/* Retransmit any packets that need to be retransmitted */

	// Loop through the last_sent_packets to check if anyone of the packets
	// 	a. have not received an ack yet, and
	// 	b. has timed out
	// If so, retransmit those packets and do multiplicative decrease:
	//	ssthresh = cwnd/2; cwnd = 1;
	// and perform slow start again
}

//////////////////////////////// Helper functions ///////////////////////////////
packet_t * create_EOF_packet() {
	packet_t* eof_packet = (packet_t *) malloc(sizeof(packet_t));
	eof_packet->len = SIZE_EOF_PACKET;
	eof_packet->ackno = 1;
	eof_packet->rwnd = 1;
	eof_packet->seqno = 0;
	eof_packet->cksum = computeChecksum(eof_packet, SIZE_EOF_PACKET);
	return eof_packet;
}

/*
 * struct packet {
 * uint16_t cksum;
 * uint16_t len;
 * uint32_t ackno;
 * uint32_t rwnd;
 * uint32_t seqno; Only valid if length > 8
 * char data[1000];
 };
 */

void convertToHostByteOrder(packet_t* packet) {
	packet->len = ntohs(packet->len);
	packet->ackno = ntohl(packet->ackno);

	/* if the packet is a data packet it additionally has a seqno that has
	 to be converted to host byte order */
	if (packet->len != SIZE_ACK_PACKET)
		packet->seqno = ntohl(packet->seqno);
}

void convertToNetworkByteOrder(packet_t *packet) {
// if data packet, convert its seqno as well
	if (packet->len != SIZE_ACK_PACKET)
		packet->seqno = htonl(packet->seqno);

	packet->len = htons(packet->len);
	packet->ackno = htonl(packet->ackno);
}

uint16_t computeChecksum(packet_t *packet, int packetLength) {
	memset(&(packet->cksum), 0, sizeof(packet->cksum));
	return cksum((void*) packet, packetLength);
}
