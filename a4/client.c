#include <stdio.h>

#include "client.h"


/* Create a new socket that connects to host
 * Waiting for a successful connection
 * Returns 
 * -- sock_fd for success
 * -- -1 for error
 */
int client_sock(char *host, unsigned short port){

    int sock_fd;
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("client: socket");
        return -1;
    }

    // Set the IP and port of the server to connect to.
    struct sockaddr_in server;
    server.sin_family = PF_INET;
    server.sin_port = htons(port);

    struct hostent *hp = gethostbyname(host);
    if (hp == NULL) {
        fprintf(stderr, "client: %s unknown host\n", host);
        return -1;
    }
    server.sin_addr = *((struct in_addr *)hp->h_addr);

    // Connect to server
    if (connect(sock_fd, (struct sockaddr *)&server, sizeof(server)) == -1) {
        perror("client:connect");
        close(sock_fd);
        return -1;
    }

    return sock_fd;
}

/* Construct client request for file/dir at path
 * request is modified to accomodate changes
 * Return 0 on success and -1 on failure
 */
int make_req(const char *client_path, const char *server_path, struct request *req){

    struct stat file_buf;
    if (lstat(client_path, &file_buf)){
        perror("client:lstat");
        return -1;
    }
    strncpy(req->path, server_path, strlen(server_path) + 1);
    // Compute file hash
    FILE *f = fopen(client_path,"r");
    if (f == NULL){
        perror("client:fopen");
        return -1;
    }
    // Populate client_req
    hash(req->hash, f);
    req->mode = file_buf.st_mode;
    req->size = file_buf.st_size;

    if (S_ISDIR(file_buf.st_mode)){
        req->type = REGDIR;
    } else{
        req->type = REGFILE;
    }

    if (fclose(f) != 0){
        perror("fclose"); 
        return -1;
    }

    return 0;
}

/*
 * Sends request struct to sock_fd over 5 read calls
 * In order of
 * -- type
 * -- path
 * -- mode
 * -- hash
 * -- size
 * Return 0 if success otherwise -1 
 */
int send_req(int sock_fd, struct request *req){

    int t = htonl(req->type);
    if(write(sock_fd, &t, sizeof(int)) == -1) {
        perror("client:write");
        return -1; 
    }

    if(write(sock_fd, req->path, MAXPATH) == -1) {
        perror("client:write");
        return -1;
    }

    mode_t m = htons(req->mode);            // mode_t is 16bit int
    if(write(sock_fd, &m, sizeof(mode_t)) == -1) {
        perror("client:write");
        return -1;
    }

    if(write(sock_fd, req->hash, BLOCKSIZE) == -1) {
        perror("client:write");
        return -1;
    }

    int s = htonl(req->size);
    if(write(sock_fd, &s, sizeof(size_t)) == -1) {
        perror("client:write");
        return -1;
    }
    //TODO: rmeove later
    /* printf("type:orig=%d, net=%d, host=%d\n", req->type, htonl(req->type), htonl(ntohl(req->type))); */
    /* printf("mode:orig=%d, net=%d, host=%d\n", req->mode, htonl(req->mode), htonl(ntohl(req->mode))); */
    /* printf("size:orig=%d, net=%d, host=%d\n", req->size, htonl(req->size), htonl(ntohl(req->size))); */
    /* printf("req:orig=%d, net=%d, host=%d\n", req->size, htonl(req->size), htonl(ntohl(req->size))); */
    return 0;
}



/*
 * precondition: req.st_mode yields regular file
 * Sends data specified by req by
 * -- open file at req.path
 * -- write to client socket where nbytes is
 * ---- BUFSIZE if eof is not reached
 * ---- position of EOF if eof is reached
 * Return 0 if success otherwise -1
 */
int send_data(int fd, const char *client_path, struct request *req){

    FILE *f;
    printf("%s", req->path);
    if((f = fopen(client_path, "r")) == NULL){
        perror("client:open");
        return -1;
    }

    int num_read;
    char buffer[BUFSIZE];

    while((num_read = fread(buffer, 1, BUFSIZE, f)) > 0){

        if(ferror(f) != 0){
            fprintf(stderr, "fread error: %s", req->path);
            return -1;
        }

        /* printf("buf = [%s] num_read = [%d] nbytes = [%d]\n", buffer, num_read, nbytes); */ // TODO: remove print

        if(write(fd, buffer, num_read) == -1){
            perror("client:write");
            return -1;
        }
    }

    if(fclose(f) != 0){
        perror("client:fclose");
        return -1;
    }
    return 0;


}

/*
 * Traverses filepath rooted at source (locally) with sock_fd
 * Then for each file or directory
 * -- makes and sends request struct
 * -- waits for response from server
 * ---- OK: continue to next file
 * ---- SENDFILE:
 * ------ forks new process, main process continues to next file, child:
 * ------ initiate new connection with server w/e request.type = TRANSFILE
 * ------ makes and sends request struct
 * ------ transmit data from file
 * ------ waits for OK, close socket and exits, otherwise handles error
 * ----ERROR: print appropriate msg includes file name then exit(1)
 * Return
 * -- -1 for any error
 * -- 0 for success
 * -- >0 the number of child processes created
 */
