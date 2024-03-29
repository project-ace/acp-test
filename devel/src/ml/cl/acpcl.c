/*
 * ACP Middle Layer: Communication Library
 * 
 * Copyright (c) 2014-2014 Kyushu University
 * Copyright (c) 2014      Institute of Systems, Information Technologies
 *                         and Nanotechnologies 2014
 * Copyright (c) 2014      FUJITSU LIMITED
 *
 * This software is released under the BSD License, see LICENSE.
 *
 * Note:
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <errno.h>
#include <time.h>
#include <acp.h>
#include "acpbl.h"
#include "acpbl_sync.h"
#include "acpcl_progress.h"

// #define DEBUG

/* global variables */
int iacp_enable_hybrid = 0; // Flag to permit multiple threads to call the functions in this file

/* static global variables */
static int crbsz; // Size of CRB for Channels
static int crqsz; // Size of CRQ for Segbufs
static acp_ga_t trashboxga; // GA of trashbox used as a dummy target of remote atomic operations
static volatile uint64_t chlk; // Lock
static int iacp_initialized_cl = 0; // Flag to check if initialization has been done once or not

/* external global variables */
extern size_t iacp_starter_memory_size_cl;

/* structs of lists */
typedef struct listitem {
    struct listitem *prev;
    struct listitem *next;
} listitem_t;

typedef struct listobj {
    struct listitem *head;
    struct listitem *tail;
} listobj_t;

/* functions */
/* handling locks */
static inline void ch_lock(void)
{
    if (iacp_enable_hybrid == 1)
        while (sync_val_compare_and_swap_8(&chlk, 0, 1)) ;
}

static inline void ch_unlock(void)
{
    if (iacp_enable_hybrid == 1) {
        sync_synchronize();
        chlk = 0;
    }
}

/* handling lists */
static void list_init(listobj_t *list)
{
    list->head = NULL;
    list->tail = NULL;
}

static void list_add(listobj_t *list, listitem_t *item)
{
    if (list->head == NULL) {
        item->prev = NULL;
        item->next = NULL;
        list->head = item; 
        list->tail = item; 
    } else {
        item->prev = list->tail;
        item->next = NULL;
        list->tail->next = item;
        list->tail = item;
    }
}

static void list_remove(listobj_t *list, listitem_t *item)
{
    if ((item->next == NULL) && (item->prev == NULL)) {
        list->head = NULL;
        list->tail = NULL;
    } else {
        if (item->next == NULL) {
            list->tail = item->prev;
            item->prev->next = item->next;
        } else if (item->prev == NULL) {
            list->head = item->next;
            item->next->prev = item->prev;
        } else {
            item->prev->next = item->next;
            item->next->prev = item->prev;
        }
    }
}

static int list_isempty(listobj_t *list)
{
    return list->head == NULL;
}

static void list_show(listobj_t *list)
{
    listitem_t *item;

    item = list->head;
    while (item != NULL) {
        printf(" %p", item);
        item = item->next;
    }
    printf("\n");
}


/*** 
 ***
 *** Segmented Buffer (Segbuf)
 ***
 *** - Overview
 ***  Interfaces for handling a pair of Segbufs (= Segmented buffers).
 ***  It supports pipelined data transfer between two processes, the source and the destination.
 ***  Each segment of the buffer on the source rank will be mirrored to the buffer on 
 ***  the destination rank, and the destination rank can notify that a segment is ready
 ***  to be overwritten to the source rank. 
 ***
 *** - Creation and connection of Segbuf
 ***  Segbufs are created on the source side and the destination side via acp_create_segbuf_src() 
 ***  function and acp_create_segbuf_dst() function, respectively.
 ***  In addition to the starting address, the segment size and the number of segments are 
 ***  specified at the creation.
 ***  After creation, Segbufs need to be connected to each other before it can perform Ack
 ***  and Ready.
 ***
 *** - Ack and Ready operations on Segbuf
 ***  Each segment of the buffer on the source rank will be mirrored to the buffer on 
 ***  the destination rank, via Ack operation (= acp_ack_segbuf() function).
 ***  After the destination rank consumed the data in a segment of the buffer, it can 
 ***  notify that the segment is ready to be overwritten by the source, via 
 ***  Ready operation (= acp_ready_segbuf() function).
 ***  Ack and Ready operations on segments are performed one by one, in-order through the buffer.
 ***  The buffer is handled in ring style.
 ***  Ack operation fails if the buffer is full and no segment is ready.
 ***  Ready operation fails if the buffer is empty.
 ***
 ***               Source rank                                    Destination rank
 ***              
 ***  +  + Size   +---------------------------+                  +---------------------------+
 ***  |  | of     
 ***  |  + Segment+---------------------------+                  +---------------------------+
 ***  |                DATA0                    <-Head    Head->      DATA0
 ***  |           +---------------------------+                  +---------------------------+
 ***  Number           DATA1                                          DATA1
 ***  of          +---------------------------+                  +---------------------------+
 ***  segments         DATA2                    <-Sent    Tail->      DATA2
 ***  |           +---------------------------+                  +---------------------------+
 ***  |                DATA3                    <-Tail    (Tail will be updated after DATA3 arrives)  
 ***  |           +---------------------------+                  +---------------------------+
 ***  |           
 ***  +           +---------------------------+                  +---------------------------+
 ***
 *** - Head, Tail and Sent indecies of Segbuf
 ***  Current status of a Segbuf is represented by three indecies: Head, Tail and Sent.
 ***  All of the three indeces are initialized to point to the top of the Segbuf on each side of 
 ***  ranks.
 ***  An Ack operation on the source starts to copy a segment at the Tail index of the local Segbuf 
 ***  to the destination, and increments the local Tail index by one.
 ***  After the completion of the copy, the Tail index of the destination is also incremented to 
 ***  notify that the data transfer of one segment has been completed.
 ***  An Ready operation on the destination increments the Head index on both side of ranks by one.
 ***  Therefore, segments in the region between the Head and the Tail may consist data.
 ***  The Sent index always points to the segment in this region and represents the position
 ***  up to where the data has been copied to the destination.
 ***  This index is used to examine if the source rank can overwrite a segment in the region of 
 ***  the local Segbuf without causing inconsistency of Ack operations performed previously.
 ***  The Head and the Tail indecies are retrieved by the ranks on both side via functions, 
 ***  acp_query_segbuf_head() and acp_query_segbuf_tail(), respectively.
 ***  The Sent index can be referred only from the source rank via acp_query_segbuf_sent() function.
 ***
 *** - Disconnetion and freeing of Segbuf
 ***  A Segbuf can be disconnected via acp_disconnect_segbuf() function.
 ***  After disconnection, only acp_free_segbuf() function, which frees the Segbuf, can be applied to 
 ***  the Segbuf.
 ***/

/* structs */
typedef struct con_segbuf_peer {
    struct con_segbuf_peer *prev;
    struct con_segbuf_peer *next;
    int rank;
    char *conninfos;
} con_segbuf_peer_t;

struct segbufitem {
    char *ctlla;
    acp_ga_t ctlga;
    char *bufla;
    acp_atkey_t bufkey;
    acp_ga_t bufga;
    acp_ga_t remotectlga;
    acp_ga_t remotebufga;
    acp_ga_t remoteconsegbufga;
    size_t segsize;
    size_t segnum;
    int state;
};

/* global variables */
static char *crq_top; // Start of CRQ region
static volatile int64_t *crq_head; // Head index of CRQ
static volatile int64_t *crq_tail; // Tail index of CRQ
static acp_ga_t *crq; // Body of CRQ
static int crq_num_slots=10; // Number of slots of CRQ
static int crq_num_free_con_segbuf_peers=10; // Number of free con_segbuf_peers struct
static int crq_num_free_con_segbufs=10; // Number of free con_segbuf_info 
static int num_free_segbuf_ctls=100; // Number of free segbuf_ctl
static con_segbuf_peer_t *con_segbuf_peer_items; // Free con_segbuf_peers 
static char *con_segbuf_items; // Free con_segbufs
static acp_atkey_t con_segbuf_items_key;
static acp_ga_t con_segbuf_items_ga;
static char *segbuf_ctl_table; // Free_segbuf_ctls
static acp_atkey_t segbuf_ctl_table_key;
static acp_ga_t segbuf_ctl_table_ga;

/* lists of structs */
static listobj_t con_src_segbuf_list;
static listobj_t con_dst_segbuf_list;

static con_segbuf_peer_t *free_con_segbuf_peer_list;

/* lists of data structures */
static char *free_con_segbuf_list;
static char *con_dst_waitga_segbuf_list;
static char *con_dst_waitcomp_segbuf_list;

/** macros for data structures 
 *
 * - CRQ (Connection Request Queue) 
 * 
 *      Starter memory
 *      +---------------------------+ +
 *      | Connection Request Buffer | | 
 *      | for Channels              | | crbsz
 *      |                           | | 
 *      +---------------------------+ +
 *      | CRQ Head (8B)             |
 *      +---------------------------+
 *      | CRQ_Tail (8B)             |
 *      +---------------------------+ +
 *      | Body of CRQ               | |
 *      +- (crq_num_slots       ----+ | crq_num_slots
 *      |   * sizeof(acp_ga_t))     | |
 *      +---------------------------+ +
 *      | Trashbox                  |
 *      +---------------------------+
 * 
 * - segbuf_info
 *
 *      +---------------------------+
 *      | crq_head (8B)             |
 *      +---------------------------+ 
 *      | crq_tail (8B)             |
 *      +---------------------------+ 
 *      | ctlga    (8B)             |
 *      +---------------------------+ 
 *      | bufga    (8B)             |
 *      +---------------------------+ 
 *      | segsize  (8B)             |
 *      +---------------------------+ 
 *      | segnum   (8B)             |
 *      +---------------------------+ 
 *      | thisga   (8B)             |
 *      +---------------------------+ 
 *      | state    (4B)             |
 *      +---------------------------+ 
 *      | segbuf   (8B)             |
 *      +---------------------------+ 
 *      | handle   (8B)             |
 *      +---------------------------+ 
 *      | next     (8B)             |
 *      +---------------------------+ 
 *      | segsize  (8B)             |
 *      +---------------------------+ 
 *
 * - segbuf_ctl
 *
 *      +---------------------------+
 *      | head     (8B)             |
 *      +---------------------------+ 
 *      | tail     (8B)             |
 *      +---------------------------+ 
 *      | sent     (8B)             |
 *      +---------------------------+ 
 *      | state    (4B)             |
 *      +---------------------------+ 
 *
 **/
#define CRQ_TOP_OFFSET (crbsz)
#define CRQ_OFFSET_HEAD      0
#define CRQ_OFFSET_TAIL      8
#define CRQ_OFFSET_QBODY    16

#define CON_SEGBUF_SIZE (76+sizeof(acp_handle_t))
#define CON_SEGBUF_OFFSET_HEAD    0
#define CON_SEGBUF_OFFSET_TAIL    8
#define CON_SEGBUF_OFFSET_CTLGA  16
#define CON_SEGBUF_OFFSET_BUFGA  24
#define CON_SEGBUF_OFFSET_SEGSZ  32
#define CON_SEGBUF_OFFSET_SEGNUM 40
#define CON_SEGBUF_OFFSET_THISGA 48
#define CON_SEGBUF_OFFSET_STATE  56
#define CON_SEGBUF_OFFSET_SEGBUF 60
#define CON_SEGBUF_OFFSET_HANDLE 68
#define CON_SEGBUF_OFFSET_NEXT   (68+sizeof(acp_handle_t))

#define CON_SEGBUF_HEAD(addr)   (*((int64_t *)((char *)addr+CON_SEGBUF_OFFSET_HEAD)))
#define CON_SEGBUF_TAIL(addr)   (*((int64_t *)((char *)addr+CON_SEGBUF_OFFSET_TAIL)))
#define CON_SEGBUF_CTLGA(addr)  (*((acp_ga_t *)((char *)addr+CON_SEGBUF_OFFSET_CTLGA)))
#define CON_SEGBUF_BUFGA(addr)  (*((acp_ga_t *)((char *)addr+CON_SEGBUF_OFFSET_BUFGA)))
#define CON_SEGBUF_SEGSZ(addr)  (*((size_t *)((char *)addr+CON_SEGBUF_OFFSET_SEGSZ)))
#define CON_SEGBUF_SEGNUM(addr) (*((size_t *)((char *)addr+CON_SEGBUF_OFFSET_SEGNUM)))
#define CON_SEGBUF_THISGA(addr) (*((acp_ga_t *)((char *)addr+CON_SEGBUF_OFFSET_THISGA)))
#define CON_SEGBUF_STATE(addr)  (*((int *)((char *)addr+CON_SEGBUF_OFFSET_STATE)))
#define CON_SEGBUF_SEGBUF(addr) (*((acp_segbuf_t *)((char *)addr+CON_SEGBUF_OFFSET_SEGBUF)))
#define CON_SEGBUF_HANDLE(addr) (*((acp_handle_t *)((char *)addr+CON_SEGBUF_OFFSET_HANDLE)))
#define CON_SEGBUF_NEXT(addr)   (*((char **)((char *)addr+CON_SEGBUF_OFFSET_NEXT)))

#define SEGBUFCTL_SIZE 28
#define SEGBUFCTL_OFFSET_HEAD    0
#define SEGBUFCTL_OFFSET_SENT    8
#define SEGBUFCTL_OFFSET_TAIL   16
#define SEGBUFCTL_OFFSET_STATE  24

#define SEGBUFCTL_HEAD(addr)   (*((int64_t *)((char *)addr+SEGBUFCTL_OFFSET_HEAD)))
#define SEGBUFCTL_SENT(addr)   (*((int64_t *)((char *)addr+SEGBUFCTL_OFFSET_SENT)))
#define SEGBUFCTL_TAIL(addr)   (*((int64_t *)((char *)addr+SEGBUFCTL_OFFSET_TAIL)))
#define SEGBUFCTL_STATE(addr)  (*((int *)((char *)addr+SEGBUFCTL_OFFSET_STATE)))

