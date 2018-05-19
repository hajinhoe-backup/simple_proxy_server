//네트워크를 위한 헤더파일
#include <netdb.h>
#include <sys/socket.h>
//입출력을 위한 헤더파일
#include <stdio.h>
#include <stdlib.h>
//기타 편의를 위한 헤더파일
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <zconf.h>
#include <signal.h>
//멀티쓰레딩
#include <pthread.h>

//기타 정의
#define SERVER_INFO "ERICA proxy (on Ubuntu)"
#define MAX_THREAD 4
/* 세부 사항
 * (1) 일부 통신 에러시 404 페이지를 임의로 전송합니다. (ip 값을 받아올 수 없거나, 암호화 통신을 요구하는 경우)
 * (2) 캐시의 url 의 크기가 512로 제한됩니다. 캐시는 url로만 판단합니다.
 * (3) POST와 피라미터를 전송하는 GET의 경우에는 캐시하지 않습니다. (외부 서버와 통신이 필수이기 때문에.)
 * (4) 유저가 원하는 경우에도 캐시를 하지 않습니다.
 * (5) 캐시는 양쪽 연결 배열로 구현되었습니다. (변수가 이용됩니다.)
 *
/* 어려웠던 점
 * (1) SSL 인증서 통신으로 인해 속도가 저하
 * (2) url 버퍼의 오버플로우로 인한 오류 (url 크기가 de fatto로 2K까지라 하는데, 여기서는 512만 저장)
 * (3) 멀티 쓰레드
 * (4) 포인터로 저장한 문자열은 크기를 바로 알 수 없는 점. 그래서 좀 저장해 놓은 게 바로 오지를 않았습니다.
 * (5) 캐시 저장 버퍼의 용량 계산을 실수로 잘 못해서 버퍼 오버플로우가 발생해서 잡는 데 약간 시간을 소모했습니다.
 *
*/
//구조체 선언
struct arg_for_thread {
    //pthread를 위한 매개 변수 전달용 구조체를 선언합니다.
    int accepted_sockfd;
    int client_int_ip;
};
//더블 연결 리스트
typedef struct cache_node {
    char url[524]; //혹시 모를 에러를 대비해서 살짝 버퍼를 높게 잡음.
    char *cache_buffer; //캐시의 메모리
    int size;
    struct cache_node *next;
    struct cache_node *pre;
} CACHE;

//전역 변수
static bool do_while = true;
static bool thread_lock = true; //쓰레드 관련 블록하려고했는데 필요한건지....
static int thread_num = 0; //사용 중인 스레드의 개수를 관리함
static int cache_size = 0; //캐시의 총 사이즈
//캐시 관리를 위한 노드
static CACHE *cache_head = NULL;
static CACHE *cache_tail = NULL;

bool is_cacheable(char *header, char *url) {
    //헤더를 보고 해당 페이지를 캐시 해야하는지 알려준다.
    if(strstr(header, "POST http") != NULL) {
        //POST 콜입니다.
        //캐시할 수 없는 경우입니다.
        printf("\nPOST request should NOT cached.\n");
        return false;
    }
    if(strstr(url, "?") != NULL) {
        //GET 이지만, 서버에 파라미터를 보내는 경우입니다.
        printf("\nParameter request should NOT cached.\n");
        return false;
    }
    if(strstr(url, "Cache-Control: no") != NULL) {
        //사용자가 캐시를 저장하지 말도록 한 경우입니다.
        printf("\nUser want this request should NOT cached.\n");
        return false;
    }
    return true;
}

