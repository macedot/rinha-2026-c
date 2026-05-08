#include "http_resp.h"

const char *score_full[6] = {
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 35\r\n\r\n{\"approved\":true,\"fraud_score\":0.0}",
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 35\r\n\r\n{\"approved\":true,\"fraud_score\":0.2}",
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 35\r\n\r\n{\"approved\":true,\"fraud_score\":0.4}",
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 36\r\n\r\n{\"approved\":false,\"fraud_score\":0.6}",
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 36\r\n\r\n{\"approved\":false,\"fraud_score\":0.8}",
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 36\r\n\r\n{\"approved\":false,\"fraud_score\":1.0}",
};

const char *resp_ready = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
const char *resp_not_found = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
const char *resp_bad_req = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
const char *resp_internal_err = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
