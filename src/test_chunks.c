#include "marla.h"
#include <string.h>
#include <unistd.h>
#include <apr_pools.h>
#include <dlfcn.h>
#include <apr_dso.h>
#include <parsegraph_user.h>
#include <apr_pools.h>
#include <dlfcn.h>
#include <apr_dso.h>
#include <httpd.h>
#include <http_config.h>
#include <http_protocol.h>
#include <ap_config.h>
#include <apr_dbd.h>
#include <mod_dbd.h>

static marla_WriteResult makeContactPage(struct marla_ChunkedPageRequest* cpr)
{
    char buf[1024];
    int len;

    // Generate the page.
    switch(cpr->handleStage) {
    case 0:
        len = snprintf(buf, sizeof buf, "<!DOCTYPE html>");
        break;
    case 1:
        len = snprintf(buf, sizeof buf, "<html>"
    "<head>"
    "<title>Hello, world!</title>"
    "<style>"
    "body > div {"
    "width: 50%%;"
    "margin: auto;"
    "overflow: hidden;"
    "background: #888;"
    "}"

    ".content {"
    "float: left;"
    "width: 66%%;"
    "}"

    ".title {"
    "background: #44f;"
    "font-size: 4em;"
    "}"

    ".list {"
    "clear:both;float: left; width: 34%%; background:"
    "}"
    "</style>"
    "</head>");
        break;
    case 2:
        len = snprintf(buf, sizeof buf,
    "<body>"
    "<div>"
    "<div class=\"title\">"
    "Hello, world!"
    "</div>"
    "<div class=\"list\" style=\"color:221877\">"
    "<ul>"
    "<li><a href='/home'>Home</a>"
    "<li><a href='/about'>About</a>"
    "<br><br><br>"
    "<br><br><br>"
    "<br><br><br>"
    "<br><br><br>"
    "<br><br><br>"
    "<br><br><br>"
    "</ul>"
    "</div>"
    "<div class=\"content\">"
    "No <b>time</b>!<br><br><br>"
    "<div class=\"container\">");
        break;
    case 3:
        len = snprintf(buf, sizeof buf,
        "<form class=\"well form-horizontal\" method=\"post\" id=\"contact_form\">"
        "<fieldset>"

        "<!-- Form Name -->"
        "<legend>Contact Us Today!</legend>"

        "<!-- Text input-->"

        "<div class=\"form-group\">"
          "<label class=\"col-md-4 control-label\">First Name</label>  "
          "<div class=\"col-md-4 inputGroupContainer\">"
          "<div class=\"input-group\">"
          "<span class=\"input-group-addon\"><i class=\"glyphicon glyphicon-user\"></i></span>\""
          "<input  name=\"first_name\" placeholder=\"First Name\" class=\"form-control\"  type=\"text\">"
            "</div>"
          "</div>"
        "</div>");
        break;
    case 4:
        len = snprintf(buf, sizeof buf,
        "<div class=\"form-group\">"
         " <label class=\"col-md-4 control-label\" >Last Name</label> "
          "  <div class=\"col-md-4 inputGroupContainer\">"
           " <div class=\"input-group\">"
          "<span class=\"input-group-addon\"><i class=\"glyphicon glyphicon-user\"></i></span>"
          "<input name=\"last_name\" placeholder=\"Last Name\" class=\"form-control\"  type=\"text\">"
           " </div>"
          "</div>"
        "</div>"
            "   <div class=\"form-group\">"
          "<label class=\"col-md-4 control-label\">E-Mail</label>"  
           " <div class=\"col-md-4 inputGroupContainer\">"
            "<div class=\"input-group\">"
             "   <span class=\"input-group-addon\"><i class=\"glyphicon glyphicon-envelope\"></i></span>"
          "<input name=\"email\" placeholder=\"E-Mail Address\" class=\"form-control\"  type=\"text\">"
           " </div>"
         " </div>"
        "</div>");
        break;
    case 5:
        len = snprintf(buf, sizeof buf,
            "<div class=\"form-group\">"
             " <label class=\"col-md-4 control-label\">Phone#</label>"  
              "  <div class=\"col-md-4 inputGroupContainer\">"
               " <div class=\"input-group\">"
                   " <span class=\"input-group-addon\"><i class=\"glyphicon glyphicon-earphone\"></i></span>"
              "<input name=\"phone\" placeholder=\"(845)555-1212\" class=\"form-control\" type=\"text\">"
              "  </div>"
              "</div>"
            "</div>");
        break;
    case 6:
        len = snprintf(buf, sizeof buf,
            "<div class=\"form-group\">"
             " <label class=\"col-md-4 control-label\">Address 1</label>"  
              "  <div class=\"col-md-4 inputGroupContainer\">"
               " <div class=\"input-group\">"
                "    <span class=\"input-group-addon\"><i class=\"glyphicon glyphicon-home\"></i></span>"
              "<input name=\"address 1\" placeholder=\"Address 1\" class=\"form-control\" type=\"text\">"
               " </div>"
              "</div>"
            "</div>"
            "<div class=\"form-group\">"
             " <label class=\"col-md-4 control-label\">Address 2</label>"  
              "  <div class=\"col-md-4 inputGroupContainer\">"
               " <div class=\"input-group\">"
                "    <span class=\"input-group-addon\"><i class=\"glyphicon glyphicon-home\"></i></span>"
          );
          break;
    case 7:
        len = snprintf(buf, sizeof buf,
              "<input name=\"address 2\" placeholder=\"Address 2\" class=\"form-control\" type=\"text\">"
               " </div>"
              "</div>"
            "</div>"
            "<div class=\"form-group\">"
             " <label class=\"col-md-4 control-label\">City</label>"  
              "  <div class=\"col-md-4 inputGroupContainer\">"
               " <div class=\"input-group\">"
                "    <span class=\"input-group-addon\"><i class=\"glyphicon glyphicon-home\"></i></span>"
              "<input name=\"city\" placeholder=\"city\" class=\"form-control\"  type=\"text\">"
               " </div>"
              "</div>"
            "</div>");
        break;
    case 8:
        len = snprintf(buf, sizeof buf,
    "<div class=\"form-group\"> "
     " <label class=\"col-md-4 control-label\">State</label>"
      "  <div class=\"input-group\">"
       "     <span class=\"input-group-addon\"><i class=\"glyphicon glyphicon-list\"></i></span>"
        "<select name=\"state\" class=\"form-control selectpicker\" >"
         " <option value=" " >Please select your state</option>"
          "<option>Alabama</option>"
          "<option>Alaska</option>"
          "<option>Arizona</option>"
          "<option>Arkansas</option>"
          "<option>California</option>"
          "<option>Colorado</option>"
          "<option>Connecticut</option>"
          "<option>Delaware</option>"
          "<option>District of Columbia</option>"
          "<option>Florida</option>"
          "<option>Georgia</option>"
          "<option>Hawaii</option>"
          "<option>Idaho</option>"
          "<option>Illinois</option>"
          "<option>Indiana</option>"
          "<option>Iowa</option>"
          "<option>Kansas</option>"
          "<option>Kentucky</option>"
          "<option>Louisiana</option>");
          break;
    case 9:
        len = snprintf(buf, sizeof buf,
          "<option>Maine</option>"
          "<option>Maryland</option>"
          "<option>Massachusetts</option>"
          "<option>Michigan</option>"
          "<option>Minnesota</option>"
          "<option>Mississippi</option>"
          "<option>Missouri</option>"
          "<option>Montana</option>"
          "<option>Nebraska</option>"
          "<option>Nevada</option>"
          "<option>New Hampshire</option>"
          "<option>New Jersey</option>"
          "<option>New Mexico</option>"
          "<option>New York</option>"
          "<option>North Carolina</option>"
          "<option>North Dakota</option>"
          "<option>Ohio</option>"
          "<option>Oklahoma</option>"
          "<option>Oregon</option>"
          "<option>Pennsylvania</option>"
          "<option>Rhode Island</option>"
          "<option>South Carolina</option>"
          "<option>South Dakota</option>"
          "<option>Tennessee</option>"
          "<option>Texas</option>"
          "<option>Utah</option>"
          "<option>Vermont</option>"
          "<option>Virginia</option>"
          "<option>Washington</option>"
          "<option>West Virginia</option>"
          "<option>Wisconsin</option>"
          "<option>Wyoming</option>"
        "</select>"
      "</div>"
    "</div>"
    "</div>");
    break;
    case 10:
        len = snprintf(buf, sizeof buf,
    "<div class=\"form-group\">"
     " <label class=\"col-md-4 control-label\">Zip Code</label>"
      "  <div class=\"col-md-4 inputGroupContainer\">"
       " <div class=\"input-group\">"
        "    <span class=\"input-group-addon\"><i class=\"glyphicon glyphicon-home\"></i></san>"
      "<input name=\"zip\" placeholder=\"Zip Code\" class=\"form-control\"  type=\"text\">"
       " </div>"
    "</div>"
    "</div>"

    "<!-- radio checks -->"
     "<div class=\"form-group\">"
      "                      <label class=\"col-md-4 control-label\">Gender</label>"
       "                     <div class=\"col-md-4\">"
        "                        <div class=\"radio\">"
         "                           <label>"
          "                              <input type=\"radio\" name=\"gender\" value=\"male\" /> Male"
           "                         </label>"
            "                    </div>");
        break;
    case 11:
        len = snprintf(buf, sizeof buf,
                   "             <div class=\"radio\">"
                    "                <label>"
                     "                   <input type=\"radio\" 	name=\"gender\" value=\"female\" /> Female"
            "	     </div>"
                     "           <div class=\"radio\">"
                      "              <label>"
                       "                 <input type=\"radio\" name=\"gender\" value=\"other\" /> Other"
                        "            </label>"
                         "       </div>"
                          "  </div>"
                        "</div>"

    "<input type=submit value=Submit><input>");
        break;
    case 12:
        len = snprintf(buf, sizeof buf,
            "</fieldset>"
            "</form>"
            "</div>"
            "</div>"
            "</div>"
            "</div>"
            "</body>"
            "</html>");
        break;
    default:
        return marla_WriteResult_CONTINUE;
    }

    // Write the generated page.
    int nwritten = marla_Ring_write(cpr->input, buf + cpr->index, len - cpr->index);
    if(nwritten == 0) {
        return marla_WriteResult_DOWNSTREAM_CHOKED;
    }
    if(cpr->index + nwritten < len) {
        if(nwritten > 0) {
            cpr->index += nwritten;
        }
    }
    else {
        // Move to the next stage.
        cpr->handleStage = ((int)cpr->handleStage) + 1;
        cpr->index = 0;
    }

    return marla_WriteResult_CONTINUE;
}

