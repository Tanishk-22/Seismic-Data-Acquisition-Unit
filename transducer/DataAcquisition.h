#ifndef _DATAACQUISITION_H_
#define _DATAACQUISITION_H_

#include <arpa/inet.h>
#include <iostream>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <map>
#include <set>
#include <queue>
#include "SeismicData.h"

static void interruptHandler(int signum);
void *recv_func(void *arg);
void *write_func(void *arg);

class DataAcquisition
{

    struct DataPacket
    {
        uint8_t packetNo;
        uint16_t packetLen;
        std::string data;
    };

    struct Subscriber
    {
        char username[BUF2_LEN];
        char ip_address[INET_ADDRSTRLEN];
        int port;
    };

    bool is_running;
    sem_t *sem_id1;
    key_t ShmKey;
    int ShmID;
    int fd;
    pthread_t r_tid,w_tid;
    struct SeismicMemory *ShmPTR;
    pthread_mutex_t lock_x;
    struct sigaction action;
    std::queue<DataPacket> dataQueue;
    std::map<std::string, Subscriber> rogue_data_centers_;
    std::map<std::string, Subscriber> subscribers_;
    std::map<std::string, int> block_list;

public:
    DataAcquisition();
    int run();
    void shutdown();
    void ReceiveFunction();
    void WriteFunction();
    static DataAcquisition *instance;
};

#endif //_DATAACQUISITION_H_