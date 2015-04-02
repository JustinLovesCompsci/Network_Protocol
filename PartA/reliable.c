
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

struct packet_list {
	struct packet_list *next;
	struct packet_list *prev; //TODO: may not be needed
};

struct sliding_window_send {
	uint32_t last_packet_acked; //sequence number
	struct packet_list last_packet_sent;
	uint32_t last_packet_written; //sequence number
};

struct sliding_window_receive {
	uint32_t last_packet_read; //sequence number
	struct packet_list last_packet_received;
	uint32_t next_packet_expected; //sequence number
};

struct reliable_state {
  rel_t *next;			/* Linked list for traversing all connections */
  rel_t **prev;

  conn_t *c;			/* This is the connection object */

  /* Add your own data fields below this */

};
rel_t *rel_list;

/* Creates a new reliable protocol session, returns NULL on failure.
 * Exactly one of c and ss should be NULL.  (ss is NULL when called
 * from rlib.c, while c is NULL when this function is called from
 * rel_demux.) */
rel_t *
rel_create (conn_t *c, const struct sockaddr_storage *ss,
	    const struct config_common *cc)
{
  rel_t *r;

  r = xmalloc (sizeof (*r));
  memset (r, 0, sizeof (*r));

  if (!c) {
    c = conn_create (r, ss);
    if (!c) {
      free (r);
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


  return r;
}

void
rel_destroy (rel_t *r)
{
  if (r->next)
    r->next->prev = r->prev;
  *r->prev = r->next;
  conn_destroy (r->c);

  /* Free any other allocated memory here */
}


/* This function only gets called when the process is running as a
 * server and must handle connections from multiple clients.  You have
 * to look up the rel_t structure based on the address in the
 * sockaddr_storage passed in.  If this is a new connection (sequence
 * number 1), you will need to allocate a new conn_t using rel_create
 * ().  (Pass rel_create NULL for the conn_t, so it will know to
 * allocate a new connection.)
 */
void
rel_demux (const struct config_common *cc,
	   const struct sockaddr_storage *ss,
	   packet_t *pkt, size_t len)
{
}

/*method called by both server and client, responsible for:
 * 	checking checksum for checking corrupted data (drop if corrupted)
 * 	convert to host byte order
 * 	check if ack only or
 *
*/
void
rel_recvpkt (rel_t *relState, packet_t *packet, size_t received_length)
{
  if (is_packet_corrupted (packet, received_length)) /* do not do anything if packet is corrupted */
    return;

  convert_packet_to_host_byte_order (packet);

  if (packet->len == ACK_PACKET_SIZE)
    process_received_ack_packet (relState, (struct ack_packet*) packet);
  else
    process_received_data_packet (relState, packet);
}


void
rel_read (rel_t *relState)
{
  if (relState->clientState == WAITING_INPUT_DATA)
  {
    /* try to read from input and create a packet */
    packet_t *packet = create_packet_from_input (relState);

    /* in case there was data in the input and a packet was created, proceed to process, save
       and send the packet */
    if (packet != NULL)
    {
      int packetLength = packet->len;

      /* change client state according to whether we are sending EOF packet or normal packet */
      relState->clientState = (packetLength == EOF_PACKET_SIZE) ? WAITING_EOF_ACK : WAITING_ACK;

      prepare_for_transmission (packet);
      conn_sendpkt (relState->c, packet, (size_t) packetLength);

      /* keep record of the last packet sent */
      save_outgoing_data_packet (relState, packet, packetLength);

      free (packet);
    }
  }
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
void
rel_output (rel_t *r)
{
	conn_t *c = r->c;
	size_t free_space = conn_bufspace(c);
//	int result = conn_output(c, buffer, free_space);


}

void
rel_timer ()
{
  /* Retransmit any packets that need to be retransmitted */

}
