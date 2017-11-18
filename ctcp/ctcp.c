/******************************************************************************
 * ctcp.c
 * ------
 * Implementation of cTCP done here. This is the only file you need to change.
 * Look at the following files for references and useful functions:
 *   - ctcp.h: Headers for this file.
 *   - ctcp_iinked_list.h: Linked list functions for managing a linked list.
 *   - ctcp_sys.h: Connection-related structs and functions, cTCP segment
 *                 definition.
 *   - ctcp_utils.h: Checksum computation, getting the current time.
 *
 *****************************************************************************/

#include "ctcp.h"
#include "ctcp_linked_list.h"
#include "ctcp_sys.h"
#include "ctcp_utils.h"

struct sender_buf {
	ctcp_segment_t *sender_seg;
	uint8_t sent_time;
	uint8_t retransmit_count;
};
typedef struct sender_buf sender_buf_t;

/**
 * Connection state.
 *
 * Stores per-connection information such as the current sequence number,
 * unacknowledged packets, etc.
 *
 * You should add to this to store other fields you might need.
 */
struct ctcp_state {
  struct ctcp_state *next;  /* Next in linked list */
  struct ctcp_state **prev; /* Prev in linked list */

  conn_t *conn;             /* Connection object -- needed in order to figure
                               out destination when sending */
  linked_list_t *segments;  /* Linked list of segments sent to this connection.
                               It may be useful to have multiple linked lists
                               for unacknowledged segments, segments that
                               haven't been sent, etc. Lab 1 uses the
                               stop-and-wait protocol and therefore does not
                               necessarily need a linked list. You may remove
                               this if this is the case for you */

  /* FIXME: Add other needed fields. */
  uint32_t seqno;
  uint32_t ackno;
  uint32_t expected_ackno;
  ctcp_config_t *cfg;

  linked_list_t *sender_buf;
  linked_list_t *receiver_buf;

  uint8_t finFlagSender;
  uint8_t finFlagRecv;
  uint8_t dropPacket;
  uint16_t windowSize;
};

/**
 * Linked list of connection states. Go through this in ctcp_timer() to
 * resubmit segments and tear down connections.
 */
static ctcp_state_t *state_list;
static int start_read = 0;

/* FIXME: Feel free to add as many helper functions as needed. Don't repeat
          code! Helper functions make the code clearer and cleaner. */


uint8_t checkForDup(linked_list_t  *receiver_buf, ctcp_segment_t *segment) {
	ll_node_t *curr = receiver_buf->head;
	while (curr != NULL) {
		ctcp_segment_t *temp_seg = curr->object;
		if (temp_seg->seqno == segment->seqno) {
			////printf("\n\nDuplicate Found\n\n");
			return 1;
		}
		curr = curr->next;
	}
	return 0;
}

uint16_t receivedBufSize(linked_list_t *receiver_buf) {
	ll_node_t *curr = receiver_buf->head;
	uint16_t size = 0;
	while (curr != NULL) {
		ctcp_segment_t *temp_seg = curr->object;
		size += (ntohs(temp_seg->len) - sizeof(ctcp_segment_t));
		curr = curr->next;
	}
	return size;
}

void addSegmentToBuffer(linked_list_t *receiver_buf, ctcp_segment_t *segment) {
	ll_node_t *curr = receiver_buf->head;;
	if(ll_length(receiver_buf) == 0) {
		ll_add_front(receiver_buf, segment);
		return;
	}
	while (curr != NULL) {
		ctcp_segment_t *temp_seg = curr->object;
		if (temp_seg->seqno > segment->seqno) {
			ll_add_after(receiver_buf, curr, segment);
			return;
		}
		curr = curr->next;
	}
	ll_add(receiver_buf, segment);
}

