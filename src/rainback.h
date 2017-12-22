#ifndef rainback_INCLUDED
#define rainback_INCLUDED

#include <sys/epoll.h>
#include <openssl/ssl.h>
#include <apr_pools.h>

#define parsegraph_BUFSIZE 1024

// ring.c
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

// client.c
enum parsegraph_RequestStage {
parsegraph_CLIENT_REQUEST_FRESH,
parsegraph_BACKEND_REQUEST_FRESH,
parsegraph_CLIENT_REQUEST_READING_METHOD,
parsegraph_CLIENT_REQUEST_PAST_METHOD,
parsegraph_CLIENT_REQUEST_READING_REQUEST_TARGET,
parsegraph_CLIENT_REQUEST_PAST_REQUEST_TARGET,
parsegraph_CLIENT_REQUEST_READING_VERSION,
parsegraph_BACKEND_REQUEST_WRITTEN,
parsegraph_BACKEND_REQUEST_READING_HEADERS,
parsegraph_CLIENT_REQUEST_READING_FIELD,
parsegraph_CLIENT_REQUEST_AWAITING_CONTINUE_WRITE,
parsegraph_CLIENT_REQUEST_AWAITING_UPGRADE_WRITE,
parsegraph_CLIENT_REQUEST_READING_REQUEST_BODY,
parsegraph_BACKEND_REQUEST_READING_RESPONSE_BODY,
parsegraph_CLIENT_REQUEST_READING_CHUNK_SIZE,
parsegraph_CLIENT_REQUEST_READING_CHUNK_BODY,
parsegraph_CLIENT_REQUEST_READING_TRAILER,
parsegraph_BACKEND_REQUEST_READING_RESPONSE_TRAILER,
parsegraph_CLIENT_REQUEST_RESPONDING,
parsegraph_BACKEND_REQUEST_RESPONDING,
parsegraph_CLIENT_REQUEST_WEBSOCKET,
parsegraph_BACKEND_REQUEST_DONE_READING,
parsegraph_CLIENT_REQUEST_DONE,
parsegraph_BACKEND_REQUEST_DONE
};

const char* parsegraph_nameRequestStage(enum parsegraph_RequestStage stage);

#define MIN_METHOD_LENGTH 3
#define MAX_METHOD_LENGTH 7
#define MAX_FIELD_NAME_LENGTH 64
#define MAX_FIELD_VALUE_LENGTH 255
#define MAX_WEBSOCKET_NONCE_LENGTH 255
#define MAX_URI_LENGTH 255
#define parsegraph_MAX_CHUNK_SIZE 0xFFFFFFFF
#define parsegraph_MAX_CHUNK_SIZE_LINE 10
#define MAX_WEBSOCKET_CONTROL_PAYLOAD 125

#define parsegraph_MESSAGE_IS_CHUNKED -1
#define parsegraph_MESSAGE_LENGTH_UNKNOWN -2
#define parsegraph_MESSAGE_USES_CLOSE -3

struct parsegraph_ChunkedPageRequest {
unsigned char resp[16*parsegraph_BUFSIZE];
int written;
int message_len;
int stage;
};

enum parsegraph_ClientEvent {
parsegraph_EVENT_HEADER,
parsegraph_EVENT_ACCEPTING_REQUEST,
parsegraph_EVENT_REQUEST_BODY,
parsegraph_EVENT_FORM_FIELD,
parsegraph_EVENT_READ,
parsegraph_EVENT_WEBSOCKET_ESTABLISHED,
parsegraph_EVENT_WEBSOCKET_MUST_READ,
parsegraph_EVENT_WEBSOCKET_MUST_WRITE,
parsegraph_EVENT_WEBSOCKET_RESPOND,
parsegraph_EVENT_GENERATE,
parsegraph_EVENT_RESPOND,
parsegraph_EVENT_WEBSOCKET_CLOSING,
parsegraph_EVENT_WEBSOCKET_CLOSE_REASON,
parsegraph_EVENT_DESTROYING
};

struct parsegraph_Connection;

struct parsegraph_BackendSource {
int fd;
};
typedef struct parsegraph_BackendSource parsegraph_BackendSource;

