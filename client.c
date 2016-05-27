#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
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
	free((name));\
	free((pieces));\
	close((sfd));\
	close((fd));

#define CLEANUP2\
	free((name));\
	free((pieces));\
	close((sfd));\
	close((sfd2));\
	close((fd));

struct sockaddr_in sock_clt, sendsock, recvsock;

int create_client_socket (int port, char* ipaddr, int bcast) {
	int l, sfd;
	struct ip_mreq mreq;

	sfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sfd == -1) {
		perror("socket");
		exit(EXIT_FAILURE);
	}
    
	l = sizeof(struct sockaddr_in);
	bzero(&sock_clt, l);

	if (bcast == 0)	 {
		sock_clt.sin_family = AF_INET;
		sock_clt.sin_port = htons(port);
		sock_clt.sin_addr.s_addr = htonl(INADDR_ANY);
		if (bind(sfd, (struct sockaddr*)&sock_clt, l) == -1) {
			perror("bind");
			exit(EXIT_FAILURE);
		}
		mreq.imr_multiaddr.s_addr = inet_addr(ipaddr);
		mreq.imr_interface.s_addr = htonl(INADDR_ANY);   
		if (setsockopt(sfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
			perror("setsockopt mreq");
			exit(EXIT_FAILURE);
		}
	} else if (bcast == 1) {
		static int so_reuseaddr = 1;
		if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &so_reuseaddr, sizeof so_reuseaddr) == -1) {
			perror("setsockopt");
			exit(EXIT_FAILURE);
		}
		sock_clt.sin_family = AF_INET;
		sock_clt.sin_addr.s_addr = htonl(INADDR_ANY);
		sock_clt.sin_port = htons(port);
		if (bind(sfd, (struct sockaddr*)&sock_clt, l) == -1) {
			perror("bind");
			exit(EXIT_FAILURE);
		}
	}

	return sfd;
}

int create_recv_socket (int port, char* ipaddr) {
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
	recvsock.sin_addr.s_addr = inet_addr(ipaddr);

	if (bind(sfd,(struct sockaddr *)&recvsock, l) == -1) {
		perror("bind");
		close(sfd);
		exit(EXIT_FAILURE);
	}

	return sfd;
}

int create_send_socket (int port, char* ipaddr) {
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
	sendsock.sin_addr.s_addr = inet_addr(ipaddr);
	return sfd;
}

