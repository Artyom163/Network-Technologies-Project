#define _GNU_SOURCE
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

//512 for data, 4 for offset, 2 for checksum, 2 for length of data (512 or less)
#define BUFSIZE 520
#define MULTICAST_GROUP "239.0.0.1"

#define CLEANUP\
	close((sfd));\
	close((fd));

#define CLEANUP2\
	close((sfd2));\
	close((sfd));\
	close((fd));

struct sockaddr_in sock_serv, recvsock, sendsock;

int create_server_socket (int port, char* ipaddr, int bcast)
{
	int l, sfd;
	static int so_broadcast = 1;

	sfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sfd == -1) {
		perror("socket");
		exit(EXIT_FAILURE);
	}

	l = sizeof(struct sockaddr_in);
	bzero(&sock_serv, l);
	
	sock_serv.sin_family = AF_INET;
	sock_serv.sin_port = htons(port);
	sock_serv.sin_addr.s_addr = inet_addr(ipaddr);

	if (bcast == 1) {
		if (setsockopt(sfd, SOL_SOCKET, SO_BROADCAST, &so_broadcast, sizeof(so_broadcast)) == -1) {
			perror("setsockopt");
			close(sfd);
			exit(EXIT_FAILURE);
		}
		if (bind(sfd, (struct sockaddr*)&sock_serv, l) == -1) {
			perror("bind");
			close(sfd);
			exit(EXIT_FAILURE);
		}
	}

	return sfd;
}

int create_recv_socket (int port) {
	int l, sfd;

	sfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sfd == -1) {
		perror("socket");
		exit(EXIT_FAILURE);
	}
    
	l = sizeof(struct sockaddr_in);
	bzero(&recvsock, l);
	
	recvsock.sin_family = AF_INET;
	recvsock.sin_port = htons(port);
	recvsock.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(sfd,(struct sockaddr *)&recvsock, l) == -1) {
		perror("bind");
		close(sfd);
		exit(EXIT_FAILURE);
	}

	return sfd;
}

int create_send_socket (int port) {
	int l, sfd;

	sfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sfd == -1) {
		perror("socket");
		exit(EXIT_FAILURE);
	}
    
	l = sizeof(struct sockaddr_in);
	bzero(&sendsock, l);
	
	sendsock.sin_family = AF_INET;
	sendsock.sin_port = htons(port);
	sendsock.sin_addr.s_addr = recvsock.sin_addr.s_addr;

	return sfd;
}

uint16_t udp_checksum(const void *buff, size_t length)
{
	const uint16_t *buf = buff;
	uint32_t sum;
	size_t len = length;

	// Calculate the sum
	sum = 0;
	while (len > 1) {
		sum += *buf++;
		if (sum & 0x80000000)
			sum = (sum & 0xFFFF) + (sum >> 16);
		len -= 2;
	}

	if (len & 1)
		// Add the padding if the packet length is odd
		sum += *((uint8_t *)buf);

	// Add the pseudo-header
	sum += htons(length);

	// Add the carries
	while (sum >> 16)
		sum = (sum & 0xFFFF) + (sum >> 16);

	// Return the one's complement of sum
	return ((uint16_t)(~sum));
}