struct parsegraph_ClientRequest {
int id;
int statusCode;
char statusLine[MAX_FIELD_VALUE_LENGTH + 1];
struct parsegraph_Connection* cxn;
struct parsegraph_Server* server;
char method[MAX_METHOD_LENGTH + 1];
char host[MAX_FIELD_VALUE_LENGTH + 1];
char uri[MAX_URI_LENGTH + 1];
char error[parsegraph_BUFSIZE];
char contentType[MAX_FIELD_VALUE_LENGTH + 1];
long int contentLen;
long int totalContentLen;
long int chunkSize;
enum parsegraph_RequestStage stage;
int expect_upgrade;
unsigned char websocket_nonce[MAX_WEBSOCKET_NONCE_LENGTH + 1];
unsigned char websocket_accept[2 * SHA_DIGEST_LENGTH + 1];
unsigned char websocket_frame[7];
int websocket_pingLen;
unsigned char websocket_ping[MAX_WEBSOCKET_CONTROL_PAYLOAD];
char websocket_pongLen;
unsigned char websocket_pong[MAX_WEBSOCKET_CONTROL_PAYLOAD];
unsigned char websocket_closeReason[MAX_WEBSOCKET_CONTROL_PAYLOAD];
char websocket_closeReasonLen;
int websocket_type;
int websocket_fin;
int needWebSocketClose;
int doingPong;
int doingWebSocketClose;
uint64_t websocketFrameWritten;
uint64_t websocketFrameOutLen;
uint64_t websocketFrameRead;
uint64_t websocketFrameLen;
char websocketOutMask[4];
char websocketMask[4];
int expect_continue;
int websocket_version;
int expect_trailer;
int expect_websocket;
int close_after_done;
void(*handle)(struct parsegraph_ClientRequest*, enum parsegraph_ClientEvent, void*, int);
void* handleData;
struct parsegraph_ClientRequest* next_request;
struct parsegraph_ClientRequest* backendPeer;
};
typedef struct parsegraph_ClientRequest parsegraph_ClientRequest;

struct parsegraph_Connection;
struct parsegraph_Server;

parsegraph_ClientRequest* parsegraph_ClientRequest_new(struct parsegraph_Connection* cxn, struct parsegraph_Server* server);
void parsegraph_ClientRequest_destroy(parsegraph_ClientRequest*);

enum parsegraph_ConnectionStage {
parsegraph_CLIENT_ACCEPTED, /* struct has been created and socket FD has been set */
parsegraph_CLIENT_SECURED, /* SSL has been accepted */
parsegraph_BACKEND_READY, /* Backend ready for requests */
parsegraph_CLIENT_COMPLETE /* Done with connection */
};

const char* parsegraph_nameConnectionStage(enum parsegraph_ConnectionStage);

enum parsegraph_ServerStatus {
parsegraph_SERVER_STOPPED = 0,
parsegraph_SERVER_STARTED = 1,
parsegraph_SERVER_WAITING_FOR_INPUT = 2,
parsegraph_SERVER_WAITING_FOR_LOCK = 3,
parsegraph_SERVER_PROCESSING = 4,
parsegraph_SERVER_DESTROYING = 5,
};

const char* parsegraph_nameServerStatus(enum parsegraph_ServerStatus);

// connection.c
struct parsegraph_Connection {

// Flags
int shouldDestroy;
int wantsWrite;
int wantsRead;
enum parsegraph_ConnectionStage stage;

struct parsegraph_Server* server;
struct parsegraph_Connection* prev_connection;
struct parsegraph_Connection* next_connection;

// Requests
parsegraph_ClientRequest* current_request;
parsegraph_ClientRequest* latest_request;
size_t requests_in_process;

// Buffers
parsegraph_Ring* input;
parsegraph_Ring* output;

// Source
void* source;
int(*readSource)(struct parsegraph_Connection*, void*, size_t);
int(*writeSource)(struct parsegraph_Connection*, void*, size_t);
void(*acceptSource)(struct parsegraph_Connection*);
int(*shutdownSource)(struct parsegraph_Connection*);
void(*destroySource)(struct parsegraph_Connection*);
int(*describeSource)(struct parsegraph_Connection*, char*, size_t);
struct epoll_event poll;
};

typedef struct parsegraph_Connection parsegraph_Connection;
parsegraph_Connection* parsegraph_Connection_new(struct parsegraph_Server* server);
void parsegraph_Connection_putback(parsegraph_Connection* cxn, size_t amount);
void parsegraph_Connection_putbackWrite(parsegraph_Connection* cxn, size_t amount);
int parsegraph_Connection_read(parsegraph_Connection* cxn, char* sink, size_t requested);
void parsegraph_Connection_handle(parsegraph_Connection* cxn, struct parsegraph_Server* server, int event);
void parsegraph_Connection_destroy(parsegraph_Connection* cxn);
int parsegraph_Connection_flush(parsegraph_Connection* cxn, int* outnflushed);
int parsegraph_Connection_write(parsegraph_Connection* cxn, const char* source, size_t requested);

typedef struct {
int fd;
SSL_CTX* ctx;
SSL* ssl;
} parsegraph_SSLSource;

