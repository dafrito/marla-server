#include "marla.h"

const char* marla_getDefaultStatusLine(int statusCode)
{
    const char* statusLine;
    switch(statusCode) {
    case 100:
        statusLine = "Continue";
        break;
    case 101:
        statusLine = "Switching Protocols";
        break;
    case 200:
        statusLine = "OK";
        break;
    case 201:
        statusLine = "Created";
        break;
    case 202:
        statusLine = "Accepted";
        break;
    case 203:
        statusLine = "Non-Authoritative Information";
        break;
    case 204:
        statusLine = "No Content";
        break;
    case 205:
        statusLine = "Reset Content";
        break;
    case 300:
        statusLine = "Multiple Choices";
        break;
    case 301:
        statusLine = "Moved Permanently";
        break;
    case 302:
        statusLine = "Found";
        break;
    case 303:
        statusLine = "See Other";
        break;
    case 305:
        statusLine = "Use Proxy";
        break;
    case 307:
        statusLine = "Temporary Redirect";
        break;
    case 400:
        statusLine = "Bad Request";
        break;
    case 402:
        statusLine = "Payment Required";
        break;
    case 403:
        statusLine = "Forbidden";
        break;
    case 404:
        statusLine = "Not Found";
        break;
    case 405:
        statusLine = "Method Not Allowed";
        break;
    case 406:
        statusLine = "Not Acceptable";
        break;
    case 408:
        statusLine = "Request Timeout";
        break;
    case 409:
        statusLine = "Conflict";
        break;
    case 410:
        statusLine = "Gone";
        break;
    case 411:
        statusLine = "Length Required";
        break;
    case 413:
        statusLine = "Payload Too Large";
        break;
    case 414:
        statusLine = "URI Too Long";
        break;
    case 415:
        statusLine = "Unsupported Media Type";
        break;
    case 417:
        statusLine = "Expectation Failed";
        break;
    case 426:
        statusLine = "Upgrade Required";
        break;
    case 500:
        statusLine = "Internal Server Error";
        break;
    case 501:
        statusLine = "Not Implemented";
        break;
    case 502:
        statusLine = "Bad Gateway";
        break;
    case 503:
        statusLine = "Service Unavailable";
        break;
    case 504:
        statusLine = "Gateway Timeout";
        break;
    case 505:
        statusLine = "HTTP Version Not Supported";
        break;
    default:
        statusLine = "Unknown";
    }
    return statusLine;
}