#define CON_SEGBUF_STAT_NOUSE -1
#define CON_SEGBUF_STAT_INIT  0
#define CON_SEGBUF_STAT_WTHD  1
#define CON_SEGBUF_STAT_WTAVL 2
#define CON_SEGBUF_STAT_WTCON 3
#define CON_SEGBUF_STAT_INV   4
#define CON_SEGBUF_STAT_CON   5

#define SEGBUF_STAT_NOUSE  -1
#define SEGBUF_STAT_INIT   0
#define SEGBUF_STAT_CON    1
#define SEGBUF_STAT_DISCON 2

static int clear_con_segbuf(char *item)
{
    CON_SEGBUF_HEAD(item) = 0;
    CON_SEGBUF_TAIL(item) = 0;
    CON_SEGBUF_CTLGA(item) = ACP_GA_NULL;
    CON_SEGBUF_BUFGA(item) = ACP_GA_NULL;
    CON_SEGBUF_SEGSZ(item) = -1;
    CON_SEGBUF_SEGNUM(item) = -1;
    CON_SEGBUF_STATE(item) = CON_SEGBUF_STAT_NOUSE;
    CON_SEGBUF_SEGBUF(item) = NULL;
    CON_SEGBUF_HANDLE(item) = ACP_HANDLE_NULL;
}

static int init_segbuf()
{
    int myrank, i;
    con_segbuf_peer_t *peer_item;
    char *item;
    int *tmp;

    myrank = acp_rank();

    /* size of crq region */
    crq_top = (char *)acp_query_address(iacp_query_starter_ga_cl(myrank)) + CRQ_TOP_OFFSET;
    crq_head = (int64_t *)(crq_top + CRQ_OFFSET_HEAD);
    crq_tail = (int64_t *)(crq_top + CRQ_OFFSET_TAIL);
    crq = (acp_ga_t *)(crq_top + CRQ_OFFSET_QBODY);

    *crq_head = 0LL;
    *crq_tail = 0LL;

    /* initialize crq body */
    for (i = 0; i < crq_num_slots; i++)
        crq[i] = ACP_GA_NULL;

    /* initialize empty lists of structures */
    list_init(&con_src_segbuf_list);
    list_init(&con_dst_segbuf_list);

    /* initialize empty lists of data arrays */
    con_dst_waitga_segbuf_list = NULL;
    con_dst_waitcomp_segbuf_list = NULL;
    
    /* initialize free lists of structures */
    con_segbuf_peer_items = malloc(crq_num_free_con_segbuf_peers * sizeof(con_segbuf_peer_t));
    for (i = 0; i < crq_num_free_con_segbuf_peers - 1; i++) {
        peer_item = &(con_segbuf_peer_items[i]);
        peer_item->prev = NULL;
        peer_item->next = &(con_segbuf_peer_items[i+1]);
        peer_item->rank = -1;
        peer_item->conninfos = NULL;
    }
    con_segbuf_peer_items[crq_num_free_con_segbuf_peers - 1].prev = NULL;
    con_segbuf_peer_items[crq_num_free_con_segbuf_peers - 1].next = NULL;
    con_segbuf_peer_items[crq_num_free_con_segbuf_peers - 1].rank = -1;
    con_segbuf_peer_items[crq_num_free_con_segbuf_peers - 1].next = NULL;
    free_con_segbuf_peer_list = &(con_segbuf_peer_items[0]);

    /* initialize free lists of data arrays */
      // con_segbuf: connection information
    con_segbuf_items = malloc(crq_num_free_con_segbufs * CON_SEGBUF_SIZE);
    con_segbuf_items_key = acp_register_memory(con_segbuf_items, crq_num_free_con_segbufs * CON_SEGBUF_SIZE, 0);
    con_segbuf_items_ga = acp_query_ga(con_segbuf_items_key, con_segbuf_items);
    for (i = 0; i < crq_num_free_con_segbufs; i++) {
        item = con_segbuf_items + i * CON_SEGBUF_SIZE;
        clear_con_segbuf(item);
        CON_SEGBUF_THISGA(item) = con_segbuf_items_ga + i * CON_SEGBUF_SIZE;
    }
    for (i = 0; i < crq_num_free_con_segbufs - 1; i++) {
        item = con_segbuf_items + i * CON_SEGBUF_SIZE;
        CON_SEGBUF_NEXT(item) = con_segbuf_items + (i+1) * CON_SEGBUF_SIZE;
    }
    item = con_segbuf_items + (crq_num_free_con_segbufs - 1) * CON_SEGBUF_SIZE;
    CON_SEGBUF_NEXT(item) = NULL;
    free_con_segbuf_list = con_segbuf_items;

      // segbuf_ctl_table: segbuf control
    segbuf_ctl_table = malloc(num_free_segbuf_ctls * SEGBUFCTL_SIZE);
    segbuf_ctl_table_key = acp_register_memory(segbuf_ctl_table, num_free_segbuf_ctls * SEGBUFCTL_SIZE, 0);
    segbuf_ctl_table_ga = acp_query_ga(segbuf_ctl_table_key, segbuf_ctl_table);
    for (i = 0; i < num_free_segbuf_ctls; i++) {
        item = segbuf_ctl_table + i * SEGBUFCTL_SIZE;
        SEGBUFCTL_HEAD(item) = 0;
        SEGBUFCTL_SENT(item) = 0;
        SEGBUFCTL_TAIL(item) = 0;
        SEGBUFCTL_STATE(item) = SEGBUF_STAT_NOUSE;
    }

#ifdef DEBUG
    fprintf(stderr, "%d: init_segbuf starter %lx crq_top %p crq %p con_segbuf_items %p con_segbuf_items_ga %lx segbuf_ctl_table %p segbuf_ctl_table_ga %lx crq_tail %ld crq_head %ld\n", 
            myrank, iacp_query_starter_ga_cl(myrank), crq_top, crq, con_segbuf_items, con_segbuf_items_ga, segbuf_ctl_table, segbuf_ctl_table_ga, *crq_tail, *crq_head);
#endif
}

static int finalize_segbuf()
{
    ch_lock();
    acp_unregister_memory(con_segbuf_items_key);
    acp_unregister_memory(segbuf_ctl_table_key);
    free(con_segbuf_peer_items);
    free(con_segbuf_items);
    free(segbuf_ctl_table);
    ch_unlock();
}

static int add_fetch_crqtail(char *item, int peer)
{
    acp_ga_t dst_crqga, dst_crq_tailga;
    int myrank;

    myrank = acp_rank();

    dst_crqga = iacp_query_starter_ga_cl(peer) + CRQ_TOP_OFFSET;
    dst_crq_tailga = dst_crqga + CRQ_OFFSET_TAIL;
    /* start fetch and add8 crq_tail on peer to crq_tail in con_segbuf_info */
    acp_add8(CON_SEGBUF_THISGA(item) + CON_SEGBUF_OFFSET_TAIL, 
             dst_crq_tailga, 1LL, ACP_HANDLE_NULL);
#ifdef DEBUG
    fprintf(stderr, "%d: add_fetch_crqtail %d %lx %lx dst_crqga %lx CRQ_TOP_OFFSET %d starter_cl %lx starter_dl %lx starter %lx\n", 
            myrank, peer, CON_SEGBUF_THISGA(item) + CON_SEGBUF_OFFSET_TAIL, dst_crq_tailga, dst_crqga, CRQ_TOP_OFFSET, iacp_query_starter_ga_cl(peer), iacp_query_starter_ga_dl(peer), acp_query_starter_ga(peer)); 
#endif
}

static int get_crqhead(char *item, int peer)
{
    acp_ga_t dst_crqga, dst_crq_headga;
    int myrank;

    myrank = acp_rank();


    dst_crqga = iacp_query_starter_ga_cl(peer) + CRQ_TOP_OFFSET;
    dst_crq_headga = dst_crqga + CRQ_OFFSET_HEAD;
    /* handle = start get crq_head on peer to crq_head in con_segbuf_info */
    CON_SEGBUF_HANDLE(item) = acp_copy(CON_SEGBUF_THISGA(item) + CON_SEGBUF_OFFSET_HEAD,
                                       dst_crq_headga, sizeof(int64_t), ACP_HANDLE_NULL);
#ifdef DEBUG
    fprintf(stderr, "%d: get_crqhead : %lx %lx\n", 
            myrank, CON_SEGBUF_THISGA(item) + CON_SEGBUF_OFFSET_HEAD, dst_crq_headga);
#endif
}

static int check_crq_tail(char *item)
{
    int myrank;

    myrank = acp_rank();

    if (acp_inquire(CON_SEGBUF_HANDLE(item)) == 0) {
        /* acp_add8 is "Fetch and add" operation. Its result is the index
         * of the entry where this_info should be put connection request in 
         * the CRQ. So, use the result of acp_add8 as it is.
         */
        CON_SEGBUF_STATE(item) = CON_SEGBUF_STAT_WTAVL;
#ifdef DEBUG
    fprintf(stderr, "%d: check_crq_tail: Tail arrived %ld state %d\n", 
            myrank, CON_SEGBUF_TAIL(item), CON_SEGBUF_STATE(item));
#endif
        return 0;
    } else {
        return 1;
    }
}

static int send_conreq(char *info, int peer)
{
    acp_ga_t dst_crqga, src_thisgaga;
    int idx;
    int myrank;

    myrank = acp_rank();

    dst_crqga = iacp_query_starter_ga_cl(peer) + CRQ_TOP_OFFSET;
    idx = CON_SEGBUF_TAIL(info) % crq_num_slots;
    src_thisgaga = CON_SEGBUF_THISGA(info) + CON_SEGBUF_OFFSET_THISGA;
    acp_copy(dst_crqga + CRQ_OFFSET_QBODY + idx*sizeof(acp_ga_t),
             src_thisgaga, sizeof(acp_ga_t), ACP_HANDLE_NULL);
}

static int check_avail(char *item, int peer)
{
    int myrank;

    myrank = acp_rank();

    if (acp_inquire(CON_SEGBUF_HANDLE(item)) == 0) {
        if ((CON_SEGBUF_TAIL(item) - CON_SEGBUF_HEAD(item) + 1) < crq_num_slots) {
            CON_SEGBUF_STATE(item) = CON_SEGBUF_STAT_WTCON;
            send_conreq(item, peer);
            return 0;
        } else {
            get_crqhead(item, peer);
        }
    }
    return 1;
}

static void progress_src_con_segbuf()
{
    con_segbuf_peer_t *this_peer, *next_peer;
    char *this_item, *next_item, *this_segbuf_ctl;
    int myrank, this_state;
    acp_segbuf_t this_segbuf;

    myrank = acp_rank();
    this_peer = (con_segbuf_peer_t *)(con_src_segbuf_list.head);
    
    while (this_peer != NULL) {
        this_item = this_peer->conninfos;
        this_state = CON_SEGBUF_STATE(this_item);
        next_peer = this_peer->next;
        switch (this_state) {
        case CON_SEGBUF_STAT_WTHD:
            if (check_crq_tail(this_item) == 0) 
                check_avail(this_item, this_peer->rank);
            break;
        case CON_SEGBUF_STAT_WTAVL:
            check_avail(this_item, this_peer->rank);
            break;
        case CON_SEGBUF_STAT_WTCON:
            /* do nothing */
            break;
        case CON_SEGBUF_STAT_INV:
#ifdef DEBUG
    fprintf(stderr, "%d: progress_src_con_segbuf : request invalidated %p\n", 
            myrank, this_item);
#endif
            CON_SEGBUF_STATE(this_item) = CON_SEGBUF_STAT_WTHD;
            add_fetch_crqtail(this_item, this_peer->rank);
            get_crqhead(this_item, this_peer->rank);
            if (check_crq_tail(this_item) == 0) 
                check_avail(this_item, this_peer->rank);
            break;
        case CON_SEGBUF_STAT_CON:
#ifdef DEBUG
    fprintf(stderr, "%d: progress_src_con_segbuf : connected %p\n", 
            myrank, this_item);
#endif
            next_item = CON_SEGBUF_NEXT(this_item);
            if (next_item != NULL) {
                /* start connection for next item, as son as possible */
                CON_SEGBUF_STATE(next_item) = CON_SEGBUF_STAT_WTHD;
                add_fetch_crqtail(next_item, this_peer->rank);
                get_crqhead(next_item, this_peer->rank);
            } 

            /* set parameters of segbuf_t */
            this_segbuf = CON_SEGBUF_SEGBUF(this_item);
            this_segbuf->remotebufga = CON_SEGBUF_BUFGA(this_item);
            this_segbuf->remotectlga = CON_SEGBUF_CTLGA(this_item);
            this_segbuf->state = SEGBUF_STAT_CON;
            /* set state of segbuf_ctl to SEGBUF_STAT_CON */
            this_segbuf_ctl = this_segbuf->ctlla;
            SEGBUFCTL_STATE(this_segbuf_ctl) = SEGBUF_STAT_CON;
            /* free this_item */
            clear_con_segbuf(this_item);
            CON_SEGBUF_NEXT(this_item) = free_con_segbuf_list;
            free_con_segbuf_list = this_item;

            if (next_item != NULL) {
#ifdef DEBUG
    fprintf(stderr, "%d: progress_src_con_segbuf : remove con_segbuf from peer. next = %p\n", 
            myrank, next_item);
#endif
                /* update this_peer */
                this_peer->conninfos = next_item;

                /* progress next_item, if possible */
                if (check_crq_tail(next_item) == 0) 
                    check_avail(next_item, this_peer->rank);
            } else {
                /* no pending connection with this peer. free this_peer */
                list_remove(&con_src_segbuf_list, (listitem_t *)this_peer);
                this_peer->next = free_con_segbuf_peer_list;
                free_con_segbuf_peer_list = this_peer;
#ifdef DEBUG
    fprintf(stderr, "%d: progress_src_con_segbuf : remove peer. con_src_segbuf_list.head = %p\n", 
            myrank, con_src_segbuf_list.head);
#endif
            }

            break;
        default:
            fprintf(stderr, "progress_src_con_segbuf: %d : wrong state %d\n", myrank, this_state);
        }
        this_peer = next_peer;
    }
}

