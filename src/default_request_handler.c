#include "rainback.h"
#include <math.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <openssl/err.h>
#include <openssl/rand.h>

int parsegraph_writeWebSocketHeader(struct parsegraph_ClientRequest* req, unsigned char opcode, uint64_t frameLen)
{
    unsigned char out[7];
    int outlen = 2;

    // Generate the header.
    if(frameLen > 65535) {
        memcpy(out + outlen, &frameLen, sizeof frameLen);
        outlen += sizeof frameLen;
    }
    else if(frameLen > 125) {
        uint16_t fl = (uint16_t)frameLen;
        memcpy(out + outlen, &fl, sizeof fl);
        outlen += sizeof fl;
    }
    else {
        out[0] = (1 << 7) | (opcode % 16);
        out[1] = (unsigned char)frameLen;
    }

    // Write the header.
    int nread = parsegraph_Connection_write(req->cxn, out, outlen);
    if(nread <= 0) {
        return nread;
    }
    if(nread < outlen) {
        parsegraph_Connection_putbackWrite(req->cxn, nread);
        return -1;
    }
    return outlen;
}

int parsegraph_writeWebSocket(struct parsegraph_ClientRequest* req, unsigned char* data, int dataLen)
{
    return parsegraph_Connection_write(req->cxn, data, dataLen);
}

int parsegraph_readWebSocket(struct parsegraph_ClientRequest* req, unsigned char* data, int dataLen)
{
    if(dataLen > req->websocketFrameLen) {
        dataLen = req->websocketFrameLen;
    }
    int nread = parsegraph_Connection_read(req->cxn, data, dataLen);
    if(nread <= 0) {
        return nread;
    }
    if(req->websocketMask) {
        // Unmask the data.
        for(int i = 0; i < nread; ++i) {
            data[i] = data[i] ^ req->websocketMask[(req->websocketFrameRead + i) % 4];
        }
    }
    req->websocketFrameRead += nread;
    return nread;
}

void parsegraph_putbackWebSocket(struct parsegraph_ClientRequest* req, int dataLen)
{
    parsegraph_Connection_putback(req->cxn, dataLen);
    req->websocketFrameRead -= dataLen;
}

const char* SERVERPORT = 0;

