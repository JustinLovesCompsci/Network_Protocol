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

#define SIZE_ACK_PACKET 8
#define SIZE_EOF_PACKET 12
#define SIZE_DATA_PCK_HEADER 12
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
	struct config_common config;
	struct sliding_window_send *sending_window;
	struct sliding_window_receive *receiving_window;
};

/* global variables */
rel_t *rel_list;
int debug = 1;
int read_EOF_from_sender = 0;
int read_EOF_from_input = 0;
int all_pkts_acked = 0;
int output_all_data = 0;

/* debug functions */
void print_rel(rel_t *);
void print_sending_window(struct sliding_window_send*);
void print_receiving_window(struct sliding_window_receive*);
void print_config(struct config_common);

/* helper functions */
struct sliding_window_send * init_sending_window();
struct sliding_window_receive * init_receiving_window();
void destroy_sending_window(struct sliding_window_send*);
void destory_receiving_window(struct sliding_window_receive*);
void send_ack_pck(rel_t*, int);
packet_t* create_EOF_packet();
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
int try_finish_sender(rel_t*);
int try_finish_receiver(rel_t*);
int is_sending_window_full(rel_t*);

/**
 * Creates a new reliable protocol session, returns NULL on failure.
 * Exactly one of c and ss should be NULL.  (ss is NULL when called
 * from rlib.c, while c is NULL when this function is called from
 * rel_demux.)
 * @author Lawrence (Aohui) Lin
 */

rel_t *
rel_create(conn_t *c, const struct sockaddr_storage *ss,
		const struct config_common *cc) {
	printf("IN rel_create\n");
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
	r->next = rel_list;
	r->prev = &rel_list;
	if (rel_list) {
		rel_list->prev = &r->next;
	}
	rel_list = r;

	/* Do any other initialization you need here */
	r->config = *cc;
	r->sending_window = init_sending_window();
	r->receiving_window = init_receiving_window();
	//print_rel(r); // debug
	return r;
}

/**
 * @author Lawrence (Aohui) Lin
 */
void rel_destroy(rel_t *r) {
	printf("IN rel_destroy\n");
	if (r->next) {
		r->next->prev = r->prev;
	}
	*r->prev = r->next;
	conn_destroy(r->c);

	/* Free any other allocated memory here */
	destroy_sending_window(r->sending_window);
	destory_receiving_window(r->receiving_window);
	free(r);
}

/* This function only gets called when the process is running as a
 * server and must handle connections from multiple clients.  You have
 * to look up the rel_t structure based on the address in the
 * sockaddr_storage passed in.  If this is a new connection (sequence
 * number 1), you will need to allocate a new conn_t using rel_create
 * ().  (Pass rel_create NULL for the conn_t, so it will know to
 * allocate a new connection.)
 */
void rel_demux(const struct config_common *cc,
		const struct sockaddr_storage *ss, packet_t *pkt, size_t len) {
}

/**
 * called by both sender and receiver
 * @author Justin (Zihao) Zhang
 */
void rel_recvpkt(rel_t *r, packet_t *pkt, size_t n) {
	printf("IN rel_recvpkt\n");
	convert_to_host_order(pkt);
	if (is_pkt_corrupted(pkt, n)) {
		printf("Received a packet that's corrupted. \n");
		return;
	}

	if (debug) {
		printf("IN rel_recvpkt, print host-order non-corrupted packet:");
		print_pkt(pkt, "packet", (int) pkt->len);
	}

	if (pkt->len == SIZE_ACK_PACKET) { /* ACK packet */
		if (debug) {
			printf("Received ACK packet");
		}
		/* update last packet acked pointer in sending window */
		if (pkt->ackno >= r->sending_window->seqno_last_packet_acked) {
			r->sending_window->seqno_last_packet_acked = pkt->ackno - 1;
		}

		if (!is_sending_window_full(r)) {
			rel_read(r);
		}

		if (r->sending_window->seqno_last_packet_acked
				== r->sending_window->seqno_last_packet_sent) {
			/* all packets sent so far have been acknowledged */
			all_pkts_acked = 1;
			try_finish_sender(r);
		} else {
			all_pkts_acked = 0;
		}

	} else { /* data packet */
		process_received_data_pkt(r, pkt);
	}
}

/**
 * sender side
 * @author Justin (Zihao) Zhang
 */