void sendAck(ctcp_state_t *state, ctcp_segment_t *segment) {
	ctcp_segment_t *new_segment = calloc(sizeof(ctcp_segment_t), 1);
	new_segment->seqno = htonl(state->seqno);
	if(ntohs(segment->len) == sizeof(ctcp_segment_t)) {
		new_segment->ackno = htonl(ntohl(segment->seqno) + 1);
	}
	else {
		new_segment->ackno = htonl(ntohl(segment->seqno) + ntohs(segment->len) - sizeof(ctcp_segment_t));
	}
	////printf("\nlength of bytes read is %d, %d, %d, %d\n", (int)ntohl(segment->seqno), (segment->len), ntohs(segment->len), (int)sizeof(ctcp_segment_t));
	state->ackno = ntohl(new_segment->ackno);
	new_segment->len = htons(sizeof(ctcp_segment_t));
	new_segment->flags |= htonl(ACK);
	if(state->finFlagRecv) {
		////printf("Sending fin from Receiver\n");
		new_segment->flags |= htonl(FIN);
	}
	//new_segment->window = htons(state->cfg->send_window - receivedBufSize(state->receiver_buf));
	new_segment->window = htons(state->cfg->recv_window);
	state->cfg->send_window = ntohs(segment->window);
	//printf("Calling ctcp_receive - ACK - 4 - sen_window: %d,%d\n", state->cfg->recv_window, segment->window);
	new_segment->cksum = htons(0);
	new_segment->cksum = cksum(new_segment, ntohs(new_segment->len));
	if(!state->dropPacket) {
		////printf("Calling ctcp_receive - conn_data \n");
		conn_send(state->conn, new_segment, ntohs(new_segment->len));
	}
	else{
		////printf("Calling ctcp_receive - drop_packet \n");
		state->dropPacket = 0;
	}
	free(new_segment);
}

void sendFin(ctcp_state_t *state) {
	////printf("Sending Fin\n");
	ctcp_segment_t *new_segment = calloc(sizeof(ctcp_segment_t), 1);
	new_segment->seqno = htonl(state->seqno);
	new_segment->ackno = htonl(state->ackno);

	//state->ackno = ntohl(new_segment->ackno);
	new_segment->len = htons(sizeof(ctcp_segment_t));
	new_segment->flags |= htonl(ACK);
	new_segment->flags |= htonl(FIN);
	//new_segment->window = htons(state->cfg->recv_window - receivedBufSize(state->receiver_buf));
	new_segment->window = htons(state->cfg->recv_window);
	state->seqno += 1;
	memcpy(new_segment->data,"",0);
	new_segment->cksum = htons(0);
	new_segment->cksum = cksum(new_segment, ntohs(new_segment->len));
		////printf("Calling sendFin	 - conn_data \n");
	conn_send(state->conn, new_segment, ntohs(new_segment->len));
	free(new_segment);
}

ctcp_state_t *ctcp_init(conn_t *conn, ctcp_config_t *cfg) {
  /* Connection could not be established. */
  if (conn == NULL) {
    return NULL;
  }
  ////printf("Calling ctcp_int\n");
  /* Established a connection. Create a new state and update the linked list
     of connection states. */
  ctcp_state_t *state = calloc(sizeof(ctcp_state_t), 1);
  state->next = state_list;
  state->prev = &state_list;
  if (state_list)
    state_list->prev = &state->next;
  state_list = state;

  /* Set fields. */
  state->conn = conn;
  /* FIXME: Do any other initialization here. */
  ////printf("Window size sender: %d, Receiver: %d, rt_timer: %d\n", cfg->send_window, cfg->recv_window, cfg->rt_timeout);
  state->cfg = cfg;
  ////printf("Window size sender: %d, Receiver: %d\n", state->cfg->send_window, state->cfg->recv_window);
  state->seqno = 1;
  state->ackno = 1;
  state->expected_ackno = 1;
  state->receiver_buf = ll_create();
  state->sender_buf = ll_create();
  state->windowSize = 0;
  state->dropPacket = 0;
  state->finFlagSender = 0;
  state->finFlagRecv = 0;

  return state;
}

void ctcp_destroy(ctcp_state_t *state) {
  /* Update linked list. */
  if (state->next)
    state->next->prev = state->prev;

  *state->prev = state->next;
  conn_remove(state->conn);

  /* FIXME: Do any other cleanup here. */
  ll_destroy(state->receiver_buf);
  ll_destroy(state->sender_buf);

  free(state);
  end_client();
}

