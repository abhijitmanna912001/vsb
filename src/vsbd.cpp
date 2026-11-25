#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/iofunc.h> // must come before dispatch.h
#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include <list>
#include <map>
#include <string>

static resmgr_connect_funcs_t connect_funcs;
static resmgr_io_funcs_t io_funcs;        // handlers for /dev/vsb
static resmgr_io_funcs_t events_io_funcs; // handlers for /dev/vsb_events
static iofunc_attr_t attr;                // /dev/vsb
static iofunc_attr_t attr_events;         // /dev/vsb_events

static const size_t MAX_WRITE = 255;

// The bus state: signal name -> value.
static std::map<std::string, std::string> g_signals;

// Change tracking. g_seq counts writes; g_seq_of records, per signal, the
// value of g_seq when it last changed.
static uint64_t g_seq = 0;
static std::map<std::string, uint64_t> g_seq_of;

// Rendered text that read replies point at. Global on purpose: it must stay
// alive until the library sends the reply, after the handler has returned.
static std::string g_snapshot;

// A subscriber waiting for the next change. ocb->offset holds the last
// sequence number that subscriber has been given.
struct Waiter {
  int rcvid;
  size_t max_bytes;
  iofunc_ocb_t *ocb;
};
static std::list<Waiter> g_waiters;

static std::string render_all() {
  std::string out;
  for (const auto &kv : g_signals) {
    out += kv.first + "=" + kv.second + "\n";
  }
  return out;
}

// Only the signals that changed after sequence number 'since'.
static std::string render_since(uint64_t since) {
  std::string out;
  for (const auto &kv : g_signals) {
    auto it = g_seq_of.find(kv.first);
    if (it != g_seq_of.end() && it->second > since) {
      out += kv.first + "=" + kv.second + "\n";
    }
  }
  return out;
}

// A signal name is non-empty and made of letters, digits, '_' or '.'.
static bool valid_name(const std::string &s) {
  if (s.empty())
    return false;
  for (char c : s) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.';
    if (!ok)
      return false;
  }
  return true;
}

// Reply to every blocked subscriber that has something new, and catch it up.
static void notify_waiters() {
  for (auto it = g_waiters.begin(); it != g_waiters.end();) {
    std::string text = render_since((uint64_t)it->ocb->offset);
    if (text.empty()) {
      ++it;
      continue;
    }
    size_t n = (text.size() < it->max_bytes) ? text.size() : it->max_bytes;
    it->ocb->offset = (off_t)g_seq;
    MsgReply(it->rcvid, (int)n, text.data(), n);
    it = g_waiters.erase(it);
  }
}

// ---- /dev/vsb : snapshot of all signals ------------------------------------

static int io_read(resmgr_context_t *ctp, io_read_t *msg, iofunc_ocb_t *ocb) {
  int status = iofunc_read_verify(ctp, msg, ocb, NULL);
  if (status != EOK) {
    return status;
  }
  if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) {
    return ENOSYS;
  }

  g_snapshot = render_all();

  size_t total = g_snapshot.size();
  size_t offset = (size_t)ocb->offset;
  size_t left = (offset < total) ? (total - offset) : 0;
  size_t nbytes = (msg->i.nbytes < left) ? msg->i.nbytes : left;

  if (nbytes > 0) {
    SETIOV(&ctp->iov[0], g_snapshot.data() + offset, nbytes);
    _IO_SET_READ_NBYTES(ctp, nbytes);
    ocb->offset += nbytes;
    ocb->attr->flags |= IOFUNC_ATTR_ATIME;
    return _RESMGR_NPARTS(1);
  }

  _IO_SET_READ_NBYTES(ctp, 0);
  return _RESMGR_NPARTS(0);
}

static int io_write(resmgr_context_t *ctp, io_write_t *msg, iofunc_ocb_t *ocb) {
  int status = iofunc_write_verify(ctp, msg, ocb, NULL);
  if (status != EOK) {
    return status;
  }
  if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) {
    return ENOSYS;
  }

  size_t nbytes = msg->i.nbytes;
  if (nbytes == 0 || nbytes > MAX_WRITE) {
    return EMSGSIZE;
  }

  // The data is not in the message struct; fetch it from the client.
  char buf[MAX_WRITE + 1];
  if (resmgr_msgread(ctp, buf, nbytes, sizeof(msg->i)) == -1) {
    return errno;
  }
  buf[nbytes] = '\0';

  // Expect "name=value", optionally ending in a newline.
  std::string line(buf);
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
    line.pop_back();
  }
  size_t eq = line.find('=');
  if (eq == std::string::npos) {
    return EINVAL;
  }
  std::string name = line.substr(0, eq);
  std::string value = line.substr(eq + 1);
  if (!valid_name(name) || value.empty()) {
    return EINVAL;
  }

  g_seq++;
  g_signals[name] = value;
  g_seq_of[name] = g_seq;
  attr.nbytes = render_all().size(); // keep ls -l size honest

  // Wake every subscriber that is blocked on /dev/vsb_events.
  notify_waiters();

  _IO_SET_WRITE_NBYTES(ctp, nbytes); // tell the client all bytes were taken
  ocb->attr->flags |= IOFUNC_ATTR_MTIME | IOFUNC_ATTR_CTIME;
  return _RESMGR_NPARTS(0);
}

