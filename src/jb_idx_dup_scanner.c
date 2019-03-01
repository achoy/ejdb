#include "ejdb2_internal.h"

static iwrc jb_idx_consume_eq(struct _JBEXEC *ctx, JQVAL *rv, JB_SCAN_CONSUMER consumer) {
  iwrc rc;
  size_t sz;
  bool matched;
  IWKV_cursor cur;
  int64_t step = 1;
  char dkeybuf[512];
  char buf[JBNUMBUF_SIZE];
  struct _JBMIDX *midx = &ctx->midx;
  JBIDX idx = midx->idx;
  IWKV_cursor_op cursor_reverse_step = IWKV_CURSOR_NEXT;
  midx->cursor_step = IWKV_CURSOR_PREV;

  IWKV_val key = {.data = buf};
  jb_idx_jqval_fill_key(rv, &key);
  key.compound = INT64_MIN;
  if (!key.size) {
    rc = IW_ERROR_ASSERTION;
    iwlog_ecode_error3(rc);
    return rc;
  }
  rc = iwkv_cursor_open(idx->idb, &cur, IWKV_CURSOR_GE, &key);
  if (rc == IWKV_ERROR_NOTFOUND) {
    return consumer(ctx, 0, 0, 0, 0, 0);
  } else RCRET(rc);

  char *dkey = (key.size + IW_VNUMBUFSZ) <= sizeof(dkeybuf) ? dkeybuf : malloc(key.size + IW_VNUMBUFSZ);
  if (!dkey) {
    return iwrc_set_errno(IW_ERROR_ALLOC, errno);
  }
  do {
    if (step > 0) --step;
    else if (step < 0) ++step;
    if (!step) {
      int vns;
      int64_t id;
      rc = iwkv_cursor_copy_key(cur, dkey, key.size + IW_VNUMBUFSZ, &sz);
      RCGO(rc, finish);
      IW_READVNUMBUF64(dkey, id, vns);
      if (key.size != sz - vns || memcmp(dkey + vns, key.data, key.size) != 0) {
        break;
      }
      do {
        step = 1;
        rc = consumer(ctx, 0, id, &step, &matched, 0);
        RCGO(rc, finish);
      } while (step < 0 && !++step);
    }
  } while (step && !(rc = iwkv_cursor_to(cur, step > 0 ? midx->cursor_step : cursor_reverse_step)));

finish:
  if (rc == IWKV_ERROR_NOTFOUND) rc = 0;
  if (dkey != dkeybuf) {
    free(dkey);
  }
  if (cur) {
    iwkv_cursor_close(&cur);
  }
  return consumer(ctx, 0, 0, 0, 0, rc);
}

static int _cmp_jqval(const void *v1, const void *v2) {
  iwrc rc;
  const JQVAL *jqv1 = v1;
  const JQVAL *jqv2 = v2;
  return jql_cmp_jqval_pair(jqv1, jqv2, &rc);
}

static iwrc jb_idx_consume_in_node(struct _JBEXEC *ctx, JQVAL *rv, JB_SCAN_CONSUMER consumer) {
  size_t sz;
  uint64_t id;
  bool matched;
  char buf[JBNUMBUF_SIZE];
  IWKV_val key = {.data = buf};
  int64_t step = 1;
  iwrc rc = 0;

  struct _JBMIDX *midx = &ctx->midx;
  int i = 0;
  JBL_NODE nv = rv->vnode->child;
  if (nv) {
    do {
      if (nv->type >= JBV_I64 && nv->type <= JBV_STR) ++i;
    } while ((nv = nv->next));
  }
  if (i == 0) {
    return consumer(ctx, 0, 0, 0, 0, 0);
  }

  JQVAL *jqvarr = malloc(i * sizeof(*jqvarr));
  if (!jqvarr) {
    return iwrc_set_errno(IW_ERROR_ALLOC, errno);
  }
  i = 0;
  nv = rv->vnode->child;
  do {
    if (nv->type >= JBV_I64 && nv->type <= JBV_STR) {
      JQVAL jqv;
      jql_node_to_jqval(nv, &jqv);
      memcpy(&jqvarr[i++], &jqv, sizeof(jqv));
    }
  } while ((nv = nv->next));

  // Sort jqvarr according to index order, lowest first (asc)
  qsort(jqvarr, i, sizeof(jqvarr[0]), _cmp_jqval);

  // todo


//finish:
//  free(jqvarr);
  return consumer(ctx, 0, 0, 0, 0, rc);
}

static iwrc jb_idx_consume_scan(struct _JBEXEC *ctx, JQVAL *rv, JB_SCAN_CONSUMER consumer) {
  return IW_ERROR_NOT_IMPLEMENTED;
}

iwrc jb_idx_dup_scanner(struct _JBEXEC *ctx, JB_SCAN_CONSUMER consumer) {
  iwrc rc;
  JQP_QUERY *qp = ctx->ux->q->qp;
  struct _JBMIDX *midx = &ctx->midx;
  JQVAL *rval = jql_unit_to_jqval(qp->aux, midx->expr1->right, &rc);
  RCRET(rc);
  switch (midx->expr1->op->value) {
    case JQP_OP_EQ:
      return jb_idx_consume_eq(ctx, rval, consumer);
    case JQP_OP_IN:
      if (rval->type == JQVAL_JBLNODE) {
        return jb_idx_consume_in_node(ctx, rval, consumer);
      } else {
        iwlog_ecode_error3(IW_ERROR_ASSERTION);
        return IW_ERROR_ASSERTION;
      }
      break;
    default:
      break;
  }
  return jb_idx_consume_scan(ctx, rval, consumer);
}
