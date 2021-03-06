#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

#include <sys/types.h>
#include <sys/socket.h>

#include "libnetfiles.h"


/////////////////////////////////////////////////////////////
//
// A structure defined to store information
// for the current net file server used.  
// This information is used by the client to
// maintain knowledge of which server it is
// current talking to.  The netserverinit
// function initializes this structure.
// All subsequent net function calls will be 
// directed to this net file server until it 
// is changed by another netserverinit call.  
// If the hostname variable is a null string, 
// that means the client has never called the
// netserverinit function to set up a server.
//
/////////////////////////////////////////////////////////////


typedef struct {
    char hostname[64];
    FILE_CONNECTION_MODE fcMode;
} NET_SERVER;



typedef struct {
    int port;
    int netfd;
    int seqNum;
    char *buf;
    int iStartPos;
    int iLength;
} FILE_PART_TYPE;



/////////////////////////////////////////////////////////////
//
// Function declarations 
//
/////////////////////////////////////////////////////////////

int     getSockfd( const char * hostname, const int port );

int     isNetServerInitialized( NET_FUNCTION_TYPE iFunc );

int     xferStrategy(NET_FUNCTION_TYPE netFunc, const int netfd, 
                     char *buf,   int nBytes, 
                     const int portCount, int *ports);

void    *sendData( void *filePart);  // thread for netwrite
void    *getData(  void *filePart);  // thread for netread




/////////////////////////////////////////////////////////////
//
// Declare global variables
//
/////////////////////////////////////////////////////////////

NET_SERVER gNetServer;



/////////////////////////////////////////////////////////////


int getSockfd( const char * hostname, const int port )
{
    int sockfd = 0;

    struct sockaddr_in serv_addr;
    struct hostent *server = NULL;


    //
    // Create a new socket 
    //
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        fprintf(stderr,"libnetfiles: socket() failed, errno= %d\n", errno);
	return -1;
    }
  
    //
    // Find the address of the given server by name
    //
    server = gethostbyname(hostname);
    if (server == NULL) {
        errno = 0;
        h_errno = HOST_NOT_FOUND;
        //fprintf(stderr,"libnetfiles: host not found, h_errno= %d\n", h_errno);
	return -1;
    }

    //
    // Initialize the server address structure.  This 
    // structure is used to do the actual connect.
    //
    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;

    bcopy((char *)server->h_addr_list[0], 
         (char *)&serv_addr.sin_addr.s_addr,
         server->h_length);

    if (port < 0 ) {
       serv_addr.sin_port = htons(NET_SERVER_PORT_NUM);
    } 
    else {
       serv_addr.sin_port = htons(port);
    }


    if (connect(sockfd,(struct sockaddr *)&serv_addr,sizeof(serv_addr)) < 0) 
    {
        fprintf(stderr,"libnetfiles: cannot connect to %s, h_errno= %d\n", 
                hostname, h_errno);
	return -1;
    }

    //printf("client getSockfd: sockfd %d connected to port %d\n", sockfd);
    return sockfd;
}


/////////////////////////////////////////////////////////////


int isNetServerInitialized( NET_FUNCTION_TYPE iFunc )
{
    if ( (strcmp(gNetServer.hostname, "") == 0) ||
         (gNetServer.fcMode <= 0 ) )
    {
        switch (iFunc) {
            case NET_OPEN:
                //fprintf(stderr,"net server not initialized before calling netopen\n");
                break;

            case NET_READ:
                //fprintf(stderr,"net server not initialized before calling netread\n");
                break;

            case NET_WRITE:
                //fprintf(stderr,"net server not initialized before calling netwrite\n");
                break;

            case NET_CLOSE:
                //fprintf(stderr,"net server not initialized before calling netclose\n");
                break;

            default:
                break;
        }
        return FALSE;
    }

    return TRUE;
}

/////////////////////////////////////////////////////////////


