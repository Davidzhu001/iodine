#include "spnlock.inc"

#include "fio_list.h"
#include "redis_connection.h"
#include "redis_engine.h"
#include "resp.h"

#include <string.h>
/* *****************************************************************************
Data Structures / State
***************************************************************************** */

typedef struct {
  pubsub_engine_s engine;
  resp_parser_pt parser;
  intptr_t sub;
  intptr_t pub;
  void *sub_ctx;
  void *pub_ctx;
  char *address;
  char *port;
  fio_list_s callbacks;
  uint16_t ref;
  volatile uint8_t active;
  volatile uint8_t sub_state;
  volatile uint8_t pub_state;
  spn_lock_i lock;
} redis_engine_s;

typedef struct {
  fio_list_s node;
  void (*callback)(pubsub_engine_s *e, resp_object_s *reply, void *udata);
  void *udata;
  size_t len;
  uint8_t sent;
} callbacks_s;

static int dealloc_engine(redis_engine_s *r) {
  if (!spn_sub(&r->ref, 1)) {
    free(r);
    return -1;
  }
  return 0;
}

/* *****************************************************************************
Writing commands
***************************************************************************** */
static void redis_pub_send(void *e, void *uuid) {
  redis_engine_s *r = e;
  if (r->pub != (intptr_t)uuid)
    return;
  callbacks_s *cb;
  spn_lock(&r->lock);

  if (fio_list_any(r->callbacks)) {
    cb = fio_node2obj(callbacks_s, node, r->callbacks.next);
    if (cb->sent == 0) {
      cb->sent = 1;
      sock_write2(.uuid = r->pub, .buffer = (uint8_t *)(cb + 1),
                  .length = cb->len, .move = 1, .dealloc = SOCK_DEALLOC_NOOP);
    }
  }
  spn_unlock(&r->lock);
  dealloc_engine(r);
}

static void schedule_pub_send(redis_engine_s *r, intptr_t uuid) {
  spn_add(&r->ref, 1);
  defer(redis_pub_send, r, (void *)uuid);
}

/* *****************************************************************************
Engine Bridge
***************************************************************************** */

static void on_message_sub(intptr_t uuid, resp_object_s *msg, void *udata) {
  if (msg->type == RESP_PUBSUB) {
    pubsub_engine_distribute(
            .engine = udata,
            .channel =
                {.name =
                     (char *)resp_obj2str(resp_obj2arr(msg)->array[1])->string,
                 .len = resp_obj2str(resp_obj2arr(msg)->array[1])->len},
            .msg = {
                .data =
                    (char *)resp_obj2str(resp_obj2arr(msg)->array[2])->string,
                .len = resp_obj2str(resp_obj2arr(msg)->array[2])->len});
    return;
  }
  (void)uuid;
}

static void on_message_pub(intptr_t uuid, resp_object_s *msg, void *udata) {
  redis_engine_s *r = udata;
  callbacks_s *cl;
  spn_lock(&r->lock);
  cl = fio_list_pop(callbacks_s, node, r->callbacks);
  spn_unlock(&r->lock);
  if (cl) {
    schedule_pub_send(r, uuid);
    if (cl->callback)
      cl->callback(&r->engine, msg, cl->udata);
    free(cl);
  } else {
    uint8_t buffer[64] = {0};
    size_t len = 63;
    resp_format(NULL, buffer, &len, msg);
    fprintf(stderr,
            "WARN: (RedisEngine) Possible issue, "
            "received unknown message (%lu bytes):\n%s\n",
            len, (char *)buffer);
  }
  (void)uuid;
}

/* *****************************************************************************
Connections
***************************************************************************** */

static inline int connect_sub(redis_engine_s *r) {
  spn_add(&r->ref, 1);
  return (r->sub = facil_connect(.address = r->address, .port = r->port,
                                 .on_connect = redis_create_client_protocol,
                                 .udata = r->sub_ctx,
                                 .on_fail = redis_protocol_cleanup));
}

static inline int connect_pub(redis_engine_s *r) {
  spn_add(&r->ref, 1);
  return (r->pub = facil_connect(.address = r->address, .port = r->port,
                                 .on_connect = redis_create_client_protocol,
                                 .udata = r->pub_ctx,
                                 .on_fail = redis_protocol_cleanup));
}

static void on_close_sub(intptr_t uuid, void *p) {
  redis_engine_s *r = p;
  if (r->sub == uuid && r->active) {
    if (r->sub_state) {
      r->sub_state = 0;
      fprintf(stderr,
              "ERROR: (RedisEngine) redis Sub "
              "connection LOST: %s:%s\n",
              r->address ? r->address : "0.0.0.0", r->port);
    }
    connect_sub(r);
    facil_run_every(50, 1, (void (*)(void *))pubsub_engine_resubscribe,
                    (void *)&r->engine, NULL);
  }
  dealloc_engine(r);
}

