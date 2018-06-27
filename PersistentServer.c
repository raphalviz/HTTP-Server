#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timeb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <libgen.h>
#include <fcntl.h>

#define __USE_XOPEN
#define _XOPEN_SOURCE 700
#include <time.h>

#define BUF_SIZE 1024
#define MAX_HEADERS 6 /* Temporary until we decide on headers */

typedef struct {
    char *name;
    char *value;
} header_tuple;

typedef struct {
    char *method;
    char *path;
    char *resource;
    char *version;
    header_tuple *headers;
} client_request;

typedef struct {
    char *version;
    char *res_code;
    char *res_message;
} response_status;

typedef struct {
    const char *file_extension;
    const char *mime_type;
} mime_map;

/* Supported MIME types / file extensions */
mime_map mime_types [] = {
    {".html", "text/html"},
    {".txt", "text/plain"},
    {".css", "text/css"},
    {".js", "text/javascript"},
    {".jpg", "image/jpeg"}
};

int num_tok = 0;

/**
    Checks what MIME type a file with the name fname is.
    @param fname : The name of the file as a string
    @return a string of the MIME type used for the file type
*/
char *get_mime_type(char *fname) {
    int i;
    char *ext = strrchr(fname, '.');

    if (ext != NULL) {
        for (i = 0; i < (sizeof(mime_types)/sizeof(mime_types[0])); i++) {
            if (strcmp((ext), mime_types[i].file_extension) == 0) {
                return (char *)mime_types[i].mime_type;
            }
        }
    }

    return fname;
}

/**
    Given a name of a file, return the size of the file in bytes.
    @param fname : Name of the file as string
    @return size of fname in bytes
*/
off_t get_file_size(char *fname) {
    struct stat buf;
    if (stat(fname, &buf) == 0) {
        return buf.st_size;
    }
    return -1;
}

/**
    Used for when "If-Modified-Since" header is supplied by client.
    Compares the date of when the requested resource is last modified
    and the time specified in the header
    @param req_object : Struct with all the info
    @return 0 if modified since, 1 otherwise
*/
int cmp_mod_date(client_request *req_object) {
    struct stat buf;
    int i;
    char *header_time;
    struct tm h_time;

    stat(req_object->path, &buf);
    memset(&h_time, 0, sizeof(struct tm));

    time_t mod_time = buf.st_mtime;

    for(i = 0; req_object->headers[i].name != NULL; i++) {
        if(strcmp(req_object->headers[i].name, "If-Modified-Since") == 0) {
            header_time = malloc(strlen(req_object->headers[i].value) + 1);
            strcpy(header_time, req_object->headers[i].value);
            strptime(header_time, "%a, %d %b %Y %H:%M:%S %Z", &h_time);

            long mtime = (long)mod_time;
            long htime = (long)mktime(&h_time);

            if (mtime > htime) {
                printf("Compared Modified Time: %ld with Specified Time: %ld\n", mtime, htime);
                return 0;
            } else {
                return 1;
            }
            break;
        }
    }

    return 1;
}

/**
    Check if there is a header entered with name hname.
    @param req_object : Struct with all the info
    @return 0 if in the headers, 1 otherwise
*/
int does_header_exist(client_request *req_object, char *hname) {
    int i;

    for(i = 0; req_object->headers[i].name != NULL; i++) {
        if(strcmp(req_object->headers[i].name, hname) == 0) {
            return 0;
            break;
        }
    }
    return 1;
}