int main (int argc, char**argv) {
	int offset_number, sfd, sfd2, fd, missing_piece, bcast, i;
	char buf[BUFSIZE];
	off_t m;
	uint16_t checksum, data_len;
	size_t name_len;
	long int n;
	unsigned int l = sizeof(struct sockaddr_in);
	struct stat buffer;
	char *filename;

	if (argc != 5) {
		printf("Wrong number of arguments.\nUsage : %s <broadcast/multicast> <bc/mc address> <port_serv> <filename>\n", argv[0]);
		return EXIT_FAILURE;
	}

	//broadcast or multicast
	if ((strcmp(argv[1], "b") == 0) || (strcmp(argv[1], "broadcast") == 0)) {
		bcast = 1;
		printf("Broadcast file transfer : %s\n", argv[4]);
	} else if ((strcmp(argv[1], "m") == 0) || (strcmp(argv[1], "multicast") == 0)) {
		bcast = 0;
		printf("Multicast file transfer : %s\n", argv[4]);
	} else {
		printf("Wrong first argument.\nUsage : %s <broadcast/multicast> <bc/mc address> <port_serv> <filename>\n", argv[0]);
		return EXIT_FAILURE;
	}
	
	//creating socket
	sfd = create_server_socket(atoi(argv[3]), argv[5], bcast);

	//reading file
	if ((fd = open(argv[4], O_RDONLY)) == -1) {
		perror("open");
		close(sfd);
		return EXIT_FAILURE;
	}

	//getting file information
	if (stat(argv[4], &buffer) == -1) {
		perror("stat");
		CLEANUP;
		return EXIT_FAILURE;
	}

    	/*send info about the file
	8 bytes for length of the file,
	8 for length of the file name,
	the rest is the file name*/

	//get the name of the file without path
	filename = basename(argv[4]);

	bzero(&buf, BUFSIZE);
	memcpy(&(buf[0]), &(buffer.st_size), 8);
	name_len = strlen(filename);
	memcpy(&(buf[8]), &name_len, 8);
	for (i = 0; i < name_len; i++)
		buf[i + 16] = filename[i];

	m = sendto(sfd, buf, BUFSIZE, 0, (struct sockaddr*)&sock_serv, l);
	if (m == -1) {
		perror("sendto");
		CLEANUP;
		return EXIT_FAILURE;
	}

	//send the file
	bzero(&buf, BUFSIZE);

	n = read(fd, buf, BUFSIZE - 8);
	offset_number = 0;
	while (n) {
		if (n == -1) {
			perror("read");
			CLEANUP;
			return EXIT_FAILURE;
		}
		checksum = (uint16_t)(udp_checksum(buf, n));
		data_len = (uint16_t)n;

		//add hash and offset to buffer
		memcpy(&(buf[512]), &offset_number, 4);
		memcpy(&(buf[516]), &checksum, 2);
		memcpy(&(buf[518]), &data_len, 2);

		m = sendto(sfd, buf, BUFSIZE, 0, (struct sockaddr*)&sock_serv, l);
		if (m == -1) {
			perror("sendto");
			CLEANUP;
			return EXIT_FAILURE;
		}
		bzero(buf, BUFSIZE);
		n = read(fd, buf, BUFSIZE - 8);
		offset_number ++;
	}

	if (sendto(sfd, buf, 0, 0, (struct sockaddr*)&sock_serv, l) == -1) {
		perror("sendto");
		CLEANUP;
		return EXIT_FAILURE;
	}
	
	bzero(buf, BUFSIZE);
	sfd2 = create_recv_socket(8081);
	m = recvfrom(sfd2, &buf, BUFSIZE, 0, (struct sockaddr *)&recvsock, &l);
	while (m) {
		if (m == -1) {
			perror("recvfrom");
			CLEANUP2;
			return EXIT_FAILURE;
		} else if (m == 4) {
			memcpy(&missing_piece, buf, 4);
			if (lseek(fd, missing_piece * 512,SEEK_SET) == -1) {
				perror("lseek");
				CLEANUP2;
				return EXIT_FAILURE;
			}
			bzero(buf, BUFSIZE);
			n = read(fd, buf, BUFSIZE - 8);
			if (n == -1) {
				perror("read");
				CLEANUP2;
				return EXIT_FAILURE;
			}
			checksum = udp_checksum(buf, BUFSIZE - 8);
			data_len = (uint16_t)n;
			
			memcpy(&(buf[512]), &missing_piece, 4);
			memcpy(&(buf[516]), &checksum, 2);
			memcpy(&(buf[518]), &data_len, 2);
			
			m = sendto(sfd2, buf, BUFSIZE, 0, (struct sockaddr*)&recvsock, l);
			if (m == -1) {
				perror("sendto");
				CLEANUP2;
				return EXIT_FAILURE;
			}

			bzero(buf, BUFSIZE);
			m = recvfrom(sfd2, &buf, BUFSIZE, 0, (struct sockaddr *)&recvsock, &l);
		} else {
			printf("Error : bad packet received\n");
			CLEANUP2;
			return EXIT_FAILURE;
		}
	}
    
	printf("File transfer completed successfully\n");

	CLEANUP;
	return EXIT_SUCCESS;
}