/*******************************************************

  netserverinit needs to handle these error codes

       Required:

       Optional:

       Implemented h_errno:
           HOST_NOT_FOUND    =  1, host not found
           INVALID_FILE_MODE = 99, invalid file connection mode

       Implemented errno:
           EINVAL = 22, Invalid argument
           ECOMM  = 70, Communication error on send

******************************************************/
int netserverinit(char *hostname, int filemode)
{
    int rc = 0;
    int sockfd = -1;
    char msg[MSG_SIZE] = "";

    //
    // Clear errno and h_errno
    //
    errno = 0;
    h_errno = 0;


    //
    // Remove current net file server name 
    // and file connection mode setting.
    //
    strcpy(gNetServer.hostname, "");
    gNetServer.fcMode = INVALID_FILE_MODE;

    //
    // Verify the given file connection mode is valid
    //
    switch (filemode) {
        case UNRESTRICTED_MODE:
        case EXCLUSIVE_MODE:   
        case TRANSACTION_MODE:   
            break;

        default:
            h_errno = INVALID_FILE_MODE;
            fprintf(stderr, "netserverinit: invalid file connection mode\n");
            return FAILURE;
            break;
    }


    //
    // Verify given hostname
    //
    if ( hostname == NULL ) {
        errno = EINVAL;  // 22 = Invalid argument
        return FAILURE;
    };

    if ( strcmp(hostname,"") == 0 ) {
        errno = EINVAL;  // 22 = Invalid argument
        return FAILURE;
    };


    //
    // Get a socket to talk to my net file server
    //
    sockfd = getSockfd( hostname, NET_SERVER_PORT_NUM );
    if ( sockfd < 0 ) {
        errno = 0;
        h_errno = HOST_NOT_FOUND;
        //fprintf(stderr, "netserverinit: host not found, %s\n", hostname);
        //fprintf(stderr, "netserverinit: errno= %d, %s\n", errno, strerror(errno));
        //fprintf(stderr, "netserverinit: h_errno= %d, %s\n", h_errno, strerror(h_errno));
        return FAILURE;
    }
    //printf("netserverinit: sockfd= %d\n", sockfd);


    // 
    // Compose my net command to send to the server.  The format is:
    //
    //     netCmd,0,0,0
    //
    bzero(msg, MSG_SIZE);
    sprintf(msg, "%d,0,0,0", NET_SERVERINIT);


    //printf("netserverinit: send to server - \"%s\"\n", msg);
    rc = write(sockfd, msg, strlen(msg));
    if ( rc < 0 ) {
        // Failed to write command to server
        h_errno = ECOMM;  // 70 = Communication error on send
        //fprintf(stderr, "netserverinit: failed to write cmd to server.  rc= %d\n", rc);
        return FAILURE;
    }


    // 
    // Read the net response coming back from the server. 
    // The response message format is:
    //
    //   result,0,0,0
    //
    bzero(msg, MSG_SIZE);
    rc = read(sockfd, msg, MSG_SIZE -1);
    if ( rc < 0 ) {
        h_errno = ECOMM;  // 70 = Communication error on send
        //fprintf(stderr,"netserverinit: fails to read from socket\n");
        if ( sockfd != 0 ) close(sockfd);
        return FAILURE;
    }

    close(sockfd);  // Don't need this socket anymore

    //
    // Received a response back from the server
    //
    //printf("netserverinit: received from server - \"%s\"\n", msg);


    // Decode the response from the server
    sscanf(msg, "%d,", &rc);
    if ( rc == SUCCESS ) {
        //
        // Save the hostname of the net server.  All subsequent
        // network function calls will go this this net server.
        //
        strcpy(gNetServer.hostname, hostname);
        gNetServer.fcMode = (FILE_CONNECTION_MODE)filemode;

        //printf("netserverinit: netServerName= %s, connection mode= %d\n", 
        //         gNetServer.hostname, gNetServer.fcMode);
        //printf("netserverinit: server responded with SUCCESS\n");
    }

    return rc;
}


/////////////////////////////////////////////////////////////


/*******************************************************

  netopen needs to handle these error codes

       Required:
           EINTR  =  4, interrupted system call
           EROFS  = 30, Read-only file system

       Optional:
           EWOULDBLOCK = 11, Operation would block

       Implemented:
           EPERM  =  1, Operation not permitted
           ENOENT =  2, No such file or directory
           EACCES = 13, Permission denied
           EISDIR = 21, Is a directory
           EINVAL = 22, Invalid argument
           ENFILE = 23, File table overflow

******************************************************/

