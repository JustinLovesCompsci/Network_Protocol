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
#define SIZE_EOF_PACKET 8
#define SIZE_DATA_PCK_HEADER 12
#define SIZE_MAX_PAYLOAD 500
#define INIT_SEQ_NUM 1

/*cient states*/
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
};

struct sliding_window_send {
	uint32_t seqno_last_packet_acked; /* sequence number */
	struct packet_node *last_packet_sent; /* a doubly linked list of sent packets */
	uint32_t seqno_last_packet_sent; /* sequence number */

	struct timeval lastTransmissionTime;
};

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

/* debug functions */
void print_rel(rel_t *);
void print_sending_window(struct sliding_window_send*);
void print_receiving_window(struct sliding_window_receive*);
void print_config(struct config_common);

/* helper functions */
struct sliding_window_send * initialize_sending_window();
struct sliding_window_receive * initialize_receiving_window();
void destroy_sending_window(struct sliding_window_send*);
void destory_receiving_window(struct sliding_window_receive*);

int checkCorruptedPacket(packet_t*, size_t);
void convertToHostByteOrder(packet_t*);
void processAckPacket(rel_t*, packet_t*);
void processPacket(rel_t*, packet_t*);
uint16_t computeChecksum(packet_t*, int);

void send_ack_pck(rel_t*, int);
struct packet_node* get_first_unread_pck(rel_t*);
struct packet_node* get_first_unacked_pck(rel_t*);
packet_t *create_packet_from_conninput(rel_t *);
void prepareToTransmit(packet_t*);
void convertToNetworkByteOrder(packet_t *);
void create_and_send_ack_packet(rel_t *, uint32_t);

/*helper for client */
void save_outgoing_data_packet(rel_t *, packet_t *, int);
struct ack_packet* createAckPacket(uint32_t);

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
	r->next = rel_list;
	r->prev = &rel_list;
	if (rel_list)
		rel_list->prev = &r->next;
	rel_list = r;

	/* Do any other initialization you need here */
	r->config = *cc;
	r->sending_window = initialize_sending_window();
	r->receiving_window = initialize_receiving_window();

	print_rel(r);
	return r;
}

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
 *
 */
void rel_recvpkt(rel_t *r, packet_t *pkt, size_t n) {
	if (checkCorruptedPacket(pkt, n))
		return; //check if corrupted

	convertToHostByteOrder(pkt); // convert to host byte order

	if (pkt->len == SIZE_ACK_PACKET) {
		processAckPacket(r, pkt); // ack packet only, client side
	} else {
		processPacket(r, pkt); // data packet, server side
	}
}

// function used by client (packet sending side)
void rel_read(rel_t *relState) {
	if (relState->client_state == WAITING_INPUT_DATA) {
		/* try to read from input and create a packet */
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

			prepareToTransmit(packet);
			conn_sendpkt(relState->c, packet, (size_t) packetLength);

			/* keep record of the last packet sent */
			save_outgoing_data_packet(relState, packet, packetLength);

			free(packet);
		}
	}

}

/*
 * Call conn_bufspace to get buffer space. If not space available, no ack and no output
 * If there is space, output data (partially or fully) and send acks
 */
void rel_output(rel_t *r) {
	conn_t *c = r->c;
	size_t free_space = conn_bufspace(c);
	if (free_space == 0) {
		return; //no ack and no output if no space available
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
			packet_ptr = packet_ptr->next;
		} else { /* enough space to output only partial packet */
			*current_pck->data += bytesWritten; //NOTE: check pointer increment correctness
			current_pck->len = current_pck->len - bytesWritten;
		}
		free_space = conn_bufspace(c);
	}

	/* send ack */
	if (ackno_to_send != -1) {
		send_ack_pck(r, ackno_to_send);
	}

	//TODO: deal with EOF
}

