#define _GNU_SOURCE
#include <stdlib.h>
#include <stdbool.h>

#include <infiniband/verbs.h>

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

struct ibv_context *ibv_open_device(struct ibv_device *device){

    struct ibv_context * cntx = malloc(sizeof(struct ibv_context));
    // attach to shmem  q
    return cntx;
}

int ibv_close_device(struct ibv_context *context){
    free (context);
    return 0;
}

struct ibv_pd *ibv_alloc_pd(struct ibv_context *context) {
    struct ibv_pd * pd = malloc(sizeof(struct ibv_pd));
    return pd;
}

int ibv_dealloc_pd(struct ibv_pd *pd){
    free(pd);
    return 0;
}

struct ibv_qp *ibv_create_qp(struct ibv_pd *pd,
                    struct ibv_qp_init_attr *qp_init_attr){
     struct ibv_qp * qp = malloc(sizeof(struct ibv_qp));
     return qp;
}                     

int ibv_destroy_qp(struct ibv_qp *qp) {
    free(qp);
    return 0;
}


int ibv_query_port(struct ibv_context *context, uint8_t port_num,
                        struct ibv_port_attr *port_attr){
    port_attr->state=IBV_PORT_ACTIVE; 
    return 0;
}

struct ibv_mr *ibv_reg_mr(struct ibv_pd *pd, void *addr,
                                 size_t length, int access){
   struct ibv_mr *mr= malloc( sizeof (*mr));
   return mr; 
}

 int ibv_dereg_mr(struct ibv_mr *mr){
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
    tmp->cq = calloc(cqe, sizeof(*tmp->cq));
    tmp->cq_context = cq_context;
    tmp->context=context;
    tmp->cqe = cqe;
    return tmp->cq;
}
                                    
int ibv_destroy_cq(struct ibv_cq *cq){
   // completion_queue_t *tmp;
    // *todo tmp=container_of
    free(cq);
    //@todo free tmp
    return 0;
}


int ibv_modify_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr,
                         int attr_mask){
    return 0;
}