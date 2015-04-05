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

/*client states*/
#define WAITING_INPUT_DATA 0
#define WAITING_ACK 1
#define WAITING_EOF_ACK 2
#define CLIENT_FINISHED 3

/*server states*/
#define WAITING_DATA_PACKET 0
#define WAITING_TO_FLUSH_DATA 1
#define SERVER_FINISHED 2

struct packet_node {
	struct packet_node *next;
	struct packet_node *prev;
	packet_t * packet;
	struct timeval* time_sent;
};

struct sliding_window_send {
	uint32_t seqno_last_packet_acked; /* sequence number */
	struct packet_node *last_packet_sent; /* a doubly linked list of sent packets */
	uint32_t seqno_last_packet_sent; /* sequence number */
};

/**
 * seqno_last_packet_expected refers to the next packet expected to be received (always one greater than seqno_last_packet_received
 */
struct sliding_window_receive {
	uint32_t seqno_last_packet_read; /* sequence number */
	struct packet_node *last_packet_received; /* a doubly linked list of received packets */
	uint32_t seqno_next_packet_expected; /* sequence number */
};

struct reliable_state {
	rel_t *next; /* Linked list for traversing all connections */
	rel_t **prev;

	conn_t *c; /* This is the connection object */

	int client_state; /*cient state*/
	int server_state; /*server state*/

	/* Add your own data fields below this */
	struct config_common config;
	struct sliding_window_send *sending_window;
	struct sliding_window_receive *receiving_window;
};

/* global variables */
rel_t *rel_list;
int debug = 1;

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
packet_t * create_EOF_packet();
int isGreaterThan(struct timeval*, int);
void send_data_pck(rel_t*, struct packet_node*, struct timeval*);

int isPacketCorrupted(packet_t*, size_t);
void convertToHostByteOrder(packet_t*);
void processAckPacket(rel_t*, packet_t*);
void processPacket(rel_t*, packet_t*);
uint16_t computeChecksum(packet_t*, int);

struct packet_node* get_first_unread_pck(rel_t*);
struct packet_node* get_first_unacked_pck(rel_t*);
packet_t *create_packet_from_conninput(rel_t *);
void prepareToTransmit(packet_t*);
void convertToNetworkByteOrder(packet_t *);

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

	print_rel(r); // debug
	return r;
}

/**
 * @author Lawrence (Aohui) Lin
 */
