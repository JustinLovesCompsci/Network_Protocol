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
#define SIZE_ACK_PACKET 12
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
	struct packet_node* last_packet_sent; /* a doubly linked list of sent packets */
	uint32_t seqno_last_packet_sent; /* sequence number of the last sent packet */

	uint32_t receiver_window_size; /* the window size at receiver (obtained from a ack packet) */
	struct packet_node* pkt_to_retransmit_start; /* a pointer to the start packet for retransmitting in the linked list */
	struct packet_node* pkt_to_retransmit_end; /* a pointer to the end packet for retransmitting in the linked list */
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
	float congestion_window; // different from receiver_window_size; find the min of two
	int num_duplicated_ack_received;
	int num_packets_sent_in_session; /* number of packets that have been sent in the current session (RTT) */

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
int is_ACK_pkt(packet_t*);
int is_new_ACK(uint32_t, rel_t*);
int check_all_sent_pkts_acked(rel_t*);
struct timeval* get_current_time();
int try_end_connection(rel_t*);
void prepare_slow_start(rel_t*);
struct packet_node* get_receiver_EOF_node(rel_t*);
int send_retransmit_pkts(rel_t*);
int is_window_available_to_send_one(rel_t*);
int min(int, int);
void send_eof_pck(rel_t*, struct packet_node*, struct timeval*);
uint32_t get_current_buffer_size(rel_t *);
int is_congestion_window_full(rel_t*);
int is_retransmitting(rel_t*);
void send_initial_eof(rel_t*);
int is_duplicate_ACK(rel_t*, packet_t*);
void prepare_congestion_avoidance(rel_t*);
void increase_congestion_window_by_mode(rel_t*);
int get_num_retransmit_pkts(rel_t*);
void reset_retransmission_field(rel_t*);

rel_t *rel_list;

/* Creates a new reliable protocol session, returns NULL on failure.
 * Exactly one of c and ss should be NULL.  (ss is NULL when called
 * from rlib.c, while c is NULL when this function is called from
 * rel_demux.) */
rel_t *
rel_create(conn_t *c, const struct sockaddr_storage *ss,
		const struct config_common *cc) {
	if (debug)
		printf("In rel_create\n");
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
	r->num_packets_sent_in_session = 0;

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
//	if (debug)  printf("config window size: %d\n", r->config.window);
//	if (debug)  printf("timeout value: %d\n", r->config.timeout);
	r->config.timeout = 400;
	return r;
}

void rel_destroy(rel_t *r) {
//	conn_destroy(r->c);
//	if (debug)  printf("In rel_destroy\n");

	/* Free any other allocated memory here */
//	if (debug)
//		printf("IN rel_destroy\n");
//
//	if (r->next) {
//		r->next->prev = r->prev;
//	}
//	*r->prev = r->next;
	conn_destroy(r->c);

	free(r);
}

void rel_demux(const struct config_common *cc,
		const struct sockaddr_storage *ss, packet_t *pkt, size_t len) {
	//leave it blank here!!!
}