void ctcp_read(ctcp_state_t *state) {
  /* FIXME */
	char *buf;
	int ret = 0;
	ctcp_segment_t *segment;
	sender_buf_t *sender;
	//printf("Calling ctcp_read - %d, %d\n", state->cfg->recv_window, state->windowSize);
	start_read = 1;
	if( (state->cfg->send_window - state->windowSize) >= MAX_SEG_DATA_SIZE) {
		////printf("Calling ctcp_read - %d, %d\n", state->cfg->recv_window, state->windowSize);
		buf = calloc ((sizeof(char) * MAX_SEG_DATA_SIZE), 1);
		ret = conn_input(state->conn, buf, MAX_SEG_DATA_SIZE);
		if(ret == 0) {
			//printf("read is 0\n");
			free(buf);
			return;
		}
		if(ret == -1) {
			//printf("read is -1 \n");
			if(ll_length(state->sender_buf) == 0) {
				sendFin(state);
			}
			state->finFlagSender = 1;
			free(buf);
			return;
		}
		else{

			state->windowSize += ret;
			//printf("read is %d, window-size = %d\n", ret, state->windowSize);
			segment = calloc(ret + sizeof(ctcp_segment_t), 1);
			segment->flags = htonl(ACK);
			memcpy(segment->data, buf, ret);
			////printf("Read : seg-Len is %d, %d, %d, htnos - %d\n", ret, (int)sizeof(ctcp_segment_t), (int)(ret + sizeof(ctcp_segment_t)), htons(ret + sizeof(ctcp_segment_t)));
			segment->len = htons(ret + sizeof(ctcp_segment_t));
		}
		segment->seqno = htonl(state->seqno);
		//printf("read is %d, window-size = %d, seqno = %d\n", ret, state->windowSize, ntohl(segment->seqno));
		state->seqno = ntohl(segment->seqno) + ret;
		segment->ackno = htonl(state->ackno);
		segment->window = htons(state->cfg->recv_window);
		segment->cksum = htons(0);
		segment->cksum = cksum(segment,ntohs(segment->len));
		sender = calloc (sizeof(sender_buf_t), 1);
		sender->sender_seg = segment;
		sender->sent_time = current_time();
		sender->retransmit_count = 0;
		ll_add(state->sender_buf, sender);
		free(buf);
		conn_send(state->conn, segment, ntohs(segment->len));
		//printf("conn_send - len %d, %d, %d\n", (int)(ret + sizeof(ctcp_segment_t)), ntohs(segment->len), (int)strlen(segment->data));
	}

}

void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len) {
  /* FIXME */
	int checksum = 0;
	////printf("\n\n\n data: %s \n\n\n", segment->data);
	//printf("\n\nCalling ctcp_receive - len = %d and seg-Len = %d, %d\n", (int)len, (int)segment->len, (int)ntohs(segment->len));
	//printf("Calling ctcp_receive\n");
	if(len < ntohs(segment->len)) {
		if(segment) {
			free(segment);
		}
		return;  //Error: Hence drop packet
	}

	////printf("FIN is %d, ACK is %d and flags in %d, segno is %d, ackno is %d state ack %d - len is %d\n", htonl(FIN), htonl(ACK), segment->flags, ntohl(segment->seqno), ntohl(segment->ackno), (state->ackno), segment->len);

	////printf(" %s \n", segment->data);
	checksum = segment->cksum;
	segment->cksum = htons(0);

	segment->cksum = cksum(segment, ntohs(segment->len));
	if(checksum != segment->cksum) {
		//printf("Error: Checksum error\n");
		if(segment) {
			free(segment);
		}
		return;
	}

	if(ntohs(segment->len) == sizeof(ctcp_segment_t)) {
		//ACK msgs.
		//printf("Calling ctcp_receive - ACK\n");
		if(segment->flags & htonl(FIN)) {
			////printf("Calling ctcp_receive - ACK - FIN came\n");
			//conn_output(state->conn, NULL, 0);
			if(state->finFlagSender) {
				////printf("Calling ctcp_receive - ACK - Sender FIN came\n");
				sendAck(state, segment);
				if(segment) {
					free(segment);
				}
				//sleep(1);
				ctcp_destroy(state);
				return;
			}
			else if(state->finFlagRecv) {
				if(segment) {
					free(segment);
				}
				ctcp_destroy(state);
				return;
			}
			sendAck(state, segment);
			if(segment) {
				free(segment);
			}
			state->finFlagRecv = 1;
			if(ll_length(state->receiver_buf) == 0) {
				conn_output(state->conn, NULL, 0);
				sendFin(state);
			}
			return;
		}
		if(state->finFlagRecv) {
			if(segment) {
				free(segment);
			}
			ctcp_destroy(state);
			return;
		}

		ll_node_t *curr = state->sender_buf->head;
		ll_node_t *temp;
		int temp_flag = 0;
		while (curr != NULL) {
			sender_buf_t *temp_buf = curr->object;
			////printf("Sequence Numbers -- : %d, %d\n", ntohl(temp_buf->sender_seg->seqno), ntohl(segment->ackno));
			temp = curr->next;
			if (ntohl(temp_buf->sender_seg->seqno) < ntohl(segment->ackno)) {
				state->windowSize -= (ntohs(temp_buf->sender_seg->len) - sizeof(ctcp_segment_t));
				//printf("Calling ctcp_receive - ACK - 3 - 1 Window =  %d\n", state->windowSize);
				free(temp_buf->sender_seg);
				free(curr->object);
				ll_remove(state->sender_buf, curr);
				temp_flag = 1;
			}
			curr = temp;
		}

		if(temp_flag && state->finFlagSender && ll_length(state->sender_buf) == 0) {
			sendFin(state);
		}
		state->cfg->send_window = ntohs(segment->window);
		if(segment) {
			free(segment);
		}
		////printf("Calling ctcp_receive - ACK - EXIT\n");
	}
	else
	{
		//printf("Calling ctcp_receive - data\n");

		if( !checkForDup(state->receiver_buf, segment) ) {
			addSegmentToBuffer(state->receiver_buf, segment);
			//ll_add_front(state->receiver_buf, segment);
		}
		//printf("state->expected_ackno = %d, ntohl(segment->seqno= %d\n", state->expected_ackno, ntohl(segment->seqno));
		if(state->expected_ackno == ntohl(segment->seqno)) {
			ctcp_output(state);
		}
		sendAck(state, segment);
		////printf("Calling ctcp_receive - data - Creating packet\n");

		if(!ll_find(state->receiver_buf, segment)) {
			//printf("Removed element now free\n");
			if(segment) {
				free(segment);
			}
		}
		//printf("Calling ctcp_receive - Exit \n");

	}

}

