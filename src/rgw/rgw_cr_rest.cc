#include "rgw_cr_rest.h"

#include "rgw_coroutine.h"

// re-include our assert to clobber the system one; fix dout:
#include "include/assert.h"

#include <boost/asio/yield.hpp>

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_rgw

class RGWCRHTTPGetDataCB : public RGWGetDataCB {
  Mutex lock;
  RGWCoroutinesEnv *env;
  RGWCoroutine *cr;
  int64_t io_id;
  bufferlist data;
public:
  RGWCRHTTPGetDataCB(RGWCoroutinesEnv *_env, RGWCoroutine *_cr, int64_t _io_id) : lock("RGWCRHTTPGetDataCB"), env(_env), cr(_cr), io_id(_io_id) {}

  int handle_data(bufferlist& bl, off_t bl_ofs, off_t bl_len) override {
    {
      Mutex::Locker l(lock);
      if (bl_len == bl.length()) {
        data.claim_append(bl);
      } else {
        bl.splice(0, bl_len, &data);
      }
    }

    env->manager->io_complete(cr, io_id);
    return 0;
  }

  void claim_data(bufferlist *dest, uint64_t max) {
    Mutex::Locker l(lock);

    if (data.length() == 0) {
      return;
    }

    if (data.length() < max) {
      max = data.length();
    }

    data.splice(0, max, dest);
  }

  bool has_data() {
    return (data.length() > 0);
  }
};


RGWStreamRWHTTPResourceCRF::~RGWStreamRWHTTPResourceCRF()
{
  delete in_cb;
}

int RGWStreamRWHTTPResourceCRF::init()
{
  env->stack->init_new_io(req);

  in_cb = new RGWCRHTTPGetDataCB(env, caller, req->get_io_id());

  req->set_in_cb(in_cb);

  int r = http_manager->add_request(req);
  if (r < 0) {
    return r;
  }

  return 0;
}

int RGWStreamRWHTTPResourceCRF::read(bufferlist *out, uint64_t max_size, bool *io_pending)
{
    reenter(&read_state) {
    while (!req->is_done()) {
      *io_pending = true;
      if (!in_cb->has_data()) {
        yield caller->io_block(0, req->get_io_id());
      }
      *io_pending = false;
      in_cb->claim_data(out, max_size);
      if (!req->is_done()) {
        yield;
      }
    }
  }
  return 0;
}

int RGWStreamRWHTTPResourceCRF::write(bufferlist& data)
{
#warning write need to throttle and block
  reenter(&write_state) {
    while (!req->is_done()) {
      yield req->add_send_data(data);
    }
  }
  return 0;
}

int RGWStreamRWHTTPResourceCRF::drain_writes(bool *need_retry)
{
  reenter(&drain_state) {
    *need_retry = true;
    yield req->finish_write();
    *need_retry = !req->is_done();
    while (!req->is_done()) {
      yield caller->io_block(0, req->get_io_id());
      *need_retry = !req->is_done();
    }
    return req->get_req_retcode();
  }
  return 0;
}

TestCR::TestCR(CephContext *_cct, RGWHTTPManager *_mgr, RGWHTTPStreamRWRequest *_req) : RGWCoroutine(_cct), cct(_cct), http_manager(_mgr),
                                                                                        req(_req) {}
TestCR::~TestCR() {
  delete crf;
}

int TestCR::operate() {
  reenter(this) {
    crf = new RGWStreamRWHTTPResourceCRF(cct, get_env(), this, http_manager, req);

    {
      int ret = crf->init();
      if (ret < 0) {
        return set_cr_error(ret);
      }
    }

    do {

      bl.clear();

      do {
        yield {
          ret = crf->read(&bl, 4 * 1024 * 1024, &need_retry);
          if (ret < 0)  {
            return set_cr_error(ret);
          }
        }
      } while (need_retry);

      if (retcode < 0) {
        return set_cr_error(ret);
      }

      dout(0) << "read " << bl.length() << " bytes" << dendl;

      if (bl.length() == 0) {
        break;
      }

      yield {
        ret = crf->write(bl);
        if (ret < 0)  {
          return set_cr_error(ret);
        }
      }

      if (retcode < 0) {
        return set_cr_error(ret);
      }

      dout(0) << "wrote " << bl.length() << " bytes" << dendl;
    } while (true);

    do {
      yield {
        int ret = crf->drain_writes(&need_retry);
        if (ret < 0) {
          return set_cr_error(ret);
        }
      }
    } while (need_retry);

    return set_cr_done();
  }
  return 0;
}

TestSpliceCR::TestSpliceCR(CephContext *_cct, RGWHTTPManager *_mgr,
                           RGWHTTPStreamRWRequest *_in_req,
                           RGWHTTPStreamRWRequest *_out_req) : RGWCoroutine(_cct), cct(_cct), http_manager(_mgr),
                                                               in_req(_in_req), out_req(_out_req) {}
TestSpliceCR::~TestSpliceCR() {
  delete in_crf;
  delete out_crf;
}

int TestSpliceCR::operate() {
  reenter(this) {
    in_crf = new RGWStreamRWHTTPResourceCRF(cct, get_env(), this, http_manager, in_req);
    out_crf = new RGWStreamRWHTTPResourceCRF(cct, get_env(), this, http_manager, out_req);

    {
      int ret = in_crf->init();
      if (ret < 0) {
        return set_cr_error(ret);
      }
    }

    do {

      bl.clear();

      do {
        yield {
          ret = in_crf->read(&bl, 4 * 1024 * 1024, &need_retry);
          if (ret < 0)  {
            return set_cr_error(ret);
          }
        }

        if (retcode < 0) {
          ldout(cct, 20) << __func__ << ": in_crf->read() retcode=" << retcode << dendl;
          return set_cr_error(ret);
        }
      } while (need_retry);

      dout(0) << "read " << bl.length() << " bytes" << dendl;

      if (bl.length() == 0) {
        break;
      }

      if (total_read == 0) {
#warning need to lock in_req->headers
        for (auto h : in_req->get_out_headers()) {
          if (h.first == "CONTENT_LENGTH") {
            out_req->set_send_length(atoi(h.second.c_str()));
          } else {
            out_req->append_header(h.first, h.second);
          }
        }
        int ret = out_crf->init();
        if (ret < 0) {
          return set_cr_error(ret);
        }
dout(0) << __FILE__ << ":" << __LINE__ << ": headers=" << in_req->get_out_headers() << dendl;
      }

      total_read += bl.length();

      yield {
        ret = out_crf->write(bl);
        if (ret < 0)  {
          return set_cr_error(ret);
        }
      }

      if (retcode < 0) {
        ldout(cct, 20) << __func__ << ": out_crf->write() retcode=" << retcode << dendl;
        return set_cr_error(ret);
      }

      dout(0) << "wrote " << bl.length() << " bytes" << dendl;
    } while (true);

    do {
      yield {
        int ret = out_crf->drain_writes(&need_retry);
        if (ret < 0) {
          return set_cr_error(ret);
        }
      }
    } while (need_retry);

    return set_cr_done();
  }
  return 0;
}
