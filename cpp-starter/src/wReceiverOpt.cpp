#include "common/Crc32.hpp"
#include "common/PacketHeader.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/stat.h>

using namespace std;

#define MAX_PACKET_SIZE 1472
#define HEADER_SIZE sizeof(PacketHeader)

void logPacket(ofstream &logfile, const PacketHeader &header) {
    logfile << header.type << " " << header.seqNum << " " << header.length << " " << header.checksum << "\n";
    logfile.flush();
}

int main(int argc, char* argv[]) {
    int port = 0, windowSize = 0;
    string outputDir, logFile;
    
    int opt;
    while ((opt = getopt(argc, argv, "p:w:d:o:")) != -1) {
        switch(opt) {
            case 'p': port = atoi(optarg); break;
            case 'w': windowSize = atoi(optarg); break;
            case 'd': outputDir = optarg; break;
            case 'o': logFile = optarg; break;
            default:
                cerr << "Usage: ./wReceiverOpt -p <port> -w <window-size> -d <output-dir> -o <output-log>\n";
                return 1;
        }
    }
    
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }
    sockaddr_in myAddr;
    memset(&myAddr, 0, sizeof(myAddr));
    myAddr.sin_family = AF_INET;
    myAddr.sin_port = htons(port);
    myAddr.sin_addr.s_addr = INADDR_ANY;
    if (::bind(sock, (sockaddr*)&myAddr, sizeof(myAddr)) < 0) {
        perror("bind");
        return 1;
    }
    
    ofstream logfile(logFile);
    if (!logfile) {
        cerr << "Error opening log file\n";
        return 1;
    }
    
    uint32_t expectedSeq = 0;
    vector<char> fileBuffer;
    bool connectionActive = false;
    int fileCount = 0;
    
    while (true) {
        char buffer[MAX_PACKET_SIZE];
        sockaddr_in fromAddr;
        socklen_t fromLen = sizeof(fromAddr);
        ssize_t recvd = recvfrom(sock, buffer, MAX_PACKET_SIZE, 0, (sockaddr*)&fromAddr, &fromLen);
        if (recvd < (ssize_t)HEADER_SIZE)
            continue;
        PacketHeader header;
        memcpy(&header, buffer, HEADER_SIZE);
        logPacket(logfile, header);
        
        uint32_t calcChecksum = 0;
        if (header.type == 2) {
            PacketHeader temp = header;
            temp.checksum = 0;
            calcChecksum = crc32(&temp, HEADER_SIZE) ^ crc32(buffer + HEADER_SIZE, header.length);
        } else {
            PacketHeader temp = header;
            temp.checksum = 0;
            calcChecksum = crc32(&temp, HEADER_SIZE);
        }
        if (calcChecksum != header.checksum)
            continue;
        
        if (header.type == 0) { // START packet
            if (connectionActive)
                continue;
            connectionActive = true;
            expectedSeq = 0;
            fileBuffer.clear();
            // In optimized mode, send ACK with same seqNum as the START packet
            PacketHeader ack;
            ack.type = 3;
            ack.seqNum = header.seqNum;
            ack.length = 0;
            ack.checksum = crc32(&ack, HEADER_SIZE - sizeof(uint32_t));
            sendto(sock, &ack, HEADER_SIZE, 0, (sockaddr*)&fromAddr, fromLen);
            logPacket(logfile, ack);
        } else if (header.type == 2 && connectionActive) { // DATA packet
            // Only accept packets within our window
            if (header.seqNum >= expectedSeq && header.seqNum < expectedSeq + windowSize) {
                // If the packet is the expected one, append data
                if (header.seqNum == expectedSeq) {
                    fileBuffer.insert(fileBuffer.end(), buffer + HEADER_SIZE, buffer + HEADER_SIZE + header.length);
                    expectedSeq++;
                }
                // In optimized mode, send an ACK with the packet’s seqNum
                PacketHeader ack;
                ack.type = 3;
                ack.seqNum = header.seqNum;
                ack.length = 0;
                ack.checksum = crc32(&ack, HEADER_SIZE - sizeof(uint32_t));
                sendto(sock, &ack, HEADER_SIZE, 0, (sockaddr*)&fromAddr, fromLen);
                logPacket(logfile, ack);
            }
        } else if (header.type == 1 && connectionActive) { // END packet
            PacketHeader ack;
            ack.type = 3;
            ack.seqNum = header.seqNum;
            ack.length = 0;
            ack.checksum = crc32(&ack, HEADER_SIZE - sizeof(uint32_t));
            sendto(sock, &ack, HEADER_SIZE, 0, (sockaddr*)&fromAddr, fromLen);
            logPacket(logfile, ack);
            // Write the received data to a file
            string outFilename = outputDir + "/FILE-" + to_string(fileCount++) + ".out";
            ofstream outfile(outFilename, ios::binary);
            outfile.write(fileBuffer.data(), fileBuffer.size());
            outfile.close();
            connectionActive = false;
        }
    }
    
    close(sock);
    logfile.close();
    return 0;
}
