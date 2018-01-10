#ifndef marla_INCLUDED
#define marla_INCLUDED

#include <sys/epoll.h>
#include <openssl/ssl.h>
#include <apr_pools.h>

#define marla_BUFSIZE 1024
#define marla_LOGBUFSIZE 32*1024

// ring.c
typedef struct {
char* buf;
unsigned int read_index;
unsigned int write_index;
size_t capacity;
} marla_Ring;

marla_Ring* marla_Ring_new(size_t capacity);
void marla_Ring_free(marla_Ring* ring);
size_t marla_Ring_size(marla_Ring* ring);
size_t marla_Ring_capacity(marla_Ring* ring);
unsigned char marla_Ring_readc(marla_Ring* ring);
int marla_Ring_read(marla_Ring* ring, unsigned char* sink, size_t size);
void marla_Ring_putbackRead(marla_Ring* ring, size_t count);
void marla_Ring_putbackWrite(marla_Ring* ring, size_t count);
void marla_Ring_slot(marla_Ring* ring, void** slot, size_t* slotLen);
size_t marla_Ring_write(marla_Ring* ring, const void* source, size_t size);
void marla_Ring_writec(marla_Ring* ring, unsigned char source);
int marla_Ring_writeStr(marla_Ring* ring, const char* source);
void marla_Ring_writeSlot(marla_Ring* ring, void** slot, size_t* slotLen);
void marla_Ring_readSlot(marla_Ring* ring, void** slot, size_t* slotLen);
void marla_Ring_simplify(marla_Ring* ring);

// client.c
enum marla_RequestReadStage {
marla_CLIENT_REQUEST_READ_FRESH,
marla_BACKEND_REQUEST_FRESH,
marla_CLIENT_REQUEST_READING_METHOD,
marla_CLIENT_REQUEST_PAST_METHOD,
marla_CLIENT_REQUEST_READING_REQUEST_TARGET,
marla_CLIENT_REQUEST_PAST_REQUEST_TARGET,
marla_CLIENT_REQUEST_READING_VERSION,
marla_BACKEND_REQUEST_WRITTEN,
marla_BACKEND_REQUEST_READING_HEADERS,
marla_CLIENT_REQUEST_READING_FIELD,
marla_CLIENT_REQUEST_AWAITING_CONTINUE_WRITE,
marla_CLIENT_REQUEST_AWAITING_UPGRADE_WRITE,
marla_CLIENT_REQUEST_READING_REQUEST_BODY,
marla_CLIENT_REQUEST_WEBSOCKET,
marla_BACKEND_REQUEST_AWAITING_RESPONSE,
marla_BACKEND_REQUEST_READING_RESPONSE_BODY,
marla_CLIENT_REQUEST_READING_CHUNK_SIZE,
marla_CLIENT_REQUEST_READING_CHUNK_BODY,
marla_CLIENT_REQUEST_READING_TRAILER,
marla_BACKEND_REQUEST_READING_RESPONSE_TRAILER,
marla_BACKEND_REQUEST_RESPONDING,
marla_CLIENT_REQUEST_DONE_READING,
marla_BACKEND_REQUEST_DONE_READING
};

enum marla_RequestWriteStage {
marla_CLIENT_REQUEST_WRITE_AWAITING_ACCEPT,
marla_CLIENT_REQUEST_WRITING_UPGRADE,
marla_CLIENT_REQUEST_WRITING_CONTINUE,
marla_CLIENT_REQUEST_WRITING_RESPONSE,
marla_CLIENT_REQUEST_WRITING_WEBSOCKET_RESPONSE,
marla_CLIENT_REQUEST_DONE_WRITING
};

const char* marla_nameRequestReadStage(enum marla_RequestReadStage stage);
const char* marla_nameRequestWriteStage(enum marla_RequestWriteStage stage);