static marla_WriteResult makeShortPage(struct marla_ChunkedPageRequest* cpr)
{
    char buf[1024];
    int len = 1024;
    if(len == cpr->index) {
        return marla_WriteResult_CONTINUE;
    }

    const char* letters = "Chicken Toenails";
    for(int i = 0; i < len; i += 16) {
        memcpy(buf + i, letters, 16);
    }

    // Write the generated page.
    size_t nwritten = marla_Ring_write(cpr->input, buf + cpr->index, len - cpr->index);
    if(nwritten == 0) {
        return marla_WriteResult_DOWNSTREAM_CHOKED;
    }
    cpr->index += nwritten;
    return marla_WriteResult_CONTINUE;
}

AP_DECLARE(void) ap_log_perror_(const char *file, int line, int module_index,
                                int level, apr_status_t status, apr_pool_t *p,
                                const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char exp[512];
    memset(exp, 0, sizeof(exp));
    int len = vsprintf(exp, fmt, args);
    write(3, exp, len);
    va_end(args);
}

static int readDuplexSource(struct marla_Connection* cxn, void* sink, size_t len)
{
    marla_Ring* ring = ((marla_Ring**)cxn->source)[0];
    return marla_Ring_read(ring, sink, len);
}

static int writeDuplexSource(struct marla_Connection* cxn, void* source, size_t len)
{
    return len;
    //marla_Ring* outputRing = ((marla_Ring**)cxn->source)[1];
    //write(0, source, len);
    //return marla_Ring_write(outputRing, source, len);
}