int netopen(const char *pathname, int flags)
{
    int netFd  = -1;
    int sockfd = -1;
    int rc     = 0;
    char msg[MSG_SIZE] = "";


    //
    // Clear errno and h_errno
    //
    errno = 0;
    h_errno = 0;

    // Check the given pathname
    if (pathname == NULL) {
        //fprintf(stderr,"netopen: pathname is NULL\n");
        errno = EINVAL;  // 22 = Invalid argument
        return FAILURE;
    } 

    if (strcmp(pathname,"") == 0) {
        //fprintf(stderr,"netopen: pathname is blank\n");
        errno = EINVAL;  // 22 = Invalid argument
        return FAILURE;
    } 
    //printf("netopen: pathname= %s, flags= %d\n", pathname, flags);

    // Check to given flags
    switch (flags) {
        case O_RDONLY:
        case O_WRONLY:
        case O_RDWR:
            // allowable file open flag
            break;
  
        default:
            // file open flag not supported
            //fprintf(stderr,"netopen: unsupported file open flags, %d\n",flags);
            errno = EINVAL;  // 22 = Invalid argument
            return FAILURE;
    } 
       

    if ( isNetServerInitialized( NET_OPEN ) != TRUE ) {
        errno = EPERM;  // 1 = Operation not permitted
        return FAILURE;
    }


    //
    // Get a socket to talk to my net file server
    //
    sockfd = getSockfd( gNetServer.hostname, NET_SERVER_PORT_NUM );
    if ( sockfd < 0 ) {
        // this error should not happen
        errno = 0;
        h_errno = HOST_NOT_FOUND;
        //fprintf(stderr, "netopen: host not found, %s\n", hostname);
        return FAILURE;
    }
    //printf("netopen: sockfd= %d\n", sockfd);


    // 
    // Compose my net command to send to the server.  The format is:
    //
    //     netCmd,connectionMode,fileOpenFlags,pathname
    //
    bzero(msg, MSG_SIZE);
    sprintf(msg, "%d,%d,%d,%s", NET_OPEN, gNetServer.fcMode, flags, pathname);
    //printf("netopen: send to server - \"%s\"\n", msg);

    rc = write(sockfd, msg, strlen(msg));
    if ( rc < 0 ) {
        // Failed to write command to server
        fprintf(stderr, "netopen: failed to write cmd to server.  rc= %d\n", rc);
        return FAILURE;
    }


    // 
    // Read the net response coming back from the server.
    // The response msg format is:
    //
    //    result,errno,h_errno,netFd
    //
    bzero(msg, MSG_SIZE);
    rc = read(sockfd, msg, MSG_SIZE -1);
    if ( rc < 0 ) {
        if ( sockfd != 0 ) close(sockfd);
        return FAILURE;
    }

    close(sockfd);  // Don't need this socket anymore

    //
    // Received a response back from the server
    //
    //printf("netopen: received from server - \"%s\"\n", msg);


    // Decode the response from the server
    sscanf(msg, "%d,%d,%d,%d", &rc, &errno, &h_errno, &netFd);
    if ( rc == FAILURE ) {
        //printf("netopen: server returns FAILURE, errno= %d (%s), h_errno=%d\n",
        //          errno, strerror(errno), h_errno);
        return FAILURE;
    }

    return netFd;
}

/////////////////////////////////////////////////////////////


/*******************************************************

  netclose needs to handle these error codes

       Required:

       Optional:
           none

       Implemented:
           EPERM  = 1, operation not permitted
           EABDF  =  9, Bad file number

******************************************************/

int netclose(int netFd)
{
    int fd = -1;
    int sockfd = -1;
    int rc     = 0;
    char msg[MSG_SIZE] = "";


    //
    // Clear errno and h_errno
    //
    errno = 0;
    h_errno = 0;


    if ( isNetServerInitialized( NET_CLOSE ) != TRUE ) {
        errno = EPERM;  // 1 = Operation not permitted
        return FAILURE;
    }


    //
    // Get a socket to talk to my net file server
    //
    sockfd = getSockfd( gNetServer.hostname, NET_SERVER_PORT_NUM );
    if ( sockfd < 0 ) {
        // this error should not happen
        errno = 0;
        h_errno = HOST_NOT_FOUND;
        //fprintf(stderr, "netclose: host not found, %s\n", hostname);
        return FAILURE;
    }
    //printf("netclose: sockfd= %d\n", sockfd);


    // 
    // Compose my net command to send to the server.  The format is:
    //
    //     netCmd,fd,0,0
    //
    bzero(msg, MSG_SIZE);
    sprintf(msg, "%d,%d,0,0", NET_CLOSE, netFd);


    //printf("netclose: send to server - \"%s\"\n", msg);
    rc = write(sockfd, msg, strlen(msg));
    if ( rc < 0 ) {
        // Failed to write command to server
        fprintf(stderr, "netclose: failed to write cmd to server.  rc= %d\n", rc);
        return FAILURE;
    }


    // 
    // Read the net response coming back from the server.
    // The response msg format is:
    //
    //    result,errno,h_errno,netFd
    //
    bzero(msg, MSG_SIZE);
    rc = read(sockfd, msg, MSG_SIZE -1);
    if ( rc < 0 ) {
        if ( sockfd != 0 ) close(sockfd);
        return FAILURE;
    }

    close(sockfd);  // Don't need this socket anymore

    //
    // Received a response back from the server
    //
    //printf("netclose: received from server - \"%s\"\n", msg);


    // Decode the response from the server
    sscanf(msg, "%d,%d,%d,%d", &rc, &errno, &h_errno, &fd);
    if ( rc == FAILURE ) {
        //fprintf(stderr, "netclose: server returns FAILURE, errno= %d (%s), h_errno=%d\n",
        //          errno, strerror(errno), h_errno);
        errno = EBADF;
        return FAILURE;
    }

    return SUCCESS;
}