void rel_recvpkt(rel_t *r, packet_t *pkt, size_t n) {
	if (debug)
		printf("received a packet\n");
	if (is_pkt_corrupted(pkt, n)) { /* Check if packet is corrupted */
		if (debug)
			printf("Received a corrupted packet. \n");
		return;
	}

	convert_to_host_order(pkt); /* from network to host byte order */

	if (debug) {
		printf("IN rel_recvpkt, print host-order non-corrupted packet: \n");
		print_pkt(pkt, "packet", (int) pkt->len);
	}

	if (is_ACK_pkt(pkt)) { /* Received an ACK packet, called by either sender or receiver (initial EOF) */
//		if (debug)  printf("Received an ACK packet with ackno %d\n", pkt->ackno);

		if (is_duplicate_ACK(r, pkt)) { /* duplicated ACK */

//			if (debug)  printf("Received a duplicated ACK with ackno %d\n", pkt->ackno);
			r->num_duplicated_ack_received++;

			if (r->num_duplicated_ack_received >= 3) { /* triple duplication */
				prepare_congestion_avoidance(r);
				/* set fast retransmission pointers */
				r->sending_window->pkt_to_retransmit_start =
						get_first_unacked_pck(r);
				if (r->sending_window->pkt_to_retransmit_start == NULL) {
					return; /* nothing to retransmit */
				}
				reset_retransmission_field(r);

			} else { /* duplicated ACK but not triple duplication yet */
				increase_congestion_window_by_mode(r);
			}

		} else if (is_new_ACK(pkt->ackno, r)) { /* new ACK packet */
			r->num_duplicated_ack_received = 1;
			r->num_packets_sent_in_session = 0;
			increase_congestion_window_by_mode(r);
			process_received_ack_pkt(r, pkt);
		} else {
//			if (debug)  printf("Received an old but not duplicated ACK\n");
		}
	} else { /* data (including EOF) packet */
		process_received_ack_pkt(r, pkt);
		process_received_data_pkt(r, pkt);
	}

//	if (r->c->sender_receiver == RECEIVER){
//		if (try_end_connection(r)) {
//			return;
//		}
//	}
}

/**
 * called after receive an ACK without triple duplication
 */
void increase_congestion_window_by_mode(rel_t* r) {
	if (r->congestion_window < r->ssthresh) { /* in slow start */
		r->congestion_window++;
	} else if (r->congestion_window > 0) {
		r->congestion_window += 1 / (r->congestion_window); /* in congestion avoidance */
	}
}

/**
 * called by receiver
 */
void send_initial_eof(rel_t* relState) {
	/* initialize packet node */
	struct packet_node* node = (struct packet_node*) malloc(
			sizeof(struct packet_node));
	packet_t* EOF_pack = (packet_t*) malloc(sizeof(packet_t));
	EOF_pack->len = SIZE_EOF_PACKET;
	EOF_pack->rwnd = relState->config.window;
	EOF_pack->ackno = 0;
	EOF_pack->seqno = INIT_SEQ_NUM;
	node->packet = EOF_pack;
	/* send the packet */
	append_node_to_last_sent(relState, node);
	struct timeval* current_time = get_current_time();
	send_eof_pck(relState, node, current_time);
	relState->has_sent_EOF_packet = 1;
}

void rel_read(rel_t *relState) {
	if (debug) {
		printf("In rel_read\n");
	}

	if (relState->c->sender_receiver == RECEIVER) {
		if (relState->has_sent_EOF_packet == 1) {
			return; // waiting for packet
		} else {
			send_initial_eof(relState);
		}
	} else /* sender mode */
	{
		for (;;) {
			if (is_sending_window_full(relState)
					|| is_congestion_window_full(relState)
					|| is_retransmitting(relState)
					|| relState->read_EOF_from_input) {
				if (debug) {
					printf(
							"Abort generating packet-> is retransmitting: %d, sending window full: %d, congestion window full: %d, "
									"read EOF before from input: %d\n",
							is_retransmitting(relState),
							is_sending_window_full(relState),
							is_congestion_window_full(relState),
							relState->read_EOF_from_input);
				}
				try_end_connection(relState);
				return;
			}
			packet_t *packet = (packet_t*) malloc(sizeof(packet_t));
			int bytesRead = conn_input(relState->c, packet->data,
			SIZE_MAX_PAYLOAD);

			relState->read_EOF_from_input = 0;
			if (bytesRead == 0) { /* no data is read from conn_input */
				free(packet);
				if (debug) {
					printf("no data is available at input now\n");
				}
				return;
			}

			if (bytesRead == -1) { /* read EOF from conn_input */
//				if (debug)
				if (debug)
					printf("\n");
				if (debug)
					printf("**************************Read EOF from input\n");
				if (debug)
					printf("\n");
				relState->read_EOF_from_input = 1;
				packet->len = (uint16_t) SIZE_EOF_PACKET;

			} else { /* read some data from conn_input */
				packet->len = (uint16_t) (SIZE_DATA_PCK_HEADER + bytesRead);
				relState->read_EOF_from_input = 0;
			}
			packet->ackno =
					relState->receiving_window->seqno_last_packet_outputted + 1;
			packet->seqno =
					(uint32_t) (relState->sending_window->seqno_last_packet_sent
							+ 1);
			packet->rwnd = relState->config.window;

			/* initialize packet node */
			struct packet_node* node = (struct packet_node*) malloc(
					sizeof(struct packet_node));
			packet_t * pack = (packet_t *) malloc(sizeof(packet_t));
			memcpy(pack, packet, sizeof(packet_t));
			node->packet = pack;

			/* send the packet */
			append_node_to_last_sent(relState, node);
			struct timeval* current_time = get_current_time();
			send_data_pck(relState, node, current_time);

			if (try_end_connection(relState)) {
				return;
			}
		}
	}
}

