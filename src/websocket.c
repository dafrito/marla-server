#include "marla.h"
#include <string.h>

/*
 *  %x0 denotes a continuation frame
 *  %x1 denotes a text frame
 *  %x2 denotes a binary frame
 *  %x3-7 are reserved for further non-control frames
 *  %x8 denotes a connection close
 *  %x9 denotes a ping
 *  %xA denotes a pong
 *  %xB-F are reserved for further control frames
 */
int marla_writeWebSocketHeader(struct marla_Request* req, unsigned char opcode, uint64_t frameLen)
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
    int nread = marla_Connection_write(req->cxn, out, outlen);
    if(nread <= 0) {
        return nread;
    }
    if(nread < outlen) {
        marla_Connection_putbackWrite(req->cxn, nread);
        return -1;
    }
    return outlen;
}

int marla_writeWebSocket(struct marla_Request* req, unsigned char* data, int dataLen)
{
    int nwritten = marla_Connection_write(req->cxn, data, dataLen);
    if(nwritten > 0) {
        req->websocketFrameWritten += nwritten;
    }
    return nwritten;
}

int marla_readWebSocket(struct marla_Request* req, unsigned char* data, int dataLen)
{
    if(dataLen > req->websocketFrameLen) {
        dataLen = req->websocketFrameLen;
    }
    int nread = marla_Connection_read(req->cxn, data, dataLen);
    if(nread <= 0) {
        return nread;
    }
    if(req->websocketMask[0] != 0) {
        // Unmask the data.
        for(int i = 0; i < nread; ++i) {
            data[i] = data[i] ^ req->websocketMask[(req->websocketFrameRead + i) % 4];
        }
    }
    req->websocketFrameRead += nread;
    return nread;
}

int marla_WebSocketRemaining(struct marla_Request* req)
{
    return req->websocketFrameLen - req->websocketFrameRead;
}

void marla_putbackWebSocketRead(struct marla_Request* req, int dataLen)
{
    marla_Connection_putbackRead(req->cxn, dataLen);
    req->websocketFrameRead -= dataLen;
}

void marla_putbackWebSocketWrite(struct marla_Request* req, int dataLen)
{
    marla_Connection_putbackWrite(req->cxn, dataLen);
    req->websocketFrameWritten -= dataLen;
}

void marla_closeWebSocketRequest(marla_Request* req, uint16_t closeCode, const char* reason, size_t reasonLen)
{
    if(req->needWebSocketClose || req->doingWebSocketClose) {
        return;
    }
    memcpy(req->websocket_closeReason, reason, reasonLen);
    req->websocket_closeReasonLen = reasonLen;
    req->websocket_closeCode = closeCode;
    req->needWebSocketClose = 1;
    if(req->handler) {
        req->handler(req, marla_EVENT_WEBSOCKET_CLOSING, &closeCode, 2);
        req->handler(req, marla_EVENT_WEBSOCKET_CLOSE_REASON, (void*)reason, reasonLen);
        req->handler(req, marla_EVENT_WEBSOCKET_CLOSE_REASON, 0, 0);
    }
    marla_clientWrite(req->cxn);
}