#define MIN_METHOD_LENGTH 3
#define MAX_METHOD_LENGTH 7
#define MAX_FIELD_NAME_LENGTH 64
#define MAX_FIELD_VALUE_LENGTH 255
#define MAX_WEBSOCKET_NONCE_LENGTH 255
#define MAX_URI_LENGTH 255
#define marla_MAX_CHUNK_SIZE 0xFFFFFFFF
#define marla_MAX_CHUNK_SIZE_LINE 10
#define MAX_WEBSOCKET_CONTROL_PAYLOAD 125
#define marla_MESSAGE_IS_CHUNKED -1
#define marla_MESSAGE_LENGTH_UNKNOWN -2
#define marla_MESSAGE_USES_CLOSE -3

// chunks

enum marla_ChunkResponseStage {
marla_CHUNK_RESPONSE_GENERATE = 0,
marla_CHUNK_RESPONSE_HEADER = 1,
marla_CHUNK_RESPONSE_RESPOND = 2,
marla_CHUNK_RESPONSE_TRAILER = 3,
marla_CHUNK_RESPONSE_DONE = 4
};
const char* marla_nameChunkResponseStage(enum marla_ChunkResponseStage stage);

struct marla_ClientRequest;
struct marla_ChunkedPageRequest {
struct marla_ClientRequest* req;
void(*handler)(struct marla_ChunkedPageRequest*);
int handleStage;
int index;
marla_Ring* input;
enum marla_ChunkResponseStage stage;
void* handleData;
};
typedef struct marla_ChunkedPageRequest marla_ChunkedPageRequest;

struct marla_ChunkedPageRequest* marla_ChunkedPageRequest_new(size_t, struct marla_ClientRequest*);
int marla_writeChunk(struct marla_ChunkedPageRequest* cpr, marla_Ring* output);
void marla_measureChunk(size_t slotLen, int avail, size_t* prefix_len, size_t* availUsed);
void marla_ChunkedPageRequest_free(struct marla_ChunkedPageRequest* cpr);
int marla_ChunkedPageRequest_process(struct marla_ChunkedPageRequest* cpr);

enum marla_ClientEvent {
marla_EVENT_HEADER,
marla_EVENT_ACCEPTING_REQUEST,
marla_EVENT_REQUEST_BODY,
marla_EVENT_FORM_FIELD,
marla_EVENT_READ,
marla_EVENT_GENERATE,
marla_EVENT_WEBSOCKET_ESTABLISHED,
marla_EVENT_WEBSOCKET_MUST_READ,
marla_EVENT_WEBSOCKET_MUST_WRITE,
marla_EVENT_WEBSOCKET_RESPOND,
marla_EVENT_RESPOND,
marla_EVENT_WEBSOCKET_CLOSING,
marla_EVENT_WEBSOCKET_CLOSE_REASON,
marla_EVENT_DESTROYING
};
void marla_chunkedRequestHandler(struct marla_ClientRequest* req, enum marla_ClientEvent ev, void* data, int datalen);

struct marla_Connection;

struct marla_BackendSource {
int fd;
};
typedef struct marla_BackendSource marla_BackendSource;

struct marla_ClientRequest {
struct marla_ClientRequest* next_request;
int id;
int statusCode;
char statusLine[MAX_FIELD_VALUE_LENGTH + 1];
struct marla_Connection* cxn;
char method[MAX_METHOD_LENGTH + 1];
char host[MAX_FIELD_VALUE_LENGTH + 1];
char uri[MAX_URI_LENGTH + 1];
char error[marla_BUFSIZE];
char contentType[MAX_FIELD_VALUE_LENGTH + 1];
enum marla_RequestReadStage readStage;
enum marla_RequestWriteStage writeStage;
int expect_upgrade;
int expect_continue;
int expect_trailer;
int expect_websocket;
int close_after_done;
void(*handle)(struct marla_ClientRequest*, enum marla_ClientEvent, void*, int);
void* handleData;
struct marla_ClientRequest* backendPeer;
long int contentLen;
long int totalContentLen;
long int chunkSize;
char websocket_nonce[MAX_WEBSOCKET_NONCE_LENGTH + 1];
char websocket_accept[2 * SHA_DIGEST_LENGTH + 1];
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
int websocket_version;
};
typedef struct marla_ClientRequest marla_ClientRequest;

