#define DART_SHARED_LIB
#include "dart_api.h"
#include "dart_native_api.h"
#include "ejdb2.h"
#include "ejdb2cfg.h"

#if defined(__GNUC__)
#define IW_NORETURN __attribute__((noreturn))
#else
#define IW_NORETURN
#endif

typedef void(*WrapperFunction)(Dart_CObject *msg, Dart_Port reply_port);

typedef enum {
  _EJD_ERROR_START = (IW_ERROR_START + 15000UL + 4000),
  EJD_ERROR_CREATE_PORT,                 /**< Failed to create a Dart port (EJD_ERROR_CREATE_PORT) */
  EJD_ERROR_INVALID_NATIVE_CALL_ARGS,    /**< Invalid native function call args (EJD_ERROR_INVALID_NATIVE_CALL_ARGS) */
  EJD_ERROR_INVALID_STATE,               /**< Invalid ejdb2_dart extension state (EJD_ERROR_INVALID_STATE) */
  _EJD_ERROR_END,
} jbr_ecode_t;

struct NativeFunctionLookup {
  const char *name;
  Dart_NativeFunction fn;
};

struct WrapperFunctionLookup {
  const char *name;
  WrapperFunction fn;
};

typedef struct EJDB2Context {
  Dart_Port port;
  EJDB db;
} EJDB2Context;

IW_INLINE Dart_Handle ejd_error_check_propagate(Dart_Handle handle);
Dart_NativeFunction ejd_resolve_name(Dart_Handle name, int argc, bool *auto_setup_scope);
static void ejdb2_ctx_finalizer(void *isolate_callback_data, Dart_WeakPersistentHandle handle, void *peer);

static void ejdb2_port(Dart_NativeArguments arguments);
static void ejdb2_set_handle(Dart_NativeArguments args);
static void ejdb2_explain_rc(Dart_NativeArguments args);

static void ejdb2_port_handler(Dart_Port receive_port, Dart_CObject *msg);
static void ejdb2_open_wrapped(Dart_CObject *msg, Dart_Port reply_port);
static void ejdb2_close_wrapped(Dart_CObject *msg, Dart_Port reply_port);

static struct NativeFunctionLookup k_scoped_functions[] = {
  {"ejdb2_port", ejdb2_port},
  {"ejdb2_set_handle", ejdb2_set_handle},
  {"ejdb2_explain_rc", ejdb2_explain_rc},
  {0, 0}
};

static struct WrapperFunctionLookup k_wrapped_functions[] = {
  {"ejdb2_open_wrapped", ejdb2_open_wrapped},
  {"ejdb2_close_wrapped", ejdb2_close_wrapped},
  {0, 0}
};

#define EJC(h_) ejd_error_check_propagate(h_)

#define EJLIB() EJC(Dart_LookupLibrary(Dart_NewStringFromCString("package:ejdb2_dart/ejdb2_dart.dart")))

#define EJPORTRC(co_, rc_)                 \
  if (rc_) {                              \
    (co_)->type = Dart_CObject_kInt64;    \
    (co_)->value.as_int64 = (rc_);        \
  }

IW_INLINE char *cobject_str(Dart_CObject *co, bool nulls, iwrc *rcp) {
  *rcp = 0;
  if (co) {
    if (co->type == Dart_CObject_kString) {
      return co->value.as_string;
    } else if (nulls && co->type == Dart_CObject_kNull) {
      return 0;
    }
  }
  *rcp = EJD_ERROR_INVALID_NATIVE_CALL_ARGS;
  return 0;
}

IW_INLINE int64_t cobject_int(Dart_CObject *co, bool nulls, iwrc *rcp) {
  *rcp = 0;
  if (co) {
    if (co->type == Dart_CObject_kInt32) {
      return co->value.as_int32;
    } else if (co->type == Dart_CObject_kInt64) {
      return co->value.as_int64;
    } else if (nulls && co->type == Dart_CObject_kNull) {
      return 0;
    }
  }
  *rcp = EJD_ERROR_INVALID_NATIVE_CALL_ARGS;
  return 0;
}

IW_INLINE bool cobject_bool(Dart_CObject *co, bool nulls, iwrc *rcp) {
  *rcp = 0;
  if (co) {
    if (co->type == Dart_CObject_kBool) {
      return co->value.as_bool;
    } else if (nulls && co->type == Dart_CObject_kNull) {
      return false;
    }
  }
  *rcp = EJD_ERROR_INVALID_NATIVE_CALL_ARGS;
  return false;
}

