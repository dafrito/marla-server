#include "rainback.h"

static const char* FORM_HTML;

struct HandleData {
void(*default_handler)(struct parsegraph_ClientRequest*, enum parsegraph_ClientEvent, void*, int);
void* default_handleData;
};

static void about_request_handler(struct parsegraph_ClientRequest* req, enum parsegraph_ClientEvent ev, void* data, int datalen)
{
    //fprintf(stderr, "about %s\n", parsegraph_nameClientEvent(ev));
    unsigned char resp[parsegraph_BUFSIZE];
    unsigned char buf[parsegraph_BUFSIZE + 1];
    int nread;
    memset(buf, 0, sizeof buf);
    struct HandleData* hd = (struct HandleData*)req->handleData;
    struct parsegraph_ChunkedPageRequest* cpr;

    switch(ev) {
    case parsegraph_EVENT_GENERATE:
        cpr = req->handleData;
        cpr->message_len = snprintf(cpr->resp, sizeof(cpr->resp),
            "<!DOCTYPE html>"
            "<html><head>"
            "<script>"
            "function run() { WS=new WebSocket(\"ws://localhost:%s/\");"
            "WS.onopen = function() { console.log('Hello'); };"
            "setInterval(function() {"
                "WS.send('Hello');"
                "console.log('written');"
            "}, 1000); }"
            "</script>"
            "</head>"
            "<body onload='run()'>"
            "Hello, <b>world.</b>"
            "<p>"
            "This is request %d from servermod"
            "<p>"
            "<a href='/contact'>Contact us!</a>"
            "</body></html>",
            SERVERPORT ? SERVERPORT : "443",
            req->id
        );
        break;
    default:
        if(hd) {
            void* myData = req->handleData;
            req->handleData = hd->default_handleData;
            hd->default_handler(req, ev, data, datalen);
            hd->default_handleData = req->handleData;
            req->handleData = myData;
        }
        break;
    }
}

static void contact_request_handler(struct parsegraph_ClientRequest* req, enum parsegraph_ClientEvent ev, void* data, int datalen)
{
    unsigned char resp[parsegraph_BUFSIZE];
    unsigned char buf[parsegraph_BUFSIZE + 1];
    int nread;
    memset(buf, 0, sizeof buf);
    struct HandleData* hd = (struct HandleData*)req->handleData;
    struct parsegraph_ChunkedPageRequest* cpr;

    switch(ev) {
    case parsegraph_EVENT_GENERATE:
        cpr = req->handleData;
        const char* str = FORM_HTML;
        cpr->message_len = strlen(str);
        strcpy(cpr->resp, str);
        break;
    default:
        if(hd) {
            void* myData = req->handleData;
            req->handleData = hd->default_handleData;
            hd->default_handler(req, ev, data, datalen);
            hd->default_handleData = req->handleData;
            req->handleData = myData;
        }
        break;
    }
}

static enum parsegraph_ServerHookStatus routeHook(struct parsegraph_ClientRequest* req, void* hookData)
{
    if(!strcmp(req->uri, "/about")) {
        struct HandleData* hd = malloc(sizeof *hd);
        hd->default_handler = req->handle;
        hd->default_handleData = req->handleData;
        req->handleData = hd;
        req->handle = about_request_handler;
    }
    else if(!strcmp(req->uri, "/contact")) {
        struct HandleData* hd = malloc(sizeof *hd);
        hd->default_handler = req->handle;
        hd->default_handleData = req->handleData;
        req->handleData = hd;
        req->handle = contact_request_handler;
    }
    return parsegraph_SERVER_HOOK_STATUS_OK;
}

void module_servermod_init(struct parsegraph_Server* server, enum parsegraph_ServerModuleEvent e)
{
    parsegraph_Server_addHook(server, parsegraph_SERVER_HOOK_ROUTE, routeHook, 0);
    //printf("Module servermod loaded.\n");
}