static void on_close_pub(intptr_t uuid, void *p) {
  redis_engine_s *r = p;
  if (r->pub == uuid && r->active) {
    connect_pub(r);
    if (r->pub_state) {
      r->pub_state = 0;
      fprintf(stderr,
              "ERROR: (RedisEngine) redis Pub "
              "connection LOST: %s:%s\n",
              r->address ? r->address : "0.0.0.0", r->port);
    }
  }
  dealloc_engine(r);
}

static void on_open_pub(intptr_t uuid, void *e) {
  redis_engine_s *r = e;
  if (r->pub != uuid)
    return;
  fprintf(stderr,
          "INFO: (RedisEngine) redis Pub "
          "connection (re)established: %s:%s\n",
          r->address ? r->address : "0.0.0.0", r->port);
  spn_lock(&r->lock);
  callbacks_s *cb;
  fio_list_for_each(callbacks_s, node, cb, r->callbacks) { cb->sent = 0; }
  spn_unlock(&r->lock);
  schedule_pub_send(r, uuid);
}

static void on_open_sub(intptr_t uuid, void *e) {
  redis_engine_s *r = e;
  if (r->sub != uuid)
    return;
  fprintf(stderr,
          "INFO: (RedisEngine) redis Sub "
          "connection (re)established: %s:%s\n",
          r->address ? r->address : "0.0.0.0", r->port);
  r->sub_state = 1;
  pubsub_engine_resubscribe(&r->engine);
  (void)uuid;
}

/* *****************************************************************************
Callbacks
***************************************************************************** */

/** Should return 0 on success and -1 on failure. */
static int subscribe(const pubsub_engine_s *eng, const char *ch, size_t ch_len,
                     uint8_t use_pattern) {
  redis_engine_s *e = (redis_engine_s *)eng;
  if (!sock_isvalid(e->sub)) {
    return 0;
  }
  resp_object_s *cmd = resp_arr2obj(2, NULL);
  if (!cmd) {
    return -1;
  }
  resp_obj2arr(cmd)->array[0] = use_pattern ? resp_str2obj("PSUBSCRIBE", 10)
                                            : resp_str2obj("SUBSCRIBE", 9);
  resp_obj2arr(cmd)->array[1] =
      (ch ? resp_str2obj(ch, ch_len) : resp_nil2obj());
  void *buffer = malloc(32 + ch_len);
  size_t size = 32 + ch_len;
  if (resp_format(e->parser, buffer, &size, cmd))
    fprintf(stderr, "ERROR: RESP format? size = %lu ch = %lu\n", size, ch_len);
  sock_write2(.uuid = e->sub, .buffer = buffer, .length = size, .move = 1);
  resp_free_object(cmd);
  return 0;
}

/** Return value is ignored. */
static void unsubscribe(const pubsub_engine_s *eng, const char *ch,
                        size_t ch_len, uint8_t use_pattern) {
  redis_engine_s *e = (redis_engine_s *)eng;

  if (!sock_isvalid(e->sub))
    return;
  resp_object_s *cmd = resp_arr2obj(2, NULL);
  if (!cmd)
    return;
  resp_obj2arr(cmd)->array[0] = use_pattern ? resp_str2obj("PUNSUBSCRIBE", 12)
                                            : resp_str2obj("UNSUBSCRIBE", 11);
  resp_obj2arr(cmd)->array[1] =
      (ch ? resp_str2obj(ch, ch_len) : resp_nil2obj());
  void *buffer = malloc(22 + ch_len);
  size_t size = 22 + ch_len;
  if (!resp_format(e->parser, buffer, &size, cmd) && size <= 22 + ch_len)
    sock_write2(.uuid = e->sub, .buffer = buffer, .length = size, .move = 1);
  resp_free_object(cmd);
}

static void deferred_publish2(void *d);
static void deferred_publish(void *data_, void *ignore) {
  if (!defer_fork_is_active())
    return;
  struct {
    redis_engine_s *e;
    resp_object_s *cmd;
  } *data = data_;
  sock_flush(data->e->pub);
  if (!sock_isvalid(data->e->pub))
    goto reschedule;
  redis_engine_send(&data->e->engine, data->cmd, NULL, NULL);
  free(data);
  return;
reschedule:
  connect_pub(data->e);
  if (!sock_isvalid(data->e->pub)) {
    fprintf(stderr,
            "ERROR: (RedisEngine) cannot connect"
            "to Redis (publish) at %s:%s",
            data->e->address, data->e->port);
    facil_run_every(96, 1, deferred_publish2, data_, NULL);
  } else
    defer(deferred_publish, data_, NULL);
  (void)ignore;
}
static void deferred_publish2(void *d) { deferred_publish(d, NULL); }

