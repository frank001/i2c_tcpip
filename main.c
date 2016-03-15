
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <linux/i2c-dev.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#define MAXBYTES 1024
unsigned char isWebSocket = 0;
int sockfd, newsockfd, portno;


void error(const char *msg) {
	perror(msg);
	exit(1);
}

int i2c_open(const char *bus, int mode) {
	int i2c_handle = open(bus, mode);
	if (i2c_handle <0) error("Error opening i2c");
	return i2c_handle;
}

int i2c_close(int handle) {
	close(handle);
	return 0;
}

int i2c_ioctl(int handle, int slave) {
	return ioctl(handle, I2C_SLAVE, slave);
}


void printData(char const *title, char *data, int bytes) {
	printf("\n%s:-------------------------\n", title);
	for (int i = 0; i < bytes; i++) {
		printf("%02x", (unsigned int)(data[i]));
		if (i % 4 == 3) printf(" ");    // groups of 8 makes more readable
		if (i % 32 == 31) printf("\n"); // lines of 32 bytes
	}
	printf("\n----------------------------- (%d bytes)\n", bytes);

}

int tcpip_read(char *buffer, int bytes) {
	int n;
	bzero(buffer, bytes);
	n = read(newsockfd, buffer, bytes);
	if (isWebSocket) 
		n = (buffer[1] & 0x7f) + 6;								//we switched protocols, read packet length
	printData("Socket read", buffer, n);
	return n;
}

void tcpip_write(char *buffer, int bytes) {
	int n;
	n = write(newsockfd, buffer, bytes);
	if (n < 0) error("ERROR writing to socket");
	printf("writing %d bytes to tcpip\n", bytes);
}



void i2c_write(int handle, char *buffer, int bytes) {
	ssize_t sz;
	char data[MAXBYTES];
	data[0] = 0;												//first byte is start address;
	for (int i = 0; i < bytes; i++) {
		data[i + 1] = buffer[i];
	}
	sz = write(handle, data, bytes + 1);						//write the data
	printf("writing %d bytes to i2c\n", bytes);
}


int i2c_read(int handle, char *data, int bytes) {
	ssize_t sz;
	char d[2];
	int byteCount;
	char crlf;

	bzero(d, 2);
	sz = write(handle, d, 1);									//set i2c_reg_addr to 0x00
	if (sz < 0) error("ERROR2 writing i2c_reg_addr");
	sz = read(handle, data, bytes);								//read i2c_reg_map
	if (sz < 0) error("ERROR reading i2c_reg_map");
	if (!isWebSocket) {
		for (int i = 0; i < bytes; i++) {						//we still need to comply with HTTP, so look for CRLF CRLF
			if (data[i] == 0x0a || data[i] == 0x0d) {
				crlf++;
				if (crlf == 4) {
					byteCount = i+1;
					break;
				}
			}
			else if (crlf > 0) crlf = 0;
		}	

	}
	else {														
		byteCount = data[1]+2;									//we switched protocols, set packet length
	}
	printData("I2C Data", data, byteCount);
	return byteCount;
}





int main(int argc, char *argv[]) {
	socklen_t clilen;
	char buffer[MAXBYTES];
	struct sockaddr_in serv_addr, cli_addr;
	unsigned char i2c_address = 0x10;
	unsigned char isSocketConnected = 0;
	unsigned int byteCount = 0;
	char data[MAXBYTES];

	//set up the socket
	if (argc < 2) {
		fprintf(stderr, "ERROR, no port provided\n");
		exit(1);
	}
	
	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0)  error("ERROR opening socket");
	
	fcntl(sockfd, F_SETFL, O_NONBLOCK);							//make the socket non-blocking

	bzero((char *)&serv_addr, sizeof(serv_addr));
	portno = atoi(argv[1]);
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = INADDR_ANY;
	serv_addr.sin_port = htons(portno);
	if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) error("ERROR on binding");

	while (1) {
		listen(sockfd, 5);										//listen for a connection
		clilen = sizeof(cli_addr);
		newsockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen);
		if (newsockfd>0) {										//we have a client, start i2c
			
			int i2c = open("/dev/i2c-1", O_RDWR);				//i2c init
			if (i2c<0) error("ERROR opening i2c");
			if (i2c_ioctl(i2c, i2c_address)<0) error("ERROR ioctl(I2C_SLAVE)");

			isSocketConnected = 1;
			bzero(buffer, MAXBYTES);

			while (isSocketConnected) {

				byteCount = tcpip_read(buffer, MAXBYTES-1);		//lets see if there´s some data

				if (byteCount) { 								//if so, lets pass it to i2c
					i2c_write(i2c, buffer, byteCount);
					if (isWebSocket && (buffer[0] == 0x88)) {	//the client wants to disconnect
						isSocketConnected = 0;
						break;
					}
				}
				
				bzero(data, 1024);
				byteCount = i2c_read(i2c, data, 255);
				if (data[0]) {									//we have new data from i2c, pass it to tcp_ip.
					tcpip_write(data, byteCount);
					if (!isWebSocket) isWebSocket = 1;			//at this point we should be connected
				}
				usleep(10 * 1000);								//free up some ticks on the rpi
			}
			i2c_close(i2c);										//the client is gone. no need to keep i2c open
			isWebSocket = 0;
		}
		usleep(10 * 1000);										//no connection from tcpip, lets rest a bit
	}
}



