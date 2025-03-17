#include "common/Crc32.hpp"
#include "common/PacketHeader.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <chrono>
#include <thread>
#include <cstring>
#include <cstdlib>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <getopt.h>
#include <algorithm>

using namespace std;
using namespace std::chrono;

#define MAX_PACKET_SIZE 1472
#define HEADER_SIZE sizeof(PacketHeader)
#define DATA_SIZE (MAX_PACKET_SIZE - HEADER_SIZE)
#define TIMEOUT_MS 500

struct Packet {
    PacketHeader header;
    vector<char> data;
    steady_clock::time_point sendTime;
    bool acked;
};

void logPacket(ofstream &logfile, const PacketHeader &header) {
    logfile << header.type << " " << header.seqNum << " " << header.length << " " << header.checksum << "\n";
    logfile.flush();
}

int main(int argc, char* argv[]) {
    string hostname;
    int port = 0, windowSize = 0;
    string inputFile, logFile;
    
    int opt;
    while ((opt = getopt(argc, argv, "h:p:w:i:o:")) != -1) {
        switch(opt) {
            case 'h': hostname = optarg; break;
            case 'p': port = atoi(optarg); break;
            case 'w': windowSize = atoi(optarg); break;
            case 'i': inputFile = optarg; break;
            case 'o': logFile = optarg; break;
            default:
                cerr << "Usage: ./wSenderOpt -h <hostname> -p <port> -w <window-size> -i <input-file> -o <output-log>\n";
                return 1;
        }
    }
    
    ifstream infile(inputFile, ios::binary);
    if (!infile) {
        cerr << "Error opening input file\n";
        return 1;
    }
    vector<char> fileData((istreambuf_iterator<char>(infile)), istreambuf_iterator<char>());
    infile.close();
    
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }
    sockaddr_in servAddr;
    memset(&servAddr, 0, sizeof(servAddr));
    servAddr.sin_family = AF_INET;
    servAddr.sin_port = htons(port);
    inet_pton(AF_INET, hostname.c_str(), &servAddr.sin_addr);
    
    ofstream logfile(logFile);
    if (!logfile) {
        cerr << "Error opening log file\n";
        return 1;
    }
    
    vector<Packet> packets;
    // START packet
    Packet startPkt;
    startPkt.header.type = 0;
    startPkt.header.seqNum = rand() % 10000;
    startPkt.header.length = 0;
    startPkt.header.checksum = crc32(&startPkt.header, HEADER_SIZE - sizeof(uint32_t));
    startPkt.acked = false;
    packets.push_back(startPkt);
    
    // DATA packets
    uint32_t seq = 0;
    size_t offset = 0;
    while(offset < fileData.size()) {
        Packet pkt;
        pkt.header.type = 2;
        pkt.header.seqNum = seq++;
        size_t chunkSize = min((size_t)DATA_SIZE, fileData.size() - offset);
        pkt.header.length = chunkSize;
        pkt.data.insert(pkt.data.end(), fileData.begin()+offset, fileData.begin()+offset+chunkSize);
        PacketHeader temp = pkt.header;
        temp.checksum = 0;
        pkt.header.checksum = crc32(&temp, HEADER_SIZE) ^ crc32(pkt.data.data(), pkt.data.size());
        pkt.acked = false;
        packets.push_back(pkt);
        offset += chunkSize;
    }
    
    // END packet
    Packet endPkt;
    endPkt.header.type = 1;
    endPkt.header.seqNum = packets[0].header.seqNum;
    endPkt.header.length = 0;
    endPkt.header.checksum = crc32(&endPkt.header, HEADER_SIZE - sizeof(uint32_t));
    endPkt.acked = false;
    packets.push_back(endPkt);
    
    // --- Send START packet and wait for individual ACK ---
    sendto(sock, &packets[0].header, HEADER_SIZE, 0, (sockaddr*)&servAddr, sizeof(servAddr));
    logPacket(logfile, packets[0].header);
    char ackBuffer[MAX_PACKET_SIZE];
    sockaddr_in fromAddr;
    socklen_t fromLen = sizeof(fromAddr);
    recvfrom(sock, ackBuffer, MAX_PACKET_SIZE, 0, (sockaddr*)&fromAddr, &fromLen);
    PacketHeader ack;
    memcpy(&ack, ackBuffer, HEADER_SIZE);
    if (ack.type == 3 && ack.seqNum == packets[0].header.seqNum)
        packets[0].acked = true;
    logPacket(logfile, ack);
    
    size_t base = 1, next = base;
    while (base < packets.size()) {
        // Send packets in window
        while (next < packets.size() && next < base + windowSize) {
            if (!packets[next].acked) {
                sendto(sock, &packets[next].header, HEADER_SIZE, 0, (sockaddr*)&servAddr, sizeof(servAddr));
                if (packets[next].header.type == 2 && !packets[next].data.empty())
                    sendto(sock, packets[next].data.data(), packets[next].data.size(), 0, (sockaddr*)&servAddr, sizeof(servAddr));
                packets[next].sendTime = steady_clock::now();
                logPacket(logfile, packets[next].header);
            }
            next++;
        }
        
        // Wait for individual ACKs
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);
        timeval tv = {0, 500000};
        if (select(sock+1, &readfds, NULL, NULL, &tv) > 0) {
            ssize_t recvd = recvfrom(sock, ackBuffer, MAX_PACKET_SIZE, 0, (sockaddr*)&fromAddr, &fromLen);
            if (recvd >= (ssize_t)HEADER_SIZE) {
                PacketHeader ackPkt;
                memcpy(&ackPkt, ackBuffer, HEADER_SIZE);
                if (ackPkt.type == 3) {
                    // In the optimized version, each ACK acknowledges one packet (its seqNum)
                    for (size_t i = base; i < packets.size(); i++) {
                        if (packets[i].header.seqNum == ackPkt.seqNum)
                            packets[i].acked = true;
                    }
                }
                logPacket(logfile, ackPkt);
            }
        }
        
        // Check per-packet timers and retransmit those that have timed out
        auto now = steady_clock::now();
        for (size_t i = base; i < next; i++) {
            if (!packets[i].acked) {
                auto elapsed = duration_cast<milliseconds>(now - packets[i].sendTime).count();
                if (elapsed >= TIMEOUT_MS) {
                    sendto(sock, &packets[i].header, HEADER_SIZE, 0, (sockaddr*)&servAddr, sizeof(servAddr));
                    if (packets[i].header.type == 2 && !packets[i].data.empty())
                        sendto(sock, packets[i].data.data(), packets[i].data.size(), 0, (sockaddr*)&servAddr, sizeof(servAddr));
                    packets[i].sendTime = steady_clock::now();
                    logPacket(logfile, packets[i].header);
                }
            }
        }
        while (base < packets.size() && packets[base].acked)
            base++;
    }
    
    close(sock);
    logfile.close();
    return 0;
}