IW_INLINE double cobject_double(Dart_CObject *co, bool nulls, iwrc *rcp) {
  *rcp = 0;
  if (co) {
    if (co->type == Dart_CObject_kDouble) {
      return co->value.as_double;
    } else if (nulls && co->type == Dart_CObject_kNull) {
      return 0;
    }
  }
  *rcp = EJD_ERROR_INVALID_NATIVE_CALL_ARGS;
  return 0;
}

IW_INLINE Dart_Handle ejd_error_check_propagate(Dart_Handle handle) {
  if (Dart_IsError(handle)) {
    Dart_PropagateError(handle);
  }
  return handle;
}

static Dart_Handle ejd_error_object_handle(iwrc rc, const char *msg) {
  Dart_Handle hmsg = Dart_Null();
  if (msg) {
    hmsg = EJC(Dart_NewStringFromCString(msg));
  } else if (rc) {
    const char *explained = iwlog_ecode_explained(rc);
    if (explained) {
      hmsg = EJC(Dart_NewStringFromCString(explained));
    }
  }
  Dart_Handle hrc = EJC(Dart_NewIntegerFromUint64(rc));
  Dart_Handle hclass = EJC(Dart_GetClass(EJLIB(), Dart_NewStringFromCString("EJDB2Error")));
  Dart_Handle args[] = {hrc, hmsg};
  return Dart_New(hclass, Dart_Null(), 2, args);
}

IW_INLINE Dart_Handle ejd_error_rc_create(iwrc rc) {
  const char *msg = iwlog_ecode_explained(rc);
  return Dart_NewUnhandledExceptionError(ejd_error_object_handle(rc, msg));
}

static void ejdb2_port(Dart_NativeArguments args) {
  EJDB2Context *ctx;
  intptr_t ptr = 0;

  Dart_EnterScope();
  Dart_SetReturnValue(args, Dart_Null());

  Dart_Handle self = EJC(Dart_GetNativeArgument(args, 0));
  EJC(Dart_GetNativeInstanceField(self, 0, &ptr));

  if (!ptr) {
    ctx = malloc(sizeof(*ctx));
    if (!ctx) {
      Dart_SetReturnValue(args, ejd_error_rc_create(IW_ERROR_ALLOC));
      goto finish;
    }
    ctx->db = 0;
    ctx->port = Dart_NewNativePort("ejdb2_port_nfn", ejdb2_port_handler, true);
    if (ctx->port == ILLEGAL_PORT) {
      Dart_SetReturnValue(args, ejd_error_rc_create(EJD_ERROR_CREATE_PORT));
      goto finish;
    }
    Dart_SetNativeInstanceField(self, 0, (intptr_t) ctx);
    Dart_NewWeakPersistentHandle(self, ctx, sizeof(*ctx), ejdb2_ctx_finalizer);
  } else {
    ctx = (void *) ptr;
  }
  Dart_SetReturnValue(args, EJC(Dart_NewSendPort(ctx->port)));

finish:
  Dart_ExitScope();
}

static void ejdb2_set_handle(Dart_NativeArguments args) {
  EJDB2Context *ctx;
  intptr_t ptr = 0;

  Dart_EnterScope();
  Dart_SetReturnValue(args, Dart_Null());

  Dart_Handle self = EJC(Dart_GetNativeArgument(args, 0));
  EJC(Dart_GetNativeInstanceField(self, 0, &ptr));

  ctx = (void *) ptr;
  if (!ctx) {
    Dart_SetReturnValue(args, ejd_error_rc_create(EJD_ERROR_INVALID_STATE));
    goto finish;
  }
  EJC(Dart_GetNativeIntegerArgument(args, 1, &ptr));
  ctx->db = (void *) ptr;

finish:
  Dart_ExitScope();
}

static void ejdb2_explain_rc(Dart_NativeArguments args) {
  Dart_EnterScope();
  int64_t llv = 0;
  EJC(Dart_GetNativeIntegerArgument(args, 0, &llv));
  const char *msg = iwlog_ecode_explained((iwrc) llv);
  if (msg) {
    Dart_SetReturnValue(args, EJC(Dart_NewStringFromCString(msg)));
  } else {
    Dart_SetReturnValue(args, Dart_Null());
  }
  Dart_ExitScope();
}