struct marla_Connection;
struct marla_Server;

marla_ClientRequest* marla_ClientRequest_new(struct marla_Connection* cxn);
void marla_ClientRequest_destroy(marla_ClientRequest*);
void marla_killClientRequest(struct marla_ClientRequest* req, const char* reason, ...);

// connection.c

enum marla_ConnectionStage {
marla_CLIENT_ACCEPTED, /* struct has been created and socket FD has been set */
marla_CLIENT_SECURED, /* SSL has been accepted */
marla_BACKEND_READY, /* Backend ready for requests */
marla_CLIENT_COMPLETE /* Done with connection */
};

const char* marla_nameConnectionStage(enum marla_ConnectionStage);
struct marla_Connection {

// Flags
int shouldDestroy;
int wantsWrite;
int wantsRead;
enum marla_ConnectionStage stage;

struct marla_Server* server;
struct marla_Connection* prev_connection;
struct marla_Connection* next_connection;

// Requests
marla_ClientRequest* current_request;
marla_ClientRequest* latest_request;
size_t requests_in_process;

// Buffers
marla_Ring* input;
marla_Ring* output;

// Source
void* source;
int(*readSource)(struct marla_Connection*, void*, size_t);
int(*writeSource)(struct marla_Connection*, void*, size_t);
void(*acceptSource)(struct marla_Connection*);
int(*shutdownSource)(struct marla_Connection*);
void(*destroySource)(struct marla_Connection*);
int(*describeSource)(struct marla_Connection*, char*, size_t);
struct epoll_event poll;
size_t flushed;
};

typedef struct marla_Connection marla_Connection;
marla_Connection* marla_Connection_new(struct marla_Server* server);
void marla_Connection_putbackRead(marla_Connection* cxn, size_t amount);
void marla_Connection_putbackWrite(marla_Connection* cxn, size_t amount);
int marla_Connection_read(marla_Connection* cxn, unsigned char* sink, size_t requested);
void marla_Connection_destroy(marla_Connection* cxn);
int marla_Connection_flush(marla_Connection* cxn, int* outnflushed);
int marla_Connection_write(marla_Connection* cxn, const void* source, size_t requested);

typedef struct {
int fd;
SSL_CTX* ctx;
SSL* ssl;
} marla_SSLSource;

typedef struct {
int fd;
} marla_ClearTextSource;
int marla_cleartext_init(marla_Connection* cxn, int fd);

// ssl.c
int marla_SSL_init(marla_Connection* cxn, SSL_CTX* ctx, int fd);

// default_request_handler.c
extern void(*default_request_handler)(struct marla_ClientRequest*, enum marla_ClientEvent, void*, int);

// WebSocket
int marla_writeWebSocket(struct marla_ClientRequest* req, unsigned char* data, int dataLen);
int marla_readWebSocket(struct marla_ClientRequest* req, unsigned char* data, int dataLen);
void marla_putbackWebSocket(struct marla_ClientRequest* req, int dataLen);
int marla_writeWebSocketHeader(struct marla_ClientRequest* req, unsigned char opcode, uint64_t frameLen);
void marla_default_websocket_handler(struct marla_ClientRequest* req, enum marla_ClientEvent ev, void* data, int datalen);

int marla_backendRead(marla_Connection* cxn);
int marla_backendWrite(marla_Connection* cxn);
void marla_Backend_init(marla_Connection* cxn, int fd);
void marla_Backend_enqueue(marla_Connection* cxn, marla_ClientRequest* req);
int marla_clientRead(marla_Connection* cxn);
int marla_clientAccept(marla_Connection* cxn);
int marla_clientWrite(marla_Connection* cxn);

// Server

enum marla_ServerModuleEvent {
marla_EVENT_SERVER_MODULE_START,
marla_EVENT_SERVER_MODULE_STOP
};