int add_cache(CACHE **pp_head, CACHE **pp_tail, char *url, char *content, int size) {
    //헤드 노드 다음에 리스트를 추가합니다.
    //만약 노드가 하나도 없다면 테일 노드에 기록을 합니다.
    CACHE *p;

    if (strlen(url) > 512) {
        printf("\nurl의 크기가 너무 큽니다. 노드를 만들 수 없습니다.\n");
        return -1;
    }
    if (url == NULL || content == NULL) {
        printf("\n올바르지 않은 정보가 있습니다. 노드를 만들 수 없습니다.\n");
        return -1;
    }

    if(NULL == *pp_head) {
        *pp_head = (CACHE *)malloc(sizeof(CACHE));
        *pp_tail = *pp_head;
        p = *pp_head;
        p -> next = NULL;
        p -> pre = NULL;
    } else {
        p = (*pp_head);
        *pp_head = (CACHE *)malloc(sizeof(CACHE));
        (*pp_head) -> next = p;
        p -> pre = (*pp_head);
        p = (*pp_head);
    }

    strcpy(p -> url, url);
    printf("\n노드를 추가했습니다. : %s\n",url);
    p -> cache_buffer = (char *)malloc(sizeof(char) * (size + 1));
    memcpy(p -> cache_buffer, content, sizeof(char) * (size + 1));
    p -> size = size + 1;
}

int del_cache(CACHE **pp_tail) {
    //제일 뒤의 노드를 삭제하고, 그 앞의 노드를 테일 노드에 기록합니다.
    CACHE *p;
    int size;

    p = *pp_tail;
    size = p -> size;
    (*pp_tail) = (*pp_tail) -> pre;
    (*pp_tail) -> next = NULL;

    free(p -> cache_buffer);
    p -> cache_buffer = NULL;
    free(p);
    p = NULL;

    return size;
}

//원형 선언
char *get_server_time();
char *int_to_ip(int number);
void error(char *msg);
void proxy_log(int int_ip_address, char *url, int file_size);
void get_domain_info(char buf[], char **domain_info);
void become_http1(char header[]);
void send_404(int accepted_sockfd);

void cache_store(char *url, char *content, int size){
    //단일 용량이 512KB 미만인 캐시 파일을 저장
    //총 용량이 5MB에 달하면 제일 오래 전에 사용된 것을 지운다.

    //노드를 기록한다.
    add_cache(&cache_head, &cache_tail, url, content, size);

    //총 용량을 기록한다.
    cache_size = cache_size + size;

    //총 용량을 확인하여 5MB를 넘는다면 노드를 삭제한다.
    while (cache_size > 5242880) {
        printf("\n용량을 초과했기 때문에, 캐시를 지웠습니다.\n");
        cache_size = cache_size - del_cache(&cache_tail);
    }
}

char *cache_access(CACHE **cache_head, CACHE **cache_tail, char *url, int *size){
    //포인터와 변수를 하나 리턴해야 하는데 어쩔 수 없이, 맨 마지막에 int 형 포인터를 받아서 size를 리턴합니다. (주의)
    //캐시를 억세스한다.
    //억세스시 순서를 제일 최근으로 바꾼다.
    //실패시 NULL을 리턴한다.
    //캐시의 파일의 번호를 리턴함.
    CACHE *p;

    p = *cache_head;

    while(p != NULL) {
        if (strcmp(p -> url, url) == 0) { //url이 동일한 경우
            //억세스에 성공한 경우 성공한 노드를 헤더 앞으로 옮깁니다.
            if(p != *cache_head) {
                if(p == *cache_tail) {
                    *cache_tail = p->pre;
                } else {
                    p->next->pre = p->pre;
                }
                p->pre->next = p->next;
                p->pre = NULL;
                (*cache_head) -> pre = p;
                p->next = *cache_head;
                *cache_head = p;
            }
            *size = p -> size;
            return p -> cache_buffer;
        } else {
            p = p -> next;
        }
    }

    return NULL;
}

