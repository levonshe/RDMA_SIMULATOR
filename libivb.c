#define _GNU_SOURCE
#include <stdlib.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <malloc.h>
#define PAGE_SIZE 4096
#include <stdbool.h>

#include <infiniband/verbs.h>
#define container_of(ptr, type, member) ({                      \
        const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
        (type *)( (char *)__mptr - offsetof(type,member) );})

#define RCV_MAX 250
struct ibv_device **ibv_get_device_list(int *num_devices){
    if ( num_devices)
        *num_devices =1;
    struct ibv_device *dev = malloc(sizeof( struct ibv_device));
    struct ibv_device **list = calloc( 2, sizeof(*dev));
    list[0] = dev;
    list[1] =  NULL;
    return list;
}

void ibv_free_device_list(struct ibv_device **list){
    free ( list[0]);
    free( list);
}
#define QP_MAX 16
#define CQ_MAX 16
#define MR_BUFFERS_MAX 16
/* we need to save MR  and WR  Dor this purpose we extend ibv_context */
typedef struct __shmem_context {
    struct ibv_context *context;
    uint16_t mr_index;
    uint16_t qp_index;
    uint32_t qp_num;
    struct ibv_qp *qp_arr[QP_MAX];
    void * mr[BUFFERS_MAX];  //my buffers which remote should read
    void * snd_wr[RCV_MAX]; // my wrire requests 
    void * rcv_wr[RCV_MAX];  // // Received  from Peer 
    void * snd_cq[CQ_MAX];  // Completed Sends to Peer 
    void * rcv_cq[CQ_MAX];  // Completed Reception  from Peer 
} shmem_context_t;
struct ibv_context *ibv_open_device(struct ibv_device *device){
    shmem_context_t * ctx = malloc(sizeof(shmem_context_t));
    ctx->context  = malloc(sizeof(struct ibv_context));
    ctx->mr_index = 0;
    ctx->qp_index = 0;
    // attach to shmem  q
    return ctx->context;
}

int ibv_close_device(struct ibv_context *context){
    shmem_context_t * ctx = container_of(context, shmem_context_t,context );
    free (context);
    free(ctx);
    return 0;
}

struct ibv_pd *ibv_alloc_pd(struct ibv_context *context) {
    struct ibv_pd * pd = malloc(sizeof(struct ibv_pd));
    pd->context = context;
    pd->handle = 1;
    return pd;
}
/**
 * ibv_create_comp_channel - Create a completion event channel
 */
struct ibv_comp_channel *ibv_create_comp_channel(struct ibv_context *context){
    struct ibv_comp_channel * cc = malloc(sizeof(struct ibv_comp_channel));
    cc->context = context;
    cc->fd =-1;
    cc->refcnt = 1;
    return cc;
}

int ibv_destroy_comp_channel(struct ibv_comp_channel *cc){
    free(cc);
    return 0;
}
/**
 * ibv_req_notify_cq - Request completion notification on a CQ.  An
 *   event will be added to the completion channel associated with the
 *   CQ when an entry is added to the CQ.
 * @cq: The completion queue to request notification for.
 * @solicited_only: If non-zero, an event will be generated only for
 *   the next solicited CQ entry.  If zero, any CQ entry, solicited or
 *   not, will generate an event.
 * 
 * @todo :LEV 
 */
int ibv_req_notify_cq(struct ibv_cq *cq, int solicited_only){
    return 1;
}


/**
 * ibv_get_cq_event - Read next CQ event
 * @channel: Channel to get next event from.
 * @cq: Used to return pointer to CQ.
 * @cq_context: Used to return consumer-supplied CQ context.
 *
 * All completion events returned by ibv_get_cq_event() must
 * eventually be acknowledged with ibv_ack_cq_events().
 */
