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
#include <limits.h>

#include "rlib.h"

int debug = 0;

/* define constants */
#define SIZE_ACK_PACKET 12 // size of an ack packet
#define SIZE_EOF_PACKET 16
#define SIZE_DATA_PCK_HEADER 16
#define SIZE_MAX_PAYLOAD 500
#define INIT_SEQ_NUM 1

struct packet_node {
	struct packet_node *next;
	struct packet_node *prev;
	packet_t * packet;
	struct timeval* time_sent;
};

struct sliding_window_send {
	uint32_t seqno_last_packet_acked; /* sequence number of the last packet receiver received */
	struct packet_node *last_packet_sent; /* a doubly linked list of sent packets */
	uint32_t seqno_last_packet_sent; /* sequence number of the last sent packet */
};

/**
 * seqno_last_packet_expected refers to the next packet expected to be received (always one greater than seqno_last_packet_received
 */
struct sliding_window_receive {
	uint32_t seqno_last_packet_outputted; /* sequence number of the last packet output to STDOUT */
	struct packet_node *last_packet_received; /* a doubly linked list of received packets */
	uint32_t seqno_next_packet_expected; /* sequence number of the next expected arrival packet */
};

struct reliable_state {
	rel_t *next; /* Linked list for traversing all connections */
	rel_t **prev;
	conn_t *c; /* This is the connection object */

	/* Add your own data fields below this */
	int ssthresh; // ssh threshold
	float congestion_window;
	int num_duplicated_ack_received;
	int num_packets_last_sent;

	/* For receiver */
	int has_sent_EOF_packet;

	/* below same as 3a */
	struct config_common config;
	struct sliding_window_send *sending_window;
	struct sliding_window_receive *receiving_window;

	int read_EOF_from_sender;
	int read_EOF_from_input;
	int all_pkts_acked;
	int output_all_data;
};

/* debug functions */
void print_config(struct config_common);
void print_rel(rel_t *);
void print_sending_window(struct sliding_window_send*);
void print_receiving_window(struct sliding_window_receive*);
void print_pointers_in_receive_window(struct sliding_window_receive*, int);

/* helper functions from 3a */
struct sliding_window_send * init_sending_window();
struct sliding_window_receive * init_receiving_window();
void destroy_sending_window(struct sliding_window_send*);
void destory_receiving_window(struct sliding_window_receive*);
void send_ack_pck(rel_t*, int);
int is_greater_than(struct timeval*, int);
void send_data_pck(rel_t*, struct packet_node*, struct timeval*);
int is_pkt_corrupted(packet_t*, size_t);
void convert_to_host_order(packet_t*);
void convert_to_network_order(packet_t*);
uint16_t get_check_sum(packet_t*, int);
struct packet_node* get_first_unread_pck(rel_t*);
struct packet_node* get_first_unacked_pck(rel_t*);
void append_node_to_last_sent(rel_t*, struct packet_node*);
void process_received_data_pkt(rel_t*, packet_t*);
void append_node_to_last_received(rel_t*, struct packet_node*);
int is_sending_window_full(rel_t*);
void process_received_ack_pkt(rel_t*, packet_t*);
int is_EOF_pkt(packet_t*);
int is_new_ACK(uint32_t, rel_t*);
int check_all_sent_pkts_acked(rel_t*);
struct timeval* get_current_time();
int try_end_connection(rel_t*);

rel_t *rel_list;

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
	r->congestion_window = 1;
	r->num_duplicated_ack_received = 0;
	r->num_packets_last_sent = 0;

	/* For receiver */
	r->has_sent_EOF_packet = 0;

	/* same as 3a */
	r->config = *cc;
	r->sending_window = init_sending_window();
	r->receiving_window = init_receiving_window();
	r->read_EOF_from_input = 0;
	r->read_EOF_from_sender = 0;
	r->output_all_data = 0;
	r->all_pkts_acked = 0;

	return r;
}