static void destroyDuplexSource(struct marla_Connection* cxn)
{
    marla_Ring** rings = ((marla_Ring**)cxn->source);
    marla_Ring_free(rings[0]);
    marla_Ring_free(rings[1]);
}

static int test_chunk_math(struct marla_Server* server)
{
    // Create the test input.
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    int nwritten = snprintf(source_str, sizeof(source_str) - 1, "GET / HTTP/1.1\r\nHost: localhost:%s\r\n\r\n", server->serverport);
    marla_Ring* ring = marla_Ring_new(marla_BUFSIZE);
    marla_Ring_write(ring, source_str, nwritten);

    marla_Ring* outputRing = marla_Ring_new(marla_BUFSIZE);
    marla_Ring* rings[2];
    rings[0] = ring;
    rings[1] = outputRing;

    // Create the connection.
    marla_Connection* cxn = marla_Connection_new(server);
    cxn->source = &rings;
    cxn->readSource = readDuplexSource;
    cxn->writeSource = writeDuplexSource;
    cxn->destroySource = destroyDuplexSource;
    struct marla_Request* req = marla_Request_new(cxn);
    cxn->current_request = req;
    cxn->latest_request = req;
    ++cxn->requests_in_process;
    marla_ChunkedPageRequest* cpr = marla_ChunkedPageRequest_new(marla_BUFSIZE, req);
    cpr->handler = makeShortPage;
    size_t expectedCumul = 1024 + strlen("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Type: text/html\r\n\r\n") + 5 + 7 + 6;

    for(;;) {
        if(cpr->stage == marla_CHUNK_RESPONSE_DONE) {
            fprintf(stderr, "Breaking. Sanity!\n");
            break;
        }
        if(0 == marla_ChunkedPageRequest_process(cpr)) {
            if(cpr->stage != marla_CHUNK_RESPONSE_DONE) {
                fprintf(stderr, "Breaking. Insanity!\n %d", cpr->stage);
                return 1;
            }
            break;
        }

        int nflushed = 0;
        marla_WriteResult wr = marla_Connection_flush(cxn, &nflushed);
        if(nflushed < 0) {
            fprintf(stderr, "Flush returned negative value.\n");
            marla_Connection_destroy(cxn);
            marla_ChunkedPageRequest_free(cpr);
            return 1;
        }
        if(cxn->flushed > expectedCumul) {
            fprintf(stderr, "Breaking. Crashing nflushed=%ld, expected=%ld, delta=%d\n", cxn->flushed, expectedCumul, (int)expectedCumul-(int)cxn->flushed);
            marla_Connection_destroy(cxn);
            marla_ChunkedPageRequest_free(cpr);
            return 1;
        }
        if(cxn->flushed == expectedCumul) {
            if(cpr->stage != marla_CHUNK_RESPONSE_DONE) {
                fprintf(stderr, "Breaking. Insanity!\n");
                marla_Connection_destroy(cxn);
                marla_ChunkedPageRequest_free(cpr);
                return 1;
            }
            else {
                fprintf(stderr, "Breaking. Sanity!\n");
            }
            break;
        }
    }
    marla_ChunkedPageRequest_free(cpr);
    {
        int nflushed;
        marla_Connection_flush(cxn, &nflushed);
    }
    cxn->flushed = 0;
    expectedCumul = 1024 + strlen("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Type: text/html\r\n\r\n") + 5 + 7 + 6 + 6;

    for(int j = 0; j < 1024; ++j) {
        //fprintf(stderr, "j=%d start\n", j);
        int nflushed;
        marla_Connection_flush(cxn, &nflushed);
        cxn->flushed = 0;
        marla_Ring_write(cxn->output, source_str, j);

        cpr = marla_ChunkedPageRequest_new(marla_BUFSIZE, req);
        cpr->handler = makeShortPage;
        while(cxn->flushed < expectedCumul) {
            if(cpr->stage == marla_CHUNK_RESPONSE_DONE) {
                break;
            }
            int nflushed;
            marla_Connection_flush(cxn, &nflushed);
            marla_ChunkedPageRequest_process(cpr);
            marla_Connection_flush(cxn, &nflushed);
            if(nflushed <= 0) {
                fprintf(stderr, "Flush returned negative value after process.\n");
                marla_Connection_destroy(cxn);
                marla_ChunkedPageRequest_free(cpr);
                return 1;
            }
            if(cxn->flushed - j > expectedCumul + 4) {
                fprintf(stderr, "j=%d, cxn->flushed=%ld, expected=%ld\n", j, cxn->flushed - j, expectedCumul);
                marla_Connection_destroy(cxn);
                marla_ChunkedPageRequest_free(cpr);
                return 1;
            }
        }
        marla_ChunkedPageRequest_free(cpr);
    }

    //Realized prefix length 4 must match the calculated prefix length 5 for 347 bytes avail with slotLen of 262 (7 padding).

    marla_Connection_destroy(cxn);
    return 0;
}