/////////////////////////////////////////////////////////////


/*******************************************************

  netwrite needs to handle these error codes

       Required
           ETIMEOUT   = 110, connection timed out 

       Optional:
	   none

       Implemented:
           EPERM      =  1, Operation not permitted
	   EBADF      =  9, Bad file number
	   EACCES     = 14, Permission denied
	   EINVAL     = 22, Invalid argument
	   ECONNRESET = 104, connection reset by peer

******************************************************/

ssize_t netwrite(int netfd, const void *buf, size_t nbyte)
{
    int fd     = -1;
    int sockfd = -1;
    int rc     =  0;
    char msg[MSG_SIZE] = "";


    //
    // Clear errno and h_errno
    //
    errno = 0;
    h_errno = 0;


    //
    // Check input parameters
    //
    if ((buf == NULL) || (nbyte < 0)) {
	errno = EINVAL;  // 22 = Invalid argument
	return FAILURE;
    }


    if ( isNetServerInitialized( NET_CLOSE ) != TRUE ) {
        errno = EPERM;  // 1 = Operation not permitted
        return FAILURE;
    }


    //
    // Get a socket to talk to my net file server
    //
    sockfd = getSockfd( gNetServer.hostname, NET_SERVER_PORT_NUM );
    if ( sockfd < 0 ) {
        // this error should not happen
        errno = 0;
        h_errno = HOST_NOT_FOUND;
        //fprintf(stderr, "netwrite: host not found, %s\n", hostname);
        return FAILURE;
    }
    //printf("client netwrite: sockfd= %d\n", sockfd);


    // 
    // Compose my net command to send to the server.  The format is:
    //
    //     netCmd,netFd,nbytes,0
    //
    bzero(msg, MSG_SIZE);
    sprintf(msg, "%d,%d,%d,0", NET_WRITE, netfd, (int)nbyte);

    //printf("client netwrite: send to server - \"%s\"\n", msg);
    rc = write(sockfd, msg, strlen(msg));
    if ( rc < 0 ) {
        // Failed to write command to server
        fprintf(stderr, "netwrite: failed to write cmd to server.  rc= %d\n", rc);
        return FAILURE;
    }

    //printf("client netwrite: waiting for response from server...\n");

    // 
    // Read the net response coming back from the server.
    // This is a configuration message from the server.
    //
    // The response msg format is:
    //
    //    result,errno,h_errno,netFd,
    //    portCount,
    //    portNum, portNum, portNum,....
    //    
    //
    bzero(msg, MSG_SIZE);
    rc = read(sockfd, msg, MSG_SIZE -1);
    if ( rc < 0 ) {
        if ( sockfd != 0 ) close(sockfd);
        return FAILURE;
    }

    //
    // Received a response back from the server
    //
    //printf("client netwrite: received %d-byte msg from server - \"%s\"\n", rc, msg);


    //
    // Save the given list of ports into an array
    //
    int portCount = 0;
    sscanf(msg, "%d,%d,%d,%d,%d,", &rc, &errno, &h_errno, &fd, &portCount);
    //printf("client netwrite: received portCount= %d\n", portCount);

    if ( portCount > 0 ) {
        int ports[MAX_FILE_TRANSFER_SOCKETS]; 
        
        int i = 0;  // Token counter
        char* token;
        for (token = strtok(msg, ","); token != NULL; token = strtok(NULL, ","))
        {
            if ( i > 4 ) {
                ports[i-5] = atoi(token);
                //printf("client netwrite: received ports[%d]= %d\n", i-5, ports[i-5]);
            }
            i++;
        }
    
        //
        // At this point, the server has given me "portCount" ports to 
        // use to transmit my "nbyte" of data.  Now I need to decide
        // how many bytes to go over each port.
        //
        rc = xferStrategy(NET_WRITE, netfd, (char *)buf, nbyte, portCount, ports);
    }


    // Read the final response from the server
    bzero(msg, MSG_SIZE);
    rc = read(sockfd, msg, MSG_SIZE -1);
    if ( rc < 0 ) {
        if ( sockfd != 0 ) close(sockfd);
        return FAILURE;
    }

    close(sockfd);  // Don't need this socket anymore

    //
    // Received a response back from the server
    //
    //printf("client netwrite: received %d-byte msg from server - \"%s\"\n", rc, msg);


    long iBytesWritten = 0;
    sscanf(msg, "%d,%d,%d,%ld", &rc, &errno, &h_errno, &iBytesWritten);
    if ( rc == FAILURE ) {
        fprintf(stderr, "client netwrite: server returns FAILURE, errno= %d (%s), h_errno=%d\n",
                  errno, strerror(errno), h_errno);
        return FAILURE;
    }

    return iBytesWritten;
}