static int progress_dst_con_segbuf()
{
    int64_t this_head, this_tail, head1, head2;
    int headidx, idx1, idx2, myrank;
    acp_ga_t rem_con_segbuf_ga, this_item_ga;
    int peer_rank;
    con_segbuf_peer_t *this_peer;
    char *this_item, *prev_item, *next_item;
    acp_segbuf_t this_segbuf;

    myrank = acp_rank();

    /* traverse requests in CRQ */
    this_tail = *crq_tail;
    this_head = *crq_head;
    while ((this_tail - this_head) > 0) {
        headidx = this_head % crq_num_slots;
#ifdef DEBUG
    fprintf(stderr, "%d: progress_dst_con_segbuf tail %ld head %ld headidx %d\n", 
            myrank, this_tail, this_head, headidx);
#endif
        /* if the request matches with a local request */
//        rem_con_segbuf_ga = *((acp_ga_t *)(crq[headidx]));
        rem_con_segbuf_ga = crq[headidx];
        if (rem_con_segbuf_ga == ACP_GA_NULL) // GA for this CRQ entry has not arrived yet
            break;
        peer_rank = acp_query_rank(rem_con_segbuf_ga);
        this_peer = (con_segbuf_peer_t *)(con_dst_segbuf_list.head);

#ifdef DEBUG
    fprintf(stderr, "%d: progress_dst_con_segbuf : check peer_rank %d with peer in list %d \n", 
            myrank, peer_rank, this_peer->rank);
#endif
        while ((this_peer != NULL) && (this_peer->rank != peer_rank))
            this_peer = this_peer->next;

        if (this_peer != NULL) {
            /* match with a local connection request */
            this_item = this_peer->conninfos;
            this_item_ga = CON_SEGBUF_THISGA(this_item);
            this_segbuf = CON_SEGBUF_SEGBUF(this_item);
            this_segbuf->remoteconsegbufga = rem_con_segbuf_ga;
            /* clear this entry of CRQ */
            crq[headidx] = ACP_GA_NULL;
            /* start get ctlga(8B), bufga(8B), segsize(8B), segnum(8B) from peer */
            CON_SEGBUF_HANDLE(this_item) = acp_copy(this_item_ga + CON_SEGBUF_OFFSET_CTLGA,
                                                    rem_con_segbuf_ga + CON_SEGBUF_OFFSET_CTLGA,
                                                    32, ACP_HANDLE_NULL);
#ifdef DEBUG
    fprintf(stderr, "%d: progress_dst_con_segbuf : get info from src %lx %lx \n", 
            myrank, this_item_ga + CON_SEGBUF_OFFSET_CTLGA, rem_con_segbuf_ga + CON_SEGBUF_OFFSET_CTLGA);
#endif
            /* retrieve this con_segbuf from this_peer */
            this_peer->conninfos = CON_SEGBUF_NEXT(this_item);
            if (this_peer->conninfos == NULL) {
                /* no pending connection with this peer. free this_peer */
                list_remove(&con_dst_segbuf_list, (listitem_t *)this_peer);
                this_peer->next = free_con_segbuf_peer_list;
                free_con_segbuf_peer_list = this_peer;
            }
            /* move the local request to waitGA list */
            CON_SEGBUF_NEXT(this_item) = con_dst_waitga_segbuf_list;
            con_dst_waitga_segbuf_list = this_item;
        }
        this_head++;
    }

    /* pack sparse entries in CRQ, and update crq_head */
    head1 = this_head-1;
    while (head1 >= *crq_head) {
        idx1 = head1 % crq_num_slots;
        if (crq[idx1] == ACP_GA_NULL)
            break;
        head1--;
    }
    head2 = head1 - 1;
    while (head2 >= *crq_head) {
        idx2 = head2 % crq_num_slots;
        if (crq[idx2] != ACP_GA_NULL) {
            crq[idx1] = crq[idx2];
            head1--;
            idx1 = head1 % crq_num_slots;
        }
        head2--;
    }
    *crq_head = head1 + 1;
    
    /* handle requests in waitGA list */
    this_item = con_dst_waitga_segbuf_list;
    prev_item = NULL;
    while (this_item != NULL) {
        if (acp_inquire(CON_SEGBUF_HANDLE(this_item)) == 0) {
            /* remote GAs and sizes have arrived */
            this_segbuf = CON_SEGBUF_SEGBUF(this_item);
            if ((CON_SEGBUF_SEGSZ(this_item) != this_segbuf->segsize) ||
                (CON_SEGBUF_SEGNUM(this_item) != this_segbuf->segnum)) {
                fprintf(stderr, "progress_dst_con_segbuf: %d : CRQ (segsize %d , segnum %d)  local request (segsize %d , segnum %d)\n", 
                        CON_SEGBUF_SEGSZ(this_item), CON_SEGBUF_SEGNUM(this_item), this_segbuf->segsize, this_segbuf->segnum);
            }

            /* copy remote GAs to local segbuf_t */
            this_segbuf->remotectlga = CON_SEGBUF_CTLGA(this_item);
            this_segbuf->remotebufga = CON_SEGBUF_BUFGA(this_item);

            /* copy local GAs to con_segbuf */
            CON_SEGBUF_CTLGA(this_item) = this_segbuf->ctlga;
            CON_SEGBUF_BUFGA(this_item) = this_segbuf->bufga;

#ifdef DEBUG
    fprintf(stderr, "%d:progress_dst_con_segbuf : src info arrived %lx %lx %ld %ld\n", 
            myrank, this_segbuf->remotectlga, this_segbuf->remotebufga, CON_SEGBUF_SEGSZ(this_item), CON_SEGBUF_SEGNUM(this_item));
#endif
            /* sendback ctlga and bufga */
            acp_copy(this_segbuf->remoteconsegbufga + CON_SEGBUF_OFFSET_CTLGA,
                     CON_SEGBUF_THISGA(this_item) + CON_SEGBUF_OFFSET_CTLGA, 2*sizeof(acp_ga_t), 
                     ACP_HANDLE_NULL);

            /* set states to CONNECTED */
            CON_SEGBUF_STATE(this_item) = CON_SEGBUF_STAT_CON;
            this_segbuf->state = SEGBUF_STAT_CON;
            SEGBUFCTL_STATE(this_segbuf->ctlla) = SEGBUF_STAT_CON;
            CON_SEGBUF_HANDLE(this_item) = acp_swap4(trashboxga, this_segbuf->remoteconsegbufga + CON_SEGBUF_OFFSET_STATE,
                                                     CON_SEGBUF_STAT_CON, ACP_HANDLE_NULL);

            /* move this request to waitcomp list */
            next_item = CON_SEGBUF_NEXT(this_item);
            if (prev_item == NULL) {
                con_dst_waitga_segbuf_list = next_item; // remove head item from the waitGA list
            } else {
                CON_SEGBUF_NEXT(prev_item) = next_item;
            }
            CON_SEGBUF_NEXT(this_item) = con_dst_waitcomp_segbuf_list;
            con_dst_waitcomp_segbuf_list = this_item;
            this_item = next_item;
        } else {
            prev_item = this_item;
            this_item = CON_SEGBUF_NEXT(this_item);
        }
    }

    /* handle requests in waitcomp list */
    this_item = con_dst_waitcomp_segbuf_list;
    prev_item = NULL;
    while (this_item != NULL) {
        next_item = CON_SEGBUF_NEXT(this_item);
        if (acp_inquire(CON_SEGBUF_HANDLE(this_item)) == 0) {
#ifdef DEBUG
    fprintf(stderr, "%d: progress_dst_con_segbuf : connected\n", 
            myrank);
#endif
            /* connection complete. free connection request. */
            if (prev_item == NULL) {
                con_dst_waitcomp_segbuf_list = next_item; // remove head item from the waitGA list
            } else {
                CON_SEGBUF_NEXT(prev_item) = next_item;
            }
            CON_SEGBUF_NEXT(this_item) = free_con_segbuf_list;
            free_con_segbuf_list = this_item;
            clear_con_segbuf(this_item);
        } else {
            prev_item = this_item;
        }
        this_item = next_item;
    }

    /* if CRQ is full */
    if ((*crq_tail - *crq_head) >= crq_num_slots) {
        /* invalidate request at the head of CRQ */
        headidx = *crq_head % crq_num_slots;
        rem_con_segbuf_ga = *((acp_ga_t *)(crq[headidx]));
        acp_swap4(trashboxga, rem_con_segbuf_ga + CON_SEGBUF_OFFSET_STATE, 
                  CON_SEGBUF_STAT_INV, ACP_HANDLE_NULL);
        crq[headidx] = ACP_GA_NULL;
        (*crq_head)++;
#ifdef DEBUG
    fprintf(stderr, "%d: progress_dst_con_segbuf : invalidate request %lx\n", 
            myrank, rem_con_segbuf_ga);
#endif

    }
}

/* progress */
void iacpcl_progress_segbuf()
{
    // progress src segbuf connection: con_src_segbuf_list
    progress_src_con_segbuf();

    // progress dst segbuf connection: con_dst_segbuf_list
    progress_dst_con_segbuf();
}

acp_segbuf_t acp_create_src_segbuf(int dst_rank, void *buf, size_t segsize, size_t segnum)
{
    int i, myrank;
    char *this_segbuf_ctl, *this_con_segbuf, *tmp_item;
    acp_segbuf_t this_segbuf;
    con_segbuf_peer_t *this_peer;

    myrank = acp_rank();

    /* prepare segbuf_t */
    this_segbuf = (acp_segbuf_t)malloc(sizeof(struct segbufitem));
    this_segbuf->bufla = buf;
    this_segbuf->bufkey = acp_register_memory(buf, segsize*segnum, 0);     // register buf 
    this_segbuf->bufga = acp_query_ga(this_segbuf->bufkey, buf);
    this_segbuf->segsize = segsize;
    this_segbuf->segnum = segnum;
    this_segbuf->state = SEGBUF_STAT_INIT;

    /* retrieve segbuf_ctl from segbuf_ctl_table */
    for (i = 0; i < num_free_segbuf_ctls; i++) {
        this_segbuf_ctl = segbuf_ctl_table + i * SEGBUFCTL_SIZE;
        if (SEGBUFCTL_STATE(this_segbuf_ctl) == SEGBUF_STAT_NOUSE)
            break;
    }
    if (SEGBUFCTL_STATE(this_segbuf_ctl) != SEGBUF_STAT_NOUSE) {
        fprintf(stderr, "acp_create_src_segbuf : %d : Number of segbuf_ctls has been exceeded the limit\n", myrank);
        free(this_segbuf);
        return NULL;
    }
    SEGBUFCTL_STATE(this_segbuf_ctl) = SEGBUF_STAT_INIT;
    this_segbuf->ctlla = this_segbuf_ctl;
    this_segbuf->ctlga = segbuf_ctl_table_ga + (this_segbuf_ctl - segbuf_ctl_table);

    /* retrieve con_segbuf from free_con_segbuf_list */
    this_con_segbuf = free_con_segbuf_list;
    if (this_con_segbuf == NULL) {
        fprintf(stderr, "acp_create_src_segbuf : %d : Number of con_segbuf has been exceeded the limit\n", myrank);
        free(this_segbuf);
        return NULL;
    }

    /* set parameters for con_segbuf */
    CON_SEGBUF_HEAD(this_con_segbuf) = -1LL;
    CON_SEGBUF_TAIL(this_con_segbuf) = -1LL;
    CON_SEGBUF_CTLGA(this_con_segbuf) = this_segbuf->ctlga;
    CON_SEGBUF_BUFGA(this_con_segbuf) = this_segbuf->bufga;
    CON_SEGBUF_SEGSZ(this_con_segbuf) = segsize;
    CON_SEGBUF_SEGNUM(this_con_segbuf) = segnum;
    CON_SEGBUF_SEGBUF(this_con_segbuf) = this_segbuf;
    CON_SEGBUF_HANDLE(this_con_segbuf) = ACP_HANDLE_NULL;

    this_peer = (con_segbuf_peer_t *)(con_src_segbuf_list.head);
    while (this_peer != NULL) {
        if (this_peer->rank == dst_rank) 
            break;
        this_peer = this_peer->next;
    }

    if (this_peer == NULL) {
        /* no other pending connection request to this dst_rank */
        this_peer = free_con_segbuf_peer_list;
        if (this_peer == NULL) {
            fprintf(stderr, "acp_create_src_segbuf : %d : Number of con_segbuf_peer has been exceeded the limit\n", myrank);
            free(this_segbuf);
            return NULL;
        }

        this_peer->rank = dst_rank;
        this_peer->conninfos = this_con_segbuf;
        CON_SEGBUF_NEXT(this_con_segbuf) = NULL;
        list_add(&con_src_segbuf_list, (listitem_t *)this_peer);

        /* start connection for this segbuf */
        CON_SEGBUF_STATE(this_con_segbuf) = CON_SEGBUF_STAT_WTHD;
        add_fetch_crqtail(this_con_segbuf, dst_rank);
        get_crqhead(this_con_segbuf, dst_rank);
    } else {
        /* there are other pending connection requests to this dst_rank */
        CON_SEGBUF_STATE(this_con_segbuf) = CON_SEGBUF_STAT_INIT;
        tmp_item = this_peer->conninfos;
        if (tmp_item == NULL) {
            fprintf(stderr, "acp_create_src_segbuf : %d : empty con_segbuf_peer\n", myrank);
            free(this_segbuf);
            return NULL;
        }
        while (CON_SEGBUF_NEXT(tmp_item) != NULL) {
            tmp_item = CON_SEGBUF_NEXT(tmp_item);
        }
        CON_SEGBUF_NEXT(tmp_item) = this_con_segbuf;
        CON_SEGBUF_NEXT(this_con_segbuf) = NULL;
    }

#ifdef DEBUG
    fprintf(stderr, "%d: acp_create_src_segbuf ctl %p %lx con %p %lx peer %p stat %d\n", 
            myrank, this_segbuf->ctlla, this_segbuf->ctlga, this_con_segbuf, CON_SEGBUF_THISGA(this_con_segbuf), this_peer, CON_SEGBUF_STATE(this_con_segbuf));
#endif
    iacpcl_progress();

#ifdef DEBUG
    fprintf(stderr, "%d: acp_create_src_segbuf done\n", 
            myrank);
#endif

    return this_segbuf;
}