static int test_chunks(struct marla_Server* server)
{
    // Create the test input.
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    int nwritten = snprintf(source_str, sizeof(source_str) - 1, "GET / HTTP/1.1\r\nHost: localhost:%s\r\n\r\n", server->serverport);
    marla_Ring* ring = marla_Ring_new(marla_BUFSIZE);
    marla_Ring_write(ring, source_str, nwritten);

    marla_Ring* outputRing = marla_Ring_new(marla_BUFSIZE);
    marla_Ring* rings[2];
    rings[0] = ring;
    rings[1] = outputRing;

    // Create the connection.
    marla_Connection* cxn = marla_Connection_new(server);
    cxn->source = &rings;
    cxn->readSource = readDuplexSource;
    cxn->writeSource = writeDuplexSource;
    cxn->destroySource = destroyDuplexSource;
    struct marla_Request* req = marla_Request_new(cxn);
    cxn->current_request = req;
    cxn->latest_request = req;
    ++cxn->requests_in_process;
    marla_ChunkedPageRequest* cpr = marla_ChunkedPageRequest_new(marla_BUFSIZE, req);
    cpr->handler = makeShortPage;
    size_t expectedCumul = 1024 + strlen("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Type: text/html\r\n\r\n") + 5 + 7 + 6;

    for(;;) {
        if(cpr->stage == marla_CHUNK_RESPONSE_DONE) {
            fprintf(stderr, "Breaking. Sanity!\n");
            break;
        }
        if(0 == marla_ChunkedPageRequest_process(cpr)) {
            if(cpr->stage != marla_CHUNK_RESPONSE_DONE) {
                fprintf(stderr, "Breaking. Insanity!\n %d", cpr->stage);
                return 1;
            }
            break;
        }

        int nflushed;
        marla_Connection_flush(cxn, &nflushed);
        if(nflushed <= 0) {
            fprintf(stderr, "Flush returned negative value.\n");
            marla_Connection_destroy(cxn);
            marla_ChunkedPageRequest_free(cpr);
            return 1;
        }
        if(cxn->flushed > expectedCumul) {
            fprintf(stderr, "Breaking. Crashing nflushed=%ld, expected=%ld, delta=%d\n", cxn->flushed, expectedCumul, (int)expectedCumul-(int)cxn->flushed);
            marla_Connection_destroy(cxn);
            marla_ChunkedPageRequest_free(cpr);
            return 1;
        }
        if(cxn->flushed == expectedCumul) {
            if(cpr->stage != marla_CHUNK_RESPONSE_DONE) {
                fprintf(stderr, "Breaking. Insanity!\n");
                marla_Connection_destroy(cxn);
                marla_ChunkedPageRequest_free(cpr);
                return 1;
            }
            else {
                fprintf(stderr, "Breaking. Sanity!\n");
            }
            break;
        }
    }
    marla_ChunkedPageRequest_free(cpr);
    {
        int nflushed;
        marla_Connection_flush(cxn, &nflushed);
    }
    cxn->flushed = 0;
    cpr = marla_ChunkedPageRequest_new(marla_BUFSIZE, req);
    cpr->handler = makeShortPage;
    expectedCumul = 1024 + strlen("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Type: text/html\r\n\r\n") + 5 + 7 + 6 + 6;
    for(;;) {
        if(cpr->stage == marla_CHUNK_RESPONSE_DONE) {
            break;
        }
        marla_ChunkedPageRequest_process(cpr);

        int nflushed;
        marla_Connection_flush(cxn, &nflushed);
        if(nflushed <= 0) {
            fprintf(stderr, "Flush returned negative value.\n");
            marla_Connection_destroy(cxn);
            marla_ChunkedPageRequest_free(cpr);
            return 1;
        }
        if(cxn->flushed > expectedCumul) {
            fprintf(stderr, "nflushed=%ld, expected=%ld\n", cxn->flushed, expectedCumul);
            marla_Connection_destroy(cxn);
            marla_ChunkedPageRequest_free(cpr);
            return 1;
        }
        if(cxn->flushed == expectedCumul) {
            break;
        }
    }
    marla_Connection_destroy(cxn);
    marla_ChunkedPageRequest_free(cpr);
    return 0;
}

