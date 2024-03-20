//
// Created by sun on 2022/12/9.
//

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <windows.h>
#include "rtp.h"

#define SERVER_PORT     8554
#define SERVER_RTP_PORT  55532
#define SERVER_RTCP_PORT 55533
#define BUF_MAX_SIZE    (1024*1024)
#define AAC_FILE_NAME   "../data/test-long.aac"

static int createTcpSocket() {
    int sockfd;
    int on = 1;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        return -1;

    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));

    return sockfd;
}

static int createUdpSocket() {
    int sockfd;
    int on = 1;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
        return -1;

    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));

    return sockfd;
}

static int bindSocketAddr(int sockfd, const char* ip, int port) {
    struct sockaddr_in addr;

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip);

    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(struct sockaddr)) < 0)
        return -1;

    return 0;
}

struct AdtsHeader {
    unsigned int syncword;  //12 bit ͬ���� '1111 1111 1111'��һ��ADTS֡�Ŀ�ʼ
    uint8_t id;        //1 bit 0����MPEG-4, 1����MPEG-2��
    uint8_t layer;     //2 bit ����Ϊ0
    uint8_t protectionAbsent;  //1 bit 1����û��CRC��0������CRC
    uint8_t profile;           //1 bit AAC����MPEG-2 AAC�ж�����3��profile��MPEG-4 AAC�ж�����6��profile��
    uint8_t samplingFreqIndex; //4 bit ������
    uint8_t privateBit;        //1bit ����ʱ����Ϊ0������ʱ����
    uint8_t channelCfg;        //3 bit ��������
    uint8_t originalCopy;      //1bit ����ʱ����Ϊ0������ʱ����
    uint8_t home;               //1 bit ����ʱ����Ϊ0������ʱ����

    uint8_t copyrightIdentificationBit;   //1 bit ����ʱ����Ϊ0������ʱ����
    uint8_t copyrightIdentificationStart; //1 bit ����ʱ����Ϊ0������ʱ����
    unsigned int aacFrameLength;               //13 bit һ��ADTS֡�ĳ��Ȱ���ADTSͷ��AACԭʼ��
    unsigned int adtsBufferFullness;           //11 bit �����������ȣ�0x7FF˵�������ʿɱ������������Ҫ���ֶΡ�CBR������Ҫ���ֶΣ���ͬ������ʹ�������ͬ�������ʹ����Ƶ�����ʱ����Ҫע�⡣

    /* number_of_raw_data_blocks_in_frame
     * ��ʾADTS֡����number_of_raw_data_blocks_in_frame + 1��AACԭʼ֡
     * ����˵number_of_raw_data_blocks_in_frame == 0
     * ��ʾ˵ADTS֡����һ��AAC���ݿ鲢����˵û�С�(һ��AACԭʼ֡����һ��ʱ����1024���������������)
     */
    uint8_t numberOfRawDataBlockInFrame; //2 bit
};

static int parseAdtsHeader(uint8_t* in, struct AdtsHeader* res) {
    static int frame_number = 0;
    memset(res, 0, sizeof(*res));

    if ((in[0] == 0xFF) && ((in[1] & 0xF0) == 0xF0))
    {
        res->id = ((uint8_t)in[1] & 0x08) >> 3;//�ڶ����ֽ���0x08������֮�󣬻�õ�13λbit��Ӧ��ֵ
        res->layer = ((uint8_t)in[1] & 0x06) >> 1;//�ڶ����ֽ���0x06������֮������1λ����õ�14,15λ����bit��Ӧ��ֵ
        res->protectionAbsent = (uint8_t)in[1] & 0x01;
        res->profile = ((uint8_t)in[2] & 0xc0) >> 6;
        res->samplingFreqIndex = ((uint8_t)in[2] & 0x3c) >> 2;
        res->privateBit = ((uint8_t)in[2] & 0x02) >> 1;
        res->channelCfg = ((((uint8_t)in[2] & 0x01) << 2) | (((unsigned int)in[3] & 0xc0) >> 6));
        res->originalCopy = ((uint8_t)in[3] & 0x20) >> 5;
        res->home = ((uint8_t)in[3] & 0x10) >> 4;
        res->copyrightIdentificationBit = ((uint8_t)in[3] & 0x08) >> 3;
        res->copyrightIdentificationStart = (uint8_t)in[3] & 0x04 >> 2;
        
        res->aacFrameLength = (((((unsigned int)in[3]) & 0x03) << 11) |
            (((unsigned int)in[4] & 0xFF) << 3) |
            ((unsigned int)in[5] & 0xE0) >> 5);

        res->adtsBufferFullness = (((unsigned int)in[5] & 0x1f) << 6 |
            ((unsigned int)in[6] & 0xfc) >> 2);
        res->numberOfRawDataBlockInFrame = ((uint8_t)in[6] & 0x03);

        return 0;
    }
    else
    {
        printf("failed to parse adts header\n");
        return -1;
    }
}

