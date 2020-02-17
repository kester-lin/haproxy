#include <error.h>
#include <stdio.h>
#include <string.h>    //strlen
#include <stdlib.h>    //strlen
#include <sys/socket.h>
#include <arpa/inet.h> //inet_addr
#include <unistd.h>    //write
#include <pthread.h> //for threading , link with lpthread
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <linux/netlink.h>

#include "shmem.h"

#define DEBUG_SHM 1

unsigned int total_idx = 0;

#define DEBUG_MSG 1
#if DEBUG_MSG
#define MSG_PRINTF(x...) printf(x)
#else
#define MSG_PRINTF(x...)
#endif

#define MAX_PAYLOAD 384

void *connection_handler(void *socket_desc);

int main(int argc, char *argv[])
{
    int shm_id = 0;
    struct proto_ipc *ipt_target = NULL; 
    //socket的建立
    char inputBuffer[256] = {};
    int sockfd = 0; 
    int forClientSockfd = 0;       
    struct sockaddr_in serverInfo;
    struct sockaddr_in clientInfo;
    int addrlen = sizeof(clientInfo);
    pthread_t thread_id;
    struct thread_data thread_data;

    struct sockaddr_nl src_addr;

    int netlink_sock_fd = 0;    

    printf("Start\n\n");

    /* get the ID of shared memory */
    shm_id = shmget((key_t)KEY_SHM_CUJU_IPC, SUPPORT_VM_CNT*(sizeof(struct proto_ipc)), 0666|IPC_CREAT);
    
    if (shm_id == -1) {
        printf("shmget error\n");
        exit(EXIT_FAILURE);
    }
    
    /* attach shared memory */
    ipt_target = (struct proto_ipc *)shmat(shm_id, (void *)0, 0);
    if (ipt_target == (void *)-1) {
        perror("shmget error");
        exit(EXIT_FAILURE);
    }

    printf("Shared Memory Ok\n");

    memset(ipt_target, 0x00, SUPPORT_VM_CNT * (sizeof(struct proto_ipc)));

    netlink_sock_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_NETFILTER);
    if (netlink_sock_fd <= 0) {
        perror("create socket failed!\n");
        return -1;
    }

    printf("Netlink Socket ID:%d\n", netlink_sock_fd);
   
    memset(&src_addr, 0, sizeof(struct sockaddr_nl));
    src_addr.nl_family = AF_NETLINK;
    src_addr.nl_pid = getpid();
    src_addr.nl_groups = 0;
 
    if (bind(netlink_sock_fd, (struct sockaddr *)&src_addr, sizeof(struct sockaddr)) < 0) {
        perror("bind socket failed!\n");
        close (netlink_sock_fd);
        return -1;
    }


#if 0//DEBUG_SHM
    while(1) {
        ipt_target->epoch_id++;
        usleep(5000);
    } 


#else 
    sockfd = socket(AF_INET , SOCK_STREAM , 0);

    if (sockfd == -1){
        perror("Fail to create a socket.\n");
    }

    //socket的連線
    bzero(&serverInfo, sizeof(serverInfo));

    serverInfo.sin_family = PF_INET;
    serverInfo.sin_addr.s_addr = INADDR_ANY;
    serverInfo.sin_port = htons(1200);

    bind(sockfd, (struct sockaddr *)&serverInfo, sizeof(serverInfo));
    
    listen(sockfd, 5);
	
    MSG_PRINTF("Wait accepted\n");

    while( (forClientSockfd = accept(sockfd, (struct sockaddr *)&clientInfo, (socklen_t*)&addrlen)) > 0 )
    {
        printf("Connection accepted:%d\n", forClientSockfd);

        thread_data.th_idx = total_idx++;
        thread_data.th_sock = forClientSockfd;
        thread_data.ipt_base = ipt_target;
        thread_data.netlink_sock = netlink_sock_fd;

        if( pthread_create( &thread_id, NULL, connection_handler, (void*) &thread_data) < 0)
        {
            perror("could not create thread");
            return 1;
        }
         
        //Now join the thread , so that we dont terminate before the thread
        //pthread_join( thread_id , NULL);
        MSG_PRINTF("Handler assigned\n");
    }
    printf("Close\n");
#endif

    /* detach shared memory */
    if (shmdt(ipt_target) == -1) {
        perror("shmdt");
        exit(EXIT_FAILURE);
    }
    
    /* destroy shared memory */
    if (shmctl(shm_id, IPC_RMID, 0) == -1) {
        perror("shmctl");
        exit(EXIT_FAILURE);
    }
    
    exit(EXIT_SUCCESS);
}

/*
 * This will handle connection for each client
 * */