void parsegraph_default_websocket_handler(struct parsegraph_ClientRequest* req, enum parsegraph_ClientEvent ev, void* data, int datalen)
{
    unsigned char resp[parsegraph_BUFSIZE];
    unsigned char buf[parsegraph_BUFSIZE + 1];
    int nread;
    memset(buf, 0, sizeof buf);

    switch(ev) {
    case parsegraph_EVENT_READ:
        if(req->websocketFrameLen == 0) {
            nread = parsegraph_Connection_read(req->cxn, req->websocket_frame, sizeof(req->websocket_frame));
            if(nread < 2) {
                if(nread > 0) {
                    memset(req->websocket_frame, 0, sizeof req->websocket_frame);
                    parsegraph_Connection_putback(req->cxn, nread);
                }
                return;
            }

            if(req->websocket_frame[0] << 1 == req->websocket_frame[0]) {
                // The FIN bit was zero.
                req->websocket_fin = 0;
            }
            else {
                // The FIN bit was nonzero.
                req->websocket_fin = 1;
            }

            for(int i = 1; i < 4; ++i) {
                unsigned char c = req->websocket_frame[0] << 1;
                if(c << i == c) {
                    // A reserved bit was zero.
                    goto fail_connection;
                }
            }

            switch(req->websocket_frame[0] % 16) {
            case 0:
                // Continuation frame.
                break;
            case 1:
                // Text frame.
                break;
            case 2:
                // Binary frame.
                break;
            case 8:
                // Close frame.
                break;
            case 9:
                // Ping frame.
                break;
            case 10:
                // Pong frame.
                break;
            default:
                // Reserved opcode.
                goto fail_connection;
            }
            req->websocket_type = req->websocket_frame[0] % 16;

            unsigned char mask;
            if(req->websocket_frame[1] << 1 == req->websocket_frame[1]) {
                // The Mask bit was zero.
                mask = 0;
            }
            else {
                // The Mask bit was nonzero.
                mask = 1;
            }

            // Get the payload length.
            uint64_t payload_len = (unsigned char)(req->websocket_frame[1] << 1) >> 1;
            if(payload_len == 126) {
                if(req->websocket_type < 0 || req->websocket_type > 2) {
                    goto fail_connection;
                }
                if(nread < 4) {
                    parsegraph_Connection_putback(req->cxn, nread);
                    memset(req->websocket_frame, 0, sizeof req->websocket_frame);
                    return;
                }
                payload_len = *(uint16_t*)(req->websocket_frame + 3);
            }
            else if(payload_len == 127) {
                if(req->websocket_type < 0 || req->websocket_type > 2) {
                    goto fail_connection;
                }
                if(nread < 10) {
                    parsegraph_Connection_putback(req->cxn, nread);
                    memset(req->websocket_frame, 0, sizeof req->websocket_frame);
                    return;
                }
                payload_len = *(uint64_t*)(req->websocket_frame + 3);
            }
            else if(nread > 2) {
                parsegraph_Connection_putback(req->cxn, nread - 2);
                // Payload length is correct as-is.
            }

            // Read the mask.
            if(mask) {
                nread = parsegraph_Connection_read(req->cxn, req->websocketMask, 4);
                if(nread < 4) {
                    if(nread > 0) {
                        parsegraph_Connection_putback(req->cxn, nread);
                    }
                    return;
                }
            }

            // Save the frame length.
            printf("Receiving packet.\n");
            req->websocketFrameLen = payload_len;
            req->websocketFrameRead = 0;
        }

        while(req->websocketFrameLen > req->websocketFrameRead) {
            switch(req->websocket_type) {
            case 8:
                // Close frame.
                if(req->websocketFrameRead == 0) {
                    unsigned char code[2];
                    nread = parsegraph_readWebSocket(req, code, 2);
                    if(nread < 2) {
                        if(nread > 0) {
                            parsegraph_Connection_putback(req->cxn, nread);
                        }
                        return;
                    }
                    req->handle(req, parsegraph_EVENT_WEBSOCKET_CLOSING, code, 2);
                    req->needWebSocketClose = 1;
                }

                nread = parsegraph_readWebSocket(req, req->websocket_ping + req->websocketFrameRead, req->websocketFrameLen - req->websocketFrameRead);
                if(nread <= 0) {
                    return;
                }
                req->handle(req, parsegraph_EVENT_WEBSOCKET_CLOSE_REASON, req->websocket_ping + req->websocketFrameRead, nread);
                break;
            case 9:
                // Ping frame.
                nread = parsegraph_readWebSocket(req, req->websocket_ping + req->websocketFrameRead, req->websocketFrameLen - req->websocketFrameRead);
                if(nread <= 0) {
                    return;
                }
                break;
            case 10:
                // Pong frame.
                if(req->websocket_pongLen != req->websocketFrameLen) {
                    goto fail_connection;
                }
                nread = parsegraph_readWebSocket(req, buf, sizeof buf);
                if(nread <= 0) {
                    return;
                }
                for(int i = 0; i < nread; ++i) {
                    if(req->websocket_pong[i + req->websocketFrameRead] != buf[i]) {
                        // Pong mismatch
                        goto fail_connection;
                    }
                }
                break;
            default:
                // Data frame.
                req->handle(req, parsegraph_EVENT_WEBSOCKET_MUST_READ, 0, 0);
            }
        }
        break;
    case parsegraph_EVENT_WEBSOCKET_MUST_READ:
        nread = parsegraph_readWebSocket(req, buf, sizeof buf);
        if(nread <= 0) {
            return;
        }
        fprintf(stdout, "READ: ");
        fflush(stdout);
        write(1, buf, nread);
        fflush(stdout);

        // Do nothing with it; only read.
        break;
    case parsegraph_EVENT_RESPOND:
        // Finish writing the current frame.
        if(req->websocketFrameOutLen != 0) {
            if(req->doingWebSocketClose) {
                int nwritten = parsegraph_writeWebSocket(req, req->websocket_closeReason + req->websocketFrameWritten, req->websocket_closeReasonLen - req->websocketFrameWritten);
                if(nwritten <= 0) {
                    return;
                }
            }
            else if(req->doingPong) {
                int nwritten = parsegraph_writeWebSocket(req, req->websocket_ping + req->websocketFrameWritten, req->websocket_pingLen - req->websocketFrameWritten);
                if(nwritten <= 0) {
                    return;
                }
            }
            else {
                req->handle(req, parsegraph_EVENT_WEBSOCKET_MUST_WRITE, 0, 0);
            }

            if(req->websocketFrameOutLen == req->websocketFrameWritten) {
                if(req->doingWebSocketClose) {
                    // Close the WebSocket connection.
                    req->cxn->stage = parsegraph_CLIENT_COMPLETE;
                    return;
                }
                if(req->doingPong) {
                    req->websocket_pingLen = 0;
                    req->doingPong = 0;
                }
            }
        }

        // Check if a close frame is needed.
        if(req->needWebSocketClose) {
            req->doingWebSocketClose = 1;
            return;
        }

        // Check if a pong frame is needed.
        if(req->websocket_pingLen >= 0) {
            req->websocketFrameWritten = 0;
            req->websocketFrameOutLen = req->websocket_pingLen;
            parsegraph_writeWebSocketHeader(req, 10, req->websocketFrameOutLen);
            req->doingPong = 1;
        }

        // Check if the handler can respond.
        req->handle(req, parsegraph_EVENT_WEBSOCKET_RESPOND, 0, 0);
        break;
    case parsegraph_EVENT_WEBSOCKET_RESPOND:
        printf("Responding.\n");
        break;
    case parsegraph_EVENT_WEBSOCKET_MUST_WRITE:
        goto fail_connection;
    }

    return;
fail_connection:
    req->cxn->stage = parsegraph_CLIENT_COMPLETE;
}