acp_segbuf_t acp_create_dst_segbuf(int src_rank, void *buf, size_t segsize, size_t segnum)
{
    int i, myrank;
    char *this_segbuf_ctl, *this_con_segbuf, *tmp_item;
    acp_segbuf_t this_segbuf;
    con_segbuf_peer_t *this_peer;

    myrank = acp_rank();

    /* prepare segbuf_t */
    this_segbuf = (acp_segbuf_t)malloc(sizeof(struct segbufitem));
    this_segbuf->bufla = buf;
    this_segbuf->bufkey = acp_register_memory(buf, segsize*segnum, 0);     // register buf 
    this_segbuf->bufga = acp_query_ga(this_segbuf->bufkey, buf);
    this_segbuf->segsize = segsize;
    this_segbuf->segnum = segnum;
    this_segbuf->state = SEGBUF_STAT_INIT;

    /* retrieve segbuf_ctl from segbuf_ctl_table */
    for (i = 0; i < num_free_segbuf_ctls; i++) {
        this_segbuf_ctl = segbuf_ctl_table + i * SEGBUFCTL_SIZE;
        if (SEGBUFCTL_STATE(this_segbuf_ctl) == SEGBUF_STAT_NOUSE)
            break;
    }
    if (SEGBUFCTL_STATE(this_segbuf_ctl) != SEGBUF_STAT_NOUSE) {
        fprintf(stderr, "acp_create_dst_segbuf : %d : Number of segbuf_ctls has been exceeded the limit\n", myrank);
        free(this_segbuf);
        return NULL;
    }
    SEGBUFCTL_STATE(this_segbuf_ctl) = SEGBUF_STAT_INIT;
    this_segbuf->ctlla = this_segbuf_ctl;
    this_segbuf->ctlga = segbuf_ctl_table_ga + (this_segbuf_ctl - segbuf_ctl_table);

    /* retrieve con_segbuf from free_con_segbuf_list */
    this_con_segbuf = free_con_segbuf_list;
    if (this_con_segbuf == NULL) {
        fprintf(stderr, "acp_create_dst_segbuf : %d : Number of con_segbuf has been exceeded the limit\n", myrank);
        free(this_segbuf);
        return NULL;
    }

    /* set parameters for con_segbuf */
    CON_SEGBUF_HEAD(this_con_segbuf) = -1LL;
    CON_SEGBUF_TAIL(this_con_segbuf) = -1LL;
    CON_SEGBUF_CTLGA(this_con_segbuf) = this_segbuf->ctlga;
    CON_SEGBUF_BUFGA(this_con_segbuf) = this_segbuf->bufga;
    CON_SEGBUF_SEGSZ(this_con_segbuf) = segsize;
    CON_SEGBUF_SEGNUM(this_con_segbuf) = segnum;
    CON_SEGBUF_SEGBUF(this_con_segbuf) = this_segbuf;
    CON_SEGBUF_HANDLE(this_con_segbuf) = ACP_HANDLE_NULL;

    this_peer = (con_segbuf_peer_t *)(con_dst_segbuf_list.head);
    while (this_peer != NULL) {
        if (this_peer->rank == src_rank) 
            break;
        this_peer = this_peer->next;
    }

    if (this_peer == NULL) {
        /* no other pending connection request to this dst_rank */
        this_peer = free_con_segbuf_peer_list;
        if (this_peer == NULL) {
            fprintf(stderr, "acp_create_dst_segbuf : %d : Number of con_segbuf_peer has been exceeded the limit\n", myrank);
            free(this_segbuf);
            return NULL;
        }

        this_peer->rank = src_rank;
        this_peer->conninfos = this_con_segbuf;
        CON_SEGBUF_NEXT(this_con_segbuf) = NULL;
        list_add(&con_dst_segbuf_list, (listitem_t *)this_peer);
#ifdef DEBUG
    fprintf(stderr, "%d: acp_create_dst_segbuf add a new peer %d to con_dst_segbuf_list \n", 
            myrank, ((con_segbuf_peer_t *)(con_dst_segbuf_list.head))->rank);
#endif

    } else {
        /* there are other pending connection requests to this dst_rank */
        tmp_item = this_peer->conninfos;
        if (tmp_item == NULL) {
            fprintf(stderr, "acp_create_dst_segbuf : %d : empty con_segbuf_peer\n", myrank);
            free(this_segbuf);
            return NULL;
        }
        while (CON_SEGBUF_NEXT(tmp_item) != NULL) {
            tmp_item = CON_SEGBUF_NEXT(tmp_item);
        }
        CON_SEGBUF_NEXT(tmp_item) = this_con_segbuf;
        CON_SEGBUF_NEXT(this_con_segbuf) = NULL;
#ifdef DEBUG
    fprintf(stderr, "%d: acp_create_dst_segbuf add a new entry to the tail of peer %d to con_dst_segbuf_list \n", 
            myrank, this_peer->rank);
#endif
    }
    CON_SEGBUF_STATE(this_con_segbuf) = CON_SEGBUF_STAT_WTCON;

#ifdef DEBUG
    fprintf(stderr, "%d: acp_create_dst_segbuf ctl %p %lx con %p %lx peer %p stat %d\n", 
            myrank, this_segbuf->ctlla, this_segbuf->ctlga, this_con_segbuf, CON_SEGBUF_THISGA(this_con_segbuf), this_peer, CON_SEGBUF_STATE(this_con_segbuf));
#endif

    iacpcl_progress();

    return this_segbuf;
}

int acp_connect_segbuf(acp_segbuf_t segbuf)
{
    volatile char *ctl;
    int myrank;

    myrank = acp_rank();

    ctl = segbuf->ctlla;

    if ((segbuf->state == SEGBUF_STAT_NOUSE) ||
        (segbuf->state == SEGBUF_STAT_DISCON) ||
        (SEGBUFCTL_STATE(ctl) == SEGBUF_STAT_NOUSE) || 
        (SEGBUFCTL_STATE(ctl) == SEGBUF_STAT_DISCON)) {
        fprintf(stderr, "acp_connect_segbuf : %d : segbuf is in a wrong state %d %d\n", myrank, segbuf->state, SEGBUFCTL_STATE(ctl));
        return -1;
    }

#ifdef DEBUG
    fprintf(stderr, "%d: acp_connect_segbuf ctl %p stat %d\n", 
            myrank, ctl, SEGBUFCTL_STATE(ctl));
#endif

    while (SEGBUFCTL_STATE(ctl) != SEGBUF_STAT_CON) 
        iacpcl_progress();

#ifdef DEBUG
    fprintf(stderr, "%d: acp_connect_segbuf done\n", 
            myrank, ctl, SEGBUFCTL_STATE(ctl));
#endif

    return 0;
}

int acp_isconnected_segbuf(acp_segbuf_t segbuf)
{
    volatile char *ctl;
    int myrank;

    myrank = acp_rank();
    
    ctl = segbuf->ctlla;

    if ((segbuf->state == SEGBUF_STAT_NOUSE) ||
        (segbuf->state == SEGBUF_STAT_DISCON) ||
        (SEGBUFCTL_STATE(ctl) == SEGBUF_STAT_NOUSE) || 
        (SEGBUFCTL_STATE(ctl) == SEGBUF_STAT_DISCON)) {
        fprintf(stderr, "acp_isconnected_segbuf : %d : segbuf is in a wrong state %d\n", myrank, SEGBUFCTL_STATE(ctl));
        return -1;
    }

    iacpcl_progress();

    if (SEGBUFCTL_STATE(ctl) == SEGBUF_STAT_CON) 
        return 0;
    else 
        return 1;
}

int acp_ready_segbuf(acp_segbuf_t segbuf)
{
    char *ctl;
    int myrank;
    int64_t head, tail;
    
    myrank = acp_rank();

    if (segbuf->state != SEGBUF_STAT_CON) {
        fprintf(stderr, "acp_ready_segbuf : %d : segbuf is not connected %d\n", myrank, segbuf->state);
        return -1;
    }

    ctl = segbuf->ctlla;
    head = SEGBUFCTL_HEAD(ctl);
    tail = SEGBUFCTL_TAIL(ctl);

    if (tail <= head) {
        fprintf(stderr, "acp_ready_segbuf : %d : segbuf is empty %lx %lx\n", myrank, head, tail);
        return -1;
    }

    /* increment head */
    head++;
    SEGBUFCTL_HEAD(ctl) = head;
    acp_copy(segbuf->remotectlga + SEGBUFCTL_OFFSET_HEAD,
             segbuf->ctlga + SEGBUFCTL_OFFSET_HEAD,
             sizeof(int64_t), ACP_HANDLE_NULL);
    iacpcl_progress();
}

int acp_ack_segbuf(acp_segbuf_t segbuf)
{
    char *ctl;
    int myrank, tailidx;
    int64_t head, tail;
    acp_handle_t handle0, handle1;
    
    myrank = acp_rank();

    if (segbuf->state != SEGBUF_STAT_CON) {
        fprintf(stderr, "acp_ack_segbuf : %d : segbuf is not connected %d\n", myrank, segbuf->state);
        return -1;
    }

    ctl = segbuf->ctlla;
    head = SEGBUFCTL_HEAD(ctl);
    tail = SEGBUFCTL_TAIL(ctl);

    if ((tail - head) >= segbuf->segnum) {
        fprintf(stderr, "acp_ack_segbuf : %d : segbuf is full %lx %lx\n", myrank, head, tail);
        return -1;
    }

    /* put a segment at tail to dst */
    tailidx = tail % segbuf->segnum;
    handle0 = acp_copy(segbuf->remotebufga + segbuf->segsize * tailidx, 
                      segbuf->bufga + segbuf->segsize * tailidx, segbuf->segsize, ACP_HANDLE_NULL);
    /* increment tail */
    tail++;
    SEGBUFCTL_TAIL(ctl) = tail;
    handle1 = acp_copy(segbuf->remotectlga + SEGBUFCTL_OFFSET_TAIL,
             segbuf->ctlga + SEGBUFCTL_OFFSET_TAIL,
             sizeof(int64_t), handle0); 
//             sizeof(int64_t), ACP_HANDLE_NULL); // shall we wait for "handle0" of the previous acp_copy???

    /* add8 to local SENT index by 1 after handle to update sentidx */
    acp_add8(trashboxga, segbuf->ctlga + SEGBUFCTL_OFFSET_SENT, 1LL, handle0);

    iacpcl_progress();
}

int acp_isready_segbuf(acp_segbuf_t segbuf)
{
    int myrank;
    char *ctl;

    myrank = acp_rank();

    if (segbuf->state != SEGBUF_STAT_CON) {
        fprintf(stderr, "acp_query_segbuf_head : %d : segbuf is not connected %d\n", myrank, segbuf->state);
        return -1;
    }

    iacpcl_progress();

    if ((SEGBUFCTL_TAIL(ctl) - SEGBUFCTL_HEAD(ctl)) < segbuf->segnum)
        return 0; // one or more segments are available in the destination buffer
    else
        return 1; // destination buffer is full
}

int acp_isacked_segbuf(acp_segbuf_t segbuf)
{
    int myrank;
    char *ctl;

    myrank = acp_rank();

    if (segbuf->state != SEGBUF_STAT_CON) {
        fprintf(stderr, "acp_query_segbuf_head : %d : segbuf is not connected %d\n", myrank, segbuf->state);
        return -1;
    }

    iacpcl_progress();

    if (SEGBUFCTL_TAIL(ctl) > SEGBUFCTL_HEAD(ctl))
        return 0;
    else
        return 1;
}

int64_t acp_query_segbuf_head(acp_segbuf_t segbuf)
{
    int myrank;

    myrank = acp_rank();

    if (segbuf->state != SEGBUF_STAT_CON) {
        fprintf(stderr, "acp_query_segbuf_head : %d : segbuf is not connected %d\n", myrank, segbuf->state);
        return -1;
    }

    iacpcl_progress();

    return (SEGBUFCTL_HEAD(segbuf->ctlla) % segbuf->segnum);
}

int64_t acp_query_segbuf_tail(acp_segbuf_t segbuf)
{
    int myrank;

    myrank = acp_rank();

    if (segbuf->state != SEGBUF_STAT_CON) {
        fprintf(stderr, "acp_query_segbuf_tail : %d : segbuf is not connected %d\n", myrank, segbuf->state);
        return -1;
    }

    iacpcl_progress();

    return (SEGBUFCTL_TAIL(segbuf->ctlla) % segbuf->segnum);
}
int64_t acp_query_segbuf_sent(acp_segbuf_t segbuf)
{
    int myrank;

    myrank = acp_rank();

    if (segbuf->state != SEGBUF_STAT_CON) {
        fprintf(stderr, "acp_query_segbuf_sent : %d : segbuf is not connected %d\n", myrank, segbuf->state);
        return -1;
    }

    iacpcl_progress();

    return (SEGBUFCTL_SENT(segbuf->ctlla) % segbuf->segnum);
}

int acp_disconnect_segbuf(acp_segbuf_t segbuf)
{
    int myrank;

    myrank = acp_rank();

    if (segbuf->state != SEGBUF_STAT_CON) {
        fprintf(stderr, "acp_disconnect_segbuf : %d : segbuf is not connected %d\n", myrank, segbuf->state);
        return -1;
    }

    iacpcl_progress();

    segbuf->state = SEGBUF_STAT_DISCON;
    acp_swap4(trashboxga, segbuf->remotectlga + SEGBUFCTL_OFFSET_STATE, SEGBUF_STAT_DISCON, ACP_HANDLE_NULL);
}