void rel_output(rel_t *r) {
	//	if (debug)  printf("IN rel_output\n");
	conn_t *c = r->c;
	size_t free_space = conn_bufspace(c);
	if (free_space == 0) { /* no ack and no output if no space available */
		return;
	}

	struct packet_node* packet_ptr = get_first_unread_pck(r);
	int ackno_to_send = -1;

	/* output data */
	while (packet_ptr != NULL && free_space > 0) {
		packet_t *current_pck = packet_ptr->packet;
		assert(current_pck != NULL);
		int bytesWritten = conn_output(c, current_pck->data,
				current_pck->len - SIZE_DATA_PCK_HEADER);
		if (bytesWritten < 0) {
			perror(
					"Error generated from conn_output for output a whole packet");
		}

		if (free_space > current_pck->len - SIZE_DATA_PCK_HEADER) { /* flushed completely a whole packet */
			assert(bytesWritten == current_pck->len - SIZE_DATA_PCK_HEADER);
			ackno_to_send = current_pck->seqno + 1;

			if (is_EOF_pkt(current_pck)) { /* EOF packet */
				if (debug)
					printf("rel_output: read EOF from sender\n");
				r->read_EOF_from_sender = 1;
			}
			packet_ptr = packet_ptr->next;

		} else { /* flushed only partial packet */
			*current_pck->data += bytesWritten; //NOTE: check pointer increment correctness
			current_pck->len = current_pck->len - bytesWritten;
		}
		free_space = conn_bufspace(c);
	}

	if (packet_ptr == NULL) {
		r->output_all_data = 1;
	}

	if (debug)
		printf("In rel_output: ackno_to_send is %d\n", ackno_to_send);

	/* send ack */
	if (ackno_to_send > 1) {
		send_ack_pck(r, ackno_to_send);
		r->receiving_window->seqno_last_packet_outputted = ackno_to_send - 1;
	}
	try_end_connection(r);
}

/**
 * Retransmit any packets that have not been acked and exceed timeout in sender
 * If so, perform slow start and retransmit the packets
 */
void rel_timer() {
//	if (debug)  printf("In rel_timer\n");
	rel_t* cur_rel = rel_list;

	while (cur_rel) {
		// receiver send initial eof

		if (cur_rel->has_sent_EOF_packet
				== 0&& cur_rel->c->sender_receiver == RECEIVER) {
			send_initial_eof(cur_rel);
			return;
		}

		struct packet_node* node = get_first_unacked_pck(cur_rel);

		while (node) {
			struct timeval* current_time = get_current_time();
			struct timeval* diff = (struct timeval*) malloc(
					sizeof(struct timeval));
			timersub(current_time, node->time_sent, diff);
			//if (debug)  printf("diff is %d:%d\n", diff->tv_sec, diff->tv_usec);

			if (is_greater_than(diff, cur_rel->config.timeout)) { /* Retransmit because exceeds timeout */
				free(current_time);
				free(diff);

				if (debug) {
					printf("Found timeout packet and start to retransmit:");
					print_pkt(node->packet, "packet", node->packet->len);
				}
				cur_rel->sending_window->pkt_to_retransmit_start = node;
				reset_retransmission_field(cur_rel);
				assert(is_retransmitting(cur_rel));
				prepare_slow_start(cur_rel);
				break;
			}
			node = node->next;
		}

		send_retransmit_pkts(cur_rel);
		cur_rel = rel_list->next;
	}
}

