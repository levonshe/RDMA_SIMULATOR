#define _GNU_SOURCE
#include <stdlib.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/file.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <malloc.h>
#include <linux/membarrier.h>

#include <sys/stat.h>        /* For mode constants */
#include <fcntl.h>           /* For O_* constants */
#include <stdbool.h>
#include <assert.h>

#include <infiniband/verbs.h>
#define container_of(ptr, type, member) ({\
        const typeof( ((type *)0)->member ) *__mptr = (ptr);\
        (type *)( (char *)__mptr - offsetof(type,member) );})

#define PAGE_SIZE 4096
#ifdef DEBUG
    #define RCV_MAX 20
    #define QP_MAX 2
    #define CQ_MAX 3
    #define MR_BUFFERS_MAX 5
    #define PD_MAX 2
#else
#define RCV_MAX 250
#define QP_MAX 16
#define PD_MAX 8
#define CQ_MAX 16
#define MR_BUFFERS_MAX 16
#endif
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
    struct ibv_send_wr snd_wr; // my wrire requests
    enum   ibv_wr_opcode wr_opcode;
    struct ibv_qp src_qp;
    struct ibv_sge *sg;
    int    wr_shared_mem_fd;
    size_t total_sq_size;
    size_t total_sq_elems;
    pid_t  cq_context;
    bool   sent_posted; // Signalled in poll_cq that receie is completed,
} sent_wr_t;


typedef struct _per_qp {
    struct ibv_qp qp;
    struct ibv_pd pd;

} per_qp_t;
typedef struct _per_mr {
    struct ibv_mr mr;
    int pid_context;
} mr_record_t;
/* we need to save MR  and WR  Dor this purpose we extend ibv_context */
typedef struct __shmem_context {
    int shared__mem_fd;
    struct ibv_context context;
    mr_record_t mr_arr[MR_BUFFERS_MAX];  //my buffers which remote  - per connection
    uint16_t mr_count;
    per_qp_t qp_arr[QP_MAX];
    struct ibv_pd pd_arr[PD_MAX];
    sent_wr_t sent_arr[RCV_MAX];
    uint32_t owner_pid;
} shmem_context_t;
typedef struct _local_rcv {
    struct ibv_recv_wr * local_rcv_wr[RCV_MAX];
    bool local_rcv_posted[RCV_MAX];
    struct ibv_qp *rcv_qp[RCV_MAX];
    // @todo atomic count
    int local_rcv_count;
} local_rcv_t;
shmem_context_t *shmem_hdr=NULL;  // Global header in shared mem
local_rcv_t local_rcv;
fd_set readfds,writefds, exceptfds;
struct timeval timeout;
struct ibv_context *ibv_open_device(struct ibv_device *device){
    int fd;
    timeout.tv_sec=5;
    timeout.tv_usec=0;
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    FD_ZERO(&exceptfds);
    