int acp_free_segbuf(volatile acp_segbuf_t segbuf)
{
    int myrank;

    myrank = acp_rank();

    if (segbuf->state != SEGBUF_STAT_DISCON) {
        fprintf(stderr, "acp_free_segbuf : %d : segbuf is not disconnected %d\n", myrank, segbuf->state);
        return -1;
    }

    while (SEGBUFCTL_STATE(segbuf->ctlla) != SEGBUF_STAT_DISCON)
        iacpcl_progress();

    acp_unregister_memory(segbuf->bufkey);
    free(segbuf);
}


/*** 
 ***
 *** Channels 
 ***
 ***/

/** macros **/

/* default parameters */
#define ACPCI_DEFAULT_EAGER_LIMIT 2048
#define ACPCI_DEFAULT_CRB_ENTRYNUM 8
#define ACPCI_DEFAULT_REQNUM 8
#define ACPCI_DEFAULT_RBNUMSLOTS 8
#define ACPCI_DEFAULT_SBNUMSLOTS 2

/* types and statuses of channels */
#define CHTYSEND 0
#define CHTYRECV 1
#define CHSTINIT    0 /* initial */
#define CHSTWTHD    1 /* sender channel: waiting head of receiver  */
#define CHSTWTAK    2 /* sender channel: waiting ack from receiver  */
#define CHSTCONN    3 /* connected  */
#define CHSTDISCONN 4 /* connected  */
#define CHSTATMASK 7 /* mask to check the status  */
#define CHSTDISCONFLAG 8 /* flag for disconnecting channel  */
#define CHCHECKSTAT(ch) (ch->state & CHSTATMASK)       /* check the status of the channel  */
#define CHCHECKDISCON(ch) (ch->state & CHSTDISCONFLAG) /* check the disconnection flag  */
#define CHSETSTAT(ch, st) (CHCHECKDISCON(ch) | st)      /* set the status of the channel  */
#define CHSETDISCON(ch) (ch->state | CHSTDISCONFLAG)   /* set the status of the channel  */

/* channel body: 
 *   - sender
 *      | Tail of RecvBuf | slot0 | slot1 | ... 
 *   - receiver
 *      | Head of RecvBuf | slot0 | slot1 | ... 
 * 
 * Tail / Head of RecvBuf: uint64_t
 * 
 */
// NO ATOMIC  #define CHSLOTOFFSET 8 /* offset of the slot0 in the channel body */
#define CHSLOTOFFSET 16 /* offset of the slot0 in the channel body */

/* signal on the connection information */
#define CONNSTVALID 0
#define CONNSTINV   1

/* statuses of requests */
#define REQSTINIT   0 /* Initial  */
#define REQSTEGR    1 /* Eager protocol */
#define REQSTDISCONN 2 /* Disconnection */
#define REQSTFIN    3 /* Finished */

/* macros for messages 
 *   Message:
 *     | Header | Payload |
 *   Header:
 *     | Type(3bits) | Size(61bits) |
 */ 
#define MSGTYBITS 3
#define MSGTYEGR     0x2000000000000000LL /* Eager */
#define MSGTYDISCONN 0xe000000000000000LL /* Disconnection */
#define MSGMAXSZ    ((1LL << (64 - MSGTYBITS)) - 1LL)
#define MSGSIZEMASK ((1LL << (64 - MSGTYBITS)) - 1LL)
#define MSGTYPEMASK 0xe000000000000000LL
#define MSGHDRSZ 8

/* statuses of CRB messages */
#define CRBSTINUSE 0  
#define CRBSTFREE  1

/*   - structure of crb:
 *        head: head of the connection request queue, uint64_t
 *        tail: tail of the connection request queue, uint64_t
 *        iacpci_crb_entrynum*crbmsg: body of the queue
 *        iacpci_crb_entrynum: table of flags to check the arrivals of connection requests
 */
/* macros for CRB */
#define CRB_TAIL_OFFSET  (sizeof(int64_t))  
#define CRB_TABLE_OFFSET (2 * sizeof(int64_t))
#define CRB_FLGTABLE_OFFSET (2 * sizeof(int64_t) + iacpci_crb_entrynum * sizeof(crbmsg_t))
#define CRB_TRASHBOX_OFFSET (2 * (sizeof(int64_t)) + iacpci_crb_entrynum * sizeof(crbmsg_t) + iacpci_crb_entrynum)

/* macros for connection info */
#define CONNINFO_CRBMSG_OFFSET (sizeof(int64_t) * 2)
#define CONNINFO_REMGA_OFFSET (sizeof(int64_t) * 2 + sizeof(crbmsg_t))
#define CONNINFO_STATE_OFFSET (sizeof(int64_t) * 2 + sizeof(crbmsg_t) + sizeof(acp_ga_t))
#define CRBFLG_OFFSET (sizeof(conninfo_t))

/* struct of channel endpoint */
typedef struct chitem {
    struct chitem *prev;
    struct chitem *next;
    uint16_t type;
    uint16_t state;
    int peer;
    char *chbody;
    acp_ga_t localga;
    acp_atkey_t localatkey;
    acp_ga_t remotega;
    int buf_entrysz;
    int rbuf_entrynum;
    int sbuf_entrynum;
    acp_handle_t *shdl;
    struct chreqitem *reqtable;
    struct chreqitem *freereqs;    
    struct listobj reqs;
    int64_t sbhead;
    int64_t sbtail;
// NO ATOMIC   uint64_t rbhead;
// NO ATOMIC   uint64_t rbtail;
    int lock;
} chitem_t;

/* struct of request item in channel  */
typedef struct chreqitem {
    struct chreqitem *prev;
    struct chreqitem *next;
    acp_ch_t ch;
    int status;
    void *addr;
    size_t size;
    size_t receivedsize;
    acp_handle_t hdl;
} chreqitem_t;

/* struct of connection request messages */
typedef struct crbmsg {
    acp_ga_t ga;
    int rank;
    int state;
    int buf_entrysz;
    int rbuf_entrynum;
    int sbuf_entrynum;
} crbmsg_t;

/* struct of connection information.
 * this will be located in the top of the chbody of the channel
 * until the completion of the connection. 
 */
typedef struct conninfo {
    int64_t head;
    int64_t tail;
    crbmsg_t crbmsg;
    acp_ga_t remotega;
    int state;
    acp_ga_t starterga;
    acp_handle_t hdl;
    chitem_t *nextch;
} conninfo_t;

/** local variables **/
static char *crbreqmsg;      /* buffer used for sending connection request  */
static acp_ga_t crbreqmsgga; /* global address of crbreqmsg  */

static listobj_t conch_list;  /* connecting channel list */
static listobj_t idlech_list; /* idle channel list */
static listobj_t reqch_list;  /* requesting channel list */

static int64_t *crbhead;   /* head of the receive queue of CRB  */
static int64_t *crbtail;   /* tail of the receive queue of CRB  */
static crbmsg_t *crbtbl;    /* table used as the receive queue of CRB */
static char *crbflgtbl;     /* table for flags of the entries of CRB */
static acp_ga_t trashboxga; /* dummy GA used as a target of remote atomics  */
static char *crbreqmsg;      /* buffer used for sending connection request  */
static acp_ga_t crbreqmsgga; /* global address of crbreqmsg  */

/** global variables **/
int iacpci_eager_limit = ACPCI_DEFAULT_EAGER_LIMIT;
int iacpci_crb_entrynum = ACPCI_DEFAULT_CRB_ENTRYNUM;
int iacpci_reqnum = ACPCI_DEFAULT_REQNUM;
int iacpci_sbnumslots = ACPCI_DEFAULT_SBNUMSLOTS;
int iacpci_rbnumslots = ACPCI_DEFAULT_RBNUMSLOTS;

/* query for the local address of the local slot specified by idx */
static inline void *localslotaddr(acp_ch_t ch, int idx)
{
    return ch->chbody + CHSLOTOFFSET + idx * (ch->buf_entrysz + MSGHDRSZ);
}

/* query for the global address of the local slot specified by idx */
static inline acp_ga_t localslotga(acp_ch_t ch, int idx)
{
    return ch->localga + CHSLOTOFFSET + idx * (ch->buf_entrysz + MSGHDRSZ);
}

/* query for the global address of the remote slot specified by idx */
static inline acp_ga_t remoteslotga(acp_ch_t ch, int idx)
{
    return ch->remotega + CHSLOTOFFSET + idx * (ch->buf_entrysz + MSGHDRSZ);
}

/* check arrival of messages */
static inline int msgarrive(acp_ch_t ch)
{
// NO ATOMIC    return ((*((uint64_t *)ch->chbody) - ch->rbhead) > 0);
    return (*((int64_t *)ch->chbody+1) > (*((int64_t *)ch->chbody)));
}

/* check availability of Recv Buffer */
static inline int rbavail(acp_ch_t ch)
{
// NO ATOMIC    return ((ch->rbtail - *((uint64_t *)ch->chbody)) < ch->rbuf_entrynum);
    return ((*((int64_t *)ch->chbody+1) - (*((int64_t *)ch->chbody))) < ch->rbuf_entrynum);
}

/* check availability of Send Buffer */
static inline int sbavail(acp_ch_t ch)
{
    return ((ch->sbtail - ch->sbhead) < ch->sbuf_entrynum);
}

/* check non-emptyness of Send Buffer */
static inline int sbnotempty(acp_ch_t ch)
{
    return (ch->sbtail > ch->sbhead);
}

/* make message header */
static inline int64_t mkmsghdr(int64_t type, size_t size)
{
    return (type | size);
}

/* local address of temporal slot for sending tail */
static inline int64_t *localtailla(acp_ch_t ch, int sbidx)
{
    return ((int64_t *)(ch->chbody + CHSLOTOFFSET + ch->sbuf_entrynum * (ch->buf_entrysz + MSGHDRSZ) + sbidx * sizeof(int64_t)));
}

/* global address of temporal slot for sending tail */
static inline acp_ga_t localtailga(acp_ch_t ch, int sbidx)
{
    return (ch->localga + CHSLOTOFFSET + ch->sbuf_entrynum * (ch->buf_entrysz + MSGHDRSZ) + sbidx * sizeof(int64_t));
}

/* initialize connection info
 *   - start getting head and atomic_fetching_and_adding tail of remote crb
 *   - change state of the channel to CHSTWTHD
 */
static void init_conninfo(acp_ch_t ch)
{
    conninfo_t *conninfo;
    int myrank;

    myrank = acp_rank();

    conninfo = (conninfo_t *)(ch->chbody);
    conninfo->state = CONNSTVALID;

    /* start getting head */
    acp_copy(ch->localga, conninfo->starterga, sizeof(int64_t), ACP_HANDLE_NULL);
    /* start atomic_fetch_and_add tail. 
     * this handle waits for both head and tail. 
     * this tail will be used as an index of this crb message. */
    conninfo->hdl = acp_add8(ch->localga + sizeof(int64_t), conninfo->starterga + sizeof(int64_t), 
                             1LL, ACP_HANDLE_NULL);
    ch->state = CHCHECKDISCON(ch) | CHSTWTHD;

#ifdef DEBUG
    fprintf(stderr, "%d: init_conninfo: ch %p get head dst %p src %p \n", 
           myrank, ch, ch->localga, conninfo->starterga);
#endif

}

/* check connecting channels list
 * 
 *  connecting channels list:
 *   list of connecting items
 *    - from conch_list.head to conch_list.tail
 *  connecting items:
 *   keeps list of connecting channels with same type and peer
 *    - from con_item->chs
 *
 */
