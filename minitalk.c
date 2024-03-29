#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define PORT "2137"   // port we're listening on
#define MAX_USERS 1024
#define MAX_UNAME_LENGTH 64

typedef struct {
    int fd;             // user file descriptor
    int target_fd;      // to whom i'm talking
    char * username;    // my user name
} MapFdUsername;

void mapFdWithUsername(MapFdUsername * map, int fd, char * user) {
    map->fd = fd;
    map->username = user;
}

void mapSetTarget(MapFdUsername * map, int target) {
    map->target_fd = target;
}

void mapDestroy(MapFdUsername * map) {
    map->fd = -1;
    map->target_fd = -1;
    if(map->username != NULL) {
        free(map->username);
    }
    map->username = NULL;
}

int mapGetFirstFree(MapFdUsername maps[]) {
    for(int i = 0; i < MAX_USERS; i++) {
        if(maps[i].fd == -1) {
            return i;
        }
    }
    
    return -1; // none left
}

MapFdUsername * mapGetUserByName(MapFdUsername maps[], char * usr) {
    for(int i = 0; i < MAX_USERS; i++) {
        if(maps[i].fd != -1 && strcmp(maps[i].username, usr) == 0) {
            return &maps[i];
        }
    }

    return NULL; // user not found
}

MapFdUsername * mapGetUserByFd(MapFdUsername maps[], int fd) {
    for(int i = 0; i < MAX_USERS; i++) {
        if(maps[i].fd == fd) {
            return &maps[i];
        }
    }

    return NULL; // user not found
}

void strReplace(char * str, char what, char c) {
    int len = strlen(str);
    while(*str != '\0') {
        if(*str == what) {
            *str = c;
        }
        str++;
    }
}

void mapListUsers(MapFdUsername maps[]) {
    printf("List:\tFD\tUSERNAME\n");
    for(int i = 0; i < MAX_USERS; i++) {
        if(maps[i].username != NULL) {
            printf("\t%i\t%s\n", maps[i].fd, maps[i].username);
        }
    }
}

void mapSendUserList(MapFdUsername maps[], int fd) {
    send(fd, "\nList:\tUSERNAME\n", 16, 0);
    for(int i = 0; i < MAX_USERS; i++) {
        if(maps[i].username != NULL) {
            send(fd, "\t", 2, 0);
            send(fd, maps[i].username, strlen(maps[i].username), 0);
            send(fd, "\n", 2, 0);
        } 
    }
}

void sendToAll(MapFdUsername maps[], char * msg) {
    for(int i = 0; i < MAX_USERS; i++) {
        // ignore server in out
        if(maps[i].fd != -1) {
            send(maps[i].fd, msg, strlen(msg), 0);
        }
    }
}

void sendDisconnectedMsg(MapFdUsername maps[], int from) {
    char * disconnectionmsg = "User you were talking to, disconnected from the server";
    for(int i = 0; i < MAX_USERS; i++) {
        if(maps[i].target_fd == from) {
            maps[i].target_fd = -1;
            send(maps[i].fd, disconnectionmsg, strlen(disconnectionmsg), 0);
            mapSendUserList(maps, maps[i].fd);
        }
    }
}

// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int main(void)
{
    printf("Server listening on PORT: %s\n", PORT);
    fflush(stdout);

    MapFdUsername * map = malloc(sizeof(MapFdUsername) * MAX_USERS);
    for(int i = 0; i < 1024; i++) {
        mapFdWithUsername(&map[i], -1, NULL);
        mapSetTarget(&map[i], -1);
    }
    int current_free_index = 0;
    const char * welcomemsg = "Please enter your login: ";
    const char * selecttargetmsg = "Please enter name of user that you want to talk with: ";
    char * message_buffer = malloc(256 * sizeof(char));
    char * newcoming_username = NULL;
    MapFdUsername * tmp_user = NULL;
    MapFdUsername * tmp_target_user = NULL;

    fd_set master;    // master file descriptor list
    fd_set read_fds;  // temp file descriptor list for select()
    int fdmax;        // maximum file descriptor number

    int listener;     // listening socket descriptor
    int newfd;        // newly accept()ed socket descriptor
    struct sockaddr_storage remoteaddr; // client address
    socklen_t addrlen;

    char buf[256];    // buffer for client data
    int nbytes;

    char remoteIP[INET6_ADDRSTRLEN];

    int yes = 1;        // for setsockopt() SO_REUSEADDR, below
    int i, j, rv;

    struct addrinfo hints, *ai, *p;

    FD_ZERO(&master);    // clear the master and temp sets
    FD_ZERO(&read_fds);

    // get us a socket and bind it
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    if ((rv = getaddrinfo(NULL, PORT, &hints, &ai)) != 0) {
        fprintf(stderr, "selectserver: %s\n", gai_strerror(rv));
        exit(1);
    }
    
    for(p = ai; p != NULL; p = p->ai_next) {
        listener = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (listener < 0) { 
            continue;
        }
        
        // lose the pesky "address already in use" error message
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));

        if (bind(listener, p->ai_addr, p->ai_addrlen) < 0) {
            close(listener);
            continue;
        }

        break;
    }

    // if we got here, it means we didn't get bound
    if (p == NULL) {
        fprintf(stderr, "selectserver: failed to bind\n");
        exit(2);
    }

    freeaddrinfo(ai); // all done with this

    // listen
    if (listen(listener, 10) == -1) {
        perror("listen");
        exit(3);
    }

    // add the listener to the master set
    FD_SET(listener, &master);

    // keep track of the biggest file descriptor
    fdmax = listener; // so far, it's this one

    // main loop
    for(;;) {
        read_fds = master; // copy it
        if (select(fdmax+1, &read_fds, NULL, NULL, NULL) == -1) {
            perror("select");
            exit(4);
        }

        // run through the existing connections looking for data to read
        for(i = 0; i <= fdmax; i++) {
            if (FD_ISSET(i, &read_fds)) { // we got one!!
                if (i == listener) {
                    // handle new connections
                    addrlen = sizeof remoteaddr;
                    newfd = accept(listener,
                        (struct sockaddr *)&remoteaddr,
                        &addrlen);

                    if (newfd == -1) {
                        perror("accept");
                    } else {
                        FD_SET(newfd, &master); // add to master set
                        if (newfd > fdmax) {    // keep track of the max
                            fdmax = newfd;
                        }
 
                        send(newfd, welcomemsg, strlen(welcomemsg), 0);

                        current_free_index = mapGetFirstFree(map);
                        if(current_free_index == -1) {
                            fprintf(stderr, "No space left for next user");
                        }
                        mapFdWithUsername(&map[current_free_index], newfd, NULL);

                        printf("selectserver: new connection from %s on socket %d\n",
                            inet_ntop(remoteaddr.ss_family,
                                get_in_addr((struct sockaddr*)&remoteaddr),
                                remoteIP, INET6_ADDRSTRLEN),
                            newfd);
                    }
                } else {
                    // handle data from a client
                    bzero(buf, sizeof buf);
                    if ((nbytes = recv(i, buf, sizeof buf, 0)) <= 0) {
                        // got error or connection closed by client
                        if (nbytes == 0) {
                            // connection closed
                            printf("selectserver: socket %d hung up\n", i);
                        } else {
                            perror("recv");
                        }
                        close(i); // bye!
                        FD_CLR(i, &master); // remove from master set

                        tmp_user = mapGetUserByFd(map, i);
                        bzero(message_buffer, 256);
                        strcat(message_buffer, "- ");
                        strcat(message_buffer, tmp_user->username);
                        strcat(message_buffer, " has disconnected\n");
                        sendToAll(map, message_buffer);
                        mapDestroy(tmp_user); // remove from UserFd list
                        sendDisconnectedMsg(map, i);
                    } else {
                        tmp_user = mapGetUserByFd(map, i); 
                        if(tmp_user == NULL) {
                            fprintf(stderr, "User[fd: %i] not found in database", i);
                            exit(1);
                        }

                        if(tmp_user->username == NULL) {
                            // user sends his login
                            newcoming_username = malloc(sizeof(char) * MAX_UNAME_LENGTH);
                            strncpy(newcoming_username, buf, MAX_UNAME_LENGTH-1);
                            newcoming_username[MAX_UNAME_LENGTH-1] = '\0';
                            strReplace(newcoming_username, '\r', '\0');
                            strReplace(newcoming_username, '\n', '\0');
                            bzero(message_buffer, 256);
                            strcat(message_buffer, "+ ");
                            strcat(message_buffer, newcoming_username);
                            strcat(message_buffer, " has connected\n");
                            sendToAll(map, message_buffer);
                            mapFdWithUsername(tmp_user, i, newcoming_username);

                            send(i, selecttargetmsg, strlen(selecttargetmsg), 0);
                            mapSendUserList(map, i);

                        } else if(tmp_user->target_fd == -1) {
                            // setup a target user
                            strReplace(buf, '\r', '\0');
                            strReplace(buf, '\n', '\0');
                            mapSetTarget(tmp_user, mapGetUserByName(map, buf)->fd);
                        } else {
                            // we got some data from a client
                            if (FD_ISSET(tmp_user->target_fd, &master)) {
                                // except the listener and ourselves
                                if (j != listener && j != i) {
                                    bzero(message_buffer, 256);
                                    strcat(message_buffer, "[");
                                    strcat(message_buffer, tmp_user->username);
                                    strcat(message_buffer, "] ");
                                    strcat(message_buffer, buf);
                                    if (send(tmp_user->target_fd, message_buffer, strlen(message_buffer), 0) == -1) {
                                        perror("send");
                                    }
                                }
                            }
                        }
                    }
                } // END handle data from client
            } // END got new incoming connection
        } // END looping through file descriptors
    } // END for(;;)--and you thought it would never end!
    
    return 0;
}