/////////////////////////////////////////////////////////////


ssize_t netread(int netfd, void *buf, size_t nbyte)
{
    int fd     = -1;
    int sockfd = -1;
    int rc     = 0;
    char msg[MSG_SIZE] = "";


    //
    // Clear errno and h_errno
    //
    errno = 0;
    h_errno = 0;


    //
    // Check input parameters
    //
    if ((buf == NULL) || (nbyte < 0)) {
	errno = EINVAL;  // 22 = Invalid argument
	return FAILURE;
    }
    bzero(buf,nbyte);


    if ( isNetServerInitialized( NET_CLOSE ) != TRUE ) {
        errno = EPERM;  // 1 = Operation not permitted
        return FAILURE;
    }


    //
    // Get a socket to talk to my net file server
    //
    sockfd = getSockfd( gNetServer.hostname, NET_SERVER_PORT_NUM );
    if ( sockfd < 0 ) {
        // this error should not happen
        errno = 0;
        h_errno = HOST_NOT_FOUND;
        //fprintf(stderr, "netread: host not found, %s\n", hostname);
        return FAILURE;
    }
    //printf("client netread: sockfd= %d\n", sockfd);


    // 
    // Compose my net command to send to the server.  The format is:
    //
    //     netCmd,netFd,nBytesWant,0
    //
    int nBytesWant = (int)nbyte;
    bzero(msg, MSG_SIZE);
    sprintf(msg, "%d,%d,%d,0", NET_READ, netfd, nBytesWant);

    //printf("client netread: send to server - \"%s\"\n", msg);
    rc = write(sockfd, msg, strlen(msg));
    if ( rc < 0 ) {
        // Failed to write command to server
        fprintf(stderr, "netread: failed to write cmd to server.  rc= %d\n", rc);
        return FAILURE;
    }

    //printf("client netread: waiting for response from server...\n");

    // 
    // Read the net response coming back from the server.
    // This is a configuration message from the server.
    //
    // The response msg format is:
    //
    //    result,errno,h_errno,netFd,
    //    nFleSize, portCount,
    //    portNum, portNum, portNum,....
    //    
    //
    bzero(msg, MSG_SIZE);
    rc = read(sockfd, msg, MSG_SIZE -1);
    if ( rc < 0 ) {
        if ( sockfd != 0 ) close(sockfd);
        return FAILURE;
    }

    //
    // Received a response back from the server
    //
    //printf("client netread: received %d-byte msg from server - \"%s\"\n", rc, msg);


    //
    // Save the given list of ports into an array
    //
    int fileSize = 0;
    int portCount = 0;
    sscanf(msg, "%d,%d,%d,%d,%d,%d,", &rc, &errno, &h_errno, &fd, &fileSize, &portCount);
    //printf("client netread: received fileSize= %d, portCount= %d\n", fileSize, portCount);

    if ( portCount > 0 ) {
        int ports[MAX_FILE_TRANSFER_SOCKETS]; 
        
        int i = 0;  // Token counter
        char* token;
        for (token = strtok(msg, ","); token != NULL; token = strtok(NULL, ","))
        {
            if ( i > 5 ) {
                ports[i-6] = atoi(token);
                //printf("client netread: received ports[%d]= %d\n", i-6, ports[i-6]);
            }
            i++;
        }
    
        //
        // I cannot want to read more bytes than the fileSize.  In that
        // case, I must reduce the number of bytes wanted.  The maximum
        // number of bytes to read from this file is the actual file size.
        //
        // At this point, the server has given me "portCount" ports to 
        // use to receive my "nbyte" of data.  Now I need to decide
        // how many bytes to go over each port.
        //
        if (nBytesWant > fileSize ) nBytesWant = fileSize;
 
        rc = xferStrategy(NET_READ, netfd, (char *)buf, nBytesWant, portCount, ports);
    }


    //
    // Read the final response from the server.  The format is:
    //
    //    resultCode, errno, h_errno, nTotalBytes
    //
    bzero(msg, MSG_SIZE);
    rc = read(sockfd, msg, MSG_SIZE -1);
    if ( rc < 0 ) {
        if ( sockfd != 0 ) close(sockfd);
        return FAILURE;
    }

    close(sockfd);  // Don't need this socket anymore


    //
    // Received a response back from the server 
    //
    //printf("client netread: received %d-byte msg from server - \"%s\"\n", rc, msg);


    long nTotalBytes = 0;
    sscanf(msg, "%d,%d,%d,%ld", &rc, &errno, &h_errno, &nTotalBytes);
    if ( rc == FAILURE ) {
        fprintf(stderr, "client netread: server returns FAILURE, errno= %d (%s), h_errno=%d\n",
                  errno, strerror(errno), h_errno);
        return FAILURE;
    }

    return nTotalBytes;
}