static int rtpSendAACFrame(int socket, const char* ip, int16_t port,
    struct RtpPacket* rtpPacket, uint8_t* frame, uint32_t frameSize) {
    //����ĵ���https://blog.csdn.net/yangguoyu8023/article/details/106517251/
    int ret;

    rtpPacket->payload[0] = 0x00;
    rtpPacket->payload[1] = 0x10;
    rtpPacket->payload[2] = (frameSize & 0x1FE0) >> 5; //��8λ
    rtpPacket->payload[3] = (frameSize & 0x1F) << 3; //��5λ

    memcpy(rtpPacket->payload + 4, frame, frameSize);

    ret = rtpSendPacketOverUdp(socket, ip, port, rtpPacket, frameSize + 4);
    if (ret < 0)
    {
        printf("failed to send rtp packet\n");
        return -1;
    }

    rtpPacket->rtpHeader.seq++;

    /*
     * �������Ƶ����44100
     * һ��AACÿ��1024������Ϊһ֡
     * ����һ����� 44100 / 1024 = 43֡
     * ʱ���������� 44100 / 43 = 1025
     * һ֡��ʱ��Ϊ 1 / 43 = 23ms
     */
    rtpPacket->rtpHeader.timestamp += 1025;

    return 0;
}

static int acceptClient(int sockfd, char* ip, int* port) {
    int clientfd;
    socklen_t len = 0;
    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));
    len = sizeof(addr);

    clientfd = accept(sockfd, (struct sockaddr*)&addr, &len);
    if (clientfd < 0)
        return -1;

    strcpy(ip, inet_ntoa(addr.sin_addr));
    *port = ntohs(addr.sin_port);

    return clientfd;
}

static char* getLineFromBuf(char* buf, char* line) {
    while (*buf != '\n')
    {
        *line = *buf;
        line++;
        buf++;
    }

    *line = '\n';
    ++line;
    *line = '\0';

    ++buf;
    return buf;
}

static int handleCmd_OPTIONS(char* result, int cseq) {
    sprintf(result, "RTSP/1.0 200 OK\r\n"
        "CSeq: %d\r\n"
        "Public: OPTIONS, DESCRIBE, SETUP, PLAY\r\n"
        "\r\n",
        cseq);

    return 0;
}

static int handleCmd_DESCRIBE(char* result, int cseq, char* url) {
    char sdp[500];
    char localIp[100];

    sscanf(url, "rtsp://%[^:]:", localIp);

    sprintf(sdp, "v=0\r\n"
        "o=- 9%ld 1 IN IP4 %s\r\n"
        "t=0 0\r\n"
        "a=control:*\r\n"
        "m=audio 0 RTP/AVP 97\r\n"
        "a=rtpmap:97 mpeg4-generic/44100/2\r\n"
        "a=fmtp:97 profile-level-id=1;mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3;config=1210;\r\n"
   
        //"a=fmtp:97 SizeLength=13;\r\n"
        "a=control:track0\r\n",
        time(NULL), localIp);

    sprintf(result, "RTSP/1.0 200 OK\r\nCSeq: %d\r\n"
        "Content-Base: %s\r\n"
        "Content-type: application/sdp\r\n"
        "Content-length: %d\r\n\r\n"
        "%s",
        cseq,
        url,
        strlen(sdp),
        sdp);

    return 0;
}