void rel_read(rel_t *relState) {
	printf("IN rel_read\n");
	for (;;) {
		if (is_sending_window_full(relState)) {
			printf("window size is full");
			return;
		}
		packet_t *packet = (packet_t*) malloc(sizeof(packet_t));
		/* read one full packet's worth of data from input */
		int bytesRead = conn_input(relState->c, packet->data, SIZE_MAX_PAYLOAD);
		read_EOF_from_input = 0;
		if (bytesRead == 0) {
			free(packet);
			printf("no date is available at input now");
			return;
		}
		if (bytesRead == -1) { /* read EOF from conn_input */
			printf("read EOF from input");
			read_EOF_from_input = 1;
			packet->len = (uint16_t) SIZE_EOF_PACKET;
		} else {
			packet->len = (uint16_t) (SIZE_DATA_PCK_HEADER + bytesRead);
		}
		packet->ackno = (uint32_t) 1; /* not piggybacking acks, don't ack any packets */
		packet->seqno =
				(uint32_t) (relState->sending_window->seqno_last_packet_sent + 1);

		/* get current send time */
		struct timeval* current_time = (struct timeval*) malloc(
				sizeof(struct timeval));
		if (gettimeofday(current_time, NULL) == -1) {
			perror("Error generated from getting current time in rel_timer");
		}

		struct packet_node* node = (struct packet_node*) malloc(
				sizeof(struct packet_node));
		node->packet = packet;
		/* send the packet */
		append_node_to_last_sent(relState, node);
		relState->sending_window->seqno_last_packet_sent = packet->seqno;
		send_data_pck(relState, node, current_time);

		if (try_finish_sender(relState)) {
			return;
		}
	}
}

/**
 * append a packet node to the last of a sent sliding window
 */
void append_node_to_last_sent(rel_t *r, struct packet_node* node) {
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
 * append a packet node to the last of a sent sliding window
 */
void append_node_to_last_received(rel_t *r, struct packet_node* node) {
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

/* called by receiver
 * process a data packet from sender
 */
void process_received_data_pkt(rel_t *r, packet_t *packet) {
	if (debug) {
		printf("Start to process received data packet");
		//printf("Packet seqno: %d, expecting: %d\n", packet->seqno, r->receiving_window->seqno_next_packet_expected);
	}

	if ((packet->seqno == r->receiving_window->seqno_next_packet_expected)) {
		/* seqno is the one expected next */
		uint32_t seqnoLastReceived =
				r->receiving_window->last_packet_received == NULL ?
						0 :
						r->receiving_window->last_packet_received->packet->seqno;
		uint32_t seqno_last_outputted =
				r->receiving_window->seqno_last_packet_outputted;
		int windowSize = r->config.window;

		//printf("seqnoLastReceived = %d, seqnoLastRead = %d, windowSize = %d\n", seqnoLastReceived, seqnoLastRead, windowSize);
		/* update receive window for the newly arrived packet */
		if ((seqnoLastReceived - seqno_last_outputted) < windowSize) {
			struct packet_node* node = (struct packet_node*) malloc(
					sizeof(struct packet_node));
			node->packet = packet;
			append_node_to_last_received(r, node);
			r->receiving_window->seqno_next_packet_expected = packet->seqno + 1;
		}
	}
}

/*
 * Call conn_bufspace to get buffer space. If not space available, no ack and no output
 * If there is space, output data (partially or fully) and send acks
 * @author Justin (Zihao) Zhang
 */
void rel_output(rel_t *r) {
	printf("IN rel_output\n");
	conn_t *c = r->c;
	size_t free_space = conn_bufspace(c);
	/* no ack and no output if no space available */
	if (free_space == 0) {
		return;
	}

	struct packet_node* packet_ptr = get_first_unread_pck(r);
	int ackno_to_send = -1;

	/* output data */
	while (packet_ptr != NULL && free_space > 0) {
		packet_t *current_pck = packet_ptr->packet;
		//printf("packet data is: %s\n", current_pck->data);
		int bytesWritten = conn_output(c, current_pck->data,
				current_pck->len - SIZE_DATA_PCK_HEADER);
		//printf("bytes written: %d\n", bytesWritten);
		if (bytesWritten < 0) {
			perror(
					"Error generated from conn_output for output a whole packet");
		}

		if (free_space > current_pck->len - SIZE_DATA_PCK_HEADER) { /* enough space to output a whole packet */
			assert(bytesWritten == current_pck->len - SIZE_DATA_PCK_HEADER);
			ackno_to_send = current_pck->seqno + 1;
			if (current_pck->len == SIZE_EOF_PACKET) { /* EOF packet */
				read_EOF_from_sender = 1;
			}
			packet_ptr = packet_ptr->next;
		} else { /* enough space to output only partial packet */
			*current_pck->data += bytesWritten; //NOTE: check pointer increment correctness
			current_pck->len = current_pck->len - bytesWritten;
		}
		free_space = conn_bufspace(c);
	}

	if (packet_ptr == NULL) {
		output_all_data = 1;
	}

	/* send ack */
	if (ackno_to_send > 1) {
		send_ack_pck(r, ackno_to_send);
		r->receiving_window->seqno_last_packet_outputted = ackno_to_send - 1;
	}
	try_finish_receiver(r);
}

/**
 * Retransmit any packets that have not been acked and exceed timeout in sender
 * @author Justin (Zihao) Zhang
 */
void rel_timer() {
//	printf("IN rel_timer\n");
	rel_t* cur_rel = rel_list;
	while (cur_rel) {
		struct packet_node* node = get_first_unacked_pck(cur_rel);
		while (node) {
			struct timeval* current_time = (struct timeval*) malloc(
					sizeof(struct timeval));
			if (gettimeofday(current_time, NULL) == -1) {
				perror(
						"Error generated from getting current time in rel_timer");
			}

			struct timeval* diff = (struct timeval*) malloc(
					sizeof(struct timeval));
			timersub(current_time, node->time_sent, diff);
			if (is_greater_than(diff, cur_rel->config.timeout)) { /* Retransmit because exceeds timeout */
				if (debug) {
					printf("Found timeout packet and start to retransmit:");
					print_pkt(node->packet, "packet", node->packet->len);
				}
				send_data_pck(cur_rel, node, current_time);
			}
			free(diff);
			node = node->next;
		}
		cur_rel = rel_list->next;
	}
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
		printf("Printing receiving window related info....\n");
		printf("Last packet read: %u, next packet expected: %u\n",
				window->seqno_last_packet_outputted,
				window->seqno_next_packet_expected);
		struct packet_node * pack = window->last_packet_received;
		while (pack) {
			print_pkt(pack->packet, "packet", pack->packet->len);
			pack = pack->next;
		}
	}
}

////////////////////////////////////////////////////////////////////////
////////////////////////// Helper functions /////////////////////////////
////////////////////////////////////////////////////////////////////////
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
 */
int is_pkt_corrupted(packet_t* packet, size_t pkt_length) {
	size_t packetLength = packet->len;
	/* If we received fewer bytes than the packet's size declare corruption. */
	if (pkt_length < packetLength) {
		return 1;
	}
	uint16_t expectedChecksum = packet->cksum;
	uint16_t computedChecksum = get_check_sum(packet, packetLength);
	return expectedChecksum != computedChecksum;
}

void send_ack_pck(rel_t* r, int ack_num) {
//TODO: make sure ack_num is not sent before
	packet_t* ack_pck = (packet_t*) malloc(sizeof(packet_t));
	ack_pck->ackno = ack_num;
	ack_pck->len = SIZE_ACK_PACKET;
	ack_pck->cksum = get_check_sum(ack_pck, SIZE_ACK_PACKET);
	convert_to_network_order(ack_pck);
	conn_sendpkt(r->c, ack_pck, SIZE_ACK_PACKET);
	free(ack_pck);
}

/**
 * @param packet is in host order
 */
void send_data_pck(rel_t*r, struct packet_node* pkt_ptr,
		struct timeval* current_time) {
	packet_t * packet = pkt_ptr->packet;
	size_t pckLen = packet->len;
	packet->cksum = get_check_sum(packet, (int) packet->len);
	assert(packet->cksum != 0);
	if (debug) {
		printf("IN send_data_pck, print host order packet");
		print_pkt(packet, "packet", packet->len);
	}
	convert_to_network_order(packet);
	conn_sendpkt(r->c, packet, pckLen);
	pkt_ptr->time_sent = current_time;
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
		if (packet_ptr->packet->seqno
				== r->sending_window->seqno_last_packet_acked + 1) {
			break;
		}
		packet_ptr = packet_ptr->prev;
	}
	return packet_ptr;
}