Dart_NativeFunction ejd_resolve_name(Dart_Handle name,
                                     int argc,
                                     bool *auto_setup_scope) {
  if (!Dart_IsString(name) || !auto_setup_scope) {
    return 0;
  }
  Dart_EnterScope();
  const char *cname;
  EJC(Dart_StringToCString(name, &cname));
  for (int i = 0; k_scoped_functions[i].name; ++i) {
    if (strcmp(cname, k_scoped_functions[i].name) == 0) {
      *auto_setup_scope = true;
      Dart_ExitScope();
      return k_scoped_functions[i].fn;
    }
  }
  Dart_ExitScope();
  return 0;
}

static void ejdb2_port_handler(Dart_Port receive_port, Dart_CObject *msg) {
  if (msg->type != Dart_CObject_kArray
      || msg->value.as_array.length < 2
      || msg->value.as_array.values[0]->type != Dart_CObject_kSendPort
      || msg->value.as_array.values[1]->type != Dart_CObject_kString) {
    iwlog_error2("Invalid message recieved");
    return;
  }
  Dart_Port reply_port = msg->value.as_array.values[0]->value.as_send_port.id;
  const char *fname = msg->value.as_array.values[1]->value.as_string;
  for (int i = 0; k_wrapped_functions[i].name; ++i) {
    if (strcmp(k_wrapped_functions[i].name, fname) == 0) {
      k_wrapped_functions[i].fn(msg, reply_port);
      break;
    }
  }
}

static void ejdb2_open_wrapped(Dart_CObject *msg, Dart_Port reply_port) {
  iwrc rc = 0;
  Dart_CObject result, rv1;
  Dart_CObject *rv[1] = {&rv1};

  int c = 0;
  const int pnum = 16;
  EJDB_OPTS opts = {0};
  EJDB db = 0;

  Dart_CObject *params = msg->value.as_array.values[2];
  Dart_CObject **varr = params->value.as_array.values;

  if (!params || params->type != Dart_CObject_kArray || params->value.as_array.length != pnum)  {
    rc = EJD_ERROR_INVALID_NATIVE_CALL_ARGS;
    goto finish;
  }

  // opts.kv.path                         // non null
  // opts.kv.oflags                       // non null
  // opts.kv.wal.enabled                  // non null
  // opts.kv.wal.check_crc_on_checkpoint
  // opts.kv.wal.checkpoint_buffer_sz
  // opts.kv.wal.checkpoint_timeout_sec
  // opts.kv.wal.savepoint_timeout_sec
  // opts.kv.wal.wal_buffer_sz
  // opts.document_buffer_sz
  // opts.sort_buffer_sz
  // opts.http.enabled
  // opts.http.access_token
  // opts.http.bind
  // opts.http.max_body_size
  // opts.http.port
  // opts.http.read_anon

  opts.kv.path = cobject_str(varr[c++], false, &rc);
  RCGO(rc, finish);
  opts.kv.oflags = cobject_int(varr[c++], false, &rc);
  RCGO(rc, finish);
  opts.kv.wal.enabled = cobject_bool(varr[c++], false, &rc);
  RCGO(rc, finish);
  opts.kv.wal.check_crc_on_checkpoint = cobject_bool(varr[c++], true, &rc);
  RCGO(rc, finish);
  opts.kv.wal.checkpoint_buffer_sz = cobject_int(varr[c++], true, &rc);
  RCGO(rc, finish);
  opts.kv.wal.checkpoint_timeout_sec = cobject_int(varr[c++], true, &rc);
  RCGO(rc, finish);
  opts.kv.wal.savepoint_timeout_sec = cobject_int(varr[c++], true, &rc);
  RCGO(rc, finish);
  opts.kv.wal.wal_buffer_sz = cobject_int(varr[c++], true, &rc);
  RCGO(rc, finish);
  opts.document_buffer_sz = cobject_int(varr[c++], true, &rc);
  RCGO(rc, finish);
  opts.sort_buffer_sz = cobject_int(varr[c++], true, &rc);
  RCGO(rc, finish);
  opts.http.enabled = cobject_bool(varr[c++], true, &rc);
  RCGO(rc, finish);
  opts.http.access_token = cobject_str(varr[c++], true, &rc);
  RCGO(rc, finish);
  opts.http.bind = cobject_str(varr[c++], true, &rc);
  RCGO(rc, finish);
  opts.http.max_body_size = cobject_int(varr[c++], true, &rc);
  RCGO(rc, finish);
  opts.http.port = cobject_int(varr[c++], true, &rc);
  RCGO(rc, finish);
  opts.http.read_anon = cobject_bool(varr[c++], true, &rc);
  RCGO(rc, finish);

  opts.http.blocking = false;
  opts.no_wal = !opts.kv.wal.enabled;
  opts.http.access_token_len = opts.http.access_token ? strlen(opts.http.access_token) : 0;

  rc = ejdb_open(&opts, &db);
  RCGO(rc, finish);

  rv1.type = Dart_CObject_kInt64;
  rv1.value.as_int64 = (intptr_t) db;
  result.type = Dart_CObject_kArray;
  result.value.as_array.length = sizeof(rv) / sizeof(rv[0]);
  result.value.as_array.values = rv;

finish:
  if (rc) {
    EJPORTRC(&result, rc);
    if (db) {
      ejdb_close(&db);
    }
  }
  Dart_PostCObject(reply_port, &result);
  return;
}