static int handl_conchlist(void)
{
    acp_ch_t ch, nextch, tmpch;
    conninfo_t *conninfo;
    int myrank, idx, offset;
    int64_t head, tail, crbidx;
    crbmsg_t *msg;
    acp_handle_t hdl;
    int numpendingrch = 0;

    ch_lock();
    myrank = acp_rank();

    /* check items in the connecting channels list */
    ch = (acp_ch_t)(conch_list.head);

    while (ch != NULL) {
        nextch = ch->next;
        switch (ch->type) {
        case CHTYSEND:
            conninfo = (conninfo_t *)(ch->chbody);
            switch (CHCHECKSTAT(ch)) {
            case CHSTINIT:
                fprintf(stderr, "handl_conchlist: rank %d : Error: head of the connecting channel list cannot be CHSTINIT. \n", 
                       myrank);
                ch_unlock();
                iacp_abort_cl();
                break;
            case CHSTWTHD:
                /* check if head (and tail) has arrived */
                if (acp_inquire(conninfo->hdl) == 0) {
                    head = conninfo->head;
                    tail = conninfo->tail; 
                    /* check if there is a room in crb */
                    if ((tail - head) < iacpci_crb_entrynum) {
                        /* send the connection request message to the tail of the crb */
                        hdl = acp_copy(conninfo->starterga + CRB_TABLE_OFFSET
                                       + sizeof(crbmsg_t) * (tail % iacpci_crb_entrynum), 
                                       ch->localga + CONNINFO_CRBMSG_OFFSET,
                                       sizeof(crbmsg_t),
                                       ACP_HANDLE_NULL);
                        acp_copy(conninfo->starterga + CRB_FLGTABLE_OFFSET + (tail % iacpci_crb_entrynum),
                                 ch->localga + CRBFLG_OFFSET, 1, hdl);
#ifdef DEBUG
                        fprintf(stderr, "%d: handl_conchlist: remote start %016llx put crb msg to %016llx (rank %d) head %lld tail %lld from %p crbmsg %d %016llx %d %d %d %d \n", 
                               myrank, iacp_query_starter_ga_cl(ch->peer), 
                               conninfo->starterga + CRB_TABLE_OFFSET + sizeof(crbmsg_t) * (tail % iacpci_crb_entrynum),
                               acp_query_rank(conninfo->starterga + CRB_TABLE_OFFSET + sizeof(crbmsg_t) * (tail % iacpci_crb_entrynum)),
                                head, tail, 
                               ch->localga + CONNINFO_CRBMSG_OFFSET,
                                (conninfo->crbmsg).rank, (conninfo->crbmsg).ga,
                               (conninfo->crbmsg).state, (conninfo->crbmsg).buf_entrysz, (conninfo->crbmsg).rbuf_entrynum,
                               (conninfo->crbmsg).sbuf_entrynum);
#endif
                        /* change state to waiting connection ack */
                        ch->state = CHSETSTAT(ch, CHSTWTAK);
                    } else { 
                        /* no room in crb. get the head of the crb again. */
                        conninfo->hdl = acp_copy(ch->localga, conninfo->starterga, sizeof(acp_ga_t), ACP_HANDLE_NULL);
                    }
                }

                break;
            case CHSTWTAK:
                if (conninfo->state != CONNSTVALID) {
#ifdef DEBUG
                    fprintf(stderr, "%d: handl_conchlist: conn req tail %llu has been invalidated\n", 
                           myrank, conninfo->tail);
#endif
                    /* connection request is invalidated. re-initialize the request. */
                    init_conninfo(ch);
                } else if (conninfo->remotega != ACP_GA_NULL) { 
#ifdef DEBUG
                    fprintf(stderr, "%d: handl_conchlist: send conn req tail %llu has been connected to ga %p (rank %d)\n", 
                           myrank, conninfo->tail, conninfo->remotega, acp_query_rank(conninfo->remotega));
#endif
                    /* remotega has arrived. complete the connection */
                    ch->state = CHSETSTAT(ch, CHSTCONN);
                    ch->remotega = conninfo->remotega;

                    /* remove this channel from the connecting list */
                    list_remove(&conch_list, (listitem_t *)ch);

                    /* check if there are pending requests in this channel already */
                    if (list_isempty(&(ch->reqs)))
                        list_add(&idlech_list, (listitem_t *)ch); /* add this channel to idle channels list  */
                    else
                        list_add(&reqch_list, (listitem_t *)ch); /* add this channel to requesting channels list  */

                    tmpch = conninfo->nextch;

                    /* check if there is a connecting channel with the same type and peer */
                    if (tmpch != NULL) {
                        list_add(&conch_list, (listitem_t *)tmpch); /* add the next channel to the connecting channels list  */
                        init_conninfo(tmpch); /* start requesting connection of the next channel  */
                    }

                    /* clear chbody */
                    memset (ch->chbody, 0, CHSLOTOFFSET + ch->sbuf_entrynum * (ch->buf_entrysz + MSGHDRSZ));
                }
                break;
            default:
                fprintf(stderr, "handl_conchlist: rank %d : Error: wrong channel status %d\n", 
                       myrank, ch->state);
                ch_unlock();
                iacp_abort_cl();
            }
            break;
        case CHTYRECV:
            tail = *crbtail;
            crbidx = *crbhead;
            idx = crbidx % iacpci_crb_entrynum;
            while ((crbidx < tail) && (crbidx < *crbhead + iacpci_crb_entrynum)) {
                if ((crbflgtbl[idx] != 0) && (crbtbl[idx].state != CRBSTFREE) 
                    && (ch->peer == crbtbl[idx].rank)) {
                    /* check parameters of channel endpoints */
                    msg = &(crbtbl[idx]);

                    if ((msg->buf_entrysz != ch->buf_entrysz) 
                        || (msg->rbuf_entrynum != ch->rbuf_entrynum)
                        || (msg->sbuf_entrynum != ch->sbuf_entrynum)) {
                        fprintf(stderr, "handl_conchlist: rank %d : Error: channel parameter does not match. peer sz %d/%d rnum %d/%d snum %d/%d\n", 
                               myrank, msg->buf_entrysz, ch->buf_entrysz, 
                               msg->rbuf_entrynum, ch->rbuf_entrynum, 
                               msg->sbuf_entrynum, ch->sbuf_entrynum);
                        ch_unlock();
                        iacp_abort_cl();
                    }

                    /* clear the flags of the entry */
                    crbflgtbl[idx] = 0;

                    /* connect */
                    ch->state = CHSETSTAT(ch, CHSTCONN);
                    msg->state = CRBSTFREE;
                    ch->remotega = msg->ga;

                    conninfo = (conninfo_t *)(ch->chbody);
                    tmpch = conninfo->nextch;
#ifdef DEBUG
                    fprintf(stderr, "%d: handl_conchlist: recv con req idx %llu has been connected to ga %p (rank %d) crbhead %lld crbtail %lld\n", 
                            myrank, crbidx, ch->remotega, acp_query_rank(ch->remotega), *crbhead, tail);
#endif

                    /* clear chbody */
                    memset (ch->chbody, 0, CHSLOTOFFSET + ch->rbuf_entrynum * (ch->buf_entrysz + MSGHDRSZ));

                    /* send back GA of this channel to the sender */
                    acp_swap8(trashboxga, ch->remotega + CONNINFO_REMGA_OFFSET,
                              ch->localga, ACP_HANDLE_NULL);

#ifdef DEBUG
                    fprintf(stderr, "%d: handl_conchlist: send back acknowledgemnet ga %p crbhead %lld crbtail %lld\n", 
                            myrank, ch->localga, *crbhead, tail);
#endif

                    /* remove this channel from the connecting list */
                    list_remove(&conch_list, (listitem_t *)ch);

                    /* check if there are pending requests in this channel already */
                    if (list_isempty(&(ch->reqs)))
                        list_add(&idlech_list, (listitem_t *)ch); /* add this channel to idle channels list  */
                    else
                        list_add(&reqch_list, (listitem_t *)ch); /* add this channel to requesting channels list  */

                    /* check if there is a connecting channel with the same type and peer */
                    if (tmpch != NULL)
                        list_add(&conch_list, (listitem_t *)tmpch); /* add the next channel to the connecting channels list  */

                    /* quit from the loop for searching connection request buffer */
                    break;

                }
                numpendingrch++;
                crbidx++;
                idx = crbidx % iacpci_crb_entrynum;
            }

            break;
        default:
            fprintf(stderr, "handl_conchlist: rank %d : Error: wrong channel type %d\n", 
                   myrank, ch->type);
            ch_unlock();
            iacp_abort_cl();
        }
        ch = nextch;
    }
    ch_unlock();

    return numpendingrch;

}

/* check connection request buffer */
static void handl_crb(int numpendingrch)
{
    int64_t crbidx, tail;
    crbmsg_t *msg;
    int idx;
    int myrank;

    ch_lock();
    myrank = acp_rank();

    tail = *crbtail;
    crbidx = *crbhead;
    idx = crbidx % iacpci_crb_entrynum;

    while ((crbidx < tail) && (crbidx < *crbhead + iacpci_crb_entrynum) && (crbtbl[idx].state == CRBSTFREE)) {
#ifdef DEBUG
        fprintf(stderr, "%d: handl_crb: crbidx %d crbhead %lld crbtail %lld\n", 
                myrank, crbidx, *crbhead, tail);
#endif
        crbidx++;
        idx = crbidx % iacpci_crb_entrynum;
    }

    /* update crbhead */
    *crbhead = crbidx;

    if ((numpendingrch > 0) && ((tail - *crbhead) >= iacpci_crb_entrynum)) {
        /* invalidate the request at the head (== oldest request) 
         * to avoid deadlock */
        msg = &(crbtbl[idx]);
        msg->state = CRBSTFREE;
        /* increment state of connection information on the requester process
         * to invalidate the request */
        acp_add4(trashboxga, 
                 msg->ga + CONNINFO_STATE_OFFSET, 1LL, ACP_HANDLE_NULL);
        *crbhead = crbidx + 1;

#ifdef DEBUG
        fprintf(stderr, "%d: handl_crb: invalidated crbidx %lld rank %d ga %p crbhead %lld crbtail %lld\n", 
                myrank, crbidx, acp_query_rank(msg->ga), msg->ga, *crbhead, *crbtail);
#endif

    }
    ch_unlock();
}

/* setup free request list of the channel
 */
static void initreq(acp_ch_t ch)
{
    int i;
    chreqitem_t *reqitem;
    int myrank;

    myrank = acp_rank();

    ch->reqtable = (chreqitem_t *)malloc(sizeof(chreqitem_t) * iacpci_reqnum);
    if (ch->reqtable == NULL) {
        fprintf(stderr, "initreq(): %d: Error: cannot allocate reqtable\n", myrank);
        iacp_abort_cl();
    }
  
    for (i = 0; i < iacpci_reqnum - 1; i++) {
        reqitem = &(ch->reqtable[i]);
        reqitem->prev = NULL;
        reqitem->next = &(ch->reqtable[i + 1]);
        reqitem->ch = ch;
    }
    ch->reqtable[iacpci_reqnum - 1].prev = NULL;
    ch->reqtable[iacpci_reqnum - 1].next = NULL;
    ch->reqtable[iacpci_reqnum - 1].ch = ch;

    ch->freereqs = &(ch->reqtable[0]);
    list_init(&(ch->reqs));
#ifdef DEBUG
    fprintf(stderr, "%d: initreq: reqtable %p \n", myrank, ch->reqtable);
#endif
}

/* create a new request item and place it at the tail of the request list
 * of the channel.
 */
static chreqitem_t *newreq(acp_ch_t ch)
{
    chreqitem_t *req;
    int myrank;

    myrank = acp_rank();

    /* check one request entry from free request list */
    req = ch->freereqs;

    if (req != NULL) {
        /* connect the item at the tail of the request list */
        ch->freereqs = req->next;
        list_add(&(ch->reqs), (listitem_t *)req);

        /* initialize members of the request item */
        req->addr = NULL;
        req->size = 0;
        req->hdl = ACP_HANDLE_NULL;
#ifdef DEBUG
        fprintf(stderr, "%d: newreq: req %p \n", myrank, req);
#endif
    } 

    return req;
}

/* free a request item */
static int freereq(chreqitem_t *req)
{
    chitem_t *ch = req->ch;

#ifdef DEBUG
    int myrank;
    myrank = acp_rank();
    fprintf(stderr, "%d: freereq: reqtable %p \n", myrank, req);
#endif
    /* add the request to the free request list */
    req->prev = NULL;
    req->next = ch->freereqs;
    ch->freereqs = req;

    return 0;
}

/* progress send requests in a channel */
static void progress_send(acp_ch_t ch)
{
    chreqitem_t *req, *nextreq;
    int sbidx, rbidx;
    int myrank;
    char *msg;
    size_t thissize;
    acp_ga_t targetga;
    acp_handle_t hdl;

    myrank = acp_rank();
    req = (chreqitem_t *)(ch->reqs.head);

    while(req) {
        while (sbnotempty(ch)) {
            sbidx = ch->sbhead % ch->sbuf_entrynum;
            if (acp_inquire(ch->shdl[sbidx]) == 0) 
                ch->sbhead++;
            else 
                break;
        }

        if (!(sbavail(ch)) || !(rbavail(ch)))
            break;

// NANRI
#ifdef DEBUG
    fprintf(stderr, "%d: progress_send: sbavail & rbavail: ch %p sbhead %lld sbtail %lld rbhead %lld rbtail %lld req %x req->size %d\n", 
// NO ATOMIC            myrank, ch, ch->sbhead, ch->sbtail, *((uint64_t *)ch->chbody), ch->rbtail, req, req->size);
            myrank, ch, ch->sbhead, ch->sbtail, *((int64_t *)ch->chbody), *((int64_t *)ch->chbody + 1), req, req->size);
#endif
        sbidx = ch->sbtail % ch->sbuf_entrynum;
        msg = localslotaddr(ch, sbidx);

        switch(req->status){
        case REQSTEGR:
            *(int64_t *)msg = mkmsghdr(MSGTYEGR, req->size);
            thissize = (req->size < ch->buf_entrysz) ? req->size : ch->buf_entrysz;
            memcpy(msg + MSGHDRSZ, req->addr, thissize);
// NO ATOMIC            targetga = remoteslotga(ch, ch->rbtail % ch->rbuf_entrynum);
            targetga = remoteslotga(ch, *((int64_t *)ch->chbody + 1) % ch->rbuf_entrynum);
// NANRI
#ifdef DEBUG
    fprintf(stderr, "%d: progress_send: acp_copy target %p source %p size %d req->size %d\n", 
            myrank, targetga, localslotga(ch, sbidx), thissize + MSGHDRSZ, req->size);
#endif
            hdl = acp_copy(targetga, localslotga(ch, sbidx), thissize + MSGHDRSZ, ACP_HANDLE_NULL);
// NO ATOMIC            ch->rbtail++;
#ifdef DEBUG
    fprintf(stderr, "%d: progress_send: acp_add8 target %p source %p \n", 
            myrank, trashboxga, ch->remotega);
#endif
// NO ATOMIC            acp_add8(trashboxga, ch->remotega, 1LL, ch->shdl[sbidx]);
            (*((int64_t *)ch->chbody + 1))++;
//            acp_copy(ch->remotega + 8, ch->localga + 8, sizeof(uint64_t), ch->shdl[sbidx]);
            *((int64_t *)localtailla(ch, sbidx)) = *((int64_t *)ch->chbody + 1);
//            ch->shdl[sbidx] = acp_copy(ch->remotega + 8, localtailga(ch, sbidx), sizeof(int64_t), hdl);
            ch->shdl[sbidx] = acp_copy(ch->remotega + 8, localtailga(ch, sbidx), sizeof(int64_t), ACP_HANDLE_NULL);

// NANRI
#ifdef DEBUG
    fprintf(stderr, "%d: progress_send: done\n", 
            myrank);
#endif
            ch->sbtail++;
            req->addr += thissize;
            req->size -= thissize;
            if (req->size <= 0) {
                req->status = REQSTFIN;
                req->hdl = ch->shdl[sbidx];
                nextreq = req->next;
                list_remove (&(ch->reqs), (listitem_t *)req);
                req = nextreq;
            }
            break;
        case REQSTDISCONN:
            *(int64_t *)msg = mkmsghdr(MSGTYDISCONN, 0);
// NO ATOMIC            targetga = remoteslotga(ch, ch->rbtail % ch->rbuf_entrynum);
            targetga = remoteslotga(ch, *((int64_t *)ch->chbody + 1) % ch->rbuf_entrynum);
            hdl = acp_copy(targetga, localslotga(ch, sbidx), MSGHDRSZ, ACP_HANDLE_NULL);
// NO ATOMIC            ch->rbtail++;
// NO ATOMIC            acp_add8(trashboxga, ch->remotega, 1LL, ch->shdl[sbidx]);
            (*((int64_t *)ch->chbody + 1))++;
//            acp_copy(ch->remotega + 8, ch->localga + 8, sizeof(uint64_t), ch->shdl[sbidx]);
            *((int64_t *)localtailla(ch, sbidx)) = *((int64_t *)ch->chbody + 1);
//            ch->shdl[sbidx] = acp_copy(ch->remotega + 8, localtailga(ch, sbidx), sizeof(int64_t), hdl);
            ch->shdl[sbidx] = acp_copy(ch->remotega + 8, localtailga(ch, sbidx), sizeof(int64_t), ACP_HANDLE_NULL);

            ch->sbtail++;
            req->status = REQSTFIN;
            req->hdl = ch->shdl[sbidx];
            if (req->next != NULL) {
                fprintf(stderr, "progress_send: %d : Error: Request is not empty after disconnect: ch %p\n", myrank, ch);
                iacp_abort_cl();
            }
            list_remove (&(ch->reqs), (listitem_t *)req);
            req = NULL;
#ifdef DEBUG
    fprintf(stderr, "%d: progress_send: sent discon\n", 
            myrank);
#endif
           
            break;
        default:
            fprintf(stderr, "progress_send: %d : Error: Wrong request status %d\n", myrank, req->status);
            iacp_abort_cl();
        }
    }
}

