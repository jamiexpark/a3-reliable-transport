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
    
    // Parse command-line arguments
    int opt;
    while ((opt = getopt(argc, argv, "p:w:d:o:")) != -1) {
        switch(opt) {
            case 'p': port = atoi(optarg); break;
            case 'w': windowSize = atoi(optarg); break;
            case 'd': outputDir = optarg; break;
            case 'o': logFile = optarg; break;
            default:
                cerr << "Usage: ./wReceiver -p <port> -w <window-size> -d <output-dir> -o <output-log>\n";
                return 1;
        }
    }
    
    // Create UDP socket and bind
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
    
    // Open log file for writing
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
        
        // Recompute checksum and drop packet if it does not match
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
            continue; // drop packet
        
        // Process packet types
        if (header.type == 0) { // START packet
            if (connectionActive)
                continue; // ignore new START if already in a connection
            connectionActive = true;
            expectedSeq = 0;
            fileBuffer.clear();
            // Send ACK for START (ACK seq = start packet’s seqNum)
            PacketHeader ack;
            ack.type = 3;
            ack.seqNum = header.seqNum;
            ack.length = 0;
            ack.checksum = crc32(&ack, HEADER_SIZE - sizeof(uint32_t));
            sendto(sock, &ack, HEADER_SIZE, 0, (sockaddr*)&fromAddr, fromLen);
            logPacket(logfile, ack);
        } else if (header.type == 2 && connectionActive) { // DATA packet
            if (header.seqNum == expectedSeq) {
                // Accept in-order packet and append its data
                fileBuffer.insert(fileBuffer.end(), buffer + HEADER_SIZE, buffer + HEADER_SIZE + header.length);
                expectedSeq++;
            }
            // Send cumulative ACK (next expected seq)
            PacketHeader ack;
            ack.type = 3;
            ack.seqNum = expectedSeq;
            ack.length = 0;
            ack.checksum = crc32(&ack, HEADER_SIZE - sizeof(uint32_t));
            sendto(sock, &ack, HEADER_SIZE, 0, (sockaddr*)&fromAddr, fromLen);
            logPacket(logfile, ack);
        } else if (header.type == 1 && connectionActive) { // END packet
            // Send ACK for END packet (ACK seq = same as END packet’s seqNum)
            PacketHeader ack;
            ack.type = 3;
            ack.seqNum = header.seqNum;
            ack.length = 0;
            ack.checksum = crc32(&ack, HEADER_SIZE - sizeof(uint32_t));
            sendto(sock, &ack, HEADER_SIZE, 0, (sockaddr*)&fromAddr, fromLen);
            logPacket(logfile, ack);
            // Write the received file to disk
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
