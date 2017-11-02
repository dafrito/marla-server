#ifndef rainback_INCLUDED
#define rainback_INCLUDED

#include <sys/epoll.h>
#include <openssl/ssl.h>

#define parsegraph_BUFSIZE 1024

typedef struct {
char* buf;
unsigned int read_index;
unsigned int write_index;
size_t capacity;
} parsegraph_Ring;

char parsegraph_Ring_readc(parsegraph_Ring* ring);
int parsegraph_Ring_read(parsegraph_Ring* ring, char* sink, size_t size);
parsegraph_Ring* parsegraph_Ring_new(size_t capacity);
void parsegraph_Ring_free(parsegraph_Ring* ring);
unsigned int parsegraph_Ring_size(parsegraph_Ring* ring);
size_t parsegraph_Ring_capacity(parsegraph_Ring* ring);
void parsegraph_Ring_putback(parsegraph_Ring* ring, size_t count);
void parsegraph_Ring_putbackWrite(parsegraph_Ring* ring, size_t count);
void parsegraph_Ring_slot(parsegraph_Ring* ring, void** slot, size_t* slotLen);
int parsegraph_Ring_write(parsegraph_Ring* ring, const char* source, size_t size);
void parsegraph_Ring_writec(parsegraph_Ring* ring, char source);
void parsegraph_Ring_writeSlot(parsegraph_Ring* ring, void** slot, size_t* slotLen);
void parsegraph_Ring_readSlot(parsegraph_Ring* ring, void** slot, size_t* slotLen);

enum rainback_Status {
    rainback_OK,
    rainback_WRONG_NATURE
};

enum parsegraph_ConnectionNature {
parsegraph_ConnectionNature_UNKNOWN,
parsegraph_ConnectionNature_CLIENT,
parsegraph_ConnectionNature_BACKEND,
parsegraph_ConnectionNature_WEBSOCKET
};

enum parsegraph_RequestStage {
parsegraph_CLIENT_REQUEST_FRESH,
parsegraph_CLIENT_REQUEST_READING_METHOD,
parsegraph_CLIENT_REQUEST_PAST_METHOD,
parsegraph_CLIENT_REQUEST_READING_REQUEST_TARGET,
parsegraph_CLIENT_REQUEST_PAST_REQUEST_TARGET,
parsegraph_CLIENT_REQUEST_READING_VERSION,
parsegraph_CLIENT_REQUEST_READING_FIELD,
parsegraph_CLIENT_REQUEST_AWAITING_CONTINUE_WRITE,
parsegraph_CLIENT_REQUEST_READING_REQUEST_BODY,
parsegraph_CLIENT_REQUEST_READING_CHUNK_SIZE,
parsegraph_CLIENT_REQUEST_READING_CHUNK_BODY,
parsegraph_CLIENT_REQUEST_READING_TRAILER,
parsegraph_CLIENT_REQUEST_RESPONDING,
parsegraph_CLIENT_REQUEST_DONE
};

#define MIN_METHOD_LENGTH 3
#define MAX_METHOD_LENGTH 7
#define MAX_FIELD_NAME_LENGTH 64
#define MAX_FIELD_VALUE_LENGTH 255
#define MAX_URI_LENGTH 255
#define parsegraph_MAX_CHUNK_SIZE 0xFFFFFFFF
#define parsegraph_MAX_CHUNK_SIZE_LINE 10

#define parsegraph_MESSAGE_IS_CHUNKED -1
#define parsegraph_MESSAGE_LENGTH_UNKNOWN -2
#define parsegraph_MESSAGE_USES_CLOSE -3

enum parsegraph_ClientEvent {
parsegraph_EVENT_HEADER,
parsegraph_EVENT_ACCEPTING_REQUEST,
parsegraph_EVENT_REQUEST_BODY,
parsegraph_EVENT_RESPOND,
parsegraph_EVENT_DESTROYING
};

struct parsegraph_Connection;
struct parsegraph_ClientRequest {
struct parsegraph_Connection* cxn;
char method[MAX_METHOD_LENGTH + 1];
char host[MAX_FIELD_VALUE_LENGTH + 1];
char uri[MAX_URI_LENGTH + 1];
long int contentLen;
long int totalContentLen;
long int chunkSize;
enum parsegraph_RequestStage stage;
int expect_continue;
int expect_trailer;
void(*handle)(struct parsegraph_ClientRequest*, enum parsegraph_ClientEvent, void*, int);
struct parsegraph_ClientRequest* next_request;
};
typedef struct parsegraph_ClientRequest parsegraph_ClientRequest;

parsegraph_ClientRequest* parsegraph_ClientRequest_new();
void parsegraph_ClientRequest_destroy(parsegraph_ClientRequest*);

enum parsegraph_ClientStage {
parsegraph_CLIENT_ACCEPTED, /* struct has been created and socket FD has been set */
parsegraph_CLIENT_SECURED, /* SSL has been accepted */
parsegraph_CLIENT_COMPLETE /* Done with connection */
};

struct parsegraph_ClientNature {
int fd;
struct epoll_event poll;
SSL_CTX* ctx;
SSL* ssl;
enum parsegraph_ClientStage stage;
parsegraph_Ring* input;
parsegraph_Ring* output;
parsegraph_ClientRequest* current_request;
parsegraph_ClientRequest* latest_request;
size_t requests_in_process;
};

enum parsegraph_ServerStage {
parsegraph_SERVER_CONNECTED,
parsegraph_SERVER_COMPLETE
};

struct parsegraph_BackendNature {

};

struct parsegraph_WebSocketNature {

};

struct parsegraph_Connection {
int shouldDestroy;
int wantsWrite;
int wantsRead;
enum parsegraph_ConnectionNature type;
union {
struct parsegraph_ClientNature client;
struct parsegraph_BackendNature backend;
struct parsegraph_WebSocketNature ws;
} nature;
};
typedef struct parsegraph_Connection parsegraph_Connection;

parsegraph_Connection* parsegraph_Connection_new();

void parsegraph_Connection_putback(parsegraph_Connection* cxn, size_t amount);
void parsegraph_Connection_putbackWrite(parsegraph_Connection* cxn, size_t amount);
int parsegraph_Connection_read(parsegraph_Connection* cxn, char* sink, size_t requested);
void parsegraph_Connection_handle(parsegraph_Connection* cxn, int event);
void parsegraph_Connection_destroy(parsegraph_Connection* cxn);
int parsegraph_Connection_flush(parsegraph_Connection* cxn, int* outnflushed);
int parsegraph_Connection_write(parsegraph_Connection* cxn, const char* source, size_t requested);

int parsegraph_Client_init(parsegraph_Connection* cxn, SSL_CTX* ctx, int fd);
enum rainback_Status parsegraph_Client_shutdown(parsegraph_Connection* cxn);

parsegraph_Connection* parsegraph_Backend_new(int fd);

#endif // rainback_INCLUDED