void ctcp_output(ctcp_state_t *state) {
  /* FIXME */

	size_t freesize = conn_bufspace(state->conn);
	ll_node_t *curr = state->receiver_buf->head;
	//printf("Length of receiver buffer= %d\n", ll_length(state->receiver_buf));
	while (curr != NULL) {
		ll_node_t *temp = curr->next;
		ctcp_segment_t *temp_seg = curr->object;
		if (freesize < (ntohs(temp_seg->len) - sizeof(ctcp_segment_t))) {
			//printf("\n\n---------------- Dropped Packets ----------------------------- \n\n");
			state->dropPacket = 1;
			return;
		}
		//printf("before calling Conn_output called \n");
		if(state->expected_ackno == ntohl(temp_seg->seqno)) {
			//printf("Conn_output called \n");
			int ret = conn_output(state->conn, temp_seg->data, (ntohs(temp_seg->len) - sizeof(ctcp_segment_t)));
			if(ret == -1) {
				//printf("\n\n---------------- Write Error ----------------------------- \n\n");
				ctcp_destroy(state);
				return;
			}
			state->expected_ackno = state->expected_ackno + ntohs(temp_seg->len) - sizeof(ctcp_segment_t);
		}
		//free(curr->object);

		ll_remove(state->receiver_buf, curr);
		curr = temp;
	}
	if(state->finFlagRecv && ll_length(state->receiver_buf) == 0) {
		conn_output(state->conn, NULL, 0);
		sendFin(state);
	}

}

void ctcp_timer() {
  /* FIXME */

	if(!start_read) {
		return;
	}

	ctcp_state_t *state = state_list;
	while (state != NULL) {

		ll_node_t *curr = state->sender_buf->head;
		while (curr != NULL) {
			sender_buf_t *temp_buf = curr->object;
			////printf("Calling ctcp_timer\n");
			if ( (current_time() - temp_buf->sent_time) >= state->cfg->rt_timeout) {
				////printf("Calling ctcp_timer - 1\n");
				if(temp_buf->retransmit_count < 5) {
					////printf("Calling ctcp_timer - 2\n");
					conn_send(state->conn, temp_buf->sender_seg, ntohs(temp_buf->sender_seg->len));
					temp_buf->sent_time = current_time();
					temp_buf->retransmit_count++;
				}
				else{
					/*TODO:breakdown*/
					ctcp_destroy(state);
					break;
				}
			}
			curr = curr->next;
		}
		state = state->next;
	}
}