/////////////////////////////////////////////////////////////


int xferStrategy(NET_FUNCTION_TYPE netFunc, const int netfd, 
                 char *buf, int nBytes, 
                 const int portCount, int *ports)
{
    int seqNum;  // file sequence number
    int iStartPos = 0;
    int iRemainingBytes = nBytes;

    pthread_t tids[portCount];

    FILE_PART_TYPE part;

    part.netfd = netfd;
    part.buf = buf;

    for ( seqNum = 1; seqNum <= portCount; seqNum++ ) {
        part.port = ports[seqNum-1];

        part.seqNum = seqNum;

        if ( seqNum == portCount ) {
            //
            // This is the only piece or the last piece
            // in a file sequence.  Send all remaining 
            // bytes in this one port.
            //
            part.iStartPos = iStartPos;
            part.iLength = iRemainingBytes;

        } else {
            part.iStartPos = iStartPos;
            if ( iRemainingBytes >= DATA_CHUNK_SIZE ) {
                part.iLength = DATA_CHUNK_SIZE;
                iRemainingBytes = iRemainingBytes - DATA_CHUNK_SIZE;
            } 
            iStartPos = iStartPos + DATA_CHUNK_SIZE;
        }

        //printf("client xferStrategy: netFunc= %d, part: port= %d, netfd= %d, seqNum= %d, iStartPos= %d, iLength= %d\n",
        //     netFunc, part.port, part.netfd, part.seqNum, part.iStartPos, part.iLength);

        //
        // Spawn a thread to send or receive one part of the data
        //
        FILE_PART_TYPE *partArg = malloc(sizeof(FILE_PART_TYPE));
        partArg->port = part.port;
        partArg->netfd = part.netfd;
        partArg->seqNum = part.seqNum;
        partArg->buf = part.buf;
        partArg->iStartPos = part.iStartPos;
        partArg->iLength = part.iLength;

        if ( netFunc == NET_WRITE ) {
            pthread_create(&tids[seqNum-1], NULL, &sendData, partArg );
        }
        else {
            pthread_create(&tids[seqNum-1], NULL, &getData, partArg );
        }
    }


    // wait for all spawned threads to finish
    int i;
    for (i=0; i < portCount; i++) {
        //printf("client xferStrategy: waiting for thread %d to finish\n", (int)tids[i]);
        
        pthread_join(tids[i], NULL);

        //printf("client xferStrategy: thread %d finished\n", (int)tids[i]);
    }


    //printf("client xferStrategy: buf= %s \n", buf);

    return 0;
}

/////////////////////////////////////////////////////////////