/** Should return 0 on success and -1 on failure. */
static int publish(const pubsub_engine_s *eng, const char *ch, size_t ch_len,
                   const char *msg, size_t msg_len, uint8_t use_pattern) {
  if (!msg || use_pattern || !ch)
    return -1;
  resp_object_s *cmd = resp_arr2obj(3, NULL);
  if (!cmd)
    return -1;
  struct {
    redis_engine_s *e;
    resp_object_s *cmd;
  } *data = malloc(sizeof(*data));
  data->e = (void *)eng;
  data->cmd = cmd;
  resp_obj2arr(cmd)->array[0] = resp_str2obj("PUBLISH", 7);
  resp_obj2arr(cmd)->array[1] = resp_str2obj(ch, ch_len);
  resp_obj2arr(cmd)->array[2] = resp_str2obj(msg, msg_len);
  deferred_publish(data, NULL);
  return 0;
}

/* *****************************************************************************
Creation
*****************************************************************************
*/

static void initialize_engine(void *en_, void *ig) {
  redis_engine_s *r = (redis_engine_s *)en_;
  (void)ig;
  connect_sub(r);
  connect_pub(r);
}

/**
See the {pubsub.h} file for documentation about engines.

function names speak for themselves ;-)
*/
pubsub_engine_s *redis_engine_create(const char *address, const char *port,
                                     uint8_t ping) {
  if (!port) {
    return NULL;
  }
  size_t addr_len = address ? strlen(address) : 0;
  size_t port_len = strlen(port);
  redis_engine_s *e = malloc(sizeof(*e) + addr_len + port_len + 2);
  *e = (redis_engine_s){
      .engine = {.subscribe = subscribe,
                 .unsubscribe = unsubscribe,
                 .publish = publish},
      .address = (char *)(e + 1),
      .port = ((char *)(e + 1) + addr_len + 1),
      .ref = 1,
      .parser = resp_parser_new(),
      .callbacks = FIO_LIST_INIT_STATIC(e->callbacks),
      .active = 1,
      .sub_state = 1,
      .pub_state = 1,
  };
  if (address)
    memcpy(e->address, address, addr_len);
  else
    e->address = NULL;
  e->address[addr_len] = 0;
  memcpy(e->port, port, port_len);
  e->port[port_len] = 0;

  e->sub_ctx =
      redis_create_context(.parser = e->parser, .on_message = on_message_sub,
                           .on_close = on_close_sub, .on_open = on_open_sub,
                           .udata = e, .ping = ping),

  e->pub_ctx =
      redis_create_context(.parser = e->parser, .on_message = on_message_pub,
                           .on_close = on_close_pub, .on_open = on_open_pub,
                           .udata = e, .ping = ping),

  defer(initialize_engine, e, NULL);
  return (pubsub_engine_s *)e;
}

/**
Sends a Redis message through the engine's connection. The response will be sent
back using the optional callback. `udata` is passed along untouched.
*/
intptr_t redis_engine_send(pubsub_engine_s *e, resp_object_s *data,
                           void (*callback)(pubsub_engine_s *e,
                                            resp_object_s *reply, void *udata),
                           void *udata) {
  if (!e || !data)
    return -1;

  redis_engine_s *r = (redis_engine_s *)e;
  size_t len = 0;
  resp_format(r->parser, NULL, &len, data);
  if (!len)
    return -1;

  callbacks_s *cb = malloc(sizeof(*cb) + len);
  *cb = (callbacks_s){
      .node = FIO_LIST_INIT_STATIC(cb->node),
      .callback = callback,
      .udata = udata,
      .len = len,
  };
  resp_format(r->parser, (uint8_t *)(cb + 1), &len, data);
  spn_lock(&r->lock);
  fio_list_unshift(callbacks_s, node, r->callbacks, cb);
  spn_unlock(&r->lock);
  schedule_pub_send(r, r->pub);
  return 0;
}

/**
See the {pubsub.h} file for documentation about engines.

function names speak for themselves ;-)
*/
void redis_engine_destroy(pubsub_engine_s *engine) {
  redis_engine_s *r = (redis_engine_s *)engine;

  spn_lock(&r->lock);
  callbacks_s *cb;
  fio_list_for_each(callbacks_s, node, cb, r->callbacks) free(cb);
  sock_force_close(r->pub);
  sock_force_close(r->sub);
  r->active = 0;
  if (dealloc_engine(r))
    return;
  spn_unlock(&r->lock);
}
