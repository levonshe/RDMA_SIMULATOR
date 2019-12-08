#define _GNU_SOURCE
#define DEBUG
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <malloc.h>

#include <sys/stat.h>        /* For mode constants */
#include <fcntl.h>           /* For O_* constants */
#include <stdbool.h>
#include <assert.h>

#include <infiniband/verbs.h>
#define container_of(ptr, type, member) ({                      \
        const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
        (type *)( (char *)__mptr - offsetof(type,member) );})

#define RCV_MAX 250
#define PAGE_SIZE 4096
#define QP_MAX 16
#define PD_MAX 8
#define CQ_MAX 16
#define MR_BUFFERS_MAX 16
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
typedef struct _per_wr {
    struct ibv_sge *sg;
    int wr_shared_mem_fd;
    size_t total_sq_size;
    size_t total_sq_elems;
    void * mapped_sgs;
    bool rcv_signalled; // Signalled in poll_cq that receie is completed,
} sge_of_wr_t;


typedef struct _per_qp {
    struct ibv_qp qp;
    struct ibv_pd pd;
    int qp_shared_mem_fd;
    struct ibv_qp *remote_qp; // peer QP
    //sge_of_wr_t snd_sge_[RCV_MAX];
    //sge_of_wr_t rcv_sge_[RCV_MAX];
    struct ibv_send_wr snd_wr[RCV_MAX]; // my wrire requests 
    struct ibv_recv_wr rcv_wr[RCV_MAX];  // // Received  from Peer 
    struct ibv_recv_wr * remote_rcv_wr[RCV_MAX];
    struct ibv_send_wr * remote_snd_wr[RCV_MAX]; 
} per_qp_t;

/* we need to save MR  and WR  Dor this purpose we extend ibv_context */
typedef struct __shmem_context {
    int header__mem_fd;
    struct ibv_context context;
    struct ibv_mr mr_arr[MR_BUFFERS_MAX];  //my buffers which remote  - per connection 
    uint16_t mr_index;
    uint16_t qp_index;
    uint32_t snd_index;
    uint32_t rcv_index;
    per_qp_t qp_arr[QP_MAX];
    struct ibv_pd pd_arr[QP_MAX];
    sge_of_wr_t sg_arr[RCV_MAX];
     
} shmem_context_t;
shmem_context_t *shmem_hdr;  // Global header
struct ibv_context *ibv_open_device(struct ibv_device *device){
    int fd;
    