void *sendData( void *filePart )
{
    int rc = FAILURE;
    FILE_PART_TYPE part;


    pthread_detach( pthread_self() );

    part.port      = ((FILE_PART_TYPE *)filePart)->port;
    part.netfd     = ((FILE_PART_TYPE *)filePart)->netfd;
    part.seqNum    = ((FILE_PART_TYPE *)filePart)->seqNum;
    part.buf       = ((FILE_PART_TYPE *)filePart)->buf;
    part.iStartPos = ((FILE_PART_TYPE *)filePart)->iStartPos;
    part.iLength   = ((FILE_PART_TYPE *)filePart)->iLength;

    free(filePart);

    //printf("client netwrite: sendData thread %ld: Part: port= %d, netfd= %d, seqNum= %d, iStartPos= %d, iLength= %d\n",
    //         pthread_self(), part.port, part.netfd, part.seqNum, part.iStartPos, part.iLength);


    // 
    // Create a new socket with the given port number to talk to the server
    //
    int sockfd = -1;
    sockfd = getSockfd( gNetServer.hostname, part.port);
    if ( sockfd < 0 ) {
        // this error should not happen
        errno = 0;
        h_errno = HOST_NOT_FOUND;
        fprintf(stderr, "client netwrite: sendData thread %d: host not found, %s\n", 
                     (int)pthread_self(), gNetServer.hostname);
        rc = FAILURE;
        pthread_exit( &rc );
    }
    //printf("client netwrite: sendData thread %d: sockfd= %d, port= %d\n", 
    //          (int)pthread_self(), sockfd, part.port);



    // 
    // Compose my net command to send to the server.  The format is:
    //
    //     netCmd,netFd,SeqNum,nbytes
    //
    char msg[MSG_SIZE] = "";
    bzero(msg, MSG_SIZE);
    sprintf(msg, "%d,%d,%d,%d", NET_WRITE, part.netfd, part.seqNum, part.iLength);

    //printf("client netwrite: sendData thread %d: send to server - \"%s\"\n",(int)pthread_self(),msg);
    rc = write(sockfd, msg, strlen(msg));
    if ( rc < 0 ) {
        // Failed to write command to server
        fprintf(stderr, "client netwrite: sendData failed to write msg to server.  rc= %d\n", rc);
        if ( sockfd != 0 ) close(sockfd);
        rc = FAILURE;
        pthread_exit( &rc );
    }


    //
    // Read the response message from server.  The format is:
    //     resultCode, errno, h_errno, seqNum, nBytes
    //
    bzero(msg, MSG_SIZE);
    rc = read(sockfd, msg, MSG_SIZE -1);
    if ( rc < 0 ) {
        fprintf(stderr,"client netwrite: sendData thread %d: fails to read from socket\n", 
                    (int)pthread_self());
        if ( sockfd != 0 ) close(sockfd);
        rc = FAILURE;
        pthread_exit( &rc );
    }
    //printf("client netwrite: sendData thread %d: received from server - \"%s\"\n",(int)pthread_self(),msg);



    // 
    // Send nBytes of data to the server
    //

    rc = write(sockfd, &(part.buf[part.iStartPos]), part.iLength);
    if ( rc < 0 ) {
        // Failed to write data to server
        fprintf(stderr, "client netwrite: sendData failed to write data to server.  rc= %d\n", rc);
        if ( sockfd != 0 ) close(sockfd);
        rc = FAILURE;
        pthread_exit( &rc );
    }
    //printf("client netwrite: sendData thread %d: send %d bytes of data to server\n",
    //          (int)pthread_self(),rc);


    //
    // Read the response message from server.  The format is:
    //     resultCode, errno, h_errno, nBytes
    //
    bzero(msg, MSG_SIZE);
    rc = read(sockfd, msg, MSG_SIZE -1);
    if ( rc < 0 ) {
        fprintf(stderr,"client netwrite: sendData thread %d: fails to read from socket\n", 
                    (int)pthread_self());
        if ( sockfd != 0 ) close(sockfd);
        rc = FAILURE;
        pthread_exit( &rc );
    }

    //printf("client netwrite: sendData thread %d: received from server - \"%s\"\n",(int)pthread_self(),msg);

    int nBytes = 0;
    sscanf(msg, "%d,%d,%d,%d", &rc, &errno, &h_errno, &nBytes);
    //printf("client netwrite: endData thread %d: server wrote %d bytes\n",(int)pthread_self(),nBytes);

    if ( sockfd != 0 ) close(sockfd);
    pthread_exit( &nBytes );

}

/////////////////////////////////////////////////////////////


