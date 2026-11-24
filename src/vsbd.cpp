#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iofunc.h> // must come before dispatch.h
#include <sys/dispatch.h>
#include <unistd.h>

static resmgr_connect_funcs_t connect_funcs;
static resmgr_io_funcs_t io_funcs;
static iofunc_attr_t attr;

// What `cat /dev/vsb` returns for now. Later this becomes a table of live
// signals.
static char g_text[] = "speed=0\n";

// Called by the library whenever a client does read() on /dev/vsb.
static int io_read(resmgr_context_t *ctp, io_read_t *msg, iofunc_ocb_t *ocb) {
  // Was the device opened for reading? If not, iofunc returns the error.
  int status = iofunc_read_verify(ctp, msg, ocb, NULL);
  if (status != EOK) {
    return status;
  }

  // We only support plain reads (no extended read types).
  if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) {
    return ENOSYS;
  }

  // ocb->offset is this client's position in the "file".
  size_t total = strlen(g_text);
  size_t offset = (size_t)ocb->offset;
  size_t left = (offset < total) ? (total - offset) : 0;
  size_t nbytes = (msg->i.nbytes < left) ? msg->i.nbytes : left;

  if (nbytes > 0) {
    // Describe the bytes to send back, then let the library reply.
    SETIOV(&ctp->iov[0], g_text + offset, nbytes);
    _IO_SET_READ_NBYTES(ctp, nbytes);
    ocb->offset += nbytes;
    ocb->attr->flags |= IOFUNC_ATTR_ATIME; // access time needs updating
    return _RESMGR_NPARTS(1);
  }

  // Nothing left: return 0 bytes, which `cat` treats as end of file.
  _IO_SET_READ_NBYTES(ctp, 0);
  return _RESMGR_NPARTS(0);
}

int main(int argc, char **argv) {
  resmgr_attr_t resmgr_attr;
  dispatch_t *dpp;
  dispatch_context_t *ctp;
  int id;

  // 1. The channel we receive messages on.
  dpp = dispatch_create_channel(-1, DISPATCH_FLAG_NOLOCK);
  if (dpp == NULL) {
    fprintf(stderr, "%s: Unable to allocate dispatch handle.\n", argv[0]);
    return EXIT_FAILURE;
  }

  // 2. Limits for this resource manager.
  memset(&resmgr_attr, 0, sizeof resmgr_attr);
  resmgr_attr.nparts_max = 1;
  resmgr_attr.msg_max_size = 2048;

  // 3. Load default handlers, then override only read.
  iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs, _RESMGR_IO_NFUNCS,
                   &io_funcs);
  io_funcs.read = io_read;

  // 4. Describe the device: a named special file, rw for everyone.
  iofunc_attr_init(&attr, S_IFNAM | 0666, 0, 0);
  attr.nbytes = strlen(g_text); // size reported by stat/ls

  // 5. Register /dev/vsb in the pathname space.
  id = resmgr_attach(dpp, &resmgr_attr, "/dev/vsb", _FTYPE_ANY, 0,
                     &connect_funcs, &io_funcs, &attr);
  if (id == -1) {
    fprintf(stderr, "%s: Unable to attach name.\n", argv[0]);
    return EXIT_FAILURE;
  }

  // 6. Allocate the context, then serve requests forever.
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