    memset(&local_rcv, 0, sizeof(local_rcv));
    if ( access("/dev/shm/ibverbs", F_OK) != 0) {
        fd = shm_open("ibverbs", O_RDWR|O_CREAT, S_IRUSR|S_IWUSR|S_IWGRP|S_IRGRP);
    }
    else {
        fd = shm_open("ibverbs", O_RDWR, S_IRUSR|S_IWUSR|S_IWGRP|S_IRGRP);
    }    
    int rc = flock(fd, LOCK_EX|LOCK_NB); // leave this lock active
    if (rc == 0) {
        rc = ftruncate (fd, sizeof(shmem_context_t));
        if ( rc < 0 ){
            perror ("Failed ftruncate shmem ibverbs header\n");
            exit(-1);
        }
        shmem_hdr = mmap(NULL, sizeof(shmem_context_t), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
        if (shmem_hdr == MAP_FAILED) {
            perror("Failed mmap shmem ibverbs header\n");
            exit(-1);
        }
        memset(shmem_hdr, 0, sizeof(*shmem_hdr));
        shmem_hdr->owner_pid = getpid();
    } else {
        
        shmem_hdr = mmap(NULL, sizeof(shmem_context_t), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
        if (shmem_hdr == MAP_FAILED) {
           perror("Failed create shmem ibverbs header\n");
           exit(-1);
         }
    }
    
    shmem_hdr->shared__mem_fd = fd;
    
    
     // Create a smmem for snd/rcv Scatter-Gather list
    for ( uint16_t j= 0; j< RCV_MAX; j++){
        char * shname = NULL;
        int rc  = asprintf( &shname,  "shmem_rcv_cqe_mem_%u", j);
        assert (rc > 0);
        if (access(shname, R_OK) == 0 ) {
            fd = shm_open(shname, O_RDWR, S_IRUSR|S_IWUSR|S_IWGRP|S_IRGRP);
        }
        else {
             fd = shm_open(shname, O_CREAT|O_RDWR, S_IRUSR|S_IWUSR|S_IWGRP|S_IRGRP);
        }
        free(shname);
        if (fd <= 0) {
           fprintf(stderr, "Failed shmem_rcv_cqe_mem_%u\n", j);
           exit(-1);
        }
        shmem_hdr->sent_arr[j].wr_shared_mem_fd = fd;
        FD_SET(fd, &readfds);
        FD_SET(fd, &writefds);
        FD_SET(fd, &exceptfds);
        shmem_hdr->sent_arr[j].total_sq_size = 0;
        shmem_hdr->sent_arr[j].sg = NULL;
        //shmem_hdr->sent_arr[j]
    }
    
    syscall(SYS_membarrier, MEMBARRIER_CMD_SHARED, 0);
    //
    return &shmem_hdr->context;
}

int ibv_close_device(struct ibv_context *context){
    shmem_context_t * ctx = container_of(context, shmem_context_t,context );
    return 0; // do not close till Per is alive, 

    for ( uint16_t j=RCV_MAX; j >= 0; j--){
        if (shmem_hdr->sent_arr[j].sent_posted) {
            return 0; // Peer did not took it, do not release shmem
        }
        shmem_hdr->sent_arr[j].wr_shared_mem_fd = -1;
        shmem_hdr->sent_arr[j].sent_posted = false;
        shmem_hdr->sent_arr[j].sg = NULL;

        if (shmem_hdr->sent_arr[j].total_sq_size ) {
            char * shname = NULL;
            int rc  = asprintf( &shname,  "shmem_rcv_cqe_mem_%u", j);
            assert (rc > 0);
            shm_unlink(shname);
            free(shname);
        }
        
        shmem_hdr->sent_arr[j].total_sq_size = 0;
        
    }
    munmap(ctx, sizeof(shmem_context_t));
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
    c->cq_context = (void *) (uintptr_t )getpid();
    fprintf(stderr, " Created CQ contexts ans Channel%d\n",getpid());
    c->comp_events_completed = 0;
    *cq=c;
    *cq_context = (void *) (uintptr_t )getpid();
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

    uint16_t i;
    syscall(SYS_membarrier, MEMBARRIER_CMD_SHARED, 0);
    for (  i = 0; i< QP_MAX; i++){
        if ( ctx->qp_arr[i].qp.qp_num == 0) {
            qp = &ctx->qp_arr[i].qp;
            qp->context = pd->context;
            qp->qp_context = pd->context; 
            // ?? qp->srq
            qp->pd = pd;
            qp->qp_num = getpid();
            fprintf(stderr, " Created QP %d\n",qp->qp_num);
            qp->state = IBV_QPS_RTS;
		    qp->events_completed = 0;
		    pthread_mutex_init(&qp->mutex, NULL);
		    pthread_cond_init(&qp->cond, NULL);
            return qp;
        }
    }
    assert (qp != NULL);
    return NULL;
}

int ibv_destroy_qp(struct ibv_qp *qp) {
    shmem_context_t * ctx = container_of(qp->context, shmem_context_t, context);
    for ( uint8_t i = 0; i< QP_MAX; i++){
        if ( ctx->qp_arr[i].qp.qp_num == qp->qp_num) 
            ctx->qp_arr[i].qp.qp_num = 0;
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
   uint16_t i;
   for (  i = 0; i< MR_BUFFERS_MAX; i++) {
       if ( ctx->mr_arr[i].pid_context == 0) {
            // Not allocate MR yet
            ctx->mr_arr[i].pid_context = getpid();
            mr = &ctx->mr_arr[i].mr;
            mr->lkey = i + 1; // key of local memory reqion
            mr->rkey = i + 1; // key of remote memory reqion
            mr->addr = addr;
            assert(mr->addr);
            mr->length = length;
            assert(mr->length);
            mr->handle= getpid();
            //mr->type = IBV_MR_TYPE_MR;
            mr->pd = pd;
            fsync(ctx->shared__mem_fd);
            syscall(SYS_membarrier, MEMBARRIER_CMD_SHARED, 0);
            return mr;
       }
   }
   return NULL;
}

 int  __attribute__((unused))ibv_dereg_mr(struct ibv_mr *mr){
    shmem_context_t * ctx = container_of(mr->pd->context, shmem_context_t, context );

    for ( uint8_t i = 0; i< MR_BUFFERS_MAX; i++) {
            if ( ctx->mr_arr[i].mr.addr == mr->addr) {
                ctx->mr_arr[i].pid_context = 0;
                mr->lkey = 0; // key of local memory reqion
                mr->rkey = 0; // key of remote memory reqion
                mr->length = 0;
                mr->handle = 0;
                ctx->mr_count--;
                return 0;
            }
    }
    return -1;
 }

 int  __attribute__((unused))ibv_query_device(struct ibv_context *context,
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

int  __attribute__((unused)) ibv_query_pkey(struct ibv_context *context, uint8_t port_num,
                          int index, uint16_t *pkey)
{
    *pkey=1;
    return 0;
}
int get_gid(struct ibv_context *context, uint8_t port_num,
                         int index, union ibv_gid *gid)
{
    return 999;
}
int  signal_local_rcv( int num_entries,
                       struct ibv_wc *wc)
{
    int found = 0;
   
    for ( uint16_t i = 0; i< RCV_MAX && (found < num_entries); i++){
        // @todo mutex
        if ( local_rcv.local_rcv_posted[i] ) {
            found++;
            wc[i].qp_num = local_rcv.rcv_qp[i]->qp_num;
            wc[i].src_qp = getpid();
            wc[i].opcode = IBV_WC_RECV;
            wc[i].status = IBV_WC_SUCCESS;
            wc[i].wr_id = local_rcv.local_rcv_wr[i]->wr_id;
            wc[i].slid = 9998;
            wc[i].sl = 13;
            //wc[i].imm_data = 0;
            //wc[i].imm_data = local_rcv.local_rcv_wr[i]->imm_data;
            wc->vendor_err = 0;
            local_rcv.local_rcv_posted[i] = false; // release posted slot i 
        }
    }
    return found;
}
static bool printonce=false;
/*
 Remote WR are registered in the QP snd_wr element
 Local WR are registered only in QP snd_local count */
/* Returns only 1 element  for now */
int  __attribute__((unused)) ibv_poll_cq(struct ibv_cq *cq, int num_entries,
                       struct ibv_wc *wc)
{
    shmem_context_t * ctx = shmem_hdr;
    uint16_t n = 0;
    bool found_peer_posted = false;
    

    assert(num_entries <64*1024);

    // First, complete local rcv
    n = signal_local_rcv(num_entries, wc);
    if ( ! printonce) {
        int sel = select(20, NULL ,&writefds, &exceptfds, &timeout);
        for (uint8_t l=0; l<RCV_MAX;l++ ){
            fprintf(stderr, "Sel res=%d\n", sel);
           // if (FD_ISSET( ctx->sent_arr[i].wr_shared_mem_fd, &readfds))
            //fprintf(stderr, "ReadFD on fd=%u\n", ctx->sent_arr[i].wr_shared_mem_fd);
            if (FD_ISSET( ctx->sent_arr[l].wr_shared_mem_fd, &writefds))
            fprintf(stderr, "WriteFD on fd=%u\n", ctx->sent_arr[l].wr_shared_mem_fd);
            if (FD_ISSET( ctx->sent_arr[l].wr_shared_mem_fd, &exceptfds))
            fprintf(stderr, "ExceptFD on fd=%u\n", ctx->sent_arr[l].wr_shared_mem_fd);
            printonce = true;
        }
    }
    for ( uint16_t i = 0; i< RCV_MAX && (n <= num_entries); i++){
        syscall(SYS_membarrier, MEMBARRIER_CMD_SHARED, 0);
        // So rcv side has access to data but how do i know when to release it?
        // I will release after signalled=true
        // we can release only Peer wr, different context
        if  (ctx->sent_arr[i].cq_context == getpid()) {
            continue;
        }
        
        if ( ctx->sent_arr[i].sent_posted ) {
            found_peer_posted= true;
            wc[n].qp_num = ctx->sent_arr[i].src_qp.qp_num;
            wc[n].vendor_err = 0;
            wc[n].src_qp = wc->qp_num;
            wc[n].opcode = IBV_WC_SEND;
            wc[n].status = IBV_WC_SUCCESS;
            wc[n].wr_id = ctx->sent_arr[i].snd_wr.wr_id;
            n++;
            // copy ctx->sent_arr[i].snd_wr to MR buffer
            // @todo need to make SG ? 
            // Assume at least MR is registered
            // Find local MR of this PEER
            uint16_t m;
            bool mr_found = false;
            for (m = 0;  m < MR_BUFFERS_MAX; m++){
                if (ctx->mr_arr[m].pid_context > 0 &&
                    (ctx->mr_arr[m].pid_context != getpid())) {
                    mr_found= true;
                    break;
                }
            }
            wc[n].imm_data = ctx->sent_arr[i].snd_wr.imm_data;
            if ( (! mr_found) || (ctx->sent_arr[i].total_sq_size == 0)) {
                continue;
            }
            
            assert(m < MR_BUFFERS_MAX);
            assert(ctx->mr_arr[m].mr.addr);
            assert(ctx->sent_arr[i].total_sq_size);
            
            
            if (ctx->sent_arr[i].total_sq_size) {
                read(ctx->sent_arr[i].wr_shared_mem_fd, ctx->mr_arr[m].mr.addr, ctx->sent_arr[i].total_sq_size);   
                assert(read >0);
                char * msg= (char *)ctx->mr_arr[m].mr.addr;
                bool foreing = (ctx->mr_arr[m].pid_context != getpid())? true:false;
                if (foreing)
                    fprintf(stderr, " Completer Shmem Slot %i Mr[m=%d] Foreign msg =%s\n", i, m,msg);
            }
            
            syscall(SYS_membarrier, MEMBARRIER_CMD_SHARED, 0);
            ctx->sent_arr[i].sent_posted = false; // release slot
            syscall(SYS_membarrier, MEMBARRIER_CMD_SHARED, 0);
            fprintf(stderr, "PID [%d]/CQ Context %d Completer Shmem Slot %i of SRC_QP [%d] released\n", 
                        getpid(), (int)(uintptr_t) ctx->sent_arr[i].cq_context, i, wc->qp_num);
             
        }
    }
    if (found_peer_posted ) {
        assert (n >0);
        n--;
    }
   
   return n;
}

/* So we mapped SG element to SHMEM
 Q : should it be per QP ?
*/
 int ibv_post_recv(struct ibv_qp *qp, struct ibv_recv_wr *wr,
				struct ibv_recv_wr **bad_wr)
{
    // just preparing to send
    for ( uint16_t i =0; i< RCV_MAX; i++){
        if (local_rcv.local_rcv_posted[i] == false) {
            local_rcv.local_rcv_posted[i] = true;
            local_rcv.local_rcv_wr[i] = wr;
            local_rcv.rcv_qp[i] = qp;
            local_rcv.local_rcv_count++;
            return 0;
        }
    }
    return -1;
}
/*
 I am willing to send, place WR into remote peer rcv buf 
 */
int ibv_post_send(struct ibv_qp *qp, struct ibv_send_wr *wr,
				struct ibv_send_wr **bad_wr)
{
    shmem_context_t * ctx = shmem_hdr;
    bool found_free_slot = false;

    // Find free snd slot to place write request
    // free slot is when completed Reception  from Peer is signalled
    for (uint16_t j=0; j < RCV_MAX; j++)
    {
        syscall(SYS_membarrier, MEMBARRIER_CMD_SHARED, 0);
        // Find free slot 
        if ( ctx->sent_arr[j].sent_posted) {
           // fprintf(stderr, "POST_SENT Shmem SQ %i not consumed\n", j);
            continue;
        }
       
        found_free_slot = true;
        // Place WR in shared mem;
        memcpy(&ctx->sent_arr[j].src_qp, qp, sizeof(*qp));
        memcpy(&ctx->sent_arr[j].snd_wr, wr, sizeof(*wr));
        ctx->sent_arr[j].cq_context = getpid();
      
        ctx->sent_arr[j].total_sq_elems = 0;
        ctx->sent_arr[j].total_sq_size = 0;
        ctx->sent_arr[j].sent_posted = true;
        ctx->sent_arr[j].wr_opcode = IBV_WC_RECV;
        if (wr->sg_list->length == 0 ) {
            ctx->sent_arr[j].wr_opcode = IBV_WC_RECV; //??
            return 0;
        }
       
        assert( ctx->sent_arr[j].snd_wr.sg_list->addr > 0); 
        if (!ctx->sent_arr[j].snd_wr.sg_list->addr) {
           return -1;
        }
        off_t sg_linear_offset = 0;
      
        int fd = ctx->sent_arr[j].wr_shared_mem_fd;
         
        // @todo - * wr->num_sge
        int rc = ftruncate(fd, wr->sg_list->length * wr->num_sge);
        assert(rc == 0);
        
        
        // @todo make a all element of list be copied to shared mem
        
        // @todo sg_linear_offset + = next sg.
       
       
        rc = write (ctx->sent_arr[j].wr_shared_mem_fd, (void *)wr->sg_list->addr, wr->sg_list->length );
        assert(rc >= 0);
        sg_linear_offset += wr->sg_list->length;
        
        ctx->sent_arr[j].total_sq_elems++;
        ctx->sent_arr[j].total_sq_size += wr->sg_list->length;
        
        ctx->sent_arr[j].sent_posted = true;
        syscall(SYS_membarrier, MEMBARRIER_CMD_SHARED, 0);
       
        fprintf(stderr, "PID[%d] POST Taking slot %u for QP=%d\n", getpid(), j,qp->qp_num );
        
        return 0;
       
    }
   
    if ( !found_free_slot) {
         fprintf(stderr, "POST_SENT Shmem SQ not consumed, no free slots\n");
         exit(-4);
    }
    fprintf(stderr, "SEND CAN NOT place WR into Peers rcv_wr, end of RCV_MAX\n");
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
 __attribute__((unused)) struct ibv_cq *  ibv_create_cq(struct ibv_context *context, int cqe,
                                    void *cq_context,
                                    struct ibv_comp_channel *channel,
                                    int comp_vector)
{

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

int  __attribute__((unused)) ibv_destroy_cq(struct ibv_cq *cq)
{
   // completion_queue_t *tmp;
    // *todo tmp=container_of
    free(cq);
    //@todo free tmp
    return 0;
}

int  __attribute__((unused))
ibv_modify_qp(struct ibv_qp *qp,
                         struct ibv_qp_attr *attr,
                          int attr_mask)
{
    qp->state = IBV_QPS_RTS;
    fprintf(stderr, "%s QP qp_num=%i Ready IBV_QPS_RTS\n", __func__, qp->qp_num);
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