/* progress recv requests in a channel */
static void progress_recv(acp_ch_t ch)
{
    chreqitem_t *req, *nextreq;
    char *msg;
    int64_t msghdr, msgtype;
    int myrank, rbidx;
    size_t msgsz, thissize, tmpsize;
    acp_handle_t hdl;

    myrank = acp_rank();
    req = (chreqitem_t *)(ch->reqs.head);

    while (req) {
        if (!msgarrive(ch)) 
            break;

// NO ATOMIC        rbidx = ch->rbhead % ch->rbuf_entrynum;
        rbidx = *((int64_t *)ch->chbody) % ch->rbuf_entrynum;
        msg = localslotaddr(ch, rbidx);
        msghdr = *((int64_t *)msg);
        msgtype = msghdr & MSGTYPEMASK;

// NANRI
#ifdef DEBUG
    fprintf(stderr, "%d: progress_recv: msgarrive: ch %p rbhead %lld rbtail %lld type %p size %lld crbhead %lld crbtail %lld\n", 
// NO ATOMIC            myrank, ch,  ch->rbhead,  *((uint64_t *)ch->chbody), msgtype, msghdr & MSGSIZEMASK);
            myrank, ch,  *((int64_t *)ch->chbody),  *((int64_t *)ch->chbody + 1), msgtype, msghdr & MSGSIZEMASK, *crbhead, *crbtail);
#endif

        switch (req->status) {
        case REQSTINIT:
        case REQSTEGR:
            /* At this point, since there is only one protocol, 
             * REQSTINIT does the same thing as REQSTEGR.
             * When other protocols become available, REQSTINIT chooses
             * the protocol to use according to the first message.
             */
            if (msgtype != MSGTYEGR){
                fprintf(stderr, "progress_recv: %d : Error: Types of the request and the message does not match req: %d, msg: %lld\n", 
                        myrank, req->status, msgtype);
                iacp_abort_cl();
            }
            if (req->size > req->receivedsize){
                msgsz = msghdr & MSGSIZEMASK;
                thissize = (msgsz < ch->buf_entrysz) ? msgsz : ch->buf_entrysz;
                tmpsize = req->size - req->receivedsize;
                thissize = (thissize < tmpsize) ? thissize : tmpsize;
                memcpy(req->addr, msg + MSGHDRSZ, thissize);
                req->receivedsize += thissize;
                req->addr += thissize;
            }
// NO ATOMIC            ch->rbhead++;
// NO ATOMIC            acp_add8(trashboxga, ch->remotega, 1LL, ACP_HANDLE_NULL);
            (*((int64_t *)ch->chbody))++;
            hdl = acp_copy(ch->remotega, ch->localga, sizeof(int64_t), ACP_HANDLE_NULL);
            acp_complete(hdl);
            if (msgsz <= ch->buf_entrysz) {
                req->status = REQSTFIN;
                nextreq = req->next;
                list_remove(&(ch->reqs), (listitem_t *)req);
                req = nextreq;
            } else {
                req->status = REQSTEGR;
            }

// NANRI
#ifdef DEBUG
            fprintf(stderr, "%d: progress_recv: end crbhead %lld crbtail %lld\n", myrank, *crbhead, *crbtail);
// NO ATOMIC            myrank, ch,  ch->rbhead,  *((uint64_t *)ch->chbody), msgtype, msghdr & MSGSIZEMASK);
#endif
            break;
        case REQSTDISCONN:
#ifdef DEBUG
    fprintf(stderr, "%d: progress_recv: received discon crbhead %lld crbtail %lld\n", 
            myrank, *crbhead, *crbtail);
#endif
            if (msgtype != MSGTYDISCONN){
                fprintf(stderr, "progress_recv: %d : Error: Types of the request and the message does not match req: %d, msg: %lld\n", 
                        myrank, req->status, msgtype);
                iacp_abort_cl();
            }

// NO ATOMIC            ch->rbhead++;
// NO ATOMIC            acp_add8(trashboxga, ch->remotega, 1LL, ACP_HANDLE_NULL);
            (*((int64_t *)ch->chbody))++;
            hdl = acp_copy(ch->remotega, ch->localga, sizeof(int64_t), ACP_HANDLE_NULL);
            acp_complete(hdl);

            ch->state = CHSTDISCONN;
            req->status = REQSTFIN;

            if (req->next != NULL) {
                fprintf(stderr, "progress_recv: %d : Error: Request is not empty after disconnect: ch %p\n", myrank, ch);
                iacp_abort_cl();
            }
            list_remove (&(ch->reqs), (listitem_t *)req);
            req = NULL;

            break;
        default:
            fprintf(stderr, "progress_recv: %d : Error: Wrong status of the request: %d\n", myrank, req->status);
            iacp_abort_cl();
        }
    }
}

/* check requesting channels */
static void handl_reqchlist(void)
{
    acp_ch_t ch, nextch;
    int myrank;

    ch_lock();
    myrank = acp_rank();
    ch = (acp_ch_t)(reqch_list.head);

    while (ch != NULL) {
        switch(ch->type) {
        case CHTYSEND:
            progress_send(ch);
            break;
        case CHTYRECV:
            progress_recv(ch);
            break;
        default:
            fprintf(stderr, "handl_reqchlist: rank %d : Error: wrong channel type %d\n", 
                   myrank, ch->type);
            ch_unlock();
            iacp_abort_cl();
        }

        nextch = ch->next;
        /* if request list is empty, move the channel to the idle channels list */
        if (list_isempty(&(ch->reqs))) {
            list_remove(&reqch_list, (listitem_t *)ch);
            list_add(&idlech_list, (listitem_t *)ch);
#ifdef DEBUG
            fprintf(stderr, "%d: handl_reqchlist: move channel %p from request list to idle list crbhead %lld crbtail %lld\n", myrank, ch, *crbhead, *crbtail);
#endif
        }
        ch = nextch;
    }
    ch_unlock();
}

/* progress */
void iacpcl_progress_ch(void)
{
    int numpendingrch;
    /* check connecting channels */
    numpendingrch = handl_conchlist();

    /* check connection request buffer */
    handl_crb(numpendingrch);

    /* check requesting channels */
    handl_reqchlist();

}

/* int iacp_init_cl(void)
 *  return: 0 (success)
 *  
 *  error: aborts when starter memory is less than crb size
 *
 *  contents:
 *  - set parameters
 *  - prepare crb (connection request buffer)
 *  - prepare channel lists
 *  - synchronize among processes
 * 
 */
static int init_ch(void)
{
    int myrank;

    myrank = acp_rank();

    /* set addresses of crb */
    crbhead = (int64_t *)acp_query_address(iacp_query_starter_ga_cl(myrank));
    crbtail = (int64_t *)((char *)crbhead + CRB_TAIL_OFFSET);
    crbtbl = (crbmsg_t *)((char *)crbhead + CRB_TABLE_OFFSET);
    crbflgtbl = (char *)crbhead + CRB_FLGTABLE_OFFSET;

#ifdef DEBUG
    fprintf(stderr, "%d: iacp_init_cl: crbhead %p, crbtail %p, crbtbl %p, trashboxga %p\n", 
           myrank, crbhead, crbtail, crbtbl, trashboxga);
#endif

    /* clear crb tbl */
    memset (crbtbl, 0, iacpci_crb_entrynum * sizeof(crbmsg_t));

    /* clear crb flgtbl */
    memset (crbflgtbl, 0, iacpci_crb_entrynum);

    /* initialize counters of crb */
    *crbhead = 0LL;
    *crbtail = 0LL;

    /*                                         
     * initialize lists
     */
    list_init(&conch_list);
    list_init(&idlech_list);
    list_init(&reqch_list);

}

int iacp_init_cl(void) 
{
    int myrank;

    ch_lock();
    if (iacp_initialized_cl != 0){
        ch_unlock();
        return 0;
    }
    iacp_initialized_cl = 1;

    myrank = acp_rank();

    /* crb (connection request buffer for Channel) */
    crbsz = 2 * (sizeof(int64_t)) + iacpci_crb_entrynum * sizeof(crbmsg_t) + iacpci_crb_entrynum;
    /* crq (connection request queue for Segbuf) */
    crqsz = 16 + crq_num_slots * sizeof(acp_ga_t);

    /* starter_memory_cl = crb + crq + trashbox */
    if (iacp_starter_memory_size_cl < (crbsz + crqsz + sizeof(acp_ga_t))) {
        fprintf(stderr, "iacp_init_cl: %d : Error iacp_starter_memory_size_cl %lu is too small for %d + %d + %d\n",
                myrank, iacp_starter_memory_size_cl, crbsz, crqsz, sizeof(acp_ga_t));
        iacp_abort_cl();
    }
#ifdef DEBUG
    fprintf(stderr, "%d: iacp_init_cl: crbsz %d crqsz %d\n", myrank, crbsz, crqsz);
#endif

    trashboxga = iacp_query_starter_ga_cl(myrank) + crbsz + crqsz;

    init_ch();

    init_segbuf();

    /* synchronize */
    acp_sync();
  
    ch_unlock();

    return 0; 
};

static int finalize_ch(void)
{
    acp_ch_t ch;
    /* wait until lists conch_list, reqch_list and disconnch_list becomes empty
     *  (all channel endpoints must be disconnected or in idle list
     */
    while ((conch_list.head != NULL) || (reqch_list.head != NULL)){
        iacpcl_progress();
    }

    /* NOTE: acp_sync is still available in the finalization function? */
    acp_sync();

    ch_lock();
    /* free channels */
    ch = (acp_ch_t)idlech_list.head;
    while(ch != NULL) {
        list_remove(&idlech_list, (listitem_t *)ch);
        acp_unregister_memory(ch->localatkey);
        free(ch->chbody);
        free(ch->reqtable);
        if (ch->shdl != NULL)
            free(ch->shdl);
        ch = (acp_ch_t)idlech_list.head;
    }
    ch_unlock();
}

int iacp_finalize_cl(void) 
{
    ch_lock();

    if (iacp_initialized_cl == 0){
        ch_unlock();
        return 0;
    }
    iacp_initialized_cl = 0;
    ch_unlock();

    finalize_ch();
    finalize_segbuf();

#ifdef DEBUG
    fprintf(stderr, "finalize: done \n");
#endif

    return 0; 
};

void iacp_abort_cl(void) 
{ 
    exit (-1);
};

/* acp_ch_t acp_create_ch(int src, int dst)
 *  parameters: 
 *    int src : source process of the channel
 *    int dst : destination process of the channel
 *
 *  return:
 *    acp_ch_t : success. a channel handle.
 *
 *  error:
 *    - src or dst is not within the 0 <= p < procs
 *    - neither src nor dst is myrank
 *    - src == dst
 *    - memory not available
 *    
 *  behavior
 *    - allocate channel endpoint and buffers
 *    - initialize channel parameters
 *    - setup a connection request message in the send channel buffer
 *    - connect this channel to conch_list
 *    - progress
 *
 *  calls
 *    - initreq()
 *    - list_add()
 *
 *  allocate
 *    - src process:
 *        endpoint: sizeof(chitem_t)
 *        send buffer: 8*3+sbuf_entrynum*(buf_entrysz+MSGHDRSZ)
 *        send handle table: sizeof(acp_handle_t)*sbuf_entrynum
 *        requests: sizeof(chreqitem_t)*acpch_chreqnum
 *    - dst process: 
 *        endpoint: sizeof(chitem_t)
 *        receive buffer: 8*3+rbuf_entrynum*(buf_entrysz+MSGHDRSZ)
 *        requests: sizeof(chreqitem_t)*acpch_chreqnum
 *
 */

