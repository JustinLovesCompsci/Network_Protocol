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
#define INIT_SEQ_NUM 1

struct packet_node {
	struct packet_node *next;
	struct packet_node *prev; //TODO: may not be needed
	packet_t * packet;
};

struct sliding_window_send {
	uint32_t last_packet_acked; //sequence number
	struct packet_node *last_packet_sent; //a doubly linked list of sent packets
	uint32_t last_packet_written; //sequence number
};

struct sliding_window_receive {
	uint32_t last_packet_read; //sequence number
	struct packet_node *last_packet_received; //a doubly linked list of received packets
	uint32_t next_packet_expected; //sequence number
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

// global variables
rel_t *rel_list;

// debug functions
void print_rel(rel_t *);
void print_sending_window(struct sliding_window_send*);
void print_receiving_window(struct sliding_window_receive*);
void print_config(struct config_common);

// helper functions
struct sliding_window_send * initialize_sending_window();
struct sliding_window_receive * initialize_receiving_window();
void destroy_sending_window(struct sliding_window_send*);
void destory_receiving_window(struct sliding_window_receive*);

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
 *   To output data you have received in decoded UDP packets, call
 conn_output.  The function conn_bufspace tells you how much space
 is available.  If you try to write more than this, conn_output
 may return that it has accepted fewer bytes than you have asked
 for.  You should flow control the sender by not acknowledging
 packets if there is no buffer space available for conn_output.
 The library calls rel_output when output has drained, at which
 point you can send out more Acks to get more data from the remote
 side.
 */
void rel_output(rel_t *r) {
	conn_t *c = r->c;
	size_t free_space = conn_bufspace(c);

//	int result = conn_output(c, buffer, free_space);

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