static const char* FORM_HTML = "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<title>Hello, world!</title>"
    "<style>"
    "body > div {"
    "width: 50%;"
    "margin: auto;"
    "overflow: hidden;"
    "background: #888;"
    "}"

    ".content {"
    "float: left;"
    "width: 66%;"
    "}"

    ".title {"
    "background: #44f;"
    "font-size: 4em;"
    "}"

    ".list {"
    "clear:both;float: left; width: 34%; background:"
    "}"
    "</style>"
    "</head>"
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
    "<div class=\"container\">"

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
    "</div>"

    "<!-- Text input-->"

    "<div class=\"form-group\">"
     " <label class=\"col-md-4 control-label\" >Last Name</label> "
      "  <div class=\"col-md-4 inputGroupContainer\">"
       " <div class=\"input-group\">"
      "<span class=\"input-group-addon\"><i class=\"glyphicon glyphicon-user\"></i></span>"
      "<input name=\"last_name\" placeholder=\"Last Name\" class=\"form-control\"  type=\"text\">"
       " </div>"
      "</div>"
    "</div>"

    "<!-- Text input-->"
        "   <div class=\"form-group\">"
      "<label class=\"col-md-4 control-label\">E-Mail</label>"  
       " <div class=\"col-md-4 inputGroupContainer\">"
        "<div class=\"input-group\">"
         "   <span class=\"input-group-addon\"><i class=\"glyphicon glyphicon-envelope\"></i></span>"
      "<input name=\"email\" placeholder=\"E-Mail Address\" class=\"form-control\"  type=\"text\">"
       " </div>"
     " </div>"
    "</div>"


    "<!-- Text input-->"

    "<div class=\"form-group\">"
     " <label class=\"col-md-4 control-label\">Phone#</label>"  
      "  <div class=\"col-md-4 inputGroupContainer\">"
       " <div class=\"input-group\">"
           " <span class=\"input-group-addon\"><i class=\"glyphicon glyphicon-earphone\"></i></span>"
      "<input name=\"phone\" placeholder=\"(845)555-1212\" class=\"form-control\" type=\"text\">"
      "  </div>"
      "</div>"
    "</div>"

    "<!-- Text input-->"

    "<div class=\"form-group\">"
     " <label class=\"col-md-4 control-label\">Address 1</label>"  
      "  <div class=\"col-md-4 inputGroupContainer\">"
       " <div class=\"input-group\">"
        "    <span class=\"input-group-addon\"><i class=\"glyphicon glyphicon-home\"></i></span>"
      "<input name=\"address 1\" placeholder=\"Address 1\" class=\"form-control\" type=\"text\">"
       " </div>"
      "</div>"
    "</div>"

    "<!-- Text input-->"

    "<div class=\"form-group\">"
     " <label class=\"col-md-4 control-label\">Address 2</label>"  
      "  <div class=\"col-md-4 inputGroupContainer\">"
       " <div class=\"input-group\">"
        "    <span class=\"input-group-addon\"><i class=\"glyphicon glyphicon-home\"></i></span>"
      "<input name=\"address 2\" placeholder=\"Address 2\" class=\"form-control\" type=\"text\">"
       " </div>"
      "</div>"
    "</div>"

    "<!-- Text input-->"

    "<div class=\"form-group\">"
     " <label class=\"col-md-4 control-label\">City</label>"  
      "  <div class=\"col-md-4 inputGroupContainer\">"
       " <div class=\"input-group\">"
        "    <span class=\"input-group-addon\"><i class=\"glyphicon glyphicon-home\"></i></span>"
      "<input name=\"city\" placeholder=\"city\" class=\"form-control\"  type=\"text\">"
       " </div>"
      "</div>"
    "</div>"

    "<!-- Select Basic -->"

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
          "<option>Louisiana</option>"
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
    "</div>"

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
            "                    </div>"
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

    "<input type=submit value=Submit><input>"

    "</fieldset>"
    "</form>"
    "</div>"
    "</div>"
    "</div>"
    "</div>"
    "</body>"
    "</html>";