int ibv_get_cq_event(struct ibv_comp_channel *channel,
		     struct ibv_cq **cq, void **cq_context){
    struct ibv_cq *c= malloc(sizeof(struct ibv_cq);;
    c->cq_context = channel->context;
    c->comp_events_completed = 1;
    *cq=c;
    *cq_context=c->cq_context;
    return 0;

}
void ibv_ack_cq_events(struct ibv_cq *cq, unsigned int nevents) {}


int ibv_dealloc_pd(struct ibv_pd *pd){
    free(pd);
    return 0;
}

struct ibv_qp *ibv_create_qp(struct ibv_pd *pd,
                    struct ibv_qp_init_attr *qp_init_attr){
    struct ibv_qp * qp = malloc(sizeof(struct ibv_qp));
     // This pair is limited to 8 WR

    shmem_context_t * ctx = container_of(pd->context, shmem_context_t, context);

    for ( uint8_t i = 0; i< QP_MAX; i++){
        if ( ctx->qp_arr[i] == NULL) {
            ctx->qp_arr[i] = qp;
            ctx->qp_index++;
            assert ( ctx->qp_index < QP_MAX);
            qp->qp_num = i;
        }
    }
    return qp;
}                     

int ibv_destroy_qp(struct ibv_qp *qp) {

    for ( uint8_t i = 0; i< QP_MAX; i++){
        if ( ctx->qp_arr[i] == qp) {
            ctx->qp_arr[i] = NULL;
            ctx->qp_index--;
        }
    }
    free(qp);
    return 0;
}


int ibv_query_port(struct ibv_context *context, uint8_t port_num,
                        struct ibv_port_attr *port_attr){
    port_attr->state=IBV_PORT_ACTIVE; 
    return 0;
}
/*
* We need to create shared memory from addr and then notify peer of this addrees
*/


struct ibv_mr *ibv_reg_mr(struct ibv_pd *pd, void *addr,
                                 size_t length, int access){
   int rc;
   struct ibv_mr *mr = NULL;
   rc = posix_memalign( &addr, PAGE_SIZE, length);
   assert (rc == 0);
   rc = mmap(addr, length, PROT_READ|PROT_WRITE, 
                MAP_SHARED|MAP_FIXED|MAP_ANONYMOUS,-1, 0 );
   assert (rc == 0);
   struct ibv_mr *mr= malloc( sizeof (*mr));
   // need  to alighn on page boundary
   mr->addr = addr;
   mr->length = length;
   mr->handle= 0;
  
   mr->pd = pd;
   shmem_context_t * ctx = container_of(pd->context, shmem_context_t, context );
   for ( uint8_t i = 0; i< MR_BUFFERS_MAX; i++) {
       if ( ctx->mr[i] == NULL) {
            ctx->mr[i] = mr;
            ctx->mr_index++;
            mr->lkey = i; // key of local memory reqion
            mr->rkey = i; // key of remote memory reqion
       }
   }
   return mr;
}

 int ibv_dereg_mr(struct ibv_mr *mr){
    shmem_context_t * ctx = container_of(mr->pd->context, shmem_context_t, context );

    for ( uint8_t i = 0; i< MR_BUFFERS_MAX; i++) {
            if ( ctx->mr[i] == mr) {
                ctx->mr[i] = NULL;
                ctx->mr_index--;
            }
    }
    munmap(mr->addr, mr->length);
    free(mr);
    return 0;
 }

 int ibv_query_device(struct ibv_context *context,
                            struct ibv_device_attr *device_attr)
{
    memset(device_attr, 1, sizeof(*device_attr));
    device_attr->max_pd=1;
    device_attr->max_qp=1;
    device_attr->max_qp_wr=1;
    device_attr->max_sge=1;
    device_attr->max_sge_rd=1;
    device_attr->max_mr_size=4096;

    return 0;
}

int ibv_query_pkey(struct ibv_context *context, uint8_t port_num,
                          int index, uint16_t *pkey)
{
    *pkey=1;                              
    return 0;
}

int ibv_query_gid(struct ibv_context *context, uint8_t port_num,
                         int index, union ibv_gid *gid)
{                            
    return 999;
}

typedef struct {
    struct ibv_context *context;
    int cqe;
    struct ibv_cq *cq;
     void *cq_context;
     struct ibv_comp_channel *channel;
    int comp_vector;
}  completion_queue_t;


struct ibv_cq *ibv_create_cq(struct ibv_context *context, int cqe,
                                    void *cq_context,
                                    struct ibv_comp_channel *channel,
                                    int comp_vector){
    completion_queue_t *tmp = malloc( sizeof(*tmp));
    tmp->comp_vector = comp_vector;
    tmp->channel = channel;
    
    tmp->context = context;
    tmp->cqe = cqe;

    struct ibv_cq * cq = calloc(cqe, sizeof(struct ibv_cq));
    tmp->cq = cq;
    cq->context = context;
    cq->cq_context = cq_context;
    cq->channel = channel;
    cq->cqe = 0;
    return cq;
}
                                    
int ibv_destroy_cq(struct ibv_cq *cq){
   // completion_queue_t *tmp;
    // *todo tmp=container_of
    free(cq);
    //@todo free tmp
    return 0;
}
/*
 Remote WR are registered in the QP */
/* Returns only 1 element */
int ibv_poll_cq(struct ibv_cq *cq, int num_entries,
                       struct ibv_wc *wc){
   shmem_context_t * ctx = container_of(cq->context, shmem_context_t, context );
   for ( uint8_t i = 0; i< 8; i++){
        if (ctx->snd_wr[i]) {
            wc->opcode =IBV_WC_SEND ;
            wc->status = IBV_WC_SUCCESS;
            ctx->snd_wr[i] = NULL ; //perhaps free?
            return 1;
        }
        if (ctx->rcv_wr[i]) {
            wc->opcode = IBV_WC_RECV ;
            wc->status = IBV_WC_SUCCESS;
            ctx->rcv_wr[i] = NULL ; //perhaps free?
            return 1;
        }
    }
   return 0;
}

int ibv_modify_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr,
                         int attr_mask){
    return 0;
}
/* Put WR into QP rcv buf
* Emulator - @TODO ignore bad_wr for now
*/
int ibv_post_recv(struct ibv_qp *qp, struct ibv_recv_wr *wr,
				struct ibv_recv_wr **bad_wr){
    shmem_context_t * ctx = container_of(qp->context, shmem_context_t, context );
    for ( uint8_t i = 0; i< RCV_MAX; i++){
        if (ctx->rcv_wr[i] == NULL) {
            ctx->rcv_wr[i] = wr;
        }
    }
    return 0;
}
/*
#include <rdma/rdma_verbs.h>

       int  rdma_post_read  (struct  rdma_cm_id  *id,  void *context, void *addr, size_t length,
       struct ibv_mr *mr, int flags, uint64_t remote_addr, uint32_t rkey);
*/