void parsegraph_default_request_handler(struct parsegraph_ClientRequest* req, enum parsegraph_ClientEvent ev, void* data, int datalen)
{
    if(req->stage == parsegraph_CLIENT_REQUEST_WEBSOCKET) {
        parsegraph_default_websocket_handler(req, ev, data, datalen);
        return;
    }

    int* acceptor;
    unsigned char resp[parsegraph_BUFSIZE];
    unsigned char buf[parsegraph_BUFSIZE + 1];
    int nread;
    memset(buf, 0, sizeof buf);
    switch(ev) {
    case parsegraph_EVENT_HEADER:
        break;
    case parsegraph_EVENT_ACCEPTING_REQUEST:
        acceptor = data;
        *acceptor = 1;
        break;
    case parsegraph_EVENT_REQUEST_BODY:
        break;
    case parsegraph_EVENT_READ:
        nread = parsegraph_Connection_read(req->cxn, resp, parsegraph_BUFSIZE);
        if(nread <= 0) {
            return;
        }

        break;
    case parsegraph_EVENT_RESPOND:
        fprintf(stderr, "Responding...\n");
        memset(resp, 0, sizeof(resp));

        const unsigned char* header = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
        int nwritten = parsegraph_Connection_write(req->cxn, header, strlen(header));
        if(nwritten <= 0) {
            return;
        }
        int cs = 0;
        int message_len = snprintf(buf, sizeof buf, "<!DOCTYPE html><html><head><script>function run() { WS=new WebSocket(\"wss://localhost:%s/\"); WS.onopen = function() { alert('Hello'); }; setInterval(function() { WS.send('Hello'); console.log('written'); }, 1000); }</script></head><body onload='run()'>Hello, <b>world.</b><p>This is request %d</body></html>", SERVERPORT ? SERVERPORT : "443", req->id);
        for(int i = 0; i <= message_len; ++i) {
            if((i == message_len) || (i && !(i & (sizeof(buf) - 2)))) {
                if(i & (sizeof(buf) - 2)) {
                    buf[i & (sizeof(buf) - 2)] = 0;
                }
                int rv = snprintf(resp, 1023, "%x\r\n", cs);
                if(rv < 0) {
                    dprintf(3, "Failed to generate response.");
                    req->cxn->stage = parsegraph_CLIENT_COMPLETE;
                    return;
                }
                memcpy(resp + rv, buf, cs);
                resp[rv + cs] = '\r';
                resp[rv + cs + 1] = '\n';

                int nwritten = parsegraph_Connection_write(req->cxn, resp, rv + cs + 2);
                if(nwritten <= 0) {
                    return;
                }
                cs = 0;
            }
            if(i == message_len) {
                break;
            }
            buf[i & (sizeof(buf) - 2)] = buf[i];
            ++cs;
        }

        nwritten = parsegraph_Connection_write(req->cxn, "0\r\n\r\n", 5);
        if(nwritten <= 0) {
            return;
        }

        // Mark connection as complete.
        req->stage = parsegraph_CLIENT_REQUEST_DONE;
        break;
    case parsegraph_EVENT_DESTROYING:
        break;
    }
}

void (*default_request_handler)(struct parsegraph_ClientRequest*, enum parsegraph_ClientEvent, void*, int) = parsegraph_default_request_handler;
