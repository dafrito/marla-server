#include "marla.h"

static marla_WriteResult makeAboutPage(struct marla_ChunkedPageRequest* cpr)
{
    char buf[1024];
    int len;

    marla_Server* server = cpr->req->cxn->server;
    char websocket_url[256];
    memset(websocket_url, 0, sizeof websocket_url);
    int off = sprintf(websocket_url, server->using_ssl ? "wss://" : "ws://");
    if(index(server->serverport, ':') != 0) {
        off += sprintf(websocket_url + off, server->serverport);
    }
    else {
        off += sprintf(websocket_url + off, "localhost:%s", server->serverport);
    }
    off += sprintf(websocket_url + off, "/environment/live");

    // Generate the page.
    switch(cpr->handleStage) {
    case 0:
        len = snprintf(buf, sizeof buf, "<!DOCTYPE html><html><head><meta charset=\"UTF-8\">");
        break;
    case 1:
        len = snprintf(buf, sizeof buf,
            "<script>"
            "function run() {"
                "WS=new WebSocket(\"%s\"); "
                "WS.onopen = function() { WS.send('123456789012345678901234567890123456'); };"
                "WS.onclose = function(c, r) { console.log(c, r); };"
            "};"
            "</script>"
            "</head>"
            "<body onload='run()'>"
            "Hello, <b>world.</b>"
            "<p>"
            "This is request %d from servermod"
            "<p>"
            "<a href='/contact'>Contact us!</a>"
            "</body></html>",
            websocket_url,
            cpr->req->id
        );
        break;
    default:
        return marla_WriteResult_CONTINUE;
    }

    // Write the generated page.
    int nwritten = marla_Ring_write(cpr->input, buf + cpr->index, len - cpr->index);
    if(nwritten == 0) {
        return marla_WriteResult_DOWNSTREAM_CHOKED;
    }
    if(nwritten + cpr->index < len) {
        if(nwritten > 0) {
            cpr->index += nwritten;
        }
    }
    else {
        // Move to the next stage.
        ++cpr->handleStage;
        cpr->index = 0;
    }

    return marla_WriteResult_CONTINUE;
}

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
    if(nwritten + cpr->index < len) {
        if(nwritten > 0) {
            cpr->index += nwritten;
        }
    }
    else {
        // Move to the next stage.
        ++cpr->handleStage;
        cpr->index = 0;
    }

    return marla_WriteResult_CONTINUE;
}

static marla_WriteResult makeCounterPage(struct marla_ChunkedPageRequest* cpr)
{
    char buf[1024];
    int len;

    marla_Server* server = cpr->req->cxn->server;
    char websocket_url[256];
    memset(websocket_url, 0, sizeof websocket_url);
    int off = sprintf(websocket_url, server->using_ssl ? "wss://" : "ws://");
    if(index(server->serverport, ':') != 0) {
        off += sprintf(websocket_url + off, server->serverport);
    }
    else {
        off += sprintf(websocket_url + off, "localhost:%s", server->serverport);
    }
    off += sprintf(websocket_url + off, "/environment/live");

    // Generate the page.
    switch(cpr->handleStage) {
    case 0:
        len = snprintf(buf, sizeof buf, "<!DOCTYPE html>");
        break;
    case 1:
        len = snprintf(buf, sizeof buf, "<html><head><meta charset=\"UTF-8\"><script>function run() { WS=new WebSocket(\"%s\"); WS.onopen = function() { console.log('Default handler.'); }; setInterval(function() { WS.send('Hello'); console.log('written'); }, 1000); }</script></head><body onload='run()'>Hello, <b>world.</b><p>This is request %d</body></html>", websocket_url, cpr->req->id);
        break;
    default:
        return marla_WriteResult_CONTINUE;
    }

    // Write the generated page.
    int nwritten = marla_Ring_write(cpr->input, buf + cpr->index, len - cpr->index);
    if(nwritten == 0) {
        return marla_WriteResult_DOWNSTREAM_CHOKED;
    }
    if(nwritten < len) {
        if(nwritten > 0) {
            cpr->index += nwritten;
        }
    }
    else {
        // Move to the next stage.
        ++cpr->handleStage;
        cpr->index = 0;
    }

    return marla_WriteResult_CONTINUE;
}

void routeHook(struct marla_Request* req, void* hookData)
{
    struct marla_ChunkedPageRequest* cpr;
    if(!strcmp(req->uri, "/about")) {
        cpr = marla_ChunkedPageRequest_new(marla_BUFSIZE, req);
        cpr->handler = makeAboutPage;
        req->handler = marla_chunkedRequestHandler;
        req->handlerData = cpr;
        return;
    }
    if(!strcmp(req->uri, "/contact")) {
        cpr = marla_ChunkedPageRequest_new(marla_BUFSIZE, req);
        cpr->handler = makeContactPage;
        req->handler = marla_chunkedRequestHandler;
        req->handlerData = cpr;
        return;
    }
    if(!strncmp(req->uri, "/user", 5)) {
        // Check for suitable termination
        if(req->uri[5] != 0 && req->uri[5] != '/' && req->uri[5] != '?') {
            // Not really handled.
            return;
        }
        // Install backend handler.
        req->handler = marla_backendClientHandler;
        return;
    }
    int len = strlen("/environment");
    if(!strncmp(req->uri, "/environment", len)) {
        // Check for suitable termination
        if(req->uri[len] != 0 && req->uri[len] != '/' && req->uri[len] != '?' && req->uri[len] != '.' ) {
            // Not really handled.
            return;
        }
        // Install backend handler.
        req->handler = marla_backendClientHandler;
        return;
    }

    const char* bufs[] = {
        "/parsegraph-1.2.js",
        "/parsegraph-widgets-1.2.js",
        "/parsegraph-1.0.js",
        "/parsegraph-widgets-1.0.js",
        "/sga.css",
        "/UnicodeData.txt",
        "/chat.html",
        "/float.html",
        "/woodwork.html",
        "/GLWidget.js",
        "/bible.html",
        "/primes.html",
        "/corporate.html",
        "/alpha.html",
        "/weetcubes.html",
        "/week.html",
        "/calendar.html",
        "/ulam.html",
        "/piers.html",
        "/lisp.html",
        "/anthonylispjs-1.0.js",
        "/chess.html",
        "/ip.html",
        "/todo.html",
        "/start.html",
        "/multislot.html",
        "/esprima.js",
        "/finish.html",
        "/surface.lisp",
        "/terminal.html",
        "/initial.html",
        "/javascript.html",
        "/builder.html",
        "/htmlgraph.html",
        "/audio.html",
        0
    };
    for(int i = 0; ; ++i) {
        const char* path = bufs[i];
        if(!path) {
            break;
        }
        if(!strncmp(req->uri, path, strlen(path))) {
            // Install backend handler.
            req->handler = marla_backendClientHandler;
            return;
        }
    }

    // Default handler.
    cpr = marla_ChunkedPageRequest_new(marla_BUFSIZE, req);
    cpr->handler = makeCounterPage;
    req->handler = marla_chunkedRequestHandler;
    req->handlerData = cpr;
}

void module_servermod_init(struct marla_Server* server, enum marla_ServerModuleEvent e)
{
    switch(e) {
    case marla_EVENT_SERVER_MODULE_START:
        marla_Server_addHook(server, marla_SERVER_HOOK_ROUTE, routeHook, 0);
        //printf("Module servermod loaded.\n");
        break;
    case marla_EVENT_SERVER_MODULE_STOP:
        break;
    }
}