void convert_to_network_order(packet_t *packet) {
	if (packet->len != SIZE_ACK_PACKET) { /* if data packet, convert its seqno */
		packet->seqno = htonl(packet->seqno);
	}
	packet->cksum = htons(packet->cksum);
	packet->len = htons(packet->len);
	packet->ackno = htonl(packet->ackno);
}

void convert_to_host_order(packet_t* packet) {
	if (packet->len != SIZE_ACK_PACKET) { /* if data packet, convert its seqno */
		packet->seqno = ntohl(packet->seqno);
	}
	packet->len = ntohs(packet->len);
	packet->ackno = ntohl(packet->ackno);
	packet->cksum = ntohs(packet->cksum);
}

uint16_t get_check_sum(packet_t *packet, int packetLength) {
	packet->cksum = 0;
	return cksum(packet, packetLength);
}

packet_t * create_EOF_packet() {
	packet_t* eof_packet = (packet_t *) malloc(sizeof(packet_t));
	eof_packet->len = SIZE_EOF_PACKET;
	eof_packet->ackno = 1;
	eof_packet->seqno = 0;
	eof_packet->cksum = get_check_sum(eof_packet, SIZE_EOF_PACKET);
	return eof_packet;
}

int try_finish_sender(rel_t* r) {
	if (all_pkts_acked && read_EOF_from_input) {
		rel_destroy(r);
		return 1;
	}
	return 0;
}

int try_finish_receiver(rel_t* r) {
	if (read_EOF_from_sender && output_all_data) {
		rel_destroy(r);
		return 1;
	}
	return 0;
}

int is_sending_window_full(rel_t* r) {
	return r->sending_window->seqno_last_packet_sent
			- r->sending_window->seqno_last_packet_acked >= r->config.window;
}