// ---- /dev/vsb_events : stream of changes -----------------------------------

static int io_read_events(resmgr_context_t *ctp, io_read_t *msg,
                          iofunc_ocb_t *ocb) {
  int status = iofunc_read_verify(ctp, msg, ocb, NULL);
  if (status != EOK) {
    return status;
  }
  if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) {
    return ENOSYS;
  }
  if (msg->i.nbytes == 0) {
    _IO_SET_READ_NBYTES(ctp, 0);
    return _RESMGR_NPARTS(0);
  }

  // Anything this subscriber hasn't seen yet? Reply right away.
  std::string text = render_since((uint64_t)ocb->offset);
  if (!text.empty()) {
    g_snapshot = text;
    size_t n =
        (g_snapshot.size() < msg->i.nbytes) ? g_snapshot.size() : msg->i.nbytes;
    ocb->offset = (off_t)g_seq;
    SETIOV(&ctp->iov[0], g_snapshot.data(), n);
    _IO_SET_READ_NBYTES(ctp, n);
    return _RESMGR_NPARTS(1);
  }

  // Caught up: remember the client and do NOT reply until something changes.
  Waiter w;
  w.rcvid = ctp->rcvid;
  w.max_bytes = msg->i.nbytes;
  w.ocb = ocb;
  g_waiters.push_back(w);
  return _RESMGR_NOREPLY;
}

// A blocked subscriber was signalled (e.g. Ctrl+C). Forget it and release it.
static int io_unblock(resmgr_context_t *ctp, io_pulse_t *msg,
                      iofunc_ocb_t *ocb) {
  int status = iofunc_unblock_default(ctp, msg, ocb);
  if (status != _RESMGR_DEFAULT) {
    return status;
  }
  for (auto it = g_waiters.begin(); it != g_waiters.end(); ++it) {
    if (it->rcvid == ctp->rcvid) {
      g_waiters.erase(it);
      return EINTR; // replies to the client with an interrupted error
    }
  }
  return _RESMGR_DEFAULT;
}

int main(int argc, char **argv) {
  resmgr_attr_t resmgr_attr;
  dispatch_t *dpp;
  dispatch_context_t *ctp;
  int id;

  dpp = dispatch_create_channel(-1, DISPATCH_FLAG_NOLOCK);
  if (dpp == NULL) {
    fprintf(stderr, "%s: Unable to allocate dispatch handle.\n", argv[0]);
    return EXIT_FAILURE;
  }

  memset(&resmgr_attr, 0, sizeof resmgr_attr);
  resmgr_attr.nparts_max = 1;
  resmgr_attr.msg_max_size = 2048;

  // /dev/vsb: default handlers, override read and write.
  iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs, _RESMGR_IO_NFUNCS,
                   &io_funcs);
  io_funcs.read = io_read;
  io_funcs.write = io_write;
  iofunc_attr_init(&attr, S_IFNAM | 0666, 0, 0);

  id = resmgr_attach(dpp, &resmgr_attr, "/dev/vsb", _FTYPE_ANY, 0,
                     &connect_funcs, &io_funcs, &attr);
  if (id == -1) {
    fprintf(stderr, "%s: Unable to attach /dev/vsb.\n", argv[0]);
    return EXIT_FAILURE;
  }

  // /dev/vsb_events: stream of changes, read-only.
  iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs, _RESMGR_IO_NFUNCS,
                   &events_io_funcs);
  events_io_funcs.read = io_read_events;
  events_io_funcs.unblock = io_unblock;
  iofunc_attr_init(&attr_events, S_IFNAM | 0444, 0, 0);

  id = resmgr_attach(dpp, &resmgr_attr, "/dev/vsb_events", _FTYPE_ANY, 0,
                     &connect_funcs, &events_io_funcs, &attr_events);
  if (id == -1) {
    fprintf(stderr, "%s: Unable to attach /dev/vsb_events.\n", argv[0]);
    return EXIT_FAILURE;
  }

  ctp = dispatch_context_alloc(dpp);
  while (1) {
    if ((ctp = dispatch_block(ctp)) == NULL) {
      fprintf(stderr, "block error\n");
      return EXIT_FAILURE;
    }
    dispatch_handler(ctp);
  }
  return EXIT_SUCCESS;
}