/**
    Generates the initial response line (a.k.a. Status Line)
    consisting of the HTTP version, response code and message
    (e.g. HTTP/1.0 200 OK)
    @param req_object : Struct containing info about the request
    @param res_status : Struct to be filled with info about response status
    @return a string that is the status line
*/
char *generate_status_line(client_request *req_object, response_status *res_status) {
    char *status;

    res_status->version = malloc(sizeof(char) * (9));
    res_status->res_code = malloc(sizeof(char) * (4));
    strcpy(res_status->version, "HTTP/1.1");

    // Check for valid method (Only GET)
    if(strcmp(req_object->method, "GET") != 0) {
        res_status->res_message = malloc(sizeof(char) * (strlen("Method Not Allowed") + 1));
        strcpy(res_status->res_code, "405");
        strcpy(res_status->res_message, "Method Not Allowed");
        status = "HTTP/1.1 405 Method Not Allowed\n";
    // Check for valid HTTP protocol (Only HTTP/1.0 or 1.1)
    } else if (strcmp(req_object->version, "HTTP/1.0") != 0 && strcmp(req_object->version, "HTTP/1.1") != 0) {
        res_status->res_message = malloc(sizeof(char) * (strlen("HTTP Version Not Supported") + 1));
        strcpy(res_status->res_code, "505");
        strcpy(res_status->res_message, "HTTP Version Not Supported");
        status = "HTTP/1.1 505 HTTP Version Not Supported\n";
    // Check for existing req_path
    } else if (access(req_object->path, F_OK) == -1) {
        res_status->res_message = malloc(sizeof(char) * (strlen("Not Found") + 1));
        strcpy(res_status->res_code, "404");
        strcpy(res_status->res_message, "Not Found");
        status = "HTTP/1.1 404 Not Found\n";
    } else if(does_header_exist(req_object, "If-Modified-Since") == 0 && cmp_mod_date(req_object) == 1) {
        res_status->res_message = malloc(sizeof(char) * (strlen("Not Modified") + 1));
        strcpy(res_status->res_code, "304");
        strcpy(res_status->res_message, "Not Modified");
        status = "HTTP/1.1 304 Not Modified\n";
    // Otherwise 200 OK
    } else {
        res_status->res_message = malloc(sizeof(char) * (strlen("OK") + 1));
        strcpy(res_status->res_code, "200");
        strcpy(res_status->res_message, "OK");
        status = "HTTP/1.1 200 OK\n";
    }
    printf("%s: %s\n", res_status->res_code, res_status->res_message);
    return status;
}


/**
    Generates the header lines for the response based on the
    details of the client's request found in req_object
    @param req_object : Struct containing info about the request
    @param res_status : Struct containing info about the status of the response
    @return a string of the header lines separated with \r\n
*/
char *generate_header_lines(client_request *req_object, response_status *res_status) {
    char *header_lines;
    char date_buf[128];
    time_t now = time(0);
    struct tm time = *gmtime(&now);
    struct stat buf;

    if (strcmp(res_status->res_code, "405") == 0) {
        char *line = "Allow: GET\r\nConnection: Keep-Alive\r\n";
        header_lines = malloc(strlen(line) + 1);
        strcpy(header_lines, line);
    }

    if (strcmp(res_status->res_code, "505") == 0 || strcmp(res_status->res_code, "404") == 0) {
        char *line = "Connection: Keep-Alive\r\n";
        header_lines = malloc(strlen(line) + 1);
        strcpy(header_lines, line);
    }

    if (strcmp(res_status->res_code, "200") == 0) {
        if (stat(req_object->path, &buf) == 0) {
            char line[128];

            sprintf(line, "Content-Length: %jd\r\n", (intmax_t)get_file_size(req_object->path));
            header_lines = malloc(1024);
            strcpy(header_lines, line);

            sprintf(line, "Content-Type: %s\r\n", get_mime_type(req_object->path));
            strcat(header_lines, line);

            strftime(date_buf, sizeof(date_buf), "%a, %d %b %Y %H:%M:%S %Z", &time);
            sprintf(line, "Date: %s\r\n", date_buf);
            strcat(header_lines, line);

            struct stat buf;
            stat(req_object->path, &buf);
            time_t mtime = buf.st_mtime;
	    strftime(date_buf, sizeof(date_buf), "%a, %d %b %Y %H:%M:%S %Z", localtime(&mtime));
            sprintf(line, "Last modified: %s\r\nConnection: Keep-Alive\r\n", date_buf);
            strcat(header_lines, line);
        }
    }
    return header_lines;
}


