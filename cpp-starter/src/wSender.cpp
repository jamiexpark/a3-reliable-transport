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

// Structure to hold packet info
struct Packet {
    PacketHeader header;
    vector<char> data; // Only used for DATA packets
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
    
    // Parse command-line arguments
    int opt;
    while ((opt = getopt(argc, argv, "h:p:w:i:o:")) != -1) {
        switch(opt) {
            case 'h': hostname = optarg; break;
            case 'p': port = atoi(optarg); break;
            case 'w': windowSize = atoi(optarg); break;
            case 'i': inputFile = optarg; break;
            case 'o': logFile = optarg; break;
            default:
                cerr << "Usage: ./wSender -h <hostname> -p <port> -w <window-size> -i <input-file> -o <output-log>\n";
                return 1;
        }
    }
    
    // Read input file into memory
    ifstream infile(inputFile, ios::binary);
    if (!infile) {
        cerr << "Error opening input file\n";
        return 1;
    }
    vector<char> fileData((istreambuf_iterator<char>(infile)), istreambuf_iterator<char>());
    infile.close();
    
    // Create UDP socket
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
    
    // Open log file for writing
    ofstream logfile(logFile);
    if (!logfile) {
        cerr << "Error opening log file\n";
        return 1;
    }
    
    vector<Packet> packets;
    // Create START packet (type 0) with a random seqNum
    Packet startPkt;
    startPkt.header.type = 0;
    startPkt.header.seqNum = rand() % 10000;
    startPkt.header.length = 0;
    startPkt.header.checksum = crc32(&startPkt.header, HEADER_SIZE - sizeof(uint32_t));
    startPkt.acked = false;
    packets.push_back(startPkt);
    
    // Create DATA packets (type 2)
    uint32_t seq = 0; // data packets start at 0
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
        // For DATA packets we combine checksum over header and data
        pkt.header.checksum = crc32(&temp, HEADER_SIZE) ^ crc32(pkt.data.data(), pkt.data.size());
        pkt.acked = false;
        packets.push_back(pkt);
        offset += chunkSize;
    }
    
    // Create END packet (type 1) with same seqNum as START packet
    Packet endPkt;
    endPkt.header.type = 1;
    endPkt.header.seqNum = packets[0].header.seqNum;
    endPkt.header.length = 0;
    endPkt.header.checksum = crc32(&endPkt.header, HEADER_SIZE - sizeof(uint32_t));
    endPkt.acked = false;
    packets.push_back(endPkt);
    
    // --- Send START packet and wait for its ACK ---
    sendto(sock, &packets[0].header, HEADER_SIZE, 0, (sockaddr*)&servAddr, sizeof(servAddr));
    logPacket(logfile, packets[0].header);
    char ackBuffer[MAX_PACKET_SIZE];
    sockaddr_in fromAddr;
    socklen_t fromLen = sizeof(fromAddr);
    // Wait (with timeout) for ACK of the START packet
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(sock, &readfds);
    timeval tv = {0, 500000}; // 500ms timeout
    if (select(sock+1, &readfds, NULL, NULL, &tv) > 0) {
        recvfrom(sock, ackBuffer, MAX_PACKET_SIZE, 0, (sockaddr*)&fromAddr, &fromLen);
        PacketHeader ack;
        memcpy(&ack, ackBuffer, HEADER_SIZE);
        if (ack.type == 3 && ack.seqNum == packets[0].header.seqNum)
            packets[0].acked = true;
        logPacket(logfile, ack);
    } else {
        // retransmit if timeout
        sendto(sock, &packets[0].header, HEADER_SIZE, 0, (sockaddr*)&servAddr, sizeof(servAddr));
        logPacket(logfile, packets[0].header);
    }
    
    // --- Sliding window transfer for DATA and END packets ---
    size_t base = 1;  // first packet index to be acknowledged (DATA packets start at index 1)
    size_t next = base;
    while (base < packets.size()) {
        // Send new packets within the window
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
        // Wait for ACKs with timeout
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);
        tv.tv_sec = 0; tv.tv_usec = 500000;
        if (select(sock+1, &readfds, NULL, NULL, &tv) > 0) {
            ssize_t recvd = recvfrom(sock, ackBuffer, MAX_PACKET_SIZE, 0, (sockaddr*)&fromAddr, &fromLen);
            if (recvd >= (ssize_t)HEADER_SIZE) {
                PacketHeader ack;
                memcpy(&ack, ackBuffer, HEADER_SIZE);
                if (ack.type == 3) {
                    // Cumulative ACK: mark all data packets with seqNum less than ack.seqNum as acknowledged.
                    for (size_t i = base; i < packets.size(); i++) {
                        if (packets[i].header.type == 2 && packets[i].header.seqNum < ack.seqNum)
                            packets[i].acked = true;
                    }
                    // Also check END packet
                    if (packets.back().header.seqNum == ack.seqNum)
                        packets.back().acked = true;
                }
                logPacket(logfile, ack);
                // Slide the window forward
                while (base < packets.size() && packets[base].acked)
                    base++;
            }
        } else {
            // Timeout: retransmit all packets in the current window
            for (size_t i = base; i < next; i++) {
                if (!packets[i].acked) {
                    sendto(sock, &packets[i].header, HEADER_SIZE, 0, (sockaddr*)&servAddr, sizeof(servAddr));
                    if (packets[i].header.type == 2 && !packets[i].data.empty())
                        sendto(sock, packets[i].data.data(), packets[i].data.size(), 0, (sockaddr*)&servAddr, sizeof(servAddr));
                    packets[i].sendTime = steady_clock::now();
                    logPacket(logfile, packets[i].header);
                }
            }
        }
    }
    
    close(sock);
    logfile.close();
    return 0;
}
