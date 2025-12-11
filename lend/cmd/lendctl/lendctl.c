#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <limits.h>
#include <errno.h>

#define PORT 4466
#define BUF_SIZE 4096
#define BIN_DIR ".lend/bin"
#define LINK_DIR ".lend/files"

static const char* get_app(const char* p) {
    const char* s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static char* abspath(const char* p) {
    char* a = realpath(p, NULL);
    if (!a) { perror("path error"); exit(1); }
    return a;
}

static void init_dir(char* d) {
    struct stat st;
    if (stat(d, &st) || !S_ISDIR(st.st_mode)) {
        if (mkdir(d, 0755) && errno != EEXIST) {
            perror("failed to create directory");
            exit(1);
        }
    }
}

static const char* get_host() {
    static char host[256] = {0};
    char path[PATH_MAX];
    snprintf(path, PATH_MAX, "%s/.lend/files", getenv("HOME"));
    
    DIR* dir = opendir(path);
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir))) {
            if (entry->d_name[0] != '.') {
                strncpy(host, entry->d_name, sizeof(host) - 1);
                break;
            }
        }
        closedir(dir);
    }
    return host[0] ? host : "default";
}

static char* message(const char* command, int start, int argc, char* argv[]){
    char* msg = malloc(BUF_SIZE);
    int len = snprintf(msg, BUF_SIZE, "%s ", command);
    for (int i = start; i < argc; i++) {
        char* param = argv[i];
        char abs_path[PATH_MAX];
        struct stat st;
        
        // Check if valid path
        if (stat(param, &st) == 0 && (S_ISDIR(st.st_mode) || S_ISREG(st.st_mode))) {
            // Get absolute path
            if (!realpath(param, abs_path)) {
                // realpath failed, treat as regular argument
                len += snprintf(msg + len, BUF_SIZE - len, "%s ", param);
                continue;
            }
            
            // Extract filename from absolute path
            char* fs_name = strrchr(abs_path, '/');
            fs_name = fs_name ? fs_name + 1 : abs_path;
            
            // Create symlink in host-specific directory
            char link[PATH_MAX];
            snprintf(link, PATH_MAX, "%s/%s/%s/%s", getenv("HOME"), LINK_DIR, get_host(), fs_name);
            (void)symlink(abs_path, link);
            
            // Send relative path from ~/.lend/files/
            len += snprintf(msg + len, BUF_SIZE - len, "FILE|%s/%s ", get_host(), fs_name);
            
        } else {
            // Non-file argument
            len += snprintf(msg + len, BUF_SIZE - len, "%s ", param);
        }  
    }
    msg[len++] = '\n';
    msg[len] = '\0';
    return msg;
}

static int conn() {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family=AF_INET, .sin_port=htons(PORT)};
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(s, (struct sockaddr*)&a, sizeof(a)) < 0) {
        close(s);
        return -1;
    }
    return s;
}

static void show_help() {
    printf("Usage: lendctl <command> [args]\n");
    printf("Commands:\n");
    printf("  link <name>    - Create link in ~/.lend/bin\n");
    printf("  <name> [args]  - Send command to local service\n");
    printf("  -h             - Show help\n");
}

int main(int argc, char* argv[]) {
    char command[BUF_SIZE];
    int start = 1;

    if (argc >= 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        show_help();
        return 0;
    }

    strncpy(command, get_app(argv[0]), BUF_SIZE-1);

    if (!strcmp(command, "lendctl") && argc >= 2) {

        if (!strcmp(argv[1], "link")){
            if (argc < 3) { fprintf(stderr, "Usage: lendctl link <name>\n"); return 1; }
            char dir[PATH_MAX], link[PATH_MAX], self[PATH_MAX];
            snprintf(dir, PATH_MAX, "%s/%s", getenv("HOME"), BIN_DIR);
            init_dir(dir);
            
            ssize_t len = readlink("/proc/self/exe", self, sizeof(self)-1);
            if (len == -1) { perror("failed to get executable path"); return 1; }
            self[len] = '\0';
            
            char* abs = abspath(self);
            snprintf(link, PATH_MAX, "%s/%s", dir, argv[2]);
            if (symlink(abs, link) < 0) { perror("failed to create link"); return 1; }
            
            return 0;

        }else{
            strncpy(command, argv[1], BUF_SIZE-1);
            start = 2;
        }
    }

    char* msg = message(command, start, argc, argv);

    int sock = conn();

    if (sock < 0) { 
        perror("connection failed"); 
        free(msg);
        return 1; 
    }
    
    if (send(sock, msg, strlen(msg), 0) < 0) {
        fprintf(stderr, "command not found: %s\n", command);
        free(msg);
        close(sock);
        return 1;
    }

    free(msg);
    close(sock);
    return 0;
}