void *getData( void *filePart )
{
    int rc = FAILURE;
    FILE_PART_TYPE part;


    pthread_detach( pthread_self() );

    part.port      = ((FILE_PART_TYPE *)filePart)->port;
    part.netfd     = ((FILE_PART_TYPE *)filePart)->netfd;
    part.seqNum    = ((FILE_PART_TYPE *)filePart)->seqNum;
    part.buf       = ((FILE_PART_TYPE *)filePart)->buf;
    part.iStartPos = ((FILE_PART_TYPE *)filePart)->iStartPos;
    part.iLength   = ((FILE_PART_TYPE *)filePart)->iLength;

    free(filePart);

    //printf("client netread: getData thread %ld: Part: port= %d, netfd= %d, seqNum= %d, iStartPos= %d, iLength= %d\n",
    //         pthread_self(), part.port, part.netfd, part.seqNum, part.iStartPos, part.iLength);


    // 
    // Create a new socket with the given port number to talk to the server
    //
    int sockfd = -1;
    sockfd = getSockfd( gNetServer.hostname, part.port);
    if ( sockfd < 0 ) {
        // this error should not happen
        errno = 0;
        h_errno = HOST_NOT_FOUND;
        fprintf(stderr, "client netread: getData thread %d: host not found, %s\n", 
                     (int)pthread_self(), gNetServer.hostname);
        rc = FAILURE;
        pthread_exit( &rc );
    }
    //printf("client netread: getData thread %d: sockfd= %d, port= %d\n", 
    //          (int)pthread_self(), sockfd, part.port);



    // 
    // Compose my net command to send to the server.  The format is:
    //
    //     netread, netFd, SeqNum, iStartPos, nbytes
    //
    char msg[MSG_SIZE] = "";
    bzero(msg, MSG_SIZE);
    sprintf(msg, "%d,%d,%d,%d,%d", NET_READ, part.netfd, part.seqNum, part.iStartPos, part.iLength);

    //printf("client netread: getData thread %d: send to server - \"%s\"\n",(int)pthread_self(),msg);
    rc = write(sockfd, msg, strlen(msg));
    if ( rc < 0 ) {
        // Failed to write command to server
        fprintf(stderr, "client netread: getData failed to write msg to server.  rc= %d\n", rc);
        if ( sockfd != 0 ) close(sockfd);
        rc = FAILURE;
        pthread_exit( &rc );
    }


    //
    // Read the response message from server.  This should
    // be "part.iLength" bytes of data
    //
    bzero(msg, MSG_SIZE);
    rc = read(sockfd, msg, MSG_SIZE -1);
    if ( rc < 0 ) {
        fprintf(stderr,"client netread: getData thread %d: fails to read from socket\n", 
                    (int)pthread_self());
        if ( sockfd != 0 ) close(sockfd);
        rc = FAILURE;
        pthread_exit( &rc );
    }
    int iBytesRecv = rc;
    //printf("client netread: getData thread %d: received %d bytes of data from server\n",(int)pthread_self(),rc);


    //
    // Return data read from server.  The caller of this "libnetfiles" library
    // gave me a char pointer, "buf", to return data I read from the server.  
    // Here, I will write out the data one byte at a time to "buf".
    //
    //
    //printf("client netread: getData thread %d: seqNum= %d\n",(int)pthread_self(), part.seqNum);
    int k = part.iStartPos;
    int i = 0;
    for (i=0; i < iBytesRecv; i++) {
         part.buf[k] = msg[i];
         //printf("client netread: getData thread %d: buf[%d]=%c\n",(int)pthread_self(), k, part.buf[k]);
         k++;
    }


    // 
    // Send a response msg back to the server.  The format is:
    //
    //     resultCode, errno, h_errno, nBytes
    //
    bzero(msg, MSG_SIZE);
    sprintf(msg, "%d,%d,%d,%d", SUCCESS, errno, h_errno, iBytesRecv);

    //printf("client netread: getData thread %d: send to server - \"%s\"\n",(int)pthread_self(),msg);
    rc = write(sockfd, msg, strlen(msg));
    if ( rc < 0 ) {
        // Failed to write command to server
        fprintf(stderr, "client netread: getData failed to write msg to server.  rc= %d\n", rc);
        if ( sockfd != 0 ) close(sockfd);
        rc = FAILURE;
        pthread_exit( &rc );
    }


    if ( sockfd != 0 ) close(sockfd);
    pthread_exit( &iBytesRecv );
}

/////////////////////////////////////////////////////////////