static void ejdb2_close_wrapped(Dart_CObject *msg, Dart_Port reply_port) {
  iwrc rc = 0;
  Dart_CObject result;
  Dart_CObject *params = msg->value.as_array.values[2];
  Dart_CObject **varr = params->value.as_array.values;
  if (!params || params->type != Dart_CObject_kArray || params->value.as_array.length != 1)  {
    rc = EJD_ERROR_INVALID_NATIVE_CALL_ARGS;
    goto finish;
  }
  intptr_t ptr = cobject_int(varr[0], false, &rc);
  RCGO(rc, finish);
  EJDB db = (EJDB) ptr;
  if (!db) {
    rc = EJD_ERROR_INVALID_NATIVE_CALL_ARGS;
    goto finish;
  }
  rc = ejdb_close(&db);

finish:
  if (rc) {
    EJPORTRC(&result, rc);
  }
  Dart_PostCObject(reply_port, &result);
  return;
}

///////////////////////////////////////////////////////////////////////////
//
///////////////////////////////////////////////////////////////////////////

static const char *_ejd_ecodefn(locale_t locale, uint32_t ecode) {
  if (!(ecode > _EJD_ERROR_START && ecode < _EJD_ERROR_END)) {
    return 0;
  }
  switch (ecode) {
    case EJD_ERROR_CREATE_PORT:
      return "Failed to create a Dart port (EJD_ERROR_CREATE_PORT)";
    case EJD_ERROR_INVALID_NATIVE_CALL_ARGS:
      return "Invalid native function call args (EJD_ERROR_INVALID_NATIVE_CALL_ARGS)";
    case EJD_ERROR_INVALID_STATE:
      return "Invalid native extension state (EJD_ERROR_INVALID_STATE)";
  }
  return 0;
}

DART_EXPORT Dart_Handle ejdb2_dart_Init(Dart_Handle parent_library) {
  fprintf(stderr, "\nejdb2_dart_Init");
  static volatile int ejd_ecodefn_initialized = 0;
  if (__sync_bool_compare_and_swap(&ejd_ecodefn_initialized, 0, 1)) {
    iwlog_register_ecodefn(_ejd_ecodefn);
  }
  if (Dart_IsError(parent_library)) {
    return parent_library;
  }
  Dart_Handle dh = Dart_SetNativeResolver(parent_library, ejd_resolve_name, 0);
  if (Dart_IsError(dh)) {
    return dh;
  }
  return Dart_Null();
}

static void ejdb2_ctx_finalizer(void *isolate_callback_data, Dart_WeakPersistentHandle handle, void *peer) {
  EJDB2Context *ctx = peer;
  if (!ctx) return;
  if (ctx->db) {
    iwrc rc = ejdb_close(&ctx->db);
    if (rc) iwlog_ecode_error3(rc);
    ctx->db = 0;
  }
  if (ctx->port != ILLEGAL_PORT) {
    Dart_CloseNativePort(ctx->port);
  }
  free(ctx);
}
