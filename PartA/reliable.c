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
#define SIZE_DATA_PCK_HEADER 12
#define INIT_SEQ_NUM 1

struct packet_node {
	struct packet_node *next;
	struct packet_node *prev;
	packet_t * packet;
};

struct sliding_window_send {
	uint32_t last_packet_acked; /* sequence number */
	struct packet_node *last_packet_sent; /* a doubly linked list of sent packets */
	uint32_t last_packet_written; /* sequence number */
};

struct sliding_window_receive {
	uint32_t last_packet_read; /* sequence number */
	struct packet_node *last_packet_received; /* a doubly linked list of received packets */
	uint32_t next_packet_expected; /* sequence number */
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
void send_ack_pck(rel_t*, int);
struct packet_node* get_first_unread_pck(rel_t*);

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
 * 	check if ack only or
 *
 */
void rel_recvpkt(rel_t *r, packet_t *pkt, size_t n) {
}

void rel_read(rel_t *s) {
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
			window->last_packet_acked, window->last_packet_written);
	struct packet_node * pack = window->last_packet_sent;
	while (pack) {
		print_pkt(pack->packet, "packet", 1);
		pack = pack->next;
	}
}

void print_receiving_window(struct sliding_window_receive * window) {
	printf("Printing receiving window related info....\n");
	printf("Last packet read: %u, next packet expected: %u\n",
			window->last_packet_read, window->next_packet_expected);
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
	window->last_packet_acked = 1;
	window->last_packet_written = 1;
	window->last_packet_sent = NULL;
	return window;
}

struct sliding_window_receive * initialize_receiving_window() {
	struct sliding_window_receive * window =
			(struct sliding_window_receive *) malloc(
					sizeof(struct sliding_window_receive));
	window->last_packet_read = 1;
	window->next_packet_expected = 1;
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
				== r->receiving_window->last_packet_read + 1) {
			break;
		}
		packet_ptr = packet_ptr->prev;
	}
	return packet_ptr;
}