/**
    Generates the entire response including the status line, header lines,
    and the message body using other generators.
    @param req_object : Struct containing info about the request
    @return a string of the entire response
*/
char *generate_response(client_request *req_object) {
    char *response;

    // getting status line
    response_status res_status;
    char *status_line = generate_status_line(req_object, &res_status);

    if(strcmp(res_status.res_code, "304") == 0) {
        struct stat buf;
        char date_buf[30];
        stat(req_object->path, &buf);
        time_t mtime = buf.st_mtime;
        strftime(date_buf, 30, "%a, %d %b %Y %H:%M:%S %Z", localtime(&mtime));
        response = malloc(strlen(res_status.res_message) + strlen(res_status.res_code) + strlen(date_buf) + 70);
        strcpy(response, "HTTP/1.1 304 Not Modified\r\n");
        strcat(response, "Last-Modified: ");
        strcat(response, date_buf);
        strcat(response, "\r\nConnection: Keep-Alive\r\n");
        return response;
    }

    // getting header lines
    char *header_line = generate_header_lines(req_object, &res_status);

    long file_size;
    FILE *fp = fopen(req_object->path, "r");

    // getting the file client requested
    // check if file was opened and 200 status code
    if (fp != NULL && strcmp(res_status.res_code, "200") == 0) {

        // first determine the file's size then reset
        // the fp to the beginning
        fseek(fp, 0, SEEK_END);
        file_size = ftell(fp);
        rewind(fp);

        // read the file into a buffer that exactly fits the data
        char buf[file_size];
        fread(buf, file_size, 1, fp);
        buf[file_size] = '\0';

        response = malloc(strlen(buf) + strlen(status_line) + strlen(header_line) + 5);
        strcpy(response, status_line);
        strcat(response, header_line);
        strcat(response, "\r\n");
        strcat(response, buf);
    } else {
        response = malloc(strlen(status_line) + strlen(header_line) + 2);
        strcpy(response, status_line);
        strcat(response, header_line);
    }

    return response;
}


/**
    Parse the request line to get the method, path, and version.
    @param req_line : A string containing the method, path and version
    @param req_object : A client_request struct
    @param client_fd : client file descriptor
    @return Number of arguments in the request line
*/
int parse_request(char *req_line, client_request *req_object, char *root_path) {
    int num_tokens = 0;
    const char delim[2] = " ";
    char path_buf[512];
    char *token, *req_method, *req_path = "\0", *req_version = "HTTP/1.1";

    req_line[strlen(req_line) - 1] = '\0';
    token = strtok(req_line, delim);

    // Loop to tokenize the request
    while(token != NULL) {
        num_tokens++;
        // Check which token we're on and assign accordingly
        switch(num_tokens) {
            case 1:
                req_method = token;
                break;
            case 2:
                req_path = token;
                break;
            case 3:
                req_version = token;
                break;
        }
        token = strtok(NULL, delim);
    }

    if(strcmp(req_path, "/") == 0) {
        req_path = "/index.html";
    }

    req_object->method = malloc(sizeof(char) * (strlen(req_method) + 1));
    strcpy(req_object->method, req_method);

    req_object->resource = malloc(sizeof(char) * (strlen(req_path) + 1));
    strcpy(req_object->resource, req_path);

    req_object->path = malloc(sizeof(char) * (strlen(req_path) + 1));
    sprintf(path_buf, "%s%s", root_path, req_path);
    strcpy(req_object->path, path_buf);

    req_object->version = malloc(sizeof(char) * (strlen(req_version) + 1));
    strcpy(req_object->version, req_version);

    return num_tokens;
}

/**
    Parse header lines and assign the values to a header_tuple.
    Add this to the array of headers in a client_request.
    TODO: Handle comma separated value and (if we have time) folded headers.
*/
void parse_headers(char *header_line, client_request *req_object) {
    const char delim[3] = ":";
    char *token, *header_name, *header_value;
    int i;

    header_line[strlen(header_line) - 1] = '\0';

    token = strtok(header_line, delim);
    header_name = token;

    token = strtok(NULL, "");
    header_value = token;

    if (header_value[0] == ' ') header_value++;

    for(i = 0; i < MAX_HEADERS; i++) {
        if(req_object->headers[i].name == NULL) {
            req_object->headers[i].name = malloc(strlen(header_name) + 1);
            strcpy(req_object->headers[i].name, header_name);

            req_object->headers[i].value = malloc(strlen(header_value) + 1);
            strcpy(req_object->headers[i].value, header_value);
            break;
        }
    }
}

/**
    Print the req_object headers.
    @param req_object : A client_request struct containing method, path and HTTP version
*/
void print_headers(client_request *req_object) {
    int i;
    for(i = 0; req_object->headers[i].name != NULL; i++) {
        printf("%s: %s\n", req_object->headers[i].name, req_object->headers[i].value);
    }
}

/**
    Check to see whether the path exists on the server.
    Primarily used for the http_root_path and the client's
    request path.
    @param path : A string denoting a relative path
    @return 0 if exists, 1 if not
*/
int valid_path(char *path) {
    int status = 1;
    struct stat path_info;

    if(stat(path, &path_info) == 0 )
    {
        // Check if the path is either a directory or file
        // otherwise it is an unsupported type
        if(path_info.st_mode & S_IFDIR || path_info.st_mode & S_IFREG)
        {
            status = 0;
        }
    }

    return status;
}

