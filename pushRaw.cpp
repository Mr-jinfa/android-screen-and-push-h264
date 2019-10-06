/*============================================================================= 
 *     FileName: raw.c 
 *         Desc: raw payload h.264 data 
 *       Author: licaibiao 
 *   LastChange: 2017-04-07  
 * =============================================================================*/
#include <stdio.h>  
#include <stdlib.h>  
#include <string.h>  
#include <memory.h>    
#include <sys/types.h>  
#include <sys/socket.h>  
#include <arpa/inet.h>
#include <netinet/in.h>  
#include <netdb.h> 
#include <unistd.h> 
#include <pthread.h>
#include <utils/Log.h>

#include "pushRaw.h"

#define DEBUG_LOG

#define LOG_TAG "ScreenRecord_thread"

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG,LOG_TAG ,__VA_ARGS__) // 定义LOGD类型   
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,LOG_TAG ,__VA_ARGS__) // 定义LOGI类型   
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,LOG_TAG ,__VA_ARGS__) // 定义LOGW类型   
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR,LOG_TAG ,__VA_ARGS__) // 定义LOGE类型   
#define LOGF(...) __android_log_print(ANDROID_LOG_FATAL,LOG_TAG ,__VA_ARGS__) // 定义LOGF类型 
int thread_retry_count=0;
int send_lock=0;

void *raw_send(void *)
{
	int rt=0, count=0, nsend=0;
	while(!rtpStop)
	{
		if(!send_lock)
		{
			//printf("---\ngo to lock\n---\n");
			pthread_mutex_lock(&m);
			//printf("have lock\n");
			while(onDataSync <= 0)
				pthread_cond_wait(&v, &m); // 进入该函数会自动对m解锁，从该函数返回会自动对m加锁
			if(rtpStop)
				return 0;
			//LOGD("get h264 data");
			//printf("[ScreenRecord_thread] get h264 data\n");
			onDataSync = 0;
			frame_struct(h264Nalu.pad_buffer, 1);

			rt = send(h264Nalu.connfd[video_pipe], h264Nalu.pad_buffer, h264Nalu.buffer_len, 0);
			pthread_mutex_unlock(&m);
			if(rt == -1)	//之前有3次尝试机会且都返回-1(对端网络异常?)子线程自动把发送锁加上并进入等待队列等待,等主线程主动唤醒
				send_lock == 1;
		}else {
			pthread_mutex_lock(&m);
			pthread_cond_wait(&v, &m);
			pthread_mutex_unlock(&m);
		}
 	}
	return 0;
}