void reset_retransmission_field(rel_t* r) {
	r->sending_window->pkt_to_retransmit_end =
			r->sending_window->last_packet_sent;
	r->num_packets_sent_in_session -= get_num_retransmit_pkts(r);
	r->num_packets_sent_in_session =
			(r->num_packets_sent_in_session < 0) ?
					0 : r->num_packets_sent_in_session;
}

int get_num_retransmit_pkts(rel_t* r) {
	return r->sending_window->pkt_to_retransmit_end
			- r->sending_window->pkt_to_retransmit_start + 1;
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
		if (debug)
			printf(
					"CONFIG info: Window size: %d, timer: %d, timeout: %d, single_connection: %d\n",
					c.window, c.timer, c.timeout, c.single_connection);
	}
}

void print_sending_window(struct sliding_window_send * window) {
	if (debug) {
		if (debug)
			printf("Printing sending window related info....\n");
		if (debug)
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

uint32_t get_current_buffer_size(rel_t * relState) {
	if (relState->c->sender_receiver == SENDER) {
		return (relState->sending_window->seqno_last_packet_sent
				- relState->sending_window->seqno_last_packet_acked);
	} else { /* receiver */
		return (relState->receiving_window->seqno_next_packet_expected
				- relState->receiving_window->seqno_last_packet_outputted - 1);
	}
}

int is_retransmitting(rel_t* r) {
	return r->sending_window->pkt_to_retransmit_start != NULL;
//	return 0;
}

int is_duplicate_ACK(rel_t* r, packet_t* pkt) {
	return r->sending_window->seqno_last_packet_acked + 1 == pkt->ackno;
}

/**
 * send unfinished retransmit packets
 * @return 1 if finished re-sending all, 0 if not
 */
int send_retransmit_pkts(rel_t* r) {
	while (is_retransmitting(r)) {
		if (debug)
			printf(
					"Start/continue retransmitting packets and window is available or not: %d\n",
					is_window_available_to_send_one(r));
		if (is_window_available_to_send_one(r)) {

			send_data_pck(r, r->sending_window->pkt_to_retransmit_start,
					get_current_time());

			/* check if retransmission is finished*/
			if (r->sending_window->pkt_to_retransmit_start
					== r->sending_window->pkt_to_retransmit_end) {
				r->sending_window->pkt_to_retransmit_start = NULL;
				r->sending_window->pkt_to_retransmit_end = NULL;
				break;
			}

			r->sending_window->pkt_to_retransmit_start =
					r->sending_window->pkt_to_retransmit_start->next;
		} else {
			break;
		}
	}
	return !is_retransmitting(r);
}

int min(int a, int b) {
	return (a < b) ? a : b;
}

int is_window_available_to_send_one(rel_t *r) {
	int available = min((int) r->congestion_window,
			r->sending_window->receiver_window_size);
	available = available - r->num_packets_sent_in_session;
	return available >= 1;
}

void prepare_slow_start(rel_t* r) {
	r->ssthresh = (int) r->congestion_window / 2;
	if (r->ssthresh <= 0) {
		r->ssthresh = 1;
	}
	r->congestion_window = 1;
	r->num_packets_sent_in_session = 0;
}

void prepare_congestion_avoidance(rel_t* r) {
	r->ssthresh = (int) r->congestion_window / 2;
	if (r->ssthresh <= 0) {
		r->ssthresh = 1;
	}
	r->congestion_window = r->ssthresh;
	if (debug)
		printf(
				"Preparing congestion avoidance: congestion window = ssthresh = %f ",
				r->congestion_window);

	assert(r->congestion_window != 0);
	r->num_packets_sent_in_session = 0;
}

void process_received_ack_pkt(rel_t *r, packet_t *pkt) {
	if (debug) {
		printf("Process packet for ACK\n");
	}

	/* update last packet acked pointer in sending window if new ack arrives */
	if (is_new_ACK(pkt->ackno, r)) {
		if (debug)
			printf("Received a new ACK with ackno %d\n", pkt->ackno);
		r->sending_window->seqno_last_packet_acked = pkt->ackno - 1;
		r->sending_window->receiver_window_size = pkt->rwnd;
		check_all_sent_pkts_acked(r);
		rel_read(r);
	}
}

/* called by receiver
 * process a data packet from sender
 */
void process_received_data_pkt(rel_t *r, packet_t *packet) {

	if (debug) {
		printf("Start to process received data packet...\n");
	}

	if (debug)
		printf("Process received data packet with seqno: %d, expecting: %d\n",
				packet->seqno, r->receiving_window->seqno_next_packet_expected);

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

		if (debug)
			printf("Re-sending ACK with ackno %d\n",
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
//
//void append_to_last_retransmit(struct retransmit_node* node, )

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

struct sliding_window_send * init_sending_window(rel_t* relState) {
	struct sliding_window_send * window = (struct sliding_window_send *) malloc(
			sizeof(struct sliding_window_send));
	window->seqno_last_packet_acked = 0;
	window->seqno_last_packet_sent = 0;
	window->last_packet_sent = NULL;
	window->pkt_to_retransmit_start = NULL;
	window->receiver_window_size = INT_MAX;
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
	assert(millisec2 > 0);
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
	if (debug)
		printf("Expecting cksum: %d, got: %d\n", expectedChecksum,
				computedChecksum);
	return expectedChecksum != computedChecksum;
}

void send_ack_pck(rel_t* r, int ack_num) {
	packet_t* ack_pck = (packet_t*) malloc(sizeof(packet_t));
	ack_pck->ackno = ack_num;
	ack_pck->len = SIZE_ACK_PACKET;
	ack_pck->rwnd = r->config.window; //TODO: check if this field is the receiver window fixed size
	if (debug)
		printf("ack_pack rwnd: %d\n", ack_pck->rwnd);
	convert_to_network_order(ack_pck);
	ack_pck->cksum = get_check_sum(ack_pck, SIZE_ACK_PACKET);
	conn_sendpkt(r->c, ack_pck, SIZE_ACK_PACKET);
	free(ack_pck);
	if (debug) {
		printf("Ack packet sent\n");
	}
}

/**
 * called by sender side
 * @param packet is in host order
 */
void send_data_pck(rel_t*r, struct packet_node* pkt_ptr,
		struct timeval* current_time) {
	packet_t * packet = pkt_ptr->packet;
	size_t pckLen = packet->len;
	convert_to_network_order(packet);
	packet->cksum = get_check_sum(packet, pckLen);
	assert(packet->cksum != 0);
	if (debug)
		printf("sending packet with seqno: %d\n", ntohl(packet->seqno));
	conn_sendpkt(r->c, packet, pckLen);
	pkt_ptr->time_sent = current_time;
	r->num_packets_sent_in_session = r->num_packets_sent_in_session + 1;

	convert_to_host_order(packet);
	if (debug) {
		printf("IN send_data_pck, print host order packet:\n");
		print_pkt(packet, "packet", packet->len);
	}
}

// called by receiver side
void send_eof_pck(rel_t*r, struct packet_node* pkt_ptr,
		struct timeval* current_time) {
	packet_t * packet = pkt_ptr->packet;
	size_t pckLen = packet->len;
	convert_to_network_order(packet);
	packet->cksum = get_check_sum(packet, pckLen);
	assert(packet->cksum != 0);
	conn_sendpkt(r->c, packet, pckLen);
	pkt_ptr->time_sent = current_time;
	convert_to_host_order(packet); // why at the end need to convert back
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
		if (debug)
			printf("packet seq: %d, sending window last pkt acked: %d\n",
					packet_ptr->packet->seqno,
					r->sending_window->seqno_last_packet_acked);
		if (packet_ptr->packet->seqno
				== r->sending_window->seqno_last_packet_acked + 1) {
			break;
		}
		packet_ptr = packet_ptr->prev;
	}
	return packet_ptr;
}

void convert_to_network_order(packet_t *packet) {
	if (!is_ACK_pkt(packet)) {
		packet->seqno = htonl(packet->seqno);
	}
	packet->len = htons(packet->len);
	packet->ackno = htonl(packet->ackno);
	packet->rwnd = htonl(packet->rwnd);
}

void convert_to_host_order(packet_t* packet) {
	if (!is_ACK_pkt(packet)) {
		packet->seqno = ntohl(packet->seqno);
	}
	packet->len = ntohs(packet->len);
	packet->ackno = ntohl(packet->ackno);
	packet->rwnd = ntohl(packet->rwnd);
}

/**
 * Calculate the checksum of a packet
 * @param packet is in network byte order
 * @return checksum in network byte order
 */
uint16_t get_check_sum(packet_t *packet, int packetLength) {
	memset(&packet->cksum, 0, sizeof(packet->cksum));
	return cksum(packet, packetLength);
}

/*
 * Check if all the conditions for ending a connection have been met
 * If so, destroy connection and return 1
 * Else return 0
 */
int try_end_connection(rel_t* r) {
	if (r->c->sender_receiver == SENDER) {
		if (debug)
			printf(
					"--> try end connection: all pkts acked %d , readEOF_from input %d, output all data %d\n",
					r->all_pkts_acked, r->read_EOF_from_input,
					r->output_all_data);

		if (r->all_pkts_acked && r->read_EOF_from_input && r->output_all_data) {
			rel_destroy(r);
			return 1;
		}
		return 0;
	} else { // receiver
		if (r->read_EOF_from_sender) {

			rel_destroy(r);
			return 1;
		}
		return 0;
	}
}

/*
 * Check if the sending window is full
 */
int is_sending_window_full(rel_t* r) {
//	if (debug)  printf(
//			"Check sending window: last packet sent: %d, last packet acked: %d, config window size: %d, receiver window size: %d\n",
//			r->sending_window->seqno_last_packet_sent,
//			r->sending_window->seqno_last_packet_acked, r->config.window,
//			r->sending_window->receiver_window_size);
	return r->sending_window->seqno_last_packet_sent
			- r->sending_window->seqno_last_packet_acked
			>= min(r->config.window, r->sending_window->receiver_window_size);
}

/*
 * Check if the congestion window is full
 */
int is_congestion_window_full(rel_t* r) {
//	if (debug)  printf(
//			"Check congestion window: last packet sent: %d, last packet acked: %d, congestion window size: %f\n",
//			r->sending_window->seqno_last_packet_sent,
//			r->sending_window->seqno_last_packet_acked, r->congestion_window);
	return r->num_packets_sent_in_session >= r->congestion_window;
}

int is_EOF_pkt(packet_t* pkt) {
//TODO: check more than length
	return pkt->len == SIZE_EOF_PACKET;
}

/*
 * Check if a given packet is an ACK packet
 */
int is_ACK_pkt(packet_t * pkt) {
//TODO: check more than length
	return pkt->len == SIZE_ACK_PACKET;
}

int is_new_ACK(uint32_t ackno, rel_t* r) {
	return ackno > r->sending_window->seqno_last_packet_acked + 1;
}

int check_all_sent_pkts_acked(rel_t* r) {
//	if (debug)  printf("seqno last acked: %u \n", r->sending_window->seqno_last_packet_acked);
//	if (debug)  printf("seqno last sent: %u \n", r->sending_window->seqno_last_packet_sent);
	r->all_pkts_acked = r->sending_window->seqno_last_packet_acked
			== r->sending_window->seqno_last_packet_sent;
	return r->all_pkts_acked;
}

struct packet_node* get_receiver_EOF_node(rel_t* r) {
	return r->sending_window->last_packet_sent;
}
