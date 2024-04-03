#include "DataAcquisition.h"

using namespace std;

DataAcquisition *DataAcquisition::instance = nullptr;

static void interruptHandler(int sig)
{
    switch (sig)
    {
    case SIGINT:
        DataAcquisition::instance->shutdown();
        break;
    }
}

void DataAcquisition::shutdown()
{
    cout << "DataAcquistion::shutdown:" << endl;
    is_running = false;
}

DataAcquisition::DataAcquisition()
{
    is_running = false;
    ShmPTR = nullptr;
    DataAcquisition::instance = this;
}

int DataAcquisition::run()
{
    int ret, len;
    struct sockaddr_in servaddr;
    const int PORT = 1153;              // Address of the server
    const char IP_ADDR[] = "127.0.0.1"; // Address of the server
    action.sa_handler = interruptHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGINT, &action, NULL);

    ShmKey = ftok(MEMNAME, 65);

    ShmID = shmget(ShmKey, sizeof(struct SeismicMemory), IPC_CREAT | 0666);
    if (ShmID < 0)
    {
        cout << "DataAcquisition: shmget() error" << endl;
        cout << strerror(errno) << endl;
        return -1;
    }

    ShmPTR = (struct SeismicMemory *)shmat(ShmID, NULL, 0);
    if (ShmPTR == (void *)-1)
    {
        cout << "DataAcquisition: shmat() error" << endl;
        cout << strerror(errno) << endl;
        return -1;
    }

    sem_id1 = sem_open(SEMNAME, O_CREAT, SEM_PERMS, 0);
    is_running = true;
    ShmPTR->packetNo = 0;

    fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (fd < 0)
    {
        cout << "Cannot create the socket" << endl;
        cout << strerror(errno) << endl;
        return -1;
    }

    memset((char *)&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    ret = inet_pton(AF_INET, IP_ADDR, &servaddr.sin_addr);
    if (ret == 0)
    {
        cout << "No such address" << endl;
        cout << strerror(errno) << endl;
        close(fd);
        return -1;
    }
    servaddr.sin_port = htons(PORT);

    ret = bind(fd, (struct sockaddr *)&servaddr, sizeof(servaddr));
    if (ret < 0)
    {
        cout << "Cannot bind the socket to the local address" << endl;
        cout << strerror(errno) << endl;
        return -1;
    }
    else
    {
#ifdef DEBUG
        cout << "socket fd:" << fd << " bound to address " << inet_ntoa(myaddr.sin_addr) << endl;
#endif
    }

    int seismicDataIndex = 0;
    is_running = true;
    pthread_mutex_init(&lock_x, NULL);
    ret = pthread_create(&r_tid, NULL, recv_func, this);
    if (ret != 0)
    {
        cout << "Cannot create receive thread" << endl;
        cout << strerror(errno) << endl;
        close(fd);
        return -1;
    }
    ret = pthread_create(&w_tid, NULL, write_func, this);
    if (ret != 0)
    {
        cout << "Cannot create receive thread" << endl;
        cout << strerror(errno) << endl;
        close(fd);
        return -1;
    }

    while (is_running)
    {
        // Lock the mutex to access the data packet queue
        if (ShmPTR->seismicData[seismicDataIndex].status == WRITTEN)
        {
            struct DataPacket packet;
            sem_wait(sem_id1);
            packet.packetNo = uint8_t(ShmPTR->packetNo);
            packet.packetLen = ShmPTR->seismicData[seismicDataIndex].packetLen;
            packet.data = string(ShmPTR->seismicData[seismicDataIndex].data);

            ShmPTR->seismicData[seismicDataIndex].status = READ;
            sem_post(sem_id1);

            pthread_mutex_lock(&lock_x);
            dataQueue.push(packet);
            pthread_mutex_unlock(&lock_x);

            ++seismicDataIndex;
            if (seismicDataIndex > NUM_DATA)
                seismicDataIndex = 0;
        }
        sleep(1);
    }
    cout << "DataAcquisition is quitting..." << endl;
    sem_close(sem_id1);
    sem_unlink(SEMNAME);
    shmdt((void *)ShmPTR);
    shmctl(ShmID, IPC_RMID, NULL);
    pthread_join(r_tid, NULL);
    pthread_join(w_tid, NULL);
    close(fd);
    return 0;
}
void *recv_func(void *arg)
{
    DataAcquisition *dataAcquisition = (DataAcquisition *)arg;
    dataAcquisition->ReceiveFunction();
    pthread_exit(NULL);
}

void *write_func(void *arg)
{
    DataAcquisition *dataAcquisition = (DataAcquisition *)arg;
    dataAcquisition->WriteFunction();
    pthread_exit(NULL);
}

void DataAcquisition::WriteFunction()
{
    DataPacket *packet;
    struct sockaddr_in dest_addr;
    while (is_running)
    {
        if (!dataQueue.empty())
        {
            cout << "DataPacket.size(): " << dataQueue.size() << "Number of Clients: " << subscribers_.size() << endl;
            packet = &(dataQueue.front());

            pthread_mutex_lock(&lock_x);
            subscribers_ = subscribers_;
            pthread_mutex_unlock(&lock_x);

            // Send the data packet to all subscribed data centers
            for (auto it = subscribers_.begin(); it != subscribers_.end(); it++)
            {

                memset(&dest_addr, 0, sizeof(dest_addr));
                dest_addr.sin_family = AF_INET;
                dest_addr.sin_port = htons(it->second.port);
                inet_pton(AF_INET, it->second.ip_address, &dest_addr.sin_addr);

                char ip_address[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &(dest_addr.sin_addr), ip_address, INET_ADDRSTRLEN);

                cout << "Sending Packet to: " << ip_address << ":" << ntohs(dest_addr.sin_port) << endl;

                int packetLength = packet->packetLen;
                int packetNumber = packet->packetNo;

                // Create the message to send to the data center
                char message[packetLength + 3];
                message[0] = packetNumber & 0xFF;
                message[1] = (packetLength >> 8) & 0xFF;
                message[2] = packetLength & 0xFF;
                memcpy(message + 3, packet->data.c_str(), packetLength);

                // Send the message to the data center
                int bytesSent = sendto(fd, (char *)message, packetLength + 3, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));

                // Check if there was an error in sending the message
                if (bytesSent < 0)
                {
                    perror("Error sending data packet to data center");
                }
            }
            pthread_mutex_lock(&lock_x);
            dataQueue.pop();
            pthread_mutex_unlock(&lock_x);
        }
        // Sleep for 1 second before sending the next data packet
        sleep(1);
    }
    cout << "DEBUG: Write thread exiting... " << endl;
    pthread_exit(NULL);
}

