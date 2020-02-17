#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/netlink.h>
 
#define MAX_PAYLOAD 256
 
int main(int argc, char * argv[])
{
    struct sockaddr_nl src_addr;
    struct sockaddr_nl dest_addr;
    struct nlmsghdr *nlh = NULL;
    struct iovec iov;
    struct msghdr msg;

    int sock_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_NETFILTER);
    if (sock_fd < 0) {
        perror("create socket failed!\n");
        return -1;
    }
   
    memset(&src_addr, 0, sizeof(struct sockaddr_nl));
    src_addr.nl_family = AF_NETLINK;
    src_addr.nl_pid = getpid();
    src_addr.nl_groups = 0;
 
    if (bind(sock_fd, (struct sockaddr *)&src_addr, sizeof(struct sockaddr)) < 0) {
        perror("bind socket failed!\n");
        close (sock_fd);
        return -1;
    }

    memset(&dest_addr, 0, sizeof(struct sockaddr_nl));
    dest_addr.nl_family = AF_NETLINK;
    dest_addr.nl_pid = 0;
    dest_addr.nl_groups = 0;
    
    nlh = (struct nlmsghdr *)malloc(NLMSG_SPACE(MAX_PAYLOAD));
    if (nlh == NULL) {
        perror("malloc nlmsghdr failed!\n");
        close(sock_fd);
        return -1;
    } 
    memset(nlh, 0, NLMSG_SPACE(MAX_PAYLOAD));
    nlh->nlmsg_len = NLMSG_SPACE(MAX_PAYLOAD);
    nlh->nlmsg_pid = getpid();
    nlh->nlmsg_flags = 0;
 
    strcpy(NLMSG_DATA(nlh), "Hello kernel!");

    iov.iov_base = (void *)nlh;
    iov.iov_len = NLMSG_SPACE(MAX_PAYLOAD);

    memset(&msg, 0, sizeof(struct msghdr));
    msg.msg_name = (void *)&dest_addr;
    msg.msg_namelen = sizeof(struct sockaddr_nl);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
 
    if (sendmsg(sock_fd, &msg, 0) < 0) {
        perror("send msg failed!\n");
        free(nlh);
        close(sock_fd);
        return -1;
    }
 
    printf("send msg: %s\n", (char *)NLMSG_DATA(nlh));
 
    memset(nlh, 0, NLMSG_SPACE(MAX_PAYLOAD));
    
    if (recvmsg(sock_fd, &msg, 0) < 0) {
        perror("recv msg failed!\n");
        free(nlh);
        close(sock_fd);
        return -1;
    }
        
    printf("receive msg: %s\n", (char *)NLMSG_DATA(nlh));
 
 
    free(nlh);
    close(sock_fd);
 
    return 0;
}

