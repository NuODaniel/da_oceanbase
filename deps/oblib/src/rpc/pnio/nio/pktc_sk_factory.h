static int pktc_sk_check_connect(pktc_sk_t* s) {
  int err = 0;
  if (s->conn_ok) {
  } else if (!skt(s, OUT)) {
    rk_info("sock not ready: %p", s);
    err = EAGAIN;
  } else if (0 != check_connect_result(s->fd)) {
    rk_info("sock not ready: %p", s);
    err = EIO;
  } else {
    s->conn_ok = 1;
    rk_info("sock connect OK: %p %s", s, T2S(sock_fd, s->fd));
    pktc_t* pc = structof(s->fty, pktc_t, sf);
    if (pc->dispatch_id != 0) {
      err = send_dispatch_handshake(s->fd, (const char*)&(pc->dispatch_id), sizeof(pc->dispatch_id));
    }
  }
  return err;
}

static int pktc_sk_handle_event(pktc_sk_t* s) {
  return pktc_sk_check_connect(s)?: pktc_sk_handle_event_ready(s);
}

static void* pktc_sk_alloc(int64_t sz) { return salloc(sz); }
static void pktc_sk_free(void* p) { sfree(p); }

static int pktc_sk_init(pktc_sf_t* sf, pktc_sk_t* s) {
  unused(sf);
  s->conn_ok = 0;
  wq_init(&s->wq);
  ib_init(&s->ib, MOD_PKTC_INBUF);
  dlink_init(&s->cb_head);
  dlink_init(&s->list_link);
  s->user_keepalive_timeout = 0;
  return 0;
}

static void pktc_sk_destroy(pktc_sf_t* sf, pktc_sk_t* s) {
  pktc_t* pc = structof(sf, pktc_t, sf);
  if (s) {
    ihash_del(&pc->sk_map, *(uint64_t*)&s->dest);
    dlink_delete(&s->list_link);
  }
}

static pktc_sk_t* pktc_sk_new(pktc_sf_t* sf) {
  pktc_sk_t* s = (pktc_sk_t*)pktc_sk_alloc(sizeof(*s));
  if (s) {
    s->fty = (sf_t*)sf;
    s->handle_event = (handle_event_t)pktc_sk_handle_event;
    pktc_sk_init(sf, s);
  }
  rk_info("sk_new: s=%p", s);
  return s;
}

static void pktc_sk_delete(pktc_sf_t* sf, pktc_sk_t* s) {
  pktc_t* io = structof(sf, pktc_t, sf);
  rk_info("sk_destroy: s=%p io=%p", s, io);
  pktc_sk_destroy(sf, s);
  pktc_write_queue_on_sk_destroy(io, s);
  pktc_resp_cb_on_sk_destroy(io, s);
  ib_destroy(&s->ib);
  pktc_sk_free(s);
}

static int pktc_sf_init(pktc_sf_t* sf) {
  sf_init((sf_t*)sf, (void*)pktc_sk_new, (void*)pktc_sk_delete);
  return 0;
}
