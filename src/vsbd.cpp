#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/iofunc.h> // must come before dispatch.h
#include <sys/dispatch.h>

#include <map>
#include <string>

static resmgr_connect_funcs_t connect_funcs;
static resmgr_io_funcs_t io_funcs;
static iofunc_attr_t attr;

static const size_t MAX_WRITE = 255;

// The bus state: signal name -> value.
static std::map<std::string, std::string> g_signals;

// Rendered text that read replies point at. Global on purpose: it must stay
// alive until the library sends the reply, after io_read has returned.
static std::string g_snapshot;

static std::string render_all() {
  std::string out;
  for (const auto &kv : g_signals) {
    out += kv.first + "=" + kv.second + "\n";
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

  g_signals[name] = value;
  attr.nbytes = render_all().size(); // keep ls -l size honest

  _IO_SET_WRITE_NBYTES(ctp, nbytes); // tell the client all bytes were taken
  ocb->attr->flags |= IOFUNC_ATTR_MTIME | IOFUNC_ATTR_CTIME;
  return _RESMGR_NPARTS(0);
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

  iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs, _RESMGR_IO_NFUNCS,
                   &io_funcs);
  io_funcs.read = io_read;
  io_funcs.write = io_write;

  iofunc_attr_init(&attr, S_IFNAM | 0666, 0, 0);

  id = resmgr_attach(dpp, &resmgr_attr, "/dev/vsb", _FTYPE_ANY, 0,
                     &connect_funcs, &io_funcs, &attr);
  if (id == -1) {
    fprintf(stderr, "%s: Unable to attach name.\n", argv[0]);
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