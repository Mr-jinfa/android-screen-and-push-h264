#include <stdio.h>  
#include <stdlib.h>   
#include <string.h>    
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <media/stagefright/MediaCodec.h>

extern int onDataSync;
extern int rtpStop;
extern pthread_mutex_t m;
extern pthread_cond_t v;
#define BITRATE 4000000

#define cmd_pipe 0
#define video_pipe 1
#define audio_pipe 2
#define connect_num 3	//接收客户端的三个连接

typedef struct 
{
	int buffer_len;	//这个pad buffer的长度
	int buffer_point; //当前pad_buffer的下标(读到那个位置)
	float fps;	//推流帧率和编码保持一致.
	int connfd[2];	//监听对端连接事件的设备描述符
	char client_ip[2][INET_ADDRSTRLEN];	//客户端ip,第一个连接进来的为视频通道
	int client_port[2];	//客户端连进来的端口
	unsigned char *pad_buffer;
}padBuffer;

typedef struct 
{
	int buffer_len;
	unsigned char *cmd_buffer;
}cmdBuffer;
#define cmdSize	64

extern padBuffer h264Nalu;
extern int thread_retry_count;
extern int send_lock;


void *raw_send(void *);
int frame_struct(uint8_t *data, int flag);