int main(int argc, char* argv[]) {
    int server_fd, client_fd, client_addr_len, opt;
    int port_no;
    struct sockaddr_in server_addr, client_addr;
    char client_msg[1024], *http_root_path;
    ssize_t bytes_read = 0;

    struct timeb t_last, t_now;
    long last, now;
    int closed = 0;

    if (argc != 3) {
        fprintf(stderr, "Usage: %s port http_root_path\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    port_no = atoi(argv[1]);

    http_root_path = argv[2];

    if (valid_path(http_root_path)) {
        fprintf(stderr, "Invalid path: %s\n", http_root_path);
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(struct sockaddr_in));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_no);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, (socklen_t)sizeof(int));

    bind(server_fd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr_in));

    listen(server_fd, 5);

    fprintf(stdout, "Now serving %s at %s on port %d\n", http_root_path, inet_ntoa(server_addr.sin_addr), port_no);

    while (1) {
        // waiting for a client connection
        printf("Listening for connections...\n");
        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, (socklen_t *)&client_addr_len);
        closed = 1;

        num_tok = 0;
        client_request req_object;
        req_object.headers = malloc(sizeof(header_tuple) * MAX_HEADERS);

        // set client_fd to be non-blocking to allow system calls such as read/write on
        // client_fd to continue if nothing was sent
        printf("Received connection from %s\n", inet_ntoa(client_addr.sin_addr));
        fcntl(client_fd, F_SETFL, O_NONBLOCK);

        // get the current time in milliseconds
        ftime(&t_last);
        last = t_last.time * 1000 + t_last.millitm;

        ftime(&t_now);
        now = t_now.time * 1000 + t_now.millitm;

        // timeout on client connection of 10000ms / 10 seconds
        // if nothing has been sent from client since our last known time
        while ((now - last) < 10000 || closed == 0) {
            // check if client sent any requests
            bytes_read = read(client_fd, client_msg, BUF_SIZE - 1);
            if (bytes_read != -1) {
                client_msg[bytes_read] = '\0';
                printf("%s: %s", inet_ntoa(client_addr.sin_addr), client_msg);

                if (num_tok == 0) {
                    num_tok = parse_request(client_msg, &req_object, http_root_path);
                    printf("Method: %s\nPath: %s\nVersion: %s\n", req_object.method, req_object.path, req_object.version);
                    // 1-3 tokens
                    if (num_tok > 0 && num_tok < 4) {
                        printf("\nWaiting for optional headers or CrLf...\n");
                    } else {
                        printf("Send response\n");
                        write(client_fd, "HTTP/1.0 400 Bad Request\nConnection: keep-alive\n", 48);
                    }
                } else {
                    if ((client_msg[0] == '\r' && client_msg[1] == '\n') || client_msg[0] == '\n' || (client_msg[0] == 'G' && client_msg[1] == 'E' && client_msg[2] == 'T')) {
                        printf("\nPrinting all headers from request:\n");
                        print_headers(&req_object);
                        printf("\nFinished entering headers, now sending response\n");

                        // generate server response
                        char *server_response = generate_response(&req_object);
                        write(client_fd, server_response, strlen(server_response));

                        /*
                        // check if client has specifically requested to close the connection
                        for(int i = 0; req_object.headers[i].name != NULL; i++) {
                            if (strcmp(req_object.headers[i].name, "Connection") == 0 && strcmp(req_object.headers[i].value, "close") == 0) {
                                close(client_fd);
                                closed = 0;
                            }
                        }
                        */

                        free(req_object.headers);
                        num_tok = 0;
                    } else {
                        if (strlen(client_msg) > 1) {
                            parse_headers(client_msg, &req_object);
                        } else {
                            printf("Send response\n");
                            write(client_fd, "HTTP/1.0 400 Bad Request\nConnection: keep-alive\n", 48);
                        }
                    }
                }

                // reset time variables to refresh client's timeout
                ftime(&t_last);
                last = t_last.time * 1000 + t_last.millitm;

                ftime(&t_now);
                now = t_now.time * 1000 + t_now.millitm;
            } else {
                // o/w client has not sent anything
                // get the current time so we can compare it to client's last request time
                ftime(&t_now);
                now = t_now.time * 1000 + t_now.millitm;
            }
        }
        // client has timed out
        // close connection
        printf("Connection timed out\n");
        write(client_fd, "\nConnection to server lost\n", 27);
        close(client_fd);
    }

    return 0;
}