    fd = shm_open("ibverbs", O_RDONLY|O_CREAT|O_TRUNC, S_IRWXU);
    ftruncate (fd, sizeof(shmem_context_t));
    shmem_hdr = mmap(NULL, sizeof(shmem_context_t ), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (! shmem_hdr) {
           fprintf(stderr, "%s :Failed create shmem ibverbs header\n", __func__);
           exit(-1);
    }
    shmem_hdr->header__mem_fd = fd;
     // Create a smmem for snd/rcv Scatter-Gather list 
    for ( uint16_t j= 0; j< RCV_MAX; j++){
        char * shname = NULL;
        int rc  = asprintf( &shname,  "shmem_rcv_cqe_mem_%u", j);
        assert (rc > 0);
        fd = shm_open(shname, O_RDONLY|O_CREAT|O_TRUNC, S_IRWXU);
        free(shname);
        if (fd <= 0) {
           fprintf(stderr, "Failed shmem_rcv_cqe_mem_%u\n", j);
           exit(-1);
        }
        
        ftruncate(fd, 0);
        shmem_hdr->sg_arr[j].wr_shared_mem_fd = fd;
        shmem_hdr->sg_arr[j].rcv_signalled = false;
        shmem_hdr->sg_arr[j].total_sq_size = 0;
        shmem_hdr->sg_arr[j].sg = NULL;
    }

    //
    return &shmem_hdr->context;
}

int ibv_close_device(struct ibv_context *context){
    shmem_context_t * ctx = container_of(context, shmem_context_t,context );
    for ( uint16_t j= 0; j< RCV_MAX; j++){
        munmap(shmem_hdr->sg_arr[j].mapped_sgs, shmem_hdr->sg_arr[j].total_sq_size);
        char * shname = NULL;
        int rc  = asprintf( &shname,  "shmem_rcv_cqe_mem_%u", j);
        assert (rc > 0);
        shm_unlink(shname);
        free(shname);
        shmem_hdr->sg_arr[j].mapped_sgs = NULL;
        shmem_hdr->sg_arr[j].wr_shared_mem_fd = 0;
        shmem_hdr->sg_arr[j].rcv_signalled = false;
        shmem_hdr->sg_arr[j].total_sq_size = 0;
        shmem_hdr->sg_arr[j].total_sq_size = 0;
        shmem_hdr->sg_arr[j].sg = NULL;
    }
    free (context);
    munmap(ctx, sizeof(*ctx));
    shm_unlink("ibverbs");
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
 * No need for emulator
 */
struct ibv_comp_channel *ibv_create_comp_channel(struct ibv_context *context){
    struct ibv_comp_channel * cc = malloc(sizeof(struct ibv_comp_channel));
    cc->context = context;
    cc->fd =-1;  // async events fd?
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
		     struct ibv_cq **cq, void **cq_context)
{
    struct ibv_cq *c= malloc(sizeof(struct ibv_cq));
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

/*
 Create pair of QP  - local and remote
*/
struct ibv_qp *ibv_create_qp(struct ibv_pd *pd,
                    struct ibv_qp_init_attr *qp_init_attr){

    shmem_context_t * ctx = container_of(pd->context, shmem_context_t, context);
    struct ibv_qp * qp = NULL;
    struct ibv_qp * remote_qp = NULL;
    uint16_t i, j;
    for (  i = 0; i< QP_MAX; i++){
        if ( ctx->qp_arr[i].qp.qp_num == 0) {
            qp = &ctx->qp_arr[i].qp;
            qp->pd = pd;
            ctx->qp_index++;
            assert ( ctx->qp_index < QP_MAX);
            qp->qp_num = i;
            ctx->qp_arr[i].remote_qp = NULL;
            qp->state = IBV_QPS_RESET;
		    qp->events_completed = 0;
		    pthread_mutex_init(&qp->mutex, NULL);
		    pthread_cond_init(&qp->cond, NULL);
        }
    }
    assert (qp != NULL);
    // Creatr a pair to QP[i]
    for ( j = 0; j< QP_MAX; j++){
        if ( ctx->qp_arr[j].qp.qp_num == 0) {

            remote_qp = &ctx->qp_arr[j].qp;
            remote_qp->pd = pd;
            ctx->qp_index++;
            assert ( ctx->qp_index < QP_MAX);
            remote_qp->qp_num = j;
            ctx->qp_arr[i].remote_qp = remote_qp;
            ctx->qp_arr[j].remote_qp = qp;
            remote_qp->state = IBV_QPS_RESET;
		    remote_qp->events_completed = 0;
		    pthread_mutex_init(&remote_qp->mutex, NULL);
		    pthread_cond_init(&remote_qp->cond, NULL);
        }
    }
    return qp;
}                     

int ibv_destroy_qp(struct ibv_qp *qp) {
    shmem_context_t * ctx = container_of(qp->context, shmem_context_t, context);
    for ( uint8_t i = 0; i< QP_MAX; i++){
        if ( ctx->qp_arr[i].qp.qp_num == qp->qp_num) {
            ctx->qp_arr[i].qp.qp_num = 0;
            ctx->qp_index--;
            ctx->qp_arr[i].remote_qp->qp_num = 0;
            ctx->qp_arr[i].remote_qp = NULL;
            ctx->qp_index--;
        }
    }
    return 0;
}


int ibv_query_port(struct ibv_context *context, uint8_t port_num,
                        struct ibv_port_attr *port_attr){
    port_attr->state=IBV_PORT_ACTIVE; 
    return 0;
}




/*
* We need to create shared memory from addr and then notify peer of this addrees
ibv_reg_mr()  registers  a  memory  region  (MR)  associated with the protection
       domain pd.
*/
struct ibv_mr *ibv_reg_mr(struct ibv_pd *pd, void *addr,
                                 size_t length, int access){
  
   struct ibv_mr *mr = NULL;
   shmem_context_t * ctx = container_of(pd->context, shmem_context_t, context);
   /// Find free slot, each PD thereticlly has its own list of memory regions
   
   for ( uint8_t i = 0; i< MR_BUFFERS_MAX; i++) {
       if ( ctx->mr_arr[i].addr == NULL) {
            mr = &ctx->mr_arr[i];
            ctx->mr_index++;
            mr->lkey = i + 1; // key of local memory reqion
            mr->rkey = i + 1; // key of remote memory reqion
            mr->addr = addr;
            mr->length = length;
            mr->handle= 0;
            //mr->type = IBV_MR_TYPE_MR;
            mr->pd = pd;
            return mr;
       }
   }
   return NULL;
}

 int ibv_dereg_mr(struct ibv_mr *mr){
    shmem_context_t * ctx = container_of(mr->pd->context, shmem_context_t, context );

    for ( uint8_t i = 0; i< MR_BUFFERS_MAX; i++) {
            if ( ctx->mr_arr[i].addr == mr->addr) {
                ctx->mr_arr[i].addr = NULL;
                mr->lkey = 0; // key of local memory reqion
                mr->rkey = 0; // key of remote memory reqion
                mr->length = 0;
                ctx->mr_index--;
                return 0;
            }
    }
    return -1;
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
/*
 Remote WR are registered in the QP snd_wr element 
 Local WR are registered only in QP snd_local count */
/* Returns only 1 element  for now */
int ibv_poll_cq(struct ibv_cq *cq, int num_entries,
                       struct ibv_wc *wc){
   shmem_context_t * ctx = shmem_hdr;
   uint16_t n;

   assert(num_entries <64*1024);
   //uint16_t qp_num;
   // Scan all QP
  // struct ibv_qp * qp;
   for (uint16_t k = 0; k< QP_MAX; k++){
        
        for ( uint16_t i = 0; i< RCV_MAX; i++){
             
            // Local snd_wr are sent immediately, i.e mapped to shmem_rcv_X
            for (n =0; n < num_entries ;n++) {
                for ( uint16_t i = 0; i< RCV_MAX; i++){
                    off_t shmem_rcv_len = lseek( ctx->sg_arr[i].wr_shared_mem_fd, 0 , SEEK_CUR);
                    if ( shmem_rcv_len > 0) {
                        if (!ctx->sg_arr[i].rcv_signalled) {
                            fprintf(stderr, "Shmem SQ %i not consumed\n", i);
                            continue;
                        }
                        wc[n].qp_num = ctx->qp_arr[k].qp.qp_num;
                        wc[n].src_qp = wc->qp_num;
                        wc[n].opcode = IBV_WC_RECV;
                        wc[n].status = IBV_WC_SUCCESS;
                        wc[n].wr_id = ctx->qp_arr[k].snd_wr[i].wr_id;
                    }
                }
            }
            for (; n < num_entries ;n++) {
                // So rcv side has access to datam but how do i know when to release it?
                // I will release after signalled=true
                off_t shmem_rcv_len = lseek( ctx->sg_arr[i].wr_shared_mem_fd, 0 , SEEK_CUR);
                if ( shmem_rcv_len > 0) {
                    wc[n].qp_num = ctx->qp_arr[k].qp.qp_num;
                    wc[n].vendor_err = 0;
                    wc[n].src_qp = wc->qp_num;
                    wc[n].opcode = IBV_WC_SEND;
                    wc[n].status = IBV_WC_SUCCESS;
                    wc[n].wr_id = ctx->qp_arr[wc->qp_num].snd_wr[i].wr_id;
                    wc[n].imm_data = ctx->qp_arr[wc->qp_num].snd_wr[i].imm_data;
                    
                    ctx->sg_arr[i].rcv_signalled = true;
                    // ? wc->sl = 1;
                    // ? wc->slid = 1;
                    // ? wc->pkey_index = 1;
               // ctx->snd_wr[i] = NULL; 
                return 1;
            }
            
        }
   }
   return -1;
}

/* So we mapped SG element to SHMEM 
 Q : should it be per QP ?
*/
int ibv_post_recv(struct ibv_qp *qp, struct ibv_recv_wr *wr,
				struct ibv_recv_wr **bad_wr)
{
    // jusr preparing to send
    return 0;
}
/* I am willimg to send, place WR into remote peer rcv buf */ 
int ibv_post_send(struct ibv_qp *qp, struct ibv_send_wr *wr,
				struct ibv_send_wr **bad_wr) 
{
    shmem_context_t * ctx = shmem_hdr;
    void *ptr;
    // Find free snd slot to place write request
    // free slot is when completed Reception  from Peer is signalled
    for (uint16_t j=0; j < RCV_MAX; j++) {
        // ctx->rcv_wr, snd_wr must be in shared mem;
        if (!ctx->sg_arr[j].rcv_signalled) {
            fprintf(stderr, "POST_RECV Shmem SQ %i not consumed\n", j);
            continue;
        }
        // Store WR that contain sge wr_id and other fields;
        memcpy(&ctx->qp_arr[qp->qp_num].snd_wr[j] , wr, sizeof(*wr));

        // Now  mmap SQ list addresses into shared mem
        
        // signalled == true, we can reuse 
        ftruncate(ctx->sg_arr[j].wr_shared_mem_fd, 0);
        ptr = mmap((uint64_t *)wr->sg_list->addr, wr->sg_list->length, PROT_READ|PROT_WRITE, MAP_SHARED,
                ctx->sg_arr[j].wr_shared_mem_fd,
                ctx->sg_arr[j].total_sq_size);
        // @todo make a list remmapped to shared mem
        if (ptr == NULL) {
             fprintf(stderr, "%s Failed to mmap SG \n", __func__ );
            exit(-1);
        }
        
        ctx->sg_arr[j].mapped_sgs = ptr;
        ctx->sg_arr[j].total_sq_elems++;
        ctx->sg_arr[j].total_sq_size += wr->sg_list->length;
        ftruncate(ctx->sg_arr[j].wr_shared_mem_fd,
                    ctx->sg_arr[j].total_sq_size);
        return 0;
    }
    fprintf(stderr, "%s CAN NOT place WR into Peers rcv_wr, end of RCV_MAX=%d\n", 
                     __func__, RCV_MAX);
    return -1;
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

int ibv_modify_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr, int attr_mask)
{
    fprintf(stderr, "%s not implemented\n", __func__);
    return 0;
}
/* I am willimg to receivev, place WR into remote peer send buf */
/* Put WR into Remote's rcv buf
* Emulator - @TODO ignore bad_wr for now
*/

/*
#include <rdma/rdma_verbs.h>

       int  rdma_post_read  (struct  rdma_cm_id  *id,  void *context, void *addr, size_t length,
       struct ibv_mr *mr, int flags, uint64_t remote_addr, uint32_t rkey);
*/
