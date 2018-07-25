#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <string.h>
#include "sock.h"

int sock_daemon_connect(int port)
{
	struct addrinfo	*res,*t;
	struct addrinfo hints={
		.ai_flags=AI_PASSIVE,
		.ai_family=AF_UNSPEC,
		.ai_socktype=SOCK_STREAM
	};
	
	char* service;
	int n;
	int sockfd=-1,connfd;

	if(asprintf(&service,"%d",port)<0)
	{
		fprintf(stderr,"asprintf failed\n");
		return -1;
	}
	
	n=getaddrinfo(NULL,service,&hints,&res);
	if(n)
	{
		fprintf(stderr,"%s for port %d\n",gai_strerror(n),port);
		free(service);
		return -1;
	}
	
	for(t=res;t;t=t->ai_next)
	{
		sockfd=socket(t->ai_family,t->ai_socktype,t->ai_protocol);
		if(sockfd>0)
		{
			n=1;
			setsockopt(sockfd,SOL_SOCKET,SO_REUSEADDR,&n,sizeof(n));
			if(!bind(sockfd,t->ai_addr,t->ai_addrlen))
				break;
			close(sockfd);
			sockfd=-1;
		}
		
	}
	
	freeaddrinfo(res);
	free(service);
	
	if(sockfd<0)
	{
		fprintf(stderr,"failed to listen to port %d\n",port);
		return -1;
	}
	
	//listen(sockfd,100);
	//connfd=accept(sockfd,NULL,0);
	//close(sockfd);
	//if(connfd<0)
	//{
	//	fprintf(stderr,"accept() failed\n");
	//	return -1;
	//}	
	//return connfd;
	return sockfd;
}

int sock_client_connect(const char *server_name,int port)
{
	struct addrinfo	*res,*t;
	struct addrinfo hints;
	hints.ai_family=AF_UNSPEC;
    hints.ai_socktype=SOCK_STREAM;
	
	char *service;
	int n;
	int sockfd=-1;
	
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if(sockfd<0){
        fprintf(stderr, "couldn't create socket\n");
        return -1;
    }
    struct sockaddr_in serveraddr;
    memset(&serveraddr, 0, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port=htons(port);
    inet_pton(AF_INET, server_name, &serveraddr.sin_addr.s_addr);
    if(connect(sockfd, (struct sockaddr*)&serveraddr, sizeof(serveraddr))<0){
        fprintf(stderr, "couldn't connect to server\n");
        return -1;
    }
	
	if(sockfd<0)
	{
		fprintf(stderr,"couldn't connect to %s:%d\n",server_name,port);
		return -1;
	}
	return sockfd;
}

static int sock_recv(int sockfd,size_t size,void *buf)
{
	int rc;

retry_after_signal:	
	rc=recv(sockfd,buf,size,MSG_WAITALL);
	//printf("sock_recv size is :%d\n",rc);
	if(rc!=size)
	{
		fprintf(stderr,"recv failed :%s,rc=%d\n",strerror(errno),rc);
		
		if((errno==EINTR)&&(rc!=0))
			goto retry_after_signal;
		if(rc)
			return rc;
		else 	return -1;
	}
	return 0;
}

static int sock_send(int sockfd,size_t size,const void *buf)
{
	int rc;
retry_after_signal:
	rc=send(sockfd,buf,size,0);
	//printf("sock_send size is :%d\n",rc);
	if(rc!=size)
	{
		fprintf(stderr,"send failed:%s,rc=%d\n",strerror(errno),rc);
		if((errno==EINTR)&&(rc!=0))
			goto retry_after_signal;
		if(rc)
			return rc;
		else 	return -1;
	}
	return 0;
}

int sock_sync_data(int sockfd,int is_daemon,size_t size,const void* out_buf,void *in_buf)
{
	int rc;
	if(is_daemon)
	{
		rc=sock_send(sockfd,size,out_buf);
		if(rc)
			return rc;
		rc=sock_recv(sockfd,size,in_buf);
		if(rc)
			return rc;
	}
	else
	{
		rc=sock_recv(sockfd,size,in_buf);
		if(rc)
			return rc;
		rc=sock_send(sockfd,size,out_buf);	
		if(rc)
			return rc;
	}
	
	return 0;
}

int sock_sync_ready(int sockfd,int is_daemon)
{
	char cm_buf='a';
	return sock_sync_data(sockfd,is_daemon,sizeof(cm_buf),&cm_buf,&cm_buf);
}