void *proxy_do(void *arg) {
    //새로운 스레드를 만들 수 없도록 블락한다.
    //thread_lock = false;
    //구조체를 arg 포인터로 받아서 변환한다.
    //connection_info->accepted_sockfd, client_int_ip를 사용가능
    struct arg_for_thread *connection_info = (struct arg_for_thread *)arg;
    int accepted_sockfd = connection_info -> accepted_sockfd;
    int client_int_ip = connection_info -> client_int_ip;

    int external_sockfd; //외부 서버와 통신을 위한 소켓이다.
    struct addrinfo hints, *res; //외부 서버의 ip 등을 가져오기 위함.

    char buf[4096] = {0, };
    ssize_t byte_count = 0;
    int file_size = 0;

    char* domain_info[2]; //도메인 관련 정보를 기록하는 배열

    char* cache_content;
    //새로운 스레드를 만들 수 있도록 블락을 푼다.
    //thread_lock = true;
    memset(&hints, 0,sizeof(hints));
    hints.ai_family=AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    //사용자에게 헤더를 받아옵니다. 헤더의 합이 8192바이트를 넘지 않는다고 가정합니다.
    byte_count = read(accepted_sockfd, buf, sizeof(buf) - 1);
    if(byte_count < 0) {
        printf("\n헤더 요청을 받을 수 없었습니다.\n");
        send_404(accepted_sockfd);
        return 0;
    }
    if (!(strstr(buf, "GET") != NULL || strstr(buf, "POST") != NULL)) {
        //GET 또는 POST가 아닌 경우
        printf("\n처리할 수 없는 요청을 받았습니다.\n");
        send_404(accepted_sockfd);
        return 0;
    }

    get_domain_info(buf, domain_info); //헤더로부터 도메인 정보를 추출

    int cache_size = 0;

    cache_content = cache_access(&cache_head, &cache_tail, domain_info[1], &cache_size);

    if (cache_content == NULL) { //캐시에 없는 내용입니다.
        char* cache_buffer = (char *)malloc(sizeof(char) * 524288); //캐시 저장을 위해 512KB의 버퍼를 선언
        memset(cache_buffer, 0, sizeof(cache_buffer));
        char* cache_buffer_pointer = cache_buffer;
        bool can_cached = is_cacheable(buf, domain_info[1]);

        if(getaddrinfo(domain_info[0],"80", &hints, &res) != 0) {
            //추출한 도메인 정보로부터 서버의 ip를 받아 옵니다. 에러시 종료
            printf("\n외부 서버의 ip를 받아오지 못 했습니다.\n");
            send_404(accepted_sockfd);
            return 0;
        }
        external_sockfd = socket(res->ai_family,res->ai_socktype,res->ai_protocol); //외부 서버에 연결합니다.
        if(connect(external_sockfd,res->ai_addr,res->ai_addrlen) != 0) {
            printf("\n외부 서버와의 연결에 실패했습니다.\n");
            send_404(accepted_sockfd);
            return 0;
        }

        become_http1(buf); //사용자 헤더의 요청을 http1.0, close 상태로 강제 변환합니다.
        write(external_sockfd, buf, sizeof(char) * byte_count); //헤더를 외부 서버로 전송합니다.
        printf("\n%s\n", buf);
        if (strstr(buf, "application/ocsp-request") != NULL) {
            //ssl 인증서 요청하는데 트래픽이 https로 들어가서 멈춥니다.... 방지...
            send_404(accepted_sockfd);
            return 0;
        }

        //post시에 추가로 전송될 수 있는 헤더를 전송합니다.
        //버퍼가 꽉 찼다면 새로 로딩
        if(byte_count == sizeof(buf)) {
            do {
                byte_count = read(accepted_sockfd, buf, sizeof(buf) - 1);
                buf[byte_count] = 0;
                printf("\n%s\n", buf);
                if (write(external_sockfd, buf, sizeof(char) * byte_count) < 0)
                    break;
            } while (byte_count > 0);
        }

        //헤더의 전송을 끝낸 후에 서버로부터 응답을 받습니다.
        byte_count = read(external_sockfd, buf, sizeof(buf) - 1);
        file_size = file_size + (int)byte_count - 1;
        buf[byte_count] = 0;
        become_http1(buf);
        printf("\n%s\n", buf);
        write(accepted_sockfd, buf, sizeof(char) * byte_count);
        //캐시 기록
        memcpy(cache_buffer, buf, sizeof(char) * (byte_count));
        cache_buffer_pointer += byte_count;

        while (byte_count > 0) {
            byte_count = read(external_sockfd, buf, sizeof(buf) - 1);
            file_size = file_size + (int)byte_count;
            buf[byte_count] = 0;
            printf("\n%s\n", buf);
            if (write(accepted_sockfd, buf, sizeof(char) * byte_count) < 0) {
                break;
            };
            if (file_size > 520000 && can_cached) {
                printf("\n캐시에 저장할 수 없는 크기의 데이터입니다.\n");
                can_cached = false;
            } else {
                memcpy(cache_buffer_pointer, buf, sizeof(char) * (byte_count));
                cache_buffer_pointer += byte_count;
            }
        }

        file_size++;//널 문자

        char **url = &domain_info[1];

        if (can_cached) {
            cache_store(*url, cache_buffer, file_size);
        }
    } else {//캐시에 있는 내용입니다.
        become_http1(buf); //사용자 헤더의 요청을 http1.0, close 상태로 강제 변환합니다.
        printf("\n%s\n", buf);
        if (strstr(buf, "application/ocsp-request") != NULL) {
            //ssl 인증서 요청하는데 트래픽이 https로 들어가서 멈춥니다.... 방지...
            send_404(accepted_sockfd);
            return 0;
        }

        //post시에 추가로 전송될 수 있는 헤더를 전송합니다.
        //버퍼가 꽉 찼다면 새로 로딩
        if(byte_count == sizeof(buf)) {
            do {
                byte_count = read(accepted_sockfd, buf, sizeof(buf) - 1);
                buf[byte_count] = 0;
                printf("\n%s\n", buf);
            } while (byte_count > 0);
        }

        //캐시에 저장된 내용을 내 줍니다.
        if (write(accepted_sockfd, cache_content, sizeof(char) * cache_size) > 0) {
            printf("\n캐시에 저장되 있던 내용을 주었습니다.\n");
        }
        printf("\n준 캐시의 양 : %d\n", cache_size);
    }

    //전송이 종료되어 소켓을 닫습니다.
    if (close(accepted_sockfd) == 0) printf("\n현재 전송이 끝나 소켓을 닫았습니다.\n");

    proxy_log(client_int_ip, domain_info[1], file_size);

    //스레드의 종료
    printf("\n스레드가 종료되었습니다.\n");
    thread_num--;
}