void rel_destroy(rel_t *r) {
//	conn_destroy(r->c);

	/* Free any other allocated memory here */
	if (debug)
		printf("IN rel_destroy\n");

	if (r->next) {
		r->next->prev = r->prev;
	}
	*r->prev = r->next;
	conn_destroy(r->c);

	free(r);
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
//			packet_t * eof_packet = make_eof_packet();
//			assert(sizeof(eof_packet) == SIZE_ACK_PACKET);
//			conn_sendpkt(s->c, eof_packet, (size_t) SIZE_ACK_PACKET);
//			s->has_sent_EOF_packet = 1;
//			free(eof_packet);
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

////////////////////////////////////////////////////////////////////////
////////////////////////// Debug functions /////////////////////////////
////////////////////////////////////////////////////////////////////////
void print_rel(rel_t * rel) {
	if (debug) {
		print_config(rel->config);
		print_sending_window(rel->sending_window);
		print_receiving_window(rel->receiving_window);
	}
}

void print_config(struct config_common c) {
	if (debug) {
		printf(
				"CONFIG info: Window size: %d, timer: %d, timeout: %d, single_connection: %d\n",
				c.window, c.timer, c.timeout, c.single_connection);
	}
}

void print_sending_window(struct sliding_window_send * window) {
	if (debug) {
		printf("Printing sending window related info....\n");
		printf("Last packet acked: %u, last packet written: %u\n",
				window->seqno_last_packet_acked,
				window->seqno_last_packet_sent);
		struct packet_node * pack = window->last_packet_sent;
		while (pack) {
			print_pkt(pack->packet, "packet", pack->packet->len);
			pack = pack->next;
		}
	}
}

void print_receiving_window(struct sliding_window_receive * window) {
	if (debug) {
		if (window == NULL)
			return;
		printf("Printing receiving window related info....\n");
		printf("Last packet read: %u, next packet expected: %u\n",
				window->seqno_last_packet_outputted,
				window->seqno_next_packet_expected);
		struct packet_node * pack = window->last_packet_received;
		while (pack) {
			printf("Packet data: %s\n", pack->packet->data);
			pack = pack->prev;
		}
	}
}

void print_pointers_in_receive_window(struct sliding_window_receive * window,
		int windowSize) {
	uint32_t seqnoLastReceived =
			window->last_packet_received == NULL ?
					0 : window->last_packet_received->packet->seqno;
	uint32_t seqno_last_outputted = window->seqno_last_packet_outputted;
	if (debug)
		printf("seqnoLastReceived = %d, seqnoLastRead = %d, windowSize = %d\n",
				seqnoLastReceived, seqno_last_outputted, windowSize);
}

////////////////////////////////////////////////////////////////////////
////////////////////////// Helper functions /////////////////////////////
////////////////////////////////////////////////////////////////////////

void process_received_ack_pkt(rel_t *r, packet_t *pkt) {
	if (debug) {
		printf("Received ACK packet\n");
	}

	/* update last packet acked pointer in sending window if new ack arrives */
	if (is_new_ACK(pkt->ackno, r)) {
		r->sending_window->seqno_last_packet_acked = pkt->ackno - 1;
		if (!is_sending_window_full(r)) {
			rel_read(r);
		}
	}

	check_all_sent_pkts_acked(r);
}

/* called by receiver
 * process a data packet from sender
 */
void process_received_data_pkt(rel_t *r, packet_t *packet) {

	if (debug) {
		printf("Start to process received data packet...\n");
		//printf("Packet seqno: %d, expecting: %d\n", packet->seqno, r->receiving_window->seqno_next_packet_expected);
	}

	if (debug)
		printf("Packet seqno: %d, expecting: %d\n", packet->seqno,
				r->receiving_window->seqno_next_packet_expected);

	if ((packet->seqno == r->receiving_window->seqno_next_packet_expected)) {
		/* seqno is the one expected next */

		uint32_t seqnoLastReceived =
				r->receiving_window->last_packet_received == NULL ?
						0 :
						r->receiving_window->last_packet_received->packet->seqno;
		uint32_t seqno_last_outputted =
				r->receiving_window->seqno_last_packet_outputted;
		int windowSize = r->config.window;
		if (debug) {
			print_pointers_in_receive_window(r->receiving_window, windowSize);
		}

		/* update receive window for the newly arrived packet */
		if ((seqnoLastReceived - seqno_last_outputted) < windowSize) {
			struct packet_node* node = (struct packet_node*) malloc(
					sizeof(struct packet_node));
			packet_t * pack = (packet_t *) malloc(sizeof(packet_t));
			memcpy(pack, packet, sizeof(packet_t));
			node->packet = pack;
			append_node_to_last_received(r, node);
		}
		rel_output(r);
	} else if (packet->seqno
			< r->receiving_window->seqno_next_packet_expected) { /* receive a data packet with a seqno less than expected, resend previous ack */
		//TODO: #11 Resending ACK
		if (debug)
			printf("sending ack with ackno %d\n",
					r->receiving_window->seqno_next_packet_expected);

		send_ack_pck(r, r->receiving_window->seqno_next_packet_expected);
	}
}

struct timeval* get_current_time() {
	struct timeval* current_time = (struct timeval*) malloc(
			sizeof(struct timeval));
	if (gettimeofday(current_time, NULL) == -1) {
		perror("Error generated from getting current time in rel_timer\n");
	}
	return current_time;
}

/**
 * append a packet node to the last of a send sliding window
 */
void append_node_to_last_sent(rel_t *r, struct packet_node* node) {
	r->sending_window->seqno_last_packet_sent = node->packet->seqno;
	if (r->sending_window->last_packet_sent == NULL) {
		r->sending_window->last_packet_sent = node;
		node->next = NULL;
		node->prev = NULL;
		return;
	}
	r->sending_window->last_packet_sent->next = node;
	node->prev = r->sending_window->last_packet_sent;
	r->sending_window->last_packet_sent = node;
	node->next = NULL;
}

/**
 * append a packet node to the last of a receive sliding window
 */
void append_node_to_last_received(rel_t *r, struct packet_node* node) {
	if (debug)
		printf("append node to last received with seqno %d\n",
				node->packet->seqno);

	r->receiving_window->seqno_next_packet_expected = node->packet->seqno + 1;
	if (r->receiving_window->last_packet_received == NULL) {
		r->receiving_window->last_packet_received = node;
		node->next = NULL;
		node->prev = NULL;
		return;
	}
	r->receiving_window->last_packet_received->next = node;
	node->prev = r->receiving_window->last_packet_received;
	r->receiving_window->last_packet_received = node;
	node->next = NULL;
}

struct sliding_window_send * init_sending_window() {
	struct sliding_window_send * window = (struct sliding_window_send *) malloc(
			sizeof(struct sliding_window_send));
	window->seqno_last_packet_acked = 0;
	window->seqno_last_packet_sent = 0;
	window->last_packet_sent = NULL;
	return window;
}

struct sliding_window_receive * init_receiving_window() {
	struct sliding_window_receive * window =
			(struct sliding_window_receive *) malloc(
					sizeof(struct sliding_window_receive));
	window->seqno_last_packet_outputted = 0;
	window->seqno_next_packet_expected = INIT_SEQ_NUM;
	window->last_packet_received = NULL;
	return window;
}

void destroy_sending_window(struct sliding_window_send* window) {
	struct packet_node * node = window->last_packet_sent;
	while (node) {
		struct packet_node * cur = node;
		free(cur->packet);
		node = node->next;
		free(cur);
	}
	free(window);
}

void destory_receiving_window(struct sliding_window_receive* window) {
	struct packet_node * node = window->last_packet_received;
	while (node) {
		struct packet_node * cur = node;
		free(cur->packet);
		node = node->next;
		free(cur);
	}
	free(window);
}

int is_greater_than(struct timeval* time1, int millisec2) {
	int millisec1 = time1->tv_sec * 1000 + time1->tv_usec / 1000;
	return millisec1 > millisec2;
}

/*
 * computing its checksum and comparing to the checksum in the packet.
 * Returns 1 if packet is corrupted and 0 if it is not.
 *  @author
 */
int is_pkt_corrupted(packet_t* packet, size_t pkt_length) {
	size_t packetLength = ntohs(packet->len);
	/* If we received fewer bytes than the packet's size declare corruption. */
	if (pkt_length < packetLength) {
		if (debug)
			printf("Packet size isn't right. \n");
		return 1;
	}
	uint16_t expectedChecksum = packet->cksum;
	uint16_t computedChecksum = get_check_sum(packet, packetLength);
//	printf("Expecting cksum: %d, got: %d\n", expectedChecksum, computedChecksum);
	return expectedChecksum != computedChecksum;
}

void send_ack_pck(rel_t* r, int ack_num) {
	packet_t* ack_pck = (packet_t*) malloc(sizeof(packet_t));
	ack_pck->ackno = ack_num;
	ack_pck->len = SIZE_ACK_PACKET;
	convert_to_network_order(ack_pck);
	ack_pck->cksum = get_check_sum(ack_pck, SIZE_ACK_PACKET);
	conn_sendpkt(r->c, ack_pck, SIZE_ACK_PACKET);
	free(ack_pck);
	if (debug) {
		printf("Ack packet sent\n");
	}
}

/**
 * @param packet is in host order
 */
void send_data_pck(rel_t*r, struct packet_node* pkt_ptr,
		struct timeval* current_time) {
	packet_t * packet = pkt_ptr->packet;
	size_t pckLen = packet->len;
	convert_to_network_order(packet);
	packet->cksum = get_check_sum(packet, pckLen);
	assert(packet->cksum != 0);
	conn_sendpkt(r->c, packet, pckLen);
	pkt_ptr->time_sent = current_time;
	convert_to_host_order(packet);
	if (debug) {
		printf("IN send_data_pck, print host order packet:\n");
		print_pkt(packet, "packet", packet->len);
	}
}

struct packet_node* get_first_unread_pck(rel_t* r) {
	struct packet_node* packet_ptr = r->receiving_window->last_packet_received;

	while (packet_ptr) {
		if (packet_ptr->packet->seqno
				== r->receiving_window->seqno_last_packet_outputted + 1) {
			break;
		}
		packet_ptr = packet_ptr->prev;
	}
	return packet_ptr;
}

struct packet_node* get_first_unacked_pck(rel_t* r) {
	struct packet_node* packet_ptr = r->sending_window->last_packet_sent;
	while (packet_ptr) {
		//printf("packet seq: %d, sending window last pkt acked: %d\n", packet_ptr->packet->seqno, r->sending_window->seqno_last_packet_acked);
		if (packet_ptr->packet->seqno
				== r->sending_window->seqno_last_packet_acked + 1) {
			break;
		}
		packet_ptr = packet_ptr->prev;
	}
	return packet_ptr;
}

/* @author */
void convert_to_network_order(packet_t *packet) {
	if (packet->len != SIZE_ACK_PACKET) { /* if data packet, convert its seqno */
		packet->seqno = htonl(packet->seqno);
	}
	packet->len = htons(packet->len);
	packet->ackno = htonl(packet->ackno);
	packet->rwnd = htonl(packet->rwnd);
}

/* @author */
void convert_to_host_order(packet_t* packet) {
	if (packet->len != SIZE_ACK_PACKET) { /* if data packet, convert its seqno */
		packet->seqno = ntohl(packet->seqno);
	}
	packet->len = ntohs(packet->len);
	packet->ackno = ntohl(packet->ackno);
	packet->rwnd = ntohl(packet->rwnd);
}

/**
 * @param packet is in network byte order
 * @return checksum in network byte order
 */
uint16_t get_check_sum(packet_t *packet, int packetLength) {
	memset(&packet->cksum, 0, sizeof(packet->cksum));
	return cksum(packet, packetLength);
}

int try_end_connection(rel_t* r) {
	if (r->all_pkts_acked && r->read_EOF_from_input && r->read_EOF_from_sender
			&& r->output_all_data) {
		rel_destroy(r);
		return 1;
	}
	return 0;
}

int is_sending_window_full(rel_t* r) {
	return r->sending_window->seqno_last_packet_sent
			- r->sending_window->seqno_last_packet_acked >= r->config.window;
}

int is_EOF_pkt(packet_t* pkt) {
//	return pkt->len == SIZE_EOF_PACKETs && strlen(pkt->data) == 0;
	printf("%s\n", pkt->data);
	return pkt->len == SIZE_EOF_PACKET;
}

int is_new_ACK(uint32_t ackno, rel_t* r) {
	return ackno > r->sending_window->seqno_last_packet_acked + 1;
}

int check_all_sent_pkts_acked(rel_t* r) {
	r->all_pkts_acked = r->sending_window->seqno_last_packet_acked
			== r->sending_window->seqno_last_packet_sent;
	return r->all_pkts_acked;
}