marla_WriteResult marla_inputWebSocket(marla_Request* req)
{
    marla_Connection* cxn = req->cxn;
    marla_Server* server = cxn->server;
    while(req->readStage == marla_CLIENT_REQUEST_WEBSOCKET) {
        if(req->needWebSocketClose) {
            return -1;
        }
        unsigned char buf[marla_BUFSIZE + 1];
        int nread;
        memset(buf, 0, sizeof buf);

        if(req->websocketFrameLen == 0) {
            nread = marla_Connection_read(req->cxn, req->websocket_frame, sizeof(req->websocket_frame));
            if(nread < 2) {
                if(nread > 0) {
                    memset(req->websocket_frame, 0, sizeof req->websocket_frame);
                    marla_Connection_putbackRead(req->cxn, nread);
                }
                marla_logLeave(server, 0);
                cxn->in_read = 0;
                return -1;
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
                    marla_killRequest(req, "A reserved bit was zero.");
                    marla_logLeave(server, 0);
                    cxn->in_read = 0;
                    return 1;
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
                marla_killRequest(req, "Reserved opcode");
                marla_logLeave(server, 0);
                cxn->in_read = 0;
                return 1;
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
                    marla_killRequest(req, "WebSocket type unrecognized");
                    marla_logLeave(server, 0);
                    cxn->in_read = 0;
                    return 1;
                }
                if(nread < 4) {
                    marla_Connection_putbackRead(req->cxn, nread);
                    memset(req->websocket_frame, 0, sizeof req->websocket_frame);
                    marla_logLeave(server, 0);
                    cxn->in_read = 0;
                    return -1;
                }
                payload_len = be16toh(*(uint16_t*)(req->websocket_frame + 3));
            }
            else if(payload_len == 127) {
                if(req->websocket_type < 0 || req->websocket_type > 2) {
                    marla_killRequest(req, "WebSocket type unrecognized");
                    marla_logLeave(server, 0);
                    cxn->in_read = 0;
                    return 1;
                }
                if(nread < 10) {
                    marla_Connection_putbackRead(req->cxn, nread);
                    memset(req->websocket_frame, 0, sizeof req->websocket_frame);
                    marla_logLeave(server, 0);
                    cxn->in_read = 0;
                    return -1;
                }
                payload_len = be64toh(*(uint64_t*)(req->websocket_frame + 3));
            }
            else if(nread > 2) {
                marla_Connection_putbackRead(req->cxn, nread - 2);
                // Payload length is correct as-is.
            }

            // Read the mask.
            if(mask) {
                nread = marla_Connection_read(req->cxn, (unsigned char*)req->websocketMask, 4);
                if(nread < 4) {
                    if(nread > 0) {
                        marla_Connection_putbackRead(req->cxn, nread);
                    }
                    marla_logLeave(server, 0);
                    cxn->in_read = 0;
                    return -1;
                }
            }

            // Save the frame length.
            req->websocketFrameLen = payload_len;
            req->websocketFrameRead = 0;
        }

        while(!req->needWebSocketClose && req->websocketFrameLen > req->websocketFrameRead) {
            switch(req->websocket_type) {
            case 8:
                // Close frame.
                if(req->websocketFrameRead == 0) {
                    unsigned char code[2];
                    nread = marla_readWebSocket(req, code, 2);
                    if(nread < 2) {
                        if(nread > 0) {
                            marla_Connection_putbackRead(req->cxn, nread);
                        }
                        marla_logLeave(server, 0);
                        cxn->in_read = 0;
                        return -1;
                    }
                    if(req->handler) {
                        req->handler(req, marla_EVENT_WEBSOCKET_CLOSING, code, be16toh(*(uint16_t*)code));
                    }
                }
                if(marla_WebSocketRemaining(req) == 0) {
                    if(req->handler) {
                        req->handler(req, marla_EVENT_WEBSOCKET_CLOSE_REASON, 0, 0);
                    }
                    req->needWebSocketClose = 1;
                    break;
                }

                unsigned char* closeReasonWritten = req->websocket_closeReason + req->websocketFrameRead - 2;
                nread = marla_readWebSocket(req, req->websocket_closeReason + req->websocketFrameRead - 2, req->websocketFrameLen - req->websocketFrameRead - 2);
                if(nread <= 0) {
                    if(req->handler && nread == 0) {
                        req->handler(req, marla_EVENT_WEBSOCKET_CLOSE_REASON, 0, 0);
                    }
                    req->needWebSocketClose = 1;
                    break;
                }
                else if(req->handler) {
                    req->handler(req, marla_EVENT_WEBSOCKET_CLOSE_REASON, closeReasonWritten, nread);
                }

                break;
            case 9:
                // Ping frame.
                nread = marla_readWebSocket(req, req->websocket_ping + req->websocketFrameRead, req->websocketFrameLen - req->websocketFrameRead);
                if(nread <= 0) {
                    marla_logLeave(server, 0);
                    cxn->in_read = 0;
                    return -1;
                }
                break;
            case 10:
                // Pong frame.
                if(req->websocket_pongLen != req->websocketFrameLen) {
                    marla_killRequest(req, "Pong mismatch");
                    marla_logLeave(server, 0);
                    cxn->in_read = 0;
                    return 1;
                }
                nread = marla_readWebSocket(req, buf, sizeof buf);
                if(nread <= 0) {
                    return -1;
                }
                for(int i = 0; i < nread; ++i) {
                    if(req->websocket_pong[i + req->websocketFrameRead] != buf[i]) {
                        // Pong mismatch
                        marla_killRequest(req, "Pong mismatch");
                        marla_logLeave(server, 0);
                        cxn->in_read = 0;
                        return 1;
                    }
                }
                break;
            default:
                // Data frame.
                nread = marla_readWebSocket(req, buf, sizeof buf);
                if(nread <= 0) {
                    if(nread == 0) {
                        if(req->handler) {
                            req->handler(req, marla_EVENT_WEBSOCKET_MUST_READ, 0, 0);
                        }
                    }
                    marla_logLeave(server, 0);
                    cxn->in_read = 0;
                    return 0;
                }
                if(req->handler) {
                    req->handler(req, marla_EVENT_WEBSOCKET_MUST_READ, buf, nread);
                    if(marla_WebSocketRemaining(req) == 0) {
                        req->handler(req, marla_EVENT_WEBSOCKET_MUST_READ, 0, 0);
                    }
                }
            }
        }

        req->websocketFrameLen = 0;
        req->websocketFrameRead = 0;
    }
    return 0;
}