void *connection_handler(void *socket_desc)
{
    //Get the socket descriptor
    int sock = ((struct thread_data*)socket_desc)->th_sock;
    int shm_idx = ((struct thread_data*)socket_desc)->th_idx;
    struct proto_ipc * ipt_addr = ((struct thread_data*)socket_desc)->ipt_base + shm_idx * sizeof(struct proto_ipc);
    int sock_fd = ((struct thread_data*)socket_desc)->netlink_sock;

    int read_size = 0;
    unsigned char client_message[sizeof(struct proto_ipc)];
    struct proto_ipc* ipc_ptr = NULL;
    u_int32_t guest_ip_db = 0; 

    struct sockaddr_nl dest_addr;
    struct nlmsghdr *nlh = NULL;
    struct iovec iov;
    struct msghdr msg;
    struct netlink_ipc nl_ipc;

    memset(&dest_addr, 0, sizeof(struct sockaddr_nl));
    dest_addr.nl_family = AF_NETLINK;
    dest_addr.nl_pid = 0;
    dest_addr.nl_groups = 0;
    
    nlh = (struct nlmsghdr *)malloc(NLMSG_SPACE(MAX_PAYLOAD));
    if (nlh == NULL) {
        perror("malloc nlmsghdr failed!\n");
        close(sock_fd);
        return 0;
    } 
    memset(nlh, 0, NLMSG_SPACE(MAX_PAYLOAD));
    nlh->nlmsg_len = NLMSG_SPACE(MAX_PAYLOAD);
    nlh->nlmsg_pid = getpid();
    nlh->nlmsg_flags = 0;
 
    iov.iov_base = (void *)nlh;
    iov.iov_len = NLMSG_SPACE(MAX_PAYLOAD);

    memset(&msg, 0, sizeof(struct msghdr));
    msg.msg_name = (void *)&dest_addr;
    msg.msg_namelen = sizeof(struct sockaddr_nl);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    //MSG_PRINTF("[%s]\n", __func__); 

    while(read_size = recv(sock, client_message, 2000, 0))
    {
        //end of string marker
		//client_message[read_size] = '\0';
#if 1
        printf("Size:%d\n", read_size); 
#else 
        printf("Size:%d\n", read_size); 
      
        for (int idx = 0; idx < read_size; idx++) {
            printf("%02x ", *(client_message + idx));

            if (idx % 8 == 7)
                printf("\n");    
        }
        printf("\n"); 
#endif
        ipc_ptr = (struct proto_ipc*)client_message;

        ipt_addr->epoch_id = ipc_ptr->epoch_id;
        ipt_addr->flush_id = ipc_ptr->flush_id;
        ipt_addr->nic_count = ipc_ptr->nic_count;
        ipt_addr->nic[0] = ipc_ptr->nic[0];
        ipt_addr->cuju_ft_mode = ipc_ptr->cuju_ft_mode;
        
        MSG_PRINTF("Epoch ID:%d\n", ipc_ptr->epoch_id);
        MSG_PRINTF("Flush ID:%d\n", ipc_ptr->flush_id);
        MSG_PRINTF("NICCount:%d\n", ipc_ptr->nic_count);

        if (ipc_ptr->epoch_id != ipc_ptr->flush_id) {
            MSG_PRINTF("Match\n");
        }

        nl_ipc.epoch_id = ipc_ptr->epoch_id;
        nl_ipc.flush_id = ipc_ptr->flush_id;
        nl_ipc.cuju_ft_mode = ipc_ptr->cuju_ft_mode;
        nl_ipc.nic_count = ipc_ptr->nic_count;;

        memcpy(NLMSG_DATA(nlh), &nl_ipc, sizeof(nl_ipc));

        //strcpy(NLMSG_DATA(nlh), (void *)&nl_ipc);
    
        if (sendmsg(sock_fd, &msg, 0) < 0) {
            perror("send msg failed!\n");
            free(nlh);
            close(sock_fd);
            return 0;
        }        

        //MSG_PRINTF("NIC Cnt:%d\n", ipc_ptr->nic_count);

        //MSG_PRINTF("CONN Cnt:%d\n", ipc_ptr->conn_count);
        //MSG_PRINTF("IP:%08x\n", *(u_int32_t*)ipc_ptr->nic[0]);
#if 0 
		if (ipc_ptr->nic_count) {
			for(int idx = 0; idx < ipc_ptr->nic_count; idx++) {
				guest_ip_db = ntohl(*((u_int32_t*)(client_message + sizeof(struct proto_ipc)) + idx));

                ((u_int32_t*)ipt_addr + sizeof(struct proto_ipc) + idx) 
				MSG_PRINTF("Find IP String %08x\n", guest_ip_db);
				
    		}
		}
#endif
        
		//clear the message buffer
		memset(client_message, 0, sizeof(struct proto_ipc));
        read_size = 0;
    }
     
    if(read_size == 0)
    {
        MSG_PRINTF("Client disconnected\n");
        fflush(stdout);
    }
    else if(read_size == -1)
    {
        perror("recv failed");
    }
         
    return 0;
} 