typedef struct {
int fd;
} parsegraph_ClearTextSource;
int parsegraph_cleartext_init(parsegraph_Connection* cxn, int fd);

// ssl.c
int parsegraph_SSL_init(parsegraph_Connection* cxn, SSL_CTX* ctx, int fd);

// default_request_handler.c
extern void(*default_request_handler)(struct parsegraph_ClientRequest*, enum parsegraph_ClientEvent, void*, int);

// WebSocket
int parsegraph_writeWebSocket(struct parsegraph_ClientRequest* req, unsigned char* data, int dataLen);
int parsegraph_readWebSocket(struct parsegraph_ClientRequest* req, unsigned char* data, int dataLen);
void parsegraph_putbackWebSocket(struct parsegraph_ClientRequest* req, int dataLen);
int parsegraph_writeWebSocketHeader(struct parsegraph_ClientRequest* req, unsigned char opcode, uint64_t frameLen);
void parsegraph_default_websocket_handler(struct parsegraph_ClientRequest* req, enum parsegraph_ClientEvent ev, void* data, int datalen);
static void parsegraph_default_request_handler(struct parsegraph_ClientRequest* req, enum parsegraph_ClientEvent ev, void* data, int datalen);

void parsegraph_backendRead(parsegraph_Connection* cxn);
void parsegraph_backendWrite(parsegraph_Connection* cxn);
void parsegraph_Backend_init(parsegraph_Connection* cxn, int fd);
void parsegraph_Backend_enqueue(parsegraph_Connection* cxn, parsegraph_ClientRequest* req);
void parsegraph_backendWrite(parsegraph_Connection* cxn);
void parsegraph_backendRead(parsegraph_Connection* cxn);

enum parsegraph_ServerModuleEvent {
parsegraph_EVENT_SERVER_MODULE_START,
parsegraph_EVENT_SERVER_MODULE_STOP
};

enum parsegraph_ServerHookStatus {
parsegraph_SERVER_HOOK_STATUS_OK = 0,
parsegraph_SERVER_HOOK_STATUS_CLOSE = -1,
parsegraph_SERVER_HOOK_STATUS_COMPLETE = 1
};

struct parsegraph_HookEntry {
enum parsegraph_ServerHookStatus(*hookFunc)(struct parsegraph_ClientRequest* req, void*);
void* hookData;
struct parsegraph_HookEntry* prevHook;
struct parsegraph_HookEntry* nextHook;
};

struct parsegraph_HookList {
struct parsegraph_HookEntry* firstHook;
struct parsegraph_HookEntry* lastHook;
};

enum parsegraph_ServerHook {
parsegraph_SERVER_HOOK_ROUTE = 0,
parsegraph_SERVER_HOOK_WEBSOCKET = 1,
parsegraph_SERVER_HOOK_MAX = 2
};

struct parsegraph_ServerModule;

struct parsegraph_Server {

struct parsegraph_Connection* first_connection;
struct parsegraph_Connection* last_connection;

char serverport[64];
char backendport[64];
const char* backendPort;
pthread_mutex_t server_mutex;
volatile enum parsegraph_ServerStatus server_status;
volatile int efd;
volatile int sfd;
int backendfd;
parsegraph_Connection* backend;
pthread_t terminal_thread;
struct parsegraph_ServerModule* first_module;
struct parsegraph_ServerModule* last_module;
struct parsegraph_HookList hooks[parsegraph_SERVER_HOOK_MAX];
};

struct parsegraph_ServerModule {
const char* moduleDef;
void* moduleHandle;
void(*moduleFunc)(struct parsegraph_Server*, enum parsegraph_ServerModuleEvent);
struct parsegraph_ServerModule* nextModule;
struct parsegraph_ServerModule* prevModule;
};
void parsegraph_Server_init(struct parsegraph_Server* server);
void parsegraph_Server_invokeHook(struct parsegraph_Server* server, enum parsegraph_ServerHook serverHook, struct parsegraph_ClientRequest* req);
int parsegraph_Server_removeHook(struct parsegraph_Server* server, enum parsegraph_ServerHook serverHook, enum parsegraph_ServerHookStatus(*hookFunc)(struct parsegraph_ClientRequest* req, void*), void* hookData);
void parsegraph_Server_addHook(struct parsegraph_Server* server, enum parsegraph_ServerHook serverHook, enum parsegraph_ServerHookStatus(*hookFunc)(struct parsegraph_ClientRequest* req, void*), void* hookData);
const char* parsegraph_nameClientEvent(enum parsegraph_ClientEvent ev);

extern const char* SERVERPORT;

#endif // rainback_INCLUDED