void rel_destroy(rel_t *r) {
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

/*method called by both server and client, responsible for:
 * 	checking checksum for checking corrupted data (drop if corrupted)
 * 	convert to host byte order
 * 	check if ack only or actual data included,
 * 		and pass the packet to corresponding handler
 * @author Steve (Siyang) Wang
 */
void rel_recvpkt(rel_t *r, packet_t *pkt, size_t n) {
	if (isPacketCorrupted(pkt, n)) {
		return; //check if corrupted
	}

	// print out packet content for debugging purpose
	char buffer [1000];
	print_pkt (pkt, buffer, sizeof(pkt));

	convertToHostByteOrder(pkt); // convert to host byte order

	if (pkt->len == SIZE_ACK_PACKET) {
		processAckPacket(r, pkt); // ack packet only, client side
	} else {
		processPacket(r, pkt); // data packet, server side
	}
}

/**
 * function used by client (packet sending side)
 * @author Steve (Siyang) Wang
 */
void rel_read(rel_t *relState) {
	if (relState->client_state == WAITING_INPUT_DATA) {
		packet_t *packet = create_packet_from_conninput(relState);

		/* in case there was data in the input and a packet was created, proceed to process, save
		 and send the packet */
		if (packet != NULL) {
			int packetLength = packet->len;

			/* change client state according to whether we are sending EOF packet or normal packet */
			relState->client_state =
					(packetLength == SIZE_EOF_PACKET) ?
					WAITING_EOF_ACK :
														WAITING_ACK;

			/* get current send time */
			struct timeval* current_time = (struct timeval*) malloc(
					sizeof(struct timeval));
			if (gettimeofday(current_time, NULL) == -1) {
				perror(
						"Error generated from getting current time in rel_timer");
			}

			struct packet_node* node = (struct packet_node*) malloc(
					sizeof(struct packet_node));
			node->packet = packet;

			appendPacketNodeToLastSent(relState, node);
			relState->sending_window->seqno_last_packet_sent = packet->seqno;
			send_data_pck(relState, node, current_time);
		}
	}
}

/**
 * append a packet node to the last of a sent sliding window
 */
void appendPacketNodeToLastSent(rel_t *r, struct packet_node* node) {
	r->sending_window->last_packet_sent->next = node;
	node->prev = r->sending_window->last_packet_sent;
	r->sending_window->last_packet_sent = node;
}

void processAckPacket(rel_t* r, packet_t* pkt) {
	process_ack(r, (packet_t*) pkt);
}

/* Server should process the data part of the packet
 * client should process ack part of the packet. */
void processPacket(rel_t* r, packet_t* pkt) {

	/* Pass the packet to the server piece to process the data packet */
	process_data_packet(r, pkt);

	/* Pass the packet to the client piece to process the ackno field */
	process_ack(r, pkt);
}

/* processes received ack only packets which have passed the corruption check.
 This functionality belongs to the client and server piece.
 */
void process_ack(rel_t *r, packet_t *packet) {
	/* proceed only if we are waiting for an ack */
	if (r->client_state == WAITING_ACK) {
		/* received ack for last normal packet sent, go back to waiting for input
		 and try to read */
		if (packet->ackno == r->sending_window->seqno_last_packet_sent + 1) {
			r->client_state = WAITING_INPUT_DATA;
			rel_read(r);
		}
	} else if (r->client_state == WAITING_EOF_ACK) {
		/* received ack for EOF packet, enter declare client connection to be finished */
		if (packet->ackno == r->sending_window->seqno_last_packet_sent + 1) {
			r->client_state = CLIENT_FINISHED;

			/* destroy the connection only if the other side's client has finished transmitting */
			if (r->server_state == SERVER_FINISHED)
				rel_destroy(r);
		}
	}
}

/* This function processes a data packet.
 * This is functionality of the server piece. */
void process_data_packet(rel_t *r, packet_t *packet) {
	// if we receive a packet we have seen and processed before then just send an ack bacl regardless on which state the server is in

	/* if we have received the next in-order packet we were expecting and we are waiting
	 for data packets process the packet */
	if ((packet->seqno == r->receiving_window->seqno_next_packet_expected)
			&& (r->server_state == WAITING_DATA_PACKET)) { // check if needed to do status check

		/* if we received an EOF packet signal to conn_output and destroy the connection if appropriate */
		if (packet->len == SIZE_EOF_PACKET) {
//			conn_output(r->c, NULL, 0); zihao part moved
			r->server_state = SERVER_FINISHED;
//			create_and_send_ack_packet(r, packet->seqno + 1);

			/* destroy the connection only if our client has finished transmitting */
			if (r->client_state == CLIENT_FINISHED)
				rel_destroy(r);
		}
		// we receive a non-EOF data packet, check receiving window size, and append
		else {
			//check if receiving window size less
			uint32_t seqnoLastReceived =
					r->receiving_window->last_packet_received->packet->seqno;
			uint32_t seqnoLastRead = r->receiving_window->seqno_last_packet_read;
			int windowSize = r->config.window;

			// keep track of last sent data
			if ((seqnoLastReceived - seqnoLastRead + 1) < windowSize) {
				r->receiving_window->last_packet_received->next = packet;
			}
			// system will handle sending ack

			//	TODO if server flush output succeeded, not change state, if flush data failed, change state to waiting to flush
			r->server_state = WAITING_TO_FLUSH_DATA;
		}
	}
}

/*
 * Call conn_bufspace to get buffer space. If not space available, no ack and no output
 * If there is space, output data (partially or fully) and send acks
 * @author Justin (Zihao) Zhang
 */
void rel_output(rel_t *r) {
	conn_t *c = r->c;
	size_t free_space = conn_bufspace(c);
	/* no ack and no output if no space available */
	if (free_space == 0) {
		return;
	}

	struct packet_node* packet_ptr = get_first_unread_pck(r);
	int ackno_to_send = -1;

	/* output data */
	while (packet_ptr && free_space > 0) {
		packet_t *current_pck = packet_ptr->packet;
		int bytesWritten = conn_output(c, current_pck->data,
				current_pck->len - SIZE_DATA_PCK_HEADER);
		if (bytesWritten < 0) {
			perror(
					"Error generated from conn_output for output a whole packet");
		}

		if (free_space > current_pck->len - SIZE_DATA_PCK_HEADER) { /* enough space to output a whole packet */
			assert(bytesWritten == current_pck->len - SIZE_DATA_PCK_HEADER);
			ackno_to_send = current_pck->seqno + 1;
			if (current_pck->len == SIZE_EOF_PACKET) { /* EOF packet */
				r->server_state = SERVER_FINISHED;
				if (r->client_state == CLIENT_FINISHED) { //TODO: what's the need for this?
					rel_destroy(r);
				}
				break;
			}
			packet_ptr = packet_ptr->next;
		} else { /* enough space to output only partial packet */
			*current_pck->data += bytesWritten; //NOTE: check pointer increment correctness
			current_pck->len = current_pck->len - bytesWritten;
		}
		free_space = conn_bufspace(c);
	}

	/* send ack */
	if (ackno_to_send > 1) {
		send_ack_pck(r, ackno_to_send);
		r->receiving_window->seqno_last_packet_read = ackno_to_send - 1;
	}
}

/**
 * Retransmit any packets that have not been acked and exceed timeout in sender
 * @author Justin (Zihao) Zhang
 */
void rel_timer() {
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
			if (isGreaterThan(diff, cur_rel->config.timeout)) { /* Retransmit because exceeds timeout */
				if (debug) {

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
	print_config(rel->config);
	print_sending_window(rel->sending_window);
	print_receiving_window(rel->receiving_window);
}

void print_config(struct config_common c) {
	printf(
			"CONFIG info: Window size: %d, timer: %d, timeout: %d, single_connection: %d\n",
			c.window, c.timer, c.timeout, c.single_connection);
}

void print_sending_window(struct sliding_window_send * window) {
	printf("Printing sending window related info....\n");
	printf("Last packet acked: %u, last packet written: %u\n",
			window->seqno_last_packet_acked, window->seqno_last_packet_sent);
	struct packet_node * pack = window->last_packet_sent;
	while (pack) {
		print_pkt(pack->packet, "packet", 1);
		pack = pack->next;
	}
}

void print_receiving_window(struct sliding_window_receive * window) {
	printf("Printing receiving window related info....\n");
	printf("Last packet read: %u, next packet expected: %u\n",
			window->seqno_last_packet_read, window->seqno_next_packet_expected);
	struct packet_node * pack = window->last_packet_received;
	while (pack) {
		print_pkt(pack->packet, "packet", 1);
		pack = pack->next;
	}
}

////////////////////////////////////////////////////////////////////////
////////////////////////// Helper functions /////////////////////////////
////////////////////////////////////////////////////////////////////////
struct sliding_window_send * init_sending_window() {
	struct sliding_window_send * window = (struct sliding_window_send *) malloc(
			sizeof(struct sliding_window_send));
	window->seqno_last_packet_acked = 0;
	window->seqno_last_packet_sent = INIT_SEQ_NUM;
	window->last_packet_sent = NULL;
	return window;
}

struct sliding_window_receive * init_receiving_window() {
	struct sliding_window_receive * window =
			(struct sliding_window_receive *) malloc(
					sizeof(struct sliding_window_receive));
	window->seqno_last_packet_read = 0;
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

int isGreaterThan(struct timeval* time1, int millisec2) {
	int millisec1 = time1->tv_sec * 1000 + time1->tv_usec / 1000;
	return millisec1 > millisec2;
}

/*
 * computing its checksum and comparing to the checksum in the packet.
 * Returns 1 if packet is corrupted and 0 if it is not.
 */
int isPacketCorrupted(packet_t* packet, size_t pkt_length) {
	int packetLength = (int) ntohs(packet->len);
	/* If we received fewer bytes than the packet's size declare corruption. */
	if (pkt_length < (size_t) packetLength) {
		return 1;
	}

	uint16_t expectedChecksum = packet->cksum;
	uint16_t computedChecksum = computeChecksum(packet, packetLength);
	return expectedChecksum != computedChecksum;
}

/* if the packet is a data packet it additionally has a seqno that has
 to be converted to host byte order */
//if (packet->len != SIZE_ACK_PACKET)
//packet->seqno = ntohl(packet->seqno);
//}
void send_ack_pck(rel_t* r, int ack_num) {
//TODO: make sure ack_num is not sent before
	packet_t* ack_pck = (packet_t*) malloc(sizeof(packet_t));
	ack_pck->ackno = ack_num;
	ack_pck->len = SIZE_ACK_PACKET;
	ack_pck->cksum = computeChecksum(ack_pck, SIZE_ACK_PACKET);
	convertToNetworkByteOrder(ack_pck);
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
	prepareToTransmit(packet);
	conn_sendpkt(r->c, packet, pckLen);
	pkt_ptr->time_sent = current_time;
}

struct packet_node* get_first_unread_pck(rel_t* r) {
	struct packet_node* packet_ptr = r->receiving_window->last_packet_received;

	while (packet_ptr) {
		if (packet_ptr->packet->seqno
				== r->receiving_window->seqno_last_packet_read + 1) {
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

/*
 * Function called to: read from conn_input, create a packet from conn_input, and return it
 * Note: if no data in conn_input: return null
 * 		 if packet created: memory is allocated (to be freed later)
 * 		 checkSum not computed here: to be computed right before packet is to be sent and converted to network order
 */
packet_t *create_packet_from_conninput(rel_t *r) {
	packet_t *packet = (packet_t*) malloc(sizeof(packet_t));

	/* read one full packet's worth of data from input */
	int bytesRead = conn_input(r->c, packet->data, SIZE_MAX_PAYLOAD);
	if (bytesRead == 0) {
		free(packet);
		return NULL;
	}
	/* create a packet else if there's some data */

	/* if we read an EOF create a zero-byte payload, else we read normal bytes that should be declared in the len field */
	packet->len =
			(bytesRead == -1) ?
					(uint16_t) SIZE_DATA_PCK_HEADER :
					(uint16_t) (SIZE_DATA_PCK_HEADER + bytesRead);
	packet->ackno = (uint32_t) 1; /* not piggybacking acks, don't ack any packets */
	packet->seqno = (uint32_t) (r->sending_window->seqno_last_packet_sent + 1);
	return packet;
}
/* Prepare for UDP
 * 1. computes and writes the checksum to the cksum field
 * 2. converts all necessary fields to network byte order
 */
void prepareToTransmit(packet_t* packet) {
	int packetLength = (int) (packet->len);
	packet->cksum = computeChecksum(packet, packetLength);
	assert(packet->cksum != 0);
	convertToNetworkByteOrder(packet);
	convertToNetworkByteOrder(packet);
	packet->cksum = computeChecksum(packet, packetLength);
}

void convertToNetworkByteOrder(packet_t *packet) {
	if (packet->len != SIZE_ACK_PACKET) { /* if data packet, convert its seqno */
		packet->seqno = htonl(packet->seqno);
	}
	packet->cksum = htons(packet->cksum);
	packet->len = htons(packet->len);
	packet->ackno = htonl(packet->ackno);
}

void convertToHostByteOrder(packet_t* packet) {
	if (packet->len != SIZE_ACK_PACKET) { /* if data packet, convert its seqno */
		packet->seqno = ntohl(packet->seqno);
	}
	packet->len = ntohs(packet->len);
	packet->ackno = ntohl(packet->ackno);
	packet->cksum = ntohs(packet->cksum);
}

// need to supply pktLength as the field might be network byte order already
uint16_t computeChecksum(packet_t *packet, int packetLength) {
	packet->cksum = 0;
	return cksum(packet, packetLength);
}

packet_t * create_EOF_packet() {
	packet_t* eof_packet = (packet_t *) malloc(sizeof(packet_t));
	eof_packet -> len = SIZE_EOF_PACKET;
	eof_packet -> ackno = 1;
	eof_packet -> seqno = 0;
	eof_packet -> cksum = computeChecksum(eof_packet, SIZE_EOF_PACKET);
	return eof_packet;
}

/**
 * Called by server side:
 * if non-EOF: flush the packet to conn_output: if succesful flush
 * flush the part of the last received
 */

//void flushPayloadToOutput