static int handleCmd_SETUP(char* result, int cseq, int clientRtpPort) {
    sprintf(result, "RTSP/1.0 200 OK\r\n"
        "CSeq: %d\r\n"
        "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d\r\n"
        "Session: 66334873\r\n"
        "\r\n",
        cseq,
        clientRtpPort,
        clientRtpPort + 1,
        SERVER_RTP_PORT,
        SERVER_RTCP_PORT
    );

    return 0;
}

static int handleCmd_PLAY(char* result, int cseq) {
    sprintf(result, "RTSP/1.0 200 OK\r\n"
        "CSeq: %d\r\n"
        "Range: npt=0.000-\r\n"
        "Session: 66334873; timeout=10\r\n\r\n",
        cseq);

    return 0;
}


static void doClient(int clientSockfd, const char* clientIP, int clientPort) {

    int serverRtpSockfd = -1, serverRtcpSockfd = -1;

    char method[40];
    char url[100];
    char version[40];
    int CSeq;

    int clientRtpPort, clientRtcpPort;
    char* rBuf = (char*)malloc(BUF_MAX_SIZE);
    char* sBuf = (char*)malloc(BUF_MAX_SIZE);

    while (true) {
        int recvLen;

        recvLen = recv(clientSockfd, rBuf, BUF_MAX_SIZE, 0);
        if (recvLen <= 0) {
            break;
        }

        rBuf[recvLen] = '\0';
        printf("%s rBuf = %s \n", __FUNCTION__, rBuf);

        const char* sep = "\n";
        char* line = strtok(rBuf, sep);
        while (line) {
            if (strstr(line, "OPTIONS") ||
                strstr(line, "DESCRIBE") ||
                strstr(line, "SETUP") ||
                strstr(line, "PLAY")) {

                if (sscanf(line, "%s %s %s\r\n", method, url, version) != 3) {
                    // error
                }
            }
            else if (strstr(line, "CSeq")) {
                if (sscanf(line, "CSeq: %d\r\n", &CSeq) != 1) {
                    // error
                }
            }
            else if (!strncmp(line, "Transport:", strlen("Transport:"))) {
                // Transport: RTP/AVP/UDP;unicast;client_port=13358-13359
                // Transport: RTP/AVP;unicast;client_port=13358-13359

                if (sscanf(line, "Transport: RTP/AVP/UDP;unicast;client_port=%d-%d\r\n",
                    &clientRtpPort, &clientRtcpPort) != 2) {
                    // error
                    printf("parse Transport error \n");
                }
            }
            line = strtok(NULL, sep);
        }

        if (!strcmp(method, "OPTIONS")) {
            if (handleCmd_OPTIONS(sBuf, CSeq))
            {
                printf("failed to handle options\n");
                break;
            }
        }
        else if (!strcmp(method, "DESCRIBE")) {
            if (handleCmd_DESCRIBE(sBuf, CSeq, url))
            {
                printf("failed to handle describe\n");
                break;
            }
        }
        else if (!strcmp(method, "SETUP")) {
            if (handleCmd_SETUP(sBuf, CSeq, clientRtpPort))
            {
                printf("failed to handle setup\n");
                break;
            }

            serverRtpSockfd = createUdpSocket();
            serverRtcpSockfd = createUdpSocket();
            if (serverRtpSockfd < 0 || serverRtcpSockfd < 0)
            {
                printf("failed to create udp socket\n");
                break;
            }

            if (bindSocketAddr(serverRtpSockfd, "0.0.0.0", SERVER_RTP_PORT) < 0 ||
                bindSocketAddr(serverRtcpSockfd, "0.0.0.0", SERVER_RTCP_PORT) < 0)
            {
                printf("failed to bind addr\n");
                break;
            }

        }
        else if (!strcmp(method, "PLAY")) {
            if (handleCmd_PLAY(sBuf, CSeq))
            {
                printf("failed to handle play\n");
                break;
            }
        }
        else {
            printf("δ�����method = %s \n", method);
            break;
        }

        printf("%s sBuf = %s \n", __FUNCTION__, sBuf);

        send(clientSockfd, sBuf, strlen(sBuf), 0);


        //��ʼ���ţ�����RTP��
        if (!strcmp(method, "PLAY")) {

            struct AdtsHeader adtsHeader;
            struct RtpPacket* rtpPacket;
            uint8_t* frame;
            int ret;

            FILE* fp = fopen(AAC_FILE_NAME, "rb");
            if (!fp) {
                printf("��ȡ %s ʧ��\n", AAC_FILE_NAME);
                break;
            }

            frame = (uint8_t*)malloc(5000);
            rtpPacket = (struct RtpPacket*)malloc(5000);

            rtpHeaderInit(rtpPacket, 0, 0, 0, RTP_VESION, RTP_PAYLOAD_TYPE_AAC, 1, 0, 0, 0x32411);

            while (true)
            {
                ret = fread(frame, 1, 7, fp);
                if (ret <= 0)
                {
                    printf("fread err\n");
                    break;
                }
                printf("fread ret=%d \n",ret);

                if (parseAdtsHeader(frame, &adtsHeader) < 0)
                {
                    printf("parseAdtsHeader err\n");
                    break;
                }
                ret = fread(frame, 1, adtsHeader.aacFrameLength - 7, fp);
                if (ret <= 0)
                {
                    printf("fread err\n");
                    break;
                }

                rtpSendAACFrame(serverRtpSockfd, clientIP, clientRtpPort,
                    rtpPacket, frame, adtsHeader.aacFrameLength - 7);

                Sleep(1);
                //usleep(23223);//1000/43.06 * 1000
            }

            free(frame);
            free(rtpPacket);

            break;

        }

        memset(method, 0, sizeof(method) / sizeof(char));
        memset(url, 0, sizeof(url) / sizeof(char));
        CSeq = 0;
    }

    closesocket(clientSockfd);
    if (serverRtpSockfd) {
        closesocket(serverRtpSockfd);
    }
    if (serverRtcpSockfd > 0) {
        closesocket(serverRtcpSockfd);
    }

    free(rBuf);
    free(sBuf);

}