int traverse(const char *source, const char *server_dest, int sock_fd, char *host, unsigned short port, int *error){
    static int child_count = 0;

    // make & send request for source
    struct request client_req;
    if (make_req(source, server_dest, &client_req) == -1){
      *error = 1;
      return child_count;
    }
    if (send_req(sock_fd, &client_req) == -1){
      *error = 1;
      return child_count;
    }

    // wait for response from server
    int res;
    int num_read = read(sock_fd, &res, sizeof(int));
    if (num_read != sizeof(int)){ // Can't do anything if message can't be interpreted
      *error = 1;
      return child_count;
    }
    res = ntohl(res);

    printf("%d \t%d \t%d \t%d \t%s\n",                  // TODO: remove print later
            getpid(), sock_fd,
            client_req.type, res, client_req.path);

	  if (res == SENDFILE){
	      child_count++;

        int result = fork();
        if (result == 0){                // Child

            // Create a new socket for child process
            int child_sock_fd = child_sock_fd = client_sock(host, port);        // TODO: what is the 2 assignment
            if (child_sock_fd == -1){
              exit(1);
            }

            // Sending request
            int file_type = client_req.type;
            client_req.type = TRANSFILE;
            if (send_req(child_sock_fd, &client_req) == -1){
              exit(1); // We cannot expect a response if the request wasn't fully sent
            }

            /* Copy file / dir has two scenario
             * based on the type of file / dir
             * -- REGFILE
             * ---- client opens file and writes to client socket
             * ---- server reads and creates new file
             * -- REGDIR
             * ---- client just have to wait for OK
             * ---- server creates dir based on req alone
             */

            if(file_type == REGFILE){
                send_data(child_sock_fd, source, &client_req); //TODO error check
            }

            /*
             * File sender client receives 2 possible responses
             * OK
             * -- terminate child process with exit status of 0
             * ERROR
             * -- print appropriate msg with file causing error
             * -- and exit with status of 1
             */
            num_read = read(child_sock_fd, &res, sizeof(int));
				    if(num_read != sizeof(int)){
                perror("client:read");
                exit(1);
            }
            res = ntohl(res);     
            

            close(child_sock_fd);

            printf("%d \t%d \t%d \t%d \t%s\n",          // TODO: remove print later
                    getpid(), child_sock_fd,
                    client_req.type, res, client_req.path);

            printf("\t\t\t\t\t\tc:%d \t%d \t%d \t%s \t",
                getpid(), client_req.size,
                client_req.mode, client_req.path);
            show_hash(client_req.hash);

            if(res == OK){
                exit(0);
            } else if (res == ERROR){
                fprintf(stderr, "client: sock [%d] at [%s] receives "
                        "ERROR from server\n", child_sock_fd, source);
                exit(1);
            } else {
                printf("unexpected res = [%d]", res);
            }
            exit(1);

        } else if (result < 0){ //parent
            perror("fork");
            *error = 1;
            return child_count;
        }

    } else if(res == ERROR){
        fprintf(stderr, "client: sock [%d] at [%s] receives "
                "ERROR from server\n", sock_fd, source);
        *error = 1;
        return child_count;
    }


    // tree traversal
    struct stat file_buf;
    if (lstat(source, &file_buf)){
        perror("client:lstat");
        *error = 1;
        return child_count;
    }


    // recursively call traverse() if source is a directory
    if (S_ISDIR(file_buf.st_mode)){
        DIR *dirp;
        struct dirent *dp;

        if ((dirp = opendir(source)) == NULL){
            perror("opendir");
            *error = 1;
            return child_count;
        }

        while ((dp = readdir(dirp)) != NULL){     // traverse dirp
            if ((dp->d_name)[0] != '.'){          // avoid dot files

                // Compute "source/filename"
                char src_path[MAXPATH];
					      // Compute server_dest/filename
                char server_path[MAXPATH];
                int inner_error; //error of file in folder
                strncpy(src_path, source, sizeof(src_path) - strlen(source) - 1);
                strncat(src_path, "/", sizeof(src_path) - strlen("/") - 1);
                strncat(src_path, dp->d_name, sizeof(src_path) - strlen(dp->d_name) - 1);
					      strncpy(server_path, server_dest, sizeof(server_path) - strlen(server_dest) - 1);
                strncat(server_path, "/", sizeof(server_path) - strlen("/") - 1);
                strncat(server_path, dp->d_name, sizeof(server_path) - strlen(dp->d_name) - 1);

                traverse(src_path, server_path, sock_fd, host, port, &inner_error);
                *error = inner_error ? inner_error : *error;
            }
        }
    }

    return child_count;

}

/*
 * The main client waits for count number of
 * child processes to terminate 
 * Return 0 if success -1 otherwise
 */
int client_wait(int count){

    while(count-- != 0){
        pid_t pid;
        int status;
        if((pid = wait(&status)) == -1) {
            perror("client:wait");
            return -1;
        } else {

            if(!WIFEXITED(status)){
                fprintf(stderr, "client:wait return no status\n");
            } else if(WEXITSTATUS(status) == 0){
                                    // TODO: remove this afterwards. here just for debugging..
                fprintf(stdout, "\t\t\t\t\t\tf:%d \tterminated "
                        "with [%d] (success)\n", pid, WEXITSTATUS(status));
            } else if(WEXITSTATUS(status) == 1){
                fprintf(stdout, "\t\t\t\t\t\tf:%d \tterminated "
                        "with [%d] (error)\n", pid, WEXITSTATUS(status));
                return -1;
            }
        }
    }
    return 0;
}