void rel_timer() {
	/* Retransmit any packets that need to be retransmitted */

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
struct sliding_window_send * initialize_sending_window() {
	struct sliding_window_send * window = (struct sliding_window_send *) malloc(
			sizeof(struct sliding_window_send));
	window->seqno_last_packet_acked = 1;
	window->last_packet_sent = 1;
	window->last_packet_sent = NULL;
	return window;
}

struct sliding_window_receive * initialize_receiving_window() {
	struct sliding_window_receive * window =
			(struct sliding_window_receive *) malloc(
					sizeof(struct sliding_window_receive));
	window->seqno_last_packet_read = 1;
	window->seqno_next_packet_expected = 1;
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

/*
 * computing its checksum and comparing to the checksum in the packet.
 * Returns 1 if packet is corrupted and 0 if it is not.
 */
int checkCorruptedPacket(packet_t* packet, size_t pkt_length) {
	int packetLength = (int) ntohs(packet->len);
	/* If we received fewer bytes than the packet's size declare corruption. */
	if (pkt_length < (size_t) packetLength)
		return 1;

	uint16_t wantedChecksum = packet->cksum;
	uint16_t computedChecksum = computeChecksum(packet, packetLength);

	return wantedChecksum != computedChecksum;
}
void convertToHostByteOrder(packet_t* packet) {
	packet->len = ntohs(packet->len);
	packet->ackno = ntohl(packet->ackno);

	/* if the packet is a data packet it additionally has a seqno that has
	 to be converted to host byte order */
	if (packet->len != SIZE_ACK_PACKET)
		packet->seqno = ntohl(packet->seqno);
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

/*
 * processes received ack only packets which have passed the corruption check.
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
	/* if we receive a packet we have seen and processed before then just send an ack back
	 regardless on which state the server is in */
	if (packet->seqno < r->receiving_window->seqno_next_packet_expected)
		create_and_send_ack_packet(r, packet->seqno + 1);

	/* if we have received the next in-order packet we were expecting and we are waiting
	 for data packets process the packet */
	if ((packet->seqno == r->receiving_window->seqno_next_packet_expected)
			&& (r->server_state == WAITING_DATA_PACKET)) {
		/* if we received an EOF packet signal to conn_output and destroy the connection if appropriate */
		if (packet->len == SIZE_EOF_PACKET) {
			conn_output(r->c, NULL, 0);
			r->server_state = SERVER_FINISHED;
			create_and_send_ack_packet(r, packet->seqno + 1);

			/* destroy the connection only if our client has finished transmitting */
			if (r->client_state == CLIENT_FINISHED)
				rel_destroy(r);
		}
		/* we receive a non-EOF data packet, so try to flush it to conn_output */
		else {
			save_incoming_data_packet(r, packet);

			if (flush_payload_to_output(r)) {
				create_and_send_ack_packet(r, packet->seqno + 1);
				r->receiving_window->seqno_next_packet_expected = packet->seqno
						+ 1;
			} else {
				r->server_state = WAITING_TO_FLUSH_DATA;
			}
		}
	}
}

void send_ack_pck(rel_t* r, int ack_num) {
	//TODO: make sure r is not null and ack_num is not sent before, update ackno recorded in r if needed
	packet_t* ack_pck;
	ack_pck->ackno = htonl(ack_num);
	ack_pck->len = htons(SIZE_ACK_PACKET);
	ack_pck->cksum = 0;
	ack_pck->cksum = cksum(ack_pck, SIZE_ACK_PACKET);
	conn_sendpkt(r->c, ack_pck, SIZE_ACK_PACKET);
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
	packet_t *packet;
	packet = xmalloc(sizeof(*packet));

	//read one full packet's worth of data from input
	int bytesRead = conn_input(r->c, packet->data, SIZE_MAX_PAYLOAD);

	if (bytesRead == 0) // no inputt
			{
		free(packet);
		return NULL;
	}
	// create a packet else if there's some data

	// if we read an EOF create a zero byte payload, else we read normal bytes that should be declared in the len field
	packet->len =
			(bytesRead == -1) ?
					(uint16_t) SIZE_DATA_PCK_HEADER :
					(uint16_t) (SIZE_DATA_PCK_HEADER + bytesRead);
	packet->ackno = (uint32_t) 1; // not piggybacking acks, don't ack any packets
	packet->seqno = (uint32_t) (r->sending_window->seqno_last_packet_sent + 1);

	return packet;

}
/* Prepare for UDP
 * converts all necessary fields to network byte order
 * computes and writes the checksum to the cksum field
 */
void prepareToTransmit(packet_t* packet) {
	int packetLength = (int) (packet->len);

	convertToNetworkByteOrder(packet);
	packet->cksum = computeChecksum(packet, packetLength);
}

void convertToNetworkByteOrder(packet_t *packet) {
// if data packet, convert its seqno as well
	if (packet->len != SIZE_ACK_PACKET)
		packet->seqno = htonl(packet->seqno);

	packet->len = htons(packet->len);
	packet->ackno = htonl(packet->ackno);
}

// need to supply pktLength as the field might be network byte order already
uint16_t computeChecksum(packet_t *packet, int packetLength) {
	memset(&(packet->cksum), 0, sizeof(packet->cksum));
	return cksum((void*) packet, packetLength);
}

// used to create and send ack packet
void create_and_send_ack_packet(rel_t *relState, uint32_t ackno) {
	struct ack_packet *ackPacket = createAckPacket(ackno);
	int packetLength = ackPacket->len;
	prepareToTransmit((packet_t*) ackPacket);
	conn_sendpkt(relState->c, (packet_t*) ackPacket, (size_t) packetLength);
	free(ackPacket);
}

/*
 Client call only: Save a copy of the last packet if we need to retransmit.
 Note:
 a pointer to a packet is necessary
 fields are already in network byte order.
 */
void save_outgoing_data_packet(rel_t *relState, packet_t *packet,
		int packetLength) {
	memcpy(&(relState->sending_window->last_packet_sent), packet, packetLength);
	relState->sending_window->seqno_last_packet_sent = (size_t) packetLength;
	relState->sending_window->seqno_last_packet_sent += 1;
	gettimeofday(&(relState->sending_window->lastTransmissionTime), NULL); // keep track of the time of transmission
}

struct ack_packet* createAckPacket(uint32_t ackNum) {
	struct ack_packet *ackPacket;
	ackPacket = malloc(sizeof(*ackPacket));

	ackPacket->len = (uint16_t) SIZE_ACK_PACKET;
	ackPacket->ackno = ackNum;

	return ackPacket;
}
/**
 * Called by server side
 * flush the part of the last received
 */
//void flushPayloadToOutput

