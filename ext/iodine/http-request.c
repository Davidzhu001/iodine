#define _GNU_SOURCE
#include "http-request.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
// we declare because we need them... implementation comes later.
static struct HttpRequest* request_new(struct Server* server, int sockfd);
static void request_destroy(struct HttpRequest* self);
static void request_first(struct HttpRequest* self);
static int request_next(struct HttpRequest* self);
static int request_find(struct HttpRequest* self, char* const name);
static char* request_name(struct HttpRequest* self);
static char* request_value(struct HttpRequest* self);

const struct HttpRequestClass HttpRequest = {
    // retures an new heap allocated request object
    .new = request_new,
    // releases the resources used by a request object and frees it's memory.
    .destroy = request_destroy,

    // Header handling

    /// restarts the header itteration
    .first = request_first,
    /// moves to the next header. returns 0 if the end of the list was
    /// reached.
    .next = request_next,
    /// finds a specific header matching the requested string.
    /// all headers are lower-case, so the string should be lower case.
    /// returns 0 if the header couldn't be found.
    .find = request_find,
    /// returns the name of the current header in the itteration cycle.
    .name = request_name,
    /// returns the value of the current header in the itteration cycle.
    .value = request_value,
};

////////////////
// The Request object implementation

// The constructor
static struct HttpRequest* request_new(struct Server* server, int sockfd) {
  struct HttpRequest* req = malloc(sizeof(struct HttpRequest));
  *req = (struct HttpRequest){.sockfd = sockfd, .server = server};
  return req;
}

// the destructor
static void request_destroy(struct HttpRequest* self) {
  if (!self)
    return;
  if (self->body_file)
    fclose(self->body_file);
  free(self);
}

// implement the following request handlers:

static void request_first(struct HttpRequest* self) {
  self->private.pos = 0;
};
static int request_next(struct HttpRequest* self) {
  // repeat the following 2 times, as it's a name + value pair
  for (int i = 0; i < 2; i++) {
    // move over characters
    while (self->private.pos < self->private.max &&
           self->private.header_hash[self->private.pos])
      self->private.pos++;
    // move over NULL
    while (self->private.pos < self->private.max &&
           !self->private.header_hash[self->private.pos])
      self->private.pos++;
  }
  if (self->private.pos == self->private.max)
    return 0;
  return 1;
}
static int request_find(struct HttpRequest* self, char* const name) {
  self->private.pos = 0;
  do {
    if (!strcmp(self->private.header_hash + self->private.pos, name))
      return 1;
  } while (request_next(self));
  return 0;
}
static char* request_name(struct HttpRequest* self) {
  if (!self->private.header_hash[self->private.pos])
    return NULL;
  return self->private.header_hash + self->private.pos;
};
static char* request_value(struct HttpRequest* self) {
  if (!self->private.header_hash[self->private.pos])
    return NULL;
  int pos = self->private.pos;
  // move over characters
  while (pos < self->private.max && self->private.header_hash[pos])
    pos++;
  // move over NULL
  while (pos < self->private.max && !self->private.header_hash[pos])
    pos++;
  if (self->private.pos == self->private.max)
    return 0;
  return self->private.header_hash + pos;
};