uint16_t udp_checksum(const void *buff, size_t length) {
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
	int fd, sfd, sfd2, bcast == -1, port, offset_number;
	char buf[BUFSIZE];
	off_t n;
	unsigned int l = sizeof(struct sockaddr_in);
	uint16_t checksum, data_len;

	if (argc != 4 || argc!= 5) {
		printf("Wrong number of arguments.\nUsage : %s <broadcast/multicast> <serv_address> <port_serv> [<multicast_addr>]\n", argv[0]);
		return EXIT_FAILURE;
	}

	//broadcast or multicast
	if ((strcmp(argv[1], "b") == 0) || (strcmp(argv[1], "broadcast") == 0)) {
		bcast = 1;
		if (argc != 4) {
			printf("Wrong number of arguments.\nUsage : %s <broadcast/multicast> <serv_address> <port_serv> [<multicast_addr>]\n", argv[0]);
			return EXIT_FAILURE;
		}
		printf("Broadcast file transfer\n");
	} else if ((strcmp(argv[1], "m") == 0) || (strcmp(argv[1], "multicast") == 0)) {
		bcast = 0;
		if (argc != 5) {
			printf("Wrong number of arguments.\nUsage : %s <broadcast/multicast> <serv_address> <port_serv> [<multicast_addr>]\n", argv[0]);
			return EXIT_FAILURE;
		}
		printf("Multicast file transfer\n");
	} else {
		printf("Wrong first argument.\nUsage : %s <broadcast/multicast> <port_serv>\n", argv[0]);
		return EXIT_FAILURE;
	}
	
	port = atoi(argv[3]);
	sfd = create_client_socket(port, bcast); 

	//receive information about the file
	bzero(&buf, BUFSIZE);
	n = recvfrom(sfd, &buf, BUFSIZE, 0, (struct sockaddr *)&sock_clt, &l);
	if (n == -1) {
		perror("recvfrom");
		return EXIT_FAILURE;
	}
	off_t file_len;
	size_t name_len;
	int num_pieces, i;

	memcpy(&file_len, &buf, 8);
	if (file_len % 512 == 0)
		num_pieces = file_len / 512;
	else
		num_pieces = file_len / 512 + 1;
	memcpy(&name_len, &(buf[8]), 8);
	char *name = (char *)malloc((name_len + 1) * sizeof(char));
	for (i = 0; i < name_len; i++)
		name[i] = buf[i + 16];
	name[name_len] = '\n';

	//create the file
	printf("Creating the output file : %s\nSize : %ld b\n", name, file_len);

	if ((fd = open(name, O_CREAT | O_WRONLY | O_TRUNC, 0600)) == -1) {
		perror("open");
		close(sfd);
		return EXIT_FAILURE;
	}

	int* pieces = (int*)malloc(num_pieces * sizeof(int));
	for (i = 0; i < num_pieces; i++)
		pieces[i] = 0;
	
	//receive the file
	printf("Start downloading\n");
	bzero(&buf, BUFSIZE);
	n = recvfrom(sfd, &buf, BUFSIZE, 0, (struct sockaddr *)&sock_clt, &l);
	while (n) {
		//printf("%lld of data received \n", n);
		if (n == -1) {
			perror("recvfrom");
			CLEANUP;
			return EXIT_FAILURE;
		}
		memcpy(&checksum, &(buf[516]), 2);
		memcpy(&data_len, &(buf[518]), 2);
		if (checksum == udp_checksum(buf, (size_t)data_len)) {
			memcpy(&offset_number, &(buf[512]), 4);
			pieces[offset_number] = 1;
			if (lseek(fd, offset_number * 512,SEEK_SET) == -1) {
				perror("lseek");
				CLEANUP;
				return EXIT_FAILURE;
			}
			write(fd, buf, data_len);
			bzero(buf, BUFSIZE);
			n = recvfrom(sfd, &buf, BUFSIZE, 0, (struct sockaddr *)&sock_clt, &l);
		} else {
			bzero(buf, BUFSIZE);
			n = recvfrom(sfd, &buf, BUFSIZE, 0, (struct sockaddr *)&sock_clt, &l);
		}
	}
	
	//download the missing pieces
	sfd2 = create_send_socket(8081, argv[2]);
	for (i = 0; i < num_pieces; i++)
		if (pieces[i] == 0) {
			//send info about the missing piece
			bzero(buf, BUFSIZE);
			//create message
			memcpy(buf, &i, 4);
			n = sendto(sfd2, buf, 4, 0, (struct sockaddr*)&sendsock, l);
			if (n == -1) {
				perror("sendto");
				CLEANUP2;
				return EXIT_FAILURE;
			}
			//receive the piece
			bzero(buf, BUFSIZE);

			n = recvfrom(sfd2, &buf, BUFSIZE, 0, (struct sockaddr *)&recvsock, &l);
			if (n == -1) {
				perror("recvfrom");
				CLEANUP2;
				return EXIT_FAILURE;
			}
			//add the piece to the file
			checksum = memcpy(&checksum, &(buf[516]), 2);
			if (checksum == udp_checksum(buf, BUFSIZE - 8)) {
				memcpy(&offset_number, &(buf[512]), 4);
				pieces[offset_number] = 1;
				memcpy(&data_len, &(buf[518]), 2);
				if (lseek(fd, offset_number * 512,SEEK_SET) == -1) {
					perror("lseek");
					CLEANUP2;
					return EXIT_FAILURE;
				}
				write(fd, buf, data_len);
			} else {
				printf("Error : some parts of the file weren't received\n");
				CLEANUP2;
				return EXIT_FAILURE;
			}
		}

	bzero(buf, BUFSIZE);
	printf("Download finished.\nPress 'n' if you don't want to stop the server.\nPress any other key to stop it.\n");
	char c;
	scanf("%c", &c);
	if (c != 'n') {
		printf("Stopping the server.\n");
		n = sendto(sfd2, buf, 0, 0, (struct sockaddr*)&sendsock, l);
	}
	
	
	printf("Download finished\n");
	CLEANUP2;
	return EXIT_SUCCESS;
}