int main(int argc, char *argv[]) {
    //클라이언트와 통신을 위한 변수
    int listen_sockfd, listen_port_no;
    socklen_t clilen;
    struct sockaddr_in serv_addr, cli_addr;
    struct arg_for_thread connection_info;

    if (argc < 2) {
        fprintf(stderr, "\n실행을 위해 포트 번호를 지정하시기 바랍니다.\n");
        exit(1);
    }

    //시그날을 무시합니다. broken pipe시 무시합니다.
    signal(SIGPIPE, SIG_IGN);

    //소켓 엶
    listen_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sockfd < 0) error("\n소켓을 열지 못 했습니다. 최근에 사용한 포트라면 잠시 기다렸다 다시 실행해보세요.\n");

    //열고 포트 지정했음
    memset((char *) &serv_addr, 0, sizeof(serv_addr));
    listen_port_no = atoi(argv[1]);
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(listen_port_no);

    if (bind(listen_sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0)
        error("\n바인딩 과정에 문제가 있습니다.\n");

    listen(listen_sockfd, 5);
    clilen = sizeof(cli_addr);

    printf("\n프로그램이 준비되었습니다. 프록시를 이용하여 http 접속을 시도하십시오.\n\n");

    //쓰레드를 위한 변수
    pthread_t thread_t;
    int status;

    //do_while을 나중에 시그날로 처리해야함.
    while(do_while) {
        if (thread_num < MAX_THREAD && thread_lock) {
            connection_info.accepted_sockfd = accept(listen_sockfd, (struct sockaddr *) &cli_addr, &clilen);
            if (connection_info.accepted_sockfd < 0) error("\n클라이언트의 요청이 수락되지 않았습니다.\n");
            connection_info.client_int_ip = cli_addr.sin_addr.s_addr;
            //if (pthread_create(&thread_t, NULL, proxy_do, (void *)&connection_info) < 0) {
            //    printf("\n사용자와의 통신을 위한 스레드가 정상적으로 수립되지 않았습니다.\n");
            //} else {
            //    printf("\n스레드가 수립되었습니다. 남은 쓰레드: %d\n", (MAX_THREAD - (thread_num + 1)));
            //    thread_num++;
            //}
            proxy_do((void *)&connection_info);
            //thread_num++;
        }
    }
}