enum marla_ServerHookStatus {
marla_SERVER_HOOK_STATUS_OK = 0,
marla_SERVER_HOOK_STATUS_CLOSE = -1,
marla_SERVER_HOOK_STATUS_COMPLETE = 1
};

struct marla_HookEntry {
enum marla_ServerHookStatus(*hookFunc)(struct marla_ClientRequest* req, void*);
void* hookData;
struct marla_HookEntry* prevHook;
struct marla_HookEntry* nextHook;
};

struct marla_HookList {
struct marla_HookEntry* firstHook;
struct marla_HookEntry* lastHook;
};

enum marla_ServerHook {
marla_SERVER_HOOK_ROUTE = 0,
marla_SERVER_HOOK_WEBSOCKET = 1,
marla_SERVER_HOOK_MAX = 2
};

struct marla_ServerModule;

enum marla_ServerStatus {
marla_SERVER_STOPPED = 0,
marla_SERVER_STARTED = 1,
marla_SERVER_WAITING_FOR_INPUT = 2,
marla_SERVER_WAITING_FOR_LOCK = 3,
marla_SERVER_PROCESSING = 4,
marla_SERVER_DESTROYING = 5,
};

const char* marla_nameServerStatus(enum marla_ServerStatus);

struct marla_Server {

struct marla_Connection* first_connection;
struct marla_Connection* last_connection;

int wantsLogWrite;
int logfd;
marla_Ring* log;
char logbuf[4096];
char logaddress[1024];
char serverport[64];
char backendport[64];
const char* backendPort;
pthread_mutex_t server_mutex;
volatile enum marla_ServerStatus server_status;
volatile int efd;
volatile int sfd;
int backendfd;
marla_Connection* backend;
pthread_t terminal_thread;
struct marla_ServerModule* first_module;
struct marla_ServerModule* last_module;
struct marla_HookList hooks[marla_SERVER_HOOK_MAX];
};

typedef struct marla_Server marla_Server;

struct marla_ServerModule {
const char* moduleDef;
void* moduleHandle;
void(*moduleFunc)(struct marla_Server*, enum marla_ServerModuleEvent);
struct marla_ServerModule* nextModule;
struct marla_ServerModule* prevModule;
};
void marla_Server_init(struct marla_Server* server);
void marla_Server_flushLog(struct marla_Server* server);
void marla_Server_free(struct marla_Server* server);
void marla_Server_invokeHook(struct marla_Server* server, enum marla_ServerHook serverHook, struct marla_ClientRequest* req);
int marla_Server_removeHook(struct marla_Server* server, enum marla_ServerHook serverHook, enum marla_ServerHookStatus(*hookFunc)(struct marla_ClientRequest* req, void*), void* hookData);
void marla_Server_addHook(struct marla_Server* server, enum marla_ServerHook serverHook, enum marla_ServerHookStatus(*hookFunc)(struct marla_ClientRequest* req, void*), void* hookData);
const char* marla_nameClientEvent(enum marla_ClientEvent ev);

void marla_Server_log(struct marla_Server* server, const char* output, size_t len);
void marla_logEntercf(marla_Server* server, const char* category, const char* fmt, ...);
void marla_logEnterc(marla_Server* server, const char* category, const char* message);
void marla_logEnter(marla_Server* server, const char* message);
void marla_logLeave(marla_Server* server, const char* message);
void marla_logLeavec(marla_Server* server, const char* category, const char* message);
void marla_logLeavef(marla_Server* server, const char* fmt, ...);
void marla_logReset(marla_Server* server, const char* message);
void marla_logResetc(marla_Server* server, const char* category, const char* message);
void marla_logMessagec(marla_Server* server, const char* category, const char* message);
void marla_logMessage(marla_Server* server, const char* message);
void marla_logMessagecf(marla_Server* server, const char* category, const char* fmt, ...);
void marla_logMessagef(marla_Server* server, const char* fmt, ...);
void marla_logEnterf(marla_Server* server, const char* fmt, ...);

#endif // marla_INCLUDED