marla_WriteResult marla_outputWebSocket(marla_Request* req)
{
    marla_Connection* cxn = req->cxn;
    marla_Server* server = cxn->server;

    while(req->writeStage == marla_CLIENT_REQUEST_WRITING_WEBSOCKET_RESPONSE) {
        // Write current output.
        if(marla_Ring_size(req->cxn->output) > 0) {
            int nflushed;
            int rv = marla_Connection_flush(req->cxn, &nflushed);
            if(rv <= 0) {
                marla_logLeave(server, "Responder choked.");
                cxn->in_write = 0;
                return rv;
            }
        }

        // Check if a close frame is needed.
        if(req->needWebSocketClose && !req->doingWebSocketClose) {
            if(marla_writeWebSocketHeader(req, 8, 2 + req->websocket_closeReasonLen) < 0) {
                marla_logLeave(server, 0);
                cxn->in_write = 0;
                return -1;
            }
            req->websocketFrameOutLen = 2 + req->websocket_closeReasonLen;
            req->websocketFrameWritten = 0;
            req->doingWebSocketClose = 1;
        }

        // Finish writing the current frame.
        if(req->websocketFrameOutLen != 0) {
            if(req->doingWebSocketClose) {
                if(req->websocketFrameWritten == 0) {
                    uint16_t closeCode = htobe16(req->websocket_closeCode);
                    int nwritten = marla_writeWebSocket(req, ((unsigned char*)&closeCode), 2);
                    if(nwritten < 2) {
                        if(nwritten > 0) {
                            marla_putbackWebSocketWrite(req, nwritten);
                        }
                        marla_logLeave(server, 0);
                        cxn->in_write = 0;
                        return -1;
                    }
                    marla_logMessagef(req->cxn->server, "Wrote close code of %d.", req->websocket_closeCode);
                    if(req->websocketFrameOutLen == req->websocketFrameWritten) {
                        marla_logMessagef(req->cxn->server, "Wrote close frame without any provided reason");
                        goto shutdown;
                    }
                }
                marla_writeWebSocket(req, req->websocket_closeReason + req->websocketFrameWritten - 2, req->websocket_closeReasonLen - req->websocketFrameWritten + 2);
                if(req->websocketFrameOutLen == req->websocketFrameWritten) {
                    goto shutdown;
                }
                marla_logLeavef(req->cxn->server, "Failed to write enter close frame");
                cxn->in_write = 0;
                return -1;
            }
            else if(req->doingPong) {
                int nwritten = marla_writeWebSocket(req, req->websocket_ping + req->websocketFrameWritten, req->websocket_pingLen - req->websocketFrameWritten);
                if(nwritten <= 0) {
                    marla_logLeave(server, 0);
                    cxn->in_write = 0;
                    return -1;
                }
                if(req->websocketFrameOutLen == req->websocketFrameWritten) {
                    req->websocket_pingLen = 0;
                    req->doingPong = 0;
                }
            }
        }

        // Check if a pong frame is needed.
        if(req->websocket_pingLen > 0) {
            req->websocketFrameWritten = 0;
            req->websocketFrameOutLen = req->websocket_pingLen;
            marla_writeWebSocketHeader(req, 10, req->websocketFrameOutLen);
            req->doingPong = 1;
        }

        // Let the handler respond.
        if(req->handler) {
            int result = -1;
            req->handler(req, marla_EVENT_WEBSOCKET_MUST_WRITE, &result, 0);
            if(result == -1) {
                marla_logLeave(server, 0);
                cxn->in_write = 0;
                return -1;
            }
        }
        else {
            marla_logLeave(server, 0);
            cxn->in_write = 0;
            return -1;
        }
    }
    return 0;

shutdown:
    cxn->stage = marla_CLIENT_COMPLETE;
    if(!cxn->shouldDestroy) {
        // Client needs shutdown.
        if(!cxn->shutdownSource || 1 == cxn->shutdownSource(cxn)) {
            cxn->shouldDestroy = 1;
        }
        marla_logLeave(server, 0);
        cxn->in_write = 0;
        return -1;
    }
    marla_logLeave(server, 0);
    cxn->in_write = 0;
    return 1;
}