int main() {
    // ����windows socket start
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        printf("PC Server Socket Start Up Error \n");
        return -1;
    }
    // ����windows socket end

    int rtspServerSockfd;

    int ret;

    rtspServerSockfd = createTcpSocket();
    if (rtspServerSockfd < 0)
    {
        printf("failed to create tcp socket\n");
        return -1;
    }

    ret = bindSocketAddr(rtspServerSockfd, "0.0.0.0", SERVER_PORT);
    if (ret < 0)
    {
        printf("failed to bind addr\n");
        return -1;
    }

    ret = listen(rtspServerSockfd, 10);
    if (ret < 0)
    {
        printf("failed to listen\n");
        return -1;
    }

    printf("%s rtsp://127.0.0.1:%d\n", __FILE__, SERVER_PORT);

    while (1)
    {
        int clientSockfd;
        char clientIp[40];
        int clientPort;

        clientSockfd = acceptClient(rtspServerSockfd, clientIp, &clientPort);
        if (clientSockfd < 0)
        {
            printf("failed to accept client\n");
            return -1;
        }

        printf("accept client;client ip:%s,client port:%d\n", clientIp, clientPort);

        doClient(clientSockfd, clientIp, clientPort);
    }

    closesocket(rtspServerSockfd);

    return 0;
}

