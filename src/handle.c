
void handle_simple(parsegraph_ClientRequest* req, int event, void* in, size_t len)
{
    // Write the reply.
    char resp[1024];
    memset(resp, 0, 1024);
    int rv = snprintf(resp, 1023, "HTTP/1.1 200 OK\r\n\r\n<html><body>Hello, <b>world.</b><br/>%d<pre>%s</pre></body></html>", strlen(requestHeaders), requestHeaders);
    if(rv < 0) {
        dprintf(3, "Failed to generate response.");
        client->nature.client.stage = parsegraph_CLIENT_COMPLETE;
        return;
    }
    int nwritten = parsegraph_Connection_write(cxn, resp, rv);
    if(nwritten <= 0) {
        common_SSL_return(client, nwritten);
        return;
    }
    if(nwritten < rv) {
        parsegraph_Connection_putbackWrite(cxn->output, 
    }
}