char *get_server_time() {
    //헤더에 사용될 서버의 시간을 구함.
    struct tm *t;
    time_t timer;
    char *day[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    char *month[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "sept",
                       "Oct", "Nov", "Dec"};

    timer = time(NULL);
    t = localtime(&timer);

    char *time;
    time = malloc(sizeof(char) * 64);

    sprintf(time, "%s, %d %s %d %d:%d:%d GMT", day[t->tm_wday], t->tm_mday, month[t->tm_mon], t->tm_year+1900, t->tm_hour, t->tm_min, t->tm_sec);

    return time;
}

char *int_to_ip(int number) {
    //int형 정수로 받아오는 ip를 통상적인 255.255.255.255 형태로 변환시킵니다.
    char ip[4];
    char *ptr = malloc(sizeof(char) * 16);
    ip[0] = (char) number;
    ip[1] = (char) (number >> 8);
    ip[2] = (char) (number >> 16);
    ip[3] = (char) (number >> 24);
    sprintf(ptr, "%d.%d.%d.%d", ip[0], ip[1],ip[2],ip[3]);
    return ptr;
}

void error(char *msg) {
    //에러 컨트롤
    perror(msg);
    exit(1);
}

void proxy_log(int int_ip_address, char *url, int file_size){
    //프록시 로그 파일
    //eg. Sun 20 May 2018 02:51:02 EST: 166.104.231.100 http://hanyang.ac.kr/ 3111
    FILE *fp = fopen("./log.txt", "a");
    char* time = get_server_time();
    char* ip = int_to_ip(int_ip_address);

    fprintf(fp, "%s %s %s %d\n", time, ip, url, file_size);

    free(time);
    time = NULL;
    free(ip);
    ip = NULL;

    fclose(fp);
}

void get_domain_info(char buf[], char **domain_info){
    //buf는 클라이언트에서 받은 request
    //domain_info는 2개의 원소를 가지는 배열 포인터
    //domain_info[0] 도메인, domain_info[1] 하위경로 포함
    char domain_name[64];
    char domain_more[2048];
    char temp[4096];
    strcpy(temp, buf);
    strtok(temp, " ");
    strcpy(domain_more, strtok(NULL, " "));

    strcpy(temp, domain_more);

    strtok(temp, "//");
    strcpy(domain_name, strtok(NULL, "/"));

    if(domain_info[0] != NULL && domain_info[1] != NULL) {
        free(domain_info[0]);
        free(domain_info[1]);
    }

    domain_info[0] = malloc(sizeof(domain_name));
    strcpy(domain_info[0], domain_name);
    domain_info[1] = malloc(sizeof(domain_more));
    strcpy(domain_info[1], domain_more);
}

void become_http1(char header[]) {
    //http1.1을 http1으로 바꿔준다.
    //keep-alive도 제거
    char* temp1 = NULL;
    char* temp2 = NULL;
    temp1 = strstr(header, "HTTP/1.1");
    if (temp1 > 0) {
        temp1 = temp1 + 7;
        *temp1 = '0';
        temp1 = strstr(header, "Connection: ");
        if (temp1 > 0) {
            temp2 = strstr(temp1, "\r\n");
            sprintf(temp1, "Connection: close%s", temp2);
        }
    }
}

void send_404(int accepted_sockfd) {
    //특별한 경우 강제로 404 페이지를 전송합니다.
    char sagongsa[] = "No page";
    char http_header[256] = {0, };

    sprintf(http_header,
            "HTTP/1.0 404 Not Found\r\n"
            "Date: %s\r\n"
            "Server: %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %ld"
            "\r\n\r\n"
            "%s"
            , get_server_time(), SERVER_INFO, "text/html; charset=utf-8", sizeof(sagongsa), sagongsa);

    write(accepted_sockfd, http_header, strlen(http_header));
    thread_num--;
    printf("\n오류로 인해 임의로 404 페이지를 전송했습니다.\n");
    if (close(accepted_sockfd) == 0) printf("\n현재 전송이 끝나 소켓을 닫았습니다.\n");
}