static int test_chunked_response(struct marla_Server* server)
{
    // Create the test input.
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    int nwritten = snprintf(source_str, sizeof(source_str) - 1, "GET / HTTP/1.1\r\nHost: localhost:%s\r\n\r\n", server->serverport);
    marla_Ring* ring = marla_Ring_new(marla_BUFSIZE);
    marla_Ring_write(ring, source_str, nwritten);

    marla_Ring* outputRing = marla_Ring_new(marla_BUFSIZE);
    marla_Ring* rings[2];
    rings[0] = ring;
    rings[1] = outputRing;

    // Create the connection.
    marla_Connection* cxn = marla_Connection_new(server);
    cxn->source = &rings;
    cxn->readSource = readDuplexSource;
    cxn->writeSource = writeDuplexSource;
    cxn->destroySource = destroyDuplexSource;
    struct marla_Request* req = marla_Request_new(cxn);
    cxn->current_request = req;
    cxn->latest_request = req;
    ++cxn->requests_in_process;
    marla_ChunkedPageRequest* cpr = marla_ChunkedPageRequest_new(marla_BUFSIZE, req);
    cpr->handler = makeContactPage;
    //size_t expectedCumul = 1024 + strlen("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Type: text/html\r\n\r\n") + 5 + 7 + 6;

    for(;;) {
        if(cpr->stage == marla_CHUNK_RESPONSE_DONE) {
            fprintf(stderr, "Breaking. Sanity!\n");
            break;
        }
        if(0 == marla_ChunkedPageRequest_process(cpr)) {
            if(cpr->stage != marla_CHUNK_RESPONSE_DONE) {
                fprintf(stderr, "Breaking. Insanity!\n %d", cpr->stage);
                return 1;
            }
            break;
        }

        int nflushed;
        marla_Connection_flush(cxn, &nflushed);
        if(nflushed <= 0) {
            fprintf(stderr, "Flush returned negative value.\n");
            marla_Connection_destroy(cxn);
            marla_ChunkedPageRequest_free(cpr);
            return 1;
        }
    }
    marla_ChunkedPageRequest_free(cpr);
    {
        int nflushed;
        marla_Connection_flush(cxn, &nflushed);
    }
    marla_Connection_destroy(cxn);
    return 0;
}

int main(int argc, char* argv[])
{
    if(argc < 2) {
        printf("Usage: %s <port>\n", argv[0]);
        return -1;
    }

    struct marla_Server server;
    marla_Server_init(&server);
    strcpy(server.serverport, argv[1]);

    int failed = 0;

    printf("test_chunk_math:");
    if(0 == test_chunk_math(&server)) {
        printf("PASSED\n");
    }
    else {
        printf("FAILED\n");
        ++failed;
    }
    printf("test_chunks:");
    if(0 == test_chunks(&server)) {
        printf("PASSED\n");
    }
    else {
        printf("FAILED\n");
        ++failed;
    }
    printf("test_chunked_response:");
    if(0 == test_chunked_response(&server)) {
        printf("PASSED\n");
    }
    else {
        printf("FAILED\n");
        ++failed;
    }
    marla_Server_free(&server);
    return failed;
}