void DataAcquisition::ReceiveFunction()
{
    int ret = 0;
    int port;
    char ip_address[INET_ADDRSTRLEN];
    char buf[BUF_LEN];
    struct sockaddr_in dest_addr;
    socklen_t addrlen = sizeof(dest_addr);
    memset(buf, 0, BUF_LEN);

    while (is_running)
    {
        memset(&dest_addr, 0, sizeof(dest_addr));
        unsleep(100000);
        int len = recvfrom(fd, buf, BUF_LEN, 0, (struct sockaddr *)&dest_addr, &addrlen);
        if (len <= 0)
            perror("Error receiving data");
            sleep(1);
        // Parse the CSV stream
        cout<<"RECIEVED: "<<buf<<endl;
        memset(ip_address, 0, INET_ADDRSTRLEN);
        inet_ntop(AF_INET, &(dest_addr.sin_addr), ip_address, INET_ADDRSTRLEN);
        port = ntohs(dest_addr.sin_port);
        string key = string(ip_address) + ":" + to_string(port);
        if (rogue_data_centers_.find(key) == rogue_data_centers_.end())
        {
            const char password[] = "Leaf";
            const char sub[] = "Subscribe";
            const char unsub[] = "Cancel";
            const char approved[] = "Subscribed";

            const int MSG_IND = 0;
            const int USER_IND = 1;
            const int PASS_IND = 2;

            if (port == 0 || ip_address == "0.0.0.0")
                return;

            string key = string(ip_address) + ":" + to_string(port);

            const int MAX = 3;

            char extractMsg[MAX][BUF2_LEN];
            int idx = 0;
            char *token = strtok(buf, ",");
            while (idx < MAX)
            {

                memset(&extractMsg[idx], 0, BUF2_LEN);

                if (token == NULL)
                    memcpy(&extractMsg[idx], "", BUF2_LEN);
                else
                    memcpy(&extractMsg[idx], token, BUF2_LEN);

                token = strtok(NULL, ",");
                idx++;
            }
            Subscriber subs;
            memcpy(&subs.username, &extractMsg[USER_IND], BUF2_LEN);
            memcpy(&subs.ip_address, &ip_address, INET_ADDRSTRLEN);
            subs.port = port;

            // Cancel a subscription
            if (strcmp(extractMsg[MSG_IND], unsub) == 0)
            {
                pthread_mutex_lock(&lock_x);
                subscribers_.erase(key);
                pthread_mutex_unlock(&lock_x);
                cout << "[" << subs.username << "] " << key << " has unsubscribed. " << endl;
            }
            // Subscribe a User
            if ((strcmp(extractMsg[MSG_IND], sub) == 0) && (strcmp(extractMsg[PASS_IND], password) == 0))
            {

                if (block_list.find(key) == block_list.end())
                {

                    pthread_mutex_lock(&lock_x);
                    subscribers_[key] = subs;
                    pthread_mutex_unlock(&lock_x);

                    cout << "[" << extractMsg[USER_IND] << "] has subscribed! " << endl;

                    sendto(fd, approved, sizeof(approved), 0, (struct sockaddr *)&dest_addr, &addrlen);
                }
                else
                    cout << "[" << extractMsg[USER_IND] << "] has already subscribed." << endl;
            }

            // Blocking the Rogue Centers
            if ((strcmp(extractMsg[MSG_IND], sub) != 0) && strcmp(extractMsg[MSG_IND], unsub) != 0)
            {
                // invalid command
                cout << "DataAcquisition: unknown command " << extractMsg[MSG_IND] << endl;
                // addes Rogue center to the BlockList
                block_list[key] = (block_list.find(key) != block_list.end()) ? block_list[key] + 1 : 1;
                if (block_list[key] >= 3)
                {
                    rogue_data_centers_[key] = subs;
                    cout << " [Blocked] " << subs.username << " " << key << endl;
                    pthread_mutex_lock(&lock_x);
                    subscribers_.erase(key);
                    pthread_mutex_unlock(&lock_x);
                }
            }
            else if (((strcmp(extractMsg[PASS_IND], password) != 0) && (strcmp(extractMsg[MSG_IND], unsub) != 0))){
                // addes Rogue center to the BlockList
                block_list[key] = (block_list.find(key) != block_list.end()) ? block_list[key] + 1 : 1;
                if (block_list[key] >= 3)
                {
                    rogue_data_centers_[key] = subs;
                    cout << " [Blocked] " << subs.username << " " << key << endl;
                    pthread_mutex_lock(&lock_x);
                    subscribers_.erase(key);
                    pthread_mutex_unlock(&lock_x);
                }
            }
        }
    }
    cout << "DEBUG: Recieve thread exiting... " << endl;
    pthread_exit(NULL);
}