acp_ch_t acp_create_ch(int src, int dst)
{
    int myrank, procs, type;
    size_t memsize;
    acp_ch_t ch, conch;
    conninfo_t *conninfo;

    ch_lock();
    myrank = acp_rank();
    procs = acp_procs();

#ifdef DEBUG
    fprintf(stderr, "%d: create ch: %d %d crbhead %lld crbtail %lld\n", myrank, src, dst, *crbhead, *crbtail);
#endif
    /* check parameters */
    if ((src < 0) || (src >= procs) || (dst < 0) || (dst >= procs)) {
        fprintf(stderr, "acp_create_ch: rank %d : Error: Wrong paramter src %d, dst %d (procs = %d) \n",
               myrank, src, dst, procs);
        ch_unlock();
        iacp_abort_cl();
    }
    if (src == dst) {
        fprintf(stderr, "acp_create_ch: rank %d : Error: ACP does not support loop back channel (src %d, dst %d) \n",
               myrank, src, dst);
        ch_unlock();
        iacp_abort_cl();
    }

    /* determine sender or receiver */
    if (src == myrank) {
        type = CHTYSEND;
    } else if (dst == myrank) {
        type = CHTYRECV;
    } else {
        fprintf(stderr, "acp_create_ch: rank %d : Error: ACP does not support remote-to-remote channel (src %d, dst %d) \n",
               myrank, src, dst);
        ch_unlock();
        iacp_abort_cl();
    }

    /* allocate channel endpoint */
    ch = (acp_ch_t)malloc(sizeof(chitem_t));
    if (ch == NULL) {
        fprintf(stderr, "acp_create_ch: rank %d : Error: cannot allocate channel endpoint structure \n", myrank);
        ch_unlock();
        iacp_abort_cl();
    }

    /* set common members */
    ch->type = type;
    ch->state = CHSTINIT;
    ch->buf_entrysz = iacpci_eager_limit;
    ch->sbuf_entrynum = iacpci_sbnumslots;
    ch->rbuf_entrynum = iacpci_rbnumslots;

    /* setup request list */
    initreq(ch);

    /* set other members */
    if (ch->type == CHTYSEND) { /* sender */
        ch->peer = dst;

        memsize = CHSLOTOFFSET + ch->sbuf_entrynum * (ch->buf_entrysz + MSGHDRSZ) + ch->sbuf_entrynum * sizeof(int64_t);
        ch->shdl = (acp_handle_t *)malloc(sizeof(acp_handle_t) * ch->sbuf_entrynum);
    } else { /* receiver */
        ch->peer = src;

        memsize = CHSLOTOFFSET + ch->rbuf_entrynum * (ch->buf_entrysz + MSGHDRSZ);
        ch->shdl = NULL;
    }

    ch->chbody = (char *)malloc(memsize);
    if (ch->chbody == NULL) {
        fprintf(stderr, "acp_create_ch: rank %d : Error: cannot allocate chbody \n", myrank);
        ch_unlock();
        iacp_abort_cl();
    }

    /* initialize head and tail */
    ch->sbhead = 0LL;
    ch->sbtail = 0LL;
// NO ATOMIC    ch->rbhead = 0LL;
// NO ATOMIC    ch->rbtail = 0LL;

    ch->localatkey = acp_register_memory(ch->chbody, memsize, 0);
    if (ch->localatkey == ACP_ATKEY_NULL){
        fprintf(stderr, "acp_create_ch: rank %d : Error: acp_register_memory failed for creating channel. \n", myrank);
        ch_unlock();
        iacp_abort_cl();
    }
    ch->localga = acp_query_ga(ch->localatkey, ch->chbody);
    if (ch->localga == ACP_ATKEY_NULL){
        fprintf(stderr, "acp_create_ch: rank %d : Error: acp_query_ga failed for creating channel. \n", myrank);
        ch_unlock();
        iacp_abort_cl();
    }

#ifdef DEBUG
    fprintf(stderr, "%d: acp_create_ch: ch %p chbody %p ch->reqs.head %p localga %016llx sz %d sb %d rb %d crbhead %lld crbtail %lld\n", 
           myrank, ch, ch->chbody, ch->reqs.head, ch->localga, 
            ch->buf_entrysz, ch->sbuf_entrynum, ch->rbuf_entrynum,
            *crbhead, *crbtail);
#endif

    conninfo = (conninfo_t *)(ch->chbody);
    conninfo->nextch = NULL;
    if (ch->type == CHTYSEND) {
        /* initialize chbody to be used as a connection request structure */
        (conninfo->crbmsg).rank = myrank;
        (conninfo->crbmsg).ga = ch->localga;
        (conninfo->crbmsg).state = CRBSTINUSE;
        (conninfo->crbmsg).buf_entrysz = ch->buf_entrysz;
        (conninfo->crbmsg).rbuf_entrynum = ch->rbuf_entrynum;
        (conninfo->crbmsg).sbuf_entrynum = ch->sbuf_entrynum;
#ifdef DEBUG
        fprintf(stderr, "%d: acp_create_ch: crbmsg %d %016llx %d %d %d %d \n", 
               myrank, (conninfo->crbmsg).rank, (conninfo->crbmsg).ga,
               (conninfo->crbmsg).state, (conninfo->crbmsg).buf_entrysz, (conninfo->crbmsg).rbuf_entrynum,
               (conninfo->crbmsg).sbuf_entrynum);
#endif
        conninfo->starterga = iacp_query_starter_ga_cl(ch->peer);
        conninfo->remotega = ACP_GA_NULL;
    }

    /* prepare value 1 to be used for sending flag to CRB */
    *((char *)(ch->chbody) + CRBFLG_OFFSET) = 1;

    /* search for the connection requests with same peer and type */
    conch = (acp_ch_t)(conch_list.head);
    while (conch != NULL) {
        if ((conch->type == ch->type) && (conch->peer == ch->peer))
            break;
        conch = conch->next;
    }

    if (conch != NULL) { 
        /* found connecting channel with the same peer and type.
         * add this channel to the tail of the list of connecting channels 
         * with the same peer and type. */
        while (((conninfo_t *)(conch->chbody))->nextch != NULL)
            conch = ((conninfo_t *)(conch->chbody))->nextch;
        ((conninfo_t *)(conch->chbody))->nextch = ch;
#ifdef DEBUG
        fprintf(stderr, "%d: acp_create_ch: added a channel with peer %d and type %d crbhead %lld crbtail %lld\n", 
                myrank, ch->peer, ch->type, *crbhead, *crbtail);
#endif

    } else {
        /* there is no connecting channels with the same peer and type */
        list_add(&conch_list, (listitem_t *)ch);

        /* this channel is in the head. start getting head (and adding_and_fetching tail) */
        if (ch->type == CHTYSEND)
            init_conninfo(ch);
#ifdef DEBUG
        fprintf(stderr, "%d: acp_create_ch: added and initiated a channel with peer %d and type %d crbhead %lld crbtail %lld\n", 
                myrank, ch->peer, ch->type, *crbhead, *crbtail);
#endif
    }

    ch_unlock();

    /* progress requests */
    iacpcl_progress();

    /* return channel endpoint handle */
    return ch;
}

acp_request_t acp_nbfree_ch(acp_ch_t ch)
{
    chreqitem_t *req;
    int myrank;

    ch_lock();
    myrank = acp_rank();

#ifdef DEBUG
    fprintf(stderr, "%d: nbfree_ch: %p crbhead %lld crbtail %lld\n", myrank, ch, *crbhead, *crbtail);
#endif
    if (CHCHECKDISCON(ch) != 0) {
        fprintf(stderr, "acp_nbfree_ch: rank %d : Error: channel has already been requested to disconnect\n", 
               myrank);
        ch_unlock();
        iacp_abort_cl();
    }

    /* if the request list of this channel is empty, move it to reqch_list  */
    if ((CHCHECKSTAT(ch) == CHSTCONN) && (list_isempty(&(ch->reqs)))) {
        list_remove(&idlech_list, (listitem_t *)ch);
        list_add(&reqch_list, (listitem_t *)ch);
#ifdef DEBUG
        fprintf(stderr, "%d: acp_nbfree_ch: moved channel %p from idle to requesting crbhead %lld crbtail %lld\n", 
                myrank, ch, *crbhead, *crbtail);
#endif
    }

    /* add disconnection request */
    req = newreq(ch);
    if (req != NULL) {
        /* set message info to req */
        req->addr = NULL;
        req->size = 0;
        req->status = REQSTDISCONN;
        /* change status to disconnecting */
        ch->state = CHSETDISCON(ch);
    }

    ch_unlock();

    /* progress */
    iacpcl_progress();

    return req;
}

int acp_free_ch(acp_ch_t ch)
{
    acp_request_t req;

    req = acp_nbfree_ch(ch);
    acp_wait_ch(req);

    return 0;
}

acp_request_t acp_nbsend_ch(acp_ch_t ch, void *sbuf, size_t sz)
{
    chreqitem_t *req;
    int myrank;

    ch_lock();
    myrank = acp_rank();

#ifdef DEBUG
    fprintf(stderr, "%d: nbsend: ch %p \n", myrank, ch);
#endif

    if (sz > MSGMAXSZ){
        fprintf(stderr, "acp_nbsend_ch: rank %d : Error: message size is too large: %lu\n", 
                myrank, sz);
        ch_unlock();
        iacp_abort_cl();
    }

    if (CHCHECKDISCON(ch) != 0) {
        fprintf(stderr, "acp_nbsend_ch: rank %d : Error: requesting nbsend to the channel that has been requested to disconnect\n", 
               myrank);
        ch_unlock();
        iacp_abort_cl();
    }

    /* if the request list of this channel is empty, move it to reqch_list  */
    if ((CHCHECKSTAT(ch) == CHSTCONN) && (list_isempty(&(ch->reqs)))) {
        list_remove(&idlech_list, (listitem_t *)ch);
        list_add(&reqch_list, (listitem_t *)ch);
#ifdef DEBUG
        fprintf(stderr, "%d: acp_nbsend_ch: moved channel %p from idle to requesting\n", 
               myrank, ch);
#endif
    }

    /* get a new request item */
    req = newreq(ch);
    if (req != NULL){
        /* set message info to req */
        req->addr = sbuf;
        req->size = sz;
        req->status = REQSTEGR;
#ifdef DEBUG
            fprintf(stderr, "%d: acp_nbsend_ch: prepared eager mesg in channel %p \n", 
                    myrank, ch);
#endif
    }

    ch_unlock();

    /* progress */
    iacpcl_progress();


    return req;
}

acp_request_t acp_nbrecv_ch(acp_ch_t ch, void *rbuf, size_t sz)
{
    chreqitem_t *req;
    int myrank;

    ch_lock();

    myrank = acp_rank();

#ifdef DEBUG
    fprintf(stderr, "%d: nbrecv: ch %p crbhead %lld crbtail %lld\n", myrank, ch, *crbhead, *crbtail);
#endif
    if (sz > MSGMAXSZ){
        fprintf(stderr, "acp_nbrecv_ch: rank %d : Error: message size is too large: %lu\n", 
                myrank, sz);
        ch_unlock();
        iacp_abort_cl();
    }

    if (CHCHECKDISCON(ch) != 0) {
        fprintf(stderr, "acp_nbrecv_ch: rank %d : Error: channel has been requested to disconnect\n", 
               myrank);
        ch_unlock();
        iacp_abort_cl();
    }

    /* if the request list of this channel is empty, move it to reqch_list  */
    if ((CHCHECKSTAT(ch) == CHSTCONN) && (list_isempty(&(ch->reqs)))) {
        list_remove(&idlech_list, (listitem_t *)ch);
        list_add(&reqch_list, (listitem_t *)ch);
#ifdef DEBUG
        fprintf(stderr, "%d: acp_nbrecv_ch: moved channel %p from idle to requesting crbhead %lld crbtail %lld\n", 
                myrank, ch, *crbhead, *crbtail);
#endif
    }

    /* get a new request item */
    req = newreq(ch);
    if (req != NULL){
        /* set message info to req */
        req->addr = rbuf;
        req->size = sz;
        req->receivedsize = 0;
        req->status = REQSTINIT;
        
#ifdef DEBUG
        fprintf(stderr, "%d: acp_nbrecv_ch: prepared receive in channel %p crbhead %lld crbtail %lld\n", 
                myrank, ch, *crbhead, *crbtail);
#endif
    }

    ch_unlock();

    /* progress */
    iacpcl_progress();

    return req;
}

int acp_send_ch(acp_ch_t ch, void *sbuf, size_t sz)
{
    acp_request_t req;

    req = acp_nbsend_ch(ch, sbuf, sz);
    acp_wait_ch(req);

    return 0;
}

int acp_recv_ch(acp_ch_t ch, void *rbuf, size_t sz)
{
    acp_request_t req;

    req = acp_nbrecv_ch(ch, rbuf, sz);
    acp_wait_ch(req);

    return 0;
}

size_t acp_wait_ch(acp_request_t req)
{
    acp_ch_t ch;
    int myrank;
    int ret;
    size_t retsz = 0;

    myrank = acp_rank();
    ch = req->ch;

#ifdef DEBUG
    fprintf(stderr, "%d: wait: ch %p crbhead %lld crbtail %lld \n", myrank, ch, *crbhead, *crbtail);
#endif

    switch(ch->type) {
    case CHTYSEND:
        while ((req->status != REQSTFIN) || (acp_inquire(req->hdl)!=0))
            iacpcl_progress();
        break;
    case CHTYRECV:
        while (req->status != REQSTFIN) 
            iacpcl_progress();
        retsz = req->receivedsize;
        break;
    default:
        fprintf(stderr, "acp_wait_ch: rank %d : Error: Wrong channel type %d\n", myrank, ch->type);
        iacp_abort_cl();
    }

    if (ch->state != CHSTDISCONN){
        ch_lock();
        freereq(req);
        ch_unlock();
    } else {
        if (ch->type == CHTYSEND){
// NO ATOMIC            while ((ch->rbtail - *((uint64_t *)ch->chbody)) > 0)
            while ((*((int64_t *)ch->chbody + 1) - *((int64_t *)ch->chbody)) > 0)
                iacpcl_progress();
        }
        ch_lock();
        ret = acp_unregister_memory(ch->localatkey); /* unregister chbody */
        if (ret < 0){
            fprintf(stderr, "acp_wait_ch: rank %d : Error: acp_unregister_memory failed for unregistering chbody. \n", myrank);
            ch_unlock();
            iacp_abort_cl();
        }
        list_remove(&idlech_list, (listitem_t *)ch);
        free(ch->chbody);
        free(ch->reqtable);
        if (ch->shdl != NULL)
            free(ch->shdl);
        free(ch);
        ch_unlock();
    }

    